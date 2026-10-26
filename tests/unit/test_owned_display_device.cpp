/**
 * @file tests/unit/test_owned_display_device.cpp
 * @brief Owned display transactions with the real settings algorithm and fake Windows/driver boundaries.
 */
// test includes
#include "../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <condition_variable>
  #include <cstring>
  #include <filesystem>
  #include <fstream>
  #include <map>
  #include <mutex>
  #include <thread>
  #include <tuple>

  // platform includes
  // WinSock2.h must precede Windows.h from libdisplaydevice.
  // clang-format off
  #include <WinSock2.h>
  #include <Windows.h>
  // clang-format on

  // lib includes
  #include <display_device/audio_context_interface.h>
  #include <display_device/windows/json.h>
  #include <display_device/windows/win_api_layer_interface.h>
  #include <display_device/windows/win_display_device_interface.h>
  #include <nlohmann/json.hpp>

  // local includes
  #include <src/display_device.h>
  #include <src/httpcommon.h>

namespace {
  using namespace display_device;
  using namespace platf::sudovda;
  using namespace std::chrono_literals;

  const GUID first_identity {0x11111111, 0x1234, 0x5678, {1, 2, 3, 4, 5, 6, 7, 8}};
  const GUID second_identity {0x22222222, 0x1234, 0x5678, {1, 2, 3, 4, 5, 6, 7, 8}};
  const LUID display_adapter {10, 20};

  struct display_model_t {
    mutable std::mutex mutex;
    ActiveTopology active {{"dock"}};
    StringSet present {"dock", "panel"};
    std::string primary {"dock"};
    DeviceDisplayModeMap modes {{"dock", {{3840, 2160}, {60, 1}}}, {"panel", {{1920, 1080}, {60, 1}}}, {"virtual1", {{1920, 1080}, {60, 1}}}, {"virtual2", {{1920, 1080}, {60, 1}}}};
    HdrStateMap hdr {{"dock", HdrState::Enabled}, {"panel", HdrState::Disabled}, {"virtual1", HdrState::Disabled}, {"virtual2", HdrState::Disabled}};
    std::function<void()> tick;
    std::function<void()> api_check_hook;
    unsigned int topology_writes {};
    std::vector<ActiveTopology> topology_requests;
    unsigned int pings {};
    unsigned int removals {};
    bool query_failure {};
    bool ambiguous {};
    bool alternative {};
    bool conflicting_library_id {};
    bool conflicting_path {};
    bool conflicting_target {};
    bool conflicting_source_adapter {};
    bool api_unavailable {};
    bool topology_failure {};
    bool mode_failure {};
    bool remove_failure {};
    bool add_failure {};

    bool active_device(const std::string &id) const {
      return std::ranges::any_of(active, [&](const auto &group) {
        return std::ranges::find(group, id) != group.end();
      });
    }

    static UINT target(const std::string &id) {
      return id == "virtual1" ? 7 : id == "virtual2" ? 8 :
                                  id == "dock"       ? 1 :
                                                       2;
    }

    static std::string id(UINT target) {
      return target == 7 ? "virtual1" : target == 8 ? "virtual2" :
                                      target == 1   ? "dock" :
                                                      "panel";
    }

    display_t display(const std::string &id) const {
      return {{display_adapter, target(id)}, id == "virtual1" ? first_identity : second_identity, id == "virtual1" ? L"virtual1-path" : L"virtual2-path", id == "virtual1" ? L"virtual1-capture" : L"virtual2-capture", display_adapter, target(id), active_device(id), true};
    }
  };

  class model_api_t final: public WinApiLayerInterface {
  public:
    explicit model_api_t(std::shared_ptr<display_model_t> model):
        model_ {std::move(model)} {}

    std::string getErrorString(LONG) const override {
      return "fixture";
    }

    std::optional<PathAndModeData> queryDisplayConfig(QueryType type) const override {
      std::lock_guard lock {model_->mutex};
      if (model_->query_failure) {
        return std::nullopt;
      }
      PathAndModeData result;
      for (const auto &id : model_->present) {
        if (type == QueryType::Active && !model_->active_device(id)) {
          continue;
        }
        DISPLAYCONFIG_PATH_INFO path {};
        path.targetInfo.adapterId = display_adapter;
        path.targetInfo.id = display_model_t::target(id);
        path.sourceInfo.adapterId = display_adapter;
        path.sourceInfo.id = display_model_t::target(id);
        path.flags = model_->active_device(id) ? DISPLAYCONFIG_PATH_ACTIVE : 0;
        result.m_paths.push_back(path);
        if ((model_->ambiguous || model_->alternative) && id == "virtual1" && type == QueryType::All) {
          auto alternative {path};
          alternative.sourceInfo.id += 100;
          if (model_->conflicting_source_adapter) {
            ++alternative.sourceInfo.adapterId.LowPart;
          }
          alternative.flags = model_->ambiguous ? path.flags : 0;
          if (model_->conflicting_target) {
            alternative.targetInfo.id += 100;
          }
          result.m_paths.push_back(alternative);
        }
      }
      return result;
    }

    bool wakeDisplay(std::chrono::milliseconds) override {
      return true;
    }

    bool keepDisplayAwake() override {
      return true;
    }

    bool restorePowerRequest() override {
      return true;
    }

    std::string getDeviceId(const DISPLAYCONFIG_PATH_INFO &path) const override {
      std::lock_guard lock {model_->mutex};
      if (path.sourceInfo.id >= 100 && model_->conflicting_library_id) {
        return "conflicting-id";
      }
      return display_model_t::id(path.targetInfo.id);
    }

    std::vector<std::byte> getEdid(const DISPLAYCONFIG_PATH_INFO &) const override {
      return {};
    }

    std::string getMonitorDevicePath(const DISPLAYCONFIG_PATH_INFO &path) const override {
      std::lock_guard lock {model_->mutex};
      if (path.sourceInfo.id >= 100 && model_->conflicting_target) {
        return "virtual1-path";
      }
      if (path.sourceInfo.id >= 100 && model_->conflicting_path) {
        return "different-monitor-path";
      }
      return display_model_t::id(path.targetInfo.id) + "-path";
    }

    std::string getFriendlyName(const DISPLAYCONFIG_PATH_INFO &path) const override {
      return getDeviceId(path);
    }

    std::string getDisplayName(const DISPLAYCONFIG_PATH_INFO &path) const override {
      std::lock_guard lock {model_->mutex};
      if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
        return display_model_t::id(path.targetInfo.id) + "-inactive";
      }
      return display_model_t::id(path.targetInfo.id) + "-capture";
    }

    LONG setDisplayConfig(std::vector<DISPLAYCONFIG_PATH_INFO>, std::vector<DISPLAYCONFIG_MODE_INFO>, UINT32) override {
      return ERROR_NOT_SUPPORTED;
    }

    std::optional<HdrState> getHdrState(const DISPLAYCONFIG_PATH_INFO &) const override {
      return std::nullopt;
    }

    bool setHdrState(const DISPLAYCONFIG_PATH_INFO &, HdrState) override {
      return false;
    }

    std::optional<Rational> getDisplayScale(std::string_view, const DISPLAYCONFIG_SOURCE_MODE &) const override {
      return Rational {1, 1};
    }

  private:
    std::shared_ptr<display_model_t> model_;
  };

  class model_devices_t final: public WinDisplayDeviceInterface {
  public:
    explicit model_devices_t(std::shared_ptr<display_model_t> model):
        model_ {std::move(model)} {}

    bool isApiAccessAvailable() const override {
      std::function<void()> hook;
      bool available;
      {
        std::lock_guard lock {model_->mutex};
        hook = model_->api_check_hook;
        available = !model_->api_unavailable;
      }
      if (hook) {
        hook();
      }
      return available;
    }

    EnumeratedDeviceList enumAvailableDevices() const override {
      std::lock_guard lock {model_->mutex};
      EnumeratedDeviceList result;
      for (const auto &id : model_->present) {
        EnumeratedDevice device {.m_device_id = id, .m_display_name = model_->active_device(id) ? id + "-capture" : "", .m_friendly_name = id};
        device.m_is_internal = id == "panel";
        if (model_->active_device(id)) {
          device.m_info = EnumeratedDevice::Info {.m_resolution = model_->modes.at(id).m_resolution, .m_refresh_rate = model_->modes.at(id).m_refresh_rate, .m_primary = model_->primary == id, .m_hdr_state = model_->hdr.at(id)};
        }
        result.push_back(std::move(device));
      }
      return result;
    }

    std::string getDisplayName(const std::string &id) const override {
      std::lock_guard lock {model_->mutex};
      return model_->present.contains(id) && model_->active_device(id) ? id + "-capture" : "";
    }

    ActiveTopology getCurrentTopology() const override {
      std::lock_guard lock {model_->mutex};
      return model_->active;
    }

    bool isTopologyValid(const ActiveTopology &topology) const override {
      StringSet ids;
      return !topology.empty() && std::ranges::all_of(topology, [&](const auto &group) {
        return !group.empty() && group.size() <= 2 && std::ranges::all_of(group, [&](const auto &id) {
          return !id.empty() && ids.insert(id).second;
        });
      });
    }

    bool isTopologyTheSame(const ActiveTopology &lhs, const ActiveTopology &rhs) const override {
      auto normalize = [](ActiveTopology value) {
        for (auto &group : value) {
          std::ranges::sort(group);
        }
        std::ranges::sort(value);
        return value;
      };
      return normalize(lhs) == normalize(rhs);
    }

    bool setTopology(const ActiveTopology &topology) override {
      std::lock_guard lock {model_->mutex};
      model_->topology_requests.push_back(topology);
      if (model_->topology_failure || !isTopologyValid(topology)) {
        return false;
      }
      for (const auto &group : topology) {
        for (const auto &id : group) {
          if (!model_->present.contains(id)) {
            return false;
          }
        }
      }
      model_->active = topology;
      ++model_->topology_writes;
      if (!model_->active_device(model_->primary)) {
        model_->primary = topology.front().front();
      }
      return true;
    }

    DeviceDisplayModeMap getCurrentDisplayModes(const StringSet &ids) const override {
      std::lock_guard lock {model_->mutex};
      DeviceDisplayModeMap result;
      for (const auto &id : ids) {
        if (model_->present.contains(id) && model_->active_device(id)) {
          result[id] = model_->modes.at(id);
        }
      }
      return result;
    }

    bool setDisplayModes(const DeviceDisplayModeMap &modes) override {
      std::lock_guard lock {model_->mutex};
      if (model_->mode_failure) {
        return false;
      }
      for (const auto &[id, mode] : modes) {
        if (!model_->active_device(id)) {
          return false;
        }
        model_->modes[id] = mode;
      }
      return true;
    }

    bool isPrimary(const std::string &id) const override {
      std::lock_guard lock {model_->mutex};
      return model_->primary == id && model_->active_device(id);
    }

    bool setAsPrimary(const std::string &id) override {
      std::lock_guard lock {model_->mutex};
      if (!model_->active_device(id)) {
        return false;
      }
      model_->primary = id;
      return true;
    }

    HdrStateMap getCurrentHdrStates(const StringSet &ids) const override {
      std::lock_guard lock {model_->mutex};
      HdrStateMap result;
      for (const auto &id : ids) {
        if (model_->present.contains(id) && model_->active_device(id)) {
          result[id] = model_->hdr.at(id);
        }
      }
      return result;
    }

    bool setHdrStates(const HdrStateMap &states) override {
      std::lock_guard lock {model_->mutex};
      for (const auto &[id, state] : states) {
        if (!model_->active_device(id)) {
          return false;
        }
        model_->hdr[id] = state;
      }
      return true;
    }

  private:
    std::shared_ptr<display_model_t> model_;
  };

  class model_heartbeat_t final: public heartbeat_t {
  public:
    explicit model_heartbeat_t(std::shared_ptr<display_model_t> model):
        model_ {std::move(model)} {}

    ~model_heartbeat_t() override {
      stop();
    }

    void stop() noexcept override {
      std::lock_guard lock {model_->mutex};
      model_->tick = {};
    }

  private:
    std::shared_ptr<display_model_t> model_;
  };

  class model_audio_t final: public AudioContextInterface {
  public:
    std::atomic_bool captured {true};
    std::atomic_uint releases {};

    bool capture() override {
      captured = true;
      return true;
    }

    bool isCaptured() const override {
      return captured;
    }

    void release() override {
      captured = false;
      ++releases;
    }
  };

  class model_system_t final: public system_t {
  public:
    explicit model_system_t(std::shared_ptr<display_model_t> model):
        model_ {std::move(model)} {}

    std::expected<void, failure_t> open() override {
      return {};
    }

    std::expected<void, failure_t> close() override {
      return {};
    }

    std::expected<std::size_t, failure_t> ioctl(DWORD code, std::span<const std::byte> input, std::span<std::byte> output) override {
      std::lock_guard lock {model_->mutex};
      if (code == wire::version) {
        const wire::version_t version {0, 2, 1, 1};
        std::memcpy(output.data(), &version, sizeof(version));
      } else if (code == wire::watchdog) {
        const wire::watchdog_t watchdog {3, 3};
        std::memcpy(output.data(), &watchdog, sizeof(watchdog));
      } else if (code == wire::add) {
        if (model_->add_failure) {
          return std::unexpected(failure_t {errc_t::transport_failed, operation_t::add, ERROR_TIMEOUT});
        }
        wire::add_t request {};
        std::memcpy(&request, input.data(), sizeof(request));
        const std::string id {IsEqualGUID(request.identity, first_identity) ? "virtual1" : "virtual2"};
        model_->present.insert(id);
        model_->active.push_back({id});
        if (model_->primary.empty()) {
          model_->primary = id;
        }
        model_->modes[id] = {{request.width, request.height}, {request.millihertz, 1000}};
        const wire::target_t target {display_adapter, display_model_t::target(id)};
        std::memcpy(output.data(), &target, sizeof(target));
      } else if (code == wire::remove) {
        if (model_->remove_failure) {
          return std::unexpected(failure_t {errc_t::remove_failed, operation_t::remove});
        }
        GUID identity {};
        std::memcpy(&identity, input.data(), sizeof(identity));
        const std::string id {IsEqualGUID(identity, first_identity) ? "virtual1" : "virtual2"};
        model_->present.erase(id);
        for (auto &group : model_->active) {
          std::erase(group, id);
        }
        std::erase_if(model_->active, [](const auto &group) {
          return group.empty();
        });
        if (model_->primary == id) {
          model_->primary = model_->active.empty() ? "" : model_->active.front().front();
        }
        ++model_->removals;
      } else if (code == wire::ping) {
        ++model_->pings;
      }
      return output.size();
    }

    std::expected<enumeration_t, failure_t> enumerate() override {
      std::lock_guard lock {model_->mutex};
      enumeration_t result;
      for (const auto &id : {std::string {"virtual1"}, std::string {"virtual2"}}) {
        if (model_->present.contains(id)) {
          const auto display {model_->display(id)};
          result.identities.push_back(display.identity);
          result.displays.push_back(display);
          if (model_->alternative) {
            auto alternative {display};
            alternative.source += 100;
            alternative.active = false;
            alternative.gdi_name = L"inactive-alternative";
            result.displays.insert(result.displays.begin(), alternative);
          }
        }
      }
      return result;
    }

    std::expected<std::unique_ptr<heartbeat_t>, failure_t> heartbeat(std::chrono::milliseconds, std::function<void()> callback) override {
      std::lock_guard lock {model_->mutex};
      model_->tick = std::move(callback);
      return std::make_unique<model_heartbeat_t>(model_);
    }

    void wait_for_enumeration(std::chrono::milliseconds) override {}

  private:
    std::shared_ptr<display_model_t> model_;
  };

  class OwnedDisplayTest: public BaseTest {
  protected:
    std::shared_ptr<display_model_t> model {std::make_shared<display_model_t>()};
    std::shared_ptr<model_api_t> api {std::make_shared<model_api_t>(model)};
    std::shared_ptr<model_devices_t> devices {std::make_shared<model_devices_t>(model)};
    std::unique_ptr<owned_display_configuration_t> source;
    std::unique_ptr<lease_t> lease;
    std::filesystem::path path;
    const recovery_authority_t authority {"transaction-fixture", true, true};

    void SetUp() override {
      BaseTest::SetUp();
      path = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / (std::string {::testing::UnitTest::GetInstance()->current_test_info()->name()} + ".display.json");
      std::error_code error;
      std::filesystem::remove(path, error);
    }

    void TearDown() override {
      http::test_support::set_state_replace_hook({});
      if (source) {
        std::ignore = source->cancel(source->status().generation);
      }
      lease.reset();
      source.reset();
      std::error_code error;
      std::filesystem::remove(path, error);
      display_device::test_reset_display_device();
      BaseTest::TearDown();
    }

    bool begin() {
      auto result {owned_display_configuration_t::test_begin(path, first_identity, 1, authority, api, devices)};
      EXPECT_TRUE(result);
      if (!result) {
        return false;
      }
      source = std::move(*result);
      source->test_set_retry_interval(10ms);
      return true;
    }

    bool headless_before_prepare(SingleDisplayConfiguration::DevicePreparation preparation) {
      {
        std::lock_guard lock {model->mutex};
        model->active.clear();
        model->present.clear();
        model->primary.clear();
      }
      if (!begin() || !acquire()) {
        return false;
      }
      {
        std::lock_guard lock {model->mutex};
        model->present.insert("panel");
        model->active.push_back({"panel"});
        model->primary = "panel";
        model->modes["panel"] = {{2560, 1440}, {120, 1}};
        model->hdr["panel"] = HdrState::Enabled;
      }
      return prepare(preparation);
    }

    bool reopen_active_source() {
      auto recovered {owned_display_configuration_t::test_recover(path, authority, api, devices)};
      if (!recovered) {
        return false;
      }
      source = std::move(*recovered);
      source->test_set_retry_interval(10ms);
      auto adopted {lease_t::recover(std::make_unique<model_system_t>(model), source->journal(), authority)};
      if (adopted.failure || !adopted.lease) {
        lease = std::move(adopted.lease);
        return false;
      }
      lease = std::move(adopted.lease);
      return lease->display() ? source->bind(*lease->display(), source->status().generation) : source->bind_absent(*lease, source->status().generation);
    }

    void reject_mapping_conflict(unsigned int kind) {
      ASSERT_TRUE(begin());
      ASSERT_TRUE(acquire());
      {
        std::lock_guard lock {model->mutex};
        model->alternative = true;
        model->conflicting_library_id = kind == 0;
        model->conflicting_path = kind == 1;
        model->conflicting_target = kind == 2;
        model->conflicting_source_adapter = kind == 3;
      }
      EXPECT_FALSE(source->bind(*lease->display(), 1));
      EXPECT_EQ(source->status().phase, owned_display_phase_e::reserved);
      EXPECT_TRUE(source->journal()->validate(authority));
    }

    bool acquire(const GUID &identity = first_identity, std::uint64_t generation = 1) {
      request_t request {identity, display_adapter, 1920, 1080, 60, 1, "Miranda", "fixture", true, 1, 0ms};
      auto result {lease_t::acquire_owned(std::make_unique<model_system_t>(model), request, source->journal(), authority)};
      EXPECT_FALSE(result.failure);
      EXPECT_TRUE(result.lease);
      if (result.failure || !result.lease || !result.lease->display()) {
        lease = std::move(result.lease);
        return false;
      }
      lease = std::move(result.lease);
      return source->bind(*lease->display(), generation);
    }

    bool prepare(SingleDisplayConfiguration::DevicePreparation preparation = SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay, std::uint64_t generation = 1) {
      SingleDisplayConfiguration config {.m_device_prep = preparation, .m_resolution = Resolution {1280, 720}, .m_refresh_rate = Rational {60, 1}, .m_hdr_state = HdrState::Disabled};
      return source->prepare(config, generation) && source->status().phase == owned_display_phase_e::ready;
    }

    bool release(std::uint64_t generation) {
      if (!source->restore(generation) || source->status().phase != owned_display_phase_e::removal_ready || !lease->cleanup()) {
        return false;
      }
      const auto receipt {lease->cleanup_receipt()};
      if (!receipt || !source->retire_source(*receipt, generation)) {
        return false;
      }
      lease.reset();
      return true;
    }

    bool wait_for(owned_display_phase_e phase) {
      // Fence each in-flight callback, including journal persistence and status publication.
      // The finite polling budget covers scheduling opportunities, not a native I/O deadline.
      for (unsigned int attempt {}; attempt < 200; ++attempt) {
        if (source->test_status_after_scheduled_work().phase == phase) {
          return true;
        }
        std::this_thread::sleep_for(5ms);
      }
      return false;
    }

    bool undock_first_generation() {
      if (!begin()) {
        return false;
      }
      // A synthetic prior G1 settings record exercises actual library recovery, including
      // physical mode/HDR obligations. Reload before executing so its cache matches the journal.
      SingleDisplayConfigState prior;
      prior.m_initial = {{{"dock"}}, {"dock"}};
      prior.m_modified = {{{"virtual1"}}, {{"dock", {{3840, 2160}, {60, 1}}}, {"virtual1", {{1920, 1080}, {60, 1}}}}, {{"dock", HdrState::Enabled}, {"virtual1", HdrState::Disabled}}, "dock"};
      const auto bytes {toJson(prior)};
      if (!source->journal()->store_settings({bytes.begin(), bytes.end()})) {
        return false;
      }
      source.reset();
      auto recovered {owned_display_configuration_t::test_recover(path, authority, api, devices)};
      if (!recovered) {
        return false;
      }
      source = std::move(*recovered);
      source->test_set_retry_interval(10ms);
      if (!acquire()) {
        return false;
      }
      {
        std::lock_guard lock {model->mutex};
        model->present.erase("dock");
        model->active = {{"virtual1"}};
        model->primary = "virtual1";
        model->modes["dock"] = {{1024, 768}, {30, 1}};
        model->hdr["dock"] = HdrState::Disabled;
        model->modes["virtual1"] = {{1280, 720}, {60, 1}};
      }
      return true;
    }

    void cross_generation(SingleDisplayConfiguration::DevicePreparation preparation, bool dock_stays = false, bool restart_recovery = false) {
      ASSERT_TRUE(undock_first_generation());
      ASSERT_TRUE(source->restore(1));
      ASSERT_EQ(source->status().phase, owned_display_phase_e::removal_ready);
      EXPECT_TRUE(source->status().unavailable_settings_pending);
      ASSERT_TRUE(lease->cleanup());
      const auto receipt {lease->cleanup_receipt()};
      ASSERT_TRUE(receipt);
      ASSERT_TRUE(source->retire_source(*receipt, 1));
      lease.reset();
      const auto old_journal {source->journal()};
      const auto pending {old_journal->settings()};
      ASSERT_TRUE(pending);
      ASSERT_FALSE(pending->empty());
      if (restart_recovery) {
        source.reset();
        auto recovered {owned_display_configuration_t::test_recover(path, authority, api, devices)};
        ASSERT_TRUE(recovered);
        source = std::move(*recovered);
        source->test_set_retry_interval(10ms);
      }
      ASSERT_TRUE(source->begin_next(second_identity, 2));
      EXPECT_EQ(source->journal()->settings(), pending);
      EXPECT_FALSE(old_journal->store_settings({}));
      EXPECT_FALSE(old_journal->retire(*receipt));
      EXPECT_FALSE(source->restore(1));
      EXPECT_FALSE(source->cancel(1));
      ASSERT_TRUE(acquire(second_identity, 2));
      ASSERT_TRUE(prepare(preparation, 2));
      const auto active_before {devices->getCurrentTopology()};
      unsigned int writes;
      {
        std::lock_guard lock {model->mutex};
        model->present.insert("dock");
        writes = model->topology_writes;
      }
      std::this_thread::sleep_for(40ms);
      EXPECT_EQ(devices->getCurrentTopology(), active_before);
      {
        std::lock_guard lock {model->mutex};
        EXPECT_EQ(model->topology_writes, writes);
        if (!dock_stays) {
          model->present.erase("dock");
        }
      }
      ASSERT_TRUE(release(2));
      EXPECT_TRUE(source->status().source_released);
      if (!dock_stays) {
        EXPECT_TRUE(source->status().unavailable_settings_pending);
        EXPECT_EQ(devices->getCurrentTopology(), (ActiveTopology {{"panel"}}));
        {
          std::lock_guard lock {model->mutex};
          EXPECT_EQ(model->primary, "panel");
          model->present.insert("dock");
        }
      } else {
        EXPECT_FALSE(source->status().unavailable_settings_pending);
        EXPECT_EQ(source->status().phase, owned_display_phase_e::restored);
      }
      ASSERT_TRUE(wait_for(owned_display_phase_e::restored));
      {
        std::lock_guard lock {model->mutex};
        EXPECT_EQ(model->modes.at("dock"), (DisplayMode {{3840, 2160}, {60, 1}}));
        EXPECT_EQ(model->hdr.at("dock"), HdrState::Enabled);
        EXPECT_EQ(model->primary, "dock");
        EXPECT_FALSE(model->present.contains("virtual1"));
        EXPECT_FALSE(model->present.contains("virtual2"));
      }
      EXPECT_TRUE(source->complete(2));
    }

    void reject_headless_reference(unsigned int field) {
      {
        std::lock_guard lock {model->mutex};
        model->active.clear();
        model->present.clear();
        model->primary.clear();
      }
      ASSERT_TRUE(begin());
      ASSERT_TRUE(acquire());
      ASSERT_TRUE(prepare());
      ASSERT_TRUE(source->cancel(1));
      SingleDisplayConfigState record;
      record.m_initial = {{{"virtual1"}}, {"virtual1"}};
      record.m_modified.m_topology = {{"virtual1"}};
      switch (field) {
        case 0:
          record.m_initial.m_topology.push_back({"panel"});
          break;
        case 1:
          record.m_initial.m_primary_devices.insert("panel");
          break;
        case 2:
          record.m_modified.m_topology.push_back({"panel"});
          break;
        case 3:
          record.m_modified.m_original_modes["panel"] = {{1920, 1080}, {60, 1}};
          break;
        case 4:
          record.m_modified.m_original_hdr_states["panel"] = HdrState::Enabled;
          break;
        case 5:
          record.m_modified.m_original_primary_device = "panel";
          break;
        case 6:
          record.m_modified.m_interim_primary_device = "panel";
          break;
      }
      const auto bytes {toJson(record)};
      ASSERT_TRUE(source->journal()->store_settings({bytes.begin(), bytes.end()}));
      source.reset();
      auto recovered {owned_display_configuration_t::test_recover(path, authority, api, devices)};
      ASSERT_TRUE(recovered);
      source = std::move(*recovered);
      ASSERT_TRUE(source->bind(*lease->display(), 1));
      const auto prior {source->journal()->settings()};
      const auto active {devices->getCurrentTopology()};
      ASSERT_TRUE(source->restore(1));
      EXPECT_EQ(source->status().phase, owned_display_phase_e::cleanup_pending);
      EXPECT_EQ(source->status().restore_error, owned_display_error_e::headless_ownership_unproven);
      EXPECT_FALSE(source->status().removal_permitted);
      EXPECT_EQ(source->journal()->settings(), prior);
      EXPECT_EQ(devices->getCurrentTopology(), active);
    }
  };
}  // namespace

TEST_F(OwnedDisplayTest, CapturesRealPreAddTopologyAndPrimary) {
  ASSERT_TRUE(begin());
  SingleDisplayConfigState before;
  ASSERT_TRUE(fromJson(source->journal()->record().baseline, before));
  EXPECT_EQ(before.m_initial.m_topology, (ActiveTopology {{"dock"}}));
  EXPECT_EQ(before.m_initial.m_primary_devices, (StringSet {"dock"}));
  ASSERT_TRUE(acquire());
  ASSERT_TRUE(prepare());
  EXPECT_EQ(source->status().display_name, "virtual1-capture");
  ASSERT_TRUE(source->status().observed);
  EXPECT_EQ(source->status().observed->m_resolution, (Resolution {1280, 720}));
  ASSERT_TRUE(release(1));
  EXPECT_EQ(devices->getCurrentTopology(), (ActiveTopology {{"dock"}}));
  EXPECT_TRUE(source->complete(1));
}

TEST_F(OwnedDisplayTest, RejectsAmbiguousExactMappingAndStaleGenerations) {
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  {
    std::lock_guard lock {model->mutex};
    model->ambiguous = true;
  }
  EXPECT_FALSE(source->bind(*lease->display(), 1));
  EXPECT_FALSE(source->restore(2));
  EXPECT_FALSE(source->cancel(2));
  EXPECT_FALSE(source->complete(2));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::reserved);
}

TEST_F(OwnedDisplayTest, FailedDisplayQueryCannotMasqueradeAsInitiallyHeadless) {
  {
    std::lock_guard lock {model->mutex};
    model->query_failure = true;
  }
  EXPECT_FALSE(owned_display_configuration_t::test_begin(path, first_identity, 1, authority, api, devices));
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(OwnedDisplayTest, BoundedUnavailableApplyReportsFailureWithoutClearingIntent) {
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  {
    std::lock_guard lock {model->mutex};
    model->api_unavailable = true;
  }
  EXPECT_TRUE(source->prepare({}, 1, 2));
  ASSERT_TRUE(wait_for(owned_display_phase_e::failed));
  EXPECT_EQ(source->status().attempts, 2);
  EXPECT_EQ(source->status().apply_result, SettingsManagerInterface::ApplyResult::ApiTemporarilyUnavailable);
  EXPECT_TRUE(source->journal()->validate(authority));
  EXPECT_FALSE(source->complete(1));
}

TEST_F(OwnedDisplayTest, DelayedRestorationKeepsLeaseHeartbeatAndSeparatesCompletion) {
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  ASSERT_TRUE(prepare());
  ASSERT_TRUE(source->restore(1, 50ms));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::reverting);
  std::function<void()> tick;
  {
    std::lock_guard lock {model->mutex};
    tick = model->tick;
  }
  ASSERT_TRUE(tick);
  tick();
  EXPECT_FALSE(lease->cleanup_receipt());
  ASSERT_TRUE(wait_for(owned_display_phase_e::removal_ready));
  EXPECT_TRUE(source->status().removal_permitted);
  EXPECT_FALSE(source->status().source_released);
  EXPECT_FALSE(source->complete(1));
}

TEST_F(OwnedDisplayTest, RestoreFailureRetainsLeaseJournalAndLegacyExclusion) {
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  ASSERT_TRUE(prepare());
  {
    std::lock_guard lock {model->mutex};
    model->topology_failure = true;
  }
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::cleanup_pending);
  EXPECT_FALSE(source->status().removal_permitted);
  EXPECT_FALSE(display_device::reset_persistence());
  display_device::configure_display(SingleDisplayConfiguration {});
  display_device::revert_configuration();
  EXPECT_TRUE(source->journal()->validate(authority));
  EXPECT_TRUE(lease->display());
  {
    std::lock_guard lock {model->mutex};
    EXPECT_TRUE(model->tick);
    model->topology_failure = false;
  }
  EXPECT_TRUE(release(1));
}

TEST_F(OwnedDisplayTest, InitiallyHeadlessRestorationDoesNotDisableNewUnownedActiveDisplay) {
  {
    std::lock_guard lock {model->mutex};
    model->active.clear();
    model->present.clear();
    model->primary.clear();
  }
  ASSERT_TRUE(begin());
  EXPECT_TRUE(source->status().initially_headless);
  EXPECT_TRUE(source->journal()->record().baseline.empty());
  ASSERT_TRUE(acquire());
  ASSERT_TRUE(prepare());
  {
    std::lock_guard lock {model->mutex};
    model->present.insert("panel");
    model->active.push_back({"panel"});
  }
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::removal_ready);
  EXPECT_TRUE(source->status().removal_permitted);
  EXPECT_EQ(devices->getCurrentTopology(), (ActiveTopology {{"panel"}}));
  {
    std::lock_guard lock {model->mutex};
    EXPECT_EQ(model->modes.at("virtual1").m_resolution, (Resolution {1280, 720}));
  }
  ASSERT_TRUE(release(1));
  EXPECT_TRUE(source->complete(1));
}

TEST_F(OwnedDisplayTest, CancelAndStatusDoNotRequireCallbackToEnterOwner) {
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  std::mutex gate;
  std::condition_variable condition;
  bool entered {}, released {};
  {
    std::lock_guard lock {model->mutex};
    model->api_check_hook = [&] {
      std::unique_lock lock {gate};
      entered = true;
      condition.notify_all();
      condition.wait(lock, [&] {
        return released;
      });
    };
  }
  std::thread preparing {[&] {
    std::ignore = source->prepare({}, 1);
  }};
  {
    std::unique_lock lock {gate};
    EXPECT_TRUE(condition.wait_for(lock, 1s, [&] {
      return entered;
    }));
  }
  EXPECT_EQ(source->status().phase, owned_display_phase_e::preparing);
  std::thread cancelling {[&] {
    EXPECT_TRUE(source->cancel(1));
  }};
  {
    std::lock_guard lock {gate};
    released = true;
    condition.notify_all();
  }
  preparing.join();
  cancelling.join();
  EXPECT_EQ(source->status().phase, owned_display_phase_e::cancelled);
  EXPECT_TRUE(source->journal()->validate(authority));
}

TEST_F(OwnedDisplayTest, ReturnedDockRemainsPrimaryWhenNextPrimarySourceReleases) {
  cross_generation(SingleDisplayConfiguration::DevicePreparation::EnsurePrimary, true);
}

TEST_F(OwnedDisplayTest, ReturnedDockRemainsPrimaryWhenNextOnlyDisplaySourceReleases) {
  cross_generation(SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay, true);
}

TEST_F(OwnedDisplayTest, RetiredRecoveryCanRestartAndAdmitNewGenerationWhileDockAbsent) {
  ASSERT_TRUE(undock_first_generation());
  ASSERT_TRUE(release(1));
  source.reset();
  auto recovered {owned_display_configuration_t::test_recover(path, authority, api, devices)};
  ASSERT_TRUE(recovered);
  source = std::move(*recovered);
  EXPECT_TRUE(source->status().source_released);
  ASSERT_TRUE(source->begin_next(second_identity, 2));
  ASSERT_TRUE(acquire(second_identity, 2));
  EXPECT_TRUE(prepare(SingleDisplayConfiguration::DevicePreparation::EnsurePrimary, 2));
}

TEST_F(OwnedDisplayTest, MissingOriginalAndRecoveredFallbackBlocksAdmissionWithoutDroppingRecovery) {
  ASSERT_TRUE(undock_first_generation());
  ASSERT_TRUE(release(1));
  {
    std::lock_guard lock {model->mutex};
    model->present.erase("panel");
    model->active.clear();
    model->primary.clear();
  }
  const auto prior {source->journal()};
  EXPECT_FALSE(source->begin_next(second_identity, 2));
  EXPECT_EQ(source->status().generation, 1);
  EXPECT_TRUE(prior->validate(authority));
  ASSERT_TRUE(prior->settings());
  EXPECT_FALSE(prior->settings()->empty());
}

TEST_F(OwnedDisplayTest, FailedRetirementOrRolloverKeepsPriorAuthorityAndBlocksAdd) {
  ASSERT_TRUE(undock_first_generation());
  ASSERT_TRUE(source->restore(1));
  ASSERT_EQ(source->status().phase, owned_display_phase_e::removal_ready);
  ASSERT_TRUE(lease->cleanup());
  const auto receipt {lease->cleanup_receipt()};
  ASSERT_TRUE(receipt);
  http::test_support::set_state_replace_hook([](const auto &, const auto &, const auto &) {
    return std::make_optional<std::uint32_t>(ERROR_ACCESS_DENIED);
  });
  EXPECT_FALSE(source->retire_source(*receipt, 1));
  EXPECT_FALSE(source->status().source_released);
  EXPECT_FALSE(source->begin_next(second_identity, 2));
  http::test_support::set_state_replace_hook({});
  ASSERT_TRUE(source->retire_source(*receipt, 1));
  lease.reset();
  const auto old_journal {source->journal()};
  ASSERT_TRUE(source->cancel(1));
  http::test_support::set_state_replace_hook([](const auto &, const auto &, const auto &) {
    return std::make_optional<std::uint32_t>(ERROR_ACCESS_DENIED);
  });
  EXPECT_FALSE(source->begin_next(second_identity, 2));
  http::test_support::set_state_replace_hook({});
  EXPECT_EQ(source->status().generation, 1);
  EXPECT_TRUE(old_journal->validate(authority));
  ASSERT_TRUE(source->begin_next(second_identity, 2));
  EXPECT_EQ(source->status().generation, 2);
}

TEST_F(OwnedDisplayTest, HeadlessDetachPreservesPhysicalCloneModeHdrAndPrimary) {
  {
    std::lock_guard lock {model->mutex};
    model->active.clear();
    model->present.clear();
    model->primary.clear();
  }
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  ASSERT_TRUE(prepare());
  {
    std::lock_guard lock {model->mutex};
    model->present.insert("panel");
    model->present.insert("dock");
    model->active = {{"virtual1"}, {"panel", "dock"}};
    model->primary = "panel";
    model->topology_requests.clear();
  }
  const auto modes {devices->getCurrentDisplayModes({"panel", "dock"})};
  const auto hdr {devices->getCurrentHdrStates({"panel", "dock"})};
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::removal_ready);
  EXPECT_EQ(devices->getCurrentTopology(), (ActiveTopology {{"panel", "dock"}}));
  EXPECT_EQ(devices->getCurrentDisplayModes({"panel", "dock"}), modes);
  EXPECT_EQ(devices->getCurrentHdrStates({"panel", "dock"}), hdr);
  EXPECT_TRUE(devices->isPrimary("panel"));
  {
    std::lock_guard lock {model->mutex};
    ASSERT_EQ(model->topology_requests.size(), 1);
    EXPECT_EQ(model->topology_requests.front(), (ActiveTopology {{"panel", "dock"}}));
  }
}

TEST_F(OwnedDisplayTest, HeadlessDiscardFailureRetainsSettingsAndForbidsRemovalUntilRetry) {
  {
    std::lock_guard lock {model->mutex};
    model->active.clear();
    model->present.clear();
    model->primary.clear();
  }
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  ASSERT_TRUE(prepare());
  const auto saved {source->journal()->settings()};
  ASSERT_TRUE(saved);
  ASSERT_FALSE(saved->empty());
  {
    std::lock_guard lock {model->mutex};
    model->present.insert("panel");
    model->active.push_back({"panel"});
  }
  http::test_support::set_state_replace_hook([](const auto &, const auto &, const auto &) {
    return std::make_optional<std::uint32_t>(ERROR_ACCESS_DENIED);
  });
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::cleanup_pending);
  EXPECT_FALSE(source->status().removal_permitted);
  EXPECT_EQ(source->journal()->settings(), saved);
  EXPECT_EQ(devices->getCurrentTopology(), (ActiveTopology {{"panel"}}));
  http::test_support::set_state_replace_hook({});
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::removal_ready);
  ASSERT_TRUE(source->journal()->settings());
  EXPECT_TRUE(source->journal()->settings()->empty());
  {
    std::lock_guard lock {model->mutex};
    model->remove_failure = true;
  }
  EXPECT_FALSE(lease->cleanup());
  EXPECT_TRUE(source->journal()->validate(authority));
  EXPECT_FALSE(source->begin_next(second_identity, 2));
  {
    std::lock_guard lock {model->mutex};
    model->remove_failure = false;
  }
  EXPECT_TRUE(release(1));
}

TEST_F(OwnedDisplayTest, EmptyHeadlessCacheStillReleasesRetainedAudio) {
  {
    std::lock_guard lock {model->mutex};
    model->active.clear();
    model->present.clear();
    model->primary.clear();
  }
  const auto audio {std::make_shared<model_audio_t>()};
  auto begun {owned_display_configuration_t::test_begin(path, first_identity, 1, authority, api, devices, nullptr, audio)};
  ASSERT_TRUE(begun);
  source = std::move(*begun);
  ASSERT_TRUE(acquire());
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::removal_ready);
  EXPECT_FALSE(audio->captured);
  EXPECT_EQ(audio->releases, 1);
}

TEST_F(OwnedDisplayTest, RestartAfterHeadlessDiscardBeforeRemovalPreservesPhysicalOutput) {
  {
    std::lock_guard lock {model->mutex};
    model->active.clear();
    model->present.clear();
    model->primary.clear();
  }
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  ASSERT_TRUE(prepare());
  {
    std::lock_guard lock {model->mutex};
    model->present.insert("panel");
    model->active.push_back({"panel"});
  }
  ASSERT_TRUE(source->restore(1));
  ASSERT_EQ(source->status().phase, owned_display_phase_e::removal_ready);
  ASSERT_TRUE(source->journal()->settings());
  ASSERT_TRUE(source->journal()->settings()->empty());
  // Failed explicit cleanup and abandonment leave intent for restart before exact removal.
  {
    std::lock_guard lock {model->mutex};
    model->remove_failure = true;
  }
  EXPECT_FALSE(lease->cleanup());
  ASSERT_TRUE(source->cancel(1));
  source.reset();
  lease.reset();
  {
    std::lock_guard lock {model->mutex};
    model->remove_failure = false;
  }
  auto recovered {owned_display_configuration_t::test_recover(path, authority, api, devices)};
  ASSERT_TRUE(recovered);
  source = std::move(*recovered);
  auto adopted {lease_t::recover(std::make_unique<model_system_t>(model), source->journal(), authority)};
  ASSERT_FALSE(adopted.failure);
  ASSERT_TRUE(adopted.lease);
  lease = std::move(adopted.lease);
  ASSERT_TRUE(lease->display());
  ASSERT_TRUE(source->bind(*lease->display(), 1));
  ASSERT_TRUE(release(1));
  EXPECT_EQ(devices->getCurrentTopology(), (ActiveTopology {{"panel"}}));
  EXPECT_TRUE(source->complete(1));
}

TEST_F(OwnedDisplayTest, HeadlessDiscardRejectsPhysicalInitialTopology) {
  reject_headless_reference(0);
}

TEST_F(OwnedDisplayTest, HeadlessDiscardRejectsPhysicalInitialPrimary) {
  reject_headless_reference(1);
}

TEST_F(OwnedDisplayTest, HeadlessDiscardRejectsPhysicalModifiedTopology) {
  reject_headless_reference(2);
}

TEST_F(OwnedDisplayTest, HeadlessDiscardRejectsPhysicalModeObligation) {
  reject_headless_reference(3);
}

TEST_F(OwnedDisplayTest, HeadlessDiscardRejectsPhysicalHdrObligation) {
  reject_headless_reference(4);
}

TEST_F(OwnedDisplayTest, HeadlessDiscardRejectsPhysicalOriginalPrimary) {
  reject_headless_reference(5);
}

TEST_F(OwnedDisplayTest, HeadlessDiscardRejectsPhysicalInterimPrimary) {
  reject_headless_reference(6);
}

TEST_F(OwnedDisplayTest, UnarmedFailedAttemptCanRetireWithoutRemovingUnknownExistingGuid) {
  ASSERT_TRUE(begin());
  // The foreign output appeared before exclusive-use preflight. It is never adopted or removed.
  {
    std::lock_guard lock {model->mutex};
    model->present.insert("virtual1");
  }
  request_t request {first_identity, display_adapter, 1920, 1080, 60, 1, "Miranda", "fixture", true, 1, 0ms};
  auto failed {lease_t::acquire_owned(std::make_unique<model_system_t>(model), request, source->journal(), authority)};
  ASSERT_TRUE(failed.failure);
  EXPECT_EQ(failed.failure->code, errc_t::already_present);
  ASSERT_TRUE(failed.cleanup_receipt);
  EXPECT_EQ(failed.cleanup_receipt->kind(), cleanup_receipt_t::kind_e::unarmed);
  EXPECT_TRUE(source->retire_source(*failed.cleanup_receipt, 1));
  {
    std::lock_guard lock {model->mutex};
    EXPECT_EQ(model->removals, 0);
    EXPECT_TRUE(model->present.contains("virtual1"));
  }
}

TEST_F(OwnedDisplayTest, RestartAfterRolloverBeforeAddKeepsPendingSettingsAndAllowsOwnedAcquisition) {
  ASSERT_TRUE(undock_first_generation());
  ASSERT_TRUE(release(1));
  ASSERT_TRUE(source->begin_next(second_identity, 2));
  const auto settings {source->journal()->settings()};
  ASSERT_TRUE(settings);
  ASSERT_FALSE(settings->empty());
  EXPECT_FALSE(source->journal()->add_was_armed());
  source.reset();
  auto recovered {owned_display_configuration_t::test_recover(path, authority, api, devices)};
  ASSERT_TRUE(recovered);
  source = std::move(*recovered);
  EXPECT_EQ(source->status().generation, 2);
  EXPECT_EQ(source->journal()->settings(), settings);
  EXPECT_FALSE(source->status().source_released);
  ASSERT_TRUE(acquire(second_identity, 2));
  EXPECT_TRUE(prepare(SingleDisplayConfiguration::DevicePreparation::EnsurePrimary, 2));
}

TEST_F(OwnedDisplayTest, PrimaryApplyFailureRemainsSeparateFromFailedRestoration) {
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  {
    std::lock_guard lock {model->mutex};
    model->mode_failure = true;
  }
  EXPECT_FALSE(prepare());
  EXPECT_EQ(source->status().apply_result, SettingsManagerInterface::ApplyResult::DisplayModePrepFailed);
  {
    std::lock_guard lock {model->mutex};
    model->topology_failure = true;
  }
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::cleanup_pending);
  EXPECT_EQ(source->status().apply_result, SettingsManagerInterface::ApplyResult::DisplayModePrepFailed);
  ASSERT_TRUE(source->status().revert_result);
  EXPECT_NE(source->status().revert_result, SettingsManagerInterface::RevertResult::Ok);
  EXPECT_TRUE(lease->display());
  EXPECT_TRUE(source->journal()->validate(authority));
}

TEST_F(OwnedDisplayTest, RestartAfterFailedArmedAddRestoresBeforeExactAbsentCleanup) {
  ASSERT_TRUE(begin());
  {
    std::lock_guard lock {model->mutex};
    model->add_failure = true;
  }
  request_t request {first_identity, display_adapter, 1920, 1080, 60, 1, "Miranda", "fixture", true, 1, 0ms};
  auto failed {lease_t::acquire_owned(std::make_unique<model_system_t>(model), request, source->journal(), authority)};
  ASSERT_TRUE(failed.failure);
  ASSERT_TRUE(failed.lease);
  EXPECT_TRUE(failed.output_uncertain);
  lease = std::move(failed.lease);
  {
    std::lock_guard lock {model->mutex};
    EXPECT_EQ(model->removals, 0);
    EXPECT_TRUE(model->tick);
  }
  ASSERT_TRUE(source->cancel(1));
  source.reset();
  lease.reset();
  {
    std::lock_guard lock {model->mutex};
    model->add_failure = false;
    EXPECT_EQ(model->removals, 0);
  }
  auto recovered {owned_display_configuration_t::test_recover(path, authority, api, devices)};
  ASSERT_TRUE(recovered);
  source = std::move(*recovered);
  auto adopted {lease_t::recover(std::make_unique<model_system_t>(model), source->journal(), authority)};
  ASSERT_FALSE(adopted.failure);
  ASSERT_TRUE(adopted.lease);
  lease = std::move(adopted.lease);
  ASSERT_TRUE(lease->identity_absent());
  ASSERT_TRUE(source->bind_absent(*lease, 1));
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::removal_ready);
  {
    std::lock_guard lock {model->mutex};
    EXPECT_EQ(model->removals, 0);
  }
  EXPECT_TRUE(release(1));
  EXPECT_TRUE(source->complete(1));
}

TEST_F(OwnedDisplayTest, PendingDockCanStayAbsentThroughNextPrimarySourceRelease) {
  cross_generation(SingleDisplayConfiguration::DevicePreparation::EnsurePrimary);
}

TEST_F(OwnedDisplayTest, PendingDockCanStayAbsentThroughNextOnlyDisplaySourceRelease) {
  cross_generation(SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay);
}

TEST_F(OwnedDisplayTest, AutomaticRecoveryPublishesRestoredOnlyAfterDurableClear) {
  ASSERT_TRUE(undock_first_generation());
  ASSERT_TRUE(release(1));
  ASSERT_EQ(source->status().phase, owned_display_phase_e::recovering);
  ASSERT_TRUE(source->status().unavailable_settings_pending);
  EXPECT_EQ(devices->getCurrentTopology(), (ActiveTopology {{"panel"}}));

  struct publication_probe_t {
    std::mutex mutex;
    std::optional<owned_display_status_t> before_clear;
  };

  const auto probe {std::make_shared<publication_probe_t>()};
  http::test_support::set_state_replace_hook([probe, transaction = source.get(), journal_path = path](const auto &destination, const auto &temporary, const auto &) -> std::optional<std::uint32_t> {
    if (destination == journal_path) {
      std::ifstream input {temporary, std::ios::binary};
      const auto envelope = nlohmann::json::parse(input);
      if (envelope.at("settings").get<std::string>().empty()) {
        const auto snapshot {transaction->status()};
        std::lock_guard lock {probe->mutex};
        probe->before_clear = snapshot;
      }
    }
    return std::nullopt;
  });
  {
    std::lock_guard lock {model->mutex};
    model->present.insert("dock");
  }
  ASSERT_TRUE(wait_for(owned_display_phase_e::restored));
  http::test_support::set_state_replace_hook({});
  {
    std::lock_guard lock {probe->mutex};
    ASSERT_TRUE(probe->before_clear);
    EXPECT_EQ(probe->before_clear->generation, 1);
    EXPECT_EQ(probe->before_clear->phase, owned_display_phase_e::recovering);
    EXPECT_TRUE(probe->before_clear->source_released);
    EXPECT_TRUE(probe->before_clear->unavailable_settings_pending);
  }
  EXPECT_FALSE(source->status().unavailable_settings_pending);
  const auto settings {source->journal()->settings()};
  ASSERT_TRUE(settings);
  EXPECT_TRUE(settings->empty());
  EXPECT_EQ(devices->getCurrentTopology(), (ActiveTopology {{"dock"}, {"panel"}}));
  {
    std::lock_guard lock {model->mutex};
    EXPECT_EQ(model->modes.at("dock"), (DisplayMode {{3840, 2160}, {60, 1}}));
    EXPECT_EQ(model->hdr.at("dock"), HdrState::Enabled);
    EXPECT_EQ(model->modes.at("panel"), (DisplayMode {{1920, 1080}, {60, 1}}));
    EXPECT_EQ(model->hdr.at("panel"), HdrState::Disabled);
    EXPECT_EQ(model->primary, "dock");
    EXPECT_FALSE(model->present.contains("virtual1"));
    EXPECT_EQ(model->removals, 1);
  }
  EXPECT_TRUE(source->complete(1));
}

TEST_F(OwnedDisplayTest, ReturnedDockPrimarySurvivesRecoveryOnlyRestartAndNextSourceRelease) {
  cross_generation(SingleDisplayConfiguration::DevicePreparation::EnsurePrimary, true, true);
}

TEST_F(OwnedDisplayTest, PhysicalBeforeHeadlessOnlyPreparationUsesOrdinaryRecovery) {
  ASSERT_TRUE(headless_before_prepare(SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay));
  EXPECT_TRUE(source->status().initially_headless);
  EXPECT_EQ(devices->getCurrentTopology(), (ActiveTopology {{"virtual1"}}));
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().revert_result, SettingsManagerInterface::RevertResult::Ok);
  EXPECT_EQ(source->status().phase, owned_display_phase_e::removal_ready);
  EXPECT_EQ(devices->getCurrentTopology(), (ActiveTopology {{"panel"}}));
  EXPECT_TRUE(devices->isPrimary("panel"));
  EXPECT_EQ(devices->getCurrentDisplayModes({"panel"}), (DeviceDisplayModeMap {{"panel", {{2560, 1440}, {120, 1}}}}));
  EXPECT_EQ(devices->getCurrentHdrStates({"panel"}), (HdrStateMap {{"panel", HdrState::Enabled}}));
  ASSERT_TRUE(release(1));
  EXPECT_TRUE(source->complete(1));
}

TEST_F(OwnedDisplayTest, PhysicalBeforeHeadlessPrimaryPreparationUsesOrdinaryRecovery) {
  ASSERT_TRUE(headless_before_prepare(SingleDisplayConfiguration::DevicePreparation::EnsurePrimary));
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().revert_result, SettingsManagerInterface::RevertResult::Ok);
  EXPECT_EQ(source->status().phase, owned_display_phase_e::removal_ready);
  EXPECT_EQ(devices->getCurrentTopology(), (ActiveTopology {{"panel"}}));
  EXPECT_TRUE(devices->isPrimary("panel"));
  ASSERT_TRUE(release(1));
  EXPECT_TRUE(source->complete(1));
}

TEST_F(OwnedDisplayTest, ValidHeadlessPhysicalObligationsSurviveFailedRestoreAndRestart) {
  ASSERT_TRUE(headless_before_prepare(SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay));
  const auto settings {source->journal()->settings()};
  ASSERT_TRUE(settings);
  ASSERT_FALSE(settings->empty());
  {
    std::lock_guard lock {model->mutex};
    model->topology_failure = true;
  }
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::cleanup_pending);
  EXPECT_FALSE(source->status().removal_permitted);
  ASSERT_TRUE(source->journal()->settings());
  EXPECT_FALSE(source->journal()->settings()->empty());
  ASSERT_TRUE(source->cancel(1));
  source.reset();
  lease.reset();
  {
    std::lock_guard lock {model->mutex};
    model->topology_failure = false;
  }
  ASSERT_TRUE(reopen_active_source());
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().revert_result, SettingsManagerInterface::RevertResult::Ok);
  EXPECT_EQ(source->status().phase, owned_display_phase_e::removal_ready);
  EXPECT_EQ(devices->getCurrentTopology(), (ActiveTopology {{"panel"}}));
  ASSERT_TRUE(release(1));
  EXPECT_TRUE(source->complete(1));
}

TEST_F(OwnedDisplayTest, DurableMappingRecoversVanishedHeadlessOutputWithoutTouchingNewPhysicalScreen) {
  {
    std::lock_guard lock {model->mutex};
    model->active.clear();
    model->present.clear();
    model->primary.clear();
  }
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  ASSERT_TRUE(prepare());
  ASSERT_TRUE(source->journal()->mapping());
  EXPECT_EQ(source->journal()->mapping()->device_id, "virtual1");
  ASSERT_TRUE(source->cancel(1));
  source.reset();
  lease.reset();
  {
    std::lock_guard lock {model->mutex};
    model->present.erase("virtual1");
    model->present.insert("panel");
    model->active = {{"panel"}};
    model->primary = "panel";
    model->modes["panel"] = {{2560, 1440}, {120, 1}};
    model->hdr["panel"] = HdrState::Enabled;
    model->topology_requests.clear();
  }
  ASSERT_TRUE(reopen_active_source());
  ASSERT_TRUE(lease->identity_absent());
  EXPECT_EQ(source->status().device_id, "virtual1");
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::removal_ready);
  EXPECT_FALSE(source->status().revert_result);
  EXPECT_EQ(devices->getCurrentTopology(), (ActiveTopology {{"panel"}}));
  EXPECT_EQ(devices->getCurrentDisplayModes({"panel"}), (DeviceDisplayModeMap {{"panel", {{2560, 1440}, {120, 1}}}}));
  EXPECT_EQ(devices->getCurrentHdrStates({"panel"}), (HdrStateMap {{"panel", HdrState::Enabled}}));
  {
    std::lock_guard lock {model->mutex};
    EXPECT_TRUE(model->topology_requests.empty());
    EXPECT_EQ(model->removals, 0);
  }
  ASSERT_TRUE(release(1));
  EXPECT_TRUE(source->complete(1));
  {
    std::lock_guard lock {model->mutex};
    EXPECT_EQ(model->removals, 1);
  }
}

TEST_F(OwnedDisplayTest, AbsenceDoesNotAuthorizeCachedReferencesWithoutDurableOwnedMapping) {
  {
    std::lock_guard lock {model->mutex};
    model->active.clear();
    model->present.clear();
    model->primary.clear();
  }
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  ASSERT_TRUE(prepare());
  ASSERT_TRUE(source->cancel(1));
  source.reset();
  lease.reset();
  ASSERT_TRUE(http::update_state_file_bytes(path.string(), [](const std::string &bytes) -> std::optional<std::string> {
    auto data = nlohmann::json::parse(bytes);
    data["mapping"] = nullptr;
    return data.dump();
  }));
  {
    std::lock_guard lock {model->mutex};
    model->present.erase("virtual1");
    model->present.insert("panel");
    model->active = {{"panel"}};
    model->primary = "panel";
    model->topology_requests.clear();
  }
  ASSERT_TRUE(reopen_active_source());
  ASSERT_TRUE(lease->identity_absent());
  EXPECT_TRUE(source->status().device_id.empty());
  const auto settings {source->journal()->settings()};
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::cleanup_pending);
  EXPECT_EQ(source->status().restore_error, owned_display_error_e::headless_ownership_unproven);
  EXPECT_EQ(source->journal()->settings(), settings);
  {
    std::lock_guard lock {model->mutex};
    EXPECT_TRUE(model->topology_requests.empty());
    EXPECT_EQ(model->removals, 0);
  }
}

TEST_F(OwnedDisplayTest, NewPhysicalPresenceDoesNotValidateUnprovenCachedReferences) {
  {
    std::lock_guard lock {model->mutex};
    model->active.clear();
    model->present.clear();
    model->primary.clear();
  }
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  ASSERT_TRUE(prepare());
  ASSERT_TRUE(source->cancel(1));
  SingleDisplayConfigState stored;
  const auto saved {source->journal()->settings()};
  ASSERT_TRUE(saved);
  ASSERT_TRUE(fromJson(std::string {saved->begin(), saved->end()}, stored));
  stored.m_modified.m_original_modes["panel"] = {{1024, 768}, {30, 1}};
  const auto bytes {toJson(stored)};
  ASSERT_TRUE(source->journal()->store_settings({bytes.begin(), bytes.end()}));
  source.reset();
  lease.reset();
  {
    std::lock_guard lock {model->mutex};
    model->present.erase("virtual1");
    model->present.insert("panel");
    model->active = {{"panel"}};
    model->primary = "panel";
    model->topology_requests.clear();
  }
  ASSERT_TRUE(reopen_active_source());
  const auto settings {source->journal()->settings()};
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::cleanup_pending);
  EXPECT_EQ(source->journal()->settings(), settings);
  EXPECT_EQ(devices->getCurrentDisplayModes({"panel"}), (DeviceDisplayModeMap {{"panel", {{1920, 1080}, {60, 1}}}}));
  {
    std::lock_guard lock {model->mutex};
    EXPECT_TRUE(model->topology_requests.empty());
    EXPECT_EQ(model->removals, 0);
  }
}

TEST_F(OwnedDisplayTest, AlternativeSourcePathsUseVerifiedActiveSelectorThroughRestart) {
  {
    std::lock_guard lock {model->mutex};
    model->alternative = true;
  }
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  ASSERT_TRUE(lease->refresh_display(1, 0ms));
  ASSERT_TRUE(prepare());
  EXPECT_EQ(source->status().display_name, "virtual1-capture");
  ASSERT_TRUE(source->cancel(1));
  source.reset();
  lease.reset();
  ASSERT_TRUE(reopen_active_source());
  ASSERT_TRUE(lease->display());
  EXPECT_TRUE(lease->display()->active);
  ASSERT_TRUE(source->restore(1));
  EXPECT_EQ(source->status().phase, owned_display_phase_e::removal_ready);
  ASSERT_TRUE(release(1));
  EXPECT_TRUE(source->complete(1));
}

TEST_F(OwnedDisplayTest, AlternativePathWithDifferentLibraryIdentityIsRejected) {
  reject_mapping_conflict(0);
}

TEST_F(OwnedDisplayTest, AlternativePathWithDifferentMonitorPathIsRejected) {
  reject_mapping_conflict(1);
}

TEST_F(OwnedDisplayTest, AlternativePathWithDifferentTargetIsRejected) {
  reject_mapping_conflict(2);
}

TEST_F(OwnedDisplayTest, AlternativePathWithDifferentSourceAdapterIsRejected) {
  reject_mapping_conflict(3);
}

TEST_F(OwnedDisplayTest, MappingMustPersistBeforeLibraryCanReferToOwnedIdentity) {
  {
    std::lock_guard lock {model->mutex};
    model->active.clear();
    model->present.clear();
    model->primary.clear();
  }
  ASSERT_TRUE(begin());
  request_t request {first_identity, display_adapter, 1920, 1080, 60, 1, "Miranda", "fixture", true, 1, 0ms};
  auto acquired {lease_t::acquire_owned(std::make_unique<model_system_t>(model), request, source->journal(), authority)};
  ASSERT_FALSE(acquired.failure);
  ASSERT_TRUE(acquired.lease);
  lease = std::move(acquired.lease);
  ASSERT_TRUE(lease->display());
  http::test_support::set_state_replace_hook([](const auto &, const auto &, const auto &) {
    return std::make_optional<std::uint32_t>(ERROR_ACCESS_DENIED);
  });
  EXPECT_FALSE(source->bind(*lease->display(), 1));
  EXPECT_FALSE(source->prepare({}, 1));
  http::test_support::set_state_replace_hook({});
  EXPECT_FALSE(source->journal()->mapping());
  ASSERT_TRUE(source->journal()->settings());
  EXPECT_TRUE(source->journal()->settings()->empty());
  ASSERT_TRUE(source->bind(*lease->display(), 1));
  ASSERT_TRUE(release(1));
  EXPECT_TRUE(source->complete(1));
}
#endif
