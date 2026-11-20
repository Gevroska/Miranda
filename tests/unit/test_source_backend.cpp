/**
 * @file tests/unit/test_source_backend.cpp
 * @brief Inert concrete Windows source construction, native binding and qualification boundaries.
 */
// test includes
#include "../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <atomic>
  #include <future>
  #include <mutex>
  #include <stdexcept>
  #include <utility>

  // platform includes
  // WinSock2.h must precede Windows.h from libdisplaydevice.
  // clang-format off
  #include <WinSock2.h>
  #include <Windows.h>
  // clang-format on

  // local includes
  #include <src/platform/windows/capture_binding.h>
  #include <src/platform/windows/source_backend.h>

namespace {
  using namespace display_device;
  using namespace platf::source_backend;
  using namespace rtsp_stream;
  using namespace std::chrono_literals;

  /** @brief Readback fixture with one positive active mode and no native operations. */
  class backend_api_t final: public WinApiLayerInterface {
  public:
    mutable std::atomic_uint reads {0};
    bool fail_query = false;
    bool empty = false;
    bool unavailable = false;
    bool wrong_selector = false;
    std::optional<HdrState> hdr;

    std::optional<PathAndModeData> queryDisplayConfig(QueryType) const override {
      ++reads;
      if (fail_query) {
        return std::nullopt;
      }
      PathAndModeData result;
      if (empty) {
        return result;
      }
      DISPLAYCONFIG_PATH_INFO path {};
      path.flags = DISPLAYCONFIG_PATH_ACTIVE;
      path.sourceInfo.adapterId = {4, 5};
      path.sourceInfo.id = 6;
      path.sourceInfo.sourceModeInfoIdx = 0;
      path.targetInfo.adapterId = {1, 2};
      path.targetInfo.id = 3;
      path.targetInfo.targetAvailable = !unavailable;
      path.targetInfo.refreshRate = {60000, 1001};
      DISPLAYCONFIG_MODE_INFO mode {};
      mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
      mode.adapterId = path.sourceInfo.adapterId;
      mode.id = path.sourceInfo.id;
      mode.sourceMode.width = 2560;
      mode.sourceMode.height = 1440;
      result.m_paths.push_back(path);
      result.m_modes.push_back(mode);
      return result;
    }

    std::string getErrorString(LONG) const override {
      return "fixture";
    }

    std::string getDeviceId(const DISPLAYCONFIG_PATH_INFO &) const override {
      return "native-library";
    }

    std::string getMonitorDevicePath(const DISPLAYCONFIG_PATH_INFO &) const override {
      return "native-path";
    }

    std::string getDisplayName(const DISPLAYCONFIG_PATH_INFO &) const override {
      return wrong_selector ? "changed-selector" : "native-selector";
    }

    std::string getFriendlyName(const DISPLAYCONFIG_PATH_INFO &) const override {
      return "native";
    }

    std::vector<std::byte> getEdid(const DISPLAYCONFIG_PATH_INFO &) const override {
      return {};
    }

    std::optional<HdrState> getHdrState(const DISPLAYCONFIG_PATH_INFO &) const override {
      return hdr;
    }

    std::optional<Rational> getDisplayScale(std::string_view, const DISPLAYCONFIG_SOURCE_MODE &) const override {
      return std::nullopt;
    }

    bool wakeDisplay(std::chrono::milliseconds) override {
      ADD_FAILURE() << "Unexpected native mutation";
      return false;
    }

    bool keepDisplayAwake() override {
      ADD_FAILURE() << "Unexpected native mutation";
      return false;
    }

    bool restorePowerRequest() override {
      ADD_FAILURE() << "Unexpected native mutation";
      return false;
    }

    LONG setDisplayConfig(std::vector<DISPLAYCONFIG_PATH_INFO>, std::vector<DISPLAYCONFIG_MODE_INFO>, UINT32) override {
      ADD_FAILURE() << "Unexpected native mutation";
      return ERROR_NOT_SUPPORTED;
    }

    bool setHdrState(const DISPLAYCONFIG_PATH_INFO &, HdrState) override {
      ADD_FAILURE() << "Unexpected native mutation";
      return false;
    }
  };

  /** @brief Ordinary scheduler fixture; configuration errors remain tolerant and restoration stays separate. */
  class native_manager_t final: public SettingsManagerInterface {
  public:
    std::atomic_uint applies {0}, reverts {0}, resets {0};
    ApplyResult apply_result = ApplyResult::DevicePrepFailed;
    SingleDisplayConfiguration configuration;
    std::shared_future<void> apply_gate;
    std::promise<void> apply_entered;

    EnumeratedDeviceList enumAvailableDevices() const override {
      return {{.m_device_id = "native-library", .m_friendly_name = "native"}};
    }

    std::string getDisplayName(const std::string &name) const override {
      return name.empty() ? "" : "native-selector";
    }

    ApplyResult applySettings(const SingleDisplayConfiguration &value) override {
      ++applies;
      configuration = value;
      if (apply_gate.valid()) {
        apply_entered.set_value();
        apply_gate.wait_for(2s);
      }
      return apply_result;
    }

    RevertResult revertSettings() override {
      ++reverts;
      return RevertResult::Ok;
    }

    bool resetPersistence() override {
      ++resets;
      return true;
    }
  };

  class denying_authority_t final: public authority_verifier_t {
  public:
    enum class outcome_e {
      denied,
      exception,
      null_grant,
      nonowning
    };
    outcome_e outcome = outcome_e::denied;
    unsigned int calls = 0;

    std::expected<std::shared_ptr<recovery_authorization_t>, platf::sudovda::failure_t> verify(const std::filesystem::path &, const display_policy::source_request_t &, std::uint64_t, const std::shared_ptr<recovery_authorization_t> &current) override {
      ++calls;
      EXPECT_FALSE(current);
      switch (outcome) {
        case outcome_e::exception:
          throw std::runtime_error {"inert qualification failure"};
        case outcome_e::null_grant:
          return std::shared_ptr<recovery_authorization_t> {};
        case outcome_e::nonowning:
          return std::shared_ptr<recovery_authorization_t> {std::shared_ptr<recovery_authorization_t> {}, borrowed_.get()};
        case outcome_e::denied:
          return std::unexpected(platf::sudovda::failure_t {platf::sudovda::errc_t::ownership_unverified, platf::sudovda::operation_t::journal});
      }
      std::unreachable();
    }

  private:
    std::shared_ptr<recovery_authorization_t> borrowed_ = test_recovery_authorization({"nonowning-fixture", true, true}, std::make_shared<int>(0));
  };

  class SourceBackendTest: public BaseTest {
  protected:
    std::shared_ptr<backend_api_t> api = std::make_shared<backend_api_t>();
    unsigned int callbacks = 0;
    native_manager_t *manager = nullptr;

    void SetUp() override {
      BaseTest::SetUp();
      auto value = std::make_unique<native_manager_t>();
      manager = value.get();
      test_install_settings_manager(std::move(value), test_make_watched_persistence());
    }

    void TearDown() override {
      test_reset_display_device();
      BaseTest::TearDown();
    }

    std::shared_ptr<source_backend_t> backend(options_t options = {}) {
      test_dependencies_t dependencies;
      dependencies.make_api = [this]() {
        ++callbacks;
        return api;
      };
      dependencies.capture_names = [this]() {
        ++callbacks;
        return std::vector<std::string> {"native-selector"};
      };
      dependencies.wake_native = [this](const auto &) {
        ++callbacks;
      };
      dependencies.render_adapter = [this](const std::string &) {
        ++callbacks;
        return std::optional {LUID {4, 5}};
      };
      dependencies.make_system = [this]() -> std::unique_ptr<platf::sudovda::system_t> {
        ++callbacks;
        ADD_FAILURE() << "Driver access";
        return {};
      };
      dependencies.begin_owned = [this](const auto &, const auto &, auto, const auto &) -> std::expected<std::unique_ptr<owned_display_configuration_t>, platf::sudovda::failure_t> {
        ++callbacks;
        ADD_FAILURE() << "B intent";
        return std::unexpected(platf::sudovda::failure_t {platf::sudovda::errc_t::journal_failed, platf::sudovda::operation_t::journal});
      };
      return test_make_backend(std::move(options), std::move(dependencies));
    }

    display_policy::source_request_t request(bool virtual_display = false) {
      display_policy::source_request_t result {};
      result.virtual_display = virtual_display;
      result.mode = {{1920, 1080}, {60, 1}, true};
      result.monitor_uuid = "11111111-1111-8111-8111-111111111111";
      return result;
    }
  };

  TEST_F(SourceBackendTest, ConstructorStatusAndAdmissionPerformNoNativeOrDriverIo) {
    const auto value = backend();
    EXPECT_EQ(value->status().generation, 0U);
    EXPECT_FALSE(value->status().restoration_ready);
    EXPECT_TRUE(value->admits(request()));
    EXPECT_TRUE(value->admits(request(true)));
    EXPECT_EQ(callbacks, 0U);
    EXPECT_EQ(api->reads, 0U);
    EXPECT_EQ(manager->applies, 0U);
    EXPECT_EQ(manager->reverts, 0U);
  }

  TEST_F(SourceBackendTest, ExistingFreshSourceVerifierDefaultsToRecoveryDenialWithoutInventedRequest) {
    denying_authority_t verifier;
    const auto qualified = verifier.verify_recovery({});
    EXPECT_FALSE(qualified);
    EXPECT_EQ(verifier.calls, 0u);
    EXPECT_EQ(callbacks, 0u);
  }

  TEST_F(SourceBackendTest, MissingAuthorityUsesCleanNoEffectsRetirementAndLeavesNativeAvailable) {
    const auto value = backend();
    EXPECT_FALSE(value->begin(request(true), 19, 1));
    EXPECT_EQ(value->status().generation, 19U);
    EXPECT_EQ(callbacks, 0U);
    EXPECT_EQ(api->reads, 0U);
    ASSERT_TRUE(value->restore(19, 0ms));
    ASSERT_TRUE(value->cleanup_and_retire(19));
    EXPECT_TRUE(value->status().source_released);
    EXPECT_FALSE(value->status().recovery_pending);
    EXPECT_FALSE(value->status().restoration_ready);
    ASSERT_TRUE(value->complete(19));
    EXPECT_TRUE(value->admits(request()));
    ASSERT_TRUE(value->begin(request(), 20, 2));
    EXPECT_EQ(value->status().source->kind, capture_source_t::kind_e::native);
    ASSERT_TRUE(value->restore(20, 0ms));
    ASSERT_TRUE(value->cleanup_and_retire(20));
    EXPECT_TRUE(value->complete(20));
  }

  TEST_F(SourceBackendTest, NativePublishesActualExactBindingWithoutMonitorUuidDespiteToleratedApplyFailure) {
    const auto value = backend();
    auto selected = request();
    selected.native_action = display_policy::native_action_e::configure;
    selected.native_configuration = SingleDisplayConfiguration {.m_device_id = "native-library", .m_device_prep = SingleDisplayConfiguration::DevicePreparation::VerifyOnly};
    ASSERT_TRUE(value->begin(selected, 1, 3));
    const auto state = value->status();
    ASSERT_TRUE(state.source);
    EXPECT_EQ(state.phase, source_backend_status_t::phase_e::ready);
    EXPECT_EQ(state.source->width, 2560U);
    EXPECT_EQ(state.source->height, 1440U);
    EXPECT_EQ(state.source->rate_numerator, 60000U);
    EXPECT_EQ(state.source->rate_denominator, 1001U);
    EXPECT_FALSE(state.source->observed_hdr);
    EXPECT_TRUE(state.source->monitor_uuid.empty());
    EXPECT_EQ(value->probe(*state.source, {}), 503);
    EXPECT_EQ(state.source->selector, "native-selector");
    EXPECT_EQ(state.source->device_id, "native-library");
    ASSERT_TRUE(state.source->windows_output);
    EXPECT_EQ(*state.source->windows_output, (windows_output_t {{1, 2}, 3, "native-path"}));
    EXPECT_EQ(manager->applies, 1U);
    EXPECT_FALSE(manager->configuration.m_resolution);
    EXPECT_FALSE(manager->configuration.m_refresh_rate);
    EXPECT_FALSE(manager->configuration.m_hdr_state);
    ASSERT_TRUE(value->restore(1, 0ms));
    ASSERT_TRUE(value->cleanup_and_retire(1));
    EXPECT_TRUE(value->complete(1));
  }

  TEST_F(SourceBackendTest, NativeDelayedRestoreRetiresLogicalSourceAndBlocksVirtualUntilPhysicalCompletion) {
    const auto value = backend();
    ASSERT_TRUE(value->begin(request(), 1, 1));
    ASSERT_TRUE(value->restore(1, 1h));
    ASSERT_TRUE(value->cleanup_and_retire(1));
    EXPECT_TRUE(value->status().source_released);
    EXPECT_TRUE(value->status().recovery_pending);
    EXPECT_FALSE(value->complete(1));
    EXPECT_FALSE(value->admits(request(true)));
    EXPECT_TRUE(value->admits(request()));
    EXPECT_FALSE(value->restore(2, 0ms));
    EXPECT_EQ(manager->reverts, 0U);
    ASSERT_TRUE(value->restore(1, 0ms));
    EXPECT_TRUE(value->complete(1));
    EXPECT_TRUE(value->admits(request(true)));
    EXPECT_EQ(manager->resets, 0U);
  }

  TEST_F(SourceBackendTest, DisabledNativeActionSchedulesOrdinaryRevertAndKeepsOptionalFieldsDisabled) {
    const auto value = backend();
    auto selected = request();
    selected.native_action = display_policy::native_action_e::revert;
    ASSERT_TRUE(value->begin(selected, 1, 1));
    EXPECT_EQ(manager->applies, 0U);
    EXPECT_EQ(manager->reverts, 1U);
    ASSERT_TRUE(value->restore(1, 0ms));
    ASSERT_TRUE(value->cleanup_and_retire(1));
    EXPECT_TRUE(value->complete(1));
  }

  TEST_F(SourceBackendTest, RetainedNativeRevalidatesExactIdentityAndRefreshesActualModeInNewEpoch) {
    const auto value = backend();
    ASSERT_TRUE(value->begin(request(), 1, 1));
    const auto original = value->status().source;
    ASSERT_TRUE(original);
    EXPECT_FALSE(value->validate_retained(*original, 1));
    ASSERT_TRUE(value->validate_retained(*original, 2));
    EXPECT_EQ(value->status().source->epoch, 2U);
    EXPECT_FALSE(value->validate_retained(*original, 3));
    const auto current = value->status().source;
    api->wrong_selector = true;
    EXPECT_FALSE(value->validate_retained(*current, 3));
    EXPECT_EQ(value->status().source->epoch, 2U);
    api->wrong_selector = false;
    ASSERT_TRUE(value->restore(1, 0ms));
    ASSERT_TRUE(value->cleanup_and_retire(1));
    EXPECT_TRUE(value->complete(1));
  }

  TEST_F(SourceBackendTest, ReadOnlyAvailabilityDistinguishesQueryFailureFromEmptyWindowsBaseline) {
    EXPECT_EQ(test_physical_source_available(*api), true);
    api->fail_query = true;
    EXPECT_FALSE(test_physical_source_available(*api).has_value());
    api->fail_query = false;
    api->empty = true;
    EXPECT_EQ(test_physical_source_available(*api), false);
    EXPECT_EQ(manager->applies, 0U);
    EXPECT_EQ(manager->reverts, 0U);
  }

  TEST_F(SourceBackendTest, MissingCapturableIdentityFailsWithoutASelectorFallbackAndStillRetiresNative) {
    api->wrong_selector = true;
    const auto value = backend();
    EXPECT_FALSE(value->begin(request(), 1, 1));
    EXPECT_FALSE(value->status().source);
    ASSERT_TRUE(value->restore(1, 0ms));
    ASSERT_TRUE(value->cleanup_and_retire(1));
    EXPECT_TRUE(value->complete(1));
  }

  TEST_F(SourceBackendTest, StatusAndAdmissionDoNotWaitForAnOrdinaryNativeConfigurationCall) {
    std::promise<void> release;
    manager->apply_gate = release.get_future().share();
    const auto value = backend();
    auto selected = request();
    selected.native_action = display_policy::native_action_e::configure;
    selected.native_configuration = SingleDisplayConfiguration {};
    auto pending = std::async(std::launch::async, [value, selected]() {
      return value->begin(selected, 1, 1);
    });
    auto unblock = util::fail_guard([&]() {
      release.set_value();
    });
    ASSERT_EQ(manager->apply_entered.get_future().wait_for(1s), std::future_status::ready);
    auto reading = std::async(std::launch::async, [value, selected]() {
      EXPECT_EQ(value->status().generation, 1U);
      EXPECT_FALSE(value->admits(selected));
    });
    EXPECT_EQ(reading.wait_for(100ms), std::future_status::ready);
    release.set_value();
    unblock.disable();
    reading.get();
    ASSERT_TRUE(pending.get());
    ASSERT_TRUE(value->restore(1, 0ms));
    ASSERT_TRUE(value->cleanup_and_retire(1));
    EXPECT_TRUE(value->complete(1));
  }

  TEST_F(SourceBackendTest, FreshNativeConfigurationSupersedesItsOwnPendingRetryWithANewGeneration) {
    const auto value = backend();
    auto selected = request();
    selected.native_action = display_policy::native_action_e::configure;
    selected.native_configuration = SingleDisplayConfiguration {};
    manager->apply_result = SettingsManagerInterface::ApplyResult::ApiTemporarilyUnavailable;
    ASSERT_TRUE(value->begin(selected, 1, 1));
    EXPECT_TRUE(test_revert_retry_is_scheduled());
    ASSERT_TRUE(value->restore(1, 1h));
    ASSERT_TRUE(value->cleanup_and_retire(1));
    manager->apply_result = SettingsManagerInterface::ApplyResult::DevicePrepFailed;
    ASSERT_TRUE(value->begin(selected, 2, 2));
    EXPECT_EQ(value->status().generation, 2U);
    EXPECT_FALSE(value->restore(1, 0ms));
    EXPECT_EQ(manager->applies, 2U);
    ASSERT_TRUE(value->restore(2, 0ms));
    ASSERT_TRUE(value->cleanup_and_retire(2));
    EXPECT_TRUE(value->complete(2));
  }

  TEST_F(SourceBackendTest, VerifierWithoutAnOwningGrantCannotCreateIntentAndStillHasANonzeroRetirementFence) {
    using outcome_e = denying_authority_t::outcome_e;
    for (const auto outcome : {outcome_e::denied, outcome_e::exception, outcome_e::null_grant, outcome_e::nonowning}) {
      const auto authority = std::make_shared<denying_authority_t>();
      authority->outcome = outcome;
      const auto value = backend({{}, authority});
      EXPECT_EQ(authority->calls, 0U);
      if (outcome == outcome_e::exception) {
        EXPECT_THROW(value->begin(request(true), 5, 1), std::runtime_error);
      } else {
        EXPECT_FALSE(value->begin(request(true), 5, 1));
      }
      EXPECT_EQ(authority->calls, 1U);
      EXPECT_EQ(value->status().generation, 5U);
      EXPECT_EQ(callbacks, 0U);
      EXPECT_EQ(api->reads, 0U);
      ASSERT_TRUE(value->restore(5, 0ms));
      ASSERT_TRUE(value->cleanup_and_retire(5));
      EXPECT_TRUE(value->complete(5));
    }
  }

  TEST_F(SourceBackendTest, InertRecoveryContextCopiesCannotCompleteOrActivateTheConcreteDispatcher) {
    const auto value = backend();
    auto context = test_recovery_context(value, 17);
    auto alias = context;
    EXPECT_TRUE(context->reconstructed());
    EXPECT_EQ(alias->generation(), 17u);
    EXPECT_EQ(alias->status().generation, 0u);
    EXPECT_FALSE(alias->status().restoration_ready);
    EXPECT_FALSE(alias->status().source);
    EXPECT_FALSE(value->complete(17));
    context.reset();
    EXPECT_EQ(callbacks, 0u);
    EXPECT_EQ(api->reads, 0u);
    EXPECT_EQ(value->status().generation, 0u);
  }
}  // namespace
#endif
