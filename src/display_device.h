/**
 * @file src/display_device.h
 * @brief Declarations for display device handling.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#ifdef _WIN32
  #include "platform/windows/sudovda.h"
#endif

// lib includes
#include <display_device/display_power_interface.h>
#include <display_device/settings_manager_interface.h>
#include <display_device/types.h>

// forward declarations
namespace platf {
  class deinit_t;
}

namespace config {
  struct video_t;
}

namespace rtsp_stream {
  struct launch_session_t;
}

namespace display_device {
  /**
   * @brief Initialize the implementation and perform the initial state recovery (if needed).
   * @param persistence_filepath File location for reading/saving persistent state.
   * @param video_config User's video related configuration.
   * @returns A deinit_t instance that performs cleanup when destroyed.
   *
   * @examples
   * const config::video_t &video_config { config::video };
   * const auto init_guard { init("/my/persitence/file.state", video_config) };
   * @examples_end
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> init(const std::filesystem::path &persistence_filepath, const config::video_t &video_config);

  /**
   * @brief Map the output name to a specific display.
   * @param output_name The user-configurable output name.
   * @returns Mapped display name or empty string if the output name could not be mapped.
   *
   * @examples
   * const auto mapped_name_config { map_output_name(config::video.output_name) };
   * const auto mapped_name_custom { map_output_name("{some-device-id}") };
   * @examples_end
   */
  [[nodiscard]] std::string map_output_name(const std::string &output_name);

  /**
   * @brief Ask the platform to wake displays before detection or capture.
   * @param display_name Platform capture selector.
   * @param timeout Maximum time to wait for platform-specific wake detection.
   * @returns True if the display was already available or the wake request succeeded.
   */
  [[nodiscard]] bool wake_display(const std::string &display_name, std::chrono::milliseconds timeout);

  /**
   * @brief Keep displays awake until the returned guard is destroyed.
   * @param reason Short human-readable reason for the power assertion.
   * @returns A guard owning the platform assertion, or nullptr if unsupported or unavailable.
   */
  [[nodiscard]] std::unique_ptr<DisplayPowerGuardInterface> keep_display_awake(const std::string &reason);

  /**
   * @brief Configure the display device based on the user configuration and the session information.
   * @note This is a convenience method for calling similar method of a different signature.
   *
   * @param video_config User's video related configuration.
   * @param session Session information.
   *
   * @examples
   * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
   * const config::video_t &video_config { config::video };
   *
   * configure_display(video_config, *launch_session);
   * @examples_end
   */
  void configure_display(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);

  /**
   * @brief Configure the display device using the provided configuration.
   *
   * In some cases configuring display can fail due to transient issues and
   * we will keep trying every 5 seconds, even if the stream has already started as there was
   * no possibility to apply settings before the stream start.
   *
   * Therefore, there is no return value as we still want to continue with the stream, so that
   * the users can do something about it once they are connected. Otherwise, we might
   * prevent users from logging in at all if we keep failing to apply configuration.
   *
   * @param config Configuration for the display.
   *
   * @examples
   * const SingleDisplayConfiguration valid_config { };
   * configure_display(valid_config);
   * @examples_end
   */
  void configure_display(const SingleDisplayConfiguration &config);

  /**
   * @brief Revert the display configuration and restore the previous state.
   *
   * In case the state could not be restored, by default it will be retried again in 5 seconds
   * (repeating indefinitely until success or until persistence is reset).
   * A recovery that still has settings saved for an unavailable display also keeps retrying
   * until that display returns and those settings are restored.
   *
   * @examples
   * revert_configuration();
   * @examples_end
   */
  void revert_configuration();

  /**
   * @brief Reset persisted display state and the captured initial state.
   *
   * This is normally used to get out of the "broken" state where the algorithm wants
   * to restore the initial display state, but it is no longer possible.
   *
   * This could happen if the display is no longer available or the hardware was changed
   * and the device ids no longer match.
   *
   * The user then accepts that Sunshine is not able to restore the state and "agrees" to
   * do it manually.
   *
   * @return True if persistence was reset, false otherwise.
   * @note Whether the function succeeds or fails, any of the scheduled "retries" from
   *       other methods will be stopped to not interfere with the user actions.
   *
   * @examples
   * const auto result = reset_persistence();
   * @examples_end
   */
  [[nodiscard]] bool reset_persistence();

  /**
   * @brief Enumerate the available devices.
   * @return A list of devices.
   *
   * @examples
   * const auto devices = enumerate_devices();
   * @examples_end
   */
  [[nodiscard]] EnumeratedDeviceList enumerate_devices();

  /**
   * @brief A tag structure indicating that configuration parsing has failed.
   */
  struct failed_to_parse_tag_t {};

  /**
   * @brief A tag structure indicating that configuration is disabled.
   */
  struct configuration_disabled_tag_t {};

  /**
   * @brief Parse the user configuration and the session information.
   * @param video_config User's video related configuration.
   * @param session Session information.
   * @return Parsed single display configuration or
   *         a tag indicating that the parsing has failed or
   *         a tag indicating that the user does not want to perform any configuration.
   *
   * @examples
   * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
   * const config::video_t &video_config { config::video };
   *
   * const auto config { parse_configuration(video_config, *launch_session) };
   * if (const auto *parsed_config { std::get_if<SingleDisplayConfiguration>(&result) }; parsed_config) {
   *    configure_display(*config);
   * }
   * @examples_end
   */
  [[nodiscard]] std::variant<failed_to_parse_tag_t, configuration_disabled_tag_t, SingleDisplayConfiguration> parse_configuration(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);

#ifdef _WIN32
  class WinApiLayerInterface;
  class WinDisplayDeviceInterface;
  class SettingsManagerInterface;
  class AudioContextInterface;

  /** @brief Copied progress for the ordinary native scheduler; no journal or virtual removal authority. */
  struct native_display_status_t {
    std::uint64_t generation {};  ///< Reject stale source operations and callback publication.
    bool source_released = false;  ///< A delayed ordinary revert was successfully scheduled.
    bool recovery_pending = false;  ///< Scheduler or ordinary persistence still requires restoration.
    std::optional<SettingsManagerInterface::ApplyResult> apply_result;  ///< Tolerated ordinary configuration result.
    bool recovery_retry_required = false;  ///< Restoration stopped on an exception; unavailable hardware remains scheduled instead.
  };

  /**
   * @brief Observe ordinary DD_DATA configuration without replacing its scheduler or persistence.
   * @details Construction and status only access local copied state. Mutations run off the owner.
   * Fresh native preparation supersedes its previous retry through the existing scheduler.
   */
  class native_display_configuration_t {
  public:
    /** @brief Allocate local progress only; never initialize native display management. */
    native_display_configuration_t();
    /** @brief Apply the exact parsed configuration, revert when explicitly disabled, or preserve on parse failure. */
    bool begin(const std::optional<SingleDisplayConfiguration> &configuration, bool disabled, std::uint64_t generation, std::chrono::milliseconds delay);
    /** @brief Schedule ordinary delayed restoration and retire the logical source without waiting for physical recovery. */
    bool restore(std::uint64_t generation, std::chrono::milliseconds delay);
    /** @brief Copy generation-fenced progress without entering DD_DATA, the scheduler or native APIs. */
    native_display_status_t status() const;
    /** @brief Check completion only; never reset or erase ordinary persistence. */
    bool complete(std::uint64_t generation) const;

  private:
    struct state_t;
    std::shared_ptr<state_t> state_;  ///< Retry callbacks retain copied progress independently of the logical source.
  };

  /** @brief Observable state of one generation's owned display operation. */
  enum class owned_display_phase_e {
    reserved,
    preparing,
    ready,
    reverting,
    removal_ready,
    recovering,
    cleanup_pending,
    restored,
    failed,
    cancelled
  };

  /** @brief Wrapper validation failure, separate from the library's apply and rollback results. */
  enum class owned_display_error_e {
    observed_source_mismatch,
    headless_unowned_outputs,
    headless_ownership_unproven,
    recovery_state_unproven,
    journal_unavailable,
    operation_exception
  };

  /** @brief A copyable status snapshot; reading it never enters the scheduler or owner. */
  struct owned_display_status_t {
    std::uint64_t generation {};  ///< Generation to compare before acting on this snapshot.
    owned_display_phase_e phase {owned_display_phase_e::reserved};  ///< Current operation state.
    unsigned int attempts {};  ///< Apply or revert attempts in the current operation.
    std::optional<SettingsManagerInterface::ApplyResult> apply_result {};  ///< Last apply result, retained across rollback.
    std::optional<SettingsManagerInterface::RevertResult> revert_result {};  ///< Separate rollback result; never overwrites apply failure.
    std::optional<owned_display_error_e> apply_error {};  ///< Primary wrapper failure, retained across rollback.
    std::optional<owned_display_error_e> restore_error {};  ///< Separate restoration validation failure.
    std::string device_id {};  ///< Exact library identity, never the monitor-container GUID or a fallback.
    std::string display_name {};  ///< Active capture selector, populated only after successful validation.
    std::optional<EnumeratedDevice::Info> observed {};  ///< Observed source properties, separate from requested mode.
    bool unavailable_settings_pending {};  ///< Library recovery survives exact-owned monitor removal.
    bool initially_headless {};  ///< Explicit exception for removing a sole owned output.
    bool source_released {};  ///< Exact removal/close and durable retirement completed; recovery may remain.
    bool removal_permitted {};  ///< Latest readback authorized exact-owned removal; retained for retirement retries.
    bool restore_retry_required {};  ///< Restoration stopped after a validation failure/exception; ordinary unavailable-device retries remain scheduled.
  };

  /**
   * @brief One owned source's existing-library settings manager, journal and observable retry work.
   * Begin before SudoVDA add. This reserves display mutation and blocks legacy configure/revert/reset
   * until this object is released. The caller owns the lease and must release every capture/admission/
   * media pin before cleanup. Keep the lease's heartbeat alive through delayed revert and failures.
   * No callback calls the owner; poll status and compare its generation. Never destroy or cancel this
   * object from its scheduler callback. Native driver cancellation is not guaranteed to finish promptly.
   */
  class owned_display_configuration_t {
  public:
    ~owned_display_configuration_t();
    owned_display_configuration_t(const owned_display_configuration_t &) = delete;
    owned_display_configuration_t &operator=(const owned_display_configuration_t &) = delete;

    /** @brief Snapshot topology/primary and persist intent atomically before any hotplug. */
    static std::expected<std::unique_ptr<owned_display_configuration_t>, platf::sudovda::failure_t> begin(const std::filesystem::path &journal_path, const GUID &identity, std::uint64_t generation, const platf::sudovda::recovery_authority_t &authority);
    /** @brief Reopen existing intent and library recovery state; failed or ambiguous reads block recovery. */
    static std::expected<std::unique_ptr<owned_display_configuration_t>, platf::sudovda::failure_t> recover(const std::filesystem::path &journal_path, const platf::sudovda::recovery_authority_t &authority);
    /** @brief Return durable capability for acquire_owned/recover; it conveys no exclusive-deployment proof. */
    std::shared_ptr<platf::sudovda::ownership_journal_t> journal() const;
    /** @brief Resolve exact target/LUID/path to the library identity; ambiguous/missing mappings fail. */
    bool bind(const platf::sudovda::display_t &display, std::uint64_t generation);
    /** @brief Bind a recovery lease whose checked enumeration proved the exact journal GUID absent. */
    bool bind_absent(const platf::sudovda::lease_t &lease, std::uint64_t generation);
    /** @brief Bind absence proved by this source's exact removal-and-close receipt. */
    bool bind_absent(const platf::sudovda::cleanup_receipt_t &receipt, std::uint64_t generation);
    /** @brief Apply with at most max_attempts; API-unavailable retries use the existing scheduler. */
    bool prepare(const SingleDisplayConfiguration &configuration, std::uint64_t generation, unsigned int max_attempts = 3);
    /** @brief Schedule existing-library restoration, retaining unavailable-device recovery independently. */
    bool restore(std::uint64_t generation, std::chrono::milliseconds delay = std::chrono::milliseconds::zero());
    /** @brief Cancel retry work for this generation without erasing recovery or authorizing removal. */
    bool cancel(std::uint64_t generation);
    /** @brief Retire a safely restored source using its exact generation's removal-and-close receipt. */
    bool retire_source(const platf::sudovda::cleanup_receipt_t &receipt, std::uint64_t generation);
    /**
     * @brief Reuse this recovery episode for a newer source after durable retirement.
     * Fences idle restoration, snapshots before add and rotates intent atomically while retaining
     * existing library settings verbatim. The reconstructed manager has one coherent cache.
     */
    bool begin_next(const GUID &identity, std::uint64_t generation);
    /** @brief Poll operation status without taking scheduler or global display-manager locks. */
    owned_display_status_t status() const;
    /** @brief Finish a retired recovery episode only after library persistence is empty. */
    bool complete(std::uint64_t generation);

  #ifdef SUNSHINE_TESTS
    /** @brief Begin through injected Windows primitives without driver or physical-display access. */
    static std::expected<std::unique_ptr<owned_display_configuration_t>, platf::sudovda::failure_t> test_begin(const std::filesystem::path &journal_path, const GUID &identity, std::uint64_t generation, const platf::sudovda::recovery_authority_t &authority, std::shared_ptr<WinApiLayerInterface> api, std::shared_ptr<WinDisplayDeviceInterface> devices, std::unique_ptr<SettingsManagerInterface> manager = nullptr, std::shared_ptr<AudioContextInterface> audio = nullptr);
    /** @brief Recover through injected Windows primitives and the real library settings manager. */
    static std::expected<std::unique_ptr<owned_display_configuration_t>, platf::sudovda::failure_t> test_recover(const std::filesystem::path &journal_path, const platf::sudovda::recovery_authority_t &authority, std::shared_ptr<WinApiLayerInterface> api, std::shared_ptr<WinDisplayDeviceInterface> devices);
    /** @brief Replace retry sleep durations for deterministic component fixtures. */
    void test_set_retry_interval(std::chrono::milliseconds interval);
    /**
     * @brief Fence in-flight scheduler work before copying status, without executing or cancelling a retry.
     * Native persistence already in progress may delay this test-only barrier. Never call it from a callback.
     */
    owned_display_status_t test_status_after_scheduled_work() const;
  #endif

  private:
    struct state_t;
    explicit owned_display_configuration_t(std::unique_ptr<state_t> state);
    static std::expected<std::unique_ptr<owned_display_configuration_t>, platf::sudovda::failure_t> make(const std::filesystem::path &journal_path, const GUID &identity, std::uint64_t generation, const platf::sudovda::recovery_authority_t &authority, bool recovery, std::shared_ptr<WinApiLayerInterface> api, std::shared_ptr<WinDisplayDeviceInterface> devices, std::unique_ptr<SettingsManagerInterface> manager, std::shared_ptr<AudioContextInterface> audio);
    std::unique_ptr<state_t> state_;
  };
#endif

#ifdef SUNSHINE_TESTS
  class SettingsManagerInterface;
  class SettingsPersistenceInterface;

  /**
   * @brief Create in-memory persistence that records whether settings are still stored.
   * @return Persistence object observed by the revert retry loop.
   */
  [[nodiscard]] std::shared_ptr<SettingsPersistenceInterface> test_make_watched_persistence();

  /**
   * @brief Install a settings manager and the persistence object the revert loop watches.
   * @param manager Settings manager used by later revert calls.
   * @param persistence Persistence object returned by `test_make_watched_persistence()`.
   */
  void test_install_settings_manager(std::unique_ptr<SettingsManagerInterface> manager, const std::shared_ptr<SettingsPersistenceInterface> &persistence);

  /**
   * @brief Override how long the revert loop waits between device checks.
   * @param interval Delay between attempts. Must be greater than zero.
   */
  void test_set_retry_interval(std::chrono::milliseconds interval);

  /**
   * @brief Report whether another revert attempt is still scheduled.
   * @return True when the revert scheduler will run again.
   */
  [[nodiscard]] bool test_revert_retry_is_scheduled();

  /**
   * @brief Stop the revert scheduler and drop the test settings manager.
   */
  void test_reset_display_device();
#endif
}  // namespace display_device
