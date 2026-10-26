/**
 * @file tests/unit/test_display_policy.cpp
 * @brief Pure display-policy precedence, identity vectors and immutable shared-source regressions.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <set>

// local includes
#include <src/display_device.h>
#include <src/display_policy.h>
#include <src/rtsp.h>

namespace {
  constexpr auto client_uuid = "11111111-2222-4333-8444-555555555555";
  constexpr auto app_uuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";

  /** @brief Supply synthetic already-authorized identities and an ordinary selected source mode. */
  display_policy::fresh_source_t fresh_source() {
    display_policy::fresh_source_t source;
    source.authenticated_client_uuid = client_uuid;
    source.application_uuid = app_uuid;
    source.application_has_lifetime = true;
    source.selected_mode = {{1920, 1080}, {60000, 1001}, false};
    return source;
  }
}  // namespace

TEST(DisplayPolicyTest, ForcePreferencesAreAdditiveAndHeadlessRequestsRequireVirtual) {
  for (unsigned mask = 0; mask < 32; ++mask) {
    auto video = config::video;
    auto fresh = fresh_source();
    video.dd.force_virtual = mask & 1;
    fresh.app_force_virtual = mask & 2;
    fresh.device.force_virtual = mask & 4;
    fresh.client_requests_virtual = mask & 8;
    fresh.physical_source_available = !(mask & 16);
    const auto request = display_policy::resolve(video, fresh);
    EXPECT_EQ(request->virtual_display, mask != 0) << mask;
  }
}

TEST(DisplayPolicyTest, ExplicitHdrComesFromActualParserAndRemainsSeparateFromStreamHint) {
  using dd_t = config::video_t::dd_t;
  for (const bool display_enabled : {false, true}) {
    for (const bool hdr_enabled : {false, true}) {
      for (const bool stream_hdr : {false, true}) {
        auto video = config::video;
        video.dd.configuration_option = display_enabled ? dd_t::config_option_e::verify_only : dd_t::config_option_e::disabled;
        video.dd.hdr_option = hdr_enabled ? dd_t::hdr_option_e::automatic : dd_t::hdr_option_e::disabled;
        video.dd.resolution_option = dd_t::resolution_option_e::disabled;
        video.dd.refresh_rate_option = dd_t::refresh_rate_option_e::disabled;
        rtsp_stream::launch_session_t launch {};
        launch.enable_hdr = stream_hdr;
        auto fresh = fresh_source();
        fresh.selected_mode.hdr = stream_hdr;
        const auto parsed = display_device::parse_configuration(video, launch);
        if (const auto configuration = std::get_if<display_device::SingleDisplayConfiguration>(&parsed); configuration && configuration->m_hdr_state) {
          fresh.requested_hdr = *configuration->m_hdr_state == display_device::HdrState::Enabled;
        }
        const auto request = display_policy::resolve(video, fresh);
        EXPECT_EQ(request->mode.hdr, stream_hdr);
#ifdef __APPLE__
        EXPECT_FALSE(request->requested_hdr);
#else
        EXPECT_EQ(request->requested_hdr, display_enabled && hdr_enabled ? std::optional {stream_hdr} : std::nullopt);
#endif
        auto joining = fresh;
        joining.requested_hdr = !stream_hdr;
        joining.selected_mode.hdr = !stream_hdr;
        EXPECT_EQ(display_policy::resolve(video, joining, request), request);
        EXPECT_EQ(request->mode.hdr, stream_hdr);
      }
    }
  }
}

TEST(DisplayPolicyTest, FullNamespaceDomainAndTupleVectorsAreFrozen) {
  using enum display_policy::identity_e;
  EXPECT_EQ(display_policy::monitor_uuid(client, client_uuid, app_uuid), "badb214e-d88e-8513-acb4-1e67c0079bf1");
  EXPECT_EQ(display_policy::monitor_uuid(application, client_uuid, app_uuid), "81688958-a2d4-8cda-9a00-9e5480ef08eb");
  EXPECT_EQ(display_policy::monitor_uuid(client_application, client_uuid, app_uuid), "f1605c99-4ea3-8117-80b8-d66d3d009d7c");
  EXPECT_EQ(display_policy::monitor_uuid(application, client_uuid, std::nullopt), "d5c4e6cf-7034-80ca-8e4f-12a3b15aefa5");
  EXPECT_EQ(display_policy::monitor_uuid(client_application, client_uuid, std::nullopt), "f4164a7c-7a64-834e-a2cb-414559b2ea1a");
  EXPECT_EQ(display_policy::monitor_uuid(application, client_uuid, "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE"), "81688958-a2d4-8cda-9a00-9e5480ef08eb");
  EXPECT_EQ(display_policy::monitor_uuid(client, app_uuid, client_uuid), display_policy::monitor_uuid(client, app_uuid, std::nullopt));
  EXPECT_EQ(display_policy::monitor_uuid(application, app_uuid, app_uuid), display_policy::monitor_uuid(application, client_uuid, app_uuid));
  const std::set identities {
    display_policy::monitor_uuid(client, app_uuid, app_uuid),
    display_policy::monitor_uuid(application, app_uuid, app_uuid),
    display_policy::monitor_uuid(client_application, app_uuid, client_uuid),
    display_policy::monitor_uuid(client_application, client_uuid, app_uuid),
    display_policy::monitor_uuid(client_application, app_uuid, app_uuid),
  };
  EXPECT_EQ(identities.size(), 5U);
  EXPECT_FALSE(proc::catalog::canonical_uuid(proc::catalog::desktop_uuid));
}

TEST(DisplayPolicyTest, IdentityRejectsNamesProtocolNumbersMalformedAndReservedApplications) {
  using enum display_policy::identity_e;
  for (const auto *invalid : {"", "Desktop", "123", "00000000-0000-0000-0000-000000000000", "11111111-2222-4333-8444-55555555555g", "111111112222-4333-8444-555555555555"}) {
    EXPECT_THROW(display_policy::monitor_uuid(client, invalid, app_uuid), std::invalid_argument) << invalid;
    EXPECT_THROW(display_policy::monitor_uuid(application, client_uuid, invalid), std::invalid_argument) << invalid;
  }
  EXPECT_THROW(display_policy::monitor_uuid(application, client_uuid, proc::catalog::desktop_uuid), std::invalid_argument);
  EXPECT_THROW(display_policy::monitor_uuid(static_cast<display_policy::identity_e>(99), client_uuid, app_uuid), std::invalid_argument);
}

TEST(DisplayPolicyTest, DeviceOverridesWinAfterOrdinarySelectionWithoutChangingSavedPreferences) {
  auto video = config::video;
  video.dd.configuration_option = config::video_t::dd_t::config_option_e::ensure_primary;
  video.adapter_name = "synthetic-render-adapter";
  video.output_name = "synthetic-native-output";
  auto fresh = fresh_source();
  fresh.device = display_policy::parse_device(display_policy::json::parse(R"({"resolution":{"width":2560,"height":1440},"refresh_rate":{"numerator":240000,"denominator":2002}})"));
  fresh.selected_mode.hdr = true;
  const auto before = fresh.device;
  const auto request = display_policy::resolve(video, fresh);
  EXPECT_EQ(request->mode.resolution, (display_policy::resolution_t {2560, 1440}));
  EXPECT_EQ(request->mode.refresh_rate, (display_policy::refresh_rate_t {120000, 1001}));
  EXPECT_TRUE(request->mode.hdr);
  EXPECT_EQ(request->topology, video.dd.configuration_option);
  EXPECT_EQ(request->adapter_name, video.adapter_name);
  EXPECT_EQ(request->native_output, video.output_name);
  EXPECT_EQ(fresh.device, before);
  EXPECT_EQ(request->edid_name, "Miranda-badb2");
  EXPECT_EQ(request->edid_serial, "badb214ed88e8");
  EXPECT_LE(request->edid_name.size(), 13U);
  EXPECT_LE(request->edid_serial.size(), 13U);
}

TEST(DisplayPolicyTest, JoiningReturnsTheExactSourceAcrossConflictingPreferencesAndInvalidMode) {
  auto video = config::video;
  auto fresh = fresh_source();
  fresh.device.force_virtual = true;
  fresh.selected_mode.hdr = true;
  const auto active = display_policy::resolve(video, fresh);
  const auto original = *active;
  fresh.authenticated_client_uuid = app_uuid;
  fresh.application_uuid = "invalid-ignored-on-join";
  fresh.device.resolution = {3840, 2160};
  fresh.device.refresh_rate = {165, 1};
  fresh.device_preferences_valid = false;
  fresh.selected_mode = {};
  video.dd.force_virtual = false;
  video.dd.virtual_identity = display_policy::identity_e::application;
  video.dd.configuration_option = config::video_t::dd_t::config_option_e::ensure_only_display;
  video.dd.config_revert_on_disconnect = !video.dd.config_revert_on_disconnect;
  const auto joined = display_policy::resolve(video, fresh, active);
  EXPECT_EQ(joined.get(), active.get());
  EXPECT_EQ(*joined, original);
  EXPECT_EQ(fresh.device.resolution, (display_policy::resolution_t {3840, 2160}));
  fresh.authenticated_client_uuid.clear();
  EXPECT_THROW(display_policy::resolve(video, fresh, active), std::invalid_argument);
}

TEST(DisplayPolicyTest, LastViewerPolicyDistinguishesApplicationsDesktopAndDisconnectRestoration) {
  auto video = config::video;
  auto fresh = fresh_source();
  video.dd.config_revert_delay = std::chrono::milliseconds {1234};
  video.dd.config_revert_on_disconnect = false;
  EXPECT_TRUE(display_policy::resolve(video, fresh)->retain_while_app_running);
  EXPECT_EQ(display_policy::resolve(video, fresh)->restore_delay.count(), 1234);
  video.dd.config_revert_on_disconnect = true;
  EXPECT_FALSE(display_policy::resolve(video, fresh)->retain_while_app_running);
  video.dd.config_revert_on_disconnect = false;
  fresh.application_has_lifetime = false;
  EXPECT_FALSE(display_policy::resolve(video, fresh)->retain_while_app_running);
  fresh.application_has_lifetime = true;
  fresh.application_uuid.reset();
  EXPECT_FALSE(display_policy::resolve(video, fresh)->retain_while_app_running);
}

TEST(DisplayPolicyTest, MalformedSavedPreferencesAndFreshModesRequireRepair) {
  const auto video = config::video;
  auto fresh = fresh_source();
  fresh.device_preferences_valid = false;
  EXPECT_THROW(display_policy::resolve(video, fresh), std::invalid_argument);
  fresh.device_preferences_valid = true;
  fresh.selected_mode.resolution.width = 0;
  EXPECT_THROW(display_policy::resolve(video, fresh), std::invalid_argument);
  fresh = fresh_source();
  fresh.selected_mode.refresh_rate.denominator = 0;
  EXPECT_THROW(display_policy::resolve(video, fresh), std::invalid_argument);
}

TEST(DisplayPolicyTest, DisplayFreeAdmissionsNeverAcquireOrPinAnExistingVideoSource) {
  const auto video = config::video;
  auto fresh = fresh_source();
  const auto active = display_policy::resolve(video, fresh);
  fresh.admission = display_policy::admission_e::no_display;
  fresh.device_preferences_valid = false;
  fresh.selected_mode = {};
  EXPECT_FALSE(display_policy::resolve(video, fresh));
  EXPECT_FALSE(display_policy::resolve(video, fresh, active));
  EXPECT_EQ(active.use_count(), 1);
}

TEST(DisplayPreferencesTest, TypedOverridesRejectMalformedOrLossyValues) {
  for (const auto *invalid : {
         R"(null)",
         R"([])",
         R"({"force_virtual":"true"})",
         R"({"resolution":"1920x1080"})",
         R"({"resolution":{"width":1920}})",
         R"({"resolution":{"width":0,"height":1080}})",
         R"({"resolution":{"width":-1,"height":1080}})",
         R"({"resolution":{"width":1920.5,"height":1080}})",
         R"({"resolution":{"width":2147483648,"height":1080}})",
         R"({"resolution":{"width":true,"height":1080}})",
         R"({"refresh_rate":59.94})",
         R"({"refresh_rate":{"numerator":60}})",
         R"({"refresh_rate":{"numerator":60000,"denominator":0}})",
         R"({"refresh_rate":{"numerator":"60000","denominator":1001}})",
         R"({"refresh_rate":{"numerator":18446744073709551615,"denominator":1001}})",
       }) {
    const auto value = display_policy::json::parse(invalid);
    EXPECT_THROW(display_policy::parse_device(value), std::invalid_argument) << invalid;
  }
}

TEST(DisplayPreferencesTest, UnknownTypedMetadataSurvivesEditsAndClearedOverrides) {
  const auto original = display_policy::json::parse(R"({"force_virtual":false,"resolution":{"width":1920,"height":1080,"foreign":[false,null,{},[]]},"refresh_rate":{"numerator":60000,"denominator":1001,"foreign":17},"foreign":{"flags":[false,null],"number":1.25}})");
  auto preferences = display_policy::parse_device(original);
  preferences.force_virtual = true;
  auto saved = display_policy::serialize_device(preferences, original);
  EXPECT_EQ(saved["foreign"], original["foreign"]);
  EXPECT_EQ(saved["resolution"]["foreign"], original["resolution"]["foreign"]);
  EXPECT_EQ(saved["refresh_rate"]["foreign"], 17);
  EXPECT_TRUE(saved["force_virtual"].is_boolean());
  preferences.resolution.reset();
  preferences.refresh_rate.reset();
  saved = display_policy::serialize_device(preferences, saved);
  EXPECT_EQ(display_policy::parse_device(saved), preferences);
  EXPECT_EQ(saved["resolution"]["foreign"], original["resolution"]["foreign"]);
  EXPECT_EQ(saved["refresh_rate"]["foreign"], 17);
}
