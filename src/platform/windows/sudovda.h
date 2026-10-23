/**
 * @file src/platform/windows/sudovda.h
 * @brief Caller-invoked SudoVDA control and owned display-source lifetime.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// platform includes
#include <windows.h>
#include <winioctl.h>

namespace platf::sudovda {
  /** @brief Failures at the control boundary; no physical-display fallback is performed. */
  enum class errc_t {
    unavailable,
    open_failed,
    transport_failed,
    response_size,
    incompatible_protocol,
    malformed_response,
    unsupported_watchdog,
    invalid_mode,
    rate_precision,
    unsupported_rate,
    invalid_metadata,
    exclusive_control_required,
    busy,
    already_present,
    enumeration_failed,
    enumeration_timeout,
    heartbeat_failed,
    remove_failed,
    close_failed
  };

  /** @brief The operation that failed, including cleanup independent of acquisition. */
  enum class operation_t {
    open,
    version,
    watchdog,
    enumerate,
    render_adapter,
    add,
    heartbeat,
    remove,
    close
  };

  /** @brief Structured diagnostic without device paths or other operational state. */
  struct failure_t {
    errc_t code;  ///< Failure category.
    operation_t operation;  ///< Failing operation.
    DWORD system_error {};  ///< Win32 error, or zero for local validation.
    std::size_t returned_bytes {};  ///< Actual response size for a size mismatch.
  };

  namespace wire {
    // Independently expressed wire contract from SudoMaker/SudoVDA at
    // a4b09fa2aa731a964d0cb5d139cb1e6240e4da12, Common/Include/sudovda-ioctl.h.
    // These are protocol declarations, not imported driver/sample implementation.
    inline constexpr GUID interface_guid {0xe5bcc234, 0x1e0c, 0x418a, {0xa0, 0xd4, 0xef, 0x8b, 0x75, 0x01, 0x41, 0x4d}};
    inline constexpr DWORD add = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);
    inline constexpr DWORD remove = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);
    inline constexpr DWORD set_adapter = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS);
    inline constexpr DWORD watchdog = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS);
    inline constexpr DWORD ping = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x888, METHOD_BUFFERED, FILE_ANY_ACCESS);
    inline constexpr DWORD version = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8ff, METHOD_BUFFERED, FILE_ANY_ACCESS);

    /** @brief Protocol 0.2.1 fields, keeping the wire boolean safe to validate. */
    struct version_t {
      std::uint8_t major;  ///< Breaking protocol version.
      std::uint8_t minor;  ///< Required feature version.
      std::uint8_t incremental;  ///< Compatible revision.
      std::uint8_t test_build;  ///< Boolean byte; exposed to the caller, not hidden.
    };

    inline constexpr version_t client_version {0, 2, 1, 1};

    /** @brief Exact add-request layout; rate is always encoded in millihertz. */
    struct add_t {
      UINT width;  ///< Positive requested pixel width.
      UINT height;  ///< Positive requested pixel height.
      UINT millihertz;  ///< At least 1000; lower values mean different units to this driver.
      GUID identity;  ///< Full persistent identity supplied by the caller.
      char name[14];  ///< At most 13 bytes followed by NUL.
      char serial[14];  ///< At most 13 bytes followed by NUL.
    };

    /** @brief Driver target identity; both fields are needed to resolve a display. */
    struct target_t {
      LUID adapter;  ///< Driver's display adapter LUID, not necessarily the render adapter.
      UINT target;  ///< Target ID local to that adapter.
    };

    /** @brief Driver-global watchdog metadata, in seconds. */
    struct watchdog_t {
      UINT timeout;  ///< Zero disables the watchdog; one second is unsupported; the pinned default is three seconds.
      UINT countdown;  ///< Remaining driver-global countdown.
    };

    static_assert(sizeof(version_t) == 4);
    static_assert(sizeof(add_t) == 56 && offsetof(add_t, identity) == 12 && offsetof(add_t, serial) == 42);
    static_assert(sizeof(target_t) == 12 && sizeof(watchdog_t) == 8);
  }  // namespace wire

  /** @brief An enumerated output; P3 will map its path through modern display management. */
  struct display_t {
    wire::target_t target {};  ///< LUID and target ID must both match the add response.
    GUID identity {};  ///< Monitor container GUID, when enumerated.
    std::wstring monitor_path;  ///< Windows monitor interface path, not a libdisplaydevice ID.
    std::wstring gdi_name;  ///< Capture source name; no fallback source is selected.
  };

  /** @brief Present monitor identities and queryable Windows display targets. */
  struct enumeration_t {
    std::vector<GUID> identities;  ///< Includes present monitors not yet in display paths.
    std::vector<display_t> displays;  ///< Available paths with usable names.
  };

  /** @brief Cancellable periodic work whose stop joins every in-flight callback. */
  class heartbeat_t {
  public:
    virtual ~heartbeat_t() = default;
    /** @brief Cancel future callbacks and synchronously join current work; must be idempotent. */
    virtual void stop() noexcept = 0;
  };

  /**
   * @brief SudoVDA-specific operating-system boundary, also used for deterministic fake tests.
   * Implementations own exactly one control handle. None of these methods runs implicitly at construction.
   */
  class system_t {
  public:
    virtual ~system_t() = default;
    /** @brief Open the exact SudoVDA SetupAPI interface. */
    virtual std::expected<void, failure_t> open() = 0;
    /** @brief Close the owned handle, reporting failure and allowing a retry. */
    virtual std::expected<void, failure_t> close() = 0;
    /** @brief Execute one control request and report its actual byte count. */
    virtual std::expected<std::size_t, failure_t> ioctl(DWORD code, std::span<const std::byte> input, std::span<std::byte> output) = 0;
    /** @brief Enumerate present monitor containers and available display paths without changing topology. */
    virtual std::expected<enumeration_t, failure_t> enumerate() = 0;
    /** @brief Start periodic callbacks; stop must precede monitor removal and handle close. */
    virtual std::expected<std::unique_ptr<heartbeat_t>, failure_t> heartbeat(std::chrono::milliseconds period, std::function<void()> callback) = 0;
    /** @brief Wait between bounded enumeration attempts; fake implementations advance deterministically. */
    virtual void wait_for_enumeration(std::chrono::milliseconds delay) = 0;
  };

  /**
   * @brief Create dormant Win32 wrappers; this function performs no driver access.
   * Native requests cancel after one second, then join cancellation before releasing buffers.
   * A malfunctioning driver that does not complete cancellation can delay joining and cleanup.
   */
  std::unique_ptr<system_t> make_win32_system();

  /** @brief One explicit source request; viewer indexes must never supply the identity. */
  struct request_t {
    GUID identity {};  ///< Persistent identity selected by the source owner.
    LUID render_adapter {};  ///< Chosen before the add operation; changes driver-global state.
    std::uint64_t width {};  ///< Positive, representable wire width; pipeline support remains separate.
    std::uint64_t height {};  ///< Positive, representable wire height.
    std::uint64_t rate_numerator {};  ///< Rational refresh rate in Hz.
    std::uint64_t rate_denominator {};  ///< Positive denominator; exact millihertz representation is required.
    std::string name;  ///< Nonempty ASCII metadata, at most 13 bytes without embedded NUL.
    std::string serial;  ///< Stable ASCII metadata, at most 13 bytes without embedded NUL.
    bool exclusive_driver_control {};  ///< Caller guarantees no other process creates/removes/reconfigures SudoVDA outputs.
    unsigned int enumeration_attempts {20};  ///< Between 1 and 100 attempts.
    std::chrono::milliseconds enumeration_interval {50};  ///< Between 0 and 100 milliseconds.
  };

  struct acquisition_t;

  /**
   * @brief One owned virtual display and its source-lifetime heartbeat; never a viewer lease.
   *
   * Acquisition requires exclusive driver use. Preflight rejects a present matching container and
   * an in-process guard permits one lease. The protocol cannot atomically prove that add created a
   * new monitor: another process can race or return an existing GUID without applying its mode.
   * This API does not claim cross-process ownership, idempotent re-add, mode updates or crash recovery.
   * The global watchdog can disconnect ALL SudoVDA outputs, and handle close does not remove monitors.
   * A one-second watchdog is rejected: the pinned driver's next tick expires even after a ping.
   * Zero disables the watchdog; supported enabled timeouts start at two seconds.
   *
   * Call cleanup explicitly and retain the lease on failure to retry. Destruction is best effort and
   * logs failures; it cannot guarantee removal when the driver fails. Topology/HDR/power/restoration
   * belong to modern Sunshine display management, before the source owner releases this lease.
   */
  class lease_t {
  public:
    ~lease_t();
    lease_t(lease_t &&) noexcept;
    lease_t &operator=(lease_t &&) = delete;
    lease_t(const lease_t &) = delete;
    lease_t &operator=(const lease_t &) = delete;

    /** @brief Acquire a display, returning a recoverable lease even if rollback fails. */
    static acquisition_t acquire(std::unique_ptr<system_t> system, const request_t &request);
    /** @brief Stop/join heartbeat, remove only the owned GUID, then close; failed removal is retryable. */
    std::expected<void, failure_t> cleanup();
    /** @brief Return checked protocol metadata, including TestBuild. */
    wire::version_t protocol() const;
    /** @brief Return the resolved output only after successful enumeration. */
    const std::optional<display_t> &display() const;
    /** @brief Return the first asynchronous ping failure, retained even if later pings recover. */
    std::optional<failure_t> heartbeat_failure() const;

  private:
    struct state_t;
    explicit lease_t(std::unique_ptr<state_t> state);
    std::unique_ptr<state_t> state_;
  };

  /** @brief Acquisition error and optional lease needing explicit cleanup after rollback failure. */
  struct acquisition_t {
    std::unique_ptr<lease_t> lease;  ///< Successful lease, or failed acquisition with resources still owned.
    std::optional<failure_t> failure;  ///< Primary acquisition failure; never hidden by rollback.
    std::optional<failure_t> cleanup_failure;  ///< Separate rollback failure; retain lease and retry cleanup.
  };
}  // namespace platf::sudovda
