/**
 * @file tests/unit/test_video_capture.cpp
 * @brief Managed capture queue, selector, cache and resource-lifetime regressions without hardware.
 */
// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <array>
#include <atomic>
#include <barrier>
#include <bitset>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>

// local includes
#include <src/config.h>
#include <src/nvhttp.h>
#include <src/process.h>
#include <src/rtsp.h>
#include <src/video.h>

namespace {
  using namespace std::chrono_literals;

  std::shared_ptr<rtsp_stream::capture_source_t> capture_source() {
    auto source = std::make_shared<rtsp_stream::capture_source_t>();
    source->generation = 4;
    source->epoch = 9;
    source->selector = "frozen-selector";
    source->device_id = "frozen-library-id";
    source->windows_output = rtsp_stream::windows_output_t {{2, 3}, 7, "monitor-path"};
    source->width = 2560;
    source->height = 1440;
    source->rate_numerator = 120;
    source->rate_denominator = 1;
    return source;
  }

  class VideoManagedProbeTest: public BaseTest {
  protected:
    void SetUp() override {
      BaseTest::SetUp();
      saved_encoder = config::video.encoder;
      saved_hevc_mode = config::video.hevc_mode;
      saved_av1_mode = config::video.av1_mode;
      saved_flags = video::software.flags;
      saved_active_hevc_mode = video::active_hevc_mode;
      saved_active_av1_mode = video::active_av1_mode;
      saved_h264 = video::software.h264.capabilities;
      saved_hevc = video::software.hevc.capabilities;
      saved_av1 = video::software.av1.capabilities;
      config::video.encoder = "software";
      config::video.hevc_mode = 1;
      config::video.av1_mode = 1;
      video::software.flags = 0;
      video::test_support::reset_encoder_selection();
    }

    void TearDown() override {
      video::test_support::set_capture_hooks({});
      video::test_support::reset_encoder_selection();
      config::video.encoder = saved_encoder;
      config::video.hevc_mode = saved_hevc_mode;
      config::video.av1_mode = saved_av1_mode;
      video::software.flags = saved_flags;
      video::active_hevc_mode = saved_active_hevc_mode;
      video::active_av1_mode = saved_active_av1_mode;
      video::software.h264.capabilities = saved_h264;
      video::software.hevc.capabilities = saved_hevc;
      video::software.av1.capabilities = saved_av1;
      BaseTest::TearDown();
    }

    std::string saved_encoder;
    int saved_hevc_mode {}, saved_av1_mode {};
    int saved_active_hevc_mode {}, saved_active_av1_mode {};
    std::uint32_t saved_flags {};
    std::bitset<video::encoder_t::MAX_FLAGS> saved_h264, saved_hevc, saved_av1;
  };

  class inert_display_t final: public platf::display_t {
  public:
    platf::capture_e capture(const push_captured_image_cb_t &, const pull_free_image_cb_t &, bool *) override {
      return platf::capture_e::ok;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      return std::make_shared<platf::img_t>();
    }

    int dummy_img(platf::img_t *) override {
      return 0;
    }
  };

  struct destruction_gate_t {
    destruction_gate_t():
        finish {release.get_future().share()} {}

    void unblock() {
      if (!unblocked.exchange(true)) {
        release.set_value();
      }
    }

    std::promise<void> deleting;
    std::promise<void> release;
    std::shared_future<void> finish;
    std::atomic_bool unblocked = false;
    std::atomic_bool destroyed = false;
  };

  /** @brief Inert actual encode_session_t with a bounded, explicitly controlled destructor. */
  class blocked_encoder_t final: public video::encode_session_t {
  public:
    explicit blocked_encoder_t(std::shared_ptr<destruction_gate_t> gate):
        gate_ {std::move(gate)} {}

    ~blocked_encoder_t() override {
      gate_->deleting.set_value();
      gate_->finish.wait_for(5s);
      gate_->destroyed = true;
    }

    int convert(platf::img_t &) override {
      return 0;
    }

    void request_idr_frame() override {}

    void request_normal_frame() override {}

    void invalidate_ref_frames(int64_t, int64_t) override {}

  private:
    std::shared_ptr<destruction_gate_t> gate_;
  };

  /** @brief Inert backend; the real owner still issues and counts every capture ticket. */
  class capture_backend_t final: public rtsp_stream::source_backend_t {
  public:
    bool begin(const display_policy::source_request_t &, std::uint64_t generation, std::uint64_t epoch) override {
      auto metadata = capture_source();
      metadata->generation = generation;
      metadata->epoch = epoch;
      std::lock_guard lock {mutex_};
      state_ = {generation, rtsp_stream::source_backend_status_t::phase_e::ready, false, false, false, metadata};
      return true;
    }

    bool validate_retained(const rtsp_stream::capture_source_t &source, std::uint64_t epoch) override {
      std::lock_guard lock {mutex_};
      auto metadata = std::make_shared<rtsp_stream::capture_source_t>(source);
      metadata->epoch = epoch;
      state_.source = metadata;
      return true;
    }

    rtsp_stream::source_backend_status_t status() const override {
      std::lock_guard lock {mutex_};
      return state_;
    }

    int probe(const rtsp_stream::capture_source_t &, std::shared_ptr<rtsp_stream::capture_pin_t>) override {
      return 200;
    }

    bool restore(std::uint64_t, std::chrono::milliseconds) override {
      ++restores;
      std::lock_guard lock {mutex_};
      state_.phase = rtsp_stream::source_backend_status_t::phase_e::removal_ready;
      return true;
    }

    bool cleanup_and_retire(std::uint64_t) override {
      std::lock_guard lock {mutex_};
      state_.source_released = true;
      state_.phase = rtsp_stream::source_backend_status_t::phase_e::restored;
      return true;
    }

    bool complete(std::uint64_t) override {
      return true;
    }

    std::atomic_int restores = 0;

  private:
    mutable std::mutex mutex_;
    rtsp_stream::source_backend_status_t state_;
  };

  /** @brief Existing real-owner/client test seams, with no listeners, GPU or process execution. */
  class CaptureOwnerPinTest: public VideoManagedProbeTest {
  protected:
    void SetUp() override {
      VideoManagedProbeTest::SetUp();
      saved_state = config::nvhttp.file_state;
      saved_fresh = config::sunshine.flags[config::flag::FRESH_STATE];
      config::sunshine.flags[config::flag::FRESH_STATE] = false;
      config::nvhttp.file_state = (std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "capture_owner_clients.json").string();
      std::error_code error;
      std::filesystem::remove(config::nvhttp.file_state, error);
      nvhttp::test_support::reset_client_state();
      credentials = test_utils::certificates::generate_ca_credentials("capture lifetime");
      client_uuid = nvhttp::test_support::add_client("capture lifetime", credentials.x509, true);
      ASSERT_TRUE(nvhttp::update_client(client_uuid, std::nullopt, permissions::view | permissions::launch));
      proc::test_support::set_lifetime_snapshot_hook([] {
        return proc::lifetime_snapshot_t {7, app_uuid, 21, false, false};
      });
      backend = std::make_shared<capture_backend_t>();
      ASSERT_TRUE(rtsp_stream::set_source_backend([backend = backend] {
        return backend;
      }));
      owner = rtsp_stream::test_support::start_owner();
    }

    void TearDown() override {
      rtsp_stream::stop_application({});
      EXPECT_TRUE(until([] {
        return !rtsp_stream::source_snapshot().managed;
      }));
      owner.reset();
      EXPECT_TRUE(rtsp_stream::set_source_backend({}));
      proc::test_support::set_lifetime_snapshot_hook({});
      nvhttp::test_support::reset_client_state();
      std::error_code error;
      std::filesystem::remove(config::nvhttp.file_state, error);
      config::nvhttp.file_state = saved_state;
      config::sunshine.flags[config::flag::FRESH_STATE] = saved_fresh;
      VideoManagedProbeTest::TearDown();
    }

    std::shared_ptr<rtsp_stream::capture_pin_t> issue_pin() {
      const SimpleWeb::CaseInsensitiveMultimap args {{"rikey", "00112233445566778899aabbccddeeff"}, {"rikeyid", "1"}, {"appid", "7"}};
      launch = nvhttp::test_support::make_launch_session(*nvhttp::current_client(client_uuid, credentials.x509), args);
      launch->appid = 7;
      launch->app_uuid = app_uuid;
      display_policy::source_request_t request {};
      request.mode = {{2560, 1440}, {120, 1}, false};
      request.restore_delay = 0ms;
      const auto selected = std::make_shared<const display_policy::source_request_t>(request);
      const auto result = rtsp_stream::prepare_source_launch(launch, [selected](const rtsp_stream::source_snapshot_t &) {
        return std::pair {200, selected};
      });
      EXPECT_EQ(result, 200);
      if (result != 200) {
        return {};
      }
      EXPECT_TRUE(until([] {
        return rtsp_stream::source_snapshot().capture_pins == 0;
      }));
      return rtsp_stream::test_support::pin_capture(launch->source);
    }

    bool until(const std::function<bool()> &predicate) {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      do {
        rtsp_stream::test_support::poll_source();
        if (predicate()) {
          return true;
        }
        std::this_thread::yield();
      } while (std::chrono::steady_clock::now() < deadline);
      return false;
    }

    inline static const std::string app_uuid = "22222222-2222-4222-8222-222222222222";
    crypto::creds_t credentials;
    std::string client_uuid, saved_state;
    bool saved_fresh = false;
    std::shared_ptr<capture_backend_t> backend;
    std::shared_ptr<rtsp_stream::launch_session_t> launch;
    std::unique_ptr<platf::deinit_t> owner;
  };
}  // namespace

TEST(VideoCaptureQueueTest, BothQueuesPreserveEqualBindingsAndRejectDifferentSources) {
  for (bool synchronous : {false, true}) {
    video::config_t first {};
    first.source = capture_source();
    auto joining = first;
    joining.source = std::make_shared<const rtsp_stream::capture_source_t>(*first.source);
    EXPECT_TRUE(video::test_support::capture_queue_accepts(first, joining, synchronous));
    for (int change = 0; change != 5; ++change) {
      auto changed = capture_source();
      if (change == 0) {
        ++changed->generation;
      } else if (change == 1) {
        ++changed->epoch;
      } else if (change == 2) {
        changed->selector = "other-selector";
      } else if (change == 3) {
        ++changed->windows_output->target_id;
      } else {
        ++changed->width;
      }
      joining.source = changed;
      EXPECT_FALSE(video::test_support::capture_queue_accepts(first, joining, synchronous)) << synchronous << ':' << change;
    }
    joining.source.reset();
    EXPECT_FALSE(video::test_support::capture_queue_accepts(first, joining, synchronous));
    EXPECT_FALSE(video::test_support::capture_queue_accepts(joining, first, synchronous));
    EXPECT_TRUE(video::test_support::capture_queue_accepts(joining, joining, synchronous));
  }
}

TEST(VideoCaptureSelectorTest, InitialAndReopenSelectionUseTheSameFrozenSelector) {
  video::config_t config {};
  config.source = capture_source();
  EXPECT_EQ(video::test_support::capture_selector(config, "global-selected-display"), "frozen-selector");
  EXPECT_EQ(video::test_support::capture_selector(config, "new-first-enumerated-display"), "frozen-selector");
  config.source.reset();
  EXPECT_EQ(video::test_support::capture_selector(config, "legacy-selection"), "legacy-selection");
}

TEST(VideoCaptureQueueTest, ConcurrentFirstProducersFreezeExactlyOneBindingInBothQueues) {
  for (bool synchronous : {false, true}) {
    std::array<safe::signal_t, 2> joined;
    std::array<video::img_event_t, 2> images;
    std::array<safe::mail_t, 2> mailboxes {std::make_shared<safe::mail_raw_t>(), std::make_shared<safe::mail_raw_t>()};
    std::array<safe::mail_raw_t::event_t<bool>, 2> shutdown {mailboxes[0]->event<bool>(mail::shutdown), mailboxes[1]->event<bool>(mail::shutdown)};
    std::array<video::config_t, 2> configs {};
    configs[0].source = capture_source();
    auto different = capture_source();
    ++different->epoch;
    configs[1].source = different;
    video::test_support::capture_queue_t queue {synchronous};
    std::barrier start {3};
    std::array<bool, 2> accepted {};
    std::jthread first {[&] {
      start.arrive_and_wait();
      accepted[0] = queue.submit(configs[0], mailboxes[0], &joined[0], &images[0]);
    }};
    std::jthread second {[&] {
      start.arrive_and_wait();
      accepted[1] = queue.submit(configs[1], mailboxes[1], &joined[1], &images[1]);
    }};
    start.arrive_and_wait();
    first.join();
    second.join();
    EXPECT_NE(accepted[0], accepted[1]);
    EXPECT_EQ(queue.pending(), 1);
    const auto queued = queue.sources();
    ASSERT_EQ(queued.size(), 1);
    EXPECT_EQ(queued.front(), configs[accepted[0] ? 0 : 1].source);
    const auto rejected = accepted[0] ? 1 : 0;
    if (synchronous) {
      EXPECT_TRUE(joined[rejected].peek());
      EXPECT_TRUE(shutdown[rejected]->peek());
    } else {
      EXPECT_FALSE(images[rejected]->running());
    }
  }
}

TEST(VideoCaptureQueueTest, OverflowAndStoppedQueuesPreserveOlderContextsAndSignalRejectedOnes) {
  for (bool synchronous : {false, true}) {
    std::array<safe::signal_t, 32> joined;
    std::array<video::img_event_t, 32> images;
    std::array<safe::mail_t, 32> mailboxes;
    std::array<safe::mail_raw_t::event_t<bool>, 32> shutdown;
    for (std::size_t i = 0; i < mailboxes.size(); ++i) {
      mailboxes[i] = std::make_shared<safe::mail_raw_t>();
      shutdown[i] = mailboxes[i]->event<bool>(mail::shutdown);
    }
    video::config_t config {};
    config.source = capture_source();
    video::test_support::capture_queue_t queue {synchronous};
    for (std::size_t i = 0; i < 30; ++i) {
      ASSERT_TRUE(queue.submit(config, mailboxes[i], &joined[i], &images[i]));
    }
    EXPECT_FALSE(queue.submit(config, mailboxes[30], &joined[30], &images[30]));
    EXPECT_EQ(queue.pending(), 30);
    for (std::size_t i = 0; i < 30; ++i) {
      EXPECT_FALSE(joined[i].peek());
      if (!synchronous) {
        EXPECT_TRUE(images[i]->running());
      }
    }
    queue.stop();
    EXPECT_FALSE(queue.submit(config, mailboxes[31], &joined[31], &images[31]));
    EXPECT_EQ(queue.pending(), 30);
    queue.finish();
    for (std::size_t i = 0; i < 32; ++i) {
      if (synchronous) {
        EXPECT_TRUE(joined[i].peek());
        EXPECT_TRUE(shutdown[i]->peek());
      } else {
        EXPECT_FALSE(images[i]->running());
      }
    }
  }
}

TEST(VideoCaptureDisplayTest, BothModesIgnoreManagedSwitchAndBoundFailedReopenWithoutEnumeration) {
  auto clear_hooks = util::fail_guard([] {
    video::test_support::set_capture_hooks({});
  });
  for (bool synchronous : {false, true}) {
    int enumerations = 0, waits = 0;
    std::vector<std::string> selectors;
    video::config_t config {};
    config.source = capture_source();
    auto hooks = std::make_shared<video::test_support::capture_hooks_t>();
    hooks->enumerate_displays = [&](platf::mem_type_e) {
      ++enumerations;
      return std::vector<std::string> {"wrong-screen"};
    };
    hooks->retry_wait = [&](std::chrono::milliseconds delay) {
      EXPECT_EQ(delay, 200ms);
      ++waits;
    };
    hooks->open_display = [&](platf::mem_type_e, const std::string &selector, const video::config_t &candidate) -> std::shared_ptr<platf::display_t> {
      selectors.emplace_back(selector);
      EXPECT_EQ(candidate.source, config.source);
      return selectors.size() == 1 ? std::make_shared<inert_display_t>() : nullptr;
    };
    video::test_support::set_capture_hooks(hooks);
    const auto result = video::test_support::capture_display_cycle(config, synchronous, 8);
    EXPECT_TRUE(result.initial_open);
    EXPECT_FALSE(result.switch_reinit);
    EXPECT_TRUE(result.switch_consumed);
    EXPECT_FALSE(result.reopened);
    EXPECT_EQ(selectors, (std::vector<std::string> {"frozen-selector", "frozen-selector", "frozen-selector"}));
    EXPECT_EQ(enumerations, 0);
    EXPECT_EQ(waits, 2);
  }
}

TEST(VideoCaptureDisplayTest, LegacyModesStillEnumerateSwitchAndReopen) {
  const auto saved_output = config::video.output_name;
  config::video.output_name.clear();
  auto restore = util::fail_guard([saved_output] {
    config::video.output_name = saved_output;
    video::test_support::set_capture_hooks({});
  });
  for (bool synchronous : {false, true}) {
    int enumerations = 0;
    std::vector<std::string> selectors;
    auto hooks = std::make_shared<video::test_support::capture_hooks_t>();
    hooks->enumerate_displays = [&](platf::mem_type_e) {
      ++enumerations;
      return std::vector<std::string> {"legacy-a", "legacy-b"};
    };
    hooks->open_display = [&](platf::mem_type_e, const std::string &selector, const video::config_t &candidate) {
      selectors.emplace_back(selector);
      EXPECT_FALSE(candidate.source);
      return std::make_shared<inert_display_t>();
    };
    video::test_support::set_capture_hooks(hooks);
    const auto result = video::test_support::capture_display_cycle({}, synchronous, 1);
    EXPECT_TRUE(result.initial_open);
    EXPECT_TRUE(result.switch_reinit);
    EXPECT_TRUE(result.switch_consumed);
    EXPECT_TRUE(result.reopened);
    EXPECT_EQ(selectors, (std::vector<std::string> {"legacy-a", "legacy-b", "legacy-b"}));
    EXPECT_EQ(enumerations, 3);
  }
}

TEST_F(VideoManagedProbeTest, FreshGenerationAndEpochBypassTheCachedEncoder) {
  int probes = 0;
  std::shared_ptr<const rtsp_stream::capture_source_t> expected;
  auto validate = [&](video::encoder_t &encoder, bool, const video::config_t &config) {
    ++probes;
    EXPECT_EQ(config.source, expected);
    encoder.h264.capabilities.reset();
    encoder.hevc.capabilities.reset();
    encoder.av1.capabilities.reset();
    encoder.h264[video::encoder_t::PASSED] = true;
    return true;
  };
  const auto original_output = config::video.output_name;
  const auto original_adapter = config::video.adapter_name;
  video::config_t config {};
  config.source = expected = capture_source();
  ASSERT_EQ(video::test_support::probe_encoders(config, validate), 0);
  EXPECT_EQ(probes, 1);
  config.source = expected = std::make_shared<const rtsp_stream::capture_source_t>(*config.source);
  EXPECT_EQ(video::test_support::probe_encoders(config, validate), 0);
  EXPECT_EQ(probes, 1);
  auto next = capture_source();
  ++next->epoch;
  config.source = expected = next;
  EXPECT_EQ(video::test_support::probe_encoders(config, validate), 0);
  EXPECT_EQ(probes, 2);
  next = std::make_shared<rtsp_stream::capture_source_t>(*next);
  ++next->generation;
  config.source = expected = next;
  EXPECT_EQ(video::test_support::probe_encoders(config, validate), 0);
  EXPECT_EQ(probes, 3);
  config.source.reset();
  expected.reset();
  EXPECT_EQ(video::test_support::probe_encoders(config, validate), 0);
  EXPECT_EQ(probes, 4);
  EXPECT_EQ(video::test_support::probe_encoders(config, validate), 0);
  EXPECT_EQ(probes, 4);
  EXPECT_EQ(config::video.output_name, original_output);
  EXPECT_EQ(config::video.adapter_name, original_adapter);
}

TEST_F(VideoManagedProbeTest, FailedFreshEpochDoesNotReuseEarlierCapabilities) {
  video::config_t config {};
  config.source = capture_source();
  auto passing = [](video::encoder_t &encoder, bool, const video::config_t &) {
    encoder.h264.capabilities.set();
    encoder.hevc.capabilities.reset();
    encoder.av1.capabilities.reset();
    return true;
  };
  ASSERT_EQ(video::test_support::probe_encoders(config, passing), 0);
  EXPECT_TRUE(video::last_encoder_probe_supported_dynamic_range_for_codec[0]);
  auto next = capture_source();
  ++next->epoch;
  config.source = next;
  int attempts = 0;
  EXPECT_NE(video::test_support::probe_encoders(config, [&](video::encoder_t &, bool, const video::config_t &candidate) {
              ++attempts;
              EXPECT_EQ(candidate.source, next);
              return false;
            }),
            0);
  EXPECT_GT(attempts, 0);
  EXPECT_FALSE(video::last_encoder_probe_supported_dynamic_range_for_codec[0]);
}

TEST_F(VideoManagedProbeTest, MissingPreissuedPinRejectsProbeAndCaptureBeforeWorkersStart) {
  video::config_t config {};
  config.source = capture_source();
  EXPECT_NE(video::probe_encoders(*config.source, {}), 0);
  ASSERT_EQ(video::test_support::probe_encoders(config, [](video::encoder_t &encoder, bool, const video::config_t &) {
              encoder.h264.capabilities.set();
              encoder.hevc.capabilities.reset();
              encoder.av1.capabilities.reset();
              return true;
            }),
            0);
  auto mailbox = std::make_shared<safe::mail_raw_t>();
  auto shutdown = mailbox->event<bool>(mail::shutdown);
  video::capture(mailbox, config, nullptr);
  EXPECT_TRUE(shutdown->peek());
}

TEST_F(CaptureOwnerPinTest, FreshManagedGuardAndInvalidEntryFailuresRetireEarlierAdvertisements) {
  auto pin = issue_pin();
  ASSERT_TRUE(pin);
  const auto source = launch->source;
  bool allowed = true;
  int guards = 0, validations = 0;
  config::video.hevc_mode = 0;
  config::video.av1_mode = 0;
  auto hooks = std::make_shared<video::test_support::capture_hooks_t>();
  hooks->allow_probe = [&] {
    ++guards;
    return allowed;
  };
  hooks->needs_reenumeration = [] {
    return false;
  };
  hooks->validate_encoder = [&](video::encoder_t &encoder, bool, const video::config_t &) {
    ++validations;
    encoder.h264.capabilities.set();
    encoder.hevc.capabilities.set();
    encoder.av1.capabilities.set();
    return true;
  };
  video::test_support::set_capture_hooks(hooks);
  ASSERT_EQ(video::probe_encoders(*source, pin), 0);
  EXPECT_TRUE(video::test_support::encoder_selected(source));
  EXPECT_TRUE(video::last_encoder_probe_supported_dynamic_range_for_codec[1]);
  EXPECT_GT(video::active_hevc_mode, 1);
  const auto guards_before_same_entry_failure = guards;
  EXPECT_NE(video::probe_encoders(*source, {}), 0);
  EXPECT_EQ(guards, guards_before_same_entry_failure);
  EXPECT_TRUE(video::test_support::encoder_selected(source));
  EXPECT_TRUE(video::last_encoder_probe_supported_dynamic_range_for_codec[1]);
  allowed = false;
  EXPECT_NE(video::probe_encoders(*source, pin), 0);
  EXPECT_NE(video::probe_encoders(), 0);
  EXPECT_TRUE(video::test_support::encoder_selected(source));
  EXPECT_TRUE(video::last_encoder_probe_supported_dynamic_range_for_codec[1]);
  auto next = *source;
  ++next.epoch;
  const auto validated_before_guard_failure = validations;
  EXPECT_NE(video::probe_encoders(next, pin), 0);
  EXPECT_EQ(validations, validated_before_guard_failure);
  EXPECT_FALSE(video::test_support::encoder_selected(source));
  EXPECT_FALSE(video::last_encoder_probe_supported_dynamic_range_for_codec[1]);
  EXPECT_FALSE(video::last_encoder_probe_supported_yuv444_for_codec[1]);
  EXPECT_EQ(video::active_hevc_mode, 1);
  EXPECT_EQ(video::active_av1_mode, 1);
  allowed = true;
  ASSERT_EQ(video::probe_encoders(*source, pin), 0);
  const auto guards_before_invalid_entry = guards;
  next.width = 0;
  EXPECT_NE(video::probe_encoders(next, pin), 0);
  EXPECT_EQ(guards, guards_before_invalid_entry);
  EXPECT_FALSE(video::test_support::encoder_selected(source));
  EXPECT_FALSE(video::last_encoder_probe_supported_dynamic_range_for_codec[1]);
  EXPECT_EQ(video::active_hevc_mode, 1);
  ASSERT_EQ(video::probe_encoders(*source, pin), 0);
  next.width = source->width;
  EXPECT_NE(video::probe_encoders(next, {}), 0);
  EXPECT_FALSE(video::test_support::encoder_selected(source));
  EXPECT_FALSE(video::last_encoder_probe_supported_dynamic_range_for_codec[1]);
  pin.reset();
}

TEST_F(CaptureOwnerPinTest, ActualDetachedEncoderDestructorKeepsOwnerTicketUntilItFinishes) {
  auto pin = issue_pin();
  ASSERT_TRUE(pin);
  auto gate = std::make_shared<destruction_gate_t>();
  auto unblock = util::fail_guard([gate] {
    gate->unblock();
  });
  auto deleting = gate->deleting.get_future();
  video::test_support::detach_encoder_teardown(std::make_unique<blocked_encoder_t>(gate), std::move(pin));
  EXPECT_EQ(deleting.wait_for(2s), std::future_status::ready);
  rtsp_stream::launch_session_clear(launch->id);
  EXPECT_TRUE(until([] {
    return rtsp_stream::source_snapshot().prepared_pins == 0;
  }));
  EXPECT_EQ(rtsp_stream::source_snapshot().capture_pins, 1);
  EXPECT_TRUE(rtsp_stream::source_snapshot().managed);
  EXPECT_EQ(backend->restores, 0);
  EXPECT_FALSE(gate->destroyed);
  gate->unblock();
  EXPECT_TRUE(until([] {
    return !rtsp_stream::source_snapshot().managed;
  }));
  EXPECT_TRUE(gate->destroyed);
  EXPECT_EQ(backend->restores, 1);
}

TEST_F(CaptureOwnerPinTest, ActualSyncWorkerRetainsOwnerTicketAfterCallerJoinAndBlockedDestruction) {
  auto pin = issue_pin();
  ASSERT_TRUE(pin);
  auto gate = std::make_shared<destruction_gate_t>();
  auto unblock = util::fail_guard([gate] {
    gate->unblock();
  });
  auto deleting = gate->deleting.get_future();
  auto worker_release = std::make_shared<destruction_gate_t>();
  auto unblock_worker = util::fail_guard([worker_release] {
    worker_release->unblock();
  });
  auto releasing = worker_release->deleting.get_future();
  video::config_t config {};
  config.source = launch->source;
  config.source_pin = std::move(pin);
  ASSERT_EQ(video::test_support::probe_encoders(config, [](video::encoder_t &encoder, bool, const video::config_t &) {
              encoder.h264.capabilities.set();
              encoder.hevc.capabilities.reset();
              encoder.av1.capabilities.reset();
              return true;
            }),
            0);
  auto hooks = std::make_shared<video::test_support::capture_hooks_t>();
  hooks->sync_encode_session = [gate] {
    return std::make_unique<blocked_encoder_t>(gate);
  };
  hooks->before_sync_worker_release = [worker_release] {
    worker_release->deleting.set_value();
    worker_release->finish.wait_for(5s);
    worker_release->destroyed = true;
  };
  video::test_support::set_capture_hooks(hooks);
  auto mailbox = std::make_shared<safe::mail_raw_t>();
  auto caller = std::async(std::launch::async, [mailbox, config = std::move(config)]() mutable {
    video::capture(mailbox, std::move(config), nullptr);
  });
  EXPECT_EQ(deleting.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(caller.wait_for(2s), std::future_status::ready);
  rtsp_stream::launch_session_clear(launch->id);
  EXPECT_TRUE(until([] {
    return rtsp_stream::source_snapshot().prepared_pins == 0;
  }));
  EXPECT_EQ(rtsp_stream::source_snapshot().capture_pins, 1);
  EXPECT_TRUE(rtsp_stream::source_snapshot().managed);
  EXPECT_EQ(backend->restores, 0);
  EXPECT_FALSE(gate->destroyed);
  gate->unblock();
  caller.get();
  EXPECT_EQ(releasing.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(gate->destroyed);
  EXPECT_EQ(rtsp_stream::source_snapshot().capture_pins, 1);
  EXPECT_TRUE(rtsp_stream::source_snapshot().managed);
  EXPECT_EQ(backend->restores, 0);
  worker_release->unblock();
  EXPECT_TRUE(until([] {
    return !rtsp_stream::source_snapshot().managed;
  }));
  EXPECT_TRUE(worker_release->destroyed);
  EXPECT_EQ(backend->restores, 1);
}

TEST_F(CaptureOwnerPinTest, StoppedQueueRejectionDropsOnlyItsTicketAndFinalContextReleaseDrainsTheOwner) {
  for (bool synchronous : {false, true}) {
    auto pin = issue_pin();
    ASSERT_TRUE(pin);
    safe::signal_t accepted_joined, rejected_joined;
    video::img_event_t accepted_images, rejected_images;
    auto mailbox = std::make_shared<safe::mail_raw_t>();
    auto shutdown = mailbox->event<bool>(mail::shutdown);
    auto queue = std::make_unique<video::test_support::capture_queue_t>(synchronous);
    video::config_t config {};
    config.source = launch->source;
    config.source_pin = std::move(pin);
    ASSERT_TRUE(queue->submit(std::move(config), mailbox, &accepted_joined, &accepted_images));
    queue->stop();
    video::config_t rejected {};
    rejected.source = launch->source;
    rejected.source_pin = rtsp_stream::test_support::pin_capture(launch->source);
    ASSERT_TRUE(rejected.source_pin);
    EXPECT_FALSE(queue->submit(std::move(rejected), mailbox, &rejected_joined, &rejected_images));
    EXPECT_TRUE(until([] {
      return rtsp_stream::source_snapshot().capture_pins == 1;
    }));
    queue->finish();
    if (synchronous) {
      EXPECT_TRUE(accepted_joined.peek());
      EXPECT_TRUE(rejected_joined.peek());
      EXPECT_TRUE(shutdown->peek());
    } else {
      EXPECT_FALSE(accepted_images->running());
      EXPECT_FALSE(rejected_images->running());
    }
    rtsp_stream::launch_session_clear(launch->id);
    EXPECT_TRUE(until([] {
      return rtsp_stream::source_snapshot().prepared_pins == 0;
    }));
    EXPECT_EQ(rtsp_stream::source_snapshot().capture_pins, 1);
    queue.reset();
    EXPECT_TRUE(until([] {
      return !rtsp_stream::source_snapshot().managed;
    }));
  }
}

TEST(VideoCaptureLifetimeTest, ActualResourceDeletionRetainsTicketAcrossSharedFromThisAndDelayedTeardown) {
  std::atomic_bool released = false;
  std::atomic_bool ticket_alive_at_destruction = false;

  struct ticket_t {
    std::atomic_bool &released;

    ~ticket_t() {
      released = true;
    }
  };

  struct resource_t: std::enable_shared_from_this<resource_t> {
    std::weak_ptr<ticket_t> ticket;
    std::promise<void> *deleting = nullptr;
    std::shared_future<void> finish;
    std::atomic_bool *ticket_alive = nullptr;

    ~resource_t() {
      deleting->set_value();
      finish.wait();
      *ticket_alive = !ticket.expired();
    }
  };

  auto ticket = std::make_shared<ticket_t>(released);
  std::promise<void> deleting, finish;
  auto deleting_future = deleting.get_future();
  auto resource = video::make_capture_resource<resource_t>(ticket);
  resource->ticket = ticket;
  resource->deleting = &deleting;
  resource->finish = finish.get_future().share();
  resource->ticket_alive = &ticket_alive_at_destruction;
  auto retained = resource->shared_from_this();
  std::weak_ptr<resource_t> stale_observer = resource;
  ticket.reset();
  resource.reset();
  EXPECT_FALSE(released);
  std::jthread teardown {[retained = std::move(retained)]() mutable {
    retained.reset();
  }};
  const auto entered = deleting_future.wait_for(2s);
  EXPECT_EQ(entered, std::future_status::ready);
  EXPECT_FALSE(released);
  finish.set_value();
  teardown.join();
  EXPECT_TRUE(ticket_alive_at_destruction);
  EXPECT_TRUE(released);
  EXPECT_TRUE(stale_observer.expired());
}

TEST(VideoCaptureLifetimeTest, LegacyAllocationWithoutATicketPreservesSharedFromThis) {
  struct resource_t: std::enable_shared_from_this<resource_t> {};

  auto resource = video::make_capture_resource<resource_t>(std::shared_ptr<int> {});
  EXPECT_EQ(resource, resource->shared_from_this());
}
