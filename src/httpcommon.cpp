/**
 * @file src/httpcommon.cpp
 * @brief Definitions for common HTTP.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <curl/curl.h>
#include <Simple-Web-Server/server_http.hpp>
#include <Simple-Web-Server/server_https.hpp>

#ifdef _WIN32
  #include <aclapi.h>
  #include <sddl.h>
  #include <Windows.h>
  #include <winioctl.h>
#else
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
  #if defined(__linux__)
    #include <sys/xattr.h>
  #elif defined(__APPLE__) || defined(__FreeBSD__)
    #include <sys/acl.h>
  #endif
#endif

// local includes
#include "config.h"
#include "crypto.h"
#include "file_handler.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "utility.h"
#include "uuid.h"

namespace http {
  using namespace std::literals;
  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  int reload_user_creds(const std::string &file);
  /**
   * @brief Check whether the Web UI credentials file exists and is readable.
   *
   * @param file Path to the credentials file.
   * @return True when the credentials file is present.
   */
  bool user_creds_exist(const std::string &file);

  std::string unique_id;  ///< Unique ID.
  net::net_e origin_web_ui_allowed;  ///< Origin web ui allowed.

  /**
   * @brief Load persisted HTTP credentials and initialize shared request state.
   */
  int init() {
    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];
    origin_web_ui_allowed = net::from_enum_string(config::nvhttp.origin_web_ui_allowed);

    if (clean_slate) {
      unique_id = uuid_util::uuid_t::generate().string();
      auto dir = std::filesystem::temp_directory_path() / "Sunshine"sv;
      config::nvhttp.cert = (dir / ("cert-"s + unique_id)).string();
      config::nvhttp.pkey = (dir / ("pkey-"s + unique_id)).string();
    }

    if ((!fs::exists(config::nvhttp.pkey) || !fs::exists(config::nvhttp.cert)) && create_creds(config::nvhttp.pkey, config::nvhttp.cert)) {
      return -1;
    }
    if (!user_creds_exist(config::sunshine.credentials_file)) {
      BOOST_LOG(info) << "Open the Web UI to set your new username and password and getting started";
    } else if (reload_user_creds(config::sunshine.credentials_file)) {
      return -1;
    }
    return 0;
  }

  namespace {
    std::unordered_set<std::string> recovery_required;  ///< Failed restorations block further writes until operator recovery.
    std::mutex state_file_mutex;  ///< Serializes credentials and pairing updates, including shared-file defaults.
#ifdef SUNSHINE_TESTS
    std::function<std::optional<std::uint32_t>(const fs::path &, const fs::path &, const fs::path &)> state_replace_hook;  ///< Deterministic native partial-failure injection.
    std::function<void(test_support::state_security_stage_e, const fs::path &)> state_security_hook;  ///< Inspect only the empty temporary at its native security gates.
    std::function<std::optional<std::uint32_t>(const fs::path &)> state_attributes_hook;  ///< Fixture-only attributes without modifying encryption state.
    std::function<void()> state_update_hook;  ///< Deterministic pause after the transaction read.
  #ifdef _WIN32
    std::function<std::optional<std::uint32_t>(test_support::state_file_stage_e, const fs::path &, const fs::path &, const fs::path &)> state_file_hook;  ///< Faults actual fixture transaction stages.
  #endif
#endif
#ifdef _WIN32
    /**
     * @brief Capture actual inherited ACEs and protection policy using the modern file security API.
     * @param handle Guarded original or temporary handle with READ_CONTROL.
     * @param information One supported authority selector or owner/group/DACL.
     * @return Self-relative owner/group/DACL descriptor, or an empty snapshot on failure.
     */
    std::vector<std::uint8_t> read_file_security(HANDLE handle, DWORD information = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION) {
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (GetSecurityInfo(handle, SE_FILE_OBJECT, information, nullptr, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS || !descriptor) {
        return {};
      }
      auto release = util::fail_guard([&]() {
        LocalFree(descriptor);
      });
      if (!IsValidSecurityDescriptor(descriptor)) {
        return {};
      }
      const auto length = GetSecurityDescriptorLength(descriptor);
      const auto *bytes = static_cast<const std::uint8_t *>(descriptor);
      return {bytes, bytes + length};
    }

    /**
     * @brief Compare valid owner/group/DACL snapshots without hiding actual authority or policy changes.
     * @param expected Original modern security descriptor.
     * @param current Modern descriptor under inspection.
     * @return Whether ordered ACEs, ownership, policy and all other descriptor controls match.
     */
    bool same_file_security(std::vector<std::uint8_t> expected, std::vector<std::uint8_t> current) {
      if (expected.size() < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || current.size() < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || !IsValidSecurityDescriptor(expected.data()) || !IsValidSecurityDescriptor(current.data()) || GetSecurityDescriptorLength(expected.data()) > expected.size() || GetSecurityDescriptorLength(current.data()) > current.size()) {
        return false;
      }
      SECURITY_DESCRIPTOR_CONTROL expected_control {}, current_control {};
      DWORD expected_revision = 0, current_revision = 0;
      if (!GetSecurityDescriptorControl(expected.data(), &expected_control, &expected_revision) || !GetSecurityDescriptorControl(current.data(), &current_control, &current_revision) || expected_revision != current_revision) {
        return false;
      }
      if (expected_control != current_control) {
        return false;
      }
      const auto same_sid = [&](auto getter) {
        PSID expected_sid = nullptr, current_sid = nullptr;
        BOOL expected_defaulted = FALSE, current_defaulted = FALSE;
        if (!getter(expected.data(), &expected_sid, &expected_defaulted) || !getter(current.data(), &current_sid, &current_defaulted) || expected_defaulted != current_defaulted) {
          return false;
        }
        if (!expected_sid || !current_sid) {
          return expected_sid == current_sid;
        }
        return IsValidSid(expected_sid) && IsValidSid(current_sid) && EqualSid(expected_sid, current_sid);
      };
      if (!same_sid(GetSecurityDescriptorOwner) || !same_sid(GetSecurityDescriptorGroup)) {
        return false;
      }
      PACL expected_acl = nullptr, current_acl = nullptr;
      BOOL expected_present = FALSE, current_present = FALSE;
      BOOL expected_defaulted = FALSE, current_defaulted = FALSE;
      if (!GetSecurityDescriptorDacl(expected.data(), &expected_present, &expected_acl, &expected_defaulted) || !GetSecurityDescriptorDacl(current.data(), &current_present, &current_acl, &current_defaulted) || expected_present != current_present || expected_defaulted != current_defaulted) {
        return false;
      }
      if (!expected_acl || !current_acl) {
        return expected_acl == current_acl;
      }
      if (!IsValidAcl(expected_acl) || !IsValidAcl(current_acl) || expected_acl->AclRevision != current_acl->AclRevision || expected_acl->AceCount != current_acl->AceCount) {
        return false;
      }
      for (DWORD index = 0; index < expected_acl->AceCount; ++index) {
        void *expected_ace = nullptr, *current_ace = nullptr;
        if (!GetAce(expected_acl, index, &expected_ace) || !GetAce(current_acl, index, &current_ace)) {
          return false;
        }
        const auto expected_size = static_cast<ACE_HEADER *>(expected_ace)->AceSize;
        if (expected_size != static_cast<ACE_HEADER *>(current_ace)->AceSize || std::memcmp(expected_ace, current_ace, expected_size)) {
          return false;
        }
      }
      return true;
    }

    /** @brief A bounded failure reason that never contains paths or state bytes. */
    struct state_file_error_t {
      const char *reason;  ///< Static diagnostic.
      DWORD native_error = ERROR_SUCCESS;  ///< Immediate error from a failed native operation, if any.
      DWORD close_error = ERROR_SUCCESS;  ///< Independently captured rename-handle close error, if any.
    };

    /** @brief Report bounded native failures without interpreting logical checks as GetLastError results. */
    void state_log_failure(const state_file_error_t &failure) {
      BOOST_LOG(error) << failure.reason;
      if (failure.native_error != ERROR_SUCCESS) {
        BOOST_LOG(error) << "State native operation error: "sv << failure.native_error;
      }
      if (failure.close_error != ERROR_SUCCESS) {
        BOOST_LOG(error) << "State rename-handle close error: "sv << failure.close_error;
      }
    }

    /** @brief Require a native transaction invariant without exposing native buffers. */
    void state_require(bool success, const char *reason) {
      if (!success) {
        throw state_file_error_t {reason};
      }
    }

    /** @brief Close transaction handles, including every precommit error path. */
    struct state_handle_t {
      HANDLE value = INVALID_HANDLE_VALUE;  ///< Solely owned native handle.

      ~state_handle_t() {
        if (value != INVALID_HANDLE_VALUE) {
          CloseHandle(value);
        }
      }

      state_handle_t() = default;

      explicit state_handle_t(HANDLE handle):
          value(handle) {}

      state_handle_t(const state_handle_t &) = delete;
      state_handle_t &operator=(const state_handle_t &) = delete;
    };

  #ifdef SUNSHINE_TESTS
    using state_stage_e = test_support::state_file_stage_e;
  #else
    /** @brief Native stages; production always performs the real operation. */
    enum class state_stage_e {
      label,  ///< Query the mandatory label.
      attributes,  ///< Query resource attributes.
      scope,  ///< Query central policy.
      metadata,  ///< Inspect supported metadata.
      backup,  ///< Create the original hard link.
      write,  ///< Write candidate bytes.
      flush,  ///< Flush candidate bytes.
      close,  ///< Close the prepared candidate.
      rename,  ///< Commit one replacing rename.
      rename_open,  ///< Open the verified source with delete access.
      rename_api,  ///< Rename through the verified source handle.
      rename_close,  ///< Report checked source-handle closure after the rename.
      verify,  ///< Verify the committed candidate.
      restore,  ///< Restore a verified original.
      cleanup  ///< Remove a known transaction-owned file.
    };
  #endif

    /** @brief Inspect a real stage or inject a deterministic fixture error. */
    bool state_stage(state_stage_e stage, const fs::path &destination, const fs::path &temporary = {}, const fs::path &backup = {}) {
  #ifdef SUNSHINE_TESTS
      if (state_file_hook) {
        if (const auto error = state_file_hook(stage, destination, temporary, backup)) {
          SetLastError(*error);
          return false;
        }
      }
  #else
      (void) stage;
      (void) destination;
      (void) temporary;
      (void) backup;
  #endif
      return true;
    }

    /** @brief Compare every selected SACL control and ordered ACE, including absent/null distinctions. */
    bool same_selected_security(std::vector<std::uint8_t> expected, std::vector<std::uint8_t> current, bool scope = false) {
      const auto valid = [](auto &value) {
        return value.size() >= sizeof(SECURITY_DESCRIPTOR_RELATIVE) && IsValidSecurityDescriptor(value.data()) && GetSecurityDescriptorLength(value.data()) <= value.size();
      };
      if (!valid(expected) || !valid(current)) {
        return false;
      }
      SECURITY_DESCRIPTOR_CONTROL left_control {}, right_control {};
      DWORD left_revision = 0, right_revision = 0;
      PACL left = nullptr, right = nullptr;
      BOOL left_present = FALSE, right_present = FALSE, left_defaulted = FALSE, right_defaulted = FALSE;
      if (!GetSecurityDescriptorControl(expected.data(), &left_control, &left_revision) || !GetSecurityDescriptorControl(current.data(), &right_control, &right_revision) || left_control != right_control || left_revision != right_revision || !GetSecurityDescriptorSacl(expected.data(), &left_present, &left, &left_defaulted) || !GetSecurityDescriptorSacl(current.data(), &right_present, &right, &right_defaulted) || left_present != right_present || left_defaulted != right_defaulted) {
        return false;
      }
      if (!left || !right) {
        return left == right;
      }
      if (!IsValidAcl(left) || !IsValidAcl(right) || left->AclRevision != right->AclRevision || left->AceCount != right->AceCount || (scope && left->AceCount)) {
        return false;
      }
      for (DWORD index = 0; index < left->AceCount; ++index) {
        void *left_ace = nullptr, *right_ace = nullptr;
        if (!GetAce(left, index, &left_ace) || !GetAce(right, index, &right_ace)) {
          return false;
        }
        const auto size = static_cast<ACE_HEADER *>(left_ace)->AceSize;
        if (size != static_cast<ACE_HEADER *>(right_ace)->AceSize || std::memcmp(left_ace, right_ace, size)) {
          return false;
        }
      }
      return true;
    }

    /** @brief Captured authority, supported basic metadata and unnamed bytes of one guarded file. */
    struct state_snapshot_t {
      BY_HANDLE_FILE_INFORMATION identity {};  ///< Volume and file ID; link count may change.
      FILE_BASIC_INFO basic {};  ///< Preserve creation time and ordinary attributes.
      std::vector<std::uint8_t> security;  ///< Exact owner/group/DACL and controls.
      std::array<std::vector<std::uint8_t>, 3> selected;  ///< Separate label, resource attribute and CAP snapshots.
      std::string bytes;  ///< Captured bytes used for parsing and verification.
    };

    constexpr std::array<DWORD, 3> state_selectors {LABEL_SECURITY_INFORMATION, ATTRIBUTE_SECURITY_INFORMATION, SCOPE_SECURITY_INFORMATION};  ///< Selectors readable with READ_CONTROL.
    constexpr DWORD state_attributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY;  ///< Supported ordinary attributes.
    std::unordered_set<std::string> retained_state_files;  ///< Known secured material requiring later cleanup.
    std::unordered_set<std::string> blocked_state_ids;  ///< Block known recovery objects even through another configured hard link or case alias.

    /** @brief Identify a recovery object only within the current process and qualified volume. */
    std::string state_id_key(const BY_HANDLE_FILE_INFORMATION &value) {
      return std::to_string(value.dwVolumeSerialNumber) + ":" + std::to_string(value.nFileIndexHigh) + ":" + std::to_string(value.nFileIndexLow);
    }

    /** @brief Read the unnamed stream from the same handle used to capture identity and policy. */
    std::string state_read_bytes(HANDLE handle) {
      LARGE_INTEGER size {}, beginning {};
      state_require(GetFileSizeEx(handle, &size) && size.QuadPart >= 0 && static_cast<unsigned long long>(size.QuadPart) <= (std::numeric_limits<std::size_t>::max)() && SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN), "State bytes could not be captured");
      std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        DWORD count = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, MAXDWORD));
        state_require(ReadFile(handle, bytes.data() + offset, chunk, &count, nullptr) && count, "State bytes could not be read completely");
        offset += count;
      }
      return bytes;
    }

    /** @brief Refuse exceptional backup streams without requesting or assuming access to audit SACLs. */
    void state_stream_inventory(HANDLE handle) {
      void *context = nullptr;
      auto abort = util::fail_guard([&]() {
        DWORD ignored = 0;
        BackupRead(handle, nullptr, 0, &ignored, TRUE, FALSE, &context);
      });
      std::array<BYTE, 4096> buffer {};
      std::size_t position = 0, available = 0;
      const auto read = [&](void *output, std::size_t size, bool end_allowed = false) {
        std::size_t copied = 0;
        while (copied < size) {
          if (position == available) {
            DWORD count = 0;
            state_require(BackupRead(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, FALSE, FALSE, &context), "State metadata inventory could not be read");
            position = 0;
            available = count;
            if (!count && end_allowed && !copied) {
              return false;
            }
            state_require(count != 0, "State metadata inventory was truncated");
          }
          const auto count = (std::min) (size - copied, available - position);
          std::memcpy(static_cast<BYTE *>(output) + copied, buffer.data() + position, count);
          position += count;
          copied += count;
        }
        return true;
      };
      const auto skip = [&](unsigned long long size) {
        const auto buffered = std::min<unsigned long long>(size, available - position);
        position += static_cast<std::size_t>(buffered);
        size -= buffered;
        if (size) {
          DWORD low = 0, high = 0;
          state_require(BackupSeek(handle, static_cast<DWORD>(size), static_cast<DWORD>(size >> 32), &low, &high, &context) && ((static_cast<unsigned long long>(high) << 32) | low) == size, "State metadata inventory could not be skipped completely");
        }
      };
      for (;;) {
        WIN32_STREAM_ID stream {};
        if (!read(&stream, offsetof(WIN32_STREAM_ID, cStreamName), true)) {
          break;
        }
        state_require(stream.Size.QuadPart >= 0, "State metadata has an invalid stream length");
        state_require(stream.dwStreamId != BACKUP_ALTERNATE_DATA, "State update refused: named streams cannot be preserved");
        state_require(stream.dwStreamId != BACKUP_EA_DATA, "State update refused: extended attributes cannot be preserved");
        state_require(stream.dwStreamId != BACKUP_OBJECT_ID, "State update refused: an object ID cannot be preserved");
        // Link topology/file IDs are outside the replacement contract. A hard-link backup adds no authority.
        state_require(stream.dwStreamId == BACKUP_DATA || stream.dwStreamId == BACKUP_LINK, "State update refused: unsupported backup metadata");
        skip(stream.dwStreamNameSize);
        skip(static_cast<unsigned long long>(stream.Size.QuadPart));
      }
    }

    /** @brief Limit this rename/backup implementation to supported local NTFS regular files. */
    void state_provider(HANDLE handle) {
      std::array<wchar_t, 32> filesystem {};
      DWORD flags = 0;
      state_require(GetFileType(handle) == FILE_TYPE_DISK && GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, &flags, filesystem.data(), static_cast<DWORD>(filesystem.size())) && std::wstring_view {filesystem.data()} == L"NTFS" && (flags & FILE_SUPPORTS_HARD_LINKS), "State update refused: a local NTFS provider with hard-link support is required");
      const auto size = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      state_require(size != 0, "State provider could not be identified");
      std::vector<wchar_t> name(static_cast<std::size_t>(size) + 1);
      const auto written = GetFinalPathNameByHandleW(handle, name.data(), static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      state_require(written && written < name.size() && !std::wstring_view {name.data()}.starts_with(L"\\\\?\\UNC\\"), "State update refused: remote providers are not qualified");
    }

    /** @brief Capture complete supported policy/metadata and refuse unsupported authority before bytes. */
    state_snapshot_t state_capture(HANDLE handle, const fs::path &path) {
      state_snapshot_t result;
      state_require(state_stage(state_stage_e::metadata, path), "State metadata inspection was refused");
      state_require(GetFileInformationByHandle(handle, &result.identity) && GetFileInformationByHandleEx(handle, FileBasicInfo, &result.basic, sizeof(result.basic)), "State file metadata could not be captured");
      DWORD attributes = result.basic.FileAttributes;
  #ifdef SUNSHINE_TESTS
      if (state_attributes_hook) {
        attributes = state_attributes_hook(path).value_or(attributes);
      }
  #endif
      state_require(attributes != INVALID_FILE_ATTRIBUTES, "State file attributes are unavailable");
      state_require(!(attributes & FILE_ATTRIBUTE_ENCRYPTED), "State update refused: EFS cannot be preserved before payload");
      state_require(!(attributes & FILE_ATTRIBUTE_COMPRESSED), "State update refused: compression cannot be preserved");
      state_require(!(attributes & FILE_ATTRIBUTE_SPARSE_FILE), "State update refused: sparse allocation cannot be preserved");
      state_require(!(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)), "State update refused: a regular non-reparse file is required");
      state_require(!(attributes & FILE_ATTRIBUTE_READONLY), "State update refused: the destination is read-only");
      state_require(!(attributes & ~state_attributes), "State update refused: unsupported file attributes");
      state_provider(handle);
      state_stream_inventory(handle);
      result.security = read_file_security(handle);
      state_require(!result.security.empty(), "State owner/group/DACL could not be captured");
      constexpr std::array<state_stage_e, 3> stages {state_stage_e::label, state_stage_e::attributes, state_stage_e::scope};
      constexpr std::array<const char *, 3> reasons {"State mandatory label could not be queried", "State resource attributes could not be queried", "State central access policy could not be queried"};
      for (std::size_t index = 0; index < state_selectors.size(); ++index) {
        state_require(state_stage(stages[index], path), reasons[index]);
        result.selected[index] = read_file_security(handle, state_selectors[index]);
        state_require(!result.selected[index].empty(), reasons[index]);
        state_require(same_selected_security(result.selected[index], result.selected[index], index == 2), index == 2 ? "State update refused: nonempty central access policy" : "State selected authority could not be represented exactly");
      }
      result.bytes = state_read_bytes(handle);
      return result;
    }

    /** @brief Compare identities without treating a new hard-link count as a different file. */
    bool state_same_id(const BY_HANDLE_FILE_INFORMATION &left, const BY_HANDLE_FILE_INFORMATION &right) {
      return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber && left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow;
    }

    /** @brief Restrict native creation-time tolerance to a final rename into genuine initial absence. */
    enum class state_creation_time_e {
      exact,  ///< All precommit, existing-original and recovery snapshots require equality.
      initial_commit  ///< A final first-save name may acquire the removed name's tunneled creation time.
    };

    /**
     * @brief Compare every supported security field and the metadata promised at this boundary.
     * @param left Expected immutable snapshot.
     * @param right Independently captured snapshot.
     * @param creation_time Exact unless verifying the final initial-absence commit.
     * @return Whether identity, payload, authority and required metadata match.
     */
    bool state_same(const state_snapshot_t &left, const state_snapshot_t &right, state_creation_time_e creation_time = state_creation_time_e::exact) {
      if (!state_same_id(left.identity, right.identity) || (creation_time != state_creation_time_e::initial_commit && left.basic.CreationTime.QuadPart != right.basic.CreationTime.QuadPart) || left.basic.FileAttributes != right.basic.FileAttributes || left.bytes != right.bytes || !same_file_security(left.security, right.security)) {
        return false;
      }
      for (std::size_t index = 0; index < state_selectors.size(); ++index) {
        if (!same_selected_security(left.selected[index], right.selected[index], index == 2)) {
          return false;
        }
      }
      return true;
    }

    /** @brief Open only the path currently named, refusing reparse traversal. */
    HANDLE state_open(const fs::path &path, DWORD access = GENERIC_READ) {
      return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    }

    /** @brief Describe a rejected committed snapshot without exposing file identity, contents or descriptors. */
    void state_log_commit_mismatch(const state_snapshot_t &expected, const state_snapshot_t &current, state_creation_time_e creation_time) {
      BOOST_LOG(error) << "State committed comparison: identity="sv << state_same_id(expected.identity, current.identity)
                       << "; volume="sv << (expected.identity.dwVolumeSerialNumber == current.identity.dwVolumeSerialNumber)
                       << "; file_id="sv << (expected.identity.nFileIndexHigh == current.identity.nFileIndexHigh && expected.identity.nFileIndexLow == current.identity.nFileIndexLow)
                       << "; bytes="sv << (expected.bytes == current.bytes)
                       << "; creation_time="sv << (expected.basic.CreationTime.QuadPart == current.basic.CreationTime.QuadPart)
                       << "; creation_time_required="sv << (creation_time != state_creation_time_e::initial_commit)
                       << "; creation_expected/current="sv << expected.basic.CreationTime.QuadPart << '/' << current.basic.CreationTime.QuadPart
                       << "; attributes_expected/current="sv << expected.basic.FileAttributes << '/' << current.basic.FileAttributes;
      const auto describe_policy = [](const char *component, auto left, auto right, bool matches) {
        SECURITY_DESCRIPTOR_CONTROL left_control {}, right_control {};
        DWORD left_revision = 0, right_revision = 0;
        const bool left_read = !left.empty() && GetSecurityDescriptorControl(left.data(), &left_control, &left_revision);
        const bool right_read = !right.empty() && GetSecurityDescriptorControl(right.data(), &right_control, &right_revision);
        BOOST_LOG(error) << "State committed "sv << component << " comparison: matches="sv << matches
                         << "; controls_read="sv << left_read << '/' << right_read
                         << "; control_expected/current="sv << left_control << '/' << right_control
                         << "; revision_expected/current="sv << left_revision << '/' << right_revision;
      };
      describe_policy("owner/group/DACL", expected.security, current.security, same_file_security(expected.security, current.security));
      constexpr std::array<const char *, 3> components {"label", "resource attributes", "central policy"};
      for (std::size_t index = 0; index < state_selectors.size(); ++index) {
        describe_policy(components[index], expected.selected[index], current.selected[index], same_selected_security(expected.selected[index], current.selected[index], index == 2));
      }
    }

    /**
     * @brief Check an existing path against the identity/policy/bytes of a known object.
     * @param path Path under inspection.
     * @param expected Captured identity, policy and bytes.
     * @param diagnose_commit Whether a failed final comparison emits bounded component diagnostics.
     * @param creation_time Exact unless verifying the final initial-absence commit.
     * @return Whether the complete snapshot matches; inspection failures also reject it.
     */
    bool state_path_matches(const fs::path &path, const state_snapshot_t &expected, bool diagnose_commit = false, state_creation_time_e creation_time = state_creation_time_e::exact) {
      try {
        state_handle_t handle {state_open(path)};
        if (handle.value == INVALID_HANDLE_VALUE) {
          if (diagnose_commit) {
            state_log_failure({"State committed path could not be opened", GetLastError()});
          }
          return false;
        }
        const auto current = state_capture(handle.value, path);
        const bool matches = state_same(expected, current, creation_time);
        if (!matches && diagnose_commit) {
          state_log_commit_mismatch(expected, current, creation_time);
        }
        return matches;
      } catch (const state_file_error_t &failure) {
        if (diagnose_commit) {
          BOOST_LOG(error) << "State committed snapshot capture failed"sv;
          state_log_failure(failure);
        }
        return false;
      } catch (...) {
        if (diagnose_commit) {
          BOOST_LOG(error) << "State committed snapshot could not be inspected"sv;
        }
        return false;
      }
    }

    /** @brief Identify a path without acquiring data access or following a reparse point. */
    bool state_path_id(const fs::path &path, const BY_HANDLE_FILE_INFORMATION &expected) {
      state_handle_t handle {CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
      BY_HANDLE_FILE_INFORMATION current {};
      return handle.value != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle.value, &current) && !(current.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) && state_same_id(expected, current);
    }

    /** @brief Observe genuine absence rather than treating an inaccessible path as missing. */
    bool state_missing(const fs::path &path) {
      if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return false;
      }
      return GetLastError() == ERROR_FILE_NOT_FOUND;
    }

    /**
     * @brief Rename a verified source while retaining the original's read/delete guard.
     * @param source Temporary or backup directory entry to rename.
     * @param destination Absolute same-volume target pathname.
     * @param backup Backup entry supplied to fixture stages.
     * @param expected Complete captured source identity, bytes and supported policy.
     * @param replace Whether the destination is a known object to replace.
     * @param destination_matches Recheck the known destination after fixture callbacks.
     */
    void state_rename(const fs::path &source, const fs::path &destination, const fs::path &backup, const state_snapshot_t &expected, bool replace, const std::function<bool()> &destination_matches) {
  #if defined(FILE_RENAME_FLAG_REPLACE_IF_EXISTS) && defined(FILE_RENAME_FLAG_POSIX_SEMANTICS) && NTDDI_VERSION >= 0x0A000002
      const auto target = fs::absolute(destination).wstring();
      constexpr auto offset = offsetof(FILE_RENAME_INFO, FileName);
      state_require(target.size() <= (MAXDWORD - offset - sizeof(wchar_t)) / sizeof(wchar_t), "State rename target exceeds the native buffer limit");
      const auto name_size = target.size() * sizeof(wchar_t);
      const auto buffer_size = std::max(sizeof(FILE_RENAME_INFO), offset + name_size + sizeof(wchar_t));
      static_assert(alignof(FILE_RENAME_INFO) <= alignof(std::max_align_t));
      auto *rename = static_cast<FILE_RENAME_INFO *>(std::calloc(1, buffer_size));
      state_require(rename != nullptr, "State aligned rename buffer could not be allocated");
      auto free_rename = util::fail_guard([&]() {
        std::free(rename);
      });
      rename->Flags = replace ? FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS : 0;
      rename->FileNameLength = static_cast<DWORD>(name_size);
      std::memcpy(rename->FileName, target.data(), name_size);
      if (!state_stage(state_stage_e::rename_open, destination, source, backup)) {
        throw state_file_error_t {"State rename-source open was refused", GetLastError()};
      }
      state_handle_t handle {state_open(source, GENERIC_READ | DELETE)};
      if (handle.value == INVALID_HANDLE_VALUE) {
        throw state_file_error_t {"State rename source could not be opened", GetLastError()};
      }
      state_require(state_same(expected, state_capture(handle.value, source)) && state_path_id(source, expected.identity), "State opened rename source changed identity, bytes or policy");
      BOOL renamed = FALSE;
      DWORD rename_error = ERROR_SUCCESS;
      if (state_stage(state_stage_e::rename_api, destination, source, backup)) {
        state_require(state_same(expected, state_capture(handle.value, source)) && state_path_id(source, expected.identity) && destination_matches(), "State source or destination changed at the native rename boundary");
        renamed = SetFileInformationByHandle(handle.value, FileRenameInfoEx, rename, static_cast<DWORD>(buffer_size));
        if (!renamed) {
          rename_error = GetLastError();
        }
      } else {
        rename_error = GetLastError();
      }
      const auto closing = std::exchange(handle.value, INVALID_HANDLE_VALUE);
      const auto closed = CloseHandle(closing);
      bool close_succeeded = closed;
      DWORD close_error = closed ? ERROR_SUCCESS : GetLastError();
      // Fault the reported close outcome only after actually closing the fixture handle.
      if (!state_stage(state_stage_e::rename_close, destination, source, backup)) {
        const auto injected_close_error = GetLastError();
        close_succeeded = false;
        if (close_error == ERROR_SUCCESS) {
          close_error = injected_close_error;
        }
      }
      if (!renamed || !close_succeeded) {
        throw state_file_error_t {renamed ? "State rename source could not be closed" : "State handle-bound rename failed", rename_error, close_error};
      }
  #else
      (void) source;
      (void) destination;
      (void) backup;
      (void) expected;
      (void) replace;
      (void) destination_matches;
      state_require(false, "State update requires extended handle-based rename support");
  #endif
    }

    /** @brief Apply creation time and supported ordinary attributes without changing access policy. */
    void state_basic_metadata(HANDLE handle, const FILE_BASIC_INFO &expected) {
      FILE_BASIC_INFO apply {};
      apply.CreationTime = expected.CreationTime;
      apply.FileAttributes = expected.FileAttributes;
      state_require(SetFileInformationByHandle(handle, FileBasicInfo, &apply, sizeof(apply)), "State creation time or ordinary attributes could not be preserved");
    }

    /** @brief Restore all supported access policy while the replacement is still empty. */
    void state_restore_empty(const fs::path &temporary, HANDLE handle, const state_snapshot_t &original) {
  #ifdef SUNSHINE_TESTS
      if (state_security_hook) {
        state_security_hook(test_support::state_security_stage_e::restore, temporary);
      }
  #endif
      SECURITY_DESCRIPTOR_CONTROL control {};
      DWORD revision = 0;
      auto restoration = original.security;
      state_require(GetSecurityDescriptorControl(restoration.data(), &control, &revision) && SetSecurityDescriptorControl(restoration.data(), SE_DACL_AUTO_INHERIT_REQ, (control & SE_DACL_AUTO_INHERITED) ? SE_DACL_AUTO_INHERIT_REQ : 0), "State empty-file inheritance input could not be prepared");
      const auto protection = (control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
      state_require(SetFileSecurityW(temporary.c_str(), DACL_SECURITY_INFORMATION | protection, restoration.data()), "State empty-file owner/group/DACL policy could not be restored");
      for (std::size_t index = 0; index < 2; ++index) {
        auto current = read_file_security(handle, state_selectors[index]);
        state_require(!current.empty(), "State temporary authority selector could not be queried");
        if (!same_selected_security(original.selected[index], current)) {
          auto selected = original.selected[index];
          PACL acl = nullptr;
          BOOL present = FALSE, defaulted = FALSE;
          state_require(GetSecurityDescriptorSacl(selected.data(), &present, &acl, &defaulted), "State selected policy could not be decoded");
          auto name = temporary.wstring();
          state_require(SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, state_selectors[index], nullptr, nullptr, nullptr, acl) == ERROR_SUCCESS, "State mandatory label or resource attributes could not be preserved while empty");
        }
      }
      state_basic_metadata(handle, original.basic);
  #ifdef SUNSHINE_TESTS
      if (state_security_hook) {
        state_security_hook(test_support::state_security_stage_e::verify, temporary);
      }
  #endif
    }

    /**
     * @brief Execute one guarded JSON replacement with a verified same-file backup and explicit recovery.
     * @param destination Configured state pathname.
     * @param temporary Unique same-directory replacement.
     * @param backup Unique same-directory original hard link.
     * @param identity Process-local recovery key.
     * @param update Mutation of the captured JSON bytes.
     * @return True only after a verified commit; cleanup failure does not negate that commit.
     */
    bool update_windows_state(const fs::path &destination, const fs::path &temporary, const fs::path &backup, const std::string &identity, const std::function<void(pt::ptree &)> &update) {
      state_handle_t original_handle;
      state_handle_t temporary_handle;
      std::optional<state_snapshot_t> original, prepared;
      std::optional<BY_HANDLE_FILE_INFORMATION> temporary_id;
      bool backup_owned = false, attempted = false, retain = false, verified_commit = false, initial_captured = false, temporary_created = false;
      const auto block = [&]() {
        retain = true;
        recovery_required.insert(identity);
        if (original) {
          blocked_state_ids.insert(state_id_key(original->identity));
        }
        if (prepared) {
          blocked_state_ids.insert(state_id_key(prepared->identity));
        }
        retained_state_files.insert(backup.string());
        retained_state_files.insert(temporary.string());
        BOOST_LOG(error) << "State outcome is uncertain; secured recovery material was retained and further updates are blocked"sv;
        return false;
      };
      const auto cleanup_file = [&](const fs::path &path, const std::optional<BY_HANDLE_FILE_INFORMATION> &expected) {
        if (state_missing(path)) {
          return;
        }
        if (!expected || !state_path_id(path, *expected) || !state_stage(state_stage_e::cleanup, destination, path, backup) || !DeleteFileW(path.c_str())) {
          retained_state_files.insert(path.string());
          BOOST_LOG(warning) << (verified_commit ? "State commit verified; secured transaction cleanup remains pending"sv : "State update failed; transaction cleanup remains pending"sv);
        }
      };
      auto cleanup = util::fail_guard([&]() {
        if (!retain) {
          if (temporary_id) {
            cleanup_file(temporary, temporary_id);
          } else if (temporary_created) {
            retained_state_files.insert(temporary.string());
            BOOST_LOG(warning) << "State temporary identity is unavailable; cleanup requires operator inspection"sv;
          }
          if (backup_owned && original) {
            cleanup_file(backup, original->identity);
          }
        }
      });
      const auto recover = [&]() {
        if (!attempted) {
          if ((original && !state_path_matches(destination, *original)) || (initial_captured && !original && !state_missing(destination))) {
            return block();
          }
          return false;
        }
        if (original && state_path_matches(destination, *original)) {
          BOOST_LOG(warning) << "State replacement failed; original bytes and policy remain verified"sv;
          return false;
        }
        const bool missing = state_missing(destination);
        const bool known_candidate = prepared && state_path_id(destination, prepared->identity);
        if (!missing && !known_candidate) {
          return block();
        }
        if (original) {
          if (!backup_owned || !state_path_matches(backup, *original) || !state_same(*original, state_capture(original_handle.value, backup)) || !state_stage(state_stage_e::restore, destination, temporary, backup) || !(missing ? state_missing(destination) : state_path_id(destination, prepared->identity)) || !state_path_matches(backup, *original)) {
            return block();
          }
          state_rename(backup, destination, backup, *original, !missing, [&]() {
            return (missing ? state_missing(destination) : state_path_id(destination, prepared->identity)) && state_same(*original, state_capture(original_handle.value, backup));
          });
          if (!state_path_matches(destination, *original)) {
            return block();
          }
        } else if (!missing && (!DeleteFileW(destination.c_str()) || !state_missing(destination))) {
          return block();
        }
        BOOST_LOG(warning) << "State replacement was rejected; the original state was restored and verified"sv;
        return false;
      };
      try {
        original_handle.value = state_open(destination, GENERIC_READ | DELETE);
        const auto original_error = original_handle.value == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (original_handle.value != INVALID_HANDLE_VALUE) {
          BY_HANDLE_FILE_INFORMATION guarded_id {};
          state_require(GetFileInformationByHandle(original_handle.value, &guarded_id) && !blocked_state_ids.contains(state_id_key(guarded_id)), "State recovery is required before updating this file through any known alias");
          original = state_capture(original_handle.value, destination);
        } else {
          if (original_error != ERROR_FILE_NOT_FOUND) {
            throw state_file_error_t {"State original could not be guarded against ordinary writers", original_error};
          }
        }
        initial_captured = true;
        pt::ptree tree;
        if (original) {
          std::istringstream input {original->bytes};
          pt::read_json(input, tree);
        }
        update(tree);
        std::ostringstream serialized;
        pt::write_json(serialized, tree);
        const auto bytes = serialized.str();
        if (original) {
          state_require(state_stage(state_stage_e::backup, destination, temporary, backup) && CreateHardLinkW(backup.c_str(), destination.c_str(), nullptr), "State original hard-link backup could not be created");
          backup_owned = true;
          state_require(state_path_matches(backup, *original) && state_path_matches(destination, *original), "State hard-link backup identity, bytes or policy could not be verified");
        }
        auto creation = original ? original->security : std::vector<std::uint8_t> {};
        PSECURITY_DESCRIPTOR initial = nullptr;
        auto free_initial = util::fail_guard([&]() {
          if (initial) {
            LocalFree(initial);
          }
        });
        if (original) {
          state_require(SetSecurityDescriptorControl(creation.data(), SE_DACL_PROTECTED, SE_DACL_PROTECTED), "State protected creation policy could not be prepared");
        } else {
          state_require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)", SDDL_REVISION_1, &initial, nullptr), "State protected creation policy could not be prepared");
        }
        const auto parent_attributes = GetFileAttributesW(fs::absolute(destination).parent_path().c_str());
        state_require(parent_attributes != INVALID_FILE_ATTRIBUTES && !(parent_attributes & FILE_ATTRIBUTE_ENCRYPTED), "State update refused: encrypted or unreadable temporary parent");
        SECURITY_ATTRIBUTES attributes {sizeof(SECURITY_ATTRIBUTES), original ? creation.data() : initial, FALSE};
        temporary_handle.value = CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        state_require(temporary_handle.value != INVALID_HANDLE_VALUE, "State protected empty temporary could not be created");
        temporary_created = true;
        BY_HANDLE_FILE_INFORMATION created_id {};
        state_require(GetFileInformationByHandle(temporary_handle.value, &created_id), "State temporary identity could not be captured");
        temporary_id = created_id;
        auto expected = state_capture(temporary_handle.value, temporary);
        state_require(expected.bytes.empty(), "State temporary was not empty before policy restoration");
        if (original) {
          state_require(created_id.dwVolumeSerialNumber == original->identity.dwVolumeSerialNumber, "State temporary and backup must share the original volume");
          state_restore_empty(temporary, temporary_handle.value, *original);
          expected = *original;
          expected.identity = created_id;
          expected.bytes.clear();
        }
        state_require(state_same(expected, state_capture(temporary_handle.value, temporary)), "State empty temporary policy could not be preserved exactly");
  #ifdef SUNSHINE_TESTS
        if (state_update_hook) {
          state_update_hook();
        }
  #endif
        std::size_t written = 0;
        while (written < bytes.size()) {
          DWORD count = 0;
          const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - written, MAXDWORD));
          state_require(state_stage(state_stage_e::write, destination, temporary, backup) && WriteFile(temporary_handle.value, bytes.data() + written, chunk, &count, nullptr) && count, "State candidate bytes could not be written completely");
          written += count;
        }
        // Content writes may set the archive attribute. Restore only supported basic metadata, never ACLs.
        state_basic_metadata(temporary_handle.value, expected.basic);
        state_require(state_stage(state_stage_e::flush, destination, temporary, backup) && FlushFileBuffers(temporary_handle.value), "State candidate could not be flushed");
        state_require(state_stage(state_stage_e::close, destination, temporary, backup) && CloseHandle(temporary_handle.value), "State candidate handle could not be closed");
        temporary_handle.value = INVALID_HANDLE_VALUE;
        expected.bytes = bytes;
        prepared = expected;
        state_require(state_path_matches(temporary, *prepared), "State closed temporary bytes or metadata failed verification");
        if (original) {
          state_require(state_same(*original, state_capture(original_handle.value, destination)) && state_path_matches(destination, *original) && state_path_matches(backup, *original), "State original or backup changed before commit");
        } else {
          state_require(state_missing(destination), "State destination appeared before initial commit");
        }
        attempted = true;
  #ifdef SUNSHINE_TESTS
        if (state_replace_hook) {
          state_require(!state_replace_hook(destination, temporary, backup), "State replacing rename was faulted");
        }
  #endif
        state_require(state_stage(state_stage_e::rename, destination, temporary, backup), "State replacing rename was refused");
        state_require((original ? state_path_matches(destination, *original) && state_path_id(destination, original->identity) : state_missing(destination)) && state_path_matches(temporary, *prepared), "State identity or policy changed at the commit boundary");
        state_rename(temporary, destination, backup, *prepared, original.has_value(), [&]() {
          return original ? state_path_matches(destination, *original) && state_path_matches(backup, *original) && state_same(*original, state_capture(original_handle.value, destination)) : state_missing(destination);
        });
        // Only this post-rename initial-absence boundary permits native filename tunneling of creation time.
        const auto committed_creation_time = original ? state_creation_time_e::exact : state_creation_time_e::initial_commit;
        state_require(state_stage(state_stage_e::verify, destination, temporary, backup) && state_path_matches(destination, *prepared, true, committed_creation_time), "State committed identity, bytes or policy failed verification");
        verified_commit = true;
        return true;
      } catch (const state_file_error_t &failure) {
        state_log_failure(failure);
      } catch (const std::exception &) {
        BOOST_LOG(error) << "State transaction could not be completed"sv;
      }
      try {
        return recover();
      } catch (const state_file_error_t &failure) {
        state_log_failure(failure);
        return block();
      } catch (...) {
        return block();
      }
    }
#endif
  }  // namespace

#ifndef _WIN32
  /**
   * @brief Refuse replacing an access ACL that mode/owner/group copying cannot preserve.
   * @param path Original file path, used when fd is negative.
   * @param fd Open temporary descriptor, or negative for the original file.
   * @return Whether an extended ACL exists or its absence cannot be established safely.
   */
  static bool unpreserved_access_acl(const fs::path &path, int fd = -1) {
  #if defined(__linux__)
    const auto size = fd < 0 ? ::getxattr(path.c_str(), "system.posix_acl_access", nullptr, 0) :
                               ::fgetxattr(fd, "system.posix_acl_access", nullptr, 0);
    if (size >= 0) {
      return size > 0;
    }
    return errno != ENODATA && errno != ENOTSUP && errno != EOPNOTSUPP;
  #elif defined(__APPLE__)
    errno = 0;
    acl_t acl = fd < 0 ? ::acl_get_file(path.c_str(), ACL_TYPE_EXTENDED) : ::acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
    if (!acl) {
      return errno != ENOENT && errno != ENOTSUP;
    }
    auto cleanup = util::fail_guard([&]() {
      ::acl_free(acl);
    });
    acl_entry_t entry;
    errno = 0;
    // Darwin returns zero for an entry and -1/EINVAL for an empty, retrieved ACL.
    return ::acl_get_entry(acl, ACL_FIRST_ENTRY, &entry) == 0 || errno != EINVAL;
  #elif defined(__FreeBSD__)
    const auto supports = [&](int name) {
      errno = 0;
      return fd < 0 ? ::pathconf(path.c_str(), name) : ::fpathconf(fd, name);
    };
    const auto nfs4 = supports(_PC_ACL_NFS4);
    if (nfs4 < 0 && errno != EINVAL) {
      return true;
    }
    acl_type_t type = ACL_TYPE_NFS4;
    if (nfs4 <= 0) {
      const auto posix = supports(_PC_ACL_EXTENDED);
      if (posix < 0 && errno != EINVAL) {
        return true;
      }
      if (posix <= 0) {
        return false;
      }
      type = ACL_TYPE_ACCESS;
    }
    acl_t acl = fd < 0 ? ::acl_get_file(path.c_str(), type) : ::acl_get_fd_np(fd, type);
    if (!acl) {
      return true;
    }
    auto cleanup = util::fail_guard([&]() {
      ::acl_free(acl);
    });
    int trivial = 0;
    return ::acl_is_trivial_np(acl, &trivial) != 0 || !trivial;
  #else
    return true;
  #endif
  }
#endif

  bool update_state_file(const std::string &file, const std::function<void(pt::ptree &)> &update) {
    std::lock_guard lock {state_file_mutex};
    const fs::path destination {file};
    std::error_code identity_error;
    const auto identity = fs::absolute(destination, identity_error).lexically_normal().string();
    if (identity_error) {
      return false;
    }
    if (recovery_required.contains(identity)) {
      BOOST_LOG(error) << "State recovery is required before further updates"sv;
      return false;
    }
    const auto suffix = uuid_util::uuid_t::generate().string();
    const auto temporary = fs::path {destination.string() + ".tmp-" + suffix};
    const auto backup = fs::path {destination.string() + ".backup-" + suffix};
#ifdef _WIN32
    return update_windows_state(destination, temporary, backup, identity, update);
#else
    bool preserve_recovery_files = false;
    auto cleanup = util::fail_guard([&]() {
      if (!preserve_recovery_files) {
        std::error_code error;
        fs::remove(temporary, error);
        fs::remove(backup, error);
      }
    });
    try {
      pt::ptree tree;
      const bool existed = fs::exists(destination);
      if (existed) {
        pt::read_json(destination.string(), tree);
      }
      update(tree);
      std::ostringstream serialized;
      pt::write_json(serialized, tree);
      const auto bytes = serialized.str();
      int handle = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
      if (handle < 0) {
        return false;
      }
      auto close = util::fail_guard([&]() {
        if (handle >= 0) {
          ::close(handle);
        }
      });
      if (existed) {
        if (unpreserved_access_acl(destination)) {
          BOOST_LOG(error) << "State update refused because its access ACL cannot be preserved"sv;
          return false;
        }
        struct stat metadata {};
        if (::stat(destination.c_str(), &metadata) || ::fchown(handle, metadata.st_uid, metadata.st_gid) || ::fchmod(handle, metadata.st_mode & 07777)) {
          return false;
        }
      }
      if (unpreserved_access_acl(temporary, handle)) {
        BOOST_LOG(error) << "State update refused because the temporary inherited an unsupported access ACL"sv;
        return false;
      }
  #ifdef SUNSHINE_TESTS
      if (state_update_hook) {
        state_update_hook();
      }
  #endif
      std::size_t written = 0;
      while (written < bytes.size()) {
        const auto count = ::write(handle, bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          return false;
        }
        written += count;
      }
      if (::fsync(handle)) {
        return false;
      }
      ::close(handle);
      handle = -1;
      fs::rename(temporary, destination);
      return true;
    } catch (const std::exception &) {
      BOOST_LOG(error) << "Could not persist state; no authorization update was published"sv;
      return false;
    }
#endif
  }

#ifdef SUNSHINE_TESTS
  namespace test_support {
    void set_state_update_hook(std::function<void()> hook) {
      std::lock_guard lock {state_file_mutex};
      state_update_hook = std::move(hook);
    }

    void set_state_security_hook(std::function<void(state_security_stage_e, const fs::path &)> hook) {
      std::lock_guard lock {state_file_mutex};
      state_security_hook = std::move(hook);
    }

    void set_state_attributes_hook(std::function<std::optional<std::uint32_t>(const fs::path &)> hook) {
      std::lock_guard lock {state_file_mutex};
      state_attributes_hook = std::move(hook);
    }

  #ifdef _WIN32
    void set_state_file_hook(std::function<std::optional<std::uint32_t>(state_file_stage_e, const fs::path &, const fs::path &, const fs::path &)> hook) {
      std::lock_guard lock {state_file_mutex};
      state_file_hook = std::move(hook);
    }

    bool state_file_security_matches(std::vector<std::uint8_t> expected, std::vector<std::uint8_t> current) {
      return same_file_security(std::move(expected), std::move(current));
    }

    bool state_selected_security_matches(std::vector<std::uint8_t> expected, std::vector<std::uint8_t> current, bool scope) {
      return same_selected_security(std::move(expected), std::move(current), scope);
    }
  #endif

    void set_state_replace_hook(std::function<std::optional<std::uint32_t>(const fs::path &, const fs::path &, const fs::path &)> hook) {
      std::lock_guard lock {state_file_mutex};
      state_replace_hook = std::move(hook);
    }
  }  // namespace test_support
#endif

  /**
   * @brief Save user creds.
   *
   * @param file Credentials file path.
   * @param username Username to save.
   * @param password Password to save.
   * @param run_our_mouth Whether to log user-facing status messages.
   * @return 0 on success, non-zero on failure.
   */
  int save_user_creds(const std::string &file, const std::string &username, const std::string &password, bool run_our_mouth) {
    const auto salt = crypto::rand_alphabet(16);
    const auto hash = util::hex(crypto::hash(password + salt)).to_string();
    if (!update_state_file(file, [&](auto &tree) {
          tree.put("username", username);
          tree.put("salt", salt);
          tree.put("password", hash);
        })) {
      return -1;
    }

    BOOST_LOG(info) << "New credentials have been created"sv;
    return 0;
  }

  /**
   * @brief Check whether the Web UI credentials file exists and is readable.
   */
  bool user_creds_exist(const std::string &file) {
    if (!fs::exists(file)) {
      return false;
    }

    pt::ptree inputTree;
    try {
      pt::read_json(file, inputTree);
      return inputTree.find("username") != inputTree.not_found() &&
             inputTree.find("password") != inputTree.not_found() &&
             inputTree.find("salt") != inputTree.not_found();
    } catch (std::exception &e) {
      BOOST_LOG(error) << "validating user credentials: "sv << e.what();
    }

    return false;
  }

  /**
   * @brief Reload the Web UI credentials from disk.
   */
  int reload_user_creds(const std::string &file) {
    pt::ptree inputTree;
    try {
      pt::read_json(file, inputTree);
      config::sunshine.username = inputTree.get<std::string>("username");
      config::sunshine.password = inputTree.get<std::string>("password");
      config::sunshine.salt = inputTree.get<std::string>("salt");
    } catch (std::exception &e) {
      BOOST_LOG(error) << "loading user credentials: "sv << e.what();
      return -1;
    }
    return 0;
  }

  /**
   * @brief Generate HTTPS credential files from the provided key and certificate paths.
   */
  int create_creds(const std::string &pkey, const std::string &cert) {
    fs::path pkey_path = pkey;
    fs::path cert_path = cert;

    auto creds = crypto::gen_creds("Sunshine Gamestream Host"sv, 2048);

    auto pkey_dir = pkey_path;
    auto cert_dir = cert_path;
    pkey_dir.remove_filename();
    cert_dir.remove_filename();

    std::error_code err_code {};
    fs::create_directories(pkey_dir, err_code);
    if (err_code) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << pkey_dir << "] :"sv << err_code.message();
      return -1;
    }

    fs::create_directories(cert_dir, err_code);
    if (err_code) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << cert_dir << "] :"sv << err_code.message();
      return -1;
    }

    if (file_handler::write_file(pkey.c_str(), creds.pkey)) {
      BOOST_LOG(error) << "Couldn't open ["sv << config::nvhttp.pkey << ']';
      return -1;
    }

    if (file_handler::write_file(cert.c_str(), creds.x509)) {
      BOOST_LOG(error) << "Couldn't open ["sv << config::nvhttp.cert << ']';
      return -1;
    }

    fs::permissions(pkey_path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, err_code);

    if (err_code) {
      BOOST_LOG(error) << "Couldn't change permissions of ["sv << config::nvhttp.pkey << "] :"sv << err_code.message();
      return -1;
    }

    fs::permissions(cert_path, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read | fs::perms::owner_write, fs::perm_options::replace, err_code);

    if (err_code) {
      BOOST_LOG(error) << "Couldn't change permissions of ["sv << config::nvhttp.cert << "] :"sv << err_code.message();
      return -1;
    }

    return 0;
  }

  /**
   * @brief Send a static file response for a Web UI request.
   */
  bool download_file(const std::string &url, const std::string &file, long ssl_version) {
    CURL *curl = curl_easy_init();
    if (!curl) {
      BOOST_LOG(error) << "Couldn't create CURL instance";
      return false;
    }

    if (std::string file_dir = file_handler::get_parent_directory(file); !file_handler::make_directory(file_dir)) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << file_dir << ']';
      curl_easy_cleanup(curl);
      return false;
    }

    FILE *fp = fopen(file.c_str(), "wb");
    if (!fp) {
      BOOST_LOG(error) << "Couldn't open ["sv << file << ']';
      curl_easy_cleanup(curl);
      return false;
    }

    curl_easy_setopt(curl, CURLOPT_SSLVERSION, ssl_version);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
      BOOST_LOG(error) << "Couldn't download ["sv << url << ", code:" << result << ']';
    }

    curl_easy_cleanup(curl);
    fclose(fp);
    return result == CURLE_OK;
  }

  /**
   * @brief Percent-encode URL data for use in HTTP query strings.
   */
  std::string url_escape(const std::string &url) {
    char *string = curl_easy_escape(nullptr, url.c_str(), static_cast<int>(url.length()));
    std::string result(string);
    curl_free(string);
    return result;
  }

  /**
   * @brief Extract the host component from a URL string.
   */
  std::string url_get_host(const std::string &url) {
    CURLU *curlu = curl_url();
    curl_url_set(curlu, CURLUPART_URL, url.c_str(), static_cast<unsigned int>(url.length()));
    char *host;
    if (curl_url_get(curlu, CURLUPART_HOST, &host, 0) != CURLUE_OK) {
      curl_url_cleanup(curlu);
      return "";
    }
    std::string result(host);
    curl_free(host);
    curl_url_cleanup(curlu);
    return result;
  }
}  // namespace http
