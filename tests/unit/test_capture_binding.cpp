/**
 * @file tests/unit/test_capture_binding.cpp
 * @brief Exact Windows capture binding with inert display-library API observations.
 */
// test includes
#include "../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <algorithm>
  #include <optional>
  #include <string>
  #include <vector>

  // platform includes
  // WinSock2.h must precede Windows.h from libdisplaydevice.
  // clang-format off
  #include <WinSock2.h>
  #include <Windows.h>
  // clang-format on

  // local includes
  #include <src/platform/windows/capture_binding.h>

namespace {
  using namespace display_device;
  using namespace platf::capture_binding;

  /** @brief Model QueryType::All alternatives and readback without changing any Windows state. */
  class binding_api_t final: public WinApiLayerInterface {
  public:
    struct entry_t {
      std::string path = "MONITOR-PATH";
      std::string device = "library-id";
      std::string selector = "exact-selector";
      std::optional<HdrState> hdr = HdrState::Disabled;
    };

    PathAndModeData data;
    std::vector<entry_t> entries;
    bool query_fails = false;
    mutable unsigned int queries = 0;
    mutable unsigned int mutations = 0;

    binding_api_t() {
      add({5, 6}, 21, true);
    }

    void add(LUID source_adapter, UINT32 source_id, bool active) {
      DISPLAYCONFIG_PATH_INFO path {};
      path.targetInfo.adapterId = {2, 3};
      path.targetInfo.id = 7;
      path.targetInfo.targetAvailable = TRUE;
      path.targetInfo.modeInfoIdx = entries.size();
      path.targetInfo.refreshRate = {120000, 1000};
      path.sourceInfo.adapterId = source_adapter;
      path.sourceInfo.id = source_id;
      path.sourceInfo.sourceModeInfoIdx = data.m_modes.size();
      path.flags = active ? DISPLAYCONFIG_PATH_ACTIVE : 0;
      DISPLAYCONFIG_MODE_INFO mode {};
      mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
      mode.adapterId = source_adapter;
      mode.id = source_id;
      mode.sourceMode.width = 2560;
      mode.sourceMode.height = 1440;
      data.m_modes.emplace_back(mode);
      data.m_paths.emplace_back(path);
      entries.emplace_back();
    }

    const entry_t &entry(const DISPLAYCONFIG_PATH_INFO &path) const {
      return entries.at(path.targetInfo.modeInfoIdx);
    }

    std::string getErrorString(LONG) const override {
      return "fixture";
    }

    std::optional<PathAndModeData> queryDisplayConfig(QueryType type) const override {
      ++queries;
      EXPECT_EQ(type, QueryType::All);
      return query_fails ? std::nullopt : std::optional<PathAndModeData> {data};
    }

    bool wakeDisplay(std::chrono::milliseconds) override {
      ++mutations;
      return false;
    }

    bool keepDisplayAwake() override {
      ++mutations;
      return false;
    }

    bool restorePowerRequest() override {
      ++mutations;
      return false;
    }

    std::string getDeviceId(const DISPLAYCONFIG_PATH_INFO &path) const override {
      return entry(path).device;
    }

    std::vector<std::byte> getEdid(const DISPLAYCONFIG_PATH_INFO &) const override {
      return {};
    }

    std::string getMonitorDevicePath(const DISPLAYCONFIG_PATH_INFO &path) const override {
      return entry(path).path;
    }

    std::string getFriendlyName(const DISPLAYCONFIG_PATH_INFO &) const override {
      return {};
    }

    std::string getDisplayName(const DISPLAYCONFIG_PATH_INFO &path) const override {
      return entry(path).selector;
    }

    LONG setDisplayConfig(std::vector<DISPLAYCONFIG_PATH_INFO>, std::vector<DISPLAYCONFIG_MODE_INFO>, UINT32) override {
      ++mutations;
      return ERROR_NOT_SUPPORTED;
    }

    std::optional<HdrState> getHdrState(const DISPLAYCONFIG_PATH_INFO &path) const override {
      return entry(path).hdr;
    }

    bool setHdrState(const DISPLAYCONFIG_PATH_INFO &, HdrState) override {
      ++mutations;
      return false;
    }

    std::optional<Rational> getDisplayScale(std::string_view, const DISPLAYCONFIG_SOURCE_MODE &) const override {
      return std::nullopt;
    }
  };

  rtsp_stream::capture_source_t source() {
    rtsp_stream::capture_source_t result;
    result.generation = 4;
    result.epoch = 9;
    result.selector = "exact-selector";
    result.device_id = "library-id";
    result.windows_output = rtsp_stream::windows_output_t {{2, 3}, 7, "monitor-path"};
    result.width = 2560;
    result.height = 1440;
    result.rate_numerator = 120;
    result.rate_denominator = 1;
    result.observed_hdr = false;
    return result;
  }
}  // namespace

TEST(CaptureBindingTest, ResolvesSourceAdapterIndependentlyAndNeverMutatesWindows) {
  binding_api_t api;
  const auto binding = source();
  const auto observed = verify(binding, api);
  ASSERT_TRUE(observed);
  EXPECT_EQ(observed->source_adapter, (rtsp_stream::luid_t {5, 6}));
  EXPECT_NE(observed->source_adapter, binding.windows_output->target_adapter);
  EXPECT_EQ(observed->source_id, 21);
  EXPECT_EQ(observed->width, 2560);
  EXPECT_EQ(observed->rate_numerator, 120000);
  EXPECT_EQ(observed->hdr, false);
  EXPECT_EQ(api.mutations, 0);
}

TEST(CaptureBindingTest, NormalizesInactiveSourceAlternativesAndIdenticalActiveMappings) {
  binding_api_t api;
  api.add({99, 100}, 78, false);
  api.entries.back().selector = "reused-inactive-name";
  api.data.m_paths.back().targetInfo.targetAvailable = FALSE;
  api.add({5, 6}, 21, true);
  EXPECT_TRUE(verify(source(), api));
}

TEST(CaptureBindingTest, RejectsUnavailableSelectedTargetBeforeOpenAndAfterInitialization) {
  binding_api_t api;
  const auto binding = source();
  const auto before = verify(binding, api);
  ASSERT_TRUE(before);
  api.data.m_paths.front().targetInfo.targetAvailable = FALSE;
  EXPECT_FALSE(observe(binding, api));
  EXPECT_FALSE(verify(binding, api));
  EXPECT_FALSE(verify_opened(binding, *before, api, {5, 6}, "exact-selector", true, 2560, 1440));
}

TEST(CaptureBindingTest, PreservesNativeCloneSourceWhileFencingTheSelectedTarget) {
  binding_api_t api;
  api.add({5, 6}, 21, true);
  api.data.m_paths.back().targetInfo.id = 8;
  api.entries.back().path = "clone-monitor";
  api.entries.back().device = "clone-library-id";
  const auto binding = source();
  const auto observed = verify(binding, api);
  ASSERT_TRUE(observed);
  EXPECT_TRUE(verify_opened(binding, *observed, api, {5, 6}, "exact-selector", true, 2560, 1440));
  std::ranges::reverse(api.data.m_paths);
  EXPECT_TRUE(verify(binding, api));
  api.data.m_paths.back().flags = 0;
  EXPECT_FALSE(verify(binding, api));
  api.data.m_paths.back().flags = DISPLAYCONFIG_PATH_ACTIVE;
  api.entries.front().path = "replacement-monitor";
  EXPECT_FALSE(verify(binding, api));
  EXPECT_EQ(api.mutations, 0);
}

TEST(CaptureBindingTest, RejectsConflictingActiveSourceMappings) {
  binding_api_t api;
  api.add({5, 6}, 22, true);
  EXPECT_FALSE(verify(source(), api));
  api.data.m_paths.back().sourceInfo.id = 21;
  api.data.m_modes.back().id = 21;
  api.data.m_paths.back().sourceInfo.adapterId.LowPart = 55;
  api.data.m_modes.back().adapterId.LowPart = 55;
  EXPECT_FALSE(verify(source(), api));
}

TEST(CaptureBindingTest, RejectsTargetPathAndLibraryIdentityConflictsEvenOnInactiveAlternatives) {
  for (int conflict = 0; conflict != 4; ++conflict) {
    binding_api_t api;
    api.add({5, 6}, 22, false);
    if (conflict == 0) {
      ++api.data.m_paths.back().targetInfo.id;
    } else if (conflict == 1) {
      ++api.data.m_paths.back().targetInfo.adapterId.LowPart;
    } else if (conflict == 2) {
      api.entries.back().path = "different-path";
    } else {
      api.entries.back().device = "different-library-id";
    }
    EXPECT_FALSE(verify(source(), api)) << conflict;
  }
}

TEST(CaptureBindingTest, RejectsSelectorReuseAndChangedSelectorWithoutFallback) {
  binding_api_t api;
  api.entries.front().selector = "new-selector";
  EXPECT_FALSE(verify(source(), api));
  api.entries.front().selector = "exact-selector";
  api.add({5, 6}, 22, true);
  api.data.m_paths.back().targetInfo.id = 8;
  api.entries.back().path = "other-monitor";
  api.entries.back().device = "other-library-id";
  EXPECT_FALSE(verify(source(), api));
  api.data.m_paths.back().sourceInfo.id = 21;
  api.data.m_modes.back().id = 21;
  api.data.m_paths.back().sourceInfo.adapterId.LowPart = 55;
  api.data.m_modes.back().adapterId.LowPart = 55;
  EXPECT_FALSE(verify(source(), api));
}

TEST(CaptureBindingTest, RejectsMissingInactiveOrInvalidModeObservations) {
  for (int failure = 0; failure != 7; ++failure) {
    binding_api_t api;
    if (failure == 0) {
      api.query_fails = true;
    } else if (failure == 1) {
      api.data.m_paths.front().flags = 0;
    } else if (failure == 2) {
      api.data.m_modes.front().sourceMode.width = 0;
    } else if (failure == 3) {
      api.data.m_paths.front().targetInfo.refreshRate.Denominator = 0;
    } else if (failure == 4) {
      api.data.m_modes.front().id = 999;
    } else if (failure == 5) {
      api.data.m_modes.front().sourceMode.height = 0;
    } else {
      api.data.m_paths.front().sourceInfo.sourceModeInfoIdx = DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID;
    }
    EXPECT_FALSE(verify(source(), api)) << failure;
  }
}

TEST(CaptureBindingTest, NativeReopenObservesActualModeAndUnknownHdr) {
  binding_api_t api;
  api.data.m_modes.front().sourceMode.width = 1920;
  api.data.m_modes.front().sourceMode.height = 1080;
  api.data.m_paths.front().targetInfo.refreshRate = {75, 1};
  api.entries.front().hdr.reset();
  auto binding = source();
  binding.requested_hdr = true;
  const auto observed = verify(binding, api);
  ASSERT_TRUE(observed);
  EXPECT_EQ(observed->width, 1920);
  EXPECT_EQ(observed->height, 1080);
  EXPECT_EQ(observed->rate_numerator, 75);
  EXPECT_FALSE(observed->hdr);
  api.entries.front().hdr = HdrState::Disabled;
  const auto disabled = verify(binding, api);
  ASSERT_TRUE(disabled);
  ASSERT_TRUE(disabled->hdr.has_value());
  EXPECT_FALSE(*disabled->hdr);
}

TEST(CaptureBindingTest, VirtualModeIsFixedAndHdrIsRequiredOnlyWhenExplicitlyRequested) {
  binding_api_t api;
  auto binding = source();
  binding.kind = rtsp_stream::capture_source_t::kind_e::owned_virtual;
  EXPECT_TRUE(verify(binding, api));
  api.entries.front().hdr.reset();
  EXPECT_TRUE(verify(binding, api));
  binding.requested_hdr = false;
  EXPECT_FALSE(verify(binding, api));
  api.entries.front().hdr = HdrState::Disabled;
  EXPECT_TRUE(verify(binding, api));
  binding.requested_hdr = true;
  EXPECT_FALSE(verify(binding, api));
  api.entries.front().hdr = HdrState::Enabled;
  EXPECT_TRUE(verify(binding, api));
  api.data.m_modes.front().sourceMode.width = 1920;
  EXPECT_FALSE(verify(binding, api));
  api.data.m_modes.front().sourceMode.width = binding.width;
  api.data.m_modes.front().sourceMode.height = 1080;
  EXPECT_FALSE(verify(binding, api));
  api.data.m_modes.front().sourceMode.height = binding.height;
  api.data.m_paths.front().targetInfo.refreshRate = {119999, 1000};
  EXPECT_FALSE(verify(binding, api));
  api.data.m_paths.front().targetInfo.refreshRate = {120000, 1000};
  EXPECT_TRUE(verify(binding, api));
}

TEST(CaptureBindingTest, PublicationChecksOpenedSourceAdapterSelectorAttachmentAndMode) {
  binding_api_t api;
  const auto binding = source();
  const auto before = verify(binding, api);
  ASSERT_TRUE(before);
  EXPECT_TRUE(verify_opened(binding, *before, api, {5, 6}, "exact-selector", true, 2560, 1440));
  EXPECT_FALSE(verify_opened(binding, *before, api, {2, 3}, "exact-selector", true, 2560, 1440));
  EXPECT_FALSE(verify_opened(binding, *before, api, {5, 6}, "other-selector", true, 2560, 1440));
  EXPECT_FALSE(verify_opened(binding, *before, api, {5, 6}, "exact-selector", false, 2560, 1440));
  EXPECT_FALSE(verify_opened(binding, *before, api, {5, 6}, "exact-selector", true, 1920, 1080));
  api.data.m_paths.front().targetInfo.id = 8;
  EXPECT_FALSE(verify_opened(binding, *before, api, {5, 6}, "exact-selector", true, 2560, 1440));
}

TEST(CaptureBindingTest, InitializationRaceRequiresANewObservationBeforePublication) {
  binding_api_t api;
  const auto binding = source();
  const auto before = verify(binding, api);
  ASSERT_TRUE(before);
  api.data.m_paths.front().targetInfo.refreshRate = {60, 1};
  EXPECT_FALSE(verify_opened(binding, *before, api, {5, 6}, "exact-selector", true, 2560, 1440));
  const auto after = verify(binding, api);
  ASSERT_TRUE(after);
  EXPECT_TRUE(verify_opened(binding, *after, api, {5, 6}, "exact-selector", true, 2560, 1440));
}
#endif
