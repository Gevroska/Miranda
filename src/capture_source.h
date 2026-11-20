/**
 * @file src/capture_source.h
 * @brief Immutable managed-source metadata and explicit capture-resource lifetime pins.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace rtsp_stream {
  /** @brief Windows LUID without importing Windows headers into the shared capture contract. */
  struct luid_t {
    std::uint32_t low {};  ///< Low 32 bits of the adapter LUID.
    std::int32_t high {};  ///< High 32 bits of the adapter LUID.
    bool operator==(const luid_t &) const = default;
  };

  /** @brief Exact Windows target identity; D resolves the current active source adapter independently. */
  struct windows_output_t {
    luid_t target_adapter;  ///< Target adapter, distinct from the active source adapter returned by D verification.
    std::uint32_t target_id {};  ///< DisplayConfig target ID on that adapter.
    std::string monitor_path_utf8;  ///< Exact monitor interface path in UTF-8.
    bool operator==(const windows_output_t &) const = default;
  };

  /**
   * @brief Exact source binding shared by probe and capture, without owning a capture pin.
   * @details D capture integration must verify every constraint before opening or reopening a source.
   * A managed selector must never fall back to a different display or modify global output selection.
   */
  struct capture_source_t {
    enum class kind_e {
      native,
      owned_virtual
    };
    kind_e kind = kind_e::native;  ///< Native output or exclusively owned virtual target.
    std::uint64_t generation {};  ///< Owner source generation, independent of persistent monitor identity.
    std::uint64_t epoch {};  ///< Probe/capture incarnation, renewed on retained reconnect.
    std::string selector;  ///< Exact active capture selector.
    std::string device_id;  ///< Exact display-library identity.
    std::string monitor_uuid;  ///< Canonical UUID text required only for owned_virtual; native must not invent one.
    std::optional<windows_output_t> windows_output;  ///< Required on Windows; other platforms do not fabricate target identities.
    std::uint32_t width {};  ///< Verified source width.
    std::uint32_t height {};  ///< Verified source height.
    std::uint32_t rate_numerator {};  ///< Verified exact source refresh numerator.
    std::uint32_t rate_denominator {1};  ///< Verified exact source refresh denominator.
    std::optional<bool> observed_hdr;  ///< Actual observed HDR; unavailable/unsupported is distinct from disabled.
    std::optional<bool> requested_hdr;  ///< Frozen explicit display-HDR request, separate from observed HDR and the client stream hint.
    bool operator==(const capture_source_t &) const = default;
  };

  /**
   * @brief One explicitly acquired capture lifetime, shared only by resources in that lifetime.
   * @details Final destruction posts a nonblocking owner event. Copying immutable metadata is not
   * resource ownership. Both detached capture/encoder teardown paths must retain a pin until their
   * actual resources are destroyed; stream worker join alone does not establish quiescence.
   */
  class capture_pin_t {
    friend class rtsp_server_t;

  private:
    /** @brief Only the owner issues counted pins with a validated completion emitter. */
    explicit capture_pin_t(std::function<void()> release):
        release_(std::move(release)) {}

  public:
    capture_pin_t(const capture_pin_t &) = delete;
    capture_pin_t &operator=(const capture_pin_t &) = delete;

    ~capture_pin_t() noexcept {
      try {
        release_();
      } catch (...) {
        // Failure to deliver leaves the owner's pin outstanding and blocks unsafe cleanup.
      }
    }

  private:
    std::function<void()> release_;  ///< Gated event emitter; never synchronously waits for the owner.
  };
}  // namespace rtsp_stream
