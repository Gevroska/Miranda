/**
 * @file tests/unit/platform/windows/test_sudovda.cpp
 * @brief SudoVDA adapter regressions using exclusively fake system boundaries.
 */
// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <algorithm>
  #include <cstdint>
  #include <cstring>
  #include <filesystem>
  #include <limits>
  #include <map>
  #include <stdexcept>
  #include <tuple>
  #include <utility>

  // lib includes
  #include <nlohmann/json.hpp>

  // local includes
  #include <src/httpcommon.h>
  #include <src/platform/windows/sudovda.h>

namespace {
  using namespace platf::sudovda;
  using namespace std::chrono_literals;

  const GUID identity {0x12345678, 0x9abc, 0xdef0, {1, 2, 3, 4, 5, 6, 7, 8}};
  const GUID other_identity {0x12345678, 0x9abc, 0xdef0, {1, 2, 3, 4, 5, 6, 7, 9}};
  const LUID render_adapter {0x98765432, 0x12345678};
  const wire::target_t target {{0x11223344, 0x55667788}, 7};

  request_t request() {
    return {identity, render_adapter, 1920, 1080, 60, 1, "Miranda", "stable-id", true, 3, 0ms};
  }

  display_t display(wire::target_t selected_target = target, GUID selected_identity = identity) {
    return {selected_target, selected_identity, L"fake-monitor-path", L"fake-gdi-name"};
  }

  struct fake_t {
    std::vector<std::string> events;
    std::optional<failure_t> open_error;
    wire::version_t version {0, 2, 1, 1};
    wire::watchdog_t watchdog {3, 3};
    wire::target_t added_target {target};
    wire::add_t added {};
    LUID adapter {};
    std::vector<GUID> removed;
    std::map<DWORD, failure_t> io_errors;
    std::map<DWORD, std::size_t> response_sizes;
    enumeration_t before;
    enumeration_t after {{identity}, {display()}};
    unsigned int enumerations {};
    unsigned int enumeration_errors {};
    unsigned int missing_enumerations {};
    unsigned int remove_failures {};
    unsigned int close_failures {};
    unsigned int heartbeat_start_failures {};
    unsigned int heartbeat_start_throws {};
    std::chrono::milliseconds period {};
    std::function<void()> tick;
    bool ping_during_stop {};
    bool preflight_error {};
    std::function<void()> before_add;

    void fire() {
      if (tick) {
        tick();
      }
    }
  };

  class fake_heartbeat_t final: public heartbeat_t {
  public:
    explicit fake_heartbeat_t(std::shared_ptr<fake_t> fake):
        fake_(std::move(fake)) {}

    ~fake_heartbeat_t() override {
      stop();
    }

    void stop() noexcept override {
      if (running_) {
        fake_->events.emplace_back("stop-request");
        if (fake_->ping_during_stop) {
          fake_->fire();
        }
        fake_->tick = {};
        fake_->events.emplace_back("stop-joined");
        running_ = false;
      }
    }

  private:
    std::shared_ptr<fake_t> fake_;
    bool running_ {true};
  };

  class fake_system_t final: public system_t {
  public:
    explicit fake_system_t(std::shared_ptr<fake_t> fake):
        fake_(std::move(fake)) {}

    std::expected<void, failure_t> open() override {
      fake_->events.emplace_back("open");
      if (fake_->open_error) {
        return std::unexpected(*fake_->open_error);
      }
      return {};
    }

    std::expected<void, failure_t> close() override {
      fake_->events.emplace_back("close");
      if (fake_->close_failures) {
        --fake_->close_failures;
        return std::unexpected(failure_t {errc_t::close_failed, operation_t::close, ERROR_ACCESS_DENIED});
      }
      return {};
    }

    std::expected<std::size_t, failure_t> ioctl(DWORD code, std::span<const std::byte> input, std::span<std::byte> output) override {
      if (code == wire::version) {
        fake_->events.emplace_back("version");
      } else if (code == wire::watchdog) {
        fake_->events.emplace_back("watchdog");
      } else if (code == wire::set_adapter) {
        fake_->events.emplace_back("adapter");
      } else if (code == wire::add) {
        fake_->events.emplace_back("add");
        if (fake_->before_add) {
          fake_->before_add();
        }
      } else if (code == wire::remove) {
        fake_->events.emplace_back("remove");
      } else if (code == wire::ping) {
        fake_->events.emplace_back("ping");
      } else {
        ADD_FAILURE() << "Unexpected IOCTL";
      }
      if (const auto failure = fake_->io_errors.find(code); failure != fake_->io_errors.end()) {
        return std::unexpected(failure->second);
      }
      if (code == wire::version) {
        EXPECT_EQ(output.size(), sizeof(fake_->version));
        std::memcpy(output.data(), &fake_->version, sizeof(fake_->version));
      } else if (code == wire::watchdog) {
        EXPECT_EQ(output.size(), sizeof(fake_->watchdog));
        std::memcpy(output.data(), &fake_->watchdog, sizeof(fake_->watchdog));
      } else if (code == wire::set_adapter) {
        EXPECT_EQ(input.size(), sizeof(fake_->adapter));
        std::memcpy(&fake_->adapter, input.data(), sizeof(fake_->adapter));
      } else if (code == wire::add) {
        EXPECT_EQ(input.size(), sizeof(fake_->added));
        std::memcpy(&fake_->added, input.data(), sizeof(fake_->added));
        EXPECT_EQ(output.size(), sizeof(fake_->added_target));
        std::memcpy(output.data(), &fake_->added_target, sizeof(fake_->added_target));
      } else if (code == wire::remove) {
        EXPECT_EQ(input.size(), sizeof(GUID));
        GUID removed {};
        std::memcpy(&removed, input.data(), sizeof(removed));
        fake_->removed.push_back(removed);
        if (fake_->remove_failures) {
          --fake_->remove_failures;
          return std::unexpected(failure_t {errc_t::transport_failed, operation_t::remove, ERROR_GEN_FAILURE});
        }
      }
      const auto override_size = fake_->response_sizes.find(code);
      return override_size == fake_->response_sizes.end() ? output.size() : override_size->second;
    }

    std::expected<enumeration_t, failure_t> enumerate() override {
      fake_->events.emplace_back("enumerate");
      if (++fake_->enumerations == 1) {
        if (fake_->preflight_error) {
          return std::unexpected(failure_t {errc_t::enumeration_failed, operation_t::enumerate, ERROR_GEN_FAILURE});
        }
        return fake_->before;
      }
      if (fake_->enumeration_errors) {
        --fake_->enumeration_errors;
        return std::unexpected(failure_t {errc_t::enumeration_failed, operation_t::enumerate, ERROR_GEN_FAILURE});
      }
      if (fake_->missing_enumerations) {
        --fake_->missing_enumerations;
        return enumeration_t {};
      }
      return fake_->after;
    }

    std::expected<std::unique_ptr<heartbeat_t>, failure_t> heartbeat(std::chrono::milliseconds period, std::function<void()> callback) override {
      fake_->events.emplace_back("start");
      if (fake_->heartbeat_start_throws) {
        --fake_->heartbeat_start_throws;
        throw std::runtime_error {"inert heartbeat creation failure"};
      }
      if (fake_->heartbeat_start_failures) {
        --fake_->heartbeat_start_failures;
        return std::unexpected(failure_t {errc_t::heartbeat_failed, operation_t::heartbeat, ERROR_NOT_ENOUGH_MEMORY});
      }
      fake_->period = period;
      fake_->tick = std::move(callback);
      return std::make_unique<fake_heartbeat_t>(fake_);
    }

    void wait_for_enumeration(std::chrono::milliseconds) override {
      fake_->events.emplace_back("wait");
      fake_->fire();
    }

  private:
    std::shared_ptr<fake_t> fake_;
  };

  class SudoVdaTest: public BaseTest {
  protected:
    std::shared_ptr<fake_t> fake = std::make_shared<fake_t>();

    acquisition_t acquire(const request_t &selected_request = request()) {
      return lease_t::acquire(std::make_unique<fake_system_t>(fake), selected_request);
    }

    std::size_t count(const std::string &event) const {
      return std::ranges::count(fake->events, event);
    }

    void expect_failure(acquisition_t &result, errc_t code) {
      ASSERT_TRUE(result.failure);
      EXPECT_EQ(result.failure->code, code);
      EXPECT_FALSE(result.lease);
      EXPECT_FALSE(result.cleanup_failure);
    }
  };
}  // namespace

TEST_F(SudoVdaTest, PreservesPinnedInterfaceAndProtocolConstants) {
  const GUID expected_interface {0xe5bcc234, 0x1e0c, 0x418a, {0xa0, 0xd4, 0xef, 0x8b, 0x75, 0x01, 0x41, 0x4d}};
  EXPECT_TRUE(IsEqualGUID(wire::interface_guid, expected_interface));
  EXPECT_EQ(wire::add, 0x222000);
  EXPECT_EQ(wire::remove, 0x222004);
  EXPECT_EQ(wire::set_adapter, 0x222008);
  EXPECT_EQ(wire::watchdog, 0x22200c);
  EXPECT_EQ(wire::ping, 0x222220);
  EXPECT_EQ(wire::version, 0x2223fc);
  EXPECT_EQ(wire::client_version.major, 0);
  EXPECT_EQ(wire::client_version.minor, 2);
  EXPECT_EQ(wire::client_version.incremental, 1);
  EXPECT_EQ(wire::client_version.test_build, 1);
}

TEST_F(SudoVdaTest, RequiresExplicitExclusiveControlBeforeOpening) {
  auto selected = request();
  selected.exclusive_driver_control = false;
  auto result = acquire(selected);
  expect_failure(result, errc_t::exclusive_control_required);
  EXPECT_TRUE(fake->events.empty());
}

TEST_F(SudoVdaTest, ReportsUnavailableAndOpenErrorsWithoutIoctls) {
  for (const auto code : {errc_t::unavailable, errc_t::open_failed}) {
    fake->open_error = failure_t {code, operation_t::open, ERROR_FILE_NOT_FOUND};
    auto result = acquire();
    expect_failure(result, code);
    EXPECT_EQ(result.failure->system_error, ERROR_FILE_NOT_FOUND);
  }
  EXPECT_EQ(count("open"), 2);
  EXPECT_EQ(count("version"), 0);
  EXPECT_EQ(count("remove"), 0);
}

TEST_F(SudoVdaTest, ReportsTransportFailureAndCloses) {
  fake->io_errors[wire::version] = {errc_t::transport_failed, operation_t::version, ERROR_ACCESS_DENIED};
  auto result = acquire();
  expect_failure(result, errc_t::transport_failed);
  EXPECT_EQ(result.failure->operation, operation_t::version);
  EXPECT_EQ(result.failure->system_error, ERROR_ACCESS_DENIED);
  EXPECT_EQ(count("close"), 1);
  EXPECT_EQ(count("add"), 0);
}

TEST_F(SudoVdaTest, RejectsMajorOrTooOldMinorProtocol) {
  for (const auto version : {wire::version_t {1, 2, 1, 1}, wire::version_t {0, 1, 255, 0}}) {
    fake->version = version;
    auto result = acquire();
    expect_failure(result, errc_t::incompatible_protocol);
  }
  EXPECT_EQ(count("add"), 0);
}

TEST_F(SudoVdaTest, AcceptsMatchingMajorAndNewerMinorAndReportsTestBuild) {
  for (const auto version : {wire::version_t {0, 2, 0, 1}, wire::version_t {0, 3, 0, 0}}) {
    fake->version = version;
    auto result = acquire();
    ASSERT_FALSE(result.failure);
    ASSERT_TRUE(result.lease);
    EXPECT_EQ(result.lease->protocol().major, version.major);
    EXPECT_EQ(result.lease->protocol().minor, version.minor);
    EXPECT_EQ(result.lease->protocol().incremental, version.incremental);
    EXPECT_EQ(result.lease->protocol().test_build, version.test_build);
    ASSERT_TRUE(result.lease->cleanup());
    fake->enumerations = 0;
  }
}

TEST_F(SudoVdaTest, ChecksEveryResponseByteCountIncludingNoOutputCommands) {
  for (const auto &[code, size] : std::vector<std::pair<DWORD, std::size_t>> {{wire::version, 3}, {wire::version, 5}, {wire::watchdog, 7}, {wire::set_adapter, 1}, {wire::add, 11}}) {
    fake->response_sizes = {{code, size}};
    auto result = acquire();
    expect_failure(result, errc_t::response_size);
    EXPECT_EQ(result.failure->returned_bytes, size);
    EXPECT_EQ(count("close"), 1);
    EXPECT_EQ(count("remove"), code == wire::add ? 1 : 0);
    fake->events.clear();
    fake->enumerations = 0;
  }
}

TEST_F(SudoVdaTest, RejectsMalformedBooleanAndWatchdogMetadata) {
  fake->version.test_build = 2;
  auto version = acquire();
  expect_failure(version, errc_t::malformed_response);
  fake->version.test_build = 1;
  fake->watchdog.countdown = 4;
  auto watchdog = acquire();
  expect_failure(watchdog, errc_t::malformed_response);
  EXPECT_EQ(count("add"), 0);
}

TEST_F(SudoVdaTest, EncodesFullIdentityMetadataModeAndRenderAdapterInOrder) {
  auto selected = request();
  selected.width = 1440;
  selected.height = 2560;
  selected.rate_numerator = 2997;
  selected.rate_denominator = 50;
  selected.name = "1234567890123";
  selected.serial = "ABCDEFGHIJKLM";
  auto result = acquire(selected);
  ASSERT_FALSE(result.failure);
  EXPECT_TRUE(IsEqualGUID(fake->added.identity, identity));
  EXPECT_FALSE(IsEqualGUID(fake->added.identity, other_identity));
  EXPECT_EQ(fake->added.width, 1440);
  EXPECT_EQ(fake->added.height, 2560);
  EXPECT_EQ(fake->added.millihertz, 59940);
  EXPECT_STREQ(fake->added.name, selected.name.c_str());
  EXPECT_STREQ(fake->added.serial, selected.serial.c_str());
  EXPECT_EQ(fake->added.name[13], '\0');
  EXPECT_EQ(fake->added.serial[13], '\0');
  EXPECT_EQ(fake->adapter.HighPart, render_adapter.HighPart);
  EXPECT_EQ(fake->adapter.LowPart, render_adapter.LowPart);
  EXPECT_LT(std::ranges::find(fake->events, "adapter"), std::ranges::find(fake->events, "add"));
  EXPECT_EQ(fake->period, 1000ms);
}

TEST_F(SudoVdaTest, AcceptsVariedRatesAndRepresentableWireBoundariesWithoutWhitelist) {
  for (const auto &[numerator, denominator, expected] : std::vector<std::tuple<std::uint64_t, std::uint64_t, UINT>> {{60, 1, 60000}, {120, 1, 120000}, {165, 1, 165000}, {241, 1, 241000}, {119879, 1000, 119879}, {1, 1, 1000}, {std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max(), 1000}, {std::numeric_limits<UINT>::max(), 1000, std::numeric_limits<UINT>::max()}}) {
    auto selected = request();
    selected.width = std::numeric_limits<UINT>::max();
    selected.height = 1;
    selected.rate_numerator = numerator;
    selected.rate_denominator = denominator;
    auto result = acquire(selected);
    ASSERT_FALSE(result.failure);
    EXPECT_EQ(fake->added.millihertz, expected);
    ASSERT_TRUE(result.lease->cleanup());
    fake->enumerations = 0;
  }
}

TEST_F(SudoVdaTest, RejectsInvalidDimensionsRationalAndEnumerationBoundsBeforeOpening) {
  std::vector<request_t> cases(10, request());
  cases[0].width = 0;
  cases[1].height = 0;
  cases[2].width = std::uint64_t {std::numeric_limits<UINT>::max()} + 1;
  cases[3].height = std::uint64_t {std::numeric_limits<UINT>::max()} + 1;
  cases[4].rate_numerator = 0;
  cases[5].rate_denominator = 0;
  cases[6].enumeration_attempts = 0;
  cases[7].enumeration_attempts = 101;
  cases[8].enumeration_interval = -1ms;
  cases[9].enumeration_interval = 101ms;
  for (const auto &selected : cases) {
    auto result = acquire(selected);
    expect_failure(result, errc_t::invalid_mode);
  }
  EXPECT_TRUE(fake->events.empty());
}

TEST_F(SudoVdaTest, ReportsUnrepresentablePrecisionAndDriverUnitOrOverflowLimits) {
  auto selected = request();
  selected.rate_numerator = 60000;
  selected.rate_denominator = 1001;
  auto precision = acquire(selected);
  expect_failure(precision, errc_t::rate_precision);
  for (const auto &[numerator, denominator] : std::vector<std::pair<std::uint64_t, std::uint64_t>> {{1, 2}, {std::uint64_t {std::numeric_limits<UINT>::max()} + 1, 1000}, {std::numeric_limits<std::uint64_t>::max(), 1}}) {
    selected.rate_numerator = numerator;
    selected.rate_denominator = denominator;
    auto result = acquire(selected);
    expect_failure(result, errc_t::unsupported_rate);
  }
  EXPECT_TRUE(fake->events.empty());
}

TEST_F(SudoVdaTest, RejectsTruncationEmbeddedNulNonAsciiEmptyAndMissingIdentity) {
  std::vector<request_t> cases(7, request());
  cases[0].name = std::string(14, 'a');
  cases[1].serial = std::string(14, 'a');
  cases[2].name = std::string("a\0b", 3);
  cases[3].serial = std::string(1, '\xff');
  cases[4].name.clear();
  cases[5].identity = {};
  cases[6].serial.clear();
  for (const auto &selected : cases) {
    auto result = acquire(selected);
    expect_failure(result, errc_t::invalid_metadata);
  }
  EXPECT_TRUE(fake->events.empty());
}

TEST_F(SudoVdaTest, RejectsPresentIdentityWithoutAddingReconfiguringOrRemovingIt) {
  fake->before.identities = {other_identity, identity};
  auto result = acquire();
  expect_failure(result, errc_t::already_present);
  EXPECT_EQ(count("adapter"), 0);
  EXPECT_EQ(count("add"), 0);
  EXPECT_EQ(count("remove"), 0);
  EXPECT_EQ(count("close"), 1);
}

TEST_F(SudoVdaTest, FailsClosedOnPreflightEnumerationError) {
  fake->preflight_error = true;
  auto result = acquire();
  expect_failure(result, errc_t::enumeration_failed);
  EXPECT_EQ(count("add"), 0);
  EXPECT_EQ(count("remove"), 0);
}

TEST_F(SudoVdaTest, MatchesAdapterAndTargetAndContainerBeforeResolving) {
  auto other_adapter = target;
  ++other_adapter.adapter.HighPart;
  auto other_target = target;
  ++other_target.target;
  fake->after.displays = {display(other_adapter), display(other_target), display(target, other_identity), display()};
  fake->after.displays.front().gdi_name = L"wrong-adapter";
  auto result = acquire();
  ASSERT_FALSE(result.failure);
  ASSERT_TRUE(result.lease->display());
  EXPECT_EQ(result.lease->display()->gdi_name, L"fake-gdi-name");
  EXPECT_EQ(result.lease->display()->target.adapter.HighPart, target.adapter.HighPart);
  EXPECT_EQ(result.lease->display()->target.adapter.LowPart, target.adapter.LowPart);
  EXPECT_EQ(result.lease->display()->target.target, target.target);
}

TEST_F(SudoVdaTest, NeverFallsBackToAnotherAdapterOrPhysicalDisplay) {
  auto wrong_adapter = target;
  ++wrong_adapter.adapter.LowPart;
  fake->after.displays = {display(wrong_adapter), display(target, other_identity)};
  auto result = acquire();
  expect_failure(result, errc_t::enumeration_timeout);
  ASSERT_EQ(fake->removed.size(), 1);
  EXPECT_TRUE(IsEqualGUID(fake->removed.front(), identity));
  EXPECT_EQ(fake->enumerations, 4);
  EXPECT_EQ(count("wait"), 2);
}

TEST_F(SudoVdaTest, RetriesTransientEnumerationAndKeepsHeartbeatRunningDeterministically) {
  fake->enumeration_errors = 1;
  fake->missing_enumerations = 1;
  auto result = acquire();
  ASSERT_FALSE(result.failure);
  EXPECT_EQ(fake->enumerations, 4);
  EXPECT_EQ(count("wait"), 2);
  EXPECT_EQ(count("ping"), 2);
  EXPECT_EQ(count("remove"), 0);
}

TEST_F(SudoVdaTest, RollsBackExactGuidOnBoundedEnumerationFailure) {
  fake->enumeration_errors = 3;
  auto result = acquire();
  expect_failure(result, errc_t::enumeration_failed);
  ASSERT_EQ(fake->removed.size(), 1);
  EXPECT_TRUE(IsEqualGUID(fake->removed.front(), identity));
  EXPECT_LT(std::ranges::find(fake->events, "stop-joined"), std::ranges::find(fake->events, "remove"));
  EXPECT_LT(std::ranges::find(fake->events, "remove"), std::ranges::find(fake->events, "close"));
}

TEST_F(SudoVdaTest, RetainsBothAcquisitionAndRollbackFailureForExplicitRetry) {
  fake->after.displays.clear();
  fake->remove_failures = 1;
  auto result = acquire();
  ASSERT_TRUE(result.failure);
  EXPECT_EQ(result.failure->code, errc_t::enumeration_timeout);
  ASSERT_TRUE(result.cleanup_failure);
  EXPECT_EQ(result.cleanup_failure->code, errc_t::remove_failed);
  ASSERT_TRUE(result.lease);
  EXPECT_FALSE(result.lease->display());
  EXPECT_EQ(count("close"), 0);
  EXPECT_EQ(count("start"), 2);
  fake->fire();
  EXPECT_EQ(count("ping"), 3);
  ASSERT_TRUE(result.lease->cleanup());
  EXPECT_EQ(count("remove"), 2);
  EXPECT_EQ(count("close"), 1);
}

TEST_F(SudoVdaTest, ProtectsProcessOwnershipUntilCleanupSucceeds) {
  auto first = acquire();
  ASSERT_FALSE(first.failure);
  auto blocked = acquire();
  expect_failure(blocked, errc_t::busy);
  EXPECT_EQ(count("open"), 1);
  fake->remove_failures = 1;
  EXPECT_FALSE(first.lease->cleanup());
  auto still_blocked = acquire();
  expect_failure(still_blocked, errc_t::busy);
  ASSERT_TRUE(first.lease->cleanup());
  fake->enumerations = 0;
  auto next = acquire();
  EXPECT_FALSE(next.failure);
}

TEST_F(SudoVdaTest, MovePreservesHeartbeatOwnershipAndCleanupIsIdempotent) {
  auto result = acquire();
  ASSERT_FALSE(result.failure);
  lease_t moved {std::move(*result.lease)};
  ASSERT_TRUE(result.lease->cleanup());
  EXPECT_FALSE(result.lease->display());
  ASSERT_TRUE(moved.display());
  fake->fire();
  EXPECT_EQ(count("ping"), 1);
  ASSERT_TRUE(moved.cleanup());
  ASSERT_TRUE(moved.cleanup());
  EXPECT_EQ(count("remove"), 1);
  EXPECT_EQ(count("close"), 1);
  fake->fire();
  EXPECT_EQ(count("ping"), 1);
}

TEST_F(SudoVdaTest, JoinsInFlightHeartbeatBeforeRemovalAndHandleClose) {
  auto result = acquire();
  ASSERT_FALSE(result.failure);
  fake->events.clear();
  fake->ping_during_stop = true;
  ASSERT_TRUE(result.lease->cleanup());
  EXPECT_EQ(fake->events, (std::vector<std::string> {"stop-request", "ping", "stop-joined", "remove", "close"}));
  EXPECT_FALSE(fake->tick);
}

TEST_F(SudoVdaTest, ReportsStickyHeartbeatFailureWithoutRemovingOtherDisplays) {
  fake->before.identities = {other_identity};
  fake->after.displays.push_back(display(target, other_identity));
  auto result = acquire();
  ASSERT_FALSE(result.failure);
  fake->io_errors[wire::ping] = {errc_t::transport_failed, operation_t::heartbeat, ERROR_DEVICE_NOT_CONNECTED};
  fake->fire();
  const auto failure = result.lease->heartbeat_failure();
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->code, errc_t::heartbeat_failed);
  EXPECT_EQ(failure->system_error, ERROR_DEVICE_NOT_CONNECTED);
  fake->io_errors.clear();
  fake->fire();
  EXPECT_TRUE(result.lease->heartbeat_failure());
  EXPECT_TRUE(fake->removed.empty());
  ASSERT_TRUE(result.lease->cleanup());
  ASSERT_EQ(fake->removed.size(), 1);
  EXPECT_TRUE(IsEqualGUID(fake->removed.front(), identity));
}

TEST_F(SudoVdaTest, PingResponseSizeFailureIsVisible) {
  auto result = acquire();
  ASSERT_FALSE(result.failure);
  fake->response_sizes[wire::ping] = 1;
  fake->fire();
  ASSERT_TRUE(result.lease->heartbeat_failure());
  EXPECT_EQ(result.lease->heartbeat_failure()->returned_bytes, 1);
}

TEST_F(SudoVdaTest, WatchdogDisabledNeedsNoHeartbeatAndMinimumSupportedTimeoutSchedulesPings) {
  fake->watchdog = {0, 0};
  auto disabled = acquire();
  ASSERT_FALSE(disabled.failure);
  EXPECT_EQ(count("start"), 0);
  ASSERT_TRUE(disabled.lease->cleanup());
  fake->watchdog = {2, 2};
  fake->enumerations = 0;
  auto short_timeout = acquire();
  ASSERT_FALSE(short_timeout.failure);
  EXPECT_EQ(fake->period, 666ms);
}

TEST_F(SudoVdaTest, RejectsOneSecondWatchdogBeforeAnyDisplayMutation) {
  fake->watchdog = {1, 1};
  auto result = acquire();
  expect_failure(result, errc_t::unsupported_watchdog);
  EXPECT_EQ(result.failure->operation, operation_t::watchdog);
  EXPECT_EQ(count("enumerate"), 0);
  EXPECT_EQ(count("adapter"), 0);
  EXPECT_EQ(count("start"), 0);
  EXPECT_EQ(count("add"), 0);
  EXPECT_EQ(count("remove"), 0);
  EXPECT_EQ(count("close"), 1);
}

TEST_F(SudoVdaTest, DoesNotAddWithoutHeartbeatAndRollsBackFailedAddAttempt) {
  fake->heartbeat_start_failures = 1;
  auto heartbeat = acquire();
  expect_failure(heartbeat, errc_t::heartbeat_failed);
  EXPECT_EQ(count("add"), 0);
  EXPECT_EQ(count("remove"), 0);
  fake->enumerations = 0;
  fake->io_errors[wire::add] = {errc_t::transport_failed, operation_t::add, ERROR_TIMEOUT};
  auto add = acquire();
  expect_failure(add, errc_t::transport_failed);
  EXPECT_EQ(count("remove"), 1);
}

TEST_F(SudoVdaTest, FailedRemovalRestartsHeartbeatAndPreservesLeaseForRetry) {
  auto result = acquire();
  ASSERT_FALSE(result.failure);
  fake->remove_failures = 1;
  auto cleanup = result.lease->cleanup();
  ASSERT_FALSE(cleanup);
  EXPECT_EQ(cleanup.error().code, errc_t::remove_failed);
  EXPECT_EQ(count("close"), 0);
  EXPECT_EQ(count("start"), 2);
  EXPECT_TRUE(result.lease->display());
  fake->fire();
  EXPECT_EQ(count("ping"), 1);
  ASSERT_TRUE(result.lease->cleanup());
}

TEST_F(SudoVdaTest, FailedRecoveryHeartbeatIsReportedAndRemovalRemainsRetryable) {
  auto result = acquire();
  ASSERT_FALSE(result.failure);
  fake->remove_failures = 1;
  fake->heartbeat_start_failures = 1;
  EXPECT_FALSE(result.lease->cleanup());
  ASSERT_TRUE(result.lease->heartbeat_failure());
  EXPECT_EQ(result.lease->heartbeat_failure()->system_error, ERROR_NOT_ENOUGH_MEMORY);
  ASSERT_TRUE(result.lease->cleanup());
}

TEST_F(SudoVdaTest, AlreadyAbsentOwnedGuidCompletesCleanupWithoutGlobalRecovery) {
  auto result = acquire();
  ASSERT_FALSE(result.failure);
  fake->io_errors[wire::remove] = {errc_t::transport_failed, operation_t::remove, ERROR_NOT_FOUND};
  ASSERT_TRUE(result.lease->cleanup());
  EXPECT_EQ(count("close"), 1);
  EXPECT_EQ(count("remove"), 1);
}

TEST_F(SudoVdaTest, CloseFailureRetainsHandleWithoutRemovingAgain) {
  auto result = acquire();
  ASSERT_FALSE(result.failure);
  fake->close_failures = 1;
  auto cleanup = result.lease->cleanup();
  ASSERT_FALSE(cleanup);
  EXPECT_EQ(cleanup.error().code, errc_t::close_failed);
  EXPECT_FALSE(result.lease->display());
  ASSERT_TRUE(result.lease->cleanup());
  EXPECT_EQ(count("remove"), 1);
  EXPECT_EQ(count("close"), 2);
}

TEST_F(SudoVdaTest, ReportsRollbackCloseFailureSeparatelyAndAllowsRetry) {
  fake->version.major = 1;
  fake->close_failures = 1;
  auto result = acquire();
  ASSERT_TRUE(result.failure);
  EXPECT_EQ(result.failure->code, errc_t::incompatible_protocol);
  ASSERT_TRUE(result.cleanup_failure);
  EXPECT_EQ(result.cleanup_failure->code, errc_t::close_failed);
  ASSERT_TRUE(result.lease);
  ASSERT_TRUE(result.lease->cleanup());
  EXPECT_EQ(count("remove"), 0);
}

TEST_F(SudoVdaTest, DestructionStopsHeartbeatRemovesOwnedGuidAndCloses) {
  {
    auto result = acquire();
    ASSERT_FALSE(result.failure);
    fake->events.clear();
  }
  EXPECT_EQ(fake->events, (std::vector<std::string> {"stop-request", "stop-joined", "remove", "close"}));
  EXPECT_FALSE(fake->tick);
}

TEST_F(SudoVdaTest, FailedRenderAdapterSelectionNeverAddsDisplay) {
  fake->io_errors[wire::set_adapter] = {errc_t::transport_failed, operation_t::render_adapter, ERROR_INVALID_PARAMETER};
  auto result = acquire();
  expect_failure(result, errc_t::transport_failed);
  EXPECT_EQ(result.failure->operation, operation_t::render_adapter);
  EXPECT_EQ(count("add"), 0);
  EXPECT_EQ(count("start"), 0);
  EXPECT_EQ(count("remove"), 0);
  EXPECT_EQ(count("close"), 1);
}

TEST_F(SudoVdaTest, MissingOutputNamesCannotProduceSuccessfulAcquisition) {
  fake->after.displays.front().gdi_name.clear();
  auto result = acquire();
  expect_failure(result, errc_t::enumeration_timeout);
  EXPECT_EQ(count("remove"), 1);
}

TEST_F(SudoVdaTest, MalformedRemoveResponseRemainsRecoverable) {
  auto result = acquire();
  ASSERT_FALSE(result.failure);
  fake->response_sizes[wire::remove] = 1;
  auto cleanup = result.lease->cleanup();
  ASSERT_FALSE(cleanup);
  EXPECT_EQ(cleanup.error().code, errc_t::remove_failed);
  EXPECT_EQ(cleanup.error().returned_bytes, 1);
  EXPECT_EQ(count("close"), 0);
  fake->response_sizes.clear();
  fake->io_errors[wire::remove] = {errc_t::transport_failed, operation_t::remove, ERROR_NOT_FOUND};
  ASSERT_TRUE(result.lease->cleanup());
}

TEST_F(SudoVdaTest, NullSystemFailsBeforeAnyDriverOperation) {
  auto result = lease_t::acquire(nullptr, request());
  expect_failure(result, errc_t::open_failed);
  EXPECT_TRUE(fake->events.empty());
}

namespace {
  class SudoVdaOwnershipTest: public SudoVdaTest {
  protected:
    std::filesystem::path path;
    const recovery_authority_t authority {"fixture-deployment", true, true};

    void SetUp() override {
      SudoVdaTest::SetUp();
      path = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / (std::string {::testing::UnitTest::GetInstance()->current_test_info()->name()} + ".ownership.json");
      std::error_code error;
      std::filesystem::remove(path, error);
    }

    void TearDown() override {
      http::test_support::set_state_replace_hook({});
      std::error_code error;
      std::filesystem::remove(path, error);
      SudoVdaTest::TearDown();
    }

    std::shared_ptr<ownership_journal_t> make_journal() {
      const auto result {ownership_journal_t::create(path, {identity, 41, authority.deployment, true, {}}, authority)};
      EXPECT_TRUE(result);
      return result ? *result : nullptr;
    }

    nlohmann::json read_envelope() {
      nlohmann::json data;
      EXPECT_TRUE(http::update_state_file_bytes(path.string(), [&](const std::string &bytes) -> std::optional<std::string> {
        data = nlohmann::json::parse(bytes);
        return std::nullopt;
      }));
      EXPECT_TRUE(data.is_object());
      return data;
    }

    std::shared_ptr<ownership_journal_t> crash_journal() {
      // Synthetic crash fixture: an exclusive preflight proved absence and durably armed add.
      auto original {make_journal()};
      EXPECT_TRUE(http::update_state_file_bytes(path.string(), [](std::string bytes) -> std::optional<std::string> {
        const auto position {bytes.find("\"add_armed\":false")};
        if (position == std::string::npos) {
          return std::nullopt;
        }
        bytes.replace(position, std::string {"\"add_armed\":false"}.size(), "\"add_armed\":true");
        return bytes;
      }));
      const auto loaded {ownership_journal_t::load(path, authority)};
      EXPECT_TRUE(loaded);
      return loaded ? *loaded : nullptr;
    }
  };
}  // namespace

TEST_F(SudoVdaOwnershipTest, RequiresIndependentTrustedAuthorityBeforeCreatingIntent) {
  auto untrusted {authority};
  untrusted.trusted_storage = false;
  EXPECT_FALSE(ownership_journal_t::create(path, {identity, 41, authority.deployment, true, {}}, untrusted));
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_TRUE(fake->events.empty());
}

TEST_F(SudoVdaOwnershipTest, ExpectedByteLoadComparesInsideExistingTransactionWithoutRewriting) {
  ASSERT_TRUE(make_journal());
  std::string original;
  ASSERT_TRUE(http::update_state_file_bytes(path.string(), [&](const std::string &bytes) -> std::optional<std::string> {
    original = bytes;
    return std::nullopt;
  }));
  EXPECT_TRUE(ownership_journal_t::load(path, authority, original));
  ASSERT_TRUE(http::update_state_file_bytes(path.string(), [](const std::string &bytes) -> std::optional<std::string> {
    return bytes + " ";
  }));
  EXPECT_FALSE(ownership_journal_t::load(path, authority, original));
  EXPECT_TRUE(ownership_journal_t::load(path, authority));
  EXPECT_TRUE(fake->events.empty());
}

TEST_F(SudoVdaOwnershipTest, UnarmedRecoveryIssuesActualReceiptWithoutTransportOrDummyMode) {
  const auto journal = make_journal();
  ASSERT_TRUE(journal);
  const auto proof = lease_t::recover_unarmed(journal, authority);
  ASSERT_TRUE(proof);
  EXPECT_EQ(proof->kind(), cleanup_receipt_t::kind_e::unarmed);
  EXPECT_TRUE(journal->accepts_receipt(*proof));
  EXPECT_TRUE(fake->events.empty());
  EXPECT_TRUE(journal->retire(*proof));
  EXPECT_FALSE(lease_t::recover_unarmed(journal, authority));
}

TEST_F(SudoVdaOwnershipTest, UnarmedRecoveryRejectsArmedChangedOrWrongDeploymentEvidence) {
  auto journal = make_journal();
  ASSERT_TRUE(journal);
  auto other = authority;
  other.deployment = "another-deployment";
  EXPECT_FALSE(lease_t::recover_unarmed(journal, other));
  ASSERT_TRUE(http::update_state_file_bytes(path.string(), [](const std::string &bytes) -> std::optional<std::string> {
    return bytes + " ";
  }));
  EXPECT_FALSE(lease_t::recover_unarmed(journal, authority));
  const auto unchanged = ownership_journal_t::load(path, authority);
  ASSERT_TRUE(unchanged);
  const auto proof = lease_t::recover_unarmed(*unchanged, authority);
  ASSERT_TRUE(proof);
  EXPECT_FALSE(journal->accepts_receipt(*proof));
  ASSERT_TRUE((*unchanged)->retire(*proof));
  ASSERT_TRUE((*unchanged)->complete());
  journal = crash_journal();
  ASSERT_TRUE(journal);
  EXPECT_FALSE(lease_t::recover_unarmed(journal, authority));
  EXPECT_TRUE(fake->events.empty());
}

TEST_F(SudoVdaOwnershipTest, JournalMutationsPersistObjectEnvelopeAcrossReloadAndRollover) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  auto envelope = read_envelope();
  ASSERT_TRUE(envelope.is_object());
  ASSERT_EQ(envelope.size(), 13);
  const auto encoded_identity = envelope.at("identity");
  EXPECT_EQ(envelope.at("schema"), 2);
  EXPECT_EQ(envelope.at("generation"), 41);
  EXPECT_FALSE(envelope.at("add_armed").get<bool>());

  ASSERT_TRUE(journal->observe_devices({"fixture-physical-id"}));
  envelope = read_envelope();
  ASSERT_TRUE(envelope.is_object());
  EXPECT_EQ(envelope.at("identity"), encoded_identity);
  EXPECT_EQ(envelope.at("recovery_ids").get<std::vector<std::string>>(), (std::vector<std::string> {"fixture-physical-id"}));

  auto result {lease_t::acquire_owned(std::make_unique<fake_system_t>(fake), request(), journal, authority)};
  ASSERT_FALSE(result.failure);
  ASSERT_TRUE(result.lease);
  ASSERT_TRUE(result.lease->display());
  ASSERT_TRUE(journal->bind_mapping(*result.lease->display(), "fixture-owned-id"));
  const std::vector<std::uint8_t> saved {'r', 0, 's'};
  ASSERT_TRUE(journal->store_settings(saved));
  envelope = read_envelope();
  ASSERT_TRUE(envelope.is_object());
  EXPECT_TRUE(envelope.at("add_armed").get<bool>());
  ASSERT_TRUE(envelope.at("mapping").is_object());
  EXPECT_EQ(envelope.at("mapping").at("identity"), encoded_identity);
  EXPECT_EQ(envelope.at("mapping").at("generation"), 41);
  EXPECT_EQ(envelope.at("mapping").at("device_id"), "fixture-owned-id");
  EXPECT_EQ(envelope.at("settings").get<std::string>(), (std::string {saved.begin(), saved.end()}));
  EXPECT_EQ(envelope.at("restoration_state"), envelope.at("settings"));
  const auto loaded {ownership_journal_t::load(path, authority)};
  ASSERT_TRUE(loaded);
  ASSERT_TRUE((*loaded)->settings());
  EXPECT_EQ(*(*loaded)->settings(), saved);
  ASSERT_TRUE((*loaded)->mapping());
  EXPECT_EQ((*loaded)->mapping()->device_id, "fixture-owned-id");

  ASSERT_TRUE(result.lease->cleanup());
  ASSERT_TRUE(result.lease->cleanup_receipt());
  ASSERT_TRUE(journal->retire(*result.lease->cleanup_receipt()));
  envelope = read_envelope();
  ASSERT_TRUE(envelope.is_object());
  EXPECT_TRUE(envelope.at("source_retired").get<bool>());
  const auto next {journal->begin_next({other_identity, 42, authority.deployment, true, {}}, authority, {"fixture-next-id"})};
  ASSERT_TRUE(next);
  envelope = read_envelope();
  ASSERT_TRUE(envelope.is_object());
  EXPECT_EQ(envelope.at("generation"), 42);
  EXPECT_FALSE(envelope.at("add_armed").get<bool>());
  EXPECT_FALSE(envelope.at("source_retired").get<bool>());
  EXPECT_TRUE(envelope.at("mapping").is_null());
  EXPECT_EQ(envelope.at("settings").get<std::string>(), (std::string {saved.begin(), saved.end()}));
  EXPECT_EQ(envelope.at("restoration_state"), envelope.at("settings"));
  const auto retained {(*next)->recovery_devices()};
  ASSERT_TRUE(retained);
  EXPECT_EQ(retained->size(), 3);
  for (const auto *device : {"fixture-physical-id", "fixture-owned-id", "fixture-next-id"}) {
    EXPECT_NE(std::ranges::find(*retained, device), retained->end());
  }
  EXPECT_FALSE(journal->validate(authority));
  EXPECT_TRUE((*next)->validate(authority));
}

TEST_F(SudoVdaOwnershipTest, FailedMetadataWritePreservesCachedAndPersistedObjectEnvelope) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  auto result {lease_t::acquire_owned(std::make_unique<fake_system_t>(fake), request(), journal, authority)};
  ASSERT_FALSE(result.failure);
  ASSERT_TRUE(result.lease);
  ASSERT_TRUE(result.lease->display());
  const auto before = read_envelope();
  ASSERT_TRUE(before.is_object());
  http::test_support::set_state_replace_hook([](const auto &, const auto &, const auto &) {
    return std::make_optional<std::uint32_t>(ERROR_ACCESS_DENIED);
  });
  EXPECT_FALSE(journal->observe_devices({"fixture-physical-id"}));
  EXPECT_FALSE(journal->bind_mapping(*result.lease->display(), "fixture-owned-id"));
  http::test_support::set_state_replace_hook({});
  EXPECT_EQ(read_envelope(), before);
  EXPECT_TRUE(journal->validate(authority));
  EXPECT_FALSE(journal->mapping());
  ASSERT_TRUE(journal->recovery_devices());
  EXPECT_TRUE(journal->recovery_devices()->empty());
  const auto loaded {ownership_journal_t::load(path, authority)};
  ASSERT_TRUE(loaded);
  EXPECT_FALSE((*loaded)->mapping());
  ASSERT_TRUE((*loaded)->recovery_devices());
  EXPECT_TRUE((*loaded)->recovery_devices()->empty());
  EXPECT_TRUE(result.lease->cleanup());
}

TEST_F(SudoVdaOwnershipTest, UnarmedIntentCannotDeleteUnknownExistingGuid) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  fake->before.identities = {identity};
  auto acquired {lease_t::acquire_owned(std::make_unique<fake_system_t>(fake), request(), journal, authority)};
  expect_failure(acquired, errc_t::already_present);
  const auto loaded {ownership_journal_t::load(path, authority)};
  ASSERT_TRUE(loaded);
  fake->events.clear();
  auto recovered {lease_t::recover(std::make_unique<fake_system_t>(fake), *loaded, authority)};
  expect_failure(recovered, errc_t::ownership_unverified);
  EXPECT_TRUE(fake->events.empty());
  EXPECT_TRUE(fake->removed.empty());
}

TEST_F(SudoVdaOwnershipTest, ArmsDurableIntentBeforeAddAndRetainsItUntilRemovalCompletes) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  fake->before_add = [&] {
    EXPECT_TRUE(http::update_state_file_bytes(path.string(), [](const std::string &bytes) -> std::optional<std::string> {
      EXPECT_NE(bytes.find("\"add_armed\":true"), std::string::npos);
      return std::nullopt;
    }));
  };
  auto result {lease_t::acquire_owned(std::make_unique<fake_system_t>(fake), request(), journal, authority)};
  ASSERT_FALSE(result.failure);
  ASSERT_TRUE(result.lease);
  EXPECT_FALSE(journal->complete());
  ASSERT_TRUE(result.lease->cleanup());
  ASSERT_TRUE(result.lease->cleanup_receipt());
  ASSERT_TRUE(journal->retire(*result.lease->cleanup_receipt()));
  EXPECT_TRUE(journal->complete());
}

TEST_F(SudoVdaOwnershipTest, FailedAtomicArmingNeverAddsOrRemovesMonitor) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  http::test_support::set_state_replace_hook([](const auto &, const auto &, const auto &) {
    return std::make_optional<std::uint32_t>(ERROR_ACCESS_DENIED);
  });
  auto result {lease_t::acquire_owned(std::make_unique<fake_system_t>(fake), request(), journal, authority)};
  expect_failure(result, errc_t::journal_failed);
  http::test_support::set_state_replace_hook({});
  EXPECT_TRUE(journal->validate(authority));
  EXPECT_FALSE(journal->add_was_armed());
  const auto envelope = read_envelope();
  ASSERT_TRUE(envelope.is_object());
  EXPECT_FALSE(envelope.at("add_armed").get<bool>());
  EXPECT_EQ(count("add"), 0);
  EXPECT_EQ(count("remove"), 0);
}

TEST_F(SudoVdaOwnershipTest, FailedAtomicSettingsWritePreservesPreviousRecoveryBytes) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  ASSERT_TRUE(journal->store_settings({1, 0, 2}));
  const auto before = read_envelope();
  ASSERT_TRUE(before.is_object());
  http::test_support::set_state_replace_hook([](const auto &, const auto &, const auto &) {
    return std::make_optional<std::uint32_t>(ERROR_ACCESS_DENIED);
  });
  EXPECT_FALSE(journal->store_settings({9}));
  http::test_support::set_state_replace_hook({});
  EXPECT_EQ(read_envelope(), before);
  ASSERT_TRUE(journal->settings());
  EXPECT_EQ(*journal->settings(), (std::vector<std::uint8_t> {1, 0, 2}));
  EXPECT_FALSE(journal->complete());
}

TEST_F(SudoVdaOwnershipTest, OutstandingOrMalformedIntentBlocksReplacementAndRecovery) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  EXPECT_FALSE(ownership_journal_t::create(path, {other_identity, 42, authority.deployment, true, {}}, authority));
  ASSERT_TRUE(http::update_state_file_bytes(path.string(), [](const auto &) {
    return std::make_optional(std::string {"broken"});
  }));
  EXPECT_FALSE(ownership_journal_t::load(path, authority));
  auto result {lease_t::acquire_owned(std::make_unique<fake_system_t>(fake), request(), journal, authority)};
  expect_failure(result, errc_t::ownership_unverified);
  EXPECT_TRUE(fake->events.empty());
}

TEST_F(SudoVdaOwnershipTest, RecoveryChecksDeploymentAndExclusiveEvidenceBeforeDriverAccess) {
  const auto journal {crash_journal()};
  ASSERT_TRUE(journal);
  for (auto wrong : {recovery_authority_t {"another-deployment", true, true}, recovery_authority_t {authority.deployment, true, false}, recovery_authority_t {authority.deployment, false, true}}) {
    auto result {lease_t::recover(std::make_unique<fake_system_t>(fake), journal, wrong)};
    expect_failure(result, errc_t::ownership_unverified);
  }
  EXPECT_TRUE(fake->events.empty());
}

TEST_F(SudoVdaOwnershipTest, RecoveryAdoptsOnlyJournalGuidWithoutAddingOrSelectingAdapter) {
  const auto journal {crash_journal()};
  ASSERT_TRUE(journal);
  fake->before = {{identity, other_identity}, {display(), display(target, other_identity)}};
  auto result {lease_t::recover(std::make_unique<fake_system_t>(fake), journal, authority)};
  ASSERT_FALSE(result.failure);
  ASSERT_TRUE(result.lease);
  EXPECT_TRUE(fake->tick);
  EXPECT_FALSE(result.lease->identity_absent());
  EXPECT_TRUE(result.lease->display());
  EXPECT_EQ(count("add"), 0);
  EXPECT_EQ(count("adapter"), 0);
  ASSERT_TRUE(result.lease->cleanup());
  ASSERT_EQ(fake->removed.size(), 1);
  EXPECT_TRUE(IsEqualGUID(fake->removed.front(), identity));
  ASSERT_TRUE(result.lease->cleanup_receipt());
  ASSERT_TRUE(journal->retire(*result.lease->cleanup_receipt()));
  EXPECT_TRUE(journal->complete());
}

TEST_F(SudoVdaOwnershipTest, RecoveryOfAbsentOwnedGuidStillPerformsExactRemovalAndClose) {
  const auto journal {crash_journal()};
  ASSERT_TRUE(journal);
  auto result {lease_t::recover(std::make_unique<fake_system_t>(fake), journal, authority)};
  ASSERT_FALSE(result.failure);
  ASSERT_TRUE(result.lease);
  EXPECT_TRUE(result.lease->identity_absent());
  fake->io_errors[wire::remove] = {errc_t::transport_failed, operation_t::remove, ERROR_NOT_FOUND};
  ASSERT_TRUE(result.lease->cleanup());
  ASSERT_TRUE(result.lease->cleanup_receipt());
  ASSERT_TRUE(journal->retire(*result.lease->cleanup_receipt()));
  EXPECT_TRUE(journal->complete());
}

TEST_F(SudoVdaOwnershipTest, ThrowingRecoveryHeartbeatCreationLatchesOnTheActualRetainedLease) {
  const auto journal = crash_journal();
  ASSERT_TRUE(journal);
  fake->heartbeat_start_throws = 2;
  auto result = lease_t::recover(std::make_unique<fake_system_t>(fake), journal, authority);
  ASSERT_TRUE(result.lease);
  ASSERT_TRUE(result.failure);
  ASSERT_TRUE(result.liveness_failure);
  ASSERT_TRUE(result.lease->heartbeat_failure());
  EXPECT_EQ(result.lease->heartbeat_failure()->code, errc_t::heartbeat_failed);
  EXPECT_EQ(result.lease->heartbeat_failure()->operation, operation_t::heartbeat);
  EXPECT_EQ(count("start"), 2u);
  EXPECT_FALSE(fake->tick);
  EXPECT_TRUE(result.output_uncertain);
  EXPECT_TRUE(result.lease->belongs_to(journal));
  EXPECT_TRUE(journal->validate(authority));
  EXPECT_EQ(count("remove"), 0u);
  EXPECT_EQ(count("close"), 0u);
}

TEST_F(SudoVdaOwnershipTest, FailedRecoveryRemoveRetainsJournalLeaseAndHeartbeatForRetry) {
  const auto journal {crash_journal()};
  ASSERT_TRUE(journal);
  fake->before = {{identity}, {display()}};
  auto result {lease_t::recover(std::make_unique<fake_system_t>(fake), journal, authority)};
  ASSERT_TRUE(result.lease);
  fake->remove_failures = 1;
  EXPECT_FALSE(result.lease->cleanup());
  EXPECT_FALSE(journal->complete());
  EXPECT_TRUE(journal->validate(authority));
  EXPECT_TRUE(fake->tick);
  ASSERT_TRUE(result.lease->cleanup());
  ASSERT_TRUE(result.lease->cleanup_receipt());
  ASSERT_TRUE(journal->retire(*result.lease->cleanup_receipt()));
  EXPECT_TRUE(journal->complete());
}

TEST_F(SudoVdaOwnershipTest, RecoveryHeartbeatFailureRetainsOwnershipWithoutPrematureRemoval) {
  const auto journal {crash_journal()};
  ASSERT_TRUE(journal);
  fake->heartbeat_start_failures = 1;
  auto result {lease_t::recover(std::make_unique<fake_system_t>(fake), journal, authority)};
  ASSERT_TRUE(result.failure);
  EXPECT_EQ(result.failure->code, errc_t::heartbeat_failed);
  ASSERT_TRUE(result.lease);
  EXPECT_EQ(count("remove"), 0);
  EXPECT_TRUE(journal->validate(authority));
  EXPECT_TRUE(result.lease->heartbeat_failure());
  ASSERT_TRUE(result.lease->cleanup());
}

TEST_F(SudoVdaOwnershipTest, ArmedAddFailureRetainsLeaseAndHeartbeatUntilOrderedCleanup) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  fake->io_errors[wire::add] = {errc_t::transport_failed, operation_t::add, ERROR_TIMEOUT};
  fake->after = {};
  auto result {lease_t::acquire_owned(std::make_unique<fake_system_t>(fake), request(), journal, authority)};
  ASSERT_TRUE(result.failure);
  EXPECT_EQ(result.failure->operation, operation_t::add);
  ASSERT_TRUE(result.lease);
  EXPECT_TRUE(result.output_uncertain);
  EXPECT_EQ(count("remove"), 0);
  EXPECT_TRUE(fake->tick);
  ASSERT_TRUE(result.lease->refresh_display(1, 0ms));
  EXPECT_TRUE(result.lease->identity_absent());
  ASSERT_TRUE(result.lease->cleanup());
  ASSERT_TRUE(result.lease->cleanup_receipt());
  EXPECT_EQ(result.lease->cleanup_receipt()->kind(), cleanup_receipt_t::kind_e::removed);
}

TEST_F(SudoVdaOwnershipTest, UnstartedRetainedHeartbeatReportsSeparateLivenessFailure) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  fake->heartbeat_start_failures = 2;
  fake->after = {};
  auto result {lease_t::acquire_owned(std::make_unique<fake_system_t>(fake), request(), journal, authority)};
  ASSERT_TRUE(result.failure);
  EXPECT_EQ(result.failure->code, errc_t::heartbeat_failed);
  ASSERT_TRUE(result.lease);
  ASSERT_TRUE(result.liveness_failure);
  EXPECT_TRUE(result.output_uncertain);
  EXPECT_FALSE(fake->tick);
  EXPECT_EQ(count("add"), 0);
  EXPECT_EQ(count("remove"), 0);
  ASSERT_TRUE(result.lease->refresh_display(1, 0ms));
  EXPECT_TRUE(result.lease->identity_absent());
  ASSERT_TRUE(result.lease->cleanup());
}

TEST_F(SudoVdaOwnershipTest, RemovalReceiptIsUnavailableUntilHandleCloseSucceeds) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  auto result {lease_t::acquire_owned(std::make_unique<fake_system_t>(fake), request(), journal, authority)};
  ASSERT_TRUE(result.lease);
  fake->close_failures = 1;
  EXPECT_FALSE(result.lease->cleanup());
  EXPECT_FALSE(result.lease->cleanup_receipt());
  EXPECT_FALSE(journal->complete());
  ASSERT_TRUE(result.lease->cleanup());
  ASSERT_TRUE(result.lease->cleanup_receipt());
  EXPECT_TRUE(journal->retire(*result.lease->cleanup_receipt()));
  EXPECT_TRUE(journal->complete());
}

TEST_F(SudoVdaOwnershipTest, InvalidUnarmedRequestReturnsNoAddReceiptWithoutDriverAccess) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  auto invalid {request()};
  invalid.width = 0;
  auto result {lease_t::acquire_owned(std::make_unique<fake_system_t>(fake), invalid, journal, authority)};
  expect_failure(result, errc_t::invalid_mode);
  ASSERT_TRUE(result.cleanup_receipt);
  EXPECT_EQ(result.cleanup_receipt->kind(), cleanup_receipt_t::kind_e::unarmed);
  EXPECT_TRUE(fake->events.empty());
  EXPECT_TRUE(journal->retire(*result.cleanup_receipt));
  EXPECT_TRUE(journal->complete());
}

TEST_F(SudoVdaOwnershipTest, AbandonedJournalLeaseNeverRemovesBeforeDisplayRestoration) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  {
    auto result {lease_t::acquire_owned(std::make_unique<fake_system_t>(fake), request(), journal, authority)};
    ASSERT_TRUE(result.lease);
    fake->events.clear();
  }
  EXPECT_EQ(count("remove"), 0);
  EXPECT_EQ(count("close"), 1);
  EXPECT_FALSE(fake->tick);
  EXPECT_TRUE(journal->validate(authority));
  EXPECT_FALSE(journal->complete());
}

TEST_F(SudoVdaOwnershipTest, ActiveAndInactiveSourceAlternativesNormalizeDuringAcquireAndRefresh) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  auto active {display()};
  active.source_adapter = target.adapter;
  active.source = 1;
  active.active = true;
  active.source_metadata_valid = true;
  active.gdi_name = L"active-source";
  auto inactive {active};
  inactive.source = 2;
  inactive.active = false;
  inactive.gdi_name = L"inactive-source";
  fake->after.displays = {inactive, active};
  auto result {lease_t::acquire_owned(std::make_unique<fake_system_t>(fake), request(), journal, authority)};
  ASSERT_FALSE(result.failure);
  ASSERT_TRUE(result.lease);
  ASSERT_TRUE(result.lease->display());
  EXPECT_EQ(result.lease->display()->gdi_name, L"active-source");
  ASSERT_TRUE(result.lease->refresh_display(1, 0ms));
  EXPECT_EQ(result.lease->display()->gdi_name, L"active-source");
  EXPECT_TRUE(result.lease->cleanup());
}

TEST_F(SudoVdaOwnershipTest, RecoveryNormalizesOneActiveMonitorWithInactiveSourceAlternatives) {
  const auto journal {crash_journal()};
  ASSERT_TRUE(journal);
  auto active {display()};
  active.source_adapter = target.adapter;
  active.source = 1;
  active.active = true;
  active.source_metadata_valid = true;
  active.gdi_name = L"active-source";
  auto inactive {active};
  inactive.source = 2;
  inactive.active = false;
  inactive.gdi_name = L"inactive-source";
  fake->before = {{identity}, {inactive, active}};
  auto result {lease_t::recover(std::make_unique<fake_system_t>(fake), journal, authority)};
  ASSERT_FALSE(result.failure);
  ASSERT_TRUE(result.lease);
  ASSERT_TRUE(result.lease->display());
  EXPECT_EQ(result.lease->display()->gdi_name, L"active-source");
  EXPECT_TRUE(result.lease->cleanup());
}

TEST_F(SudoVdaOwnershipTest, RecoveryRejectsTwoDistinctActiveSourcesForOwnedMonitor) {
  const auto journal {crash_journal()};
  ASSERT_TRUE(journal);
  auto first {display()};
  first.source_adapter = target.adapter;
  first.source = 1;
  first.active = true;
  first.source_metadata_valid = true;
  auto second {first};
  second.source = 2;
  fake->before = {{identity}, {first, second}};
  auto result {lease_t::recover(std::make_unique<fake_system_t>(fake), journal, authority)};
  ASSERT_TRUE(result.failure);
  EXPECT_EQ(result.failure->code, errc_t::ownership_unverified);
  ASSERT_TRUE(result.lease);
  EXPECT_EQ(count("remove"), 0);
  EXPECT_TRUE(fake->tick);
  EXPECT_TRUE(result.lease->cleanup());
}

TEST_F(SudoVdaOwnershipTest, RecoveryRejectsConflictingMonitorPathsForOwnedGuid) {
  const auto journal {crash_journal()};
  ASSERT_TRUE(journal);
  auto first {display()};
  auto second {first};
  second.monitor_path = L"another-monitor-path";
  fake->before = {{identity}, {first, second}};
  auto result {lease_t::recover(std::make_unique<fake_system_t>(fake), journal, authority)};
  ASSERT_TRUE(result.failure);
  EXPECT_EQ(result.failure->code, errc_t::ownership_unverified);
  ASSERT_TRUE(result.lease);
  EXPECT_EQ(count("remove"), 0);
  EXPECT_TRUE(result.lease->cleanup());
}

TEST_F(SudoVdaOwnershipTest, RefreshRejectsConflictingTargetForOwnedGuid) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  auto result {lease_t::acquire_owned(std::make_unique<fake_system_t>(fake), request(), journal, authority)};
  ASSERT_TRUE(result.lease);
  auto other {display()};
  ++other.target.target;
  fake->after.displays.push_back(other);
  const auto refreshed {result.lease->refresh_display(1, 0ms)};
  ASSERT_FALSE(refreshed);
  EXPECT_EQ(refreshed.error().code, errc_t::ownership_unverified);
  EXPECT_FALSE(result.lease->display());
  EXPECT_FALSE(result.lease->identity_absent());
  EXPECT_EQ(count("remove"), 0);
  EXPECT_TRUE(result.lease->cleanup());
}

TEST_F(SudoVdaOwnershipTest, SettingsClearAtomicallyPreservesEpisodeRestorationEvidenceAcrossReload) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  const std::vector<std::uint8_t> saved {'e', 'p', 'i', 's', 'o', 'd', 'e'};
  ASSERT_TRUE(journal->store_settings(saved));
  http::test_support::set_state_replace_hook([](const auto &, const auto &, const auto &) {
    return std::make_optional<std::uint32_t>(ERROR_ACCESS_DENIED);
  });
  EXPECT_FALSE(journal->store_settings({}));
  http::test_support::set_state_replace_hook({});
  ASSERT_TRUE(journal->settings());
  EXPECT_EQ(*journal->settings(), saved);
  ASSERT_TRUE(journal->restoration_state());
  EXPECT_EQ(*journal->restoration_state(), saved);
  ASSERT_TRUE(journal->store_settings({}));
  const auto loaded {ownership_journal_t::load(path, authority)};
  ASSERT_TRUE(loaded);
  ASSERT_TRUE((*loaded)->settings());
  EXPECT_TRUE((*loaded)->settings()->empty());
  ASSERT_TRUE((*loaded)->restoration_state());
  EXPECT_EQ(*(*loaded)->restoration_state(), saved);
}

TEST_F(SudoVdaOwnershipTest, ProvenanceCapacityFailurePreservesReferencedRecoveryState) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  ASSERT_TRUE(journal->store_settings({1, 2, 3}));
  std::vector<std::string> observed;
  for (unsigned int index {}; index < 257; ++index) {
    observed.push_back("fixture-device-" + std::to_string(index));
  }
  EXPECT_FALSE(journal->observe_devices(observed));
  EXPECT_TRUE(journal->validate(authority));
  ASSERT_TRUE(journal->settings());
  EXPECT_EQ(*journal->settings(), (std::vector<std::uint8_t> {1, 2, 3}));
  ASSERT_TRUE(journal->recovery_devices());
  EXPECT_TRUE(journal->recovery_devices()->empty());
}

TEST_F(SudoVdaOwnershipTest, UnpublishedVersionOneIntentIsRejectedWithoutReplacement) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  std::string old_bytes;
  ASSERT_TRUE(http::update_state_file_bytes(path.string(), [&](std::string bytes) -> std::optional<std::string> {
    const auto position {bytes.find("\"schema\":2")};
    EXPECT_NE(position, std::string::npos);
    if (position == std::string::npos) {
      return std::nullopt;
    }
    bytes.replace(position, std::string {"\"schema\":2"}.size(), "\"schema\":1");
    old_bytes = bytes;
    return bytes;
  }));
  EXPECT_FALSE(ownership_journal_t::load(path, authority));
  ASSERT_TRUE(http::update_state_file_bytes(path.string(), [&](const auto &bytes) -> std::optional<std::string> {
    EXPECT_EQ(bytes, old_bytes);
    return std::nullopt;
  }));
}

TEST_F(SudoVdaOwnershipTest, RecoveryRejectsAlternativeFromConflictingSourceAdapter) {
  const auto journal {crash_journal()};
  ASSERT_TRUE(journal);
  auto first {display()};
  first.source_adapter = target.adapter;
  first.source = 1;
  first.active = true;
  first.source_metadata_valid = true;
  auto second {first};
  second.source = 2;
  second.active = false;
  ++second.source_adapter.LowPart;
  fake->before = {{identity}, {first, second}};
  auto result {lease_t::recover(std::make_unique<fake_system_t>(fake), journal, authority)};
  ASSERT_TRUE(result.failure);
  EXPECT_EQ(result.failure->code, errc_t::ownership_unverified);
  ASSERT_TRUE(result.lease);
  EXPECT_EQ(count("remove"), 0);
  EXPECT_TRUE(result.lease->cleanup());
}

TEST_F(SudoVdaOwnershipTest, MappingLoadRejectsUnsignedAdapterHighThatWouldNarrowToSignedValue) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  auto result {lease_t::acquire_owned(std::make_unique<fake_system_t>(fake), request(), journal, authority)};
  ASSERT_TRUE(result.lease);
  ASSERT_TRUE(result.lease->display());
  ASSERT_TRUE(journal->bind_mapping(*result.lease->display(), "fixture-owned-id"));
  ASSERT_TRUE(result.lease->cleanup());
  ASSERT_TRUE(http::update_state_file_bytes(path.string(), [](const std::string &bytes) -> std::optional<std::string> {
    auto data = nlohmann::json::parse(bytes);
    data["mapping"]["adapter_high"] = std::numeric_limits<std::uint64_t>::max();
    return data.dump();
  }));
  EXPECT_FALSE(ownership_journal_t::load(path, authority));
}

TEST_F(SudoVdaOwnershipTest, JournalLoadRejectsNegativeGuidByteThatWouldNarrowToOriginalByte) {
  const auto journal {make_journal()};
  ASSERT_TRUE(journal);
  ASSERT_TRUE(http::update_state_file_bytes(path.string(), [](const std::string &bytes) -> std::optional<std::string> {
    auto data = nlohmann::json::parse(bytes);
    data["identity"][0] = static_cast<std::int64_t>(data["identity"][0].get<unsigned int>()) - 0x100000000LL;
    return data.dump();
  }));
  EXPECT_FALSE(ownership_journal_t::load(path, authority));
  EXPECT_TRUE(fake->events.empty());
}
#endif
