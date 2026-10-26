/**
 * @file tests/unit/test_rtsp_source_owner.cpp
 * @brief Exercise managed admission and lifetime accounting with an inert display backend.
 */

// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>

// lib includes
extern "C" {
#include <moonlight-common-c/src/Rtsp.h>
}

// local includes
#include <src/config.h>
#include <src/nvhttp.h>
#include <src/process.h>
#include <src/rtsp.h>

namespace rtsp_stream {
  class rtsp_server_t;
  extern rtsp_server_t server;
  void free_msg(PRTSP_MESSAGE message);
  using msg_t = util::safe_ptr<RTSP_MESSAGE, &free_msg>;
  void cmd_announce(rtsp_server_t *server, boost::asio::ip::tcp::socket &socket, launch_session_t &launch, msg_t &&message);
}  // namespace rtsp_stream

namespace {
  using namespace std::chrono_literals;
  using namespace rtsp_stream;

  /** @brief Fake only: models published transaction progress and checked retirement independently. */
  class source_backend_fake_t: public source_backend_t {
  public:
    std::atomic_int begins {0}, validates {0}, probes {0}, restores {0}, cleanups {0}, completes {0};
    std::atomic_bool fail_probe {false}, fail_restore {false}, fail_cleanup {false}, fail_complete {false};
    std::atomic_bool pending_recovery {false};
    std::atomic_bool enforce_recovery_kind {false}, refresh_recovery {false};
    std::atomic_uint progresses {0};
    bool defer_ready_until_progress = false, terminal_progress_failure = false;
    std::shared_future<void> progress_gate;
    std::promise<void> progress_entered;
    std::function<void()> advance_progress_clock;
    std::shared_future<void> begin_gate;
    std::shared_future<void> complete_gate;
    std::promise<void> began;
    std::promise<void> restoration_started;
    std::promise<void> completion_started;
    std::shared_ptr<capture_pin_t> delayed_probe_pin;
    bool delay_probe_pin = false;
    bool native_mode_differs = false;
    bool unknown_hdr = false;
    bool omit_requested_hdr = false;
    bool wrong_observed_hdr = false;
    bool retained_native_mode_changes = false;
    std::optional<bool> probe_requested_hdr;
    std::chrono::milliseconds observed_delay {};

    bool admits(const display_policy::source_request_t &request) const override {
      std::lock_guard lock {mutex};
      return !enforce_recovery_kind || !state.recovery_pending || !state.source || request.virtual_display == (state.source->kind == capture_source_t::kind_e::owned_virtual);
    }

    void progress(std::uint64_t generation) override {
      if (defer_ready_until_progress) {
        if (++progresses == 1) {
          progress_entered.set_value();
        }
        if (progress_gate.valid()) {
          progress_gate.wait_for(2s);
        }
        if (advance_progress_clock) {
          advance_progress_clock();
        }
        std::lock_guard lock {mutex};
        if (state.generation == generation && !state.source_released) {
          state.phase = terminal_progress_failure ? source_backend_status_t::phase_e::failed : source_backend_status_t::phase_e::ready;
        }
        return;
      }
      std::lock_guard lock {mutex};
      if (refresh_recovery && state.generation == generation && state.source_released) {
        state.recovery_pending = pending_recovery;
      }
    }

    bool begin(const display_policy::source_request_t &request, std::uint64_t generation, std::uint64_t epoch) override {
      if (++begins == 1) {
        began.set_value();
      }
      if (begin_gate.valid()) {
        begin_gate.wait_for(2s);
      }
      auto metadata = std::make_shared<capture_source_t>();
      metadata->generation = generation;
      metadata->kind = request.virtual_display ? capture_source_t::kind_e::owned_virtual : capture_source_t::kind_e::native;
      metadata->epoch = epoch;
      metadata->selector = "exact-selector";
      metadata->device_id = "exact-device";
      metadata->monitor_uuid = request.virtual_display ? request.monitor_uuid : "";
      metadata->windows_output = windows_output_t {{1, 0}, 7, "exact-monitor-path"};
      metadata->width = request.mode.resolution.width;
      metadata->height = request.mode.resolution.height;
      metadata->rate_numerator = request.mode.refresh_rate.numerator;
      metadata->rate_denominator = request.mode.refresh_rate.denominator;
      metadata->observed_hdr = request.requested_hdr;
      metadata->requested_hdr = request.requested_hdr;
      if (omit_requested_hdr) {
        metadata->requested_hdr.reset();
      }
      if (wrong_observed_hdr && request.requested_hdr) {
        metadata->observed_hdr = !*request.requested_hdr;
      }
      if (unknown_hdr) {
        metadata->observed_hdr.reset();
      }
      if (!request.virtual_display && native_mode_differs) {
        metadata->width = 1280;
        metadata->height = 720;
        metadata->rate_numerator = 75;
        metadata->rate_denominator = 1;
        metadata->observed_hdr = false;
      }
      std::lock_guard lock {mutex};
      state = {generation, defer_ready_until_progress ? source_backend_status_t::phase_e::preparing : source_backend_status_t::phase_e::ready, false, false, false, metadata};
      return true;
    }

    bool validate_retained(const capture_source_t &source, std::uint64_t epoch) override {
      ++validates;
      std::lock_guard lock {mutex};
      if (!state.source || *state.source != source) {
        return false;
      }
      auto metadata = std::make_shared<capture_source_t>(source);
      metadata->epoch = epoch;
      if (retained_native_mode_changes && metadata->kind == capture_source_t::kind_e::native) {
        metadata->width = 1280;
        metadata->height = 720;
        metadata->rate_numerator = 75;
        metadata->rate_denominator = 1;
        metadata->observed_hdr = true;
      }
      state.source = metadata;
      return true;
    }

    source_backend_status_t status() const override {
      std::lock_guard lock {mutex};
      return state;
    }

    int probe(const capture_source_t &source, std::shared_ptr<capture_pin_t> pin) override {
      ++probes;
      probe_requested_hdr = source.requested_hdr;
      if (delay_probe_pin) {
        delayed_probe_pin = std::move(pin);
      }
      return fail_probe ? 503 : 200;
    }

    bool restore(std::uint64_t generation, std::chrono::milliseconds delay) override {
      if (++restores == 1) {
        restoration_started.set_value();
      }
      std::lock_guard lock {mutex};
      observed_delay = delay;
      if (generation != state.generation) {
        return false;
      }
      state.phase = fail_restore ? source_backend_status_t::phase_e::failed : source_backend_status_t::phase_e::removal_ready;
      return !fail_restore;
    }

    bool cleanup_and_retire(std::uint64_t generation) override {
      ++cleanups;
      std::lock_guard lock {mutex};
      if (generation != state.generation || fail_cleanup) {
        return false;
      }
      state.source_released = true;
      state.recovery_pending = pending_recovery;
      state.phase = source_backend_status_t::phase_e::restored;
      return true;
    }

    bool complete(std::uint64_t) override {
      if (++completes == 1) {
        completion_started.set_value();
      }
      if (complete_gate.valid()) {
        complete_gate.wait_for(2s);
      }
      return !fail_complete;
    }

    void heartbeat_failed() {
      std::lock_guard lock {mutex};
      state.heartbeat_failed = true;
    }

    void stale_mapping() {
      std::lock_guard lock {mutex};
      auto metadata = std::make_shared<capture_source_t>(*state.source);
      ++metadata->generation;
      state.source = metadata;
    }

  private:
    mutable std::mutex mutex;
    source_backend_status_t state;
  };

  /** @brief Run the real owner with fake transactions and synthetic genuine-app lifetime. */
  class RtspSourceOwnerTest: public BaseTest {
  protected:
    void SetUp() override {
      BaseTest::SetUp();
      original_state = config::nvhttp.file_state;
      original_timeout = config::stream.ping_timeout;
      original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
      config::sunshine.flags[config::flag::FRESH_STATE] = false;
      config::nvhttp.file_state = (std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "source_owner_clients.json").string();
      std::error_code error;
      std::filesystem::remove(config::nvhttp.file_state, error);
      nvhttp::test_support::reset_client_state();
      credentials_a = test_utils::certificates::generate_ca_credentials("source A");
      credentials_b = test_utils::certificates::generate_ca_credentials("source B");
      uuid_a = nvhttp::test_support::add_client("A", credentials_a.x509, true);
      uuid_b = nvhttp::test_support::add_client("B", credentials_b.x509, true);
      ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, permissions::view | permissions::launch));
      ASSERT_TRUE(nvhttp::update_client(uuid_b, std::nullopt, permissions::view));
      proc::test_support::set_lifetime_snapshot_hook([this]() {
        return proc::lifetime_snapshot_t {7, app_uuid, app_instance, app_live, app_placeholder};
      });
      backend = std::make_shared<source_backend_fake_t>();
      ASSERT_TRUE(set_source_backend([backend = backend]() {
        return backend;
      }));
      owner = test_support::start_owner();
    }

    void TearDown() override {
      test_support::set_source_progress_clock({});
      backend->fail_restore = false;
      backend->fail_cleanup = false;
      backend->fail_complete = false;
      backend->pending_recovery = false;
      backend->refresh_recovery = true;
      backend->delayed_probe_pin.reset();
      stop_application({});
      for (const auto &token : tokens) {
        test_support::release_media(token);
      }
      EXPECT_TRUE(until([]() {
        retry_source_cleanup();
        return !source_snapshot().managed && !source_snapshot().recovery_pending;
      }));
      owner.reset();
      EXPECT_TRUE(set_source_backend({}));
      proc::test_support::set_lifetime_snapshot_hook({});
      nvhttp::test_support::reset_client_state();
      std::error_code error;
      std::filesystem::remove(config::nvhttp.file_state, error);
      config::nvhttp.file_state = original_state;
      config::stream.ping_timeout = original_timeout;
      config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
      BaseTest::TearDown();
    }

    std::shared_ptr<launch_session_t> token(bool second = false) {
      const auto &uuid = second ? uuid_b : uuid_a;
      const auto &credentials = second ? credentials_b : credentials_a;
      const SimpleWeb::CaseInsensitiveMultimap args {{"rikey", "00112233445566778899aabbccddeeff"}, {"rikeyid", "1"}, {"appid", "7"}};
      auto launch = nvhttp::test_support::make_launch_session(*nvhttp::current_client(uuid, credentials.x509), args);
      launch->appid = 7;
      launch->app_uuid = app_uuid;
      tokens.emplace_back(launch);
      return launch;
    }

    std::shared_ptr<const display_policy::source_request_t> request(bool retain = false) {
      display_policy::source_request_t result {};
      result.virtual_display = true;
      result.monitor_uuid = "11111111-1111-8111-8111-111111111111";
      result.mode = {{1920, 1080}, {60000, 1001}, true};
      result.retain_while_app_running = retain;
      result.restore_delay = 15ms;
      return std::make_shared<const display_policy::source_request_t>(result);
    }

    int prepare(const std::shared_ptr<launch_session_t> &launch, std::shared_ptr<const display_policy::source_request_t> selected = {}, std::function<int()> execute = {}, std::function<void()> rollback = {}) {
      if (!selected) {
        selected = request();
      }
      return prepare_source_launch(launch, [selected](const source_snapshot_t &) {
        return std::pair {200, selected};
      },
                                   std::move(execute),
                                   std::move(rollback));
    }

    void start_media(const std::shared_ptr<launch_session_t> &launch) {
      ASSERT_TRUE(test_support::claim_launch(launch));
      ASSERT_TRUE(test_support::start_media(launch, true));
      launch_session_clear(launch->id);
      source_snapshot();
    }

    bool until(const std::function<bool()> &predicate) {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      do {
        test_support::poll_source();
        if (predicate()) {
          return true;
        }
        std::this_thread::yield();
      } while (std::chrono::steady_clock::now() < deadline);
      return false;
    }

    void slow_progress_observation(bool terminal_failure) {
      auto ticks = std::make_shared<std::atomic_llong>(0);
      test_support::set_source_progress_clock([ticks]() {
        return std::chrono::steady_clock::time_point {} + std::chrono::milliseconds {ticks->load()};
      });
      backend->defer_ready_until_progress = true;
      backend->terminal_progress_failure = terminal_failure;
      backend->advance_progress_clock = [ticks]() {
        ticks->fetch_add(101);
      };
      std::promise<void> release;
      backend->progress_gate = release.get_future().share();
      auto first = token();
      auto pending = std::async(std::launch::async, [this, first]() {
        return prepare(first);
      });
      auto unblock = util::fail_guard([&]() {
        release.set_value();
      });
      ASSERT_EQ(backend->progress_entered.get_future().wait_for(1s), std::future_status::ready);
      auto observation = std::async(std::launch::async, []() {
        return source_snapshot();
      });
      ASSERT_EQ(observation.wait_for(1s), std::future_status::ready);
      EXPECT_TRUE(observation.get().busy);
      EXPECT_EQ(pending.wait_for(0ms), std::future_status::timeout);
      release.set_value();
      unblock.disable();
      // Every refresh consumes 101 ms on the injected progress clock. No admission clock or
      // deadline changes, and barriers rather than arbitrary sleeps control publication.
      ASSERT_EQ(pending.wait_for(1s), std::future_status::ready);
      EXPECT_EQ(pending.get(), terminal_failure ? 503 : 200);
      EXPECT_EQ(backend->progresses, 1U);
      EXPECT_EQ(backend->probes, terminal_failure ? 0 : 1);
      launch_session_clear(first->id);
      ASSERT_TRUE(until([]() {
        return !source_snapshot().managed && !source_snapshot().recovery_pending;
      }));
    }

    inline static const std::string app_uuid = "22222222-2222-4222-8222-222222222222";
    std::atomic_bool app_live {false}, app_placeholder {false};
    std::atomic_uint64_t app_instance {21};
    crypto::creds_t credentials_a, credentials_b;
    std::string uuid_a, uuid_b, original_state;
    decltype(config::stream.ping_timeout) original_timeout;
    bool original_fresh_state = false;
    std::shared_ptr<source_backend_fake_t> backend;
    std::unique_ptr<platf::deinit_t> owner;
    std::vector<std::shared_ptr<launch_session_t>> tokens;
  };

  TEST_F(RtspSourceOwnerTest, TwoFirstAdmissionsSerializeBeforeBlockingBackendWork) {
    std::promise<void> release;
    backend->begin_gate = release.get_future().share();
    auto first = token();
    auto pending = std::async(std::launch::async, [this, first]() {
      return prepare(first);
    });
    ASSERT_EQ(backend->began.get_future().wait_for(1s), std::future_status::ready);
    EXPECT_EQ(prepare(token(true)), 503);
    EXPECT_EQ(backend->begins, 1);
    release.set_value();
    EXPECT_EQ(pending.get(), 200);
  }

  TEST_F(RtspSourceOwnerTest, SlowCompletedProgressPublishesReadyBeforeAnotherRefresh) {
    slow_progress_observation(false);
  }

  TEST_F(RtspSourceOwnerTest, SlowCompletedProgressConsumesTerminalFailureBeforeAnotherRefresh) {
    slow_progress_observation(true);
  }

  TEST_F(RtspSourceOwnerTest, SlowProgressDoesNotDelayCancellationOrPublishAfterRevocation) {
    auto ticks = std::make_shared<std::atomic_llong>(0);
    test_support::set_source_progress_clock([ticks]() {
      return std::chrono::steady_clock::time_point {} + std::chrono::milliseconds {ticks->load()};
    });
    backend->defer_ready_until_progress = true;
    backend->advance_progress_clock = [ticks]() {
      ticks->fetch_add(101);
    };
    std::promise<void> release;
    backend->progress_gate = release.get_future().share();
    auto first = token();
    auto pending = std::async(std::launch::async, [this, first]() {
      return prepare(first);
    });
    auto unblock = util::fail_guard([&]() {
      release.set_value();
    });
    ASSERT_EQ(backend->progress_entered.get_future().wait_for(1s), std::future_status::ready);
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, 0U));
    ASSERT_EQ(pending.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(pending.get(), 403);
    EXPECT_TRUE(source_snapshot().busy);
    release.set_value();
    unblock.disable();
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed && !source_snapshot().recovery_pending;
    }));
    EXPECT_EQ(backend->progresses, 1U);
    EXPECT_EQ(backend->probes, 0);
  }

  TEST_F(RtspSourceOwnerTest, DeadlineConsumedByPreflightSkipsBeginAndAllowsReplacementAndShutdown) {
    config::stream.ping_timeout = 20ms;
    const auto launch = token();
    EXPECT_EQ(prepare_source_launch(launch, [selected = request()](const source_snapshot_t &) {
                std::this_thread::sleep_for(40ms);
                return std::pair {200, selected};
              }),
              503);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    EXPECT_EQ(backend->begins, 0);
    EXPECT_EQ(backend->restores, 0);
    config::stream.ping_timeout = original_timeout;
    EXPECT_EQ(prepare(token()), 200);
    stop_application({});
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    auto stopping = std::async(std::launch::async, []() {
      test_support::stop_owner();
    });
    EXPECT_EQ(stopping.wait_for(1s), std::future_status::ready);
  }

  TEST_F(RtspSourceOwnerTest, RevocationBeforeWorkerEntrySkipsBeginAndDoesNotStrandSource) {
    auto reached = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::promise<void>>();
    const auto gate = release->get_future().share();
    test_support::set_owner_hook([reached, gate](std::string_view event) {
      if (event == "source_before_entry") {
        reached->set_value();
        gate.wait_for(2s);
      }
    });
    const auto launch = token();
    auto result = std::async(std::launch::async, [this, launch]() {
      return prepare(launch);
    });
    ASSERT_EQ(reached->get_future().wait_for(1s), std::future_status::ready);
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, 0U));
    EXPECT_EQ(result.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(result.get(), 403);
    release->set_value();
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    test_support::set_owner_hook({});
    EXPECT_EQ(backend->begins, 0);
    EXPECT_EQ(backend->restores, 0);
    EXPECT_EQ(prepare(token(true)), 200);
  }

  TEST_F(RtspSourceOwnerTest, SkippedBeginPreservesOlderIndependentRecoveryForNextSource) {
    backend->pending_recovery = true;
    auto first = token();
    ASSERT_EQ(prepare(first), 200);
    launch_session_clear(first->id);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    EXPECT_EQ(backend->completes, 0);
    config::stream.ping_timeout = 20ms;
    EXPECT_EQ(prepare_source_launch(token(), [selected = request()](const source_snapshot_t &) {
                std::this_thread::sleep_for(40ms);
                return std::pair {200, selected};
              }),
              503);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    EXPECT_EQ(backend->begins, 1);
    config::stream.ping_timeout = original_timeout;
    backend->pending_recovery = false;
    ASSERT_TRUE(until([]() {
      return !source_snapshot().busy;
    }));
    auto next = token();
    ASSERT_EQ(prepare(next), 200);
    EXPECT_EQ(backend->begins, 2);
    launch_session_clear(next->id);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    EXPECT_EQ(backend->completes, 1);
  }

  TEST_F(RtspSourceOwnerTest, JoinInheritsExactSourceAndIgnoresConflictingMode) {
    auto a = token();
    ASSERT_EQ(prepare(a), 200);
    start_media(a);
    const auto original = source_snapshot();
    auto different = std::make_shared<display_policy::source_request_t>(*request());
    different->mode = {{800, 600}, {165, 1}, false};
    auto b = token(true);
    ASSERT_EQ(prepare(b, different), 200);
    start_media(b);
    EXPECT_EQ(b->source, a->source);
    EXPECT_EQ(source_snapshot().request, original.request);
    EXPECT_EQ(backend->begins, 1);
    EXPECT_EQ(backend->probes, 1);
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, 0U));
    test_support::release_media(a);
    EXPECT_TRUE(test_support::token_authorized(b));
    EXPECT_EQ(source_snapshot().media_pins, 1U);
    EXPECT_EQ(backend->restores, 0);
  }

  TEST_F(RtspSourceOwnerTest, CrossKindRetiredRecoveryRejectsBeforeNewGenerationOrProcessClaim) {
    backend->enforce_recovery_kind = true;
    backend->refresh_recovery = true;
    for (const bool first_virtual : {false, true}) {
      backend->pending_recovery = true;
      auto first_request = std::make_shared<display_policy::source_request_t>(*request());
      first_request->virtual_display = first_virtual;
      auto first = token();
      ASSERT_EQ(prepare(first, first_request), 200);
      const auto generation = first->source_generation;
      launch_session_clear(first->id);
      ASSERT_TRUE(until([]() {
        return !source_snapshot().managed && !source_snapshot().busy;
      }));
      auto opposite = std::make_shared<display_policy::source_request_t>(*request());
      opposite->virtual_display = !first_virtual;
      const auto begins = backend->begins.load();
      EXPECT_EQ(prepare(token(), opposite), 503);
      EXPECT_EQ(backend->begins, begins);
      EXPECT_EQ(backend->status().generation, generation);
      const auto completes = backend->completes.load();
      backend->pending_recovery = false;
      ASSERT_TRUE(until([&]() {
        return backend->completes > completes && !source_snapshot().busy;
      }));
      auto second = token();
      ASSERT_EQ(prepare(second, opposite), 200);
      EXPECT_EQ(second->source_generation, generation + 1);
      launch_session_clear(second->id);
      ASSERT_TRUE(until([]() {
        return !source_snapshot().managed && !source_snapshot().busy;
      }));
    }
  }

  TEST_F(RtspSourceOwnerTest, RetiredIdleRecoveryCompletesWithoutAReplacementOrRepeatedRemoval) {
    backend->pending_recovery = true;
    backend->refresh_recovery = true;
    auto first = token();
    ASSERT_EQ(prepare(first), 200);
    const auto generation = first->source_generation;
    launch_session_clear(first->id);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed && !source_snapshot().busy;
    }));
    EXPECT_EQ(backend->completes, 0);
    backend->pending_recovery = false;
    ASSERT_TRUE(until([&]() {
      return backend->completes == 1 && !source_snapshot().busy;
    }));
    EXPECT_EQ(backend->status().generation, generation);
    EXPECT_EQ(backend->begins, 1);
    EXPECT_EQ(backend->restores, 1);
    EXPECT_EQ(backend->cleanups, 1);
  }

  TEST_F(RtspSourceOwnerTest, RecoveredGenerationSeedIsQuiescentAndDoesNotReuseAnOlderJournalGeneration) {
    ASSERT_TRUE(seed_source_generation(900));
    auto first = token();
    ASSERT_EQ(prepare(first), 200);
    EXPECT_GE(first->source_generation, 901U);
    EXPECT_FALSE(seed_source_generation(2000));
    EXPECT_FALSE(seed_source_generation(UINT64_MAX));
    launch_session_clear(first->id);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
  }

  TEST_F(RtspSourceOwnerTest, IdleCompletionFencesFreshAdmissionWithoutCreatingANewSourceRecord) {
    backend->pending_recovery = true;
    backend->refresh_recovery = true;
    std::promise<void> release;
    backend->complete_gate = release.get_future().share();
    auto unblock = util::fail_guard([&]() {
      release.set_value();
    });
    auto first = token();
    ASSERT_EQ(prepare(first), 200);
    const auto generation = first->source_generation;
    launch_session_clear(first->id);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    backend->pending_recovery = false;
    ASSERT_EQ(backend->completion_started.get_future().wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(source_snapshot().busy);
    EXPECT_EQ(prepare(token()), 503);
    EXPECT_FALSE(source_snapshot().managed);
    EXPECT_EQ(backend->status().generation, generation);
    release.set_value();
    unblock.disable();
    ASSERT_TRUE(until([]() {
      return !source_snapshot().recovery_pending;
    }));
  }

  TEST_F(RtspSourceOwnerTest, NativeFirstUsesObservedModeAndForceVirtualJoinKeepsExactNativeOutput) {
    backend->native_mode_differs = true;
    auto native = std::make_shared<display_policy::source_request_t>(*request());
    native->virtual_display = false;
    native->requested_hdr = true;
    auto a = token();
    ASSERT_EQ(prepare(a, native), 200);
    start_media(a);
    EXPECT_EQ(a->source->kind, capture_source_t::kind_e::native);
    EXPECT_TRUE(a->source->monitor_uuid.empty());
    EXPECT_EQ(a->source->width, 1280U);
    EXPECT_EQ(a->source->observed_hdr, false);
    EXPECT_EQ(a->source->requested_hdr, true);
    auto b = token(true);
    ASSERT_EQ(prepare(b, request()), 200);
    EXPECT_EQ(a->source, b->source);
    EXPECT_EQ(b->source->requested_hdr, true);
    EXPECT_EQ(source_snapshot().request, native);
    EXPECT_EQ(backend->begins, 1);
    EXPECT_EQ(backend->probes, 1);
  }

  TEST_F(RtspSourceOwnerTest, UnknownObservedHdrIsAllowedOnlyWithoutExplicitVirtualHdrRequirement) {
    backend->unknown_hdr = true;
    auto a = token();
    ASSERT_EQ(prepare(a), 200);
    EXPECT_FALSE(a->source->observed_hdr);
    EXPECT_FALSE(a->source->requested_hdr);
    auto different = *a->source;
    different.requested_hdr = false;
    EXPECT_NE(different, *a->source);
    EXPECT_EQ(different.observed_hdr, a->source->observed_hdr);
    launch_session_clear(a->id);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    auto explicit_hdr = std::make_shared<display_policy::source_request_t>(*request());
    explicit_hdr->requested_hdr = true;
    EXPECT_EQ(prepare(token(), explicit_hdr), 503);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
  }

  TEST_F(RtspSourceOwnerTest, ExplicitVirtualHdrMetadataComesFromFrozenRequestAndReachesProbe) {
    backend->omit_requested_hdr = true;
    for (const bool hdr : {false, true}) {
      auto selected = std::make_shared<display_policy::source_request_t>(*request());
      selected->requested_hdr = hdr;
      selected->mode.hdr = !hdr;
      auto launch = token();
      ASSERT_EQ(prepare(launch, selected), 200);
      EXPECT_EQ(launch->source->requested_hdr, hdr);
      EXPECT_EQ(launch->source->observed_hdr, hdr);
      EXPECT_EQ(backend->probe_requested_hdr, hdr);
      auto different = *launch->source;
      different.requested_hdr = !hdr;
      EXPECT_NE(different, *launch->source);
      different.requested_hdr = hdr;
      EXPECT_EQ(different, *launch->source);
      launch_session_clear(launch->id);
      ASSERT_TRUE(until([]() {
        return !source_snapshot().managed;
      }));
    }
  }

  TEST_F(RtspSourceOwnerTest, ExplicitVirtualHdrRejectsOppositeObservationForEnabledAndDisabledRequests) {
    backend->wrong_observed_hdr = true;
    for (const bool hdr : {false, true}) {
      auto selected = std::make_shared<display_policy::source_request_t>(*request());
      selected->requested_hdr = hdr;
      EXPECT_EQ(prepare(token(), selected), 503);
      ASSERT_TRUE(until([]() {
        return !source_snapshot().managed;
      }));
    }
    EXPECT_EQ(backend->probes, 0);
  }

  TEST_F(RtspSourceOwnerTest, NativeRetainedRevalidationKeepsHdrRequestButAcceptsActualModeAndHdr) {
    app_live = true;
    backend->retained_native_mode_changes = true;
    auto native = std::make_shared<display_policy::source_request_t>(*request(true));
    native->virtual_display = false;
    native->requested_hdr = false;
    auto first = token();
    ASSERT_EQ(prepare(first, native), 200);
    start_media(first);
    const auto original = first->source;
    EXPECT_EQ(original->observed_hdr, false);
    test_support::release_media(first);
    ASSERT_TRUE(until([]() {
      return source_snapshot().retained;
    }));
    auto next = token(true);
    ASSERT_EQ(prepare(next, request()), 200);
    EXPECT_EQ(next->source->kind, capture_source_t::kind_e::native);
    EXPECT_EQ(next->source->requested_hdr, false);
    EXPECT_EQ(next->source->observed_hdr, true);
    EXPECT_EQ(next->source->width, 1280U);
    EXPECT_EQ(next->source->selector, original->selector);
    EXPECT_EQ(next->source->device_id, original->device_id);
    EXPECT_EQ(next->source->windows_output, original->windows_output);
    EXPECT_NE(next->source->epoch, original->epoch);
    EXPECT_EQ(backend->probe_requested_hdr, false);
    EXPECT_EQ(backend->validates, 1);
  }

  TEST_F(RtspSourceOwnerTest, NativeWithoutExplicitHdrKeepsUnknownObservationAndUnspecifiedRequest) {
    backend->unknown_hdr = true;
    auto native = std::make_shared<display_policy::source_request_t>(*request());
    native->virtual_display = false;
    auto launch = token();
    ASSERT_EQ(prepare(launch, native), 200);
    EXPECT_EQ(launch->source->kind, capture_source_t::kind_e::native);
    EXPECT_FALSE(launch->source->requested_hdr);
    EXPECT_FALSE(launch->source->observed_hdr);
    EXPECT_FALSE(backend->probe_requested_hdr);
  }

  TEST_F(RtspSourceOwnerTest, DifferentApplicationCannotReplacePinnedSource) {
    auto a = token();
    ASSERT_EQ(prepare(a), 200);
    start_media(a);
    auto b = token(true);
    b->appid = 8;
    b->app_uuid = "33333333-3333-4333-8333-333333333333";
    EXPECT_EQ(prepare(b), 503);
    EXPECT_EQ(source_snapshot().app_uuid, app_uuid);
    EXPECT_EQ(backend->begins, 1);
  }

  TEST_F(RtspSourceOwnerTest, PendingPreparedPinSurvivesLastMediaDeparture) {
    auto a = token();
    ASSERT_EQ(prepare(a), 200);
    start_media(a);
    auto b = token(true);
    ASSERT_EQ(prepare(b), 200);
    test_support::release_media(a);
    test_support::poll_source();
    EXPECT_EQ(source_snapshot().prepared_pins, 1U);
    EXPECT_EQ(backend->restores, 0);
    launch_session_clear(b->id);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    EXPECT_EQ(backend->restores, 1);
    EXPECT_EQ(backend->cleanups, 1);
    EXPECT_EQ(backend->observed_delay, 15ms);
  }

  TEST_F(RtspSourceOwnerTest, DeferredJoinRefreshPinsSourceBeforeLastMediaDeparts) {
    auto a = token();
    ASSERT_EQ(prepare(a), 200);
    start_media(a);
    auto reached = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::promise<void>>();
    const auto gate = release->get_future().share();
    auto b = token(true);
    auto result = std::async(std::launch::async, [b, selected = request(), reached, gate]() {
      return prepare_source_launch(b, [selected](const source_snapshot_t &) {
        return std::pair {200, selected};
      },
                                   {},
                                   {},
                                   [reached, gate]() {
                                     reached->set_value();
                                     gate.wait_for(2s);
                                   });
    });
    ASSERT_EQ(reached->get_future().wait_for(1s), std::future_status::ready);
    test_support::release_media(a);
    EXPECT_EQ(source_snapshot().prepared_pins, 1U);
    EXPECT_EQ(backend->restores, 0);
    release->set_value();
    EXPECT_EQ(result.get(), 200);
    EXPECT_EQ(a->source, b->source);
    EXPECT_EQ(backend->probes, 1);
  }

  TEST_F(RtspSourceOwnerTest, FailedStartReleasesPreparationOnlyAfterUnwindTransition) {
    auto launch = token();
    ASSERT_EQ(prepare(launch), 200);
    ASSERT_TRUE(test_support::claim_launch(launch));
    EXPECT_EQ(source_snapshot().prepared_pins, 1U);
    EXPECT_EQ(source_snapshot().media_pins, 0U);
    EXPECT_FALSE(test_support::start_media(launch, false));
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    EXPECT_FALSE(test_support::token_authorized(launch));
  }

  TEST_F(RtspSourceOwnerTest, InvalidAnnounceReleasesPreparedOwnershipThroughProductionHandler) {
    auto launch = token();
    ASSERT_EQ(prepare(launch), 200);
    using boost::asio::ip::tcp;
    boost::asio::io_context io;
    tcp::acceptor listener {io, {boost::asio::ip::address_v4::loopback(), 0}};
    tcp::socket client {io};
    client.connect(listener.local_endpoint());
    auto socket = listener.accept();
    msg_t message {new RTSP_MESSAGE {}};
    std::string payload = "s=test\r\n";
    message->sequenceNumber = 1;
    message->payload = payload.data();
    message->payloadLength = static_cast<int>(payload.size());
    test_support::owner_operation([&]() {
      ASSERT_TRUE(on_owner_thread());
      EXPECT_FALSE(on_process_worker());
      cmd_announce(&server, socket, *launch, std::move(message));
    });
    socket.shutdown(tcp::socket::shutdown_send);
    std::string response;
    boost::system::error_code read_error;
    boost::asio::read(client, boost::asio::dynamic_buffer(response), read_error);
    ASSERT_EQ(read_error, boost::asio::error::eof);
    EXPECT_TRUE(response.starts_with("RTSP/1.0 400 BAD REQUEST\r\n"));
    EXPECT_NE(response.find("CSeq: 1\r\n"), std::string::npos);
    EXPECT_FALSE(test_support::token_authorized(launch));
    EXPECT_EQ(source_snapshot().prepared_pins, 0U);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
  }

  TEST_F(RtspSourceOwnerTest, RevocationDuringApplicationExecutionAnswersImmediatelyAndRollsBackAfterDrain) {
    auto launch = token();
    std::promise<void> executing, release;
    const auto gate = release.get_future().share();
    std::atomic_int rollbacks {0};
    auto result = std::async(std::launch::async, [this, launch, &executing, gate, &rollbacks]() {
      return prepare(launch, {}, [&executing, gate]() {
        executing.set_value();
        gate.wait_for(2s);
        return 200;
      },
                     [&rollbacks]() {
                       ++rollbacks;
                     });
    });
    ASSERT_EQ(executing.get_future().wait_for(1s), std::future_status::ready);
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, 0U));
    ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(result.get(), 403);
    EXPECT_EQ(rollbacks, 0);
    EXPECT_EQ(backend->restores, 0);
    release.set_value();
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    EXPECT_EQ(rollbacks, 1);
  }

  TEST_F(RtspSourceOwnerTest, ExpiredPreparationCancelsBeforeSlowBeginDrains) {
    config::stream.ping_timeout = 30ms;
    std::promise<void> release;
    backend->begin_gate = release.get_future().share();
    auto launch = token();
    auto result = std::async(std::launch::async, [this, launch]() {
      return prepare(launch);
    });
    ASSERT_EQ(backend->began.get_future().wait_for(1s), std::future_status::ready);
    ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(result.get(), 503);
    EXPECT_EQ(backend->restores, 0);
    EXPECT_TRUE(source_snapshot().busy);
    release.set_value();
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    EXPECT_EQ(backend->probes, 0);
  }

  TEST_F(RtspSourceOwnerTest, RevocationCancelsWithoutWaitingForSlowBeginOrExecutingApplication) {
    std::promise<void> release;
    backend->begin_gate = release.get_future().share();
    auto launch = token();
    std::atomic_int executions {0};
    auto result = std::async(std::launch::async, [this, launch, &executions]() {
      return prepare(launch, {}, [&]() {
        ++executions;
        return 200;
      });
    });
    ASSERT_EQ(backend->began.get_future().wait_for(1s), std::future_status::ready);
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, 0U));
    ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(result.get(), 403);
    EXPECT_EQ(executions, 0);
    EXPECT_EQ(backend->restores, 0);
    release.set_value();
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
  }

  TEST_F(RtspSourceOwnerTest, RetainedReconnectValidatesExactMappingAndStartsNewProbeEpoch) {
    app_live = true;
    auto a = token();
    ASSERT_EQ(prepare(a, request(true)), 200);
    start_media(a);
    const auto metadata = a->source;
    test_support::release_media(a);
    ASSERT_TRUE(until([]() {
      return source_snapshot().retained;
    }));
    auto b = token(true);
    ASSERT_EQ(prepare(b, request(true)), 200);
    EXPECT_EQ(b->source->generation, metadata->generation);
    EXPECT_NE(b->source->epoch, metadata->epoch);
    EXPECT_EQ(backend->validates, 1);
    EXPECT_EQ(backend->probes, 2);
    EXPECT_EQ(backend->begins, 1);
    EXPECT_FALSE(test_support::pin_capture(metadata));
  }

  TEST_F(RtspSourceOwnerTest, RevokeBeforeRetainedValidationPreservesPriorSourceAndEpoch) {
    app_live = true;
    auto first = token();
    ASSERT_EQ(prepare(first, request(true)), 200);
    start_media(first);
    const auto retained = first->source;
    test_support::release_media(first);
    ASSERT_TRUE(until([]() {
      return source_snapshot().retained;
    }));
    auto reached = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::promise<void>>();
    const auto gate = release->get_future().share();
    test_support::set_owner_hook([reached, gate](std::string_view event) {
      if (event == "source_validate_before_entry") {
        reached->set_value();
        gate.wait_for(2s);
      }
    });
    auto joining = token(true);
    auto result = std::async(std::launch::async, [this, joining]() {
      return prepare(joining, request(true));
    });
    ASSERT_EQ(reached->get_future().wait_for(1s), std::future_status::ready);
    ASSERT_TRUE(nvhttp::update_client(uuid_b, std::nullopt, 0U));
    EXPECT_EQ(result.get(), 403);
    release->set_value();
    ASSERT_TRUE(until([]() {
      return source_snapshot().retained;
    }));
    test_support::set_owner_hook({});
    EXPECT_EQ(source_snapshot().source, retained);
    EXPECT_EQ(backend->validates, 0);
    EXPECT_EQ(backend->restores, 0);
  }

  TEST_F(RtspSourceOwnerTest, RetainedReconnectCannotUseAnOldLiveObservationAfterApplicationExit) {
    app_live = true;
    auto first = token();
    ASSERT_EQ(prepare(first, request(true)), 200);
    start_media(first);
    test_support::release_media(first);
    ASSERT_TRUE(until([]() {
      return source_snapshot().retained;
    }));
    app_live = false;
    EXPECT_EQ(prepare(token(true), request(true)), 503);
    EXPECT_EQ(backend->validates, 0);
    EXPECT_EQ(backend->probes, 1);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
  }

  TEST_F(RtspSourceOwnerTest, RetainedAppExitIsObservedWithoutMediaWorkersAndStaleEventsAreIgnored) {
    app_live = true;
    auto a = token();
    ASSERT_EQ(prepare(a, request(true)), 200);
    start_media(a);
    test_support::release_media(a);
    ASSERT_TRUE(until([]() {
      return source_snapshot().retained;
    }));
    application_exited(app_instance + 1);
    test_support::poll_source();
    EXPECT_TRUE(source_snapshot().retained);
    app_live = false;
    ASSERT_EQ(backend->restoration_started.get_future().wait_for(1s), std::future_status::ready);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
  }

  TEST_F(RtspSourceOwnerTest, StaleRetainedMappingCannotReprobeOrPublishAReplacement) {
    app_live = true;
    auto a = token();
    ASSERT_EQ(prepare(a, request(true)), 200);
    start_media(a);
    test_support::release_media(a);
    ASSERT_TRUE(until([]() {
      return source_snapshot().retained;
    }));
    backend->stale_mapping();
    EXPECT_EQ(prepare(token(true), request(true)), 503);
    EXPECT_EQ(backend->probes, 1);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
  }

  TEST_F(RtspSourceOwnerTest, RestoreOnDisconnectOverridesLiveAppRetention) {
    app_live = true;
    auto a = token();
    ASSERT_EQ(prepare(a, request(false)), 200);
    start_media(a);
    test_support::release_media(a);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
  }

  TEST_F(RtspSourceOwnerTest, DesktopAndPlaceboNeverProvideRetainedLifetime) {
    app_placeholder = true;
    auto a = token();
    a->appid = 0;
    a->app_uuid.clear();
    ASSERT_EQ(prepare(a, request(true)), 200);
    start_media(a);
    test_support::release_media(a);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    EXPECT_EQ(backend->restores, 1);
  }

  TEST_F(RtspSourceOwnerTest, ActualCapturePinOutlivesMediaAndBlocksRestoreUntilFinalRelease) {
    auto a = token();
    ASSERT_EQ(prepare(a), 200);
    start_media(a);
    auto pin = test_support::pin_capture(a->source);
    auto detached_resource = pin;
    ASSERT_TRUE(pin);
    test_support::release_media(a);
    pin.reset();
    test_support::poll_source();
    EXPECT_EQ(source_snapshot().capture_pins, 1U);
    EXPECT_EQ(backend->restores, 0);
    detached_resource.reset();
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
  }

  TEST_F(RtspSourceOwnerTest, ProbeResourcePinBlocksCleanupAfterFailedProbeUntilActualDestruction) {
    backend->fail_probe = true;
    backend->delay_probe_pin = true;
    EXPECT_EQ(prepare(token()), 503);
    EXPECT_EQ(source_snapshot().capture_pins, 1U);
    EXPECT_EQ(backend->restores, 0);
    backend->delayed_probe_pin.reset();
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
  }

  TEST_F(RtspSourceOwnerTest, CleanupFailureRetainsOwnershipBlocksReplacementAndRequiresExplicitRetry) {
    backend->fail_cleanup = true;
    auto a = token();
    ASSERT_EQ(prepare(a), 200);
    launch_session_clear(a->id);
    ASSERT_TRUE(until([]() {
      return source_snapshot().cleanup_blocked;
    }));
    EXPECT_TRUE(source_snapshot().busy);
    EXPECT_EQ(prepare(token(true)), 503);
    for (int i = 0; i < 10; ++i) {
      test_support::poll_source();
    }
    EXPECT_EQ(backend->cleanups, 1);
    backend->fail_cleanup = false;
    retry_source_cleanup();
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
  }

  TEST_F(RtspSourceOwnerTest, StickyHeartbeatFailureInvalidatesAllAdmissionsAndKeepsCaptureLeaseUntilDrain) {
    auto a = token();
    ASSERT_EQ(prepare(a), 200);
    start_media(a);
    auto b = token(true);
    ASSERT_EQ(prepare(b), 200);
    auto pin = test_support::pin_capture(a->source);
    ASSERT_TRUE(pin);
    backend->heartbeat_failed();
    test_support::poll_source();
    EXPECT_FALSE(test_support::token_authorized(a));
    EXPECT_FALSE(test_support::token_authorized(b));
    EXPECT_EQ(backend->restores, 0);
    test_support::release_media(a);
    pin.reset();
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    EXPECT_EQ(backend->cleanups, 1);
  }

  TEST_F(RtspSourceOwnerTest, RestoreFailureKeepsRecoveryAndCanBeRetriedWithoutReplacingSource) {
    backend->fail_restore = true;
    auto a = token();
    ASSERT_EQ(prepare(a), 200);
    launch_session_clear(a->id);
    ASSERT_TRUE(until([]() {
      return source_snapshot().cleanup_blocked;
    }));
    EXPECT_TRUE(source_snapshot().managed);
    EXPECT_EQ(backend->cleanups, 0);
    backend->fail_restore = false;
    retry_source_cleanup();
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
  }

  TEST_F(RtspSourceOwnerTest, FailedRecoveryCompletionDoesNotRepeatRemovalAndKeepsAdmissionBlocked) {
    backend->fail_complete = true;
    std::promise<void> release;
    backend->complete_gate = release.get_future().share();
    bool released = false;
    auto unblock = util::fail_guard([&]() {
      if (!released) {
        release.set_value();
      }
    });
    auto a = token();
    ASSERT_EQ(prepare(a), 200);
    launch_session_clear(a->id);
    ASSERT_EQ(backend->completion_started.get_future().wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(source_snapshot().busy);
    EXPECT_FALSE(source_snapshot().cleanup_blocked);
    release.set_value();
    released = true;
    ASSERT_TRUE(until([]() {
      return source_snapshot().cleanup_blocked;
    }));
    EXPECT_EQ(backend->cleanups, 1);
    EXPECT_EQ(backend->completes, 1);
    EXPECT_EQ(prepare(token(true)), 503);
    backend->fail_complete = false;
    retry_source_cleanup();
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
    EXPECT_EQ(backend->cleanups, 1);
    EXPECT_EQ(backend->completes, 2);
    EXPECT_EQ(backend->restores, 1);
  }

  TEST_F(RtspSourceOwnerTest, DeniedStopPreservesSourceAndAuthorizedStopInvalidatesEveryAdmission) {
    auto a = token();
    ASSERT_EQ(prepare(a), 200);
    start_media(a);
    auto b = token(true);
    ASSERT_EQ(prepare(b), 200);
    EXPECT_FALSE(authorize_operation(uuid_b, credentials_b.x509, permissions::launch, []() {
      stop_application({});
    }));
    EXPECT_TRUE(test_support::token_authorized(a));
    EXPECT_TRUE(test_support::token_authorized(b));
    EXPECT_TRUE(authorize_operation(uuid_a, credentials_a.x509, permissions::launch, []() {
      stop_application({});
    }));
    EXPECT_FALSE(test_support::token_authorized(a));
    EXPECT_FALSE(test_support::token_authorized(b));
    test_support::release_media(a);
    ASSERT_TRUE(until([]() {
      return !source_snapshot().managed;
    }));
  }

  TEST_F(RtspSourceOwnerTest, ShutdownContinuesDrainingFinalCaptureReleaseWithoutOwnerWorkerCycle) {
    auto a = token();
    ASSERT_EQ(prepare(a), 200);
    auto pin = test_support::pin_capture(a->source);
    ASSERT_TRUE(pin);
    auto stopping = std::async(std::launch::async, []() {
      test_support::stop_owner();
    });
    EXPECT_EQ(stopping.wait_for(30ms), std::future_status::timeout);
    pin.reset();
    EXPECT_EQ(stopping.wait_for(2s), std::future_status::ready);
    stopping.get();
  }
}  // namespace
