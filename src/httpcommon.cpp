/**
 * @file src/httpcommon.cpp
 * @brief Definitions for common HTTP.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
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
#include "app_catalog.h"
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

    /**
     * @brief Read complete binary bytes and explicitly check native read and close errors before preparation.
     */
    bool read_state_bytes(const fs::path &path, std::string &bytes) {
#ifdef _WIN32
      auto *input = _wfopen(path.c_str(), L"rb");
#else
      auto *input = std::fopen(path.c_str(), "rb");
#endif
      if (!input) {
        return false;
      }
      auto close = util::fail_guard([&]() {
        if (input) {
          std::fclose(input);
        }
      });
      std::array<char, 16384> buffer;
      while (true) {
        const auto count = std::fread(buffer.data(), 1, buffer.size(), input);
        if (std::ferror(input)) {
          return false;
        }
        bytes.append(buffer.data(), count);
        if (std::feof(input)) {
          break;
        }
        if (!count) {
          return false;
        }
      }
      const auto result = std::fclose(input);
      input = nullptr;
      return result == 0;
    }
#ifdef SUNSHINE_TESTS
    std::function<std::optional<std::uint32_t>(const fs::path &, const fs::path &, const fs::path &)> state_replace_hook;  ///< Deterministic native partial-failure injection.
    std::function<void(test_support::state_security_stage_e, const fs::path &)> state_security_hook;  ///< Inspect only the empty temporary at its native security gates.
    std::function<std::optional<std::uint32_t>(const fs::path &)> state_attributes_hook;  ///< Fixture-only attributes without modifying encryption state.
    std::function<void()> state_update_hook;  ///< Deterministic pause after the transaction read.
#endif
#ifdef _WIN32
    /**
     * @brief Capture actual inherited ACEs and protection policy using the modern file security API.
     * @param path Existing state file or empty temporary.
     * @return Self-relative owner/group/DACL descriptor, or an empty snapshot on failure.
     */
    std::vector<std::uint8_t> read_file_security(const fs::path &path) {
      constexpr DWORD information = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
      auto name = path.wstring();
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (GetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, information, nullptr, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS || !descriptor) {
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
     * @param replacement Allow only a final Windows-managed auto-inheritance marker addition.
     * @return Whether ordered ACEs, ownership, policy and all other descriptor controls match.
     */
    bool same_file_security(std::vector<std::uint8_t> expected, std::vector<std::uint8_t> current, bool replacement = false) {
      if (expected.size() < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || current.size() < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || !IsValidSecurityDescriptor(expected.data()) || !IsValidSecurityDescriptor(current.data()) || GetSecurityDescriptorLength(expected.data()) > expected.size() || GetSecurityDescriptorLength(current.data()) > current.size()) {
        return false;
      }
      SECURITY_DESCRIPTOR_CONTROL expected_control {}, current_control {};
      DWORD expected_revision = 0, current_revision = 0;
      if (!GetSecurityDescriptorControl(expected.data(), &expected_control, &expected_revision) || !GetSecurityDescriptorControl(current.data(), &current_control, &current_revision) || expected_revision != current_revision) {
        return false;
      }
      const auto changed = expected_control ^ current_control;
      // ReplaceFileW may set this current-inheritance-model marker without changing protection or any ACE.
      const bool marker_added = replacement && changed == SE_DACL_AUTO_INHERITED &&
                                !(expected_control & SE_DACL_AUTO_INHERITED) && (current_control & SE_DACL_AUTO_INHERITED);
      if (changed && !marker_added) {
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
    return update_state_file_bytes(file, [&](const std::string &original) -> std::optional<std::string> {
      pt::ptree tree;
      if (!original.empty() || fs::exists(file)) {
        std::istringstream input {original};
        pt::read_json(input, tree);
      }
      update(tree);
      std::ostringstream serialized;
      pt::write_json(serialized, tree);
      return serialized.str();
    });
  }

  bool update_state_file_bytes(const std::string &file, const std::function<std::optional<std::string>(const std::string &)> &prepare) {
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
    bool preserve_recovery_files = false;
    auto cleanup = util::fail_guard([&]() {
      if (!preserve_recovery_files) {
        std::error_code error;
        fs::remove(temporary, error);
        fs::remove(backup, error);
      }
    });
    try {
      std::string original;
      const bool existed = fs::exists(destination);
      if (existed) {
#ifdef _WIN32
        auto file_attributes = GetFileAttributesW(destination.c_str());
  #ifdef SUNSHINE_TESTS
        if (state_attributes_hook) {
          file_attributes = state_attributes_hook(destination).value_or(file_attributes);
        }
  #endif
        if (file_attributes == INVALID_FILE_ATTRIBUTES || (file_attributes & FILE_ATTRIBUTE_ENCRYPTED)) {
          BOOST_LOG(error) << "State update refused because file attributes are unavailable or EFS encryption cannot be preserved before writing"sv;
          return false;
        }
#endif
        if (!read_state_bytes(destination, original)) {
          return false;
        }
      }
      const auto replacement = prepare(original);
      if (!replacement) {
        return true;
      }
      const auto &bytes = *replacement;
#ifdef _WIN32
      std::vector<std::uint8_t> existing_security;
      std::vector<std::uint8_t> creation_security;
      PSECURITY_DESCRIPTOR new_security = nullptr;
      auto free_security = util::fail_guard([&]() {
        if (new_security) {
          LocalFree(new_security);
        }
      });
      SECURITY_ATTRIBUTES attributes {sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
      if (existed) {
        existing_security = read_file_security(destination);
        if (existing_security.empty()) {
          return false;
        }
        creation_security = existing_security;
        // Protect creation from parent grants, retaining an immutable original for exact policy restoration.
        if (!SetSecurityDescriptorControl(creation_security.data(), SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
          return false;
        }
        attributes.lpSecurityDescriptor = creation_security.data();
      } else {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)", SDDL_REVISION_1, &new_security, nullptr)) {
          return false;
        }
        attributes.lpSecurityDescriptor = new_security;
      }
      HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        return false;
      }
      auto close = util::fail_guard([&]() {
        if (handle != INVALID_HANDLE_VALUE) {
          CloseHandle(handle);
        }
      });
      if (existed) {
  #ifdef SUNSHINE_TESTS
        if (state_security_hook) {
          state_security_hook(test_support::state_security_stage_e::restore, temporary);
        }
  #endif
        SECURITY_DESCRIPTOR_CONTROL control {};
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(existing_security.data(), &control, &revision)) {
          return false;
        }
        const auto protection = (control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
        auto restoration_security = existing_security;
        // Request the captured inheritance model as setter input; keep the readback snapshot immutable.
        if (!SetSecurityDescriptorControl(restoration_security.data(), SE_DACL_AUTO_INHERIT_REQ, (control & SE_DACL_AUTO_INHERITED) ? SE_DACL_AUTO_INHERIT_REQ : 0)) {
          return false;
        }
        if (!SetFileSecurityW(temporary.c_str(), DACL_SECURITY_INFORMATION | protection, restoration_security.data())) {
          BOOST_LOG(error) << "State update refused because the empty temporary's security policy could not be restored"sv;
          return false;
        }
  #ifdef SUNSHINE_TESTS
        if (state_security_hook) {
          state_security_hook(test_support::state_security_stage_e::verify, temporary);
        }
  #endif
        if (!same_file_security(existing_security, read_file_security(temporary))) {
          BOOST_LOG(error) << "State update refused because the empty temporary's security policy could not be preserved exactly"sv;
          return false;
        }
      }
#else
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
#endif
#ifndef _WIN32
      if (unpreserved_access_acl(temporary, handle)) {
        BOOST_LOG(error) << "State update refused because the temporary inherited an unsupported access ACL"sv;
        return false;
      }
#endif
#ifdef SUNSHINE_TESTS
      if (state_update_hook) {
        state_update_hook();
      }
#endif
      // Detect external edits observed before writing. Arbitrary external writers must still cooperate.
      std::string latest;
      if (fs::exists(destination) != existed || (existed && (!read_state_bytes(destination, latest) || latest != original))) {
        return false;
      }
      std::size_t written = 0;
      while (written < bytes.size()) {
#ifdef _WIN32
        DWORD count = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - written, MAXDWORD));
        if (!WriteFile(handle, bytes.data() + written, chunk, &count, nullptr) || !count) {
          return false;
        }
#else
        const auto count = ::write(handle, bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          return false;
        }
#endif
        written += count;
      }
#ifdef _WIN32
      if (!FlushFileBuffers(handle)) {
        return false;
      }
      CloseHandle(handle);
      handle = INVALID_HANDLE_VALUE;
      if (existed) {
        // A backup makes the documented partial ReplaceFile failures recoverable without publishing a grant.
        std::optional<std::uint32_t> injected;
  #ifdef SUNSHINE_TESTS
        if (state_replace_hook) {
          injected = state_replace_hook(destination, temporary, backup);
        }
  #endif
        const bool replaced = !injected && ReplaceFileW(destination.c_str(), temporary.c_str(), backup.c_str(), 0, nullptr, nullptr);
        if (!replaced) {
          const auto replacement_error = injected.value_or(GetLastError());
          if (replacement_error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
            if (!MoveFileExW(backup.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
              preserve_recovery_files = true;
              recovery_required.insert(identity);
              BOOST_LOG(error) << "State replacement failed and the secured backup requires operator recovery"sv;
            }
          }
          return false;
        }
      } else if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return false;
      }
#else
      if (::fsync(handle)) {
        return false;
      }
      ::close(handle);
      handle = -1;
      fs::rename(temporary, destination);
#endif
      return true;
    } catch (const std::exception &) {
      BOOST_LOG(error) << "Could not persist state; no authorization update was published"sv;
      return false;
    }
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
    bool state_file_security_matches(std::vector<std::uint8_t> expected, std::vector<std::uint8_t> current, bool replacement) {
      return same_file_security(std::move(expected), std::move(current), replacement);
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
    if (!update_state_file_bytes(file, [&](const std::string &original) -> std::optional<std::string> {
          auto tree = original.empty() && !fs::exists(file) ? proc::catalog::json::object() : proc::catalog::parse(original);
          if (!tree.is_object()) {
            return std::nullopt;
          }
          tree["username"] = username;
          tree["salt"] = salt;
          tree["password"] = hash;
          return tree.dump(2) + "\n";
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
