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
#include <system_error>
#include <thread>
#include <utility>

// platform includes
#include <setupapi.h>

// local includes
#include "src/logging.h"

namespace platf::sudovda {
  namespace {
    std::atomic_bool active_lease {};

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
          result.displays.push_back({{path.targetInfo.adapterId, path.targetInfo.id}, container->second, target_name.monitorDevicePath, source_name.viewGdiDeviceName});
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

  struct lease_t::state_t {
    std::unique_ptr<system_t> system;
    std::unique_ptr<heartbeat_t> heartbeat;
    std::mutex io_mutex;
    mutable std::mutex failure_mutex;
    std::optional<failure_t> heartbeat_error;
    wire::version_t version {};
    GUID identity {};
    std::optional<display_t> display;
    std::chrono::milliseconds heartbeat_period {};
    bool reserved {};
    bool opened {};
    bool owned {};

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
    if (active_lease.exchange(true)) {
      return {nullptr, failure_t {errc_t::busy, operation_t::open}, {}};
    }
    state.reserved = true;
    const auto fail = [&](failure_t failure) -> acquisition_t {
      auto cleanup = lease->cleanup();
      if (!cleanup) {
        return {std::move(lease), failure, cleanup.error()};
      }
      return {nullptr, failure, {}};
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
    for (unsigned int attempt = 0; attempt < request.enumeration_attempts; ++attempt) {
      auto enumeration = state.system->enumerate();
      if (enumeration) {
        auto output = std::ranges::find_if(enumeration->displays, [&](const auto &display) {
          return same_luid(display.target.adapter, target.adapter) && display.target.target == target.target &&
                 IsEqualGUID(display.identity, request.identity) && !display.monitor_path.empty() && !display.gdi_name.empty();
        });
        if (output != enumeration->displays.end()) {
          state.display = std::move(*output);
          return {std::move(lease), {}, {}};
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

  std::expected<void, failure_t> lease_t::cleanup() {
    if (!state_) {
      return {};
    }
    auto &state = *state_;
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
    return {};
  }

  wire::version_t lease_t::protocol() const {
    return state_ ? state_->version : wire::version_t {};
  }

  const std::optional<display_t> &lease_t::display() const {
    static const std::optional<display_t> empty;
    return state_ ? state_->display : empty;
  }

  std::optional<failure_t> lease_t::heartbeat_failure() const {
    if (!state_) {
      return {};
    }
    std::lock_guard lock {state_->failure_mutex};
    return state_->heartbeat_error;
  }
}  // namespace platf::sudovda
