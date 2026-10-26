/**
 * @file tests/unit/platform/windows/test_source_startup.cpp
 * @brief Startup classification and ordinary mutation exclusion without display or driver access.
 */
// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <atomic>
  #include <condition_variable>
  #include <filesystem>
  #include <fstream>
  #include <future>
  #include <limits>
  #include <mutex>
  #include <stdexcept>
  #include <thread>
  #include <utility>
  #include <vector>

  // platform includes
  // WinSock2.h must precede Windows.h from libdisplaydevice.
  // clang-format off
  #include <WinSock2.h>
  #include <Windows.h>
  // clang-format on

  // lib includes
  #include <display_device/settings_persistence_interface.h>
  #include <display_device/windows/json.h>
  #include <nlohmann/json.hpp>

  // local includes
  #include <src/config.h>
  #include <src/platform/windows/source_backend.h>
  #include <src/platform/windows/source_startup.h>
  #include <src/utility.h>

namespace {
  using namespace platf::source_startup;
  using namespace display_device;
  using namespace std::chrono_literals;

  SingleDisplayConfigState pending_state() {
    SingleDisplayConfigState state;
    state.m_initial = {{{"dock"}}, {"dock"}};
    state.m_modified = {{{"panel"}}, {{"dock", {{3840, 2160}, {60, 1}}}}, {{"dock", HdrState::Enabled}}, "dock", "panel"};
    return state;
  }

  coordinator_t::test_file_t encoded(std::string bytes) {
    return {store_state_e::completed_empty, std::move(bytes)};
  }

  nlohmann::json pending_owned() {
    const auto baseline = toJson(pending_state());
    return {{"schema", 2}, {"cleanup_pending", true}, {"add_armed", false}, {"source_retired", false}, {"identity", std::vector<unsigned int> {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}}, {"generation", std::uint64_t {17}}, {"deployment", "startup-fixture"}, {"initially_headless", false}, {"baseline", baseline}, {"settings", baseline}, {"restoration_state", baseline}, {"recovery_ids", std::vector<std::string> {"dock", "panel"}}, {"mapping", nullptr}};
  }

  class SourceStartupTest: public BaseTest {
  protected:
    coordinator_t::test_storage_t input;
    unsigned int reads {};

    std::unique_ptr<coordinator_t> make() {
      return coordinator_t::test_make({}, [this]() {
        ++reads;
        return input;
      });
    }

    void SetUp() override {
      BaseTest::SetUp();
      test_reset_display_device();
    }

    void TearDown() override {
      test_reset_display_device();
      BaseTest::TearDown();
    }
  };

  class counting_manager_t final: public SettingsManagerInterface {
  public:
    unsigned int applies {};
    unsigned int reverts {};
    unsigned int resets {};
    mutable unsigned int enumerations {};

    EnumeratedDeviceList enumAvailableDevices() const override {
      ++enumerations;
      return {EnumeratedDevice {.m_device_id = "dock"}};
    }

    std::string getDisplayName(const std::string &id) const override {
      return id;
    }

    ApplyResult applySettings(const SingleDisplayConfiguration &) override {
      ++applies;
      return ApplyResult::Ok;
    }

    RevertResult revertSettings() override {
      ++reverts;
      return RevertResult::ApiTemporarilyUnavailable;
    }

    bool resetPersistence() override {
      ++resets;
      return true;
    }
  };

  struct lifetime_observations_t {
    std::atomic_uint reverts {};
    std::atomic_uint resets {};
    std::atomic_uint destructions {};
    std::mutex mutex;
    std::condition_variable changed;
    bool block_revert {};
    bool entered {};
    bool released {};

    bool wait_for_revert() {
      std::unique_lock lock {mutex};
      return changed.wait_for(lock, 5s, [&]() {
        return entered;
      });
    }

    void release_revert() {
      {
        std::lock_guard lock {mutex};
        released = true;
      }
      changed.notify_all();
    }
  };

  class lifetime_manager_t final: public SettingsManagerInterface {
  public:
    explicit lifetime_manager_t(std::shared_ptr<lifetime_observations_t> observed):
        observed_ {std::move(observed)} {}

    ~lifetime_manager_t() override {
      ++observed_->destructions;
    }

    EnumeratedDeviceList enumAvailableDevices() const override {
      return {EnumeratedDevice {.m_device_id = "dock"}};
    }

    std::string getDisplayName(const std::string &id) const override {
      return id;
    }

    ApplyResult applySettings(const SingleDisplayConfiguration &) override {
      return ApplyResult::Ok;
    }

    RevertResult revertSettings() override {
      ++observed_->reverts;
      std::unique_lock lock {observed_->mutex};
      if (observed_->block_revert) {
        observed_->entered = true;
        observed_->changed.notify_all();
        observed_->changed.wait(lock, [&]() {
          return observed_->released;
        });
      }
      return RevertResult::ApiTemporarilyUnavailable;
    }

    bool resetPersistence() override {
      ++observed_->resets;
      return true;
    }

  private:
    std::shared_ptr<lifetime_observations_t> observed_;
  };
}  // namespace

TEST_F(SourceStartupTest, ConstructionAndStatusDoNotReadStorageOrChangeDisplayState) {
  auto coordinator = make();
  EXPECT_EQ(coordinator->status().disposition, disposition_e::uninspected);
  EXPECT_EQ(coordinator->status().owned, store_state_e::unchecked);
  EXPECT_EQ(reads, 0u);
  EXPECT_FALSE(ordinary_startup_recovery().mutation_excluded);
  EXPECT_TRUE(reset_persistence());
}

TEST_F(SourceStartupTest, ClassifiesAbsentAndExplicitCompletedEmptyWithoutReleasingExclusion) {
  for (const auto owned : {store_state_e::absent, store_state_e::completed_empty}) {
    for (const auto ordinary : {store_state_e::absent, store_state_e::completed_empty}) {
      test_reset_display_device();
      input = {{owned, {}}, {ordinary, {}}, false};
      auto coordinator = make();
      const auto current = coordinator->inspect();
      EXPECT_EQ(current.owned, owned);
      EXPECT_EQ(current.ordinary, ordinary);
      EXPECT_EQ(current.disposition, disposition_e::ordinary_clear);
      EXPECT_TRUE(current.exclusion_held);
      EXPECT_FALSE(reset_persistence());
      EXPECT_TRUE(coordinator->release_for_ordinary());
      EXPECT_TRUE(reset_persistence());
    }
  }
}

TEST_F(SourceStartupTest, CopiesLegacyRecoveryBeforeAllowingNativeAndRejectingVirtual) {
  const auto saved = pending_state();
  input.ordinary = encoded(toJson(saved));
  auto coordinator = make();
  unsigned int native_entries {};
  platf::source_backend::test_dependencies_t dependencies;
  dependencies.make_api = [&]() -> std::shared_ptr<WinApiLayerInterface> {
    ++native_entries;
    return nullptr;
  };
  auto backend = platf::source_backend::test_make_backend({}, std::move(dependencies));
  display_policy::source_request_t native;
  auto virtual_request = native;
  virtual_request.virtual_display = true;

  const auto inspected = coordinator->inspect();
  ASSERT_EQ(inspected.disposition, disposition_e::ordinary_recovery);
  ASSERT_TRUE(inspected.ordinary_recovery);
  EXPECT_EQ(*inspected.ordinary_recovery, saved);
  EXPECT_FALSE(backend->admits(native));
  EXPECT_FALSE(backend->admits(virtual_request));
  ASSERT_TRUE(coordinator->release_for_ordinary());
  EXPECT_TRUE(ordinary_startup_recovery().recovery_pending);
  EXPECT_TRUE(backend->admits(native));
  EXPECT_FALSE(backend->admits(virtual_request));
  EXPECT_FALSE(backend->begin(virtual_request, 1, 1));
  EXPECT_EQ(backend->status().generation, 0u);
  EXPECT_EQ(coordinator->status().owned_generation, 0u);
  EXPECT_EQ(native_entries, 0u);

  // Only ordinary persistence/scheduler completion clears the copied legacy obligation.
  auto persistence = test_make_watched_persistence();
  EXPECT_TRUE(persistence->store({1}));
  EXPECT_FALSE(backend->admits(virtual_request));
  EXPECT_TRUE(persistence->clear());
  EXPECT_TRUE(backend->admits(virtual_request));
}

TEST_F(SourceStartupTest, ClearStoresDoNotRequireVirtualProvisioningForNativeAdmission) {
  auto backend = platf::source_backend::test_make_backend({}, {});
  display_policy::source_request_t native;
  auto virtual_request = native;
  virtual_request.virtual_display = true;
  EXPECT_TRUE(backend->admits(native));
  EXPECT_TRUE(backend->admits(virtual_request));
  auto coordinator = make();
  ASSERT_TRUE(coordinator->release_for_ordinary());
  EXPECT_TRUE(backend->admits(native));
  EXPECT_TRUE(backend->admits(virtual_request));
  EXPECT_FALSE(ordinary_startup_recovery().recovery_pending);
}

TEST_F(SourceStartupTest, ValidOwnedJournalNeverConfersAuthorityOrReleasesOrdinaryMutation) {
  input.owned = encoded(pending_owned().dump());
  auto coordinator = make();
  const auto current = coordinator->inspect();
  EXPECT_EQ(current.owned, store_state_e::pending);
  EXPECT_EQ(current.owned_generation, 17u);
  EXPECT_EQ(current.disposition, disposition_e::owned_requires_authority);
  EXPECT_FALSE(coordinator->release_for_ordinary());
  EXPECT_FALSE(reset_persistence());
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
}

TEST_F(SourceStartupTest, BothPendingStoresAreConflictingAndNeverMerged) {
  input.owned = encoded(pending_owned().dump());
  input.ordinary = encoded(toJson(pending_state()));
  const auto owned_bytes = input.owned.bytes;
  const auto ordinary_bytes = input.ordinary.bytes;
  auto coordinator = make();
  const auto current = coordinator->inspect();
  EXPECT_EQ(current.disposition, disposition_e::conflicting_recovery);
  EXPECT_FALSE(coordinator->release_for_ordinary());
  EXPECT_EQ(input.owned.bytes, owned_bytes);
  EXPECT_EQ(input.ordinary.bytes, ordinary_bytes);
}

TEST_F(SourceStartupTest, UnreadableUnsafeAndMalformedStoresFailClosed) {
  const std::vector<coordinator_t::test_file_t> failures {
    {store_state_e::unreadable, {}},
    {store_state_e::unsafe_path, {}},
    encoded("{"),
    encoded("null"),
    encoded("{}"),
    encoded(" ")
  };
  for (bool owned : {false, true}) {
    for (const auto &failure : failures) {
      test_reset_display_device();
      input = {};
      (owned ? input.owned : input.ordinary) = failure;
      auto coordinator = make();
      EXPECT_EQ(coordinator->inspect().disposition, disposition_e::blocked);
      EXPECT_FALSE(coordinator->release_for_ordinary());
      EXPECT_FALSE(reset_persistence());
    }
  }
}

TEST_F(SourceStartupTest, RejectsOwnedSchemaIdentityGenerationMappingAndLibraryStateDamage) {
  std::vector<nlohmann::json> failures;
  auto changed = pending_owned();
  changed["schema"] = 3;
  failures.push_back(changed);
  changed = pending_owned();
  changed["identity"][0] = 256;
  failures.push_back(changed);
  changed = pending_owned();
  changed["generation"] = 0;
  failures.push_back(changed);
  changed = pending_owned();
  changed["initially_headless"] = true;
  failures.push_back(changed);
  changed = pending_owned();
  changed["restoration_state"] = "{}";
  failures.push_back(changed);
  changed = pending_owned();
  changed["mapping"] = {{"generation", 18}, {"identity", changed["identity"]}, {"adapter_low", 1u}, {"adapter_high", 0}, {"target", 1u}, {"device_id", "dock"}, {"monitor_path", "monitor"}};
  failures.push_back(changed);
  for (const auto &failure : failures) {
    test_reset_display_device();
    input.owned = encoded(failure.dump());
    auto coordinator = make();
    EXPECT_EQ(coordinator->inspect().owned, store_state_e::malformed);
    EXPECT_FALSE(coordinator->release_for_ordinary());
  }
}

TEST_F(SourceStartupTest, RetiredAndUnarmedPendingEpisodesKeepTheirActualGeneration) {
  for (bool armed : {false, true}) {
    for (bool retired : {false, true}) {
      test_reset_display_device();
      auto journal = pending_owned();
      journal["add_armed"] = armed;
      journal["source_retired"] = retired;
      journal["generation"] = std::numeric_limits<std::uint64_t>::max();
      if (retired) {
        journal["settings"] = "";
      }
      input.owned = encoded(journal.dump());
      auto coordinator = make();
      EXPECT_EQ(coordinator->inspect().owned_generation, std::numeric_limits<std::uint64_t>::max());
      EXPECT_FALSE(coordinator->release_for_ordinary());
    }
  }
}

TEST_F(SourceStartupTest, DuplicateKeysExcessNestingAndOversizedLegacyStateAreMalformed) {
  const auto bytes = toJson(pending_state());
  const std::vector<std::string> failures {
    "{\"initial\":{}," + bytes.substr(1),
    std::string(66, '[') + "0" + std::string(66, ']'),
    std::string(1024 * 1024 + 1, ' ')
  };
  for (const auto &failure : failures) {
    test_reset_display_device();
    input.ordinary = encoded(failure);
    auto coordinator = make();
    EXPECT_EQ(coordinator->inspect().ordinary, store_state_e::malformed);
    EXPECT_FALSE(coordinator->release_for_ordinary());
  }
}

TEST_F(SourceStartupTest, TransactionResidueBlocksAbsentEmptyAndPendingOwnedDestinations) {
  for (const auto &file : std::vector<coordinator_t::test_file_t> {{store_state_e::absent, {}}, encoded({}), encoded(pending_owned().dump())}) {
    test_reset_display_device();
    input.owned = file;
    input.transaction_residue = true;
    auto coordinator = make();
    EXPECT_EQ(coordinator->inspect().owned, store_state_e::transaction_residue);
    EXPECT_FALSE(coordinator->release_for_ordinary());
  }
}

TEST_F(SourceStartupTest, ReleaseRechecksStorageInsteadOfTrustingCachedClearInspection) {
  auto coordinator = make();
  EXPECT_EQ(coordinator->inspect().disposition, disposition_e::ordinary_clear);
  input.transaction_residue = true;
  EXPECT_FALSE(coordinator->release_for_ordinary());
  EXPECT_EQ(reads, 2u);
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
  input.transaction_residue = false;
  EXPECT_FALSE(coordinator->release_for_ordinary());
  EXPECT_EQ(reads, 3u);
  EXPECT_EQ(coordinator->status().disposition, disposition_e::recovery_proof_required);
}

TEST_F(SourceStartupTest, RefusalAndCoordinatorDestructionDoNotReleaseOrTransferTheGate) {
  {
    auto coordinator = make();
    input.owned.access = store_state_e::unreadable;
    EXPECT_EQ(coordinator->inspect().disposition, disposition_e::blocked);
  }
  EXPECT_FALSE(reset_persistence());
  input = {};
  auto other = make();
  const auto prior_reads = reads;
  EXPECT_FALSE(other->release_for_ordinary());
  EXPECT_EQ(reads, prior_reads);
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
}

TEST_F(SourceStartupTest, StorageExceptionKeepsGateClosedAndReinspectionCannotEraseTheUncertainty) {
  bool fail = true;
  auto coordinator = coordinator_t::test_make({}, [&]() {
    if (fail) {
      throw std::runtime_error {"fixture read failure"};
    }
    return input;
  });
  EXPECT_EQ(coordinator->inspect().owned, store_state_e::unreadable);
  EXPECT_FALSE(reset_persistence());
  fail = false;
  EXPECT_FALSE(coordinator->release_for_ordinary());
  EXPECT_TRUE(coordinator->status().recovery_proof_required);
}

TEST_F(SourceStartupTest, DeletingOrEmptyingPreviouslyObservedOwnedIntentCannotImpersonateCleanup) {
  for (const auto replacement : {store_state_e::absent, store_state_e::completed_empty}) {
    test_reset_display_device();
    input.owned = encoded(pending_owned().dump());
    auto coordinator = make();
    ASSERT_EQ(coordinator->inspect().owned, store_state_e::pending);
    input.owned = {replacement, {}};
    EXPECT_FALSE(coordinator->release_for_ordinary());
    EXPECT_EQ(coordinator->status().disposition, disposition_e::recovery_proof_required);
    EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
  }
}

TEST_F(SourceStartupTest, GateFencesOrdinarySchedulerAndAllUnmanagedMutationEntryPoints) {
  auto persistence = test_make_watched_persistence();
  auto manager = std::make_unique<counting_manager_t>();
  auto *observed = manager.get();
  test_install_settings_manager(std::move(manager), persistence);
  test_set_retry_interval(1h);
  revert_configuration();
  ASSERT_TRUE(test_revert_retry_is_scheduled());
  const auto prior_reverts = observed->reverts;
  const auto prior_enumerations = observed->enumerations;
  auto coordinator = make();
  EXPECT_EQ(coordinator->inspect().disposition, disposition_e::ordinary_clear);
  EXPECT_FALSE(test_revert_retry_is_scheduled());

  EXPECT_FALSE(init("unused.state", config::video_t {}));
  configure_display(SingleDisplayConfiguration {});
  configure_display(config::video_t {}, rtsp_stream::launch_session_t {});
  revert_configuration();
  EXPECT_FALSE(reset_persistence());
  native_display_configuration_t native;
  EXPECT_FALSE(native.begin(SingleDisplayConfiguration {}, false, 1, 0ms));
  EXPECT_FALSE(native.restore(1, 0ms));
  EXPECT_EQ(observed->applies, 0u);
  EXPECT_EQ(observed->reverts, prior_reverts);
  EXPECT_EQ(observed->resets, 0u);
  EXPECT_EQ(observed->enumerations, prior_enumerations);
  EXPECT_FALSE(coordinator->release_for_ordinary());
}

TEST_F(SourceStartupTest, GateAcquisitionWaitsForInflightOrdinaryRetryBeforeInspectingStorage) {
  auto observed = std::make_shared<lifetime_observations_t>();
  observed->block_revert = true;
  auto persistence = test_make_watched_persistence();
  test_install_settings_manager(std::make_unique<lifetime_manager_t>(observed), persistence);
  test_set_retry_interval(1h);
  native_display_configuration_t native;
  std::atomic_uint storage_reads {};
  auto coordinator = coordinator_t::test_make({}, [&]() {
    ++storage_reads;
    return input;
  });
  std::future<status_t> inspection;
  // Release before the async future's destructor on every assertion-failure path.
  auto unblock = util::fail_guard([observed]() {
    observed->release_revert();
  });

  ASSERT_TRUE(native.begin(std::nullopt, false, 1, 0ms));
  ASSERT_TRUE(native.restore(1, 1ms));
  ASSERT_TRUE(observed->wait_for_revert());
  ASSERT_EQ(observed->reverts.load(), 1u);
  inspection = std::async(std::launch::async, [&]() {
    return coordinator->inspect();
  });

  // The copied flag proves the inspector has entered acquire(), rather than merely waiting
  // for its worker to start. The callback remains held at the independent fake-manager barrier.
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!ordinary_startup_recovery().mutation_excluded && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(ordinary_startup_recovery().mutation_excluded);
  EXPECT_EQ(inspection.wait_for(0ms), std::future_status::timeout);
  EXPECT_EQ(storage_reads.load(), 0u);
  EXPECT_EQ(coordinator->status().disposition, disposition_e::uninspected);
  EXPECT_TRUE(native.status().recovery_pending);

  observed->release_revert();
  unblock.disable();
  ASSERT_EQ(inspection.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(inspection.get().disposition, disposition_e::ordinary_clear);
  EXPECT_EQ(storage_reads.load(), 1u);
  EXPECT_FALSE(test_revert_retry_is_scheduled());
  EXPECT_EQ(observed->reverts.load(), 1u);
  revert_configuration();
  EXPECT_FALSE(native.restore(1, 0ms));
  EXPECT_FALSE(reset_persistence());
  EXPECT_FALSE(test_revert_retry_is_scheduled());
  EXPECT_EQ(observed->reverts.load(), 1u);
}

TEST_F(SourceStartupTest, ActualOrdinaryGuardTeardownPreservesRecoveryWithoutRevertOrResetAcrossStartupGate) {
  auto observed = std::make_shared<lifetime_observations_t>();
  auto persistence = test_make_watched_persistence();
  const auto bytes = toJson(pending_state());
  const std::vector<std::uint8_t> saved {bytes.begin(), bytes.end()};
  ASSERT_TRUE(persistence->store(saved));
  test_install_settings_manager(std::make_unique<lifetime_manager_t>(observed), persistence);
  auto guard = test_make_deinit_guard();
  input.ordinary = encoded(bytes);
  auto coordinator = make();
  ASSERT_EQ(coordinator->inspect().disposition, disposition_e::ordinary_recovery);

  guard.reset();
  EXPECT_EQ(observed->destructions.load(), 1u);
  EXPECT_EQ(observed->reverts.load(), 0u);
  EXPECT_EQ(observed->resets.load(), 0u);
  EXPECT_EQ(persistence->load(), std::make_optional(saved));
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
  EXPECT_FALSE(test_revert_retry_is_scheduled());
  EXPECT_FALSE(reset_persistence());
  EXPECT_TRUE(coordinator->release_for_ordinary());
  EXPECT_TRUE(ordinary_startup_recovery().recovery_pending);
  EXPECT_EQ(persistence->load(), std::make_optional(saved));
}

TEST_F(SourceStartupTest, ActualOrdinaryGuardTeardownKeepsInheritedSingleRevertWithoutStartupGate) {
  auto observed = std::make_shared<lifetime_observations_t>();
  auto persistence = test_make_watched_persistence();
  const auto bytes = toJson(pending_state());
  const std::vector<std::uint8_t> saved {bytes.begin(), bytes.end()};
  ASSERT_TRUE(persistence->store(saved));
  test_install_settings_manager(std::make_unique<lifetime_manager_t>(observed), persistence);
  auto guard = test_make_deinit_guard();
  ASSERT_FALSE(ordinary_startup_recovery().mutation_excluded);

  guard.reset();
  EXPECT_EQ(observed->destructions.load(), 1u);
  EXPECT_EQ(observed->reverts.load(), 1u);
  EXPECT_EQ(observed->resets.load(), 0u);
  EXPECT_EQ(persistence->load(), std::make_optional(saved));
  EXPECT_FALSE(test_revert_retry_is_scheduled());
  EXPECT_TRUE(reset_persistence());
  EXPECT_EQ(observed->reverts.load(), 1u);
}

TEST_F(SourceStartupTest, OwnedReservationCannotRescheduleOrdinaryRecoveryAcrossStartupExclusion) {
  auto persistence = test_make_watched_persistence();
  ASSERT_TRUE(persistence->store({1}));
  auto manager = std::make_unique<counting_manager_t>();
  auto *observed = manager.get();
  test_install_settings_manager(std::move(manager), persistence);
  input.ordinary = encoded(toJson(pending_state()));
  auto coordinator = make();
  EXPECT_EQ(coordinator->inspect().disposition, disposition_e::ordinary_recovery);
  const auto attempt = owned_display_configuration_t::test_begin({}, GUID {}, 1, {"startup-fixture", true, true}, nullptr, nullptr);
  ASSERT_FALSE(attempt);
  EXPECT_EQ(attempt.error().code, platf::sudovda::errc_t::busy);
  EXPECT_EQ(observed->reverts, 0u);
  EXPECT_EQ(observed->enumerations, 0u);
  EXPECT_FALSE(test_revert_retry_is_scheduled());
  EXPECT_FALSE(reset_persistence());
}

namespace {
  class SourceStartupStorageTest: public BaseTest {
  protected:
    std::filesystem::path directory;
    paths_t paths;
    std::vector<std::filesystem::path> files;

    void write(const std::filesystem::path &path, const std::string &bytes) {
      std::ofstream output {path, std::ios::binary | std::ios::trunc};
      ASSERT_TRUE(output);
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      ASSERT_TRUE(output);
      files.push_back(path);
    }

    void SetUp() override {
      BaseTest::SetUp();
      test_reset_display_device();
      directory = std::filesystem::absolute(std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / (::testing::UnitTest::GetInstance()->current_test_info()->name() + std::string {".startup"})).lexically_normal().make_preferred();
      ASSERT_TRUE(std::filesystem::create_directory(directory));
      paths = {directory / "owned.json", directory / "display_device.state"};
    }

    void TearDown() override {
      test_reset_display_device();
      std::error_code error;
      for (const auto &file : files) {
        std::filesystem::remove(file, error);
      }
      std::filesystem::remove(directory, error);
      BaseTest::TearDown();
    }
  };
}  // namespace

TEST_F(SourceStartupStorageTest, ChecksRealAbsentEmptyAndPinnedLibraryBytesWithoutInitializingManager) {
  coordinator_t coordinator {paths};
  EXPECT_EQ(coordinator.inspect().disposition, disposition_e::ordinary_clear);
  write(paths.owned_journal, {});
  write(paths.ordinary_state, toJson(pending_state()));
  const auto pending = coordinator.inspect();
  EXPECT_EQ(pending.owned, store_state_e::completed_empty);
  EXPECT_EQ(pending.ordinary, store_state_e::pending);
  ASSERT_TRUE(pending.ordinary_recovery);
  EXPECT_EQ(*pending.ordinary_recovery, pending_state());
  EXPECT_TRUE(coordinator.release_for_ordinary());
  EXPECT_TRUE(ordinary_startup_recovery().recovery_pending);
}

TEST_F(SourceStartupStorageTest, RefusesBothWriterResidueKindsWithoutDeletingOrChoosingEither) {
  const auto temporary = std::filesystem::path {paths.owned_journal.native() + L".tmp-test"};
  const auto backup = std::filesystem::path {paths.owned_journal.native() + L".backup-test"};
  write(temporary, "temporary intent");
  write(backup, "earlier intent");
  coordinator_t coordinator {paths};
  EXPECT_EQ(coordinator.inspect().owned, store_state_e::transaction_residue);
  EXPECT_FALSE(coordinator.release_for_ordinary());
  EXPECT_TRUE(std::filesystem::exists(temporary));
  EXPECT_TRUE(std::filesystem::exists(backup));
}

TEST_F(SourceStartupStorageTest, LockedOwnedFileIsUnreadableRatherThanAbsent) {
  write(paths.owned_journal, {});
  const auto writer = CreateFileW(paths.owned_journal.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(writer, INVALID_HANDLE_VALUE);
  coordinator_t coordinator {paths};
  const auto status = coordinator.inspect();
  EXPECT_EQ(status.owned, store_state_e::unreadable);
  EXPECT_FALSE(coordinator.release_for_ordinary());
  EXPECT_TRUE(CloseHandle(writer));
}

TEST_F(SourceStartupStorageTest, MissingAncestorAndAliasedStoresCannotProveClearState) {
  paths.owned_journal = directory / "missing" / "owned.json";
  coordinator_t coordinator {paths};
  EXPECT_EQ(coordinator.inspect().owned, store_state_e::unreadable);
  EXPECT_FALSE(coordinator.release_for_ordinary());
  test_reset_display_device();
  coordinator_t aliased {{paths.ordinary_state, paths.ordinary_state}};
  EXPECT_EQ(aliased.inspect().owned, store_state_e::unsafe_path);
  EXPECT_FALSE(aliased.release_for_ordinary());
}

TEST_F(SourceStartupStorageTest, HardlinkedLeafCannotPassPathIdentityInspection) {
  write(paths.ordinary_state, {});
  ASSERT_TRUE(CreateHardLinkW(paths.owned_journal.c_str(), paths.ordinary_state.c_str(), nullptr));
  files.push_back(paths.owned_journal);
  coordinator_t coordinator {paths};
  const auto current = coordinator.inspect();
  EXPECT_EQ(current.owned, store_state_e::unsafe_path);
  EXPECT_EQ(current.ordinary, store_state_e::unsafe_path);
  EXPECT_FALSE(coordinator.release_for_ordinary());
}

TEST_F(SourceStartupStorageTest, ReadsActualOwnedWriterFormatWithoutIssuingRecoveryAuthority) {
  const GUID identity {1, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11}};
  const platf::sudovda::ownership_record_t record {identity, 42, "startup-fixture", false, toJson(pending_state())};
  auto journal = platf::sudovda::ownership_journal_t::create(paths.owned_journal, record, {"startup-fixture", true, true}, {"dock", "panel"});
  ASSERT_TRUE(journal);
  files.push_back(paths.owned_journal);
  coordinator_t coordinator {paths};
  const auto current = coordinator.inspect();
  EXPECT_EQ(current.owned, store_state_e::pending);
  EXPECT_EQ(current.owned_generation, 42u);
  EXPECT_EQ(current.disposition, disposition_e::owned_requires_authority);
  EXPECT_FALSE(coordinator.release_for_ordinary());
}

TEST_F(SourceStartupStorageTest, OrdinaryInitializationCannotReplaceTheInspectedLegacyLocation) {
  write(paths.ordinary_state, toJson(pending_state()));
  coordinator_t coordinator {paths};
  ASSERT_TRUE(coordinator.release_for_ordinary());
  EXPECT_FALSE(init(directory / "other.state", config::video_t {}));
  EXPECT_TRUE(ordinary_startup_recovery().recovery_pending);
}
#endif
