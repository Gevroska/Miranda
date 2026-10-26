/**
 * @file src/display_device.cpp
 * @brief Definitions for display device handling.
 */
// header include
#include "display_device.h"

// standard includes
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string_view>
#include <tuple>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <display_device/audio_context_interface.h>
#include <display_device/file_settings_persistence.h>
#include <display_device/json.h>
#include <display_device/retry_scheduler.h>
#include <display_device/settings_manager_interface.h>

// local includes
#include "audio.h"
#include "platform/common.h"
#include "rtsp.h"

// platform-specific includes
#ifdef _WIN32
  #include "platform/windows/utf_utils.h"

  #include <display_device/windows/json.h>
  #include <display_device/windows/settings_manager.h>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>
#endif

#ifdef __APPLE__
  #include <display_device/macos/display_power.h>
  #include <display_device/macos/mac_api_layer.h>
  #include <display_device/macos/mac_display_device.h>
  #include <display_device/macos/settings_manager.h>
#endif

namespace display_device {
  namespace {
    constexpr std::chrono::milliseconds DEFAULT_RETRY_INTERVAL {5000};

#ifdef _WIN32
    std::atomic_bool startup_excluded {false};  ///< Copied gate, independent of native manager lifetime.
    std::atomic_bool startup_legacy_tracked {false};  ///< Preserve default behavior until startup inspection participates.
    std::atomic_bool startup_legacy_pending {false};  ///< Updated by the ordinary persistence authority after adoption.

    void publish_ordinary_persistence(bool pending) {
      if (startup_legacy_tracked.load()) {
        startup_legacy_pending = pending;
      }
    }
#endif

    /**
     * @brief Persistence wrapper that remembers whether a settings record is still stored.
     *
     * `revertSettings()` returns success both when the original layout is fully restored and when an
     * unplugged display's settings are intentionally kept. The kept record is the library's signal that
     * another revert is still required: a store leaves the record in place, and a clear removes it.
     */
    class watched_settings_persistence_t: public SettingsPersistenceInterface {
    public:
      /**
       * @brief Wrap a persistence implementation and adopt its current contents.
       * @param inner Persistence implementation that reads and writes the record.
       */
      explicit watched_settings_persistence_t(std::shared_ptr<SettingsPersistenceInterface> inner):
          inner {std::move(inner)} {
        const auto loaded = this->inner->load();
        pending_restores = loaded && !loaded->empty();
#ifdef _WIN32
        if (!loaded && startup_legacy_tracked.load()) {
          pending_restores = true;
        }
        publish_ordinary_persistence(pending_restores.load());
#endif
      }

      /**
       * @copydoc SettingsPersistenceInterface::store
       */
      [[nodiscard]] bool store(const std::vector<std::uint8_t> &data) override {
        if (!inner->store(data)) {
          return false;
        }

        pending_restores = !data.empty();
#ifdef _WIN32
        publish_ordinary_persistence(!data.empty());
#endif
        return true;
      }

      /**
       * @copydoc SettingsPersistenceInterface::load
       */
      [[nodiscard]] std::optional<std::vector<std::uint8_t>> load() const override {
        return inner->load();
      }

      /**
       * @copydoc SettingsPersistenceInterface::clear
       */
      [[nodiscard]] bool clear() override {
        if (!inner->clear()) {
          return false;
        }

        pending_restores = false;
#ifdef _WIN32
        publish_ordinary_persistence(false);
#endif
        return true;
      }

      /**
       * @brief Report whether a settings record is still stored.
       * @return True when a previous store has not been cleared.
       */
      [[nodiscard]] bool has_pending_restores() const {
        return pending_restores;
      }

    private:
      std::shared_ptr<SettingsPersistenceInterface> inner;  ///< Persistence implementation that owns the stored bytes.
      std::atomic_bool pending_restores {false};  ///< True while a non-empty settings record is stored.
    };

    /**
     * @brief A global for the settings manager interface and other settings whose lifetime is managed by `display_device::init(...)`.
     */
    struct {
      std::mutex mutex {};
      std::chrono::milliseconds config_revert_delay {0};
      std::unique_ptr<RetryScheduler<SettingsManagerInterface>> sm_instance {nullptr};
      std::shared_ptr<watched_settings_persistence_t> persistence_watch {};  ///< Observes whether revert left settings stored.
#ifdef _WIN32
      std::weak_ptr<int> owned_reservation;  ///< Excludes legacy mutation while a managed source is reserved.
      std::shared_ptr<ordinary_startup_exclusion_t> startup_exclusion;  ///< Sticky across refused/failed reconstruction and coordinator destruction.
      std::optional<std::filesystem::path> startup_ordinary_path;  ///< Bind ordinary initialization to the inspected persistence location.
#endif
#ifdef SUNSHINE_TESTS
      std::optional<std::chrono::milliseconds> test_retry_interval {};  ///< Override for the revert retry delay.
#endif
    } DD_DATA;

    /**
     * @brief Helper class for capturing audio context when the API demands it.
     *
     * The capture is needed to be done in case some of the displays are going
     * to be deactivated before the stream starts. In this case the audio context
     * will be captured for this display and can be restored once it is turned back.
     */
    class sunshine_audio_context_t: public AudioContextInterface {
    public:
      [[nodiscard]] bool capture() override {
        return context_scheduler.execute([](auto &audio_context) {
          // Explicitly releasing the context first in case it was not release yet so that it can be potentially cleaned up.
          audio_context = boost::none;
          audio_context = audio_context_t {};

          // Always say that we have captured it successfully as otherwise the settings change procedure will be aborted.
          return true;
        });
      }

      [[nodiscard]] bool isCaptured() const override {
        return context_scheduler.execute([](const auto &audio_context) {
          if (audio_context) {
            // In case we still have context we need to check whether it was released or not.
            // If it was released we can pretend that we no longer have it as it will be immediately cleaned up in `capture` method before we acquire new context.
            return !audio_context->released;
          }

          return false;
        });
      }

      void release() override {
        context_scheduler.schedule([](auto &audio_context, auto &stop_token) {
          if (audio_context) {
            audio_context->released = true;

            const auto *audio_ctx_ptr = audio_context->audio_ctx_ref.get();
            if (audio_ctx_ptr && !audio::is_audio_ctx_sink_available(*audio_ctx_ptr) && audio_context->retry_counter > 0) {
              // It is possible that the audio sink is not immediately available after the display is turned on.
              // Therefore, we will hold on to the audio context a little longer, until it is either available
              // or we time out.
              --audio_context->retry_counter;
              return;
            }
          }

          audio_context = boost::none;
          stop_token.requestStop();
        },
                                   SchedulerOptions {.m_sleep_durations = {2s}});
      }

    private:
      struct audio_context_t {
        /**
         * @brief A reference to the audio context that will automatically extend the audio session.
         * @note It is auto-initialized here for convenience.
         */
        decltype(audio::get_audio_ctx_ref()) audio_ctx_ref {audio::get_audio_ctx_ref()};

        /**
         * @brief Will be set to true if the capture was released, but we still have to keep the context around, because the device is not available.
         */
        bool released {false};

        /**
         * @brief How many times to check if the audio sink is available before giving up.
         */
        int retry_counter {15};
      };

      RetryScheduler<boost::optional<audio_context_t>> context_scheduler {std::make_unique<boost::optional<audio_context_t>>(boost::none)};
    };

    /**
     * @brief Convert string to unsigned int.
     * @note For random reason there is std::stoi, but not std::stou...
     * @param value String to be converted
     * @return Parsed unsigned integer.
     */
    unsigned int stou(const std::string &value) {
      unsigned long result {std::stoul(value)};
      if (result > std::numeric_limits<unsigned int>::max()) {
        throw std::out_of_range("stou");
      }
      return (int) result;
    }

#ifdef __APPLE__
    bool is_unsigned_integer(std::string_view value) {
      return !value.empty() && std::ranges::all_of(value, [](unsigned char character) {
        return std::isdigit(character);
      });
    }
#endif

    std::string_view apply_result_name(SettingsManagerInterface::ApplyResult result) {
      using enum SettingsManagerInterface::ApplyResult;

      switch (result) {
        case Ok:
          return "Ok";
        case ApiTemporarilyUnavailable:
          return "ApiTemporarilyUnavailable";
        case DevicePrepFailed:
          return "DevicePrepFailed";
        case PrimaryDevicePrepFailed:
          return "PrimaryDevicePrepFailed";
        case DisplayModePrepFailed:
          return "DisplayModePrepFailed";
        case HdrStatePrepFailed:
          return "HdrStatePrepFailed";
        case PersistenceSaveFailed:
          return "PersistenceSaveFailed";
      }

      return "Unknown";
    }

    /**
     * @brief Parse resolution value from the string.
     * @param input String to be parsed.
     * @param output Reference to output variable to fill in.
     * @returns True on successful parsing (empty string allowed), false otherwise.
     *
     * @examples
     * std::optional<Resolution> resolution;
     * if (parse_resolution_string("1920x1080", resolution)) {
     *   if (resolution) {
     *     BOOST_LOG(info) << "Value was specified";
     *   }
     *   else {
     *     BOOST_LOG(info) << "Value was empty";
     *   }
     * }
     * @examples_end
     */
    bool parse_resolution_string(const std::string &input, std::optional<Resolution> &output) {
      const std::string trimmed_input {boost::algorithm::trim_copy(input)};
      const std::regex resolution_regex {R"(^(\d+)x(\d+)$)"};

      if (std::smatch match; std::regex_match(trimmed_input, match, resolution_regex)) {
        try {
          output = Resolution {
            stou(match[1].str()),
            stou(match[2].str())
          };
          return true;
        } catch (const std::out_of_range &) {
          BOOST_LOG(error) << "Failed to parse resolution string " << trimmed_input << " (number out of range).";
        } catch (const std::exception &err) {
          BOOST_LOG(error) << "Failed to parse resolution string " << trimmed_input << ":\n"
                           << err.what();
        }
      } else {
        if (trimmed_input.empty()) {
          output = std::nullopt;
          return true;
        }

        BOOST_LOG(error) << "Failed to parse resolution string " << trimmed_input << R"(. It must match a "1920x1080" pattern!)";
      }

      return false;
    }

    /**
     * @brief Parse refresh rate value from the string.
     * @param input String to be parsed.
     * @param output Reference to output variable to fill in.
     * @param allow_decimal_point Specify whether the decimal point is allowed or not.
     * @returns True on successful parsing (empty string allowed), false otherwise.
     *
     * @examples
     * std::optional<FloatingPoint> refresh_rate;
     * if (parse_refresh_rate_string("59.95", refresh_rate)) {
     *   if (refresh_rate) {
     *     BOOST_LOG(info) << "Value was specified";
     *   }
     *   else {
     *     BOOST_LOG(info) << "Value was empty";
     *   }
     * }
     * @examples_end
     */
    bool parse_refresh_rate_string(const std::string &input, std::optional<FloatingPoint> &output, const bool allow_decimal_point = true) {
      static const auto is_zero {[](const auto &character) {
        return character == '0';
      }};
      const std::string trimmed_input {boost::algorithm::trim_copy(input)};
      const std::regex refresh_rate_regex {allow_decimal_point ? R"(^(\d+)(?:\.(\d+))?$)" : R"(^(\d+)$)"};

      if (std::smatch match; std::regex_match(trimmed_input, match, refresh_rate_regex)) {
        try {
          // Here we are trimming zeros from the string to possibly reduce out of bounds case
          std::string trimmed_match_1 {boost::algorithm::trim_left_copy_if(match[1].str(), is_zero)};
          if (trimmed_match_1.empty()) {
            trimmed_match_1 = "0"s;  // Just in case ALL the string is full of zeros, we want to leave one
          }

          std::string trimmed_match_2;
          if (allow_decimal_point && match[2].matched) {
            trimmed_match_2 = boost::algorithm::trim_right_copy_if(match[2].str(), is_zero);
          }

          if (!trimmed_match_2.empty()) {
            // We have a decimal point and will have to split it into numerator and denominator.
            // For example:
            //   59.995:
            //     numerator = 59995
            //     denominator = 1000

            // We are essentially removing the decimal point here: 59.995 -> 59995
            const std::string numerator_str {trimmed_match_1 + trimmed_match_2};
            const auto numerator {stou(numerator_str)};

            // Here we are counting decimal places and calculating denominator: 10^decimal_places
            const auto denominator {static_cast<unsigned int>(std::pow(10, trimmed_match_2.size()))};

            output = Rational {numerator, denominator};
          } else {
            // We do not have a decimal point, just a valid number.
            // For example:
            //   60:
            //     numerator = 60
            //     denominator = 1
            output = Rational {stou(trimmed_match_1), 1};
          }
          return true;
        } catch (const std::out_of_range &) {
          BOOST_LOG(error) << "Failed to parse refresh rate string " << trimmed_input << " (number out of range).";
        } catch (const std::exception &err) {
          BOOST_LOG(error) << "Failed to parse refresh rate string " << trimmed_input << ":\n"
                           << err.what();
        }
      } else {
        if (trimmed_input.empty()) {
          output = std::nullopt;
          return true;
        }

        BOOST_LOG(error) << "Failed to parse refresh rate string " << trimmed_input << ". Must have a pattern of " << (allow_decimal_point ? R"("123" or "123.456")" : R"("123")") << "!";
      }

      return false;
    }

    /**
     * @brief Parse device preparation option from the user configuration and the session information.
     * @param video_config User's video related configuration.
     * @returns Parsed device preparation value we need to use.
     *          Empty optional if no preparation nor configuration shall take place.
     *
     * @examples
     * const config::video_t &video_config { config::video };
     * const auto device_prep_option = parse_device_prep_option(video_config);
     * @examples_end
     */
    std::optional<SingleDisplayConfiguration::DevicePreparation> parse_device_prep_option(const config::video_t &video_config) {
      using enum config::video_t::dd_t::config_option_e;
      using enum SingleDisplayConfiguration::DevicePreparation;

      switch (video_config.dd.configuration_option) {
        case verify_only:
          return VerifyOnly;
        case ensure_active:
          return EnsureActive;
        case ensure_primary:
          return EnsurePrimary;
        case ensure_only_display:
          return EnsureOnlyDisplay;
        case disabled:
          break;
      }

      return std::nullopt;
    }

    /**
     * @brief Parse resolution option from the user configuration and the session information.
     * @param video_config User's video related configuration.
     * @param session Session information.
     * @param config A reference to a display config object that will be modified on success.
     * @returns True on successful parsing, false otherwise.
     *
     * @examples
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
     * const config::video_t &video_config { config::video };
     *
     * SingleDisplayConfiguration config;
     * const bool success = parse_resolution_option(video_config, *launch_session, config);
     * @examples_end
     */
    bool parse_resolution_option(const config::video_t &video_config, const rtsp_stream::launch_session_t &session, SingleDisplayConfiguration &config) {
      using resolution_option_e = config::video_t::dd_t::resolution_option_e;

      switch (video_config.dd.resolution_option) {
        case resolution_option_e::automatic:
          {
            if (!session.enable_sops) {
              BOOST_LOG(warning) << R"(Sunshine is configured to change resolution automatically, but the "Optimize game settings" is not set in the client! Resolution will not be changed.)";
            } else if (session.width >= 0 && session.height >= 0) {
              config.m_resolution = Resolution {
                static_cast<unsigned int>(session.width),
                static_cast<unsigned int>(session.height)
              };
            } else {
              BOOST_LOG(error) << "Resolution provided by client session config is invalid: " << session.width << "x" << session.height;
              return false;
            }
            break;
          }
        case resolution_option_e::manual:
          {
            if (!session.enable_sops) {
              BOOST_LOG(warning) << R"(Sunshine is configured to change resolution manually, but the "Optimize game settings" is not set in the client! Resolution will not be changed.)";
            } else {
              if (!parse_resolution_string(video_config.dd.manual_resolution, config.m_resolution)) {
                BOOST_LOG(error) << "Failed to parse manual resolution string!";
                return false;
              }

              if (!config.m_resolution) {
                BOOST_LOG(error) << "Manual resolution must be specified!";
                return false;
              }
            }
            break;
          }
        case resolution_option_e::disabled:
          break;
      }

      return true;
    }

    /**
     * @brief Parse refresh rate option from the user configuration and the session information.
     * @param video_config User's video related configuration.
     * @param session Session information.
     * @param config A reference to a config object that will be modified on success.
     * @returns True on successful parsing, false otherwise.
     *
     * @examples
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
     * const config::video_t &video_config { config::video };
     *
     * SingleDisplayConfiguration config;
     * const bool success = parse_refresh_rate_option(video_config, *launch_session, config);
     * @examples_end
     */
    bool parse_refresh_rate_option(const config::video_t &video_config, const rtsp_stream::launch_session_t &session, SingleDisplayConfiguration &config) {
      using refresh_rate_option_e = config::video_t::dd_t::refresh_rate_option_e;

      switch (video_config.dd.refresh_rate_option) {
        case refresh_rate_option_e::automatic:
          {
            if (session.fps >= 0) {
              config.m_refresh_rate = Rational {static_cast<unsigned int>(session.fps), 1};
            } else {
              BOOST_LOG(error) << "FPS value provided by client session config is invalid: " << session.fps;
              return false;
            }
            break;
          }
        case refresh_rate_option_e::manual:
          {
            if (!parse_refresh_rate_string(video_config.dd.manual_refresh_rate, config.m_refresh_rate)) {
              BOOST_LOG(error) << "Failed to parse manual refresh rate string!";
              return false;
            }

            if (!config.m_refresh_rate) {
              BOOST_LOG(error) << "Manual refresh rate must be specified!";
              return false;
            }
            break;
          }
        case refresh_rate_option_e::disabled:
          break;
      }

      return true;
    }

    /**
     * @brief Parse HDR option from the user configuration and the session information.
     * @param video_config User's video related configuration.
     * @param session Session information.
     * @returns Parsed HDR state value we need to switch to.
     *          Empty optional if no action is required.
     *
     * @examples
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
     * const config::video_t &video_config { config::video };
     * const auto hdr_option = parse_hdr_option(video_config, *launch_session);
     * @examples_end
     */
    std::optional<HdrState> parse_hdr_option(const config::video_t &video_config, const rtsp_stream::launch_session_t &session) {
      using hdr_option_e = config::video_t::dd_t::hdr_option_e;

      switch (video_config.dd.hdr_option) {
        case hdr_option_e::automatic:
          return session.enable_hdr ? HdrState::Enabled : HdrState::Disabled;
        case hdr_option_e::disabled:
          break;
      }

      return std::nullopt;
    }

    /**
     * @brief Indicates which remapping fields and config structure shall be used.
     */
    enum class remapping_type_e {
      mixed,  ///! Both reseolution and refresh rate may be remapped
      resolution_only,  ///! Only resolution will be remapped
      refresh_rate_only  ///! Only refresh rate will be remapped
    };

    /**
     * @brief Determine the ramapping type from the user config.
     * @param video_config User's video related configuration.
     * @returns Enum value if remapping can be performed, null optional if remapping shall be skipped.
     */
    std::optional<remapping_type_e> determine_remapping_type(const config::video_t &video_config) {
      using dd_t = config::video_t::dd_t;
      const bool auto_resolution {video_config.dd.resolution_option == dd_t::resolution_option_e::automatic};
      const bool auto_refresh_rate {video_config.dd.refresh_rate_option == dd_t::refresh_rate_option_e::automatic};

      if (auto_resolution && auto_refresh_rate) {
        return remapping_type_e::mixed;
      }

      if (auto_resolution) {
        return remapping_type_e::resolution_only;
      }

      if (auto_refresh_rate) {
        return remapping_type_e::refresh_rate_only;
      }

      return std::nullopt;
    }

    /**
     * @brief Enumerates supported contains remapping data parsed from the string options.
     */
    struct parsed_remapping_entry_t {
      std::optional<Resolution> requested_resolution;
      std::optional<FloatingPoint> requested_fps;
      std::optional<Resolution> final_resolution;
      std::optional<FloatingPoint> final_refresh_rate;
    };

    /**
     * @brief Check if resolution is to be mapped based on remmaping type.
     * @param type Remapping type to check.
     * @returns True if resolution is to be mapped, false otherwise.
     */
    bool is_resolution_mapped(const remapping_type_e type) {
      return type == remapping_type_e::resolution_only || type == remapping_type_e::mixed;
    }

    /**
     * @brief Check if FPS is to be mapped based on remmaping type.
     * @param type Remapping type to check.
     * @returns True if FPS is to be mapped, false otherwise.
     */
    bool is_fps_mapped(const remapping_type_e type) {
      return type == remapping_type_e::refresh_rate_only || type == remapping_type_e::mixed;
    }

    /**
     * @brief Parse the remapping entry from the config into an internal structure.
     * @param entry Entry to parse.
     * @param type Specify which entry fields should be parsed.
     * @returns Parsed structure or null optional if a necessary field could not be parsed.
     */
    std::optional<parsed_remapping_entry_t> parse_remapping_entry(const config::video_t::dd_t::mode_remapping_entry_t &entry, const remapping_type_e type) {
      parsed_remapping_entry_t result {};

      if (is_resolution_mapped(type) && (!parse_resolution_string(entry.requested_resolution, result.requested_resolution) || !parse_resolution_string(entry.final_resolution, result.final_resolution))) {
        return std::nullopt;
      }

      if (is_fps_mapped(type) && (!parse_refresh_rate_string(entry.requested_fps, result.requested_fps, false) || !parse_refresh_rate_string(entry.final_refresh_rate, result.final_refresh_rate))) {
        return std::nullopt;
      }

      return result;
    }

    /**
     * @brief Remap the the requested display mode based on the config.
     * @param video_config User's video related configuration.
     * @param session Session information.
     * @param config A reference to a config object that will be modified on success.
     * @returns True if the remapping was performed or skipped, false if remapping has failed due to invalid config.
     *
     * @examples
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
     * const config::video_t &video_config { config::video };
     *
     * SingleDisplayConfiguration config;
     * const bool success = remap_display_mode_if_needed(video_config, *launch_session, config);
     * @examples_end
     */
    bool remap_display_mode_if_needed(const config::video_t &video_config, const rtsp_stream::launch_session_t &session, SingleDisplayConfiguration &config) {
      const auto remapping_type {determine_remapping_type(video_config)};
      if (!remapping_type) {
        return true;
      }

      const auto &remapping_list {[&]() {
        using enum remapping_type_e;

        switch (*remapping_type) {
          case resolution_only:
            return video_config.dd.mode_remapping.resolution_only;
          case refresh_rate_only:
            return video_config.dd.mode_remapping.refresh_rate_only;
          case mixed:
          default:
            return video_config.dd.mode_remapping.mixed;
        }
      }()};

      if (remapping_list.empty()) {
        BOOST_LOG(debug) << "No values are available for display mode remapping.";
        return true;
      }
      BOOST_LOG(debug) << "Trying to remap display modes...";

      const auto entry_to_string {[type = *remapping_type](const config::video_t::dd_t::mode_remapping_entry_t &entry) {
        const bool mapping_resolution {is_resolution_mapped(type)};
        const bool mapping_fps {is_fps_mapped(type)};

        // clang-format off
        return (mapping_resolution ? "  - requested resolution: "s + entry.requested_resolution + "\n" : "") +
               (mapping_fps ?        "  - requested FPS: "s + entry.requested_fps + "\n" : "") +
               (mapping_resolution ? "  - final resolution: "s + entry.final_resolution + "\n" : "") +
               (mapping_fps ?        "  - final refresh rate: "s + entry.final_refresh_rate : "");
        // clang-format on
      }};

      for (const auto &entry : remapping_list) {
        const auto parsed_entry {parse_remapping_entry(entry, *remapping_type)};
        if (!parsed_entry) {
          BOOST_LOG(error) << "Failed to parse remapping entry from:\n"
                           << entry_to_string(entry);
          return false;
        }

        if (!parsed_entry->final_resolution && !parsed_entry->final_refresh_rate) {
          BOOST_LOG(error) << "At least one final value must be set for remapping display modes! Entry:\n"
                           << entry_to_string(entry);
          return false;
        }

        if (!session.enable_sops && (parsed_entry->requested_resolution || parsed_entry->final_resolution)) {
          BOOST_LOG(warning) << R"(Skipping remapping entry, because the "Optimize game settings" is not set in the client! Entry:\n)"
                             << entry_to_string(entry);
          continue;
        }

        // Note: at this point config should already have parsed resolution set.
        if (parsed_entry->requested_resolution && parsed_entry->requested_resolution != config.m_resolution) {
          BOOST_LOG(verbose) << "Skipping remapping because requested resolutions do not match! Entry:\n"
                             << entry_to_string(entry);
          continue;
        }

        // Note: at this point config should already have parsed refresh rate set.
        if (parsed_entry->requested_fps && parsed_entry->requested_fps != config.m_refresh_rate) {
          BOOST_LOG(verbose) << "Skipping remapping because requested FPS do not match! Entry:\n"
                             << entry_to_string(entry);
          continue;
        }

        BOOST_LOG(info) << "Remapping requested display mode. Entry:\n"
                        << entry_to_string(entry);
        if (parsed_entry->final_resolution) {
          config.m_resolution = parsed_entry->final_resolution;
        }
        if (parsed_entry->final_refresh_rate) {
          config.m_refresh_rate = parsed_entry->final_refresh_rate;
        }
        break;
      }

      return true;
    }

#if defined(_WIN32) || defined(__APPLE__)
    /**
     * @brief Create the persistence object shared by the settings manager and the revert loop.
     * @param persistence_filepath File location for saving persistent state.
     * @return Persistence wrapper that reports whether a settings record remains stored.
     */
    std::shared_ptr<watched_settings_persistence_t> make_persistence_watch(const std::filesystem::path &persistence_filepath) {
      auto watch {std::make_shared<watched_settings_persistence_t>(std::make_shared<FileSettingsPersistence>(persistence_filepath))};
      DD_DATA.persistence_watch = watch;
      return watch;
    }
#endif

    /**
     * @brief Construct a settings manager interface to manage display device settings.
     * @param persistence_filepath File location for saving persistent state.
     * @param video_config User's video related configuration.
     * @return An interface or nullptr if the OS does not support the interface.
     */
    std::unique_ptr<SettingsManagerInterface> make_settings_manager([[maybe_unused]] const std::filesystem::path &persistence_filepath, [[maybe_unused]] const config::video_t &video_config) {
      DD_DATA.persistence_watch.reset();
#ifdef _WIN32
      return std::make_unique<SettingsManager>(
        std::make_shared<WinDisplayDevice>(std::make_shared<WinApiLayer>(), false),
        std::make_shared<sunshine_audio_context_t>(),
        std::make_unique<PersistentState>(make_persistence_watch(persistence_filepath)),
        WinWorkarounds {
          .m_hdr_blank_delay = video_config.dd.wa.hdr_toggle_delay != std::chrono::milliseconds::zero() ? std::make_optional(video_config.dd.wa.hdr_toggle_delay) : std::nullopt
        }
      );
#elif defined(__APPLE__)
      return std::make_unique<MacSettingsManager>(
        std::make_shared<MacDisplayDevice>(std::make_shared<MacApiLayer>()),
        std::make_shared<sunshine_audio_context_t>(),
        std::make_unique<MacPersistentState>(make_persistence_watch(persistence_filepath)),
        MacWorkarounds {}
      );
#else
      return nullptr;
#endif
    }

    std::unique_ptr<DisplayPowerInterface> make_display_power() {
#ifdef __APPLE__
      return std::make_unique<MacDisplayPower>(std::make_shared<MacApiLayer>());
#else
      return nullptr;
#endif
    }

    /**
     * @brief Defines the "revert config" algorithms.
     */
    enum class revert_option_e {
      try_once,  ///< Try reverting once and then abort.
      try_indefinitely,  ///< Keep trying to revert indefinitely.
      try_indefinitely_with_delay  ///< Keep trying to revert indefinitely, but delay the first try by some amount of time.
    };

    /**
     * @brief Attempt one scheduled revert, stopping when the saved layout is fully restored.
     * @param try_once Stop after a single revert regardless of the result.
     * @param tried_out_devices Device snapshot from the previous attempt. Updated when another try must wait for a device change.
     * @param persistence_watch Observes whether a settings record is still stored.
     * @param settings_iface Settings manager that performs the revert.
     * @param stop_token Token used to stop the scheduler once the revert is complete.
     */
    void try_revert_settings(const bool try_once, StringSet &tried_out_devices, const std::shared_ptr<watched_settings_persistence_t> &persistence_watch, SettingsManagerInterface &settings_iface, SchedulerStopToken &stop_token) {
      if (try_once) {
        std::ignore = settings_iface.revertSettings();
        stop_token.requestStop();
        return;
      }

      auto available_devices {[&settings_iface]() {
        const auto devices {settings_iface.enumAvailableDevices()};
        StringSet parsed_devices;

        std::transform(
          std::begin(devices),
          std::end(devices),
          std::inserter(parsed_devices, std::end(parsed_devices)),
          [](const auto &device) {
            return device.m_device_id + " - " + device.m_friendly_name;
          }
        );

        return parsed_devices;
      }()};
      if (available_devices == tried_out_devices) {
        BOOST_LOG(debug) << "Skipping reverting configuration, because no newly added/removed devices were detected since last check. Currently available devices:\n"
                         << toJson(available_devices);
        return;
      }

      using enum SettingsManagerInterface::RevertResult;
      const auto result {settings_iface.revertSettings()};
      // A saved record after success means an unplugged display still has HDR, mode, or primary settings to restore.
      const bool restores_pending {persistence_watch && persistence_watch->has_pending_restores()};
      if (result == Ok && !restores_pending) {
        stop_token.requestStop();
        return;
      }
      if (result == ApiTemporarilyUnavailable) {
        // Do nothing and retry next time
        return;
      }

      // Try again only after a device is added or removed. That includes redocking a display whose settings are still saved.
      if (result == Ok) {
        BOOST_LOG(info) << "Recovered the available display layout, but settings for an unavailable display are still pending. Will retry once devices are added or removed. Currently available devices:\n"
                        << toJson(available_devices);
      } else {
        BOOST_LOG(warning) << "Failed to revert display device configuration (will retry once devices are added or removed). Enabling all of the available devices:\n"
                           << toJson(available_devices);
      }
      tried_out_devices.swap(available_devices);
    }

    /**
     * @brief Reverts the configuration based on the provided option.
     * @note This is function does not lock mutex.
     */
    void revert_configuration_unlocked(const revert_option_e option) {
      if (!DD_DATA.sm_instance) {
        // Platform is not supported, nothing to do.
        return;
      }

      // Note: by default the executor function is immediately executed in the calling thread. With delay, we want to avoid that.
#ifdef SUNSHINE_TESTS
      const auto retry_interval {DD_DATA.test_retry_interval.value_or(DEFAULT_RETRY_INTERVAL)};
#else
      const auto retry_interval {DEFAULT_RETRY_INTERVAL};
#endif
      SchedulerOptions scheduler_option {.m_sleep_durations = {retry_interval}};
      if (option == revert_option_e::try_indefinitely_with_delay && DD_DATA.config_revert_delay > std::chrono::milliseconds::zero()) {
        scheduler_option.m_sleep_durations = {DD_DATA.config_revert_delay, retry_interval};
        scheduler_option.m_execution = SchedulerOptions::Execution::ScheduledOnly;
      }

      const auto persistence_watch {DD_DATA.persistence_watch};
      DD_DATA.sm_instance->schedule([try_once = (option == revert_option_e::try_once), tried_out_devices = StringSet {}, persistence_watch](auto &settings_iface, auto &stop_token) mutable {
        try_revert_settings(try_once, tried_out_devices, persistence_watch, settings_iface, stop_token);
      },
                                    scheduler_option);
    }

    /** @brief Construct the ordinary DD lifetime guard without initializing a settings manager. */
    std::unique_ptr<platf::deinit_t> make_deinit_guard() {
      class deinit_t: public platf::deinit_t {
      public:
        ~deinit_t() override {
          std::lock_guard lock {DD_DATA.mutex};
#ifdef _WIN32
          if (DD_DATA.startup_exclusion || !DD_DATA.owned_reservation.expired()) {
            DD_DATA.sm_instance.reset();
            return;
          }
#endif
          try {
            // This may throw if used incorrectly. At the moment this will not happen, however
            // in case some unforeseen changes are made that could raise an exception,
            // we definitely don't want this to happen in destructor. Especially in the
            // deinit_t where the outcome does not really matter.
            revert_configuration_unlocked(revert_option_e::try_once);
          } catch (std::exception &err) {
            BOOST_LOG(fatal) << err.what();
          }

          DD_DATA.sm_instance = nullptr;
        }
      };

      return std::make_unique<deinit_t>();
    }
  }  // namespace

  std::unique_ptr<platf::deinit_t> init(const std::filesystem::path &persistence_filepath, const config::video_t &video_config) {
    std::lock_guard lock {DD_DATA.mutex};
#ifdef _WIN32
    if (DD_DATA.startup_exclusion || !DD_DATA.owned_reservation.expired()) {
      return nullptr;
    }
    if (DD_DATA.startup_ordinary_path && persistence_filepath != *DD_DATA.startup_ordinary_path) {
      return nullptr;
    }
#endif
    // We can support re-init without any issues, however we should make sure to clean up first!
    revert_configuration_unlocked(revert_option_e::try_once);
    DD_DATA.config_revert_delay = video_config.dd.config_revert_delay;
    DD_DATA.sm_instance = nullptr;

    // If we fail to create settings manager, this means platform is not supported, and
    // we will need to provided error-free pass-trough in other methods
    if (auto settings_manager {make_settings_manager(persistence_filepath, video_config)}) {
      DD_DATA.sm_instance = std::make_unique<RetryScheduler<SettingsManagerInterface>>(std::move(settings_manager));

      const auto available_devices {DD_DATA.sm_instance->execute([](auto &settings_iface) {
        return settings_iface.enumAvailableDevices();
      })};
      BOOST_LOG(info) << "Currently available display devices:\n"
                      << toJson(available_devices);

      // In case we have failed to revert configuration before shutting down, we should
      // do it now.
      revert_configuration_unlocked(revert_option_e::try_indefinitely);
    }

    return make_deinit_guard();
  }

  bool wake_display(const std::string &display_name, std::chrono::milliseconds timeout) {
    const auto display_power {make_display_power()};
    if (!display_power) {
      return false;
    }

    return display_power->wakeDisplay(display_name, timeout);
  }

  std::unique_ptr<DisplayPowerGuardInterface> keep_display_awake(const std::string &reason) {
    const auto display_power {make_display_power()};
    if (!display_power) {
      return nullptr;
    }

    return display_power->keepDisplayAwake(reason);
  }

  std::string map_output_name(const std::string &output_name) {
    std::lock_guard lock {DD_DATA.mutex};
    if (!DD_DATA.sm_instance) {
      // Fallback to giving back the output name if the platform is not supported.
      return output_name;
    }

    const auto mapped_name {DD_DATA.sm_instance->execute([&output_name](auto &settings_iface) {
      return settings_iface.getDisplayName(output_name);
    })};

#ifdef __APPLE__
    if (mapped_name.empty() && is_unsigned_integer(output_name)) {
      return output_name;
    }
#endif

    return mapped_name;
  }

  void configure_display(const config::video_t &video_config, const rtsp_stream::launch_session_t &session) {
    const auto result {parse_configuration(video_config, session)};
    if (const auto *parsed_config {std::get_if<SingleDisplayConfiguration>(&result)}; parsed_config) {
      configure_display(*parsed_config);
      return;
    }

    if (const auto *disabled {std::get_if<configuration_disabled_tag_t>(&result)}; disabled) {
      BOOST_LOG(info) << "Display device configuration is disabled. Reverting any active display device configuration.";
      revert_configuration();
      return;
    }

    BOOST_LOG(error) << "Failed to parse display device configuration. Display settings will not be changed.";
    // Error details should already be logged for failed_to_parse_tag_t case, and we also don't
    // want to revert active configuration in case we have any
  }

  void configure_display(const SingleDisplayConfiguration &config) {
    std::lock_guard lock {DD_DATA.mutex};
#ifdef _WIN32
    if (DD_DATA.startup_exclusion || !DD_DATA.owned_reservation.expired()) {
      BOOST_LOG(warning) << "Ignoring unmanaged display configuration while display recovery excludes ordinary mutation.";
      return;
    }
#endif
    if (!DD_DATA.sm_instance) {
      // Platform is not supported, nothing to do.
      return;
    }

    BOOST_LOG(info) << "Scheduling display device configuration:\n"
                    << toJson(config);

    DD_DATA.sm_instance->schedule([config](auto &settings_iface, auto &stop_token) {
      using enum SettingsManagerInterface::ApplyResult;

      // We only want to keep retrying in case of a transient errors.
      // In other cases, when we either fail or succeed we just want to stop...
      const auto result {settings_iface.applySettings(config)};
      if (result == Ok) {
        BOOST_LOG(info) << "Display device configuration applied successfully.";
      } else if (result == ApiTemporarilyUnavailable) {
        BOOST_LOG(warning) << "Display device configuration API is temporarily unavailable. Will retry.";
      } else {
        BOOST_LOG(error) << "Display device configuration failed with result: " << apply_result_name(result);
      }

      if (result != ApiTemporarilyUnavailable) {
        stop_token.requestStop();
      }
    },
                                  {.m_sleep_durations = {DEFAULT_RETRY_INTERVAL}});
  }

  void revert_configuration() {
    std::lock_guard lock {DD_DATA.mutex};
#ifdef _WIN32
    if (DD_DATA.startup_exclusion || !DD_DATA.owned_reservation.expired()) {
      return;
    }
#endif
    revert_configuration_unlocked(revert_option_e::try_indefinitely_with_delay);
  }

  bool reset_persistence() {
    std::lock_guard lock {DD_DATA.mutex};
#ifdef _WIN32
    if (DD_DATA.startup_exclusion || !DD_DATA.owned_reservation.expired()) {
      return false;
    }
#endif
    if (!DD_DATA.sm_instance) {
      // Platform is not supported, assume success.
      return true;
    }

    return DD_DATA.sm_instance->execute([](auto &settings_iface, auto &stop_token) {
      // Whatever the outcome is we want to stop interfering with the user,
      // so any schedulers need to be stopped.
      stop_token.requestStop();
      return settings_iface.resetPersistence();
    });
  }

  EnumeratedDeviceList enumerate_devices() {
    std::lock_guard lock {DD_DATA.mutex};
    if (!DD_DATA.sm_instance) {
      // Platform is not supported.
      return {};
    }

    return DD_DATA.sm_instance->execute([](auto &settings_iface) {
      return settings_iface.enumAvailableDevices();
    });
  }

  std::variant<failed_to_parse_tag_t, configuration_disabled_tag_t, SingleDisplayConfiguration> parse_configuration(const config::video_t &video_config, const rtsp_stream::launch_session_t &session) {
    const auto device_prep {parse_device_prep_option(video_config)};
    if (!device_prep) {
      return configuration_disabled_tag_t {};
    }

    SingleDisplayConfiguration config;
    config.m_device_id = video_config.output_name;
    config.m_device_prep = *device_prep;

    const auto hdr_state {parse_hdr_option(video_config, session)};
#ifdef __APPLE__
    if (hdr_state) {
      BOOST_LOG(info) << "Ignoring HDR display device request on macOS because macOS HDR changes are not supported by libdisplaydevice.";
    }
#else
    config.m_hdr_state = hdr_state;
#endif

#ifdef __APPLE__
    if (config.m_device_prep != SingleDisplayConfiguration::DevicePreparation::VerifyOnly) {
      BOOST_LOG(warning) << "macOS libdisplaydevice currently supports only VerifyOnly display preparation. The requested preparation mode will fail.";
    }
#endif

    if (!parse_resolution_option(video_config, session, config)) {
      // Error already logged
      return failed_to_parse_tag_t {};
    }

    if (!parse_refresh_rate_option(video_config, session, config)) {
      // Error already logged
      return failed_to_parse_tag_t {};
    }

    if (!remap_display_mode_if_needed(video_config, session, config)) {
      // Error already logged
      return failed_to_parse_tag_t {};
    }

    return config;
  }

#ifdef _WIN32
  ordinary_startup_recovery_t ordinary_startup_recovery() {
    return {startup_excluded.load(), startup_legacy_pending.load()};
  }

  std::shared_ptr<ordinary_startup_exclusion_t> ordinary_startup_exclusion_t::acquire() {
    std::lock_guard lock {DD_DATA.mutex};
    if (DD_DATA.startup_exclusion) {
      return nullptr;
    }
    auto exclusion = std::shared_ptr<ordinary_startup_exclusion_t> {new ordinary_startup_exclusion_t};
    DD_DATA.startup_exclusion = exclusion;
    startup_excluded = true;
    if (DD_DATA.sm_instance) {
      // Fence callbacks already scheduled before the gate. No further ordinary work can enter.
      DD_DATA.sm_instance->stop();
    }
    return exclusion;
  }

  bool ordinary_startup_exclusion_t::release_to_ordinary(bool legacy_pending, const std::filesystem::path &ordinary_path) {
    std::lock_guard lock {DD_DATA.mutex};
    if (DD_DATA.startup_exclusion.get() != this || !DD_DATA.owned_reservation.expired() || DD_DATA.sm_instance) {
      return false;
    }
    // Publish the copied obligation before exposing any ordinary/native admission path.
    DD_DATA.startup_ordinary_path = ordinary_path;
    startup_legacy_tracked = true;
    startup_legacy_pending = legacy_pending;
    DD_DATA.startup_exclusion.reset();
    startup_excluded = false;
    return true;
  }

  struct native_display_configuration_t::state_t {
    struct recovery_t {
      explicit recovery_t(bool pending):
          pending {pending} {}

      std::atomic_bool pending;  ///< Ordinary persistence or scheduled work still requires restoration.
      std::atomic_bool retry_required {false};  ///< Callback stopped on an exception and requires serialized rescheduling.
    };

    mutable std::mutex mutex;  ///< Protect copied progress only; never held during native work.
    native_display_status_t status;  ///< Latest logical generation and tolerated apply observation.
    std::shared_ptr<recovery_t> recovery = std::make_shared<recovery_t>(false);  ///< One scheduler episode; replacement fences old callback publication.
  };

  native_display_configuration_t::native_display_configuration_t():
      state_ {std::make_shared<state_t>()} {}

  native_display_status_t native_display_configuration_t::status() const {
    std::lock_guard lock {state_->mutex};
    auto result = state_->status;
    result.recovery_retry_required = state_->recovery->retry_required.load();
    result.recovery_pending = state_->recovery->pending.load() || result.recovery_retry_required;
    return result;
  }

  bool native_display_configuration_t::begin(const std::optional<SingleDisplayConfiguration> &configuration, bool disabled, std::uint64_t generation, std::chrono::milliseconds delay) {
    if (!generation) {
      return false;
    }
    {
      std::lock_guard lock {state_->mutex};
      if (generation <= state_->status.generation) {
        return false;
      }
      state_->status = {.generation = generation};
    }
    if (disabled) {
      return restore(generation, delay);
    }
    std::lock_guard command {DD_DATA.mutex};
    if (DD_DATA.startup_exclusion || !DD_DATA.owned_reservation.expired()) {
      return false;
    }
    if (!configuration || !DD_DATA.sm_instance) {
      return true;
    }
    DD_DATA.sm_instance->schedule([state = state_, generation, configuration = *configuration](auto &manager, auto &stop) {
      const auto result = manager.applySettings(configuration);
      if (result == SettingsManagerInterface::ApplyResult::Ok) {
        BOOST_LOG(info) << "Display device configuration applied successfully.";
      } else if (result == SettingsManagerInterface::ApplyResult::ApiTemporarilyUnavailable) {
        BOOST_LOG(warning) << "Display device configuration API is temporarily unavailable. Will retry.";
      } else {
        BOOST_LOG(error) << "Display device configuration failed with result: " << apply_result_name(result);
      }
      {
        std::lock_guard lock {state->mutex};
        if (state->status.generation == generation) {
          state->status.apply_result = result;
        }
      }
      if (result != SettingsManagerInterface::ApplyResult::ApiTemporarilyUnavailable) {
        stop.requestStop();
      }
    },
                                  {.m_sleep_durations = {DEFAULT_RETRY_INTERVAL}});
    return true;
  }

  bool native_display_configuration_t::restore(std::uint64_t generation, std::chrono::milliseconds delay) {
    if (status().generation != generation || !generation || delay < std::chrono::milliseconds::zero()) {
      return false;
    }
    std::lock_guard command {DD_DATA.mutex};
    if (DD_DATA.startup_exclusion || !DD_DATA.owned_reservation.expired()) {
      return false;
    }
    if (!DD_DATA.sm_instance) {
      std::lock_guard lock {state_->mutex};
      state_->status.source_released = true;
      state_->recovery = std::make_shared<state_t::recovery_t>(false);
      return true;
    }
    const auto recovery = std::make_shared<state_t::recovery_t>(true);
    {
      std::lock_guard lock {state_->mutex};
      state_->recovery = recovery;
    }
  #ifdef SUNSHINE_TESTS
    const auto interval = DD_DATA.test_retry_interval.value_or(DEFAULT_RETRY_INTERVAL);
  #else
    const auto interval = DEFAULT_RETRY_INTERVAL;
  #endif
    SchedulerOptions options {.m_sleep_durations = {interval}};
    if (delay > std::chrono::milliseconds::zero()) {
      options.m_sleep_durations = {delay, interval};
      options.m_execution = SchedulerOptions::Execution::ScheduledOnly;
    }
    const auto persistence = DD_DATA.persistence_watch;
    DD_DATA.sm_instance->schedule([recovery, persistence, tried = StringSet {}](auto &manager, auto &stop) mutable {
      try {
        try_revert_settings(false, tried, persistence, manager, stop);
        recovery->pending = !stop.stopRequested() || (persistence && persistence->has_pending_restores());
      } catch (const std::exception &failure) {
        recovery->pending = true;
        recovery->retry_required = true;
        stop.requestStop();
        BOOST_LOG(error) << "Native display restoration stopped: " << failure.what();
      }
    },
                                  options);
    std::lock_guard lock {state_->mutex};
    state_->status.source_released = true;
    return true;
  }

  bool native_display_configuration_t::complete(std::uint64_t generation) const {
    const auto current = status();
    return generation && current.generation == generation && current.source_released && !current.recovery_pending;
  }

  namespace {
    StringSet recovery_references(const SingleDisplayConfigState &state) {
      StringSet result {state.m_initial.m_primary_devices};
      for (const auto *topology : {&state.m_initial.m_topology, &state.m_modified.m_topology}) {
        for (const auto &group : *topology) {
          result.insert(group.begin(), group.end());
        }
      }
      for (const auto &[id, mode] : state.m_modified.m_original_modes) {
        result.insert(id);
      }
      for (const auto &[id, hdr] : state.m_modified.m_original_hdr_states) {
        result.insert(id);
      }
      for (const auto *primary : {&state.m_modified.m_original_primary_device, &state.m_modified.m_interim_primary_device}) {
        if (!primary->empty()) {
          result.insert(*primary);
        }
      }
      return result;
    }

    /** @brief Keep library persistence inside the same atomic record as ownership intent. */
    class owned_settings_persistence_t final: public SettingsPersistenceInterface {
    public:
      explicit owned_settings_persistence_t(std::shared_ptr<platf::sudovda::ownership_journal_t> journal):
          journal_ {std::move(journal)} {}

      bool store(const std::vector<std::uint8_t> &data) override {
        SingleDisplayConfigState parsed;
        const auto known {journal_->recovery_devices()};
        if (!known || !fromJson(std::string {data.begin(), data.end()}, parsed)) {
          return false;
        }
        const StringSet allowed {known->begin(), known->end()};
        const auto references {recovery_references(parsed)};
        if (!std::ranges::all_of(references, [&](const auto &id) {
              return allowed.contains(id);
            })) {
          return false;
        }
        return journal_->store_settings(data);
      }

      std::optional<std::vector<std::uint8_t>> load() const override {
        return journal_->settings();
      }

      bool clear() override {
        return journal_->store_settings({});
      }

    private:
      std::shared_ptr<platf::sudovda::ownership_journal_t> journal_;  ///< Journal owning all recovery bytes.
    };

    StringSet topology_devices(const ActiveTopology &topology) {
      StringSet result;
      for (const auto &group : topology) {
        result.insert(group.begin(), group.end());
      }
      return result;
    }

    bool matching_target(const DISPLAYCONFIG_PATH_INFO &path, const platf::sudovda::display_t &display) {
      return path.targetInfo.adapterId.HighPart == display.target.adapter.HighPart &&
             path.targetInfo.adapterId.LowPart == display.target.adapter.LowPart &&
             path.targetInfo.id == display.target.target;
    }
  }  // namespace

  struct owned_display_configuration_t::state_t {
    std::shared_ptr<int> reservation;
    std::shared_ptr<platf::sudovda::ownership_journal_t> journal;
    std::shared_ptr<WinApiLayerInterface> api;
    std::shared_ptr<WinDisplayDeviceInterface> devices;
    std::shared_ptr<AudioContextInterface> audio;
    PersistentState *persistent_state {};
    std::unique_ptr<RetryScheduler<SettingsManagerInterface>> scheduler;
    SingleDisplayConfigState baseline;
    std::optional<platf::sudovda::display_t> bound_display;
    bool owned_absent {};
    platf::sudovda::recovery_authority_t authority;
    std::mutex command_mutex;
    mutable std::mutex status_mutex;
    owned_display_status_t status;
    std::atomic_uint64_t revision {};
    std::chrono::milliseconds retry_interval {DEFAULT_RETRY_INTERVAL};

    bool valid_generation(std::uint64_t generation) const {
      return reservation && generation == journal->record().generation;
    }

    std::vector<std::string> observed_devices() const {
      std::vector<std::string> result;
      for (const auto &device : devices->enumAvailableDevices()) {
        result.push_back(device.m_device_id);
      }
      return result;
    }

    bool valid_library_state(const SingleDisplayConfigState &state) const {
      const auto known {journal->recovery_devices()};
      if (!known || !devices->isTopologyValid(state.m_initial.m_topology) || (!state.m_modified.m_topology.empty() && !devices->isTopologyValid(state.m_modified.m_topology)) || (state.m_modified.hasModifications() && state.m_modified.m_topology.empty())) {
        return false;
      }
      const StringSet allowed {known->begin(), known->end()};
      const auto references {recovery_references(state)};
      return std::ranges::all_of(references, [&](const auto &id) {
        return allowed.contains(id);
      });
    }

    bool validated_settings(std::optional<SingleDisplayConfigState> &state) const {
      if (!persistent_state || !journal->validate(authority)) {
        return false;
      }
      const auto bytes {journal->settings()};
      if (!bytes) {
        return false;
      }
      const auto &cached {persistent_state->getState()};
      if (bytes->empty()) {
        state.reset();
        return !cached;
      }
      SingleDisplayConfigState parsed;
      if (!fromJson(std::string {bytes->begin(), bytes->end()}, parsed) || !cached || *cached != parsed || !valid_library_state(parsed)) {
        return false;
      }
      state = std::move(parsed);
      return true;
    }

    std::optional<platf::sudovda::ownership_record_t> snapshot(const GUID &identity, std::uint64_t generation) {
      const auto raw {api->queryDisplayConfig(QueryType::Active)};
      if (!raw || !devices->isApiAccessAvailable()) {
        return std::nullopt;
      }
      platf::sudovda::ownership_record_t record {identity, generation, authority.deployment, raw->m_paths.empty(), {}};
      SingleDisplayConfigState captured;
      if (!record.initially_headless) {
        captured.m_initial.m_topology = devices->getCurrentTopology();
        if (!devices->isTopologyValid(captured.m_initial.m_topology)) {
          return std::nullopt;
        }
        for (const auto &id : topology_devices(captured.m_initial.m_topology)) {
          if (devices->isPrimary(id)) {
            captured.m_initial.m_primary_devices.insert(id);
          }
        }
        if (captured.m_initial.m_primary_devices.empty()) {
          return std::nullopt;
        }
        captured.m_modified.m_topology = captured.m_initial.m_topology;
        captured.m_modified.m_original_primary_device = *captured.m_initial.m_primary_devices.begin();
        bool serialized {false};
        record.baseline = toJson(captured, JSON_COMPACT, &serialized);
        if (!serialized) {
          return std::nullopt;
        }
      }
      return record;
    }

    bool reconstruct_manager() {
      scheduler.reset();
      persistent_state = nullptr;
      try {
        auto persistence {std::make_shared<owned_settings_persistence_t>(journal)};
        auto persistent {std::make_unique<PersistentState>(persistence, true)};
        persistent_state = persistent.get();
        auto manager {std::make_unique<SettingsManager>(devices, audio, std::move(persistent), WinWorkarounds {})};
        audio = manager->getAudioContextApi();
        scheduler = std::make_unique<RetryScheduler<SettingsManagerInterface>>(std::move(manager));
        return true;
      } catch (const std::exception &) {
        persistent_state = nullptr;
        return false;
      }
    }

    std::string resolve(const platf::sudovda::display_t &display, std::string *active_name = nullptr) const {
      if (active_name) {
        active_name->clear();
      }
      if (!IsEqualGUID(display.identity, journal->record().identity) || display.monitor_path.empty()) {
        return {};
      }
      const auto paths {api->queryDisplayConfig(QueryType::All)};
      if (!paths) {
        return {};
      }
      const auto expected_path {utf_utils::to_utf8(display.monitor_path)};
      std::string found;
      std::optional<std::tuple<LONG, ULONG, UINT>> active_source;
      std::optional<std::pair<LONG, ULONG>> source_adapter;
      std::set<UINT> sources;
      std::string selector;
      for (const auto &path : paths->m_paths) {
        const auto monitor {api->getMonitorDevicePath(path)};
        const bool same_path {boost::iequals(monitor, expected_path)};
        if (!matching_target(path, display)) {
          if (same_path) {
            return {};
          }
          continue;
        }
        if (!same_path) {
          return {};
        }
        const auto adapter {std::pair {path.sourceInfo.adapterId.HighPart, path.sourceInfo.adapterId.LowPart}};
        if ((source_adapter && *source_adapter != adapter) || !sources.insert(path.sourceInfo.id).second) {
          return {};
        }
        source_adapter = adapter;
        const auto id {api->getDeviceId(path)};
        if (id.empty() || (!found.empty() && found != id)) {
          return {};
        }
        found = id;
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0) {
          const auto source {std::tuple {path.sourceInfo.adapterId.HighPart, path.sourceInfo.adapterId.LowPart, path.sourceInfo.id}};
          const auto name {api->getDisplayName(path)};
          if (name.empty() || (active_source && (*active_source != source || selector != name))) {
            return {};
          }
          active_source = source;
          selector = name;
        }
      }
      if (active_name) {
        *active_name = selector;
      }
      return found;
    }

    bool removal_ready() {
      const auto raw {api->queryDisplayConfig(QueryType::Active)};
      if (!raw) {
        return false;
      }
      const auto current {devices->getCurrentTopology()};
      if (!raw->m_paths.empty() && !devices->isTopologyValid(current)) {
        return false;
      }
      const auto active {topology_devices(current)};
      const auto id {bound_display ? resolve(*bound_display) : std::string {}};
      std::string owned_id;
      {
        std::lock_guard lock {status_mutex};
        owned_id = status.device_id;
      }
      if (!id.empty() && !owned_id.empty() && id != owned_id) {
        return false;
      }
      if (owned_id.empty()) {
        // Recovery may follow a crash before bind. Resolve from the adopted lease before removal,
        // or prove the journal GUID is absent through lease cleanup; never guess an active selector.
        if (!owned_absent) {
          return false;
        }
      }
      const auto evidence {journal->restoration_state()};
      if (!evidence) {
        return false;
      }
      SingleDisplayConfigState restored;
      if (!evidence->empty() && (!fromJson(std::string {evidence->begin(), evidence->end()}, restored) || !valid_library_state(restored))) {
        return false;
      }
      auto initial {topology_devices(restored.m_initial.m_topology)};
      initial.erase(owned_id);
      if (active.contains(owned_id)) {
        // Only a proven first-headless owned-only destination permits last-output removal.
        return journal->record().initially_headless && initial.empty() && active.size() == 1;
      }
      if (initial.empty()) {
        return !active.empty() || (journal->record().initially_headless && owned_absent);
      }
      const auto available {devices->enumAvailableDevices()};
      if (available.empty()) {
        return false;
      }
      for (const auto &device : available) {
        if (initial.contains(device.m_device_id) && !active.contains(device.m_device_id)) {
          return false;
        }
        if (device.m_device_id != owned_id && restored.m_initial.m_primary_devices.contains(device.m_device_id) && (!device.m_info || !device.m_info->m_primary)) {
          return false;
        }
      }
      return !active.empty();
    }

    bool headless_owned_only() const {
      std::optional<SingleDisplayConfigState> parsed;
      if (!validated_settings(parsed)) {
        return false;
      }
      if (!parsed) {
        return true;
      }
      const auto &stored {*parsed};
      std::string owned;
      {
        std::lock_guard lock {status_mutex};
        owned = status.device_id;
      }
      if (owned.empty()) {
        return false;
      }
      const auto only_owned {[&](const auto &ids) {
        return std::ranges::all_of(ids, [&](const auto &id) {
          return id == owned;
        });
      }};
      const auto optional_owned {[&](const std::string &id) {
        return id.empty() || id == owned;
      }};
      StringSet mode_ids;
      for (const auto &[id, mode] : stored.m_modified.m_original_modes) {
        mode_ids.insert(id);
      }
      for (const auto &[id, hdr] : stored.m_modified.m_original_hdr_states) {
        mode_ids.insert(id);
      }
      return only_owned(topology_devices(stored.m_initial.m_topology)) && only_owned(stored.m_initial.m_primary_devices) &&
             only_owned(topology_devices(stored.m_modified.m_topology)) && only_owned(mode_ids) &&
             optional_owned(stored.m_modified.m_original_primary_device) && optional_owned(stored.m_modified.m_interim_primary_device);
    }

    bool detach_headless_owned() {
      const auto raw {api->queryDisplayConfig(QueryType::Active)};
      if (!raw) {
        return false;
      }
      const auto current {devices->getCurrentTopology()};
      if (!raw->m_paths.empty() && !devices->isTopologyValid(current)) {
        return false;
      }
      std::string owned;
      {
        std::lock_guard lock {status_mutex};
        owned = status.device_id;
      }
      if ((!bound_display && !owned_absent) || (owned.empty() && !owned_absent) || (bound_display && resolve(*bound_display) != owned)) {
        return false;
      }
      ActiveTopology remaining {current};
      for (auto &group : remaining) {
        std::erase(group, owned);
      }
      std::erase_if(remaining, [](const auto &group) {
        return group.empty();
      });
      const auto unowned {topology_devices(remaining)};
      const auto modes {devices->getCurrentDisplayModes(unowned)};
      const auto hdr {devices->getCurrentHdrStates(unowned)};
      if (modes.size() != unowned.size() || hdr.size() != unowned.size()) {
        return false;
      }
      StringSet primary;
      for (const auto &id : unowned) {
        if (devices->isPrimary(id)) {
          primary.insert(id);
        }
      }
      if (!remaining.empty() && !devices->isTopologyTheSame(current, remaining) && (!devices->setTopology(remaining) || !devices->isTopologyTheSame(devices->getCurrentTopology(), remaining))) {
        return false;
      }
      // Never submit an empty topology. A sole owned output is removed only by exact lease cleanup.
      return modes == devices->getCurrentDisplayModes(unowned) && hdr == devices->getCurrentHdrStates(unowned) &&
             std::ranges::all_of(primary, [&](const auto &id) {
               return devices->isPrimary(id);
             });
    }
  };

  owned_display_configuration_t::owned_display_configuration_t(std::unique_ptr<state_t> state):
      state_ {std::move(state)} {}

  owned_display_configuration_t::~owned_display_configuration_t() {
    // No global or status lock is held while joining callbacks. They never synchronously call an owner.
    ++state_->revision;
    state_->scheduler.reset();
  }

  std::expected<std::unique_ptr<owned_display_configuration_t>, platf::sudovda::failure_t> owned_display_configuration_t::make(const std::filesystem::path &journal_path, const GUID &identity, std::uint64_t generation, const platf::sudovda::recovery_authority_t &authority, bool recovery, std::shared_ptr<WinApiLayerInterface> api, std::shared_ptr<WinDisplayDeviceInterface> devices, std::unique_ptr<SettingsManagerInterface> manager, std::shared_ptr<AudioContextInterface> audio) {
    using namespace platf::sudovda;
    const auto fail {[](errc_t code) {
      return std::unexpected(failure_t {code, operation_t::journal});
    }};
    auto state {std::make_unique<state_t>()};
    state->api = std::move(api);
    state->devices = std::move(devices);
    state->audio = std::move(audio);
    state->authority = authority;
    {
      std::lock_guard lock {DD_DATA.mutex};
      if (!DD_DATA.owned_reservation.expired()) {
        return fail(errc_t::busy);
      }
      if (DD_DATA.sm_instance) {
        DD_DATA.sm_instance->stop();
      }
      if (DD_DATA.persistence_watch && DD_DATA.persistence_watch->has_pending_restores()) {
        if (!DD_DATA.startup_exclusion) {
          revert_configuration_unlocked(revert_option_e::try_indefinitely);
        }
        return fail(errc_t::busy);
      }
      state->reservation = std::make_shared<int>(0);
      DD_DATA.owned_reservation = state->reservation;
    }
    if (!state->api || !state->devices) {
      return fail(errc_t::enumeration_failed);
    }
    if (recovery) {
      auto journal {ownership_journal_t::load(journal_path, authority)};
      if (!journal) {
        return std::unexpected(journal.error());
      }
      state->journal = std::move(*journal);
      if (!state->journal->record().initially_headless && (!fromJson(state->journal->record().baseline, state->baseline) || !state->devices->isTopologyValid(state->baseline.m_initial.m_topology))) {
        return fail(errc_t::ownership_unverified);
      }
    } else {
      const auto record {state->snapshot(identity, generation)};
      if (!record) {
        return fail(errc_t::enumeration_failed);
      }
      if (!record->initially_headless && !fromJson(record->baseline, state->baseline)) {
        return fail(errc_t::journal_failed);
      }
      const auto observed {state->observed_devices()};
      const StringSet observed_ids {observed.begin(), observed.end()};
      const auto initial_refs {recovery_references(state->baseline)};
      if (!std::ranges::all_of(initial_refs, [&](const auto &id) {
            return observed_ids.contains(id);
          })) {
        return fail(errc_t::enumeration_failed);
      }
      auto journal {ownership_journal_t::create(journal_path, *record, authority, observed)};
      if (!journal) {
        return std::unexpected(journal.error());
      }
      state->journal = std::move(*journal);
    }
    if (!manager) {
      auto persistence {std::make_shared<owned_settings_persistence_t>(state->journal)};
      // A malformed library record is an error, never permission to forget restoration state.
      try {
        auto persistent {std::make_unique<PersistentState>(persistence, true)};
        state->persistent_state = persistent.get();
        auto concrete {std::make_unique<SettingsManager>(state->devices, state->audio, std::move(persistent), WinWorkarounds {})};
        state->audio = concrete->getAudioContextApi();
        manager = std::move(concrete);
      } catch (const std::exception &) {
        return fail(errc_t::journal_failed);
      }
    }
    state->status.generation = state->journal->record().generation;
    if (const auto mapping {state->journal->mapping()}) {
      state->status.device_id = mapping->device_id;
    }
    state->status.initially_headless = state->journal->record().initially_headless;
    state->status.source_released = state->journal->source_retired();
    if (state->status.source_released) {
      state->status.phase = owned_display_phase_e::recovering;
    }
    state->scheduler = std::make_unique<RetryScheduler<SettingsManagerInterface>>(std::move(manager));
    return std::unique_ptr<owned_display_configuration_t> {new owned_display_configuration_t(std::move(state))};
  }

  std::expected<std::unique_ptr<owned_display_configuration_t>, platf::sudovda::failure_t> owned_display_configuration_t::begin(const std::filesystem::path &journal_path, const GUID &identity, std::uint64_t generation, const platf::sudovda::recovery_authority_t &authority) {
    auto api {std::make_shared<WinApiLayer>()};
    return make(journal_path, identity, generation, authority, false, api, std::make_shared<WinDisplayDevice>(api, false), nullptr, std::make_shared<sunshine_audio_context_t>());
  }

  std::expected<std::unique_ptr<owned_display_configuration_t>, platf::sudovda::failure_t> owned_display_configuration_t::recover(const std::filesystem::path &journal_path, const platf::sudovda::recovery_authority_t &authority) {
    auto api {std::make_shared<WinApiLayer>()};
    return make(journal_path, {}, 0, authority, true, api, std::make_shared<WinDisplayDevice>(api, false), nullptr, std::make_shared<sunshine_audio_context_t>());
  }

  std::shared_ptr<platf::sudovda::ownership_journal_t> owned_display_configuration_t::journal() const {
    std::lock_guard command {state_->command_mutex};
    return state_->journal;
  }

  bool owned_display_configuration_t::bind(const platf::sudovda::display_t &display, std::uint64_t generation) {
    std::lock_guard command {state_->command_mutex};
    if (!state_->valid_generation(generation)) {
      return false;
    }
    {
      std::lock_guard lock {state_->status_mutex};
      if (state_->status.phase != owned_display_phase_e::reserved) {
        return false;
      }
    }
    const auto id {state_->resolve(display)};
    if (id.empty()) {
      return false;
    }
    {
      std::lock_guard lock {state_->status_mutex};
      if (!state_->status.device_id.empty() && state_->status.device_id != id) {
        return false;
      }
    }
    if (!state_->journal->bind_mapping(display, id)) {
      return false;
    }
    std::lock_guard lock {state_->status_mutex};
    if (!state_->status.device_id.empty() && state_->status.device_id != id) {
      return false;
    }
    state_->bound_display = display;
    state_->status.device_id = id;
    return true;
  }

  bool owned_display_configuration_t::bind_absent(const platf::sudovda::lease_t &lease, std::uint64_t generation) {
    std::lock_guard command {state_->command_mutex};
    if (!state_->valid_generation(generation) || !lease.belongs_to(state_->journal) || !lease.identity_absent() || !IsEqualGUID(lease.identity(), state_->journal->record().identity)) {
      return false;
    }
    std::lock_guard lock {state_->status_mutex};
    if (state_->status.phase != owned_display_phase_e::reserved || state_->bound_display) {
      return false;
    }
    state_->owned_absent = true;
    return true;
  }

  bool owned_display_configuration_t::prepare(const SingleDisplayConfiguration &configuration, std::uint64_t generation, unsigned int max_attempts) {
    std::lock_guard command {state_->command_mutex};
    if (!state_->valid_generation(generation) || !state_->scheduler || state_->journal->source_retired() || !state_->journal->add_was_armed() || !max_attempts || max_attempts > 100 || !state_->bound_display || !state_->journal->validate(state_->authority)) {
      return false;
    }
    SingleDisplayConfiguration selected {configuration};
    {
      std::lock_guard lock {state_->status_mutex};
      if (state_->status.phase != owned_display_phase_e::reserved || (!selected.m_device_id.empty() && selected.m_device_id != state_->status.device_id)) {
        return false;
      }
      selected.m_device_id = state_->status.device_id;
      state_->status.phase = owned_display_phase_e::preparing;
      state_->status.attempts = 0;
    }
    const auto revision {++state_->revision};
    state_->scheduler->schedule([state = state_.get(), revision, selected, max_attempts](auto &manager, auto &stop) {
      try {
        if (state->revision != revision) {
          stop.requestStop();
          return;
        }
        if (!state->journal->validate(state->authority)) {
          std::lock_guard lock {state->status_mutex};
          state->status.phase = owned_display_phase_e::failed;
          state->status.apply_error = owned_display_error_e::journal_unavailable;
          stop.requestStop();
          return;
        }
        std::optional<SingleDisplayConfigState> previous;
        if (!state->validated_settings(previous) || !state->journal->observe_devices(state->observed_devices())) {
          std::lock_guard lock {state->status_mutex};
          state->status.phase = owned_display_phase_e::failed;
          state->status.apply_error = owned_display_error_e::recovery_state_unproven;
          stop.requestStop();
          return;
        }
        const auto result {manager.applySettings(selected)};
        const auto name {result == SettingsManagerInterface::ApplyResult::Ok ? manager.getDisplayName(selected.m_device_id) : std::string {}};
        std::optional<EnumeratedDevice::Info> observed;
        std::string active_name;
        if (!name.empty() && state->bound_display && state->resolve(*state->bound_display, &active_name) == selected.m_device_id && active_name == name) {
          for (const auto &device : manager.enumAvailableDevices()) {
            if (device.m_device_id == selected.m_device_id && device.m_display_name == name) {
              observed = device.m_info;
            }
          }
        }
        const bool verified {observed && (!selected.m_resolution || observed->m_resolution == *selected.m_resolution) && (!selected.m_refresh_rate || detail::fuzzyCompare(observed->m_refresh_rate, *selected.m_refresh_rate)) && (!selected.m_hdr_state || observed->m_hdr_state == selected.m_hdr_state)};
        if (state->revision != revision) {
          stop.requestStop();
          return;
        }
        std::lock_guard lock {state->status_mutex};
        state->status.apply_result = result;
        ++state->status.attempts;
        if (result == SettingsManagerInterface::ApplyResult::Ok && verified) {
          state->status.phase = owned_display_phase_e::ready;
          state->status.display_name = name;
          state->status.observed = observed;
          stop.requestStop();
        } else if (result != SettingsManagerInterface::ApplyResult::ApiTemporarilyUnavailable || state->status.attempts >= max_attempts) {
          state->status.phase = owned_display_phase_e::failed;
          if (result == SettingsManagerInterface::ApplyResult::Ok) {
            state->status.apply_error = owned_display_error_e::observed_source_mismatch;
          }
          stop.requestStop();
        }
      } catch (const std::exception &) {
        if (state->revision == revision) {
          std::lock_guard lock {state->status_mutex};
          state->status.phase = owned_display_phase_e::failed;
          state->status.apply_error = owned_display_error_e::operation_exception;
        }
        stop.requestStop();
      }
    },
                                {.m_sleep_durations = {state_->retry_interval}});
    return true;
  }

  bool owned_display_configuration_t::bind_absent(const platf::sudovda::cleanup_receipt_t &receipt, std::uint64_t generation) {
    std::lock_guard command {state_->command_mutex};
    if (!state_->valid_generation(generation) || receipt.kind() != platf::sudovda::cleanup_receipt_t::kind_e::removed || !state_->journal->accepts_receipt(receipt)) {
      return false;
    }
    std::lock_guard lock {state_->status_mutex};
    if (state_->status.phase != owned_display_phase_e::reserved || state_->bound_display) {
      return false;
    }
    state_->owned_absent = true;
    return true;
  }

  bool owned_display_configuration_t::restore(std::uint64_t generation, std::chrono::milliseconds delay) {
    std::lock_guard command {state_->command_mutex};
    if (!state_->valid_generation(generation) || !state_->scheduler || delay < std::chrono::milliseconds::zero()) {
      return false;
    }
    const auto revision {++state_->revision};
    {
      std::lock_guard lock {state_->status_mutex};
      state_->status.phase = owned_display_phase_e::reverting;
      state_->status.attempts = 0;
      state_->status.display_name.clear();
      state_->status.restore_retry_required = false;
    }
    SchedulerOptions options {.m_sleep_durations = {state_->retry_interval}};
    if (delay > std::chrono::milliseconds::zero()) {
      options.m_sleep_durations = {delay, state_->retry_interval};
      options.m_execution = SchedulerOptions::Execution::ScheduledOnly;
    }
    state_->scheduler->schedule([state = state_.get(), revision](auto &manager, auto &stop) {
      try {
        if (state->revision != revision) {
          stop.requestStop();
          return;
        }
        if (!state->journal->validate(state->authority)) {
          std::lock_guard lock {state->status_mutex};
          state->status.phase = owned_display_phase_e::cleanup_pending;
          state->status.removal_permitted = false;
          state->status.restore_error = owned_display_error_e::journal_unavailable;
          state->status.restore_retry_required = true;
          stop.requestStop();
          return;
        }
        const bool headless {state->journal->record().initially_headless && !state->journal->source_retired()};
        std::optional<SingleDisplayConfigState> previous;
        std::string owned_id;
        {
          std::lock_guard lock {state->status_mutex};
          owned_id = state->status.device_id;
        }
        if ((!state->journal->source_retired() && !state->bound_display && !state->owned_absent) || !state->validated_settings(previous) || (headless && previous && owned_id.empty())) {
          std::lock_guard lock {state->status_mutex};
          state->status.phase = owned_display_phase_e::cleanup_pending;
          state->status.removal_permitted = false;
          state->status.restore_error = headless ? owned_display_error_e::headless_ownership_unproven : owned_display_error_e::recovery_state_unproven;
          state->status.restore_retry_required = true;
          stop.requestStop();
          return;
        }
        const bool owned_only {headless && state->headless_owned_only()};
        if (!owned_only && !state->journal->observe_devices(state->observed_devices())) {
          std::lock_guard lock {state->status_mutex};
          state->status.phase = owned_display_phase_e::cleanup_pending;
          state->status.removal_permitted = false;
          state->status.restore_error = owned_display_error_e::journal_unavailable;
          state->status.restore_retry_required = true;
          stop.requestStop();
          return;
        }
        const bool detached {owned_only && state->detach_headless_owned()};
        const bool discarded {detached && manager.resetPersistence()};
        if (discarded && state->audio && state->audio->isCaptured()) {
          // The library returns early for an already-empty cache. Release its retained audio too.
          state->audio->release();
        }
        const std::optional<SettingsManagerInterface::RevertResult> result {owned_only ? std::nullopt : std::make_optional(manager.revertSettings())};
        const bool ordinary_detached {!headless || owned_only || result != SettingsManagerInterface::RevertResult::Ok || state->detach_headless_owned()};
        const auto settings {state->journal->settings()};
        const bool retired {state->journal->source_retired()};
        const bool ready {(discarded || result == SettingsManagerInterface::RevertResult::Ok) && ordinary_detached && settings && (retired || state->removal_ready())};
        if (state->revision != revision) {
          stop.requestStop();
          return;
        }
        std::lock_guard lock {state->status_mutex};
        state->status.revert_result = result;
        state->status.restore_error = (owned_only && !detached) || !ordinary_detached ? std::make_optional(owned_display_error_e::headless_unowned_outputs) :
                                      (!settings || (owned_only && !discarded))       ? std::make_optional(owned_display_error_e::journal_unavailable) :
                                                                                        std::nullopt;
        ++state->status.attempts;
        state->status.unavailable_settings_pending = settings && !settings->empty();
        state->status.removal_permitted = !retired && ready;
        state->status.restore_retry_required = false;
        state->status.phase = ready ? (retired ? (settings->empty() ? owned_display_phase_e::restored : owned_display_phase_e::recovering) : owned_display_phase_e::removal_ready) : owned_display_phase_e::cleanup_pending;
        if (ready && settings->empty()) {
          stop.requestStop();
        }
      } catch (const std::exception &) {
        if (state->revision == revision) {
          std::lock_guard lock {state->status_mutex};
          state->status.phase = owned_display_phase_e::cleanup_pending;
          state->status.removal_permitted = false;
          state->status.restore_error = owned_display_error_e::operation_exception;
          state->status.restore_retry_required = true;
        }
        stop.requestStop();
      }
    },
                                options);
    return true;
  }

  bool owned_display_configuration_t::cancel(std::uint64_t generation) {
    std::lock_guard command {state_->command_mutex};
    if (!state_->valid_generation(generation)) {
      return false;
    }
    ++state_->revision;
    if (state_->scheduler) {
      state_->scheduler->stop();
    }
    std::lock_guard lock {state_->status_mutex};
    state_->status.phase = owned_display_phase_e::cancelled;
    state_->status.display_name.clear();
    return true;
  }

  owned_display_status_t owned_display_configuration_t::status() const {
    std::lock_guard lock {state_->status_mutex};
    return state_->status;
  }

  bool owned_display_configuration_t::retire_source(const platf::sudovda::cleanup_receipt_t &receipt, std::uint64_t generation) {
    std::unique_lock command {state_->command_mutex};
    if (!state_->valid_generation(generation) || !state_->scheduler) {
      return false;
    }
    {
      std::lock_guard lock {state_->status_mutex};
      if (!state_->status.removal_permitted && receipt.kind() != platf::sudovda::cleanup_receipt_t::kind_e::unarmed) {
        return false;
      }
    }
    ++state_->revision;
    state_->scheduler->stop();
    if (!state_->journal->retire(receipt)) {
      std::lock_guard lock {state_->status_mutex};
      state_->status.phase = owned_display_phase_e::cleanup_pending;
      return false;
    }
    {
      std::lock_guard lock {state_->status_mutex};
      state_->status.source_released = true;
      state_->status.phase = owned_display_phase_e::recovering;
    }
    command.unlock();
    // The owner serializes source transitions. A concurrent begin_next is still safe: generation
    // validation rejects this obsolete recovery request before it reaches the scheduler.
    std::ignore = restore(generation);
    return true;
  }

  bool owned_display_configuration_t::begin_next(const GUID &identity, std::uint64_t generation) {
    std::lock_guard command {state_->command_mutex};
    if (!state_->reservation || !state_->scheduler || !state_->journal->source_retired() || generation <= state_->journal->record().generation) {
      return false;
    }
    try {
      ++state_->revision;
      state_->scheduler->stop();
      if (!state_->journal->validate(state_->authority)) {
        return false;
      }
      std::optional<SingleDisplayConfigState> previous;
      if (!state_->validated_settings(previous) || !state_->journal->observe_devices(state_->observed_devices())) {
        return false;
      }
      const auto result {state_->scheduler->execute([](auto &manager) {
        return manager.revertSettings();
      })};
      if (result != SettingsManagerInterface::RevertResult::Ok) {
        std::lock_guard lock {state_->status_mutex};
        state_->status.revert_result = result;
        state_->status.phase = owned_display_phase_e::cleanup_pending;
        return false;
      }
      const auto record {state_->snapshot(identity, generation)};
      if (!record) {
        return false;
      }
      SingleDisplayConfigState next_baseline;
      if (!record->initially_headless && !fromJson(record->baseline, next_baseline)) {
        return false;
      }
      auto next {state_->journal->begin_next(*record, state_->authority, state_->observed_devices())};
      if (!next) {
        std::lock_guard lock {state_->status_mutex};
        state_->status.phase = owned_display_phase_e::cleanup_pending;
        return false;
      }
      state_->journal = std::move(*next);
      state_->baseline = std::move(next_baseline);
      state_->bound_display.reset();
      state_->owned_absent = false;
      {
        std::lock_guard lock {state_->status_mutex};
        state_->status = owned_display_status_t {.generation = generation, .initially_headless = record->initially_headless};
      }
      if (!state_->reconstruct_manager()) {
        std::lock_guard lock {state_->status_mutex};
        state_->status.phase = owned_display_phase_e::cleanup_pending;
        return false;
      }
      // No background revert is scheduled during preparation, active media or retained application life.
      return true;
    } catch (const std::exception &) {
      std::lock_guard lock {state_->status_mutex};
      state_->status.generation = state_->journal->record().generation;
      state_->status.phase = owned_display_phase_e::cleanup_pending;
      state_->status.restore_error = owned_display_error_e::operation_exception;
      return false;
    }
  }

  bool owned_display_configuration_t::complete(std::uint64_t generation) {
    std::lock_guard command {state_->command_mutex};
    if (!state_->valid_generation(generation)) {
      return false;
    }
    {
      std::lock_guard lock {state_->status_mutex};
      if (state_->status.phase != owned_display_phase_e::restored || !state_->status.source_released || state_->status.unavailable_settings_pending) {
        return false;
      }
    }
    state_->scheduler->stop();
    if (!state_->journal->complete()) {
      return false;
    }
    std::lock_guard lock {state_->status_mutex};
    state_->status.phase = owned_display_phase_e::restored;
    state_->reservation.reset();
    return true;
  }

  #ifdef SUNSHINE_TESTS
  std::expected<std::unique_ptr<owned_display_configuration_t>, platf::sudovda::failure_t> owned_display_configuration_t::test_begin(const std::filesystem::path &journal_path, const GUID &identity, std::uint64_t generation, const platf::sudovda::recovery_authority_t &authority, std::shared_ptr<WinApiLayerInterface> api, std::shared_ptr<WinDisplayDeviceInterface> devices, std::unique_ptr<SettingsManagerInterface> manager, std::shared_ptr<AudioContextInterface> audio) {
    return make(journal_path, identity, generation, authority, false, std::move(api), std::move(devices), std::move(manager), std::move(audio));
  }

  void owned_display_configuration_t::test_set_retry_interval(std::chrono::milliseconds interval) {
    std::lock_guard command {state_->command_mutex};
    if (interval > std::chrono::milliseconds::zero()) {
      state_->retry_interval = interval;
    }
  }

  owned_display_status_t owned_display_configuration_t::test_status_after_scheduled_work() const {
    std::lock_guard command {state_->command_mutex};
    if (!state_->scheduler) {
      return status();
    }
    return state_->scheduler->execute([this](auto &) {
      return status();
    });
  }

  std::expected<std::unique_ptr<owned_display_configuration_t>, platf::sudovda::failure_t> owned_display_configuration_t::test_recover(const std::filesystem::path &journal_path, const platf::sudovda::recovery_authority_t &authority, std::shared_ptr<WinApiLayerInterface> api, std::shared_ptr<WinDisplayDeviceInterface> devices) {
    return make(journal_path, {}, 0, authority, true, std::move(api), std::move(devices), nullptr, nullptr);
  }
  #endif
#endif

#ifdef SUNSHINE_TESTS
  namespace {
    /**
     * @brief In-memory persistence used by revert tests.
     */
    class memory_settings_persistence_t: public SettingsPersistenceInterface {
    public:
      /**
       * @copydoc SettingsPersistenceInterface::store
       */
      [[nodiscard]] bool store(const std::vector<std::uint8_t> &data) override {
        stored = data;
        return true;
      }

      /**
       * @copydoc SettingsPersistenceInterface::load
       */
      [[nodiscard]] std::optional<std::vector<std::uint8_t>> load() const override {
        return stored;
      }

      /**
       * @copydoc SettingsPersistenceInterface::clear
       */
      [[nodiscard]] bool clear() override {
        stored.clear();
        return true;
      }

    private:
      std::vector<std::uint8_t> stored;  ///< Last stored record, or empty after a clear.
    };
  }  // namespace

  std::shared_ptr<SettingsPersistenceInterface> test_make_watched_persistence() {
    return std::make_shared<watched_settings_persistence_t>(std::make_shared<memory_settings_persistence_t>());
  }

  void test_install_settings_manager(std::unique_ptr<SettingsManagerInterface> manager, const std::shared_ptr<SettingsPersistenceInterface> &persistence) {
    std::lock_guard lock {DD_DATA.mutex};
    DD_DATA.sm_instance.reset();
    DD_DATA.persistence_watch = std::dynamic_pointer_cast<watched_settings_persistence_t>(persistence);
    if (!manager || !DD_DATA.persistence_watch) {
      return;
    }

    DD_DATA.sm_instance = std::make_unique<RetryScheduler<SettingsManagerInterface>>(std::move(manager));
  }

  std::unique_ptr<platf::deinit_t> test_make_deinit_guard() {
    return make_deinit_guard();
  }

  void test_set_retry_interval(const std::chrono::milliseconds interval) {
    std::lock_guard lock {DD_DATA.mutex};
    DD_DATA.test_retry_interval = interval;
  }

  bool test_revert_retry_is_scheduled() {
    std::lock_guard lock {DD_DATA.mutex};
    return DD_DATA.sm_instance && DD_DATA.sm_instance->isScheduled();
  }

  void test_reset_display_device() {
    std::lock_guard lock {DD_DATA.mutex};
    if (DD_DATA.sm_instance) {
      DD_DATA.sm_instance->stop();
    }
    DD_DATA.sm_instance.reset();
    DD_DATA.persistence_watch.reset();
    DD_DATA.test_retry_interval.reset();
  #ifdef _WIN32
    DD_DATA.startup_exclusion.reset();
    DD_DATA.startup_ordinary_path.reset();
    startup_excluded = false;
    startup_legacy_tracked = false;
    startup_legacy_pending = false;
  #endif
  }
#endif
}  // namespace display_device
