/**
 * @file tests/unit/test_owned_display_device.cpp
 * @brief Owned display transactions with the real settings algorithm and fake Windows/driver boundaries.
 */
// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <condition_variable>
  #include <cstring>
  #include <filesystem>
  #include <fstream>
  #include <future>
  #include <map>
  #include <mutex>
  #include <stdexcept>
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
  #include <display_device/settings_persistence_interface.h>
  #include <display_device/windows/json.h>
  #include <display_device/windows/persistent_state.h>
  #include <display_device/windows/settings_manager.h>
  #include <display_device/windows/win_api_layer_interface.h>
  #include <display_device/windows/win_display_device_interface.h>
  #include <nlohmann/json.hpp>

  // local includes
  #include <src/display_device.h>
  #include <src/httpcommon.h>
  #include <src/nvhttp.h>
  #include <src/platform/windows/source_backend.h>
  #include <src/platform/windows/source_startup.h>
  #include <src/process.h>

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
    unsigned int additions {};
    unsigned int adapter_selections {};
    unsigned int settings_mutations {};
    unsigned int heartbeat_creations {};
    unsigned int heartbeat_creation_throws {};
    unsigned int watchdog_timeout {3};
    unsigned int transport_closes {};
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
    bool capture_metadata {};  ///< Enable complete D readback only for the concrete-backend cases.
    bool heartbeat_failure {};  ///< Inject a sticky asynchronous ping failure.
    bool close_failure {};  ///< Keep a removed lease alive until its transport close succeeds.
    bool enumeration_exception {};  ///< Throw after recovery has adopted the journal-backed lease.
    bool protocol_failure {};  ///< Fail recovery before output adoption, allowing close-only retry coverage.
    std::optional<display_t> recovery_display_override;  ///< Driver observation only; never changes persisted mapping.
    std::string recovery_library_id;  ///< Current Windows identity override for the recorded virtual target.
    bool recovery_driver_absent {};  ///< Driver absence conflicting with a still-present Windows mapping.
    std::optional<DISPLAYCONFIG_PATH_INFO> retained_windows_path;  ///< Explicit stale Windows path, independent of current driver enumeration.

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
      if (id == "virtual1" && recovery_display_override) {
        return *recovery_display_override;
      }
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
        path.targetInfo.targetAvailable = TRUE;
        if (model_->capture_metadata) {
          path.targetInfo.refreshRate = {model_->modes.at(id).m_refresh_rate.m_numerator, model_->modes.at(id).m_refresh_rate.m_denominator};
          path.sourceInfo.sourceModeInfoIdx = result.m_modes.size();
          DISPLAYCONFIG_MODE_INFO mode {};
          mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
          mode.adapterId = path.sourceInfo.adapterId;
          mode.id = path.sourceInfo.id;
          mode.sourceMode.width = model_->modes.at(id).m_resolution.m_width;
          mode.sourceMode.height = model_->modes.at(id).m_resolution.m_height;
          result.m_modes.push_back(mode);
        }
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
      if (model_->retained_windows_path && (type == QueryType::All || (model_->retained_windows_path->flags & DISPLAYCONFIG_PATH_ACTIVE))) {
        result.m_paths.push_back(*model_->retained_windows_path);
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
      if (path.targetInfo.id == display_model_t::target("virtual1") && !model_->recovery_library_id.empty()) {
        return model_->recovery_library_id;
      }
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

    std::optional<HdrState> getHdrState(const DISPLAYCONFIG_PATH_INFO &path) const override {
      std::lock_guard lock {model_->mutex};
      return model_->capture_metadata ? std::optional {model_->hdr.at(display_model_t::id(path.targetInfo.id))} : std::nullopt;
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
      ++model_->settings_mutations;
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
      ++model_->settings_mutations;
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
      ++model_->settings_mutations;
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
      ++model_->settings_mutations;
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
      std::lock_guard lock {model_->mutex};
      ++model_->transport_closes;
      if (model_->close_failure) {
        return std::unexpected(failure_t {errc_t::close_failed, operation_t::close});
      }
      return {};
    }

    std::expected<std::size_t, failure_t> ioctl(DWORD code, std::span<const std::byte> input, std::span<std::byte> output) override {
      std::lock_guard lock {model_->mutex};
      if (code == wire::version) {
        if (model_->protocol_failure) {
          return std::unexpected(failure_t {errc_t::transport_failed, operation_t::version});
        }
        const wire::version_t version {0, 2, 1, 1};
        std::memcpy(output.data(), &version, sizeof(version));
      } else if (code == wire::watchdog) {
        const wire::watchdog_t watchdog {model_->watchdog_timeout, model_->watchdog_timeout};
        std::memcpy(output.data(), &watchdog, sizeof(watchdog));
      } else if (code == wire::add) {
        ++model_->additions;
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
        if (model_->heartbeat_failure) {
          return std::unexpected(failure_t {errc_t::heartbeat_failed, operation_t::heartbeat});
        }
      } else if (code == wire::set_adapter) {
        ++model_->adapter_selections;
      }
      return output.size();
    }

    std::expected<enumeration_t, failure_t> enumerate() override {
      std::lock_guard lock {model_->mutex};
      if (model_->enumeration_exception) {
        throw std::runtime_error {"inert adopted recovery enumeration failure"};
      }
      enumeration_t result;
      for (const auto &id : {std::string {"virtual1"}, std::string {"virtual2"}}) {
        if (id == "virtual1" && model_->recovery_driver_absent) {
          continue;
        }
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
      ++model_->heartbeat_creations;
      if (model_->heartbeat_creation_throws) {
        --model_->heartbeat_creation_throws;
        throw std::runtime_error {"inert heartbeat creation failure"};
      }
      model_->tick = std::move(callback);
      return std::make_unique<model_heartbeat_t>(model_);
    }

    void wait_for_enumeration(std::chrono::milliseconds) override {}

  private:
    std::shared_ptr<display_model_t> model_;
  };

  /** @brief Explicit fake verifier; supplies no production provisioning implementation. */
  class fixture_authority_t final: public platf::source_backend::authority_verifier_t {
  public:
    std::expected<recovery_authority_t, failure_t> verify(const std::filesystem::path &, const display_policy::source_request_t &, std::uint64_t) override {
      return recovery_authority_t {"transaction-fixture", true, true};
    }
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
    bool owner_boundaries = false;
    owned_display_configuration_t *concrete_transaction = nullptr;  ///< Fixture-only view while the concrete backend retains B.

    std::shared_ptr<rtsp_stream::source_backend_t> concrete_backend(bool fail_readback_allocation = false) {
      model->capture_metadata = true;
      platf::source_backend::test_dependencies_t dependencies;
      dependencies.make_api = [this, fail_readback_allocation]() -> std::shared_ptr<WinApiLayerInterface> {
        if (fail_readback_allocation) {
          throw std::runtime_error {"inert readback allocation failure"};
        }
        return api;
      };
      dependencies.capture_names = []() {
        return std::vector<std::string> {"dock-capture"};
      };
      dependencies.wake_native = [this](const auto &) {
        if (!owner_boundaries) {
          ADD_FAILURE() << "Virtual must not wake native displays";
        }
      };
      dependencies.render_adapter = [](const auto &) {
        return std::optional {display_adapter};
      };
      dependencies.make_system = [this]() {
        return std::make_unique<model_system_t>(model);
      };
      dependencies.begin_owned = [this](const auto &file, const auto &identity, auto generation, const auto &qualification) {
        auto result = owned_display_configuration_t::test_begin(file, identity, generation, qualification, api, devices);
        if (result) {
          (*result)->test_set_retry_interval(10ms);
          concrete_transaction = result->get();
        }
        return result;
      };
      if (owner_boundaries) {
        dependencies.probe = [](const auto &, auto pin) {
          EXPECT_TRUE(pin);
          return pin ? 200 : 503;
        };
      }
      return platf::source_backend::test_make_backend({path, std::make_shared<fixture_authority_t>()}, std::move(dependencies));
    }

    display_policy::source_request_t concrete_request(bool second = false) {
      display_policy::source_request_t request {};
      request.virtual_display = true;
      request.monitor_uuid = second ? "22222222-1234-5678-0102-030405060708" : "11111111-1234-5678-0102-030405060708";
      request.edid_name = "Miranda";
      request.edid_serial = "fixture";
      request.mode = {{1920, 1080}, {60, 1}, true};
      request.topology = config::video_t::dd_t::config_option_e::ensure_only_display;
      return request;
    }

    bool concrete_until(const std::shared_ptr<rtsp_stream::source_backend_t> &backend, std::uint64_t generation, const std::function<bool(const rtsp_stream::source_backend_status_t &)> &predicate) {
      for (unsigned int attempt = 0; attempt < 200; ++attempt) {
        backend->progress(generation);
        if (predicate(backend->status())) {
          return true;
        }
        std::this_thread::sleep_for(5ms);
      }
      return false;
    }

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

namespace {
  /** @brief Test-only model of held deployment authorization; never infers it from the journal. */
  class reconstruction_authority_t final: public platf::source_backend::authority_verifier_t {
  public:
    std::string deployment {"transaction-fixture"};
    unsigned int calls {};
    bool denied {};
    std::weak_ptr<void> lifetime;

    std::expected<recovery_authority_t, failure_t> verify(const std::filesystem::path &, const display_policy::source_request_t &, std::uint64_t) override {
      ADD_FAILURE() << "Recovery must not synthesize a fresh source request";
      return std::unexpected(failure_t {errc_t::ownership_unverified, operation_t::journal});
    }

    std::expected<std::shared_ptr<platf::source_backend::recovery_authorization_t>, failure_t> verify_recovery(const std::filesystem::path &) override {
      ++calls;
      if (denied) {
        return std::unexpected(failure_t {errc_t::ownership_unverified, operation_t::journal});
      }
      auto held = std::make_shared<int>(0);
      lifetime = held;
      return platf::source_backend::test_recovery_authorization({deployment, true, true}, std::move(held));
    }
  };

  class OwnedReconstructionTest: public OwnedDisplayTest {
  protected:
    std::shared_ptr<reconstruction_authority_t> verifier {std::make_shared<reconstruction_authority_t>()};
    std::unique_ptr<platf::source_startup::coordinator_t> startup;
    std::shared_ptr<platf::source_backend::recovery_context_t> context;
    unsigned int b_calls {}, driver_calls {};
    bool fail_driver_allocation {};
    std::function<void()> before_b;
    std::filesystem::path canonical_path;

    void TearDown() override {
      context.reset();
      startup.reset();
      OwnedDisplayTest::TearDown();
    }

    std::string journal_bytes() {
      std::ifstream input {path, std::ios::binary};
      return {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
    }

    nlohmann::json journal_data() {
      return nlohmann::json::parse(journal_bytes());
    }

    void write_bytes(const std::string &bytes) {
      std::ofstream output {path, std::ios::binary | std::ios::trunc};
      output << bytes;
      ASSERT_TRUE(output.good());
    }

    bool crash_episode(bool armed = true) {
      if (!begin()) {
        return false;
      }
      if (armed && (!acquire() || !prepare())) {
        return false;
      }
      if (!source->cancel(1)) {
        return false;
      }
      lease.reset();  // A managed abandoned lease closes without removing; durable intent remains.
      source.reset();
      return true;
    }

    void make_startup(std::function<platf::source_startup::coordinator_t::test_storage_t()> read = {}) {
      canonical_path = std::filesystem::absolute(path).lexically_normal();
      canonical_path.make_preferred();
      platf::source_backend::test_dependencies_t dependencies;
      dependencies.make_api = []() -> std::shared_ptr<WinApiLayerInterface> {
        ADD_FAILURE() << "Recovery must not initialize capture readback";
        return {};
      };
      dependencies.capture_names = []() {
        ADD_FAILURE() << "Recovery must not enumerate capture selectors";
        return std::vector<std::string> {};
      };
      dependencies.render_adapter = [](const auto &) -> std::optional<LUID> {
        ADD_FAILURE() << "Recovery must not select a rendering adapter";
        return {};
      };
      dependencies.begin_owned = [](const auto &, const auto &, auto, const auto &) -> std::expected<std::unique_ptr<owned_display_configuration_t>, failure_t> {
        ADD_FAILURE() << "Recovery must not create or rotate intent";
        return std::unexpected(failure_t {errc_t::journal_failed, operation_t::journal});
      };
      dependencies.make_system = [this]() -> std::unique_ptr<system_t> {
        ++driver_calls;
        EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
        EXPECT_FALSE(verifier->lifetime.expired());
        if (fail_driver_allocation) {
          throw std::runtime_error {"inert recovery allocation failure"};
        }
        return std::make_unique<model_system_t>(model);
      };
      dependencies.recover_owned = [this](const auto &file, const auto &qualification, const auto &expected) {
        ++b_calls;
        EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
        EXPECT_FALSE(verifier->lifetime.expired());
        if (before_b) {
          before_b();
        }
        auto result = owned_display_configuration_t::test_recover(file, qualification, api, devices, expected);
        if (result) {
          (*result)->test_set_retry_interval(10ms);
          concrete_transaction = result->get();
        }
        return result;
      };
      startup = platf::source_startup::coordinator_t::test_make({canonical_path, canonical_path.parent_path() / (canonical_path.filename().native() + L".ordinary.state")}, std::move(read), std::move(dependencies));
    }

    std::shared_ptr<platf::source_backend::recovery_context_t> reconstruct() {
      context = startup->reconstruct({canonical_path, verifier});
      return context;
    }

    void reject_heartbeat_creation(unsigned int throws) {
      ASSERT_TRUE(crash_episode());
      const auto original = journal_bytes();
      const auto mutations = model->settings_mutations;
      const auto closes = model->transport_closes;
      const auto creations = model->heartbeat_creations;
      model->heartbeat_creation_throws = throws;
      make_startup();
      ASSERT_TRUE(reconstruct());
      EXPECT_FALSE(context->reconstructed());
      // Before progress publishes copied diagnostics, status reads the actual retained lease latch.
      EXPECT_TRUE(context->status().heartbeat_failed);
      ASSERT_TRUE(context->diagnostics().acquisition);
      ASSERT_TRUE(context->diagnostics().liveness);
      EXPECT_EQ(context->diagnostics().liveness->operation, operation_t::heartbeat);
      EXPECT_EQ(model->heartbeat_creations, creations + 2);
      EXPECT_EQ(static_cast<bool>(model->tick), throws == 1);
      const auto retained = context;
      const auto backend = platf::source_backend::test_recovery_backend(*context);
      EXPECT_EQ(reconstruct(), retained);
      EXPECT_FALSE(context->reconstructed());
      EXPECT_FALSE(backend->restore(1, 0ms));
      EXPECT_FALSE(backend->cleanup_and_retire(1));
      EXPECT_TRUE(context->status().heartbeat_failed);
      EXPECT_EQ(model->heartbeat_creations, creations + 2);
      EXPECT_EQ(model->settings_mutations, mutations);
      EXPECT_EQ(model->transport_closes, closes);
      EXPECT_EQ(model->removals, 0u);
      EXPECT_EQ(model->additions, 1u);
      EXPECT_EQ(b_calls, 1u);
      EXPECT_EQ(driver_calls, 1u);
      EXPECT_EQ(journal_bytes(), original);
      EXPECT_FALSE(verifier->lifetime.expired());
      EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
    }

    void reject_changed_binding(unsigned int kind, bool initially_absent = false, bool inactive_conflict = false) {
      ASSERT_TRUE(crash_episode());
      {
        std::lock_guard lock {model->mutex};
        model->api_unavailable = true;
        if (initially_absent) {
          model->present.erase("virtual1");
          model->active = {{"dock"}};
          model->primary = "dock";
        }
      }
      make_startup();
      ASSERT_TRUE(reconstruct());
      ASSERT_TRUE(context->reconstructed());
      ASSERT_TRUE(concrete_transaction->cancel(1));
      const auto original = journal_bytes();
      const auto mapping = journal_data().at("mapping");
      const auto mutations = model->settings_mutations;
      {
        std::lock_guard lock {model->mutex};
        model->present.insert("virtual1");
        model->active = {{"virtual1"}};
        model->primary = "virtual1";
        auto changed = model->display("virtual1");
        if (kind == 0) {
          changed.monitor_path = L"changed-owned-monitor-path";
          model->recovery_display_override = changed;
        } else if (kind == 1) {
          ++changed.target.target;
          model->recovery_display_override = changed;
        } else if (kind == 2) {
          model->recovery_library_id = "changed-owned-library-id";
        } else if (kind == 3) {
          model->alternative = true;
          model->conflicting_library_id = true;
        } else {
          model->recovery_driver_absent = true;
          if (inactive_conflict) {
            model->active = {{"dock"}};
            model->primary = "dock";
          }
        }
        model->api_unavailable = false;
      }
      if (kind == 4) {
        const auto windows = api->queryDisplayConfig(QueryType::All);
        ASSERT_TRUE(windows);
        const auto matching = std::ranges::find_if(windows->m_paths, [](const auto &path) {
          return path.targetInfo.id == display_model_t::target("virtual1");
        });
        ASSERT_NE(matching, windows->m_paths.end());
        EXPECT_EQ(matching->targetInfo.targetAvailable, TRUE);
        EXPECT_EQ(static_cast<bool>(matching->flags & DISPLAYCONFIG_PATH_ACTIVE), !inactive_conflict);
        const auto driver = model_system_t {model}.enumerate();
        ASSERT_TRUE(driver);
        EXPECT_TRUE(driver->identities.empty());
      }
      const auto retained = context;
      const auto backend = platf::source_backend::test_recovery_backend(*context);
      EXPECT_EQ(reconstruct(), retained);
      EXPECT_FALSE(context->reconstructed());
      EXPECT_FALSE(backend->restore(1, 0ms));
      EXPECT_FALSE(backend->cleanup_and_retire(1));
      EXPECT_EQ(model->settings_mutations, mutations);
      EXPECT_EQ(model->removals, 0u);
      EXPECT_EQ(model->additions, 1u);
      EXPECT_EQ(b_calls, 1u);
      EXPECT_EQ(driver_calls, 1u);
      EXPECT_EQ(journal_bytes(), original);
      EXPECT_EQ(journal_data().at("mapping"), mapping);
      EXPECT_FALSE(verifier->lifetime.expired());
      EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
      ASSERT_TRUE(model->tick);
      model->tick();
      EXPECT_GT(model->pings, 0u);
      {
        std::lock_guard lock {model->mutex};
        model->recovery_display_override.reset();
        model->recovery_library_id.clear();
        model->alternative = false;
        model->conflicting_library_id = false;
        model->recovery_driver_absent = false;
        if (inactive_conflict) {
          model->active = {{"virtual1"}};
          model->primary = "virtual1";
        }
      }
      EXPECT_EQ(reconstruct(), retained);
      EXPECT_TRUE(context->reconstructed());
      EXPECT_EQ(journal_data().at("mapping"), mapping);
      EXPECT_EQ(context->generation(), 1u);
      EXPECT_EQ(b_calls, 1u);
      EXPECT_EQ(driver_calls, 1u);
      EXPECT_GT(model->settings_mutations, mutations);
    }

    void accept_unavailable_retained_path(bool active) {
      ASSERT_TRUE(crash_episode());
      const auto original = journal_data();
      {
        std::lock_guard lock {model->mutex};
        model->present.erase("virtual1");
        model->active = {{"dock"}};
        model->primary = "dock";
        DISPLAYCONFIG_PATH_INFO retained {};
        retained.sourceInfo.adapterId = display_adapter;
        retained.sourceInfo.id = display_model_t::target("virtual1");
        retained.targetInfo.adapterId = display_adapter;
        retained.targetInfo.id = display_model_t::target("virtual1");
        retained.targetInfo.targetAvailable = FALSE;
        retained.flags = active ? DISPLAYCONFIG_PATH_ACTIVE : 0;
        model->retained_windows_path = retained;
      }
      const auto driver = model_system_t {model}.enumerate();
      ASSERT_TRUE(driver);
      EXPECT_TRUE(driver->identities.empty());
      const auto windows = api->queryDisplayConfig(QueryType::All);
      ASSERT_TRUE(windows);
      const auto retained = std::ranges::find_if(windows->m_paths, [](const auto &path) {
        return path.targetInfo.id == display_model_t::target("virtual1");
      });
      ASSERT_NE(retained, windows->m_paths.end());
      EXPECT_FALSE(retained->targetInfo.targetAvailable);
      EXPECT_EQ(static_cast<bool>(retained->flags & DISPLAYCONFIG_PATH_ACTIVE), active);
      make_startup();
      ASSERT_TRUE(reconstruct());
      ASSERT_TRUE(context->reconstructed());
      ASSERT_TRUE(concrete_transaction);
      const auto observed = concrete_transaction->test_status_after_scheduled_work();
      EXPECT_EQ(observed.phase, owned_display_phase_e::removal_ready);
      EXPECT_TRUE(observed.removal_permitted);
      EXPECT_EQ(observed.revert_result, SettingsManagerInterface::RevertResult::Ok);
      const auto current = journal_data();
      EXPECT_EQ(current.at("mapping"), original.at("mapping"));
      EXPECT_EQ(current.at("identity"), original.at("identity"));
      EXPECT_EQ(current.at("generation"), original.at("generation"));
      EXPECT_EQ(current.at("baseline"), original.at("baseline"));
      EXPECT_EQ(model->additions, 1u);
      EXPECT_EQ(model->removals, 0u);
      EXPECT_EQ(b_calls, 1u);
      EXPECT_EQ(driver_calls, 1u);
      EXPECT_FALSE(verifier->lifetime.expired());
      EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
    }
  };
}  // namespace

namespace {
  /** @brief Actual journal/B recovery retained by the real owner; no source factory is registered. */
  class OwnedRecoveryAdoptionTest: public OwnedReconstructionTest {
  protected:
    std::unique_ptr<platf::deinit_t> recovery_owner;
    std::uint64_t recovery_generation {};
    std::shared_ptr<const proc::published_snapshot_t> saved_application;

    void SetUp() override {
      OwnedReconstructionTest::SetUp();
      saved_application = proc::proc.published_snapshot();
      proc::proc.publish_snapshot(std::make_shared<const proc::published_snapshot_t>());
      recovery_owner = rtsp_stream::test_support::start_owner();
    }

    void TearDown() override {
      rtsp_stream::test_support::set_owner_hook({});
      recovery_owner.reset();
      EXPECT_TRUE(rtsp_stream::test_support::discard_source_recovery());
      proc::proc.publish_snapshot(std::move(saved_application));
      OwnedReconstructionTest::TearDown();
    }

    void prepare_startup() {
      // Select a new test episode before reconstruction; adoption must preserve this exact value.
      recovery_generation = rtsp_stream::test_support::source_generation_floor() + 10;
      auto data = journal_data();
      data["generation"] = recovery_generation;
      if (!data["mapping"].is_null()) {
        data["mapping"]["generation"] = recovery_generation;
      }
      write_bytes(data.dump());
      make_startup();
      ASSERT_TRUE(reconstruct());
      ASSERT_TRUE(context->reconstructed());
    }

    bool owner_until(const std::function<bool(const rtsp_stream::source_snapshot_t &)> &predicate) {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      do {
        rtsp_stream::test_support::poll_source();
        if (predicate(rtsp_stream::source_snapshot())) {
          return true;
        }
        std::this_thread::yield();
      } while (std::chrono::steady_clock::now() < deadline);
      return false;
    }

    void reject_changed_after_transfer(bool journal_changed) {
      ASSERT_TRUE(crash_episode());
      model->api_unavailable = true;
      prepare_startup();
      ASSERT_TRUE(concrete_transaction->cancel(recovery_generation));
      const auto mutations = model->settings_mutations;
      rtsp_stream::test_support::set_owner_hook([](std::string_view event) {
        if (event == "recovery_after_commit") {
          throw std::runtime_error {"injected timer failure"};
        }
      });
      auto alias = context;
      ASSERT_TRUE(rtsp_stream::adopt_source_recovery(context));
      rtsp_stream::test_support::set_owner_hook({});
      if (journal_changed) {
        write_bytes("malformed changed journal");
      } else {
        std::lock_guard lock {model->mutex};
        auto changed = model->display("virtual1");
        changed.monitor_path = L"changed-after-owner-transfer";
        model->recovery_display_override = changed;
      }
      {
        std::lock_guard lock {model->mutex};
        model->api_unavailable = false;
      }
      rtsp_stream::retry_source_cleanup();
      ASSERT_TRUE(owner_until([](const auto &state) {
        return state.cleanup_blocked;
      }));
      EXPECT_TRUE(rtsp_stream::source_snapshot().busy);
      EXPECT_FALSE(rtsp_stream::source_snapshot().awaiting_completion);
      EXPECT_EQ(alias->generation(), recovery_generation);
      EXPECT_EQ(model->settings_mutations, mutations);
      EXPECT_EQ(model->removals, 0u);
      EXPECT_TRUE(model->tick);
      EXPECT_FALSE(verifier->lifetime.expired());
      EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
      EXPECT_EQ(b_calls, 1u);
      EXPECT_EQ(driver_calls, 1u);
    }

    void stop_after_persistent_timer_failure(bool concurrent_process) {
      ASSERT_TRUE(crash_episode());
      model->api_unavailable = true;
      prepare_startup();
      const auto exact_context = context.get();
      std::weak_ptr<platf::source_backend::recovery_context_t> retained = context;
      std::promise<void> observation_entered, registration_failed, failed_again, stop_entered;
      std::promise<void> release_observation, process_entered, release_process;
      const auto observation_gate = release_observation.get_future().share();
      const auto process_gate = release_process.get_future().share();
      std::atomic_bool fail_registration {true};
      std::atomic_uint registrations {};
      std::future<void> stopping, process;
      bool observation_released = false, process_released = false, drain_succeeded = false;
      // Failure cleanup can arrange a healthy registration after assertions stop the case.
      // It cannot satisfy the bounded drain assertion made with persistent failure enabled.
      auto cleanup = util::fail_guard([&]() {
        fail_registration = false;
        if (!observation_released) {
          release_observation.set_value();
        }
        if (!process_released) {
          release_process.set_value();
        }
        if (!drain_succeeded) {
          rtsp_stream::retry_source_cleanup();
        }
        if (stopping.valid()) {
          stopping.wait();
        }
        if (process.valid()) {
          process.wait();
        }
        rtsp_stream::test_support::set_owner_hook({});
      });
      rtsp_stream::test_support::set_owner_hook([&](std::string_view event) {
        if (event == "recovery_observation_before_progress") {
          observation_entered.set_value();
          observation_gate.wait();
        } else if (event == "adopted_work_timer_before_registration" && fail_registration) {
          const auto attempt = ++registrations;
          if (attempt == 1) {
            registration_failed.set_value();
          }
          if (attempt == 2) {
            failed_again.set_value();
          }
          throw std::runtime_error {"persistent timer wait registration failure"};
        } else if (event == "stop_begin") {
          stop_entered.set_value();
        }
      });
      ASSERT_TRUE(rtsp_stream::adopt_source_recovery(context));
      ASSERT_FALSE(context);
      ASSERT_EQ(observation_entered.get_future().wait_for(1s), std::future_status::ready);
      ASSERT_EQ(registration_failed.get_future().wait_for(1s), std::future_status::ready);
      startup.reset();
      EXPECT_EQ(retained.lock().get(), exact_context);
      EXPECT_TRUE(rtsp_stream::source_snapshot().cleanup_blocked);
      EXPECT_TRUE(rtsp_stream::test_support::pending_owner_work());
      if (concurrent_process) {
        process = rtsp_stream::process_operation([&]() {
          process_entered.set_value();
          process_gate.wait();
        });
        ASSERT_EQ(process_entered.get_future().wait_for(1s), std::future_status::ready);
        ASSERT_EQ(failed_again.get_future().wait_for(1s), std::future_status::ready);
      }
      stopping = std::async(std::launch::async, []() {
        rtsp_stream::test_support::stop_owner();
      });
      ASSERT_EQ(stop_entered.get_future().wait_for(1s), std::future_status::ready);
      EXPECT_EQ(stopping.wait_for(0ms), std::future_status::timeout);
      release_observation.set_value();
      observation_released = true;
      if (concurrent_process) {
        EXPECT_EQ(stopping.wait_for(30ms), std::future_status::timeout);
        release_process.set_value();
        process_released = true;
      }
      ASSERT_EQ(stopping.wait_for(1s), std::future_status::ready);
      stopping.get();
      drain_succeeded = true;
      if (process.valid()) {
        process.get();
      }
      EXPECT_FALSE(rtsp_stream::test_support::pending_owner_work());
      EXPECT_EQ(retained.lock().get(), exact_context);
      ASSERT_FALSE(retained.expired());
      EXPECT_EQ(retained.lock()->generation(), recovery_generation);
      EXPECT_FALSE(retained.lock()->status().source_released);
      EXPECT_EQ(rtsp_stream::test_support::source_generation_floor(), recovery_generation);
      EXPECT_TRUE(rtsp_stream::source_snapshot().busy);
      EXPECT_TRUE(rtsp_stream::source_snapshot().cleanup_blocked);
      EXPECT_TRUE(rtsp_stream::source_snapshot().recovery_pending);
      EXPECT_FALSE(rtsp_stream::source_snapshot().awaiting_completion);
      EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
      EXPECT_FALSE(reset_persistence());
      EXPECT_FALSE(verifier->lifetime.expired());
      EXPECT_TRUE(model->tick);
      EXPECT_FALSE(journal_data().at("source_retired").get<bool>());
      EXPECT_EQ(journal_data().at("generation"), recovery_generation);
      EXPECT_EQ(model->removals, 0u);
      EXPECT_EQ(model->additions, 1u);
      EXPECT_EQ(b_calls, 1u);
      EXPECT_EQ(driver_calls, 1u);
      EXPECT_GE(registrations.load(), concurrent_process ? 2u : 1u);
    }

    void expect_retained_completion(unsigned int expected_removals) {
      ASSERT_TRUE(owner_until([](const auto &status) {
        return status.awaiting_completion;
      }));
      EXPECT_TRUE(rtsp_stream::source_snapshot().busy);
      EXPECT_TRUE(rtsp_stream::source_snapshot().recovery_pending);
      EXPECT_FALSE(rtsp_stream::source_snapshot().managed);
      EXPECT_FALSE(rtsp_stream::source_snapshot().source);
      EXPECT_EQ(rtsp_stream::test_support::source_generation_floor(), recovery_generation);
      EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
      EXPECT_FALSE(reset_persistence());
      EXPECT_FALSE(verifier->lifetime.expired());
      EXPECT_FALSE(journal_bytes().empty());
      const auto data = journal_data();
      EXPECT_EQ(data.at("generation"), recovery_generation);
      EXPECT_TRUE(data.at("source_retired").get<bool>());
      EXPECT_EQ(model->removals, expected_removals);
    }
  };
}  // namespace

TEST_F(OwnedRecoveryAdoptionTest, UnarmedActualBStopsBeforeCompletionAndRetainsProofAfterCoordinatorDestruction) {
  ASSERT_TRUE(crash_episode(false));
  prepare_startup();
  const auto backend = platf::source_backend::test_recovery_backend(*context);
  std::weak_ptr<platf::source_backend::recovery_context_t> weak = context;
  ASSERT_TRUE(rtsp_stream::adopt_source_recovery(context));
  EXPECT_FALSE(context);
  startup.reset();
  EXPECT_FALSE(weak.expired());
  expect_retained_completion(0);
  EXPECT_EQ(model->additions, 0u);
  EXPECT_EQ(driver_calls, 0u);
  EXPECT_TRUE(backend->status().restoration_ready);
  EXPECT_TRUE(backend->status().recovery_pending);
  EXPECT_FALSE(backend->complete(recovery_generation));
}

TEST_F(OwnedRecoveryAdoptionTest, FailedDispatchAndStoppedLoopRetainSoleActualContextAuthorityLeaseAndGate) {
  ASSERT_TRUE(crash_episode());
  prepare_startup();
  std::weak_ptr<platf::source_backend::recovery_context_t> retained = context;
  rtsp_stream::test_support::set_owner_hook([](std::string_view event) {
    if (event == "recovery_after_commit") {
      throw std::runtime_error {"injected timer dispatch failure"};
    }
  });
  ASSERT_TRUE(rtsp_stream::adopt_source_recovery(context));
  ASSERT_FALSE(context);
  startup.reset();
  EXPECT_FALSE(retained.expired());
  EXPECT_FALSE(verifier->lifetime.expired());
  EXPECT_TRUE(model->tick);
  EXPECT_TRUE(rtsp_stream::source_snapshot().busy);
  EXPECT_TRUE(rtsp_stream::source_snapshot().cleanup_blocked);
  EXPECT_FALSE(journal_data().at("source_retired").get<bool>());
  rtsp_stream::test_support::set_owner_hook({});
  recovery_owner.reset();
  EXPECT_FALSE(retained.expired());
  EXPECT_FALSE(verifier->lifetime.expired());
  EXPECT_TRUE(model->tick);
  EXPECT_TRUE(rtsp_stream::source_snapshot().busy);
  EXPECT_TRUE(rtsp_stream::source_snapshot().recovery_pending);
  EXPECT_TRUE(rtsp_stream::source_snapshot().cleanup_blocked);
  EXPECT_FALSE(rtsp_stream::set_source_backend({}));
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
  EXPECT_FALSE(reset_persistence());
  EXPECT_EQ(model->removals, 0u);
  EXPECT_EQ(model->additions, 1u);
}

TEST_F(OwnedRecoveryAdoptionTest, PersistentTimerRegistrationFailureDrainsTheSoleActualRecoveryFutureOnStop) {
  stop_after_persistent_timer_failure(false);
}

TEST_F(OwnedRecoveryAdoptionTest, PersistentTimerRegistrationFailureDrainsRecoveryAndConcurrentProcessWorkOnStop) {
  stop_after_persistent_timer_failure(true);
}

TEST_F(OwnedRecoveryAdoptionTest, ArmedPresentUsesActualRemovalReceiptAndNeverReopensBOrCreatesIntent) {
  ASSERT_TRUE(crash_episode());
  prepare_startup();
  const auto identity = journal_data().at("identity");
  const auto adapter_selections = model->adapter_selections;
  ASSERT_TRUE(rtsp_stream::adopt_source_recovery(context));
  expect_retained_completion(1);
  EXPECT_EQ(journal_data().at("identity"), identity);
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 1u);
  EXPECT_EQ(model->additions, 1u);
  EXPECT_EQ(model->adapter_selections, adapter_selections);
}

TEST_F(OwnedRecoveryAdoptionTest, ArmedAbsentAdoptionRetiresTheSameEpisodeWithoutRemovingAnotherOutput) {
  ASSERT_TRUE(crash_episode());
  {
    std::lock_guard lock {model->mutex};
    model->present.erase("virtual1");
    model->active = {{"dock"}};
    model->primary = "dock";
  }
  prepare_startup();
  ASSERT_TRUE(rtsp_stream::adopt_source_recovery(context));
  // An absent adopted GUID still requires the exact adapter-issued removal receipt.
  expect_retained_completion(1);
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 1u);
  EXPECT_EQ(model->additions, 1u);
}

TEST_F(OwnedRecoveryAdoptionTest, RetiredActualBContinuesWithoutDriverReacquisitionOrRepeatedRemoval) {
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  ASSERT_TRUE(prepare());
  ASSERT_TRUE(release(1));
  source.reset();
  prepare_startup();
  ASSERT_TRUE(rtsp_stream::adopt_source_recovery(context));
  expect_retained_completion(1);
  EXPECT_EQ(driver_calls, 0u);
  EXPECT_EQ(model->additions, 1u);
}

TEST_F(OwnedRecoveryAdoptionTest, OfferedAndAdoptedAliasesCannotInspectFenceOrReconstructB) {
  ASSERT_TRUE(crash_episode());
  prepare_startup();
  auto alias = context;
  const auto before = startup->status();
  std::promise<void> offered, release_offer;
  const auto gate = release_offer.get_future().share();
  rtsp_stream::test_support::set_owner_hook([&](std::string_view event) {
    if (event == "recovery_offer_reserved") {
      offered.set_value();
      gate.wait();
    }
  });
  auto adoption = std::async(std::launch::async, [&]() {
    return rtsp_stream::adopt_source_recovery(context);
  });
  auto unblock = util::fail_guard([&]() {
    release_offer.set_value();
  });
  ASSERT_EQ(offered.get_future().wait_for(1s), std::future_status::ready);
  EXPECT_EQ(startup->inspect().disposition, before.disposition);
  EXPECT_EQ(startup->reconstruct({canonical_path, verifier}), alias);
  EXPECT_FALSE(startup->release_for_ordinary());
  EXPECT_FALSE(rtsp_stream::adopt_source_recovery(alias));
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 1u);
  release_offer.set_value();
  unblock.disable();
  ASSERT_TRUE(adoption.get());
  rtsp_stream::test_support::set_owner_hook({});
  EXPECT_EQ(startup->inspect().disposition, before.disposition);
  EXPECT_EQ(startup->reconstruct({canonical_path, verifier}), alias);
  EXPECT_FALSE(rtsp_stream::adopt_source_recovery(alias));
  alias.reset();
  expect_retained_completion(1);
}

TEST_F(OwnedRecoveryAdoptionTest, CoordinatorOperationInFlightRejectsOfferWithoutWaitingOnOwnerOrChangingFloor) {
  ASSERT_TRUE(crash_episode());
  recovery_generation = rtsp_stream::test_support::source_generation_floor() + 10;
  auto data = journal_data();
  data["generation"] = recovery_generation;
  data["mapping"]["generation"] = recovery_generation;
  write_bytes(data.dump());
  std::promise<void> entered, release_inspection;
  auto gate = release_inspection.get_future().share();
  std::atomic_bool block_next_read {false};
  make_startup([&]() {
    if (block_next_read.exchange(false)) {
      entered.set_value();
      gate.wait();
    }
    platf::source_startup::coordinator_t::test_storage_t observed;
    observed.owned = {platf::source_startup::store_state_e::completed_empty, journal_bytes()};
    return observed;
  });
  ASSERT_TRUE(reconstruct());
  ASSERT_TRUE(context->reconstructed());
  auto alias = context;
  const auto floor = rtsp_stream::test_support::source_generation_floor();
  block_next_read = true;
  auto reconstruction = std::async(std::launch::async, [&]() {
    return startup->reconstruct({canonical_path, verifier});
  });
  auto unblock = util::fail_guard([&]() {
    release_inspection.set_value();
  });
  ASSERT_EQ(entered.get_future().wait_for(1s), std::future_status::ready);
  EXPECT_FALSE(rtsp_stream::adopt_source_recovery(alias));
  EXPECT_EQ(alias, context);
  EXPECT_EQ(rtsp_stream::test_support::source_generation_floor(), floor);
  release_inspection.set_value();
  unblock.disable();
  EXPECT_EQ(reconstruction.get(), context);
}

TEST_F(OwnedRecoveryAdoptionTest, RemovalFailureAndCloseOnlyRetryRetainTheActualLeaseAndIssuedRetirementOrder) {
  ASSERT_TRUE(crash_episode());
  prepare_startup();
  {
    std::lock_guard lock {model->mutex};
    model->remove_failure = true;
  }
  auto alias = context;
  ASSERT_TRUE(rtsp_stream::adopt_source_recovery(context));
  ASSERT_TRUE(owner_until([](const auto &state) {
    return state.cleanup_blocked;
  }));
  EXPECT_EQ(model->removals, 0u);
  EXPECT_FALSE(journal_data().at("source_retired").get<bool>());
  EXPECT_TRUE(model->tick);
  startup.reset();
  EXPECT_FALSE(verifier->lifetime.expired());
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
  {
    std::lock_guard lock {model->mutex};
    model->remove_failure = false;
    model->close_failure = true;
  }
  rtsp_stream::retry_source_cleanup();
  ASSERT_TRUE(owner_until([&](const auto &state) {
    return state.cleanup_blocked && model->removals == 1u;
  }));
  EXPECT_FALSE(journal_data().at("source_retired").get<bool>());
  // Removal completed; the retained close-only transport needs no display heartbeat.
  EXPECT_FALSE(model->tick);
  EXPECT_FALSE(verifier->lifetime.expired());
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
  {
    std::lock_guard lock {model->mutex};
    model->close_failure = false;
  }
  rtsp_stream::retry_source_cleanup();
  expect_retained_completion(1);
  EXPECT_FALSE(alias->status().source);
  EXPECT_TRUE(alias->status().source_released);
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 1u);
}

TEST_F(OwnedRecoveryAdoptionTest, ChangedActualMappingAfterTransferCannotAuthorizeSettingsOrRemoval) {
  reject_changed_after_transfer(false);
}

TEST_F(OwnedRecoveryAdoptionTest, MalformedActualJournalAfterTransferRetainsTheExactBAndLeaseWithoutReopening) {
  reject_changed_after_transfer(true);
}

TEST_F(OwnedRecoveryAdoptionTest, UnavailableSettingsAndStickyHeartbeatRetainActualResourcesAndGate) {
  ASSERT_TRUE(crash_episode());
  model->api_unavailable = true;
  prepare_startup();
  auto alias = context;
  ASSERT_TRUE(rtsp_stream::adopt_source_recovery(context));
  EXPECT_FALSE(rtsp_stream::source_snapshot().awaiting_completion);
  EXPECT_TRUE(rtsp_stream::source_snapshot().busy);
  EXPECT_EQ(model->removals, 0u);
  std::function<void()> tick;
  {
    std::lock_guard lock {model->mutex};
    model->heartbeat_failure = true;
    tick = model->tick;
  }
  ASSERT_TRUE(tick);
  tick();
  ASSERT_TRUE(owner_until([](const auto &state) {
    return state.cleanup_blocked;
  }));
  startup.reset();
  {
    std::lock_guard lock {model->mutex};
    model->heartbeat_failure = false;
    model->api_unavailable = false;
  }
  rtsp_stream::retry_source_cleanup();
  rtsp_stream::test_support::poll_source();
  EXPECT_TRUE(alias->status().heartbeat_failed);
  EXPECT_FALSE(rtsp_stream::source_snapshot().awaiting_completion);
  EXPECT_TRUE(rtsp_stream::source_snapshot().busy);
  EXPECT_FALSE(verifier->lifetime.expired());
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
  EXPECT_FALSE(journal_data().at("source_retired").get<bool>());
  EXPECT_EQ(model->removals, 0u);
}

TEST_F(OwnedReconstructionTest, UnarmedReconstructsRealEpisodeWithoutDriverOrNewIntentAndKeepsGate) {
  ASSERT_TRUE(crash_episode(false));
  const auto original = journal_data();
  make_startup();
  ASSERT_TRUE(reconstruct());
  ASSERT_TRUE(context->reconstructed());
  EXPECT_EQ(context->generation(), 1u);
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 0u);
  EXPECT_EQ(model->additions, 0u);
  EXPECT_EQ(model->removals, 0u);
  const auto current = journal_data();
  EXPECT_EQ(current.at("identity"), original.at("identity"));
  EXPECT_EQ(current.at("generation"), original.at("generation"));
  EXPECT_EQ(current.at("baseline"), original.at("baseline"));
  EXPECT_TRUE(current.at("source_retired").get<bool>());
  EXPECT_FALSE(context->status().source);
  EXPECT_TRUE(context->status().source_released);
  EXPECT_TRUE(context->status().recovery_pending);
  EXPECT_FALSE(startup->status().prior_uncertainty);
  EXPECT_FALSE(startup->release_for_ordinary());
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
  EXPECT_FALSE(verifier->lifetime.expired());
}

TEST_F(OwnedReconstructionTest, DeniedAuthorityRetainsOneContextBeforeBAndRetriesOnlyTheSameVerifier) {
  ASSERT_TRUE(crash_episode(false));
  const auto original = journal_bytes();
  verifier->denied = true;
  make_startup();
  ASSERT_TRUE(reconstruct());
  EXPECT_FALSE(context->reconstructed());
  EXPECT_EQ(b_calls, 0u);
  EXPECT_EQ(driver_calls, 0u);
  EXPECT_EQ(journal_bytes(), original);
  const auto retained = context;
  EXPECT_FALSE(startup->reconstruct({canonical_path, std::make_shared<reconstruction_authority_t>()}));
  verifier->denied = false;
  EXPECT_EQ(reconstruct(), retained);
  EXPECT_TRUE(context->reconstructed());
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 0u);
}

TEST_F(OwnedReconstructionTest, ArmedPresentRetainsExactSettingsAndRequiresRealCloseReceiptBeforeRetirement) {
  ASSERT_TRUE(crash_episode());
  const auto original = journal_data();
  model->api_unavailable = true;
  make_startup();
  ASSERT_TRUE(reconstruct());
  ASSERT_TRUE(context->reconstructed());
  const auto backend = platf::source_backend::test_recovery_backend(*context);
  EXPECT_EQ(context->generation(), 1u);
  EXPECT_EQ(driver_calls, 1u);
  EXPECT_EQ(model->additions, 1u);
  EXPECT_EQ(model->adapter_selections, 1u);
  EXPECT_EQ(model->removals, 0u);
  EXPECT_EQ(journal_data().at("settings"), original.at("settings"));
  EXPECT_EQ(journal_data().at("baseline"), original.at("baseline"));
  EXPECT_EQ(journal_data().at("mapping"), original.at("mapping"));
  EXPECT_FALSE(backend->status().source);
  EXPECT_FALSE(backend->admits(concrete_request()));
  EXPECT_FALSE(backend->begin(concrete_request(), 2, 1));
  EXPECT_EQ(backend->probe({}, {}), 503);
  EXPECT_FALSE(backend->validate_retained({}, 1));
  EXPECT_FALSE(backend->cleanup_and_retire(2));
  EXPECT_FALSE(backend->cleanup_and_retire(1));
  {
    std::lock_guard lock {model->mutex};
    model->api_unavailable = false;
  }
  ASSERT_TRUE(backend->restore(1, 0ms));
  model->close_failure = true;
  EXPECT_FALSE(backend->cleanup_and_retire(1));
  EXPECT_EQ(model->removals, 1u);
  EXPECT_FALSE(journal_data().at("source_retired").get<bool>());
  ASSERT_TRUE(context->diagnostics().cleanup);
  model->close_failure = false;
  EXPECT_TRUE(backend->cleanup_and_retire(1));
  EXPECT_EQ(model->removals, 1u);
  EXPECT_TRUE(journal_data().at("source_retired").get<bool>());
  EXPECT_FALSE(backend->complete(1));
  EXPECT_FALSE(startup->release_for_ordinary());
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
}

TEST_F(OwnedReconstructionTest, ArmedAbsentUsesExactRecoveryLeaseWithoutAddOrSourceReadiness) {
  ASSERT_TRUE(crash_episode());
  model->present.erase("virtual1");
  model->active = {{"dock"}};
  model->primary = "dock";
  make_startup();
  ASSERT_TRUE(reconstruct());
  ASSERT_TRUE(context->reconstructed());
  const auto backend = platf::source_backend::test_recovery_backend(*context);
  EXPECT_EQ(driver_calls, 1u);
  EXPECT_EQ(model->additions, 1u);
  EXPECT_FALSE(backend->status().source);
  ASSERT_TRUE(backend->cleanup_and_retire(1));
  EXPECT_TRUE(journal_data().at("source_retired").get<bool>());
  EXPECT_EQ(journal_data().at("generation"), 1);
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
}

TEST_F(OwnedReconstructionTest, ReinspectionFencesRetainedBRetryBeforeOpeningCheckedStorageHandles) {
  ASSERT_TRUE(crash_episode());
  model->api_unavailable = true;
  make_startup();
  ASSERT_TRUE(reconstruct());
  ASSERT_TRUE(context->reconstructed());
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  const auto release_future = release.get_future().share();
  std::atomic_bool blocked {false};
  {
    std::lock_guard lock {model->mutex};
    model->api_check_hook = [&]() {
      if (!blocked.exchange(true)) {
        entered.set_value();
        release_future.wait();
      }
    };
  }
  const bool observed = entered_future.wait_for(2s) == std::future_status::ready;
  if (!observed) {
    release.set_value();
    std::ignore = concrete_transaction->cancel(1);
    std::lock_guard lock {model->mutex};
    model->api_check_hook = {};
    FAIL() << "Expected the retained B retry callback";
  }
  auto retry = std::async(std::launch::async, [&]() {
    return startup->reconstruct({canonical_path, verifier});
  });
  EXPECT_EQ(retry.wait_for(20ms), std::future_status::timeout);
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
  release.set_value();
  EXPECT_EQ(retry.get(), context);
  EXPECT_FALSE(startup->status().prior_uncertainty);
  EXPECT_TRUE(context->reconstructed());
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 1u);
  EXPECT_EQ(model->removals, 0u);
  ASSERT_TRUE(concrete_transaction->cancel(1));
  {
    std::lock_guard lock {model->mutex};
    model->api_check_hook = {};
  }
}

TEST_F(OwnedReconstructionTest, RetiredEpisodeResumesBWithoutReacquiringOrRemoving) {
  ASSERT_TRUE(begin());
  ASSERT_TRUE(acquire());
  ASSERT_TRUE(prepare());
  ASSERT_TRUE(release(1));
  source.reset();
  const auto original = journal_data();
  make_startup();
  ASSERT_TRUE(reconstruct());
  ASSERT_TRUE(context->reconstructed());
  EXPECT_EQ(driver_calls, 0u);
  EXPECT_EQ(model->removals, 1u);
  EXPECT_EQ(model->additions, 1u);
  EXPECT_EQ(journal_data().at("identity"), original.at("identity"));
  EXPECT_EQ(journal_data().at("generation"), original.at("generation"));
  EXPECT_EQ(journal_data().at("baseline"), original.at("baseline"));
  EXPECT_TRUE(context->status().source_released);
  EXPECT_FALSE(context->status().source);
  EXPECT_FALSE(startup->release_for_ordinary());
}

TEST_F(OwnedReconstructionTest, BindingFailureRetainsAdoptedLeaseAuthorityAndSameGenerationForRetry) {
  ASSERT_TRUE(crash_episode());
  const auto original = journal_bytes();
  model->query_failure = true;
  make_startup();
  ASSERT_TRUE(reconstruct());
  EXPECT_FALSE(context->reconstructed());
  const auto retained = context;
  EXPECT_EQ(driver_calls, 1u);
  EXPECT_EQ(model->removals, 0u);
  ASSERT_TRUE(model->tick);
  model->tick();
  EXPECT_GT(model->pings, 0u);
  EXPECT_FALSE(startup->reconstruct({canonical_path, std::make_shared<reconstruction_authority_t>()}));
  EXPECT_FALSE(verifier->lifetime.expired());
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
  auto changed = nlohmann::json::parse(original);
  changed["generation"] = std::uint64_t {2};
  changed["mapping"]["generation"] = std::uint64_t {2};
  write_bytes(changed.dump());
  EXPECT_EQ(reconstruct(), retained);
  EXPECT_FALSE(context->reconstructed());
  EXPECT_EQ(context->generation(), 1u);
  EXPECT_EQ(driver_calls, 1u);
  EXPECT_TRUE(model->tick);
  write_bytes(original);
  model->query_failure = false;
  ASSERT_EQ(reconstruct(), retained);
  EXPECT_TRUE(context->reconstructed());
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 1u);
  EXPECT_EQ(verifier->calls, 1u);
  EXPECT_EQ(model->additions, 1u);
  EXPECT_EQ(context->generation(), 1u);
}

TEST_F(OwnedReconstructionTest, LaterOrdinaryConflictRetainsPartialLeaseAndCannotBeClearedByDeletion) {
  ASSERT_TRUE(crash_episode());
  model->query_failure = true;
  make_startup();
  ASSERT_TRUE(reconstruct());
  ASSERT_FALSE(context->reconstructed());
  const auto ordinary_path = canonical_path.parent_path() / (canonical_path.filename().native() + L".ordinary.state");
  {
    std::ofstream output {ordinary_path, std::ios::binary};
    output << journal_data().at("baseline").get<std::string>();
  }
  EXPECT_FALSE(startup->reconstruct({canonical_path, verifier}));
  EXPECT_TRUE(startup->status().prior_uncertainty);
  std::error_code error;
  std::filesystem::remove(ordinary_path, error);
  EXPECT_FALSE(error);
  EXPECT_FALSE(startup->reconstruct({canonical_path, verifier}));
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 1u);
  EXPECT_TRUE(model->tick);
  EXPECT_FALSE(verifier->lifetime.expired());
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
}

TEST_F(OwnedReconstructionTest, FailureAfterBRetainsReservationAndRetriesWithoutReopeningB) {
  ASSERT_TRUE(crash_episode());
  fail_driver_allocation = true;
  make_startup();
  ASSERT_TRUE(reconstruct());
  EXPECT_FALSE(context->reconstructed());
  EXPECT_EQ(b_calls, 1u);
  EXPECT_FALSE(owned_display_configuration_t::test_recover(path, authority, api, devices));
  EXPECT_FALSE(verifier->lifetime.expired());
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
  const auto retained = context;
  fail_driver_allocation = false;
  EXPECT_EQ(reconstruct(), retained);
  EXPECT_TRUE(context->reconstructed());
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 2u);
  EXPECT_EQ(model->additions, 1u);
}

TEST_F(OwnedReconstructionTest, ExceptionAfterDriverAdoptionReturnsAndRetainsTheActualLease) {
  ASSERT_TRUE(crash_episode());
  model->enumeration_exception = true;
  make_startup();
  ASSERT_TRUE(reconstruct());
  EXPECT_FALSE(context->reconstructed());
  ASSERT_TRUE(context->diagnostics().acquisition);
  EXPECT_EQ(context->diagnostics().acquisition->code, errc_t::transport_failed);
  EXPECT_EQ(driver_calls, 1u);
  EXPECT_EQ(model->removals, 0u);
  EXPECT_TRUE(model->tick);
  const auto retained = context;
  model->enumeration_exception = false;
  EXPECT_EQ(reconstruct(), retained);
  EXPECT_TRUE(context->reconstructed());
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 1u);
  EXPECT_EQ(model->additions, 1u);
  startup.reset();
  EXPECT_FALSE(verifier->lifetime.expired());
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
}

TEST_F(OwnedReconstructionTest, FailedPreAdoptionCloseRetriesTransportWithoutRemovingOrReopeningB) {
  ASSERT_TRUE(crash_episode());
  model->protocol_failure = true;
  model->close_failure = true;
  make_startup();
  ASSERT_TRUE(reconstruct());
  EXPECT_FALSE(context->reconstructed());
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 1u);
  EXPECT_EQ(model->removals, 0u);
  ASSERT_TRUE(context->diagnostics().cleanup);
  EXPECT_EQ(context->diagnostics().cleanup->operation, operation_t::close);
  EXPECT_FALSE(verifier->lifetime.expired());
  const auto retained = context;
  model->protocol_failure = false;
  model->close_failure = false;
  EXPECT_EQ(reconstruct(), retained);
  EXPECT_TRUE(context->reconstructed());
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 2u);
  EXPECT_EQ(model->additions, 1u);
  EXPECT_EQ(model->removals, 0u);
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
}

TEST_F(OwnedReconstructionTest, ThrowingHeartbeatCreationRemainsStickyEvenIfFailurePathEstablishesAWorker) {
  reject_heartbeat_creation(1);
}

TEST_F(OwnedReconstructionTest, RepeatedThrowingHeartbeatCreationCannotAdvanceTheSameRetainedContext) {
  reject_heartbeat_creation(2);
}

TEST_F(OwnedReconstructionTest, DisabledWatchdogNeedsNoHeartbeatWorkerAndStillReconstructs) {
  model->watchdog_timeout = 0;
  ASSERT_TRUE(crash_episode());
  model->heartbeat_creation_throws = 2;
  model->api_unavailable = true;
  make_startup();
  ASSERT_TRUE(reconstruct());
  EXPECT_TRUE(context->reconstructed());
  EXPECT_FALSE(context->status().heartbeat_failed);
  EXPECT_FALSE(context->diagnostics().liveness);
  EXPECT_EQ(model->heartbeat_creations, 0u);
  EXPECT_FALSE(model->tick);
  EXPECT_EQ(model->removals, 0u);
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
}

TEST_F(OwnedReconstructionTest, ChangedMonitorPathAfterSuccessfulBindRefusesBeforeAnyFurtherSettingsMutation) {
  reject_changed_binding(0);
}

TEST_F(OwnedReconstructionTest, ChangedTargetAfterSuccessfulBindRefusesBeforeAnyFurtherSettingsMutation) {
  reject_changed_binding(1);
}

TEST_F(OwnedReconstructionTest, ChangedLibraryIdentityAfterSuccessfulBindRefusesBeforeAnyFurtherSettingsMutation) {
  reject_changed_binding(2);
}

TEST_F(OwnedReconstructionTest, AmbiguousWindowsIdentityAfterSuccessfulBindRefusesBeforeAnyFurtherSettingsMutation) {
  reject_changed_binding(3);
}

TEST_F(OwnedReconstructionTest, DriverAbsenceCannotOverrideStillPresentWindowsMappingOnRetry) {
  reject_changed_binding(4);
}

TEST_F(OwnedReconstructionTest, AvailableInactiveMatchingPathRefusesBeforeAnyFurtherSettingsMutation) {
  reject_changed_binding(4, false, true);
}

TEST_F(OwnedReconstructionTest, UnavailableInactiveMatchingPathDoesNotConflictWithActualDriverAbsence) {
  accept_unavailable_retained_path(false);
}

TEST_F(OwnedReconstructionTest, UnavailableTransientActiveMatchingPathDoesNotConflictWithActualDriverAbsence) {
  accept_unavailable_retained_path(true);
}

TEST_F(OwnedReconstructionTest, InitiallyAbsentRecoveryRejectsConflictingPresentMappingBeforeRestoration) {
  reject_changed_binding(0, true);
}

TEST_F(OwnedReconstructionTest, ExactPresentAbsentPresentTransitionsKeepTheOriginalMappingAndResumeRestoration) {
  ASSERT_TRUE(crash_episode());
  model->api_unavailable = true;
  make_startup();
  ASSERT_TRUE(reconstruct());
  ASSERT_TRUE(context->reconstructed());
  const auto retained = context;
  const auto mapping = journal_data().at("mapping");
  ASSERT_TRUE(concrete_transaction->cancel(1));
  {
    std::lock_guard lock {model->mutex};
    model->present.erase("virtual1");
    model->active = {{"dock"}};
    model->primary = "dock";
  }
  EXPECT_EQ(reconstruct(), retained);
  EXPECT_TRUE(context->reconstructed());
  EXPECT_EQ(journal_data().at("mapping"), mapping);
  ASSERT_TRUE(concrete_transaction->cancel(1));
  const auto mutations = model->settings_mutations;
  {
    std::lock_guard lock {model->mutex};
    model->present.insert("virtual1");
    model->active = {{"virtual1"}};
    model->primary = "virtual1";
    model->api_unavailable = false;
  }
  EXPECT_EQ(reconstruct(), retained);
  EXPECT_TRUE(context->reconstructed());
  EXPECT_EQ(journal_data().at("mapping"), mapping);
  EXPECT_GT(model->settings_mutations, mutations);
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 1u);
  EXPECT_EQ(model->additions, 1u);
  EXPECT_EQ(model->removals, 0u);
  EXPECT_FALSE(verifier->lifetime.expired());
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
}

TEST_F(OwnedReconstructionTest, InterruptedBeforeFirstBindRecoveryPersistsOnlyItsFirstExactMapping) {
  ASSERT_TRUE(begin());
  const auto baseline = source->journal()->record().baseline;
  const request_t request {first_identity, display_adapter, 1920, 1080, 60, 1, "Miranda", "fixture", true, 1, 0ms};
  auto acquired = lease_t::acquire_owned(std::make_unique<model_system_t>(model), request, source->journal(), authority);
  ASSERT_FALSE(acquired.failure);
  ASSERT_TRUE(acquired.lease);
  ASSERT_FALSE(source->journal()->mapping());
  ASSERT_TRUE(source->cancel(1));
  acquired.lease.reset();
  source.reset();
  make_startup();
  ASSERT_TRUE(reconstruct());
  EXPECT_TRUE(context->reconstructed());
  const auto data = journal_data();
  ASSERT_TRUE(data.at("mapping").is_object());
  EXPECT_EQ(data.at("mapping").at("generation"), 1);
  EXPECT_EQ(data.at("mapping").at("identity"), data.at("identity"));
  EXPECT_EQ(data.at("mapping").at("device_id"), "virtual1");
  EXPECT_EQ(data.at("baseline"), baseline);
  EXPECT_EQ(model->additions, 1u);
  EXPECT_EQ(model->removals, 0u);
  EXPECT_EQ(b_calls, 1u);
  EXPECT_EQ(driver_calls, 1u);
}

TEST_F(OwnedReconstructionTest, ChangedBytesInsideCheckedLoadRefuseThenRetryExactOriginalEpisode) {
  ASSERT_TRUE(crash_episode(false));
  const auto original = journal_bytes();
  make_startup();
  before_b = [&]() {
    write_bytes(original + " ");
  };
  ASSERT_TRUE(reconstruct());
  EXPECT_FALSE(context->reconstructed());
  EXPECT_EQ(driver_calls, 0u);
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
  const auto retained = context;
  before_b = {};
  write_bytes(original);
  EXPECT_EQ(reconstruct(), retained);
  EXPECT_TRUE(context->reconstructed());
  EXPECT_EQ(context->generation(), 1u);
  EXPECT_EQ(verifier->calls, 1u);
  EXPECT_EQ(b_calls, 2u);
  EXPECT_EQ(driver_calls, 0u);
}

TEST_F(OwnedReconstructionTest, IndependentDeploymentMismatchCannotOpenDriverOrReplacePendingIntent) {
  ASSERT_TRUE(crash_episode(false));
  const auto original = journal_bytes();
  verifier->deployment = "independent-other-deployment";
  make_startup();
  ASSERT_TRUE(reconstruct());
  EXPECT_FALSE(context->reconstructed());
  EXPECT_EQ(driver_calls, 0u);
  EXPECT_EQ(journal_bytes(), original);
  EXPECT_FALSE(verifier->lifetime.expired());
  EXPECT_FALSE(startup->release_for_ordinary());
}

TEST_F(OwnedDisplayTest, StartupExclusionSurvivesFailedConstructionAndExactJournalRecovery) {
  auto owned_path = std::filesystem::absolute(path).lexically_normal();
  owned_path.make_preferred();
  const auto ordinary_path = owned_path.parent_path() / (owned_path.filename().native() + L".ordinary.state");
  platf::source_startup::coordinator_t startup {{owned_path, ordinary_path}};
  ASSERT_EQ(startup.inspect().disposition, platf::source_startup::disposition_e::ordinary_clear);

  const auto failed_begin = owned_display_configuration_t::test_begin(path, first_identity, 1, authority, nullptr, devices);
  ASSERT_FALSE(failed_begin);
  EXPECT_FALSE(reset_persistence());
  ASSERT_TRUE(begin());
  const auto journal = source->journal();
  ASSERT_TRUE(journal);
  EXPECT_EQ(journal->record().generation, 1u);
  source.reset();
  EXPECT_FALSE(reset_persistence());

  const auto failed_recovery = owned_display_configuration_t::test_recover(path, authority, nullptr, devices);
  ASSERT_FALSE(failed_recovery);
  EXPECT_TRUE(ordinary_startup_recovery().mutation_excluded);
  auto recovered = owned_display_configuration_t::test_recover(path, authority, api, devices);
  ASSERT_TRUE(recovered);
  source = std::move(*recovered);
  EXPECT_EQ(source->status().generation, 1u);
  EXPECT_EQ(source->journal()->record().generation, journal->record().generation);
  EXPECT_FALSE(startup.release_for_ordinary());
  source.reset();
  EXPECT_FALSE(startup.release_for_ordinary());
  EXPECT_FALSE(init(ordinary_path, config::video_t {}));
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_EQ(model->topology_writes, 0u);
  EXPECT_EQ(model->removals, 0u);
  EXPECT_EQ(model->pings, 0u);
}

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

TEST_F(OwnedDisplayTest, ConcreteBackendBindsObservedVirtualSourceAndRevalidatesRetainedEpoch) {
  const auto backend = concrete_backend();
  auto request = concrete_request();
  ASSERT_TRUE(backend->begin(request, 1, 1));
  ASSERT_TRUE(concrete_until(backend, 1, [](const auto &state) {
    return state.phase == rtsp_stream::source_backend_status_t::phase_e::ready;
  }));
  const auto original = backend->status().source;
  ASSERT_TRUE(original);
  EXPECT_EQ(original->device_id, "virtual1");
  EXPECT_EQ(original->selector, "virtual1-capture");
  EXPECT_EQ(original->width, 1920U);
  EXPECT_EQ(original->rate_numerator, 60U);
  EXPECT_FALSE(original->requested_hdr);
  EXPECT_EQ(original->observed_hdr, false);
  EXPECT_TRUE(backend->validate_retained(*original, 2));
  EXPECT_EQ(backend->status().source->epoch, 2U);
  EXPECT_FALSE(backend->validate_retained(*original, 3));
  ASSERT_TRUE(backend->restore(1, 0ms));
  ASSERT_TRUE(backend->cleanup_and_retire(1));
  EXPECT_TRUE(backend->status().source_released);
  EXPECT_TRUE(concrete_until(backend, 1, [](const auto &state) {
    return !state.recovery_pending;
  }));
  EXPECT_TRUE(backend->complete(1));
  EXPECT_EQ(model->removals, 1U);
}

TEST_F(OwnedDisplayTest, ConcreteBackendArmedFailedAddKeepsHeartbeatUntilRestorationAndIssuedReceipt) {
  model->add_failure = true;
  const auto backend = concrete_backend();
  EXPECT_FALSE(backend->begin(concrete_request(), 7, 1));
  const auto diagnostics = platf::source_backend::test_diagnostics(*backend);
  ASSERT_TRUE(diagnostics.acquisition);
  EXPECT_EQ(diagnostics.acquisition->operation, operation_t::add);
  EXPECT_FALSE(backend->status().source_released);
  std::function<void()> tick;
  {
    std::lock_guard lock {model->mutex};
    tick = model->tick;
  }
  ASSERT_TRUE(tick);
  tick();
  EXPECT_GT(model->pings, 0U);
  EXPECT_EQ(model->removals, 0U);
  ASSERT_TRUE(backend->restore(7, 0ms));
  EXPECT_EQ(model->removals, 0U);
  ASSERT_TRUE(backend->cleanup_and_retire(7));
  EXPECT_EQ(model->removals, 1U);
  EXPECT_TRUE(backend->status().source_released);
  EXPECT_TRUE(concrete_until(backend, 7, [](const auto &state) {
    return !state.recovery_pending;
  }));
  EXPECT_TRUE(backend->complete(7));
}

TEST_F(OwnedDisplayTest, ConcreteBackendExceptionAfterIntentRetiresOnlyWithTheAdaptersUnarmedProof) {
  const auto backend = concrete_backend(true);
  EXPECT_THROW(backend->begin(concrete_request(), 9, 1), std::runtime_error);
  EXPECT_EQ(backend->status().generation, 9U);
  EXPECT_FALSE(backend->status().source_released);
  EXPECT_EQ(model->removals, 0U);
  EXPECT_FALSE(model->tick);
  ASSERT_TRUE(backend->restore(9, 0ms));
  ASSERT_TRUE(backend->cleanup_and_retire(9));
  EXPECT_TRUE(backend->status().source_released);
  EXPECT_EQ(model->removals, 0U);
  EXPECT_TRUE(concrete_until(backend, 9, [](const auto &state) {
    return !state.recovery_pending;
  }));
  EXPECT_TRUE(backend->complete(9));
}

TEST_F(OwnedDisplayTest, ConcreteBackendUnarmedAcquisitionDenialRequiresRealUnarmedReceipt) {
  {
    std::lock_guard lock {model->mutex};
    model->present.insert("virtual1");
  }
  const auto backend = concrete_backend();
  EXPECT_FALSE(backend->begin(concrete_request(), 1, 1));
  ASSERT_TRUE(platf::source_backend::test_diagnostics(*backend).acquisition);
  EXPECT_EQ(platf::source_backend::test_diagnostics(*backend).acquisition->code, errc_t::already_present);
  ASSERT_TRUE(backend->restore(1, 0ms));
  ASSERT_TRUE(backend->cleanup_and_retire(1));
  EXPECT_TRUE(concrete_until(backend, 1, [](const auto &state) {
    return !state.recovery_pending;
  }));
  EXPECT_TRUE(backend->complete(1));
  EXPECT_EQ(model->removals, 0U);
  EXPECT_TRUE(model->present.contains("virtual1"));
}

TEST_F(OwnedDisplayTest, ConcreteBackendStickyHeartbeatAndCleanupFailureRetainSeparateDiagnosticsAndOwnership) {
  const auto backend = concrete_backend();
  ASSERT_TRUE(backend->begin(concrete_request(), 1, 1));
  {
    std::lock_guard lock {model->mutex};
    model->heartbeat_failure = true;
  }
  std::function<void()> tick;
  {
    std::lock_guard lock {model->mutex};
    tick = model->tick;
  }
  ASSERT_TRUE(tick);
  tick();
  EXPECT_TRUE(backend->status().heartbeat_failed);
  backend->progress(1);
  EXPECT_TRUE(backend->status().heartbeat_failed);
  {
    std::lock_guard lock {model->mutex};
    model->heartbeat_failure = false;
    model->remove_failure = true;
  }
  tick();
  backend->progress(1);
  EXPECT_TRUE(backend->status().heartbeat_failed);
  ASSERT_TRUE(backend->restore(1, 0ms));
  EXPECT_FALSE(backend->cleanup_and_retire(1));
  EXPECT_FALSE(backend->status().source_released);
  const auto diagnostics = platf::source_backend::test_diagnostics(*backend);
  EXPECT_FALSE(diagnostics.acquisition);
  ASSERT_TRUE(diagnostics.cleanup);
  EXPECT_EQ(diagnostics.cleanup->code, errc_t::remove_failed);
  EXPECT_TRUE(diagnostics.liveness);
  {
    std::lock_guard lock {model->mutex};
    model->remove_failure = false;
  }
  ASSERT_TRUE(backend->cleanup_and_retire(1));
  EXPECT_TRUE(concrete_until(backend, 1, [](const auto &state) {
    return !state.recovery_pending;
  }));
  EXPECT_TRUE(backend->complete(1));
}

TEST_F(OwnedDisplayTest, ConcreteBackendRejectsNativeDuringRetiredVirtualRecoveryAndRollsActualNextGuid) {
  const auto backend = concrete_backend();
  auto request = concrete_request();
  // Keep a real dock obligation across both generations; topology-only recovery may finish early.
  request.topology = config::video_t::dd_t::config_option_e::ensure_primary;
  ASSERT_TRUE(backend->begin(request, 1, 1));
  ASSERT_NE(concrete_transaction, nullptr);
  const auto first_journal = concrete_transaction->journal();
  const auto applied = first_journal->settings();
  ASSERT_TRUE(applied);
  SingleDisplayConfigState applied_state;
  ASSERT_TRUE(fromJson(std::string {applied->begin(), applied->end()}, applied_state));
  EXPECT_EQ(applied_state.m_modified.m_original_primary_device, "dock");
  EXPECT_TRUE(applied_state.m_modified.m_original_modes.contains("dock"));
  {
    std::lock_guard lock {model->mutex};
    model->present.erase("dock");
    model->active = {{"virtual1"}};
    model->primary = "virtual1";
  }
  ASSERT_TRUE(backend->restore(1, 0ms));
  ASSERT_TRUE(backend->cleanup_and_retire(1));
  EXPECT_TRUE(backend->status().source_released);
  EXPECT_TRUE(backend->status().recovery_pending);
  EXPECT_TRUE(first_journal->source_retired());
  const auto pending = first_journal->settings();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->empty());
  SingleDisplayConfigState pending_state;
  ASSERT_TRUE(fromJson(std::string {pending->begin(), pending->end()}, pending_state));
  EXPECT_EQ(pending_state.m_initial.m_topology, (ActiveTopology {{"dock"}, {"panel"}}));
  EXPECT_EQ(pending_state.m_modified.m_topology, (ActiveTopology {{"dock"}}));
  EXPECT_EQ(pending_state.m_modified.m_original_primary_device, "dock");
  EXPECT_TRUE(pending_state.m_modified.m_original_modes.contains("dock"));
  {
    std::lock_guard lock {model->mutex};
    EXPECT_EQ(model->removals, 1U);
    EXPECT_EQ(model->active, (ActiveTopology {{"panel"}}));
    EXPECT_EQ(model->primary, "panel");
    EXPECT_FALSE(model->present.contains("virtual1"));
  }
  auto native = concrete_request();
  native.virtual_display = false;
  EXPECT_FALSE(backend->admits(native));
  EXPECT_FALSE(backend->begin(native, 2, 2));
  EXPECT_EQ(backend->status().generation, 1U);
  EXPECT_TRUE(backend->admits(concrete_request(true)));
  ASSERT_TRUE(backend->begin(concrete_request(true), 3, 3));
  ASSERT_TRUE(backend->status().source);
  EXPECT_EQ(backend->status().source->monitor_uuid, concrete_request(true).monitor_uuid);
  EXPECT_EQ(backend->status().source->device_id, "virtual2");
  const auto second_journal = concrete_transaction->journal();
  EXPECT_TRUE(IsEqualGUID(second_journal->record().identity, second_identity));
  EXPECT_EQ(second_journal->record().generation, 3U);
  EXPECT_FALSE(second_journal->source_retired());
  const auto carried = second_journal->settings();
  ASSERT_TRUE(carried);
  SingleDisplayConfigState carried_state;
  ASSERT_TRUE(fromJson(std::string {carried->begin(), carried->end()}, carried_state));
  EXPECT_EQ(carried_state.m_initial.m_topology, pending_state.m_initial.m_topology);
  EXPECT_EQ(carried_state.m_modified.m_original_primary_device, "dock");
  EXPECT_TRUE(carried_state.m_modified.m_original_modes.contains("dock"));
  backend->progress(1);
  EXPECT_EQ(backend->status().generation, 3U);
  {
    std::lock_guard lock {model->mutex};
    EXPECT_EQ(model->removals, 1U);
    EXPECT_EQ(model->active, (ActiveTopology {{"virtual2"}}));
    model->present.insert("dock");
  }
  ASSERT_TRUE(backend->restore(3, 0ms));
  ASSERT_TRUE(backend->cleanup_and_retire(3));
  EXPECT_TRUE(concrete_until(backend, 3, [](const auto &state) {
    return !state.recovery_pending;
  }));
  EXPECT_TRUE(second_journal->source_retired());
  ASSERT_TRUE(second_journal->settings());
  EXPECT_TRUE(second_journal->settings()->empty());
  {
    std::lock_guard lock {model->mutex};
    EXPECT_EQ(model->removals, 2U);
    EXPECT_EQ(model->active, (ActiveTopology {{"dock"}, {"panel"}}));
    EXPECT_EQ(model->primary, "dock");
    EXPECT_EQ(model->modes.at("dock"), (DisplayMode {{3840, 2160}, {60, 1}}));
    EXPECT_EQ(model->hdr.at("dock"), HdrState::Enabled);
    EXPECT_FALSE(model->present.contains("virtual1"));
    EXPECT_FALSE(model->present.contains("virtual2"));
  }
  EXPECT_TRUE(backend->complete(3));
}

TEST_F(OwnedDisplayTest, ConcreteBackendRetriesCloseAfterRemovalWithoutRepeatingRemovalOrFabricatingReceipt) {
  for (const bool headless : {false, true}) {
    if (headless) {
      std::lock_guard lock {model->mutex};
      model->active.clear();
      model->present.clear();
      model->primary.clear();
    }
    const auto prior_removals = model->removals;
    const auto backend = concrete_backend();
    ASSERT_TRUE(backend->begin(concrete_request(), 1, 1));
    ASSERT_TRUE(backend->restore(1, 0ms));
    {
      std::lock_guard lock {model->mutex};
      model->close_failure = true;
    }
    EXPECT_FALSE(backend->cleanup_and_retire(1));
    EXPECT_EQ(model->removals, prior_removals + 1);
    EXPECT_FALSE(backend->status().source_released);
    ASSERT_TRUE(platf::source_backend::test_diagnostics(*backend).cleanup);
    EXPECT_EQ(platf::source_backend::test_diagnostics(*backend).cleanup->code, errc_t::close_failed);
    {
      std::lock_guard lock {model->mutex};
      model->close_failure = false;
    }
    ASSERT_TRUE(backend->cleanup_and_retire(1));
    EXPECT_EQ(model->removals, prior_removals + 1);
    EXPECT_TRUE(backend->status().source_released);
    EXPECT_TRUE(concrete_until(backend, 1, [](const auto &state) {
      return !state.recovery_pending;
    }));
    EXPECT_TRUE(backend->complete(1));
  }
}

namespace {
  /** @brief Actual concrete adapters and pinned managers under the real owner, with inert Windows/codec boundaries. */
  class ConcreteSourceRecoveryOwnerTest: public OwnedDisplayTest {
  protected:
    std::shared_ptr<rtsp_stream::source_backend_t> backend;
    std::shared_ptr<SettingsPersistenceInterface> native_persistence;
    std::unique_ptr<platf::deinit_t> owner;
    crypto::creds_t credentials;
    std::string client_uuid, previous_client_state;
    bool previous_fresh_state = false;
    std::vector<std::shared_ptr<rtsp_stream::launch_session_t>> launches;

    void SetUp() override {
      OwnedDisplayTest::SetUp();
      owner_boundaries = true;
      previous_client_state = config::nvhttp.file_state;
      previous_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
      config::sunshine.flags[config::flag::FRESH_STATE] = false;
      config::nvhttp.file_state = path.string() + ".clients.json";
      std::error_code error;
      std::filesystem::remove(config::nvhttp.file_state, error);
      nvhttp::test_support::reset_client_state();
      credentials = test_utils::certificates::generate_ca_credentials("concrete recovery");
      client_uuid = nvhttp::test_support::add_client("concrete recovery", credentials.x509, true);
      ASSERT_TRUE(nvhttp::update_client(client_uuid, std::nullopt, permissions::view | permissions::launch));
      proc::test_support::set_lifetime_snapshot_hook([]() {
        return proc::lifetime_snapshot_t {};
      });
      native_persistence = test_make_watched_persistence();
      auto manager = std::make_unique<SettingsManager>(devices, std::make_shared<model_audio_t>(), std::make_unique<PersistentState>(native_persistence, true), WinWorkarounds {});
      test_install_settings_manager(std::move(manager), native_persistence);
      test_set_retry_interval(10ms);
      backend = concrete_backend();
      ASSERT_TRUE(rtsp_stream::set_source_backend([value = backend]() {
        return value;
      }));
      owner = rtsp_stream::test_support::start_owner();
    }

    void TearDown() override {
      {
        std::lock_guard lock {model->mutex};
        model->api_check_hook = {};
        model->present.insert("dock");
        model->present.insert("panel");
        model->remove_failure = false;
        model->close_failure = false;
        model->api_unavailable = false;
      }
      for (const auto &launch : launches) {
        rtsp_stream::launch_session_clear(launch->id);
      }
      EXPECT_TRUE(owner_until([]() {
        rtsp_stream::retry_source_cleanup();
        const auto state = rtsp_stream::source_snapshot();
        return !state.managed && !state.recovery_pending;
      }));
      owner.reset();
      EXPECT_TRUE(rtsp_stream::set_source_backend({}));
      backend.reset();
      proc::test_support::set_lifetime_snapshot_hook({});
      nvhttp::test_support::reset_client_state();
      std::error_code error;
      std::filesystem::remove(config::nvhttp.file_state, error);
      config::nvhttp.file_state = previous_client_state;
      config::sunshine.flags[config::flag::FRESH_STATE] = previous_fresh_state;
      OwnedDisplayTest::TearDown();
    }

    bool owner_until(const std::function<bool()> &predicate) {
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

    std::shared_ptr<rtsp_stream::launch_session_t> token() {
      const SimpleWeb::CaseInsensitiveMultimap args {{"rikey", "00112233445566778899aabbccddeeff"}, {"rikeyid", "1"}, {"appid", "0"}};
      auto launch = nvhttp::test_support::make_launch_session(*nvhttp::current_client(client_uuid, credentials.x509), args);
      launch->appid = 0;
      launch->app_uuid.clear();
      launches.push_back(launch);
      return launch;
    }

    int prepare(const std::shared_ptr<rtsp_stream::launch_session_t> &launch, display_policy::source_request_t request) {
      return rtsp_stream::prepare_source_launch(launch, [request = std::make_shared<const display_policy::source_request_t>(std::move(request))](const auto &) {
        return std::pair {200, request};
      });
    }

    display_policy::source_request_t native_request(bool configure = false) {
      auto request = concrete_request();
      request.virtual_display = false;
      if (configure) {
        request.native_action = display_policy::native_action_e::configure;
        request.native_configuration = SingleDisplayConfiguration {.m_device_id = "dock", .m_device_prep = SingleDisplayConfiguration::DevicePreparation::VerifyOnly, .m_resolution = Resolution {1280, 720}};
      }
      return request;
    }
  };
}  // namespace

TEST_F(ConcreteSourceRecoveryOwnerTest, PublicRetryRestartsStoppedNativeRestorationWithoutClearingPersistence) {
  auto request = native_request(true);
  request.restore_delay = 1ms;
  const auto first = token();
  ASSERT_EQ(prepare(first, request), 200);
  const auto generation = first->source_generation;
  ASSERT_TRUE(native_persistence->load());
  EXPECT_FALSE(native_persistence->load()->empty());
  std::promise<void> entered, release;
  const auto gate = release.get_future().share();
  auto first_attempt = std::make_shared<std::atomic_bool>(true);
  {
    std::lock_guard lock {model->mutex};
    model->api_check_hook = [first_attempt, gate, &entered]() {
      if (first_attempt->exchange(false)) {
        entered.set_value();
        EXPECT_EQ(gate.wait_for(2s), std::future_status::ready);
        throw std::runtime_error {"inert native restore failure"};
      }
    };
  }
  auto unblock = util::fail_guard([&]() {
    release.set_value();
  });
  rtsp_stream::launch_session_clear(first->id);
  ASSERT_EQ(entered.get_future().wait_for(1s), std::future_status::ready);
  ASSERT_TRUE(owner_until([]() {
    const auto state = rtsp_stream::source_snapshot();
    return !state.managed && state.recovery_pending;
  }));
  EXPECT_EQ(prepare(token(), concrete_request()), 503);
  release.set_value();
  unblock.disable();
  ASSERT_TRUE(owner_until([&]() {
    return backend->status().recovery_retry_required && !rtsp_stream::source_snapshot().busy;
  }));
  EXPECT_EQ(backend->status().generation, generation);
  ASSERT_TRUE(native_persistence->load());
  EXPECT_FALSE(native_persistence->load()->empty());
  {
    std::lock_guard lock {model->mutex};
    EXPECT_EQ(model->modes.at("dock").m_resolution, (Resolution {1280, 720}));
    EXPECT_EQ(model->removals, 0U);
    model->api_check_hook = {};
  }
  EXPECT_FALSE(backend->admits(concrete_request()));
  rtsp_stream::retry_source_cleanup();
  ASSERT_TRUE(owner_until([]() {
    return !rtsp_stream::source_snapshot().recovery_pending;
  }));
  ASSERT_TRUE(native_persistence->load());
  EXPECT_TRUE(native_persistence->load()->empty());
  {
    std::lock_guard lock {model->mutex};
    EXPECT_EQ(model->modes.at("dock").m_resolution, (Resolution {3840, 2160}));
    EXPECT_EQ(model->removals, 0U);
  }
  EXPECT_FALSE(backend->status().recovery_retry_required);
  EXPECT_EQ(backend->status().generation, generation);
  EXPECT_TRUE(backend->admits(concrete_request()));
  const auto next = token();
  ASSERT_EQ(prepare(next, concrete_request()), 200);
  rtsp_stream::launch_session_clear(next->id);
  ASSERT_TRUE(owner_until([]() {
    const auto state = rtsp_stream::source_snapshot();
    return !state.managed && !state.recovery_pending;
  }));
  EXPECT_EQ(model->removals, 1U);
}

TEST_F(ConcreteSourceRecoveryOwnerTest, PublicRetryRestartsRetiredVirtualRestorationAndWaitsForActualPhysicalRecovery) {
  const auto first = token();
  auto request = concrete_request();
  // EnsurePrimary records the dock's real primary/mode obligations before it disappears.
  // A virtual-only change can legitimately leave no pending settings after fallback recovery.
  request.topology = config::video_t::dd_t::config_option_e::ensure_primary;
  ASSERT_EQ(prepare(first, request), 200);
  const auto generation = first->source_generation;
  ASSERT_NE(concrete_transaction, nullptr);
  const auto journal = concrete_transaction->journal();
  const auto applied = journal->settings();
  ASSERT_TRUE(applied);
  SingleDisplayConfigState applied_state;
  ASSERT_TRUE(fromJson(std::string {applied->begin(), applied->end()}, applied_state));
  EXPECT_EQ(applied_state.m_modified.m_original_primary_device, "dock");
  EXPECT_TRUE(applied_state.m_modified.m_original_modes.contains("dock"));
  auto retired_entered = std::make_shared<std::promise<void>>();
  const auto entered = retired_entered->get_future();
  std::promise<void> release;
  const auto gate = release.get_future().share();
  auto fail_once = std::make_shared<std::atomic_bool>(true);
  auto retired_attempts = std::make_shared<std::atomic_uint>(0);
  {
    std::lock_guard lock {model->mutex};
    EXPECT_EQ(model->primary, "virtual1");
    model->present.erase("dock");
    model->active = {{"virtual1"}};
    model->api_check_hook = [this, fail_once, retired_attempts, retired_entered, gate]() {
      if (concrete_transaction->status().source_released) {
        ++*retired_attempts;
        if (fail_once->exchange(false)) {
          retired_entered->set_value();
          gate.wait();
          throw std::runtime_error {"inert retired B restore failure"};
        }
      }
    };
  }
  auto unblock = util::fail_guard([&]() {
    release.set_value();
  });
  rtsp_stream::launch_session_clear(first->id);
  ASSERT_TRUE(owner_until([&]() {
    return entered.wait_for(0s) == std::future_status::ready;
  }));
  EXPECT_TRUE(concrete_transaction->status().source_released);
  EXPECT_TRUE(journal->source_retired());
  EXPECT_EQ(model->removals, 1U);
  const auto gated = rtsp_stream::source_snapshot();
  EXPECT_TRUE(gated.managed);
  EXPECT_TRUE(gated.busy);
  const auto pending = journal->settings();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->empty());
  SingleDisplayConfigState pending_state;
  ASSERT_TRUE(fromJson(std::string {pending->begin(), pending->end()}, pending_state));
  EXPECT_EQ(pending_state.m_initial.m_topology, (ActiveTopology {{"dock"}, {"panel"}}));
  EXPECT_EQ(pending_state.m_modified.m_topology, (ActiveTopology {{"dock"}}));
  EXPECT_EQ(pending_state.m_modified.m_original_primary_device, "dock");
  EXPECT_TRUE(pending_state.m_modified.m_original_modes.contains("dock"));
  release.set_value();
  unblock.disable();
  ASSERT_TRUE(owner_until([&]() {
    const auto state = rtsp_stream::source_snapshot();
    return !state.managed && !state.busy && state.recovery_pending && backend->status().recovery_retry_required;
  }));
  EXPECT_EQ(backend->status().generation, generation);
  EXPECT_EQ(model->removals, 1U);
  EXPECT_EQ(retired_attempts->load(), 1U);
  EXPECT_EQ(journal->settings(), pending);
  EXPECT_EQ(prepare(token(), native_request()), 503);
  EXPECT_FALSE(backend->retry_recovery(generation + 1));
  const auto before_retry = retired_attempts->load();
  rtsp_stream::retry_source_cleanup();
  ASSERT_TRUE(owner_until([&]() {
    return retired_attempts->load() > before_retry && !backend->status().recovery_retry_required && !rtsp_stream::source_snapshot().busy;
  }));
  EXPECT_TRUE(rtsp_stream::source_snapshot().recovery_pending);
  EXPECT_EQ(backend->status().generation, generation);
  EXPECT_EQ(journal->record().generation, generation);
  EXPECT_TRUE(journal->source_retired());
  ASSERT_TRUE(journal->settings());
  EXPECT_FALSE(journal->settings()->empty());
  EXPECT_EQ(journal->settings(), pending);
  EXPECT_FALSE(backend->admits(native_request()));
  EXPECT_EQ(model->removals, 1U);
  {
    std::lock_guard lock {model->mutex};
    EXPECT_EQ(model->active, (ActiveTopology {{"panel"}}));
    EXPECT_EQ(model->primary, "panel");
    model->api_check_hook = {};
    model->present.insert("dock");
  }
  ASSERT_TRUE(owner_until([]() {
    return !rtsp_stream::source_snapshot().recovery_pending;
  }));
  EXPECT_TRUE(backend->admits(native_request()));
  EXPECT_EQ(backend->status().generation, generation);
  EXPECT_EQ(model->removals, 1U);
  {
    std::lock_guard lock {model->mutex};
    EXPECT_EQ(model->active, (ActiveTopology {{"dock"}, {"panel"}}));
    EXPECT_TRUE(model->active_device("dock"));
    EXPECT_EQ(model->primary, "dock");
  }
  const auto next = token();
  ASSERT_EQ(prepare(next, native_request()), 200);
  rtsp_stream::launch_session_clear(next->id);
  ASSERT_TRUE(owner_until([]() {
    const auto state = rtsp_stream::source_snapshot();
    return !state.managed && !state.recovery_pending;
  }));
  EXPECT_EQ(model->removals, 1U);
}
#endif
