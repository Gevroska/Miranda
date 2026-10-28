/**
 * @file src/platform/windows/capture_binding.h
 * @brief Read-only resolution of an immutable capture target to its active Windows source.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

// lib includes
#include <boost/algorithm/string/predicate.hpp>
#include <display_device/windows/win_api_layer_interface.h>
#include <display_device/windows/win_api_utils.h>

// local includes
#include "src/capture_source.h"

namespace platf::capture_binding {
  /** @brief Current active source and observed mode for one exact target. */
  struct observation_t {
    rtsp_stream::luid_t source_adapter;  ///< DXGI capture adapter; not the target or requested render adapter.
    std::uint32_t source_id {};  ///< Active DisplayConfig source ID.
    std::uint32_t width {};  ///< Actual desktop source width.
    std::uint32_t height {};  ///< Actual desktop source height.
    std::uint32_t rate_numerator {};  ///< Actual target refresh numerator.
    std::uint32_t rate_denominator {};  ///< Actual target refresh denominator.
    std::optional<bool> hdr;  ///< Actual HDR state; unknown is not disabled.
    bool operator==(const observation_t &) const = default;
  };

  /** @brief Convert a Windows adapter LUID into the shared, header-independent representation. */
  inline rtsp_stream::luid_t luid(const LUID &value) {
    return {value.LowPart, value.HighPart};
  }

  /**
   * @brief Observe the unique active source of an exact target, without changing Windows state.
   * @details QueryType::All can contain several inactive source alternatives for the same target.
   * Those alternatives carry no capture authority. Every target/path/library-ID mapping must agree,
   * and every active mapping must name the same source and frozen selector.
   * @param source Frozen target, library identity and selector; mode fields need not be populated yet.
   * @param api Pinned display-library Windows API boundary.
   * @return Positive observed mode and active source, or no value for missing/conflicting identity.
   */
  inline std::optional<observation_t> observe(const rtsp_stream::capture_source_t &source, const display_device::WinApiLayerInterface &api) {
    if (!source.windows_output || source.selector.empty() || source.device_id.empty() || source.windows_output->monitor_path_utf8.empty()) {
      return std::nullopt;
    }
    const auto data = api.queryDisplayConfig(display_device::QueryType::All);
    if (!data) {
      return std::nullopt;
    }
    const auto &target = *source.windows_output;
    std::optional<observation_t> observed;
    std::optional<std::pair<rtsp_stream::luid_t, std::uint32_t>> selector_source;
    for (const auto &path : data->m_paths) {
      const bool same_target = luid(path.targetInfo.adapterId) == target.target_adapter && path.targetInfo.id == target.target_id;
      const auto monitor_path = api.getMonitorDevicePath(path);
      const bool same_path = boost::iequals(monitor_path, target.monitor_path_utf8);
      const auto device_id = api.getDeviceId(path);
      const bool same_device = device_id == source.device_id;
      const bool active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
      const auto selector = active ? api.getDisplayName(path) : std::string {};
      if (active && selector == source.selector) {
        const auto current_source = std::pair {luid(path.sourceInfo.adapterId), path.sourceInfo.id};
        if (selector_source && *selector_source != current_source) {
          return std::nullopt;
        }
        selector_source = current_source;
      }
      if (!same_target) {
        if (same_path || same_device || (active && selector == source.selector && source.kind != rtsp_stream::capture_source_t::kind_e::native)) {
          return std::nullopt;
        }
        continue;
      }
      if (!same_path || !same_device) {
        return std::nullopt;
      }
      if (!active) {
        continue;
      }
      if (!path.targetInfo.targetAvailable || selector != source.selector) {
        return std::nullopt;
      }
      const auto index = display_device::win_utils::getSourceIndex(path, data->m_modes);
      const auto mode = display_device::win_utils::getSourceMode(index, data->m_modes);
      if (!mode || luid(data->m_modes[*index].adapterId) != luid(path.sourceInfo.adapterId) || data->m_modes[*index].id != path.sourceInfo.id || mode->width == 0 || mode->height == 0 || path.targetInfo.refreshRate.Numerator == 0 || path.targetInfo.refreshRate.Denominator == 0) {
        return std::nullopt;
      }
      const auto hdr_state = api.getHdrState(path);
      observation_t current {
        luid(path.sourceInfo.adapterId),
        path.sourceInfo.id,
        mode->width,
        mode->height,
        path.targetInfo.refreshRate.Numerator,
        path.targetInfo.refreshRate.Denominator,
        hdr_state ? std::optional<bool> {*hdr_state == display_device::HdrState::Enabled} : std::nullopt
      };
      if (observed && *observed != current) {
        return std::nullopt;
      }
      observed = current;
    }
    return observed;
  }

  /**
   * @brief Verify capture identity and owned-virtual creation constraints before opening a display.
   * @details Native outputs may change mode while retaining their exact identity. Virtual targets
   * retain their creation mode and enforce HDR only when display configuration explicitly requested it.
   */
  inline std::optional<observation_t> verify(const rtsp_stream::capture_source_t &source, const display_device::WinApiLayerInterface &api) {
    auto result = observe(source, api);
    if (!result) {
      return std::nullopt;
    }
    if (source.kind == rtsp_stream::capture_source_t::kind_e::owned_virtual && (source.width != result->width || source.height != result->height || source.rate_numerator == 0 || source.rate_denominator == 0 || static_cast<std::uint64_t>(source.rate_numerator) * result->rate_denominator != static_cast<std::uint64_t>(result->rate_numerator) * source.rate_denominator || (source.requested_hdr && result->hdr != source.requested_hdr))) {
      return std::nullopt;
    }
    return result;
  }

  /**
   * @brief Recheck Windows identity and the actual opened DXGI output immediately before publication.
   * @details This closes ordinary initialization races and rejects GDI-name reuse; it cannot make
   * arbitrary concurrent hotplug atomic. A changed source requires an owner-validated epoch.
   */
  inline bool verify_opened(const rtsp_stream::capture_source_t &source, const observation_t &before, const display_device::WinApiLayerInterface &api, const rtsp_stream::luid_t &adapter, std::string_view selector, bool attached, std::uint32_t width, std::uint32_t height) {
    const auto after = verify(source, api);
    return after && *after == before && adapter == after->source_adapter && attached && selector == source.selector && width == after->width && height == after->height;
  }
}  // namespace platf::capture_binding
