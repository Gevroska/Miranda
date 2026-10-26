/**
 * @file src/platform/windows/source_startup.cpp
 * @brief Checked startup reads without initializing a display settings manager.
 */
// header include
#include "source_startup.h"

// standard includes
#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// lib includes
#include <display_device/windows/json.h>
#include <nlohmann/json.hpp>

// local includes
#include "utf_utils.h"

namespace platf::source_startup {
  namespace {
    constexpr std::size_t owned_limit {24 * 1024 * 1024};
    constexpr std::size_t settings_limit {1024 * 1024};

    struct handle_t {
      HANDLE value {INVALID_HANDLE_VALUE};

      explicit handle_t(HANDLE value):
          value {value} {}

      handle_t(handle_t &&other) noexcept:
          value {std::exchange(other.value, INVALID_HANDLE_VALUE)} {}

      handle_t(const handle_t &) = delete;
      handle_t &operator=(const handle_t &) = delete;

      ~handle_t() {
        if (value != INVALID_HANDLE_VALUE) {
          CloseHandle(value);
        }
      }
    };

    struct file_t {
      store_state_e access {store_state_e::unreadable};
      std::string bytes;
    };

    struct storage_t {
      file_t owned;
      file_t ordinary;
      bool residue {};
      std::vector<handle_t> handles;  ///< Retain checked objects through classification and safe gate release.
    };

    bool same_path(const std::wstring &left, const std::wstring &right) {
      return left.size() == right.size() && CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
    }

    bool safe_absolute_path(const std::filesystem::path &path) {
      const auto text = path.native();
      if (text.size() < 4 || text.size() > 32760 || !path.is_absolute() || path != path.lexically_normal() || path.root_name().native().size() != 2 || text[1] != L':' || text[2] != L'\\' || text.find(L'\0') != std::wstring::npos || text.find_first_of(L"/*?\"") != std::wstring::npos || text.find(L':', 2) != std::wstring::npos || GetDriveTypeW(path.root_path().c_str()) != DRIVE_FIXED) {
        return false;
      }
      for (const auto &part : path.relative_path()) {
        const auto component = part.native();
        if (component.empty() || component.back() == L'.' || component.back() == L' ') {
          return false;
        }
      }
      return true;
    }

    bool checked_handle(HANDLE handle, const std::filesystem::path &path, bool directory) {
      BY_HANDLE_FILE_INFORMATION information {};
      if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &information) || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || static_cast<bool>(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory || (!directory && information.nNumberOfLinks != 1)) {
        return false;
      }
      std::array<wchar_t, 32768> final_path {};
      const auto length = GetFinalPathNameByHandleW(handle, final_path.data(), static_cast<DWORD>(final_path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      if (!length || length >= final_path.size()) {
        return false;
      }
      return same_path(std::wstring {final_path.data(), length}, L"\\\\?\\" + path.native());
    }

    store_state_e open_ancestors(const std::filesystem::path &path, storage_t &storage) {
      auto current = path.root_path();
      const auto open = [&](const std::filesystem::path &directory) {
        handle_t handle {CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (handle.value == INVALID_HANDLE_VALUE) {
          return store_state_e::unreadable;
        }
        if (!checked_handle(handle.value, directory, true)) {
          return store_state_e::unsafe_path;
        }
        // Deny write/delete sharing to fence in-place reparse conversion as well as replacement.
        storage.handles.push_back(std::move(handle));
        return store_state_e::completed_empty;
      };
      if (const auto access = open(current); access != store_state_e::completed_empty) {
        return access;
      }
      for (const auto &part : path.parent_path().relative_path()) {
        current /= part;
        if (const auto access = open(current); access != store_state_e::completed_empty) {
          return access;
        }
      }
      return store_state_e::completed_empty;
    }

    file_t read_file(const std::filesystem::path &path, std::size_t limit, storage_t &storage) {
      if (!safe_absolute_path(path)) {
        return {store_state_e::unsafe_path, {}};
      }
      if (const auto access = open_ancestors(path, storage); access != store_state_e::completed_empty) {
        return {access, {}};
      }
      handle_t file {CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
      if (file.value == INVALID_HANDLE_VALUE) {
        return {GetLastError() == ERROR_FILE_NOT_FOUND ? store_state_e::absent : store_state_e::unreadable, {}};
      }
      if (!checked_handle(file.value, path, false)) {
        return {store_state_e::unsafe_path, {}};
      }
      BY_HANDLE_FILE_INFORMATION before {};
      if (!GetFileInformationByHandle(file.value, &before)) {
        return {store_state_e::unreadable, {}};
      }
      if (before.nFileSizeHigh || before.nFileSizeLow > limit) {
        return {store_state_e::malformed, {}};
      }
      std::string bytes(before.nFileSizeLow, '\0');
      DWORD read {};
      if (!bytes.empty() && (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())) {
        return {store_state_e::unreadable, {}};
      }
      char extra {};
      BY_HANDLE_FILE_INFORMATION after {};
      if (!ReadFile(file.value, &extra, 1, &read, nullptr) || read || !GetFileInformationByHandle(file.value, &after) || before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow || before.ftLastWriteTime.dwLowDateTime != after.ftLastWriteTime.dwLowDateTime || before.ftLastWriteTime.dwHighDateTime != after.ftLastWriteTime.dwHighDateTime) {
        return {store_state_e::unreadable, {}};
      }
      storage.handles.push_back(std::move(file));
      return {store_state_e::completed_empty, std::move(bytes)};
    }

    std::optional<bool> transaction_residue(const std::filesystem::path &path) {
      const auto temporary = path.filename().native() + L".tmp-";
      const auto backup = path.filename().native() + L".backup-";
      WIN32_FIND_DATAW data {};
      const auto search = FindFirstFileW((path.parent_path() / (path.filename().native() + L".*")).c_str(), &data);
      if (search == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? std::make_optional(false) : std::nullopt;
      }
      bool residue {};
      bool complete {};
      for (std::size_t count = 0; count < 4096; ++count) {
        const std::wstring name {data.cFileName};
        if ((name.size() >= temporary.size() && same_path(name.substr(0, temporary.size()), temporary)) || (name.size() >= backup.size() && same_path(name.substr(0, backup.size()), backup))) {
          residue = true;
          break;
        }
        if (!FindNextFileW(search, &data)) {
          complete = GetLastError() == ERROR_NO_MORE_FILES;
          break;
        }
      }
      const bool closed = FindClose(search);
      return closed && (residue || complete) ? std::make_optional(residue) : std::nullopt;
    }

    storage_t read_storage(const paths_t &paths) {
      storage_t result;
      if (same_path(paths.owned_journal.native(), paths.ordinary_state.native())) {
        result.owned.access = result.ordinary.access = store_state_e::unsafe_path;
        return result;
      }
      result.owned = read_file(paths.owned_journal, owned_limit, result);
      result.ordinary = read_file(paths.ordinary_state, settings_limit, result);
      if (result.owned.access == store_state_e::absent || result.owned.access == store_state_e::completed_empty) {
        const auto residue = transaction_residue(paths.owned_journal);
        if (!residue) {
          result.owned.access = store_state_e::unreadable;
        } else {
          result.residue = *residue;
        }
      }
      // Existing leaves are pinned against writing/replacement. Recheck missing leaves so a file
      // appearing while the other store was inspected is uncertainty, never a clear snapshot.
      if (result.owned.access == store_state_e::absent && read_file(paths.owned_journal, owned_limit, result).access != store_state_e::absent) {
        result.owned.access = store_state_e::unreadable;
      }
      if (result.ordinary.access == store_state_e::absent && read_file(paths.ordinary_state, settings_limit, result).access != store_state_e::absent) {
        result.ordinary.access = store_state_e::unreadable;
      }
      return result;
    }

    nlohmann::json checked_json(const std::string &bytes) {
      std::vector<std::set<std::string>> keys;
      return nlohmann::json::parse(bytes, [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json &value) {
        if (depth > 64) {
          throw std::invalid_argument {"Startup state exceeds the supported nesting limit"};
        }
        if (event == nlohmann::json::parse_event_t::object_start) {
          keys.emplace_back();
        } else if (event == nlohmann::json::parse_event_t::object_end) {
          keys.pop_back();
        } else if (event == nlohmann::json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second) {
          throw std::invalid_argument {"Duplicate startup state field"};
        }
        return true;
      });
    }

    bool valid_id(const std::string &id) {
      return !id.empty() && id.size() <= 1024 && id.find('\0') == std::string::npos;
    }

    bool library_state(const std::string &bytes, display_device::SingleDisplayConfigState &state) {
      return bytes.size() <= settings_limit && checked_json(bytes).is_object() && display_device::fromJson(bytes, state);
    }

    std::optional<std::uint64_t> owned_generation(const std::string &bytes) {
      if (bytes.size() > owned_limit) {
        return std::nullopt;
      }
      const auto data = checked_json(bytes);
      if (!data.is_object() || data.size() != 13 || data.at("schema") != 2 || data.at("cleanup_pending") != true || !data.at("add_armed").is_boolean() || !data.at("source_retired").is_boolean() || !data.at("generation").is_number_unsigned() || !data.at("generation").get<std::uint64_t>() || !data.at("initially_headless").is_boolean()) {
        return std::nullopt;
      }
      const auto &identity = data.at("identity");
      if (!identity.is_array() || identity.size() != sizeof(GUID) || std::ranges::any_of(identity, [](const auto &byte) {
            return !byte.is_number_unsigned() || byte.template get<std::uint64_t>() > 255;
          }) ||
          std::ranges::all_of(identity, [](const auto &byte) {
            return byte == 0;
          })) {
        return std::nullopt;
      }
      const auto deployment = data.at("deployment").get<std::string>();
      const auto baseline = data.at("baseline").get<std::string>();
      const auto settings = data.at("settings").get<std::string>();
      const auto restoration = data.at("restoration_state").get<std::string>();
      const auto ids = data.at("recovery_ids").get<std::vector<std::string>>();
      if (deployment.empty() || deployment.size() > 128 || deployment.find('\0') != std::string::npos || baseline.size() > settings_limit || settings.size() > settings_limit || restoration.size() > settings_limit || data.at("initially_headless").get<bool>() != baseline.empty() || (!settings.empty() && settings != restoration) || ids.size() > 256 || !std::ranges::all_of(ids, valid_id)) {
        return std::nullopt;
      }
      display_device::SingleDisplayConfigState state;
      for (const auto *encoded : {&baseline, &settings, &restoration}) {
        if (!encoded->empty() && !library_state(*encoded, state)) {
          return std::nullopt;
        }
      }
      const auto &mapping = data.at("mapping");
      if (!mapping.is_null()) {
        if (!mapping.is_object() || mapping.size() != 7 || !mapping.at("generation").is_number_unsigned() || mapping.at("generation") != data.at("generation") || mapping.at("identity") != identity || !mapping.at("adapter_low").is_number_unsigned() || mapping.at("adapter_low").get<std::uint64_t>() > std::numeric_limits<ULONG>::max() || !mapping.at("adapter_high").is_number_integer() || (mapping.at("adapter_high").is_number_unsigned() && mapping.at("adapter_high").get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<LONG>::max())) || mapping.at("adapter_high").get<std::int64_t>() < std::numeric_limits<LONG>::min() || mapping.at("adapter_high").get<std::int64_t>() > std::numeric_limits<LONG>::max() || !mapping.at("target").is_number_unsigned() || mapping.at("target").get<std::uint64_t>() > std::numeric_limits<UINT>::max()) {
          return std::nullopt;
        }
        const auto monitor_path = utf_utils::from_utf8(mapping.at("monitor_path").get<std::string>());
        const auto id = mapping.at("device_id").get<std::string>();
        if (monitor_path.empty() || monitor_path.size() > 32768 || monitor_path.find(L'\0') != std::wstring::npos || !valid_id(id) || std::ranges::find(ids, id) == ids.end()) {
          return std::nullopt;
        }
      }
      return data.at("generation").get<std::uint64_t>();
    }

    bool clear(store_state_e value) {
      return value == store_state_e::absent || value == store_state_e::completed_empty;
    }

    status_t classify(const storage_t &storage) {
      status_t result {.owned = storage.owned.access, .ordinary = storage.ordinary.access, .disposition = disposition_e::blocked, .exclusion_held = true};
      if (storage.owned.access == store_state_e::completed_empty && !storage.owned.bytes.empty()) {
        try {
          const auto generation = owned_generation(storage.owned.bytes);
          result.owned = generation ? store_state_e::pending : store_state_e::malformed;
          result.owned_generation = generation.value_or(0);
        } catch (const std::exception &) {
          result.owned = store_state_e::malformed;
        }
      }
      if (storage.ordinary.access == store_state_e::completed_empty && !storage.ordinary.bytes.empty()) {
        display_device::SingleDisplayConfigState state;
        try {
          if (library_state(storage.ordinary.bytes, state)) {
            result.ordinary = store_state_e::pending;
            result.ordinary_recovery = std::move(state);
          } else {
            result.ordinary = store_state_e::malformed;
          }
        } catch (const std::exception &) {
          result.ordinary = store_state_e::malformed;
        }
      }
      if (storage.residue) {
        result.owned = store_state_e::transaction_residue;
      }
      if (clear(result.owned) && clear(result.ordinary)) {
        result.disposition = disposition_e::ordinary_clear;
      } else if (clear(result.owned) && result.ordinary == store_state_e::pending) {
        result.disposition = disposition_e::ordinary_recovery;
      } else if (result.owned == store_state_e::pending && clear(result.ordinary)) {
        result.disposition = disposition_e::owned_requires_authority;
      } else if (result.owned == store_state_e::pending && result.ordinary == store_state_e::pending) {
        result.disposition = disposition_e::conflicting_recovery;
      }
      return result;
    }
  }  // namespace

  struct coordinator_t::state_t {
    explicit state_t(paths_t paths):
        paths {std::move(paths)} {}

    paths_t paths;  ///< Immutable input locations; not authority evidence.
    std::mutex operation;  ///< Serialize explicit inspection and release on the operation lane.
    mutable std::mutex publication;  ///< Protect copied status only.
    status_t published;
    bool recovery_proof_required {};  ///< A missing/empty destination cannot discharge previously observed uncertainty or owned intent.
    std::shared_ptr<display_device::ordinary_startup_exclusion_t> exclusion;  ///< Shared handoff retains ordinary exclusion across B failures.
    std::optional<storage_t> inspection;  ///< Guards live only through the explicit inspection/release operation.
#ifdef SUNSHINE_TESTS
    std::function<test_storage_t()> test_read;
#endif
  };

  coordinator_t::coordinator_t(paths_t paths):
      state_ {std::make_unique<state_t>(std::move(paths))} {}

  coordinator_t::~coordinator_t() = default;

  status_t coordinator_t::status() const {
    std::lock_guard lock {state_->publication};
    return state_->published;
  }

  status_t coordinator_t::inspect_locked() {
    status_t result {.disposition = disposition_e::blocked};
    try {
      if (!state_->exclusion) {
        state_->exclusion = display_device::ordinary_startup_exclusion_t::acquire();
      }
      if (state_->exclusion) {
#ifdef SUNSHINE_TESTS
        if (state_->test_read) {
          const auto input = state_->test_read();
          state_->inspection.emplace();
          state_->inspection->owned = {input.owned.access, input.owned.bytes};
          state_->inspection->ordinary = {input.ordinary.access, input.ordinary.bytes};
          state_->inspection->residue = input.transaction_residue;
        } else
#endif
        {
          state_->inspection = read_storage(state_->paths);
        }
        result = classify(*state_->inspection);
        state_->recovery_proof_required = state_->recovery_proof_required || !clear(result.owned) ||
                                          (!clear(result.ordinary) && result.ordinary != store_state_e::pending);
      } else {
        result.exclusion_held = display_device::ordinary_startup_recovery().mutation_excluded;
      }
    } catch (const std::exception &) {
      result.owned = result.ordinary = store_state_e::unreadable;
      result.exclusion_held = display_device::ordinary_startup_recovery().mutation_excluded;
      state_->recovery_proof_required = true;
    }
    result.recovery_proof_required = state_->recovery_proof_required;
    if (result.recovery_proof_required && (result.disposition == disposition_e::ordinary_clear || result.disposition == disposition_e::ordinary_recovery)) {
      result.disposition = disposition_e::recovery_proof_required;
    }
    std::lock_guard lock {state_->publication};
    state_->published = result;
    return result;
  }

  status_t coordinator_t::inspect() {
    std::lock_guard operation {state_->operation};
    const auto result = inspect_locked();
    state_->inspection.reset();
    return result;
  }

  bool coordinator_t::release_for_ordinary() {
    std::lock_guard operation {state_->operation};
    auto result = inspect_locked();
    const bool allowed = result.disposition == disposition_e::ordinary_clear || result.disposition == disposition_e::ordinary_recovery;
    const bool released = allowed && state_->exclusion && state_->exclusion->release_to_ordinary(result.ordinary == store_state_e::pending, state_->paths.ordinary_state);
    if (released) {
      result.exclusion_held = false;
      result.ordinary_released = true;
      state_->exclusion.reset();
      std::lock_guard lock {state_->publication};
      state_->published = std::move(result);
    }
    state_->inspection.reset();
    return released;
  }

#ifdef SUNSHINE_TESTS
  std::unique_ptr<coordinator_t> coordinator_t::test_make(paths_t paths, std::function<test_storage_t()> read) {
    auto result = std::make_unique<coordinator_t>(std::move(paths));
    result->state_->test_read = std::move(read);
    return result;
  }
#endif
}  // namespace platf::source_startup
