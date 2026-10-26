/**
 * @file src/display_policy.h
 * @brief Typed display preferences and side-effect-free source-request resolution.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "app_catalog.h"
#include "config.h"
#include "crypto.h"

namespace display_policy {
  using json = nlohmann::ordered_json;
  using identity_e = config::video_t::dd_t::identity_option_e;

  inline constexpr auto identity_namespace_v1 = "e944e3d0-e64c-49d9-8ef4-2f57df0d8d7b";  ///< Frozen namespace for Miranda's first virtual-monitor identity encoding.

  /** @brief Keep future non-video admissions separate from video-source ownership. */
  enum class admission_e {
    video,  ///< A video admission may prepare or join a source.
    no_display  ///< Reserved for an authorized admission that must not acquire or pin a video source.
  };

  /** @brief Positive pixel dimensions, independent of per-stream scaling. */
  struct resolution_t {
    std::uint32_t width {};  ///< Source width in pixels.
    std::uint32_t height {};  ///< Source height in pixels.
    bool operator==(const resolution_t &) const = default;
  };

  /** @brief Exact source refresh rate in hertz; never an integer stream-FPS approximation. */
  struct refresh_rate_t {
    std::uint32_t numerator {};  ///< Positive rational numerator.
    std::uint32_t denominator {1};  ///< Positive rational denominator.
    bool operator==(const refresh_rate_t &) const = default;
  };

  /** @brief Administrator-owned preferences for one persistent pairing. */
  struct device_preferences_t {
    bool force_virtual = false;  ///< Additive opt-in, never a physical-display veto.
    std::optional<resolution_t> resolution;  ///< Fresh-source override, absent preserves ordinary selection.
    std::optional<refresh_rate_t> refresh_rate;  ///< Exact fresh-source override in hertz.
    bool operator==(const device_preferences_t &) const = default;
  };

  /** @brief Read a bounded positive integer without accepting strings, booleans or floats. */
  inline std::uint32_t positive_integer(const json &value) {
    if (!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() <= 0)) {
      throw std::invalid_argument {"display: expected a positive integer"};
    }
    const auto number = value.get<std::uint64_t>();
    if (!number || number > INT32_MAX) {
      throw std::invalid_argument {"display: integer exceeds the source-mode range"};
    }
    return static_cast<std::uint32_t>(number);
  }

  /** @brief Parse known device fields strictly while leaving unknown metadata to the typed document. */
  inline device_preferences_t parse_device(const json &value) {
    if (!value.is_object()) {
      throw std::invalid_argument {"display: expected an object"};
    }
    device_preferences_t result;
    if (value.contains("force_virtual")) {
      if (!value["force_virtual"].is_boolean()) {
        throw std::invalid_argument {"display.force_virtual: expected a boolean"};
      }
      result.force_virtual = value["force_virtual"].get<bool>();
    }
    if (value.contains("resolution") && !value["resolution"].is_null()) {
      const auto &resolution = value["resolution"];
      if (!resolution.is_object() || resolution.contains("width") != resolution.contains("height")) {
        throw std::invalid_argument {"display.resolution: expected width and height"};
      }
      if (resolution.contains("width")) {
        result.resolution = resolution_t {positive_integer(resolution["width"]), positive_integer(resolution["height"])};
      }
    }
    if (value.contains("refresh_rate") && !value["refresh_rate"].is_null()) {
      const auto &rate = value["refresh_rate"];
      if (!rate.is_object() || rate.contains("numerator") != rate.contains("denominator")) {
        throw std::invalid_argument {"display.refresh_rate: expected numerator and denominator"};
      }
      if (rate.contains("numerator")) {
        auto numerator = positive_integer(rate["numerator"]);
        auto denominator = positive_integer(rate["denominator"]);
        const auto divisor = std::gcd(numerator, denominator);
        result.refresh_rate = refresh_rate_t {numerator / divisor, denominator / divisor};
      }
    }
    return result;
  }

  /** @brief Replace known fields while retaining every unknown typed member, including mode metadata. */
  inline json serialize_device(const device_preferences_t &preferences, json original = json::object()) {
    if (!original.is_object()) {
      original = json::object();
    }
    original["force_virtual"] = preferences.force_virtual;
    if (preferences.resolution) {
      auto &resolution = original["resolution"];
      if (!resolution.is_object()) {
        resolution = json::object();
      }
      resolution["width"] = preferences.resolution->width;
      resolution["height"] = preferences.resolution->height;
    } else {
      auto &resolution = original["resolution"];
      if (resolution.is_object()) {
        resolution.erase("width");
        resolution.erase("height");
      }
      if (!resolution.is_object() || resolution.empty()) {
        resolution = nullptr;
      }
    }
    if (preferences.refresh_rate) {
      auto &rate = original["refresh_rate"];
      if (!rate.is_object()) {
        rate = json::object();
      }
      rate["numerator"] = preferences.refresh_rate->numerator;
      rate["denominator"] = preferences.refresh_rate->denominator;
    } else {
      auto &rate = original["refresh_rate"];
      if (rate.is_object()) {
        rate.erase("numerator");
        rate.erase("denominator");
      }
      if (!rate.is_object() || rate.empty()) {
        rate = nullptr;
      }
    }
    return original;
  }

  /** @brief Canonicalize a pairing UUID without importing the application's reserved-identity rules. */
  inline std::optional<std::string> canonical_client_uuid(std::string value) {
    if (value.size() != 36) {
      return std::nullopt;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (i == 8 || i == 13 || i == 18 || i == 23) {
        if (value[i] != '-') {
          return std::nullopt;
        }
      } else if (value[i] >= 'A' && value[i] <= 'F') {
        value[i] += 'a' - 'A';
      } else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) {
        return std::nullopt;
      }
    }
    return value == "00000000-0000-0000-0000-000000000000" ? std::nullopt : std::optional {std::move(value)};
  }

  /**
   * @brief Derive a full RFC 9562 version-8 monitor UUID from versioned, domain-separated stable identities.
   * @details SHA-256 input is the ASCII namespace UUID, newline, domain and newline-delimited canonical UUIDs.
   * Names, protocol numeric IDs, client query uniqueid and boot-local display IDs are deliberately absent.
   */
  inline std::string monitor_uuid(identity_e policy, const std::string &client_uuid, const std::optional<std::string> &application_uuid) {
    const auto client = canonical_client_uuid(client_uuid);
    const auto application = application_uuid ? proc::catalog::canonical_uuid(*application_uuid) : std::optional<std::string> {proc::catalog::desktop_uuid};
    if (!client || !application) {
      throw std::invalid_argument {"display identity: expected persistent pairing and application UUIDs"};
    }
    std::string input = std::string {identity_namespace_v1} + "\n";
    switch (policy) {
      case identity_e::client:
        input += "client\n" + *client;
        break;
      case identity_e::application:
        input += "application\n" + *application;
        break;
      case identity_e::client_application:
        input += "client-application\n" + *client + "\n" + *application;
        break;
      default:
        throw std::invalid_argument {"display identity: unknown policy"};
    }
    auto digest = crypto::hash(input);
    digest[6] = (digest[6] & 0x0f) | 0x80;
    digest[8] = (digest[8] & 0x3f) | 0x80;
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < 16; ++i) {
      if (i == 4 || i == 6 || i == 8 || i == 10) {
        result += '-';
      }
      result += hex[digest[i] >> 4];
      result += hex[digest[i] & 0x0f];
    }
    return result;
  }

  /** @brief One source mode, distinct from observed display state and individual stream formats. */
  struct mode_t {
    resolution_t resolution;  ///< Selected source dimensions.
    refresh_rate_t refresh_rate;  ///< Exact selected source rate.
    bool hdr = false;  ///< Selected source HDR state.
    bool operator==(const mode_t &) const = default;
  };

  /** @brief Source inputs supplied only after host-side admission authorization. */
  struct fresh_source_t {
    admission_e admission = admission_e::video;  ///< Ordinary video or explicitly display-free future admission.
    std::string authenticated_client_uuid;  ///< Current persistent pairing identity, never the query uniqueid.
    std::optional<std::string> application_uuid;  ///< P1c stable app UUID, or no value for protocol Desktop.
    bool application_has_lifetime = false;  ///< True only for a tracked application, never a no-app/Desktop placeholder.
    bool app_force_virtual = false;  ///< Saved application opt-in.
    bool client_requests_virtual = false;  ///< Authorized Artemis opt-in.
    bool physical_source_available = true;  ///< Headless selection requires a virtual source.
    device_preferences_t device;  ///< Current administrator-owned device settings.
    bool device_preferences_valid = true;  ///< Invalid persisted settings require repair before fresh preparation.
    mode_t selected_mode;  ///< Modern Sunshine selection after manual/automatic/remapping processing.
    std::optional<bool> requested_hdr;  ///< Actual parsed display HDR mutation, absent when display/HDR configuration is disabled.
  };

  /** @brief Immutable preparation specification; the later source owner supplies observed device mapping. */
  struct source_request_t {
    bool virtual_display {};  ///< Whether acquisition must use the managed virtual backend.
    std::string monitor_uuid;  ///< Full persistent GUID; never a boot-local capture selector.
    std::string edid_name;  ///< Stable ASCII display name bounded to SudoVDA's 13-byte field.
    std::string edid_serial;  ///< Stable ASCII serial bounded to SudoVDA's 13-byte field.
    mode_t mode;  ///< Requested source mode, not an assertion of observed state.
    std::optional<bool> requested_hdr;  ///< Explicit parsed display setting, separate from stream hint and observed HDR.
    config::video_t::dd_t::config_option_e topology;  ///< Existing Sunshine topology policy.
    std::string adapter_name;  ///< Configured render adapter, to be resolved before acquisition.
    std::string native_output;  ///< Configured native selector; no virtual-acquisition fallback is implied.
    bool retain_while_app_running {};  ///< DEC-46: retain by default, restore/remove with opt-in disconnect restoration.
    std::chrono::milliseconds restore_delay {};  ///< Existing configured restoration delay.
    bool operator==(const source_request_t &) const = default;
  };

  /**
   * @brief Resolve one const source request without touching settings, displays, sessions or processes.
   * @param video Snapshot of the current host settings.
   * @param fresh Authorized identity, saved preferences and preselected ordinary source mode.
   * @param active Existing source snapshot; every later video admission receives this exact object unchanged.
   * @pre The caller has revalidated the required permission for this admission, including video permission on joins.
   */
  inline std::shared_ptr<const source_request_t> resolve(const config::video_t &video, const fresh_source_t &fresh, std::shared_ptr<const source_request_t> active = {}) {
    if (!canonical_client_uuid(fresh.authenticated_client_uuid)) {
      throw std::invalid_argument {"display identity: missing authenticated pairing UUID"};
    }
    if (fresh.admission == admission_e::no_display) {
      return {};
    }
    if (fresh.admission != admission_e::video) {
      throw std::invalid_argument {"display: unknown admission kind"};
    }
    if (active) {
      return active;
    }
    if (!fresh.device_preferences_valid) {
      throw std::invalid_argument {"display: administrator repair of saved device preferences is required"};
    }
    source_request_t result;
    result.virtual_display = video.dd.force_virtual || fresh.app_force_virtual || fresh.device.force_virtual || fresh.client_requests_virtual || !fresh.physical_source_available;
    result.monitor_uuid = monitor_uuid(video.dd.virtual_identity, fresh.authenticated_client_uuid, fresh.application_uuid);
    result.edid_name = "Miranda-" + result.monitor_uuid.substr(0, 5);
    auto serial = result.monitor_uuid;
    std::erase(serial, '-');
    result.edid_serial = serial.substr(0, 13);
    result.mode = fresh.selected_mode;
    result.requested_hdr = fresh.requested_hdr;
    if (fresh.device.resolution) {
      result.mode.resolution = *fresh.device.resolution;
    }
    if (fresh.device.refresh_rate) {
      result.mode.refresh_rate = *fresh.device.refresh_rate;
    }
    const auto &mode = result.mode;
    if (!mode.resolution.width || !mode.resolution.height || mode.resolution.width > INT32_MAX || mode.resolution.height > INT32_MAX || !mode.refresh_rate.numerator || !mode.refresh_rate.denominator) {
      throw std::invalid_argument {"display mode: expected positive source dimensions and rational rate"};
    }
    const auto divisor = std::gcd(result.mode.refresh_rate.numerator, result.mode.refresh_rate.denominator);
    result.mode.refresh_rate.numerator /= divisor;
    result.mode.refresh_rate.denominator /= divisor;
    result.topology = video.dd.configuration_option;
    result.adapter_name = video.adapter_name;
    result.native_output = video.output_name;
    result.retain_while_app_running = fresh.application_uuid.has_value() && fresh.application_has_lifetime && !video.dd.config_revert_on_disconnect;
    result.restore_delay = video.dd.config_revert_delay;
    return std::make_shared<const source_request_t>(std::move(result));
  }
}  // namespace display_policy
