/**
 * @file src/platform/windows/sudovda.cpp
 * @brief Checked SudoVDA requests and one recoverable display-source lease.
 */
// class header include
#include "sudovda.h"

// standard includes
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

// lib includes
#include <nlohmann/json.hpp>

// platform includes
#include <setupapi.h>

// local includes
#include "src/httpcommon.h"
#include "src/logging.h"
#include "utf_utils.h"

namespace platf::sudovda {
  namespace {
    std::atomic_bool active_lease {};

    bool valid_authority(const recovery_authority_t &authority, const std::string &deployment) {
      return authority.trusted_storage && authority.exclusive_driver_control &&
             !deployment.empty() && deployment.size() <= 128 && authority.deployment == deployment;
    }

    bool valid_device_ids(const std::vector<std::string> &ids) {
      return ids.size() <= 256 && std::ranges::all_of(ids, [](const auto &id) {
               return !id.empty() && id.size() <= 1024 && id.find('\0') == std::string::npos;
             });
    }

    bool same_monitor_path(const std::wstring &left, const std::wstring &right) {
      return left.size() <= 32768 && right.size() <= 32768 && CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
    }

    std::optional<ownership_mapping_t> decode_mapping(const nlohmann::json &data) {
      const auto &mapping {data.at("mapping")};
      if (mapping.is_null()) {
        return std::nullopt;
      }
      if (!mapping.is_object() || mapping.size() != 7 || !mapping.at("generation").is_number_unsigned() || mapping.at("generation") != data.at("generation") || mapping.at("identity") != data.at("identity") || !mapping.at("adapter_low").is_number_unsigned() || mapping.at("adapter_low").get<std::uint64_t>() > std::numeric_limits<ULONG>::max() || !mapping.at("adapter_high").is_number_integer() || (mapping.at("adapter_high").is_number_unsigned() && mapping.at("adapter_high").get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<LONG>::max())) || mapping.at("adapter_high").get<std::int64_t>() < std::numeric_limits<LONG>::min() || mapping.at("adapter_high").get<std::int64_t>() > std::numeric_limits<LONG>::max() || !mapping.at("target").is_number_unsigned() || mapping.at("target").get<std::uint64_t>() > std::numeric_limits<UINT>::max()) {
        throw std::invalid_argument {"Invalid ownership mapping"};
      }
      ownership_mapping_t result {{{mapping.at("adapter_low").get<ULONG>(), mapping.at("adapter_high").get<LONG>()}, mapping.at("target").get<UINT>()}, utf_utils::from_utf8(mapping.at("monitor_path").get<std::string>()), mapping.at("device_id").get<std::string>()};
      if (result.monitor_path.empty() || result.monitor_path.size() > 32768 || result.monitor_path.find(L'\0') != std::wstring::npos || !valid_device_ids({result.device_id})) {
        throw std::invalid_argument {"Invalid ownership mapping"};
      }
      return result;
    }

    nlohmann::json journal_json(const ownership_record_t &record, const std::vector<std::string> &observed_devices) {
      const auto bytes {std::as_bytes(std::span {&record.identity, 1})};
      std::vector<unsigned int> identity;
      for (const auto byte : bytes) {
        identity.push_back(std::to_integer<unsigned int>(byte));
      }
      const std::set<std::string> devices {observed_devices.begin(), observed_devices.end()};
      return {{"schema", 2}, {"cleanup_pending", true}, {"add_armed", false}, {"source_retired", false}, {"identity", identity}, {"generation", record.generation}, {"deployment", record.deployment}, {"initially_headless", record.initially_headless}, {"baseline", record.baseline}, {"settings", record.baseline}, {"restoration_state", record.baseline}, {"recovery_ids", devices}, {"mapping", nullptr}};
    }

    bool valid_record(const ownership_record_t &record) {
      return !IsEqualGUID(record.identity, GUID {}) && record.generation &&
             !record.deployment.empty() && record.deployment.size() <= 128 &&
             record.baseline.size() <= 1024 * 1024 &&
             record.initially_headless == record.baseline.empty();
    }

    operation_t operation_for(DWORD code) {
      switch (code) {
        case wire::version:
          return operation_t::version;
        case wire::watchdog:
          return operation_t::watchdog;
        case wire::set_adapter:
          return operation_t::render_adapter;
        case wire::add:
          return operation_t::add;
        case wire::remove:
          return operation_t::remove;
        default:
          return operation_t::heartbeat;
      }
    }

    template<class T>
    std::span<const std::byte> input_bytes(const T &value) {
      return std::as_bytes(std::span {&value, 1});
    }

    template<class T>
    std::span<std::byte> output_bytes(T &value) {
      return std::as_writable_bytes(std::span {&value, 1});
    }

    bool same_luid(const LUID &left, const LUID &right) {
      return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
    }

    std::expected<std::optional<display_t>, failure_t> normalized_owned_display(const enumeration_t &enumeration, const GUID &identity, const std::optional<wire::target_t> &expected = std::nullopt) {
      const bool present {std::ranges::any_of(enumeration.identities, [&](const auto &id) {
        return IsEqualGUID(id, identity);
      })};
      std::optional<display_t> result;
      std::optional<std::tuple<LONG, ULONG, UINT>> active_source;
      std::optional<std::pair<LONG, ULONG>> source_adapter;
      std::set<UINT> sources;
      for (const auto &display : enumeration.displays) {
        if (!IsEqualGUID(display.identity, identity)) {
          continue;
        }
        if (!present || (expected && (!same_luid(display.target.adapter, expected->adapter) || display.target.target != expected->target)) || (result && (!same_luid(display.target.adapter, result->target.adapter) || display.target.target != result->target.target || !same_monitor_path(display.monitor_path, result->monitor_path)))) {
          return std::unexpected(failure_t {errc_t::ownership_unverified, operation_t::enumerate});
        }
        if (display.monitor_path.empty() || display.gdi_name.empty()) {
          return std::unexpected(failure_t {errc_t::enumeration_timeout, operation_t::enumerate});
        }
        if (display.source_metadata_valid) {
          const auto adapter {std::pair {display.source_adapter.HighPart, display.source_adapter.LowPart}};
          if ((source_adapter && *source_adapter != adapter) || !sources.insert(display.source).second) {
            return std::unexpected(failure_t {errc_t::ownership_unverified, operation_t::enumerate});
          }
          source_adapter = adapter;
        }
        if (display.source_metadata_valid && display.active) {
          const auto source {std::tuple {display.source_adapter.HighPart, display.source_adapter.LowPart, display.source}};
          if (active_source && (*active_source != source || result->gdi_name != display.gdi_name)) {
            return std::unexpected(failure_t {errc_t::ownership_unverified, operation_t::enumerate});
          }
          active_source = source;
          result = display;
        } else if (!result) {
          result = display;
        }
      }
      return result;
    }

    std::expected<wire::add_t, failure_t> checked_request(const request_t &request) {
      if (!request.exclusive_driver_control) {
        return std::unexpected(failure_t {errc_t::exclusive_control_required, operation_t::add});
      }
      constexpr auto max_uint = std::numeric_limits<UINT>::max();
      if (!request.width || request.width > max_uint || !request.height || request.height > max_uint || !request.rate_numerator || !request.rate_denominator || !request.enumeration_attempts || request.enumeration_attempts > 100 || request.enumeration_interval.count() < 0 || request.enumeration_interval > std::chrono::milliseconds {100}) {
        return std::unexpected(failure_t {errc_t::invalid_mode, operation_t::add});
      }

      // Reduce before multiplying, so even uint64 rational inputs cannot overflow.
      const auto divisor = std::gcd(request.rate_denominator, std::uint64_t {1000});
      const auto denominator = request.rate_denominator / divisor;
      if (request.rate_numerator % denominator) {
        return std::unexpected(failure_t {errc_t::rate_precision, operation_t::add});
      }
      const auto numerator = request.rate_numerator / denominator;
      const auto multiplier = 1000 / divisor;
      if (numerator > max_uint / multiplier || numerator * multiplier < 1000) {
        return std::unexpected(failure_t {errc_t::unsupported_rate, operation_t::add});
      }
      const auto metadata_ok = [](const std::string &text) {
        return !text.empty() && text.size() <= 13 && std::ranges::all_of(text, [](unsigned char c) {
          return c >= 0x20 && c <= 0x7e;
        });
      };
      if (IsEqualGUID(request.identity, GUID {}) || !metadata_ok(request.name) || !metadata_ok(request.serial)) {
        return std::unexpected(failure_t {errc_t::invalid_metadata, operation_t::add});
      }
      wire::add_t result {};
      result.width = static_cast<UINT>(request.width);
      result.height = static_cast<UINT>(request.height);
      result.millihertz = static_cast<UINT>(numerator * multiplier);
      result.identity = request.identity;
      std::memcpy(result.name, request.name.data(), request.name.size());
      std::memcpy(result.serial, request.serial.data(), request.serial.size());
      return result;
    }

    class win32_heartbeat_t final: public heartbeat_t {
    public:
      win32_heartbeat_t(std::chrono::milliseconds period, std::function<void()> callback):
          worker_([this, period, callback = std::move(callback)](std::stop_token stop) {
            std::unique_lock lock {mutex_};
            while (!condition_.wait_for(lock, stop, period, [] {
              return false;
            })) {
              if (stop.stop_requested()) {
                break;
              }
              lock.unlock();
              callback();
              lock.lock();
            }
          }) {}

      ~win32_heartbeat_t() override {
        stop();
      }

      void stop() noexcept override {
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable()) {
          worker_.join();
        }
      }

    private:
      std::mutex mutex_;
      std::condition_variable_any condition_;
      std::jthread worker_;
    };

    struct device_set_t {
      HDEVINFO handle;

      ~device_set_t() {
        if (handle != INVALID_HANDLE_VALUE && !SetupDiDestroyDeviceInfoList(handle)) {
          BOOST_LOG(error) << "SudoVDA device-information close failed: " << GetLastError();
        }
      }
    };

    class win32_system_t final: public system_t {
    public:
      ~win32_system_t() override {
        if (auto result = close(); !result) {
          BOOST_LOG(error) << "SudoVDA handle close failed: " << result.error().system_error;
        }
      }

      std::expected<void, failure_t> open() override {
        if (handle_ != INVALID_HANDLE_VALUE) {
          return std::unexpected(failure_t {errc_t::open_failed, operation_t::open, ERROR_ALREADY_INITIALIZED});
        }
        device_set_t devices {SetupDiGetClassDevsW(&wire::interface_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)};
        if (devices.handle == INVALID_HANDLE_VALUE) {
          return std::unexpected(failure_t {errc_t::open_failed, operation_t::open, GetLastError()});
        }
        SP_DEVICE_INTERFACE_DATA interface_data {};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(devices.handle, nullptr, &wire::interface_guid, 0, &interface_data)) {
          const auto error = GetLastError();
          return std::unexpected(failure_t {error == ERROR_NO_MORE_ITEMS ? errc_t::unavailable : errc_t::open_failed, operation_t::open, error});
        }
        DWORD bytes {};
        if (SetupDiGetDeviceInterfaceDetailW(devices.handle, &interface_data, nullptr, 0, &bytes, nullptr) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
          return std::unexpected(failure_t {errc_t::open_failed, operation_t::open, GetLastError()});
        }
        std::vector<std::byte> storage(bytes);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devices.handle, &interface_data, detail, bytes, nullptr, nullptr)) {
          return std::unexpected(failure_t {errc_t::open_failed, operation_t::open, GetLastError()});
        }
        handle_ = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
          return std::unexpected(failure_t {errc_t::open_failed, operation_t::open, GetLastError()});
        }
        return {};
      }

      std::expected<void, failure_t> close() override {
        if (handle_ != INVALID_HANDLE_VALUE) {
          if (!CloseHandle(handle_)) {
            return std::unexpected(failure_t {errc_t::close_failed, operation_t::close, GetLastError()});
          }
          handle_ = INVALID_HANDLE_VALUE;
        }
        return {};
      }

      std::expected<std::size_t, failure_t> ioctl(DWORD code, std::span<const std::byte> input, std::span<std::byte> output) override {
        const auto operation = operation_for(code);
        if (input.size() > std::numeric_limits<DWORD>::max() || output.size() > std::numeric_limits<DWORD>::max()) {
          return std::unexpected(failure_t {errc_t::transport_failed, operation, ERROR_INVALID_PARAMETER});
        }
        OVERLAPPED overlapped {};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) {
          return std::unexpected(failure_t {errc_t::transport_failed, operation, GetLastError()});
        }
        DWORD returned {};
        BOOL success = DeviceIoControl(handle_, code, const_cast<std::byte *>(input.data()), static_cast<DWORD>(input.size()), output.data(), static_cast<DWORD>(output.size()), &returned, &overlapped);
        DWORD error = success ? ERROR_SUCCESS : GetLastError();
        if (!success && error == ERROR_IO_PENDING) {
          const auto wait = WaitForSingleObject(overlapped.hEvent, 1000);
          if (wait == WAIT_OBJECT_0) {
            success = GetOverlappedResult(handle_, &overlapped, &returned, FALSE);
            error = success ? ERROR_SUCCESS : GetLastError();
          } else {
            error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            CancelIoEx(handle_, &overlapped);
            // Join cancellation before stack buffers/event go out of scope.
            GetOverlappedResult(handle_, &overlapped, &returned, TRUE);
          }
        }
        CloseHandle(overlapped.hEvent);
        if (!success) {
          return std::unexpected(failure_t {errc_t::transport_failed, operation, error});
        }
        return returned;
      }

      std::expected<enumeration_t, failure_t> enumerate() override {
        enumeration_t result;
        constexpr GUID monitor_interface {0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};
        constexpr DEVPROPKEY container_key {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
        device_set_t devices {SetupDiGetClassDevsW(&monitor_interface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)};
        const auto failed = [](DWORD error) {
          return std::unexpected(failure_t {errc_t::enumeration_failed, operation_t::enumerate, error});
        };
        if (devices.handle == INVALID_HANDLE_VALUE) {
          return failed(GetLastError());
        }
        std::vector<std::pair<std::wstring, GUID>> containers;
        for (DWORD index = 0;; ++index) {
          SP_DEVICE_INTERFACE_DATA interface_data {};
          interface_data.cbSize = sizeof(interface_data);
          if (!SetupDiEnumDeviceInterfaces(devices.handle, nullptr, &monitor_interface, index, &interface_data)) {
            const auto error = GetLastError();
            if (error == ERROR_NO_MORE_ITEMS) {
              break;
            }
            return failed(error);
          }
          DWORD bytes {};
          if (SetupDiGetDeviceInterfaceDetailW(devices.handle, &interface_data, nullptr, 0, &bytes, nullptr) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            return failed(GetLastError());
          }
          std::vector<std::byte> storage(bytes);
          auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
          detail->cbSize = sizeof(*detail);
          SP_DEVINFO_DATA device_data {};
          device_data.cbSize = sizeof(device_data);
          if (!SetupDiGetDeviceInterfaceDetailW(devices.handle, &interface_data, detail, bytes, nullptr, &device_data)) {
            return failed(GetLastError());
          }
          GUID identity {};
          DEVPROPTYPE type {};
          DWORD returned {};
          if (!SetupDiGetDevicePropertyW(devices.handle, &device_data, &container_key, &type, reinterpret_cast<PBYTE>(&identity), sizeof(identity), &returned, 0)) {
            return failed(GetLastError());
          }
          if (type != DEVPROP_TYPE_GUID || returned != sizeof(identity)) {
            return failed(ERROR_INVALID_DATA);
          }
          containers.emplace_back(detail->DevicePath, identity);
          result.identities.push_back(identity);
        }

        UINT path_count {}, mode_count {};
        auto status = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &path_count, &mode_count);
        if (status != ERROR_SUCCESS) {
          return failed(status);
        }
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        status = QueryDisplayConfig(QDC_ALL_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
        if (status != ERROR_SUCCESS) {
          return failed(status);
        }
        paths.resize(path_count);
        for (const auto &path : paths) {
          if (!path.targetInfo.targetAvailable) {
            continue;
          }
          DISPLAYCONFIG_TARGET_DEVICE_NAME target_name {};
          target_name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target_name), path.targetInfo.adapterId, path.targetInfo.id};
          if (DisplayConfigGetDeviceInfo(&target_name.header) != ERROR_SUCCESS) {
            continue;
          }
          const auto container = std::ranges::find_if(containers, [&](const auto &entry) {
            return _wcsicmp(entry.first.c_str(), target_name.monitorDevicePath) == 0;
          });
          if (container == containers.end()) {
            continue;
          }
          DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name {};
          source_name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source_name), path.sourceInfo.adapterId, path.sourceInfo.id};
          if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS || !source_name.viewGdiDeviceName[0]) {
            continue;
          }
          result.displays.push_back({{path.targetInfo.adapterId, path.targetInfo.id}, container->second, target_name.monitorDevicePath, source_name.viewGdiDeviceName, path.sourceInfo.adapterId, path.sourceInfo.id, (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0, true});
        }
        return result;
      }

      std::expected<std::unique_ptr<heartbeat_t>, failure_t> heartbeat(std::chrono::milliseconds period, std::function<void()> callback) override {
        try {
          return std::make_unique<win32_heartbeat_t>(period, std::move(callback));
        } catch (const std::system_error &error) {
          return std::unexpected(failure_t {errc_t::heartbeat_failed, operation_t::heartbeat, static_cast<DWORD>(error.code().value())});
        }
      }

      void wait_for_enumeration(std::chrono::milliseconds delay) override {
        std::this_thread::sleep_for(delay);
      }

    private:
      HANDLE handle_ {INVALID_HANDLE_VALUE};
    };
  }  // namespace

  struct ownership_journal_t::state_t {
    std::filesystem::path path;
    ownership_record_t record;
    nlohmann::json data;
    std::string bytes;
    bool removal_confirmed {};
    bool unarmed_confirmed {};
    mutable std::mutex mutex;

    bool unchanged() const {
      bool matches {false};
      const bool read {http::update_state_file_bytes(path.string(), [&](const std::string &current) -> std::optional<std::string> {
        matches = !bytes.empty() && current == bytes;
        return std::nullopt;
      })};
      return read && matches;
    }

    bool replace(nlohmann::json replacement) {
      const auto next_bytes {replacement.dump()};
      bool matches {false};
      const bool stored {http::update_state_file_bytes(path.string(), [&](const std::string &current) -> std::optional<std::string> {
        matches = !bytes.empty() && current == bytes;
        return matches ? std::make_optional(next_bytes) : std::nullopt;
      })};
      if (!stored || !matches) {
        return false;
      }
      data = std::move(replacement);
      bytes = next_bytes;
      return true;
    }
  };

  ownership_journal_t::ownership_journal_t(std::shared_ptr<state_t> state):
      state_ {std::move(state)} {}

  cleanup_receipt_t::cleanup_receipt_t(std::shared_ptr<ownership_journal_t> journal, GUID identity, std::uint64_t generation, kind_e kind):
      journal_ {std::move(journal)},
      identity_ {identity},
      generation_ {generation},
      kind_ {kind} {}

  cleanup_receipt_t::kind_e cleanup_receipt_t::kind() const {
    return kind_;
  }

  std::expected<std::shared_ptr<ownership_journal_t>, failure_t> ownership_journal_t::create(const std::filesystem::path &path, const ownership_record_t &record, const recovery_authority_t &authority, const std::vector<std::string> &observed_devices) {
    if (path.empty() || !valid_record(record) || !valid_authority(authority, record.deployment) || !valid_device_ids(observed_devices)) {
      return std::unexpected(failure_t {errc_t::ownership_unverified, operation_t::journal});
    }
    auto state {std::make_shared<state_t>()};
    state->path = path;
    state->record = record;
    state->data = journal_json(record, observed_devices);
    state->bytes = state->data.dump();
    bool vacant {false};
    const bool stored {http::update_state_file_bytes(path.string(), [&](const std::string &current) -> std::optional<std::string> {
      vacant = current.empty();
      return vacant ? std::make_optional(state->bytes) : std::nullopt;
    })};
    if (!stored || !vacant) {
      return std::unexpected(failure_t {errc_t::journal_failed, operation_t::journal});
    }
    return std::shared_ptr<ownership_journal_t> {new ownership_journal_t(std::move(state))};
  }

  std::expected<std::shared_ptr<ownership_journal_t>, failure_t> ownership_journal_t::load(const std::filesystem::path &path, const recovery_authority_t &authority) {
    if (path.empty() || !valid_authority(authority, authority.deployment)) {
      return std::unexpected(failure_t {errc_t::ownership_unverified, operation_t::journal});
    }
    auto state {std::make_shared<state_t>()};
    state->path = path;
    bool valid {false};
    const bool read {http::update_state_file_bytes(path.string(), [&](const std::string &current) -> std::optional<std::string> {
      if (current.empty() || current.size() > 24 * 1024 * 1024) {
        return std::nullopt;
      }
      try {
        state->data = nlohmann::json::parse(current);
        const auto &data {state->data};
        if (data.size() != 13 || data.at("schema") != 2 || data.at("cleanup_pending") != true || !data.at("add_armed").is_boolean() || !data.at("source_retired").is_boolean()) {
          return std::nullopt;
        }
        const auto &encoded_identity {data.at("identity")};
        if (!encoded_identity.is_array() || encoded_identity.size() != sizeof(GUID) || std::ranges::any_of(encoded_identity, [](const auto &value) {
              return !value.is_number_unsigned() || value.template get<std::uint64_t>() > 255;
            })) {
          return std::nullopt;
        }
        const auto identity {encoded_identity.get<std::vector<unsigned int>>()};
        auto destination {std::as_writable_bytes(std::span {&state->record.identity, 1})};
        std::ranges::transform(identity, destination.begin(), [](auto value) {
          return static_cast<std::byte>(value);
        });
        if (!data.at("generation").is_number_unsigned()) {
          return std::nullopt;
        }
        state->record.generation = data.at("generation").get<std::uint64_t>();
        state->record.deployment = data.at("deployment").get<std::string>();
        state->record.initially_headless = data.at("initially_headless").get<bool>();
        state->record.baseline = data.at("baseline").get<std::string>();
        const auto settings {data.at("settings").get<std::string>()};
        const auto restoration {data.at("restoration_state").get<std::string>()};
        const auto devices {data.at("recovery_ids").get<std::vector<std::string>>()};
        const auto mapping {decode_mapping(data)};
        valid = valid_record(state->record) && valid_authority(authority, state->record.deployment) && settings.size() <= 1024 * 1024 && restoration.size() <= 1024 * 1024 && (settings.empty() || settings == restoration) && valid_device_ids(devices) &&
                (!mapping || std::ranges::find(devices, mapping->device_id) != devices.end());
        if (valid) {
          state->bytes = current;
        }
      } catch (const std::exception &) {
        valid = false;
      }
      return std::nullopt;
    })};
    if (!read || !valid) {
      return std::unexpected(failure_t {errc_t::ownership_unverified, operation_t::journal});
    }
    return std::shared_ptr<ownership_journal_t> {new ownership_journal_t(std::move(state))};
  }

  const ownership_record_t &ownership_journal_t::record() const {
    return state_->record;
  }

  bool ownership_journal_t::validate(const recovery_authority_t &authority) const {
    std::lock_guard lock {state_->mutex};
    return valid_authority(authority, state_->record.deployment) && state_->unchanged();
  }

  std::optional<std::vector<std::uint8_t>> ownership_journal_t::settings() const {
    std::lock_guard lock {state_->mutex};
    if (!state_->unchanged()) {
      return std::nullopt;
    }
    const auto settings {state_->data.at("settings").get<std::string>()};
    return std::vector<std::uint8_t> {settings.begin(), settings.end()};
  }

  std::optional<std::vector<std::uint8_t>> ownership_journal_t::restoration_state() const {
    std::lock_guard lock {state_->mutex};
    if (!state_->unchanged()) {
      return std::nullopt;
    }
    const auto bytes {state_->data.at("restoration_state").get<std::string>()};
    return std::vector<std::uint8_t> {bytes.begin(), bytes.end()};
  }

  std::optional<std::vector<std::string>> ownership_journal_t::recovery_devices() const {
    std::lock_guard lock {state_->mutex};
    if (!state_->unchanged()) {
      return std::nullopt;
    }
    return state_->data.at("recovery_ids").get<std::vector<std::string>>();
  }

  bool ownership_journal_t::observe_devices(const std::vector<std::string> &device_ids) {
    if (!valid_device_ids(device_ids)) {
      return false;
    }
    std::lock_guard lock {state_->mutex};
    const auto previous {state_->data.at("recovery_ids").get<std::vector<std::string>>()};
    std::set<std::string> combined {previous.begin(), previous.end()};
    combined.insert(device_ids.begin(), device_ids.end());
    if (combined.size() > 256) {
      return false;
    }
    auto data = state_->data;
    data["recovery_ids"] = combined;
    return state_->replace(std::move(data));
  }

  bool ownership_journal_t::bind_mapping(const display_t &display, const std::string &device_id) {
    if (!IsEqualGUID(display.identity, state_->record.identity) || display.monitor_path.empty() || display.monitor_path.size() > 32768 || display.monitor_path.find(L'\0') != std::wstring::npos || !valid_device_ids({device_id})) {
      return false;
    }
    std::lock_guard lock {state_->mutex};
    if (!state_->data.at("add_armed").get<bool>() || state_->data.at("source_retired").get<bool>()) {
      return false;
    }
    const auto previous {decode_mapping(state_->data)};
    if (previous && (previous->device_id != device_id || !same_monitor_path(previous->monitor_path, display.monitor_path) || previous->target.adapter.HighPart != display.target.adapter.HighPart || previous->target.adapter.LowPart != display.target.adapter.LowPart || previous->target.target != display.target.target)) {
      return false;
    }
    auto data = state_->data;
    data["mapping"] = {{"identity", data.at("identity")}, {"generation", state_->record.generation}, {"adapter_low", display.target.adapter.LowPart}, {"adapter_high", display.target.adapter.HighPart}, {"target", display.target.target}, {"monitor_path", utf_utils::to_utf8(display.monitor_path)}, {"device_id", device_id}};
    const auto ids {data.at("recovery_ids").get<std::vector<std::string>>()};
    std::set<std::string> combined {ids.begin(), ids.end()};
    combined.insert(device_id);
    if (combined.size() > 256) {
      return false;
    }
    data["recovery_ids"] = combined;
    return state_->replace(std::move(data));
  }

  std::optional<ownership_mapping_t> ownership_journal_t::mapping() const {
    std::lock_guard lock {state_->mutex};
    return state_->unchanged() ? decode_mapping(state_->data) : std::nullopt;
  }

  bool ownership_journal_t::store_settings(const std::vector<std::uint8_t> &settings) {
    if (settings.size() > 1024 * 1024) {
      return false;
    }
    std::lock_guard lock {state_->mutex};
    auto data = state_->data;
    data["settings"] = std::string {settings.begin(), settings.end()};
    if (!settings.empty()) {
      data["restoration_state"] = data["settings"];
    }
    const auto bytes {data.dump()};
    bool matches {false};
    const bool stored {http::update_state_file_bytes(state_->path.string(), [&](const std::string &current) -> std::optional<std::string> {
      matches = !state_->bytes.empty() && current == state_->bytes;
      return matches ? std::make_optional(bytes) : std::nullopt;
    })};
    if (stored && matches) {
      state_->data = std::move(data);
      state_->bytes = bytes;
      return true;
    }
    return false;
  }

  bool ownership_journal_t::complete() {
    std::lock_guard lock {state_->mutex};
    if (!state_->data.at("source_retired").get<bool>() || !state_->data.at("settings").get<std::string>().empty()) {
      return false;
    }
    bool matches {false};
    const bool stored {http::update_state_file_bytes(state_->path.string(), [&](const std::string &current) -> std::optional<std::string> {
      matches = !state_->bytes.empty() && current == state_->bytes;
      return matches ? std::make_optional(std::string {}) : std::nullopt;
    })};
    if (stored && matches) {
      state_->bytes.clear();
      return true;
    }
    return false;
  }

  void ownership_journal_t::confirm_removal() {
    std::lock_guard lock {state_->mutex};
    state_->removal_confirmed = true;
  }

  bool ownership_journal_t::confirm_unarmed() {
    std::lock_guard lock {state_->mutex};
    state_->unarmed_confirmed = !state_->data.at("add_armed").get<bool>() && state_->unchanged();
    return state_->unarmed_confirmed;
  }

  bool ownership_journal_t::arm_add() {
    std::lock_guard lock {state_->mutex};
    if (state_->data.at("add_armed").get<bool>()) {
      return false;
    }
    auto data = state_->data;
    data["add_armed"] = true;
    const auto bytes {data.dump()};
    bool matches {false};
    const bool stored {http::update_state_file_bytes(state_->path.string(), [&](const std::string &current) -> std::optional<std::string> {
      matches = !state_->bytes.empty() && current == state_->bytes;
      return matches ? std::make_optional(bytes) : std::nullopt;
    })};
    if (stored && matches) {
      state_->data = std::move(data);
      state_->bytes = bytes;
      return true;
    }
    return false;
  }

  bool ownership_journal_t::add_armed() const {
    std::lock_guard lock {state_->mutex};
    return state_->data.at("add_armed").get<bool>() && state_->unchanged();
  }

  bool ownership_journal_t::retire(const cleanup_receipt_t &receipt) {
    std::lock_guard lock {state_->mutex};
    if (receipt.journal_.get() != this || receipt.generation_ != state_->record.generation || !IsEqualGUID(receipt.identity_, state_->record.identity) || (receipt.kind_ == cleanup_receipt_t::kind_e::removed ? !state_->removal_confirmed : (!state_->unarmed_confirmed || state_->data.at("add_armed").get<bool>()))) {
      return false;
    }
    auto data = state_->data;
    data["source_retired"] = true;
    const auto bytes {data.dump()};
    bool matches {false};
    const bool stored {http::update_state_file_bytes(state_->path.string(), [&](const std::string &current) -> std::optional<std::string> {
      matches = !state_->bytes.empty() && current == state_->bytes;
      return matches ? std::make_optional(bytes) : std::nullopt;
    })};
    if (stored && matches) {
      state_->data = std::move(data);
      state_->bytes = bytes;
      return true;
    }
    return false;
  }

  bool ownership_journal_t::source_retired() const {
    std::lock_guard lock {state_->mutex};
    return state_->data.at("source_retired").get<bool>();
  }

  bool ownership_journal_t::accepts_receipt(const cleanup_receipt_t &receipt) const {
    std::lock_guard lock {state_->mutex};
    return receipt.journal_.get() == this && receipt.generation_ == state_->record.generation &&
           IsEqualGUID(receipt.identity_, state_->record.identity) && state_->unchanged() &&
           (receipt.kind_ == cleanup_receipt_t::kind_e::removed ? state_->removal_confirmed : (state_->unarmed_confirmed && !state_->data.at("add_armed").get<bool>()));
  }

  bool ownership_journal_t::add_was_armed() const {
    return add_armed();
  }

  std::expected<std::shared_ptr<ownership_journal_t>, failure_t> ownership_journal_t::begin_next(const ownership_record_t &record, const recovery_authority_t &authority, const std::vector<std::string> &observed_devices) {
    std::lock_guard lock {state_->mutex};
    if (!state_->data.at("source_retired").get<bool>() || !valid_record(record) || record.generation <= state_->record.generation || record.deployment != state_->record.deployment || !valid_authority(authority, record.deployment) || !valid_device_ids(observed_devices)) {
      return std::unexpected(failure_t {errc_t::ownership_unverified, operation_t::journal});
    }
    auto next {std::make_shared<state_t>()};
    next->path = state_->path;
    next->record = record;
    next->data = journal_json(record, observed_devices);
    const auto previous_ids {state_->data.at("recovery_ids").get<std::vector<std::string>>()};
    std::set<std::string> retained {previous_ids.begin(), previous_ids.end()};
    retained.insert(observed_devices.begin(), observed_devices.end());
    if (retained.size() > 256) {
      return std::unexpected(failure_t {errc_t::journal_failed, operation_t::journal});
    }
    next->data["recovery_ids"] = retained;
    const auto pending {state_->data.at("settings").get<std::string>()};
    if (!pending.empty()) {
      next->data["settings"] = pending;
      next->data["restoration_state"] = state_->data.at("restoration_state");
    }
    next->bytes = next->data.dump();
    bool matches {false};
    const bool stored {http::update_state_file_bytes(state_->path.string(), [&](const std::string &current) -> std::optional<std::string> {
      matches = !state_->bytes.empty() && current == state_->bytes;
      return matches ? std::make_optional(next->bytes) : std::nullopt;
    })};
    if (!stored || !matches) {
      return std::unexpected(failure_t {errc_t::journal_failed, operation_t::journal});
    }
    // The previous object deliberately retains its old expected bytes: all old capabilities are stale.
    return std::shared_ptr<ownership_journal_t> {new ownership_journal_t(std::move(next))};
  }

  struct lease_t::state_t {
    std::unique_ptr<system_t> system;
    std::unique_ptr<heartbeat_t> heartbeat;
    std::mutex io_mutex;
    mutable std::mutex failure_mutex;
    std::optional<failure_t> heartbeat_error;
    wire::version_t version {};
    GUID identity {};
    std::optional<display_t> display;
    std::optional<wire::target_t> target;
    std::chrono::milliseconds heartbeat_period {};
    bool reserved {};
    bool opened {};
    bool owned {};
    bool removed {};
    bool absent {};
    bool closed_unarmed {};
    std::shared_ptr<ownership_journal_t> journal;

    ~state_t() {
      if (reserved) {
        active_lease = false;
      }
    }

    std::expected<void, failure_t> command(DWORD code, std::span<const std::byte> input = {}, std::span<std::byte> output = {}) {
      std::lock_guard lock {io_mutex};
      auto result = system->ioctl(code, input, output);
      if (!result) {
        return std::unexpected(result.error());
      }
      if (*result != output.size()) {
        return std::unexpected(failure_t {errc_t::response_size, operation_for(code), 0, *result});
      }
      return {};
    }

    void remember_heartbeat_failure(failure_t failure) {
      std::lock_guard lock {failure_mutex};
      if (!heartbeat_error) {
        failure.code = errc_t::heartbeat_failed;
        heartbeat_error = failure;
      }
    }

    std::expected<void, failure_t> start_heartbeat() {
      if (heartbeat || heartbeat_period.count() == 0) {
        return {};
      }
      auto result = system->heartbeat(heartbeat_period, [this] {
        if (auto ping = command(wire::ping); !ping) {
          remember_heartbeat_failure(ping.error());
        }
      });
      if (!result) {
        remember_heartbeat_failure(result.error());
        return std::unexpected(result.error());
      }
      if (!*result) {
        const failure_t failure {errc_t::heartbeat_failed, operation_t::heartbeat, ERROR_INVALID_HANDLE};
        remember_heartbeat_failure(failure);
        return std::unexpected(failure);
      }
      heartbeat = std::move(*result);
      return {};
    }
  };

  std::unique_ptr<system_t> make_win32_system() {
    return std::make_unique<win32_system_t>();
  }

  lease_t::lease_t(std::unique_ptr<state_t> state):
      state_(std::move(state)) {}

  lease_t::lease_t(lease_t &&) noexcept = default;

  lease_t::~lease_t() {
    if (state_ && state_->journal && state_->owned) {
      // A dropped managed lease is not permission to remove before display restoration.
      // Callback/handle destruction is unavoidable here; retain intent for explicit recovery.
      if (state_->heartbeat) {
        state_->heartbeat->stop();
        state_->heartbeat.reset();
      }
      if (state_->opened) {
        std::ignore = state_->system->close();
      }
      BOOST_LOG(error) << "Journal-backed SudoVDA lease abandoned before ordered cleanup; recovery remains required and watchdog liveness is no longer guaranteed.";
      return;
    }
    if (auto result = cleanup(); !result) {
      BOOST_LOG(error) << "SudoVDA display cleanup failed; explicit recovery required (category " << static_cast<int>(result.error().code) << ", Win32 " << result.error().system_error << ")";
      // Final best effort cannot promise cleanup. Stop callbacks before state/handle destruction.
      if (state_->heartbeat) {
        state_->heartbeat->stop();
        state_->heartbeat.reset();
      }
    }
  }

  acquisition_t lease_t::acquire(std::unique_ptr<system_t> system, const request_t &request) {
    return acquire_impl(std::move(system), request, nullptr);
  }

  acquisition_t lease_t::acquire_impl(std::unique_ptr<system_t> system, const request_t &request, const std::shared_ptr<ownership_journal_t> &journal) {
    auto params = checked_request(request);
    if (!params) {
      return {nullptr, params.error(), {}};
    }
    if (!system) {
      return {nullptr, failure_t {errc_t::open_failed, operation_t::open, ERROR_INVALID_PARAMETER}, {}};
    }
    auto lease = std::unique_ptr<lease_t> {new lease_t(std::make_unique<state_t>())};
    auto &state = *lease->state_;
    state.system = std::move(system);
    state.identity = request.identity;
    state.journal = journal;
    if (active_lease.exchange(true)) {
      return {nullptr, failure_t {errc_t::busy, operation_t::open}, {}};
    }
    state.reserved = true;
    const auto fail = [&](failure_t failure) -> acquisition_t {
      if (journal && state.owned) {
        // The source owner must restore/read back topology before removing an armed owned output.
        const auto heartbeat {state.start_heartbeat()};
        return {std::move(lease), failure, {}, {}, heartbeat ? std::nullopt : std::make_optional(heartbeat.error()), true};
      }
      auto cleanup = lease->cleanup();
      if (!cleanup) {
        return {std::move(lease), failure, cleanup.error()};
      }
      return {nullptr, failure, {}, lease->cleanup_receipt()};
    };
    if (auto open = state.system->open(); !open) {
      return fail(open.error());
    }
    state.opened = true;
    if (auto version = state.command(wire::version, {}, output_bytes(state.version)); !version) {
      return fail(version.error());
    }
    if (state.version.test_build > 1) {
      return fail({errc_t::malformed_response, operation_t::version});
    }
    if (state.version.major != wire::client_version.major || state.version.minor < wire::client_version.minor) {
      return fail({errc_t::incompatible_protocol, operation_t::version});
    }
    wire::watchdog_t watchdog {};
    if (auto query = state.command(wire::watchdog, {}, output_bytes(watchdog)); !query) {
      return fail(query.error());
    }
    if (watchdog.countdown > watchdog.timeout) {
      return fail({errc_t::malformed_response, operation_t::watchdog});
    }
    if (watchdog.timeout == 1) {
      // Every driver tick decrements to zero even immediately after a reset to one.
      return fail({errc_t::unsupported_watchdog, operation_t::watchdog});
    }
    if (watchdog.timeout) {
      state.heartbeat_period = std::chrono::milliseconds {std::min<std::uint64_t>(1000, std::uint64_t {watchdog.timeout} * 1000 / 3)};
    }
    auto before = state.system->enumerate();
    if (!before) {
      return fail(before.error());
    }
    if (std::ranges::any_of(before->identities, [&](const auto &identity) {
          return IsEqualGUID(identity, request.identity);
        })) {
      return fail({errc_t::already_present, operation_t::add});
    }
    // Arm only after an absent exact GUID was checked under the caller's exclusive-deployment
    // precondition. A freshly written intent for an unknown existing GUID cannot grant deletion.
    if (journal && !journal->arm_add()) {
      return fail({errc_t::journal_failed, operation_t::journal});
    }
    if (journal) {
      // Once armed, even an adapter/heartbeat failure completes exact-GUID removal before receipt.
      state.owned = true;
    }
    if (auto adapter = state.command(wire::set_adapter, input_bytes(request.render_adapter)); !adapter) {
      return fail(adapter.error());
    }
    if (auto heartbeat = state.start_heartbeat(); !heartbeat) {
      return fail(heartbeat.error());
    }
    wire::target_t target {};
    // Under the required exclusive-use contract, this absent GUID belongs to this attempt.
    // Even a failed/timed-out add may need removal; ERROR_NOT_FOUND means nothing was left.
    state.owned = true;
    if (auto add = state.command(wire::add, input_bytes(*params), output_bytes(target)); !add) {
      return fail(add.error());
    }
    state.target = target;
    for (unsigned int attempt = 0; attempt < request.enumeration_attempts; ++attempt) {
      auto enumeration = state.system->enumerate();
      if (enumeration) {
        if (journal) {
          auto output {normalized_owned_display(*enumeration, state.identity, target)};
          if (!output) {
            return fail(output.error());
          }
          if (*output) {
            state.display = std::move(**output);
            return {std::move(lease), {}, {}};
          }
        } else {
          auto output = std::ranges::find_if(enumeration->displays, [&](const auto &display) {
            return same_luid(display.target.adapter, target.adapter) && display.target.target == target.target &&
                   IsEqualGUID(display.identity, request.identity) && !display.monitor_path.empty() && !display.gdi_name.empty();
          });
          if (output != enumeration->displays.end()) {
            state.display = std::move(*output);
            return {std::move(lease), {}, {}};
          }
        }
      } else if (attempt + 1 == request.enumeration_attempts) {
        return fail(enumeration.error());
      }
      if (attempt + 1 < request.enumeration_attempts) {
        state.system->wait_for_enumeration(request.enumeration_interval);
      }
    }
    return fail({errc_t::enumeration_timeout, operation_t::enumerate});
  }

  acquisition_t lease_t::acquire_owned(std::unique_ptr<system_t> system, const request_t &request, const std::shared_ptr<ownership_journal_t> &journal, const recovery_authority_t &authority) {
    if (!journal || !journal->validate(authority) || journal->source_retired() || !request.exclusive_driver_control || !IsEqualGUID(request.identity, journal->record().identity)) {
      return {nullptr, failure_t {errc_t::ownership_unverified, operation_t::journal}, {}};
    }
    auto result {acquire_impl(std::move(system), request, journal)};
    if (result.failure && !result.lease && !result.cleanup_failure && !result.cleanup_receipt && journal->confirm_unarmed()) {
      result.cleanup_receipt = cleanup_receipt_t {journal, journal->record().identity, journal->record().generation, cleanup_receipt_t::kind_e::unarmed};
    }
    return result;
  }

  acquisition_t lease_t::recover(std::unique_ptr<system_t> system, const std::shared_ptr<ownership_journal_t> &journal, const recovery_authority_t &authority) {
    if (!system || !journal || !journal->validate(authority) || journal->source_retired() || !journal->add_armed()) {
      return {nullptr, failure_t {errc_t::ownership_unverified, operation_t::journal}, {}};
    }
    auto lease {std::unique_ptr<lease_t> {new lease_t(std::make_unique<state_t>())}};
    auto &state {*lease->state_};
    state.system = std::move(system);
    state.identity = journal->record().identity;
    state.journal = journal;
    if (active_lease.exchange(true)) {
      return {nullptr, failure_t {errc_t::busy, operation_t::open}, {}};
    }
    state.reserved = true;
    const auto fail = [&](failure_t failure) -> acquisition_t {
      // Before adoption, closing the handle is safe; after adoption the owner must restore first.
      if (state.owned) {
        const auto heartbeat {state.start_heartbeat()};
        return {std::move(lease), failure, {}, {}, heartbeat ? std::nullopt : std::make_optional(heartbeat.error()), true};
      }
      auto cleanup {lease->cleanup()};
      return cleanup ? acquisition_t {nullptr, failure, {}} : acquisition_t {std::move(lease), failure, cleanup.error()};
    };
    if (auto result {state.system->open()}; !result) {
      return fail(result.error());
    }
    state.opened = true;
    if (auto result {state.command(wire::version, {}, output_bytes(state.version))}; !result) {
      return fail(result.error());
    }
    if (state.version.test_build > 1 || state.version.major != wire::client_version.major || state.version.minor < wire::client_version.minor) {
      return fail({errc_t::incompatible_protocol, operation_t::version});
    }
    wire::watchdog_t watchdog {};
    if (auto result {state.command(wire::watchdog, {}, output_bytes(watchdog))}; !result) {
      return fail(result.error());
    }
    if (watchdog.countdown > watchdog.timeout || watchdog.timeout == 1) {
      return fail({watchdog.timeout == 1 ? errc_t::unsupported_watchdog : errc_t::malformed_response, operation_t::watchdog});
    }
    if (watchdog.timeout) {
      state.heartbeat_period = std::chrono::milliseconds {std::min<std::uint64_t>(1000, std::uint64_t {watchdog.timeout} * 1000 / 3)};
    }
    // No caller-supplied GUID reaches removal: identity came only from the validated journal.
    state.owned = true;
    if (auto result {state.start_heartbeat()}; !result) {
      return fail(result.error());
    }
    auto present {state.system->enumerate()};
    if (!present) {
      return fail(present.error());
    }
    const bool absent {std::ranges::none_of(present->identities, [&](const auto &identity) {
      return IsEqualGUID(identity, state.identity);
    })};
    auto normalized {normalized_owned_display(*present, state.identity)};
    if (!normalized) {
      return fail(normalized.error());
    }
    state.display = std::move(*normalized);
    if (!absent && !state.display) {
      return fail({errc_t::enumeration_timeout, operation_t::enumerate});
    }
    state.absent = absent;
    return {std::move(lease), {}, {}};
  }

  std::expected<void, failure_t> lease_t::cleanup() {
    if (!state_) {
      return {};
    }
    auto &state = *state_;
    if (state.owned && state.journal && (!state.journal->add_armed() || state.journal->source_retired())) {
      return std::unexpected(failure_t {errc_t::ownership_unverified, operation_t::journal});
    }
    if (state.heartbeat) {
      state.heartbeat->stop();
      state.heartbeat.reset();
    }
    if (state.owned) {
      auto remove = state.command(wire::remove, input_bytes(state.identity));
      if (!remove && remove.error().system_error != ERROR_NOT_FOUND) {
        auto failure = remove.error();
        failure.code = errc_t::remove_failed;
        (void) state.start_heartbeat();
        return std::unexpected(failure);
      }
      state.owned = false;
      state.removed = true;
      state.absent = true;
      state.display.reset();
    }
    if (state.opened) {
      if (auto close = state.system->close(); !close) {
        return std::unexpected(close.error());
      }
      state.opened = false;
    }
    if (state.reserved) {
      active_lease = false;
      state.reserved = false;
    }
    if (state.journal && state.removed) {
      state.journal->confirm_removal();
    } else if (state.journal && !state.owned && !state.opened) {
      state.closed_unarmed = state.journal->confirm_unarmed();
    }
    return {};
  }

  wire::version_t lease_t::protocol() const {
    return state_ ? state_->version : wire::version_t {};
  }

  const std::optional<display_t> &lease_t::display() const {
    static const std::optional<display_t> empty;
    return state_ ? state_->display : empty;
  }

  GUID lease_t::identity() const {
    return state_ ? state_->identity : GUID {};
  }

  bool lease_t::identity_absent() const {
    return state_ && state_->absent;
  }

  bool lease_t::belongs_to(const std::shared_ptr<ownership_journal_t> &journal) const {
    return state_ && journal && state_->journal == journal && journal->add_armed();
  }

  std::optional<cleanup_receipt_t> lease_t::cleanup_receipt() const {
    if (!state_ || !state_->journal || (!state_->removed && !state_->closed_unarmed) || state_->opened || state_->owned) {
      return std::nullopt;
    }
    return cleanup_receipt_t {state_->journal, state_->identity, state_->journal->record().generation, state_->removed ? cleanup_receipt_t::kind_e::removed : cleanup_receipt_t::kind_e::unarmed};
  }

  std::expected<void, failure_t> lease_t::refresh_display(unsigned int attempts, std::chrono::milliseconds interval) {
    if (!state_ || !state_->owned || !state_->opened || !attempts || attempts > 100 || interval < std::chrono::milliseconds::zero() || interval > std::chrono::milliseconds {100} || (state_->journal && (!state_->journal->add_armed() || state_->journal->source_retired()))) {
      return std::unexpected(failure_t {errc_t::ownership_unverified, operation_t::enumerate});
    }
    auto &state {*state_};
    state.absent = false;
    state.display.reset();
    for (unsigned int attempt {}; attempt < attempts; ++attempt) {
      auto current {state.system->enumerate()};
      if (current) {
        const bool present {std::ranges::any_of(current->identities, [&](const auto &identity) {
          return IsEqualGUID(identity, state.identity);
        })};
        auto normalized {normalized_owned_display(*current, state.identity, state.target)};
        if (!normalized) {
          return std::unexpected(normalized.error());
        }
        state.display = std::move(*normalized);
        if (!present) {
          state.absent = true;
          return {};
        }
        if (state.display) {
          return {};
        }
      } else if (attempt + 1 == attempts) {
        return std::unexpected(current.error());
      }
      if (attempt + 1 < attempts) {
        state.system->wait_for_enumeration(interval);
      }
    }
    return std::unexpected(failure_t {errc_t::enumeration_timeout, operation_t::enumerate});
  }

  std::optional<failure_t> lease_t::heartbeat_failure() const {
    if (!state_) {
      return {};
    }
    std::lock_guard lock {state_->failure_mutex};
    return state_->heartbeat_error;
  }
}  // namespace platf::sudovda
