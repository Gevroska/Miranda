/**
 * @file src/platform/windows/recovery_storage.cpp
 * @brief Read-only inspection of protected machine recovery storage.
 */
// header include
#include "recovery_storage.h"

// standard includes
#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// platform includes
// clang-format off
#include <WinSock2.h>
#include <Windows.h>
#include <aclapi.h>
#include <knownfolders.h>
#include <sddl.h>
#include <shlobj.h>
#include <winternl.h>
// clang-format on

// local includes
#include "src/utility.h"

// These registry declarations are absent from the pinned MinGW user-mode headers.
extern "C" {
  NTSTATUS NTAPI NtOpenKeyEx(PHANDLE key, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes, ULONG options);
  NTSTATUS NTAPI NtQueryKey(HANDLE key, int information_class, PVOID information, ULONG length, PULONG returned);
}

namespace platf::recovery_storage {
  namespace {
    // Win32 uses the same opaque pointer types for both read-only and mutating calls.
    using registry_handle_t = std::unique_ptr<std::remove_pointer_t<HKEY>, util::Destroy<HKEY, LSTATUS, &RegCloseKey>>;
    using directory_handle_t = std::unique_ptr<void, util::Destroy<HANDLE, BOOL, &CloseHandle>>;
    using local_ptr_t = std::unique_ptr<void, util::Destroy<HLOCAL, HLOCAL, &LocalFree>>;
    using known_path_t = util::safe_ptr<void, &CoTaskMemFree>;
    template<class T>
    using result_t = std::expected<T, error_e>;

    constexpr wchar_t machine_key[] = L"\\REGISTRY\\MACHINE";
    constexpr wchar_t product_name[] = L"Miranda";
    constexpr wchar_t recovery_name[] = L"DisplayRecovery";
    constexpr wchar_t manifest_name[] = L"Manifest";
    constexpr wchar_t journal_name[] = L"owned.state";
    constexpr REGSAM registry_access = KEY_WOW64_64KEY | KEY_QUERY_VALUE | READ_CONTROL;
    constexpr std::size_t max_path = 2048;
    constexpr std::size_t max_directories = 24;
    constexpr std::size_t manifest_size = 64;
    constexpr int key_name_information = 3;  ///< KeyNameInformation in KEY_INFORMATION_CLASS.
    constexpr auto security_information = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;

    enum class policy_e {
      windows_ancestor,
      windows_program_data,
      product
    };

    struct inputs_t {
#ifdef SUNSHINE_TESTS
      std::optional<test::roots_t> fixture;
#endif
    };

    bool same_name(std::wstring_view left, std::wstring_view right) {
      return left.size() == right.size() && CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
    }

    bool safe_component(std::wstring_view name) {
      return !name.empty() && name.back() != L'.' && name.back() != L' ' &&
             std::ranges::none_of(name, [](wchar_t c) {
               return c < L' ' || std::wstring_view {L"\\/:*?\"<>|"}.find(c) != std::wstring_view::npos;
             });
    }

    bool nonzero(const std::array<std::uint8_t, 16> &bytes) {
      return std::ranges::any_of(bytes, [](std::uint8_t byte) {
        return byte != 0;
      });
    }

    result_t<installation_t> read_manifest(HKEY key) {
      // Bytes 0-15 family, 16-17 schema LE, 18-19 policy LE, 20 mode, 21-23 reserved,
      // 24-39 deployment UUID, 40-47 volume serial LE, 48-63 opaque FileIdInfo identifier.
      constexpr std::array<std::uint8_t, 16> family {'M', 'i', 'r', 'a', 'n', 'd', 'a', 'R', 'e', 'c', 'o', 'v', 'e', 'r', 'y', 0};
      constexpr std::array<std::uint8_t, 4> version {1, 0, 1, 0};
      std::array<std::uint8_t, manifest_size> bytes {};
      DWORD length = bytes.size();
      DWORD type {};
      const auto status = RegQueryValueExW(key, manifest_name, nullptr, &type, bytes.data(), &length);
      if (status == ERROR_FILE_NOT_FOUND) {
        return std::unexpected(error_e::incomplete);
      }
      if (status == ERROR_MORE_DATA || (status == ERROR_SUCCESS && (type != REG_BINARY || length != bytes.size()))) {
        return std::unexpected(error_e::unsafe);
      }
      if (status != ERROR_SUCCESS) {
        return std::unexpected(error_e::unreadable);
      }
      if (!std::equal(family.begin(), family.end(), bytes.begin()) || !std::equal(version.begin(), version.end(), bytes.begin() + 16) || bytes[21] || bytes[22] || bytes[23]) {
        return std::unexpected(error_e::unsafe);
      }

      installation_t manifest {};
      manifest.mode = static_cast<mode_e>(bytes[20]);
      std::copy_n(bytes.begin() + 24, manifest.deployment_id.size(), manifest.deployment_id.begin());
      for (std::size_t index = 0; index < 8; ++index) {
        manifest.directory.volume_serial |= static_cast<std::uint64_t>(bytes[40 + index]) << (index * 8);
      }
      std::copy_n(bytes.begin() + 48, manifest.directory.file_id.size(), manifest.directory.file_id.begin());
      if ((manifest.mode != mode_e::native_only && manifest.mode != mode_e::recovery_managed) || !nonzero(manifest.deployment_id) || !nonzero(manifest.directory.file_id)) {
        return std::unexpected(error_e::unsafe);
      }
      return manifest;
    }

    struct security_snapshot_t {
      SECURITY_DESCRIPTOR_CONTROL control {};
      std::vector<BYTE> owner;
      std::vector<BYTE> dacl;
      bool operator==(const security_snapshot_t &) const = default;
    };

    DWORD permitted_ancestor_rights(PSID trustee, bool registry, bool program_data_default, BYTE ace_flags) {
      constexpr DWORD directory_read = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
      constexpr DWORD sibling_creation = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
      // Default ProgramData grants 0x116 to Builtin Users. On NTFS, a held present child
      // keeps its parent nonempty (reparse insertion requires an empty directory), and
      // changing case sensitivity requires delete-child in addition to these bits.
      constexpr DWORD program_data_users = sibling_creation | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
      static_assert(program_data_users == 0x116);
      if (IsWellKnownSid(trustee, WinCreatorOwnerSid) && (ace_flags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE))) {
        // This placeholder is substituted on inheritance; it grants no other principal
        // access to the current object. Every product descendant has a protected DACL.
        return registry ? KEY_ALL_ACCESS : FILE_ALL_ACCESS;
      }
      if (registry) {
        return KEY_READ;
      }
      if (program_data_default && IsWellKnownSid(trustee, WinBuiltinUsersSid)) {
        return directory_read | program_data_users;
      }
      return directory_read | sibling_creation;
    }

    result_t<security_snapshot_t> observe_security(HANDLE handle, SE_OBJECT_TYPE kind, policy_e policy, PSID fixture_owner) {
      const bool registry = kind == SE_REGISTRY_KEY;
      const bool product = policy == policy_e::product;
      PSID owner {};
      PACL dacl {};
      PSECURITY_DESCRIPTOR raw {};
      const auto status = GetSecurityInfo(handle, kind, security_information, &owner, nullptr, &dacl, nullptr, &raw);
      const local_ptr_t descriptor {raw};
      if (status != ERROR_SUCCESS || !descriptor) {
        return std::unexpected(error_e::unreadable);
      }

      SECURITY_DESCRIPTOR_CONTROL control {};
      DWORD revision {};
      ACL_SIZE_INFORMATION acl_size {};
      if (!IsValidSecurityDescriptor(raw) || !GetSecurityDescriptorControl(raw, &control, &revision) || !(control & SE_DACL_PRESENT) || !owner || !IsValidSid(owner) || !dacl || !IsValidAcl(dacl) || !GetAclInformation(dacl, &acl_size, sizeof(acl_size), AclSizeInformation)) {
        return std::unexpected(error_e::unsafe);
      }

      // Canonical Windows Modules Installer service SID; arbitrary service SIDs are untrusted.
      static const local_ptr_t servicing_sid {[] {
        PSID sid {};
        ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &sid);
        return sid;
      }()};
      const auto system_principal = [fixture_owner](PSID sid) {
        return fixture_owner ? EqualSid(sid, fixture_owner) : IsWellKnownSid(sid, WinLocalSystemSid);
      };
      const auto administrators = [](PSID sid) {
        return IsWellKnownSid(sid, WinBuiltinAdministratorsSid);
      };
      const auto servicing = [&](PSID sid) {
        return !product && servicing_sid && EqualSid(sid, servicing_sid.get());
      };
      if ((!system_principal(owner) && !administrators(owner) && !servicing(owner)) || (product && (!(control & SE_DACL_PROTECTED) || dacl->AclRevision != ACL_REVISION || acl_size.AceCount != 2))) {
        return std::unexpected(error_e::unsafe);
      }

      const bool program_data_default = policy == policy_e::windows_program_data &&
                                        system_principal(owner) && (control & SE_DACL_PROTECTED);
      const DWORD all_rights = registry ? KEY_ALL_ACCESS : FILE_ALL_ACCESS;
      GENERIC_MAPPING generic = registry ?
                                  GENERIC_MAPPING {KEY_READ, KEY_WRITE, KEY_EXECUTE, KEY_ALL_ACCESS} :
                                  GENERIC_MAPPING {FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
      constexpr auto inheritance_flags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | NO_PROPAGATE_INHERIT_ACE | INHERIT_ONLY_ACE | INHERITED_ACE;
      // The access-allowed and access-denied ACEs have the same mask/SID layout.
      constexpr auto sid_offset = offsetof(ACCESS_ALLOWED_ACE, SidStart);
      bool product_system {};
      bool product_administrators {};
      for (DWORD index = 0; index < acl_size.AceCount; ++index) {
        void *raw_ace {};
        if (!GetAce(dacl, index, &raw_ace)) {
          return std::unexpected(error_e::unsafe);
        }
        const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(raw_ace);
        const auto header = ace->Header;
        if ((header.AceType != ACCESS_ALLOWED_ACE_TYPE && header.AceType != ACCESS_DENIED_ACE_TYPE) || (header.AceFlags & ~inheritance_flags) || header.AceSize < sid_offset + GetSidLengthRequired(0)) {
          return std::unexpected(error_e::unsafe);
        }
        auto *sid = reinterpret_cast<SID *>(&static_cast<ACCESS_ALLOWED_ACE *>(raw_ace)->SidStart);
        if (header.AceSize != sid_offset + GetSidLengthRequired(sid->SubAuthorityCount) || !IsValidSid(sid)) {
          return std::unexpected(error_e::unsafe);
        }
        DWORD rights = ace->Mask;
        MapGenericMask(&rights, &generic);
        if (rights & ~all_rights) {
          return std::unexpected(error_e::unsafe);
        }

        if (product) {
          if (header.AceType != ACCESS_ALLOWED_ACE_TYPE || header.AceFlags || rights != all_rights) {
            return std::unexpected(error_e::unsafe);
          }
          product_system |= system_principal(sid);
          product_administrators |= administrators(sid);
        } else if (header.AceType == ACCESS_ALLOWED_ACE_TYPE && !(header.AceFlags & INHERIT_ONLY_ACE) && !system_principal(sid) && !administrators(sid) && !servicing(sid) && (rights & ~permitted_ancestor_rights(sid, registry, program_data_default, header.AceFlags))) {
          // Ignore denies for the purpose of giving credit to fewer rights. The allowed mask
          // must exclude substitution even if every deny is ineffective for a particular token.
          return std::unexpected(error_e::unsafe);
        }
      }
      if (product && (!product_system || !product_administrators)) {
        return std::unexpected(error_e::unsafe);
      }

      const auto *owner_bytes = static_cast<const BYTE *>(owner);
      const auto *dacl_bytes = reinterpret_cast<const BYTE *>(dacl);
      return security_snapshot_t {control, {owner_bytes, owner_bytes + GetLengthSid(owner)}, {dacl_bytes, dacl_bytes + acl_size.AclBytesInUse}};
    }

    result_t<std::wstring> registry_name(HKEY key) {
      // KEY_NAME_INFORMATION has a byte length followed by UTF-16 without a terminator.
      struct {
        ULONG length;
        wchar_t name[max_path];
      } information {};

      ULONG returned {};
      if (NtQueryKey(key, key_name_information, &information, sizeof(information), &returned) < 0 || returned < offsetof(decltype(information), name) || information.length % sizeof(wchar_t) || information.length == 0 || information.length > sizeof(information.name) || information.length > returned - offsetof(decltype(information), name)) {
        return std::unexpected(error_e::unreadable);
      }
      return std::wstring {information.name, information.length / sizeof(wchar_t)};
    }

    result_t<registry_handle_t> open_registry_anchor(const inputs_t &inputs) {
#ifdef SUNSHINE_TESTS
      if (inputs.fixture) {
        HKEY key {};
        const auto &name = inputs.fixture->registry_anchor;
        if (!safe_component(name) || name.size() > 255) {
          return std::unexpected(error_e::unsafe);
        }
        if (RegOpenKeyExW(HKEY_CURRENT_USER, name.c_str(), REG_OPTION_OPEN_LINK, registry_access, &key) != ERROR_SUCCESS) {
          return std::unexpected(error_e::unreadable);
        }
        return registry_handle_t {key};
      }
#endif
#ifdef _WIN64
      // Open the native machine root to get a real handle and bypass predefined-key overrides.
      // Native 64-bit callers cannot be redirected by UAC registry virtualization.
      HANDLE key {};
      UNICODE_STRING name {};
      RtlInitUnicodeString(&name, machine_key);
      OBJECT_ATTRIBUTES attributes {};
      InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
      if (NtOpenKeyEx(&key, registry_access, &attributes, REG_OPTION_OPEN_LINK) >= 0) {
        return registry_handle_t {static_cast<HKEY>(key)};
      }
#endif
      return std::unexpected(error_e::unreadable);
    }

    struct registry_snapshot_t {
      std::uint64_t last_write {};
      security_snapshot_t security;
      bool operator==(const registry_snapshot_t &) const = default;
    };

    struct registry_key_t {
      registry_handle_t handle;
      std::wstring name;
      policy_e policy;
      registry_snapshot_t snapshot;
    };

    result_t<registry_snapshot_t> observe_key(const registry_key_t &key, PSID fixture_owner) {
      const auto name = registry_name(key.handle.get());
      if (!name) {
        return std::unexpected(name.error());
      }
      if (!same_name(*name, key.name)) {
        return std::unexpected(error_e::unsafe);
      }

      // A link opened with OPEN_LINK has only SymbolicLinkValue and has no subkeys.
      // An uninitialized link cannot supply the required descendants or terminal manifest.
      const auto link = RegQueryValueExW(key.handle.get(), L"SymbolicLinkValue", nullptr, nullptr, nullptr, nullptr);
      if (link != ERROR_FILE_NOT_FOUND) {
        return std::unexpected(link == ERROR_SUCCESS ? error_e::unsafe : error_e::unreadable);
      }
      auto security = observe_security(key.handle.get(), SE_REGISTRY_KEY, key.policy, fixture_owner);
      if (!security) {
        return std::unexpected(security.error());
      }
      FILETIME last_write {};
      if (RegQueryInfoKeyW(key.handle.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &last_write) != ERROR_SUCCESS) {
        return std::unexpected(error_e::unreadable);
      }
      return registry_snapshot_t {(static_cast<std::uint64_t>(last_write.dwHighDateTime) << 32) | last_write.dwLowDateTime, std::move(*security)};
    }

    result_t<std::vector<registry_key_t>> open_registry(const inputs_t &inputs, PSID fixture_owner) {
      auto anchor = open_registry_anchor(inputs);
      if (!anchor) {
        return std::unexpected(anchor.error());
      }
      std::wstring name {machine_key};
#ifdef SUNSHINE_TESTS
      if (inputs.fixture) {
        auto fixture_name = registry_name(anchor->get());
        if (!fixture_name) {
          return std::unexpected(fixture_name.error());
        }
        name = std::move(*fixture_name);
      }
#endif
      std::vector<registry_key_t> keys;
      keys.reserve(4);
      keys.push_back({std::move(*anchor), std::move(name), policy_e::windows_ancestor, {}});
      constexpr std::array children {L"SOFTWARE", product_name, recovery_name};
      for (const auto *child : children) {
        auto &parent = keys.back();
        auto snapshot = observe_key(parent, fixture_owner);
        if (!snapshot) {
          return std::unexpected(snapshot.error());
        }
        parent.snapshot = std::move(*snapshot);

        HKEY key {};
        const auto status = RegOpenKeyExW(parent.handle.get(), child, REG_OPTION_OPEN_LINK, registry_access, &key);
        if (status != ERROR_SUCCESS) {
          if (status == ERROR_FILE_NOT_FOUND) {
            return std::unexpected(keys.size() == 2 ? error_e::no_install_record : error_e::incomplete);
          }
          return std::unexpected(error_e::unreadable);
        }
        const auto policy = keys.size() == 1 ? policy_e::windows_ancestor : policy_e::product;
        keys.push_back({registry_handle_t {key}, parent.name + L'\\' + child, policy, {}});
      }
      auto snapshot = observe_key(keys.back(), fixture_owner);
      if (!snapshot) {
        return std::unexpected(snapshot.error());
      }
      keys.back().snapshot = std::move(*snapshot);
      return keys;
    }

    result_t<std::filesystem::path> program_data_path(const inputs_t &inputs) {
#ifdef SUNSHINE_TESTS
      if (inputs.fixture) {
        return inputs.fixture->program_data;
      }
#endif
      PWSTR path {};
      const auto status = SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DONT_VERIFY, nullptr, &path);
      const known_path_t owned_path {path};
      if (FAILED(status) || !owned_path) {
        return std::unexpected(error_e::unreadable);
      }
      return std::filesystem::path {static_cast<const wchar_t *>(owned_path.get())};
    }

    result_t<std::vector<std::wstring>> directory_names(const std::filesystem::path &program_data) {
      const auto &path = program_data.native();
      if (path.size() < 4 || path.size() > max_path - 80 || !((L'A' <= path[0] && path[0] <= L'Z') || (L'a' <= path[0] && path[0] <= L'z')) || path[1] != L':' || path[2] != L'\\') {
        return std::unexpected(error_e::unsafe);
      }
      std::vector<std::wstring> names {path.substr(0, 3)};
      for (std::size_t start = 3; start <= path.size();) {
        const auto end = path.find(L'\\', start);
        const auto part = std::wstring_view {path}.substr(start, end == path.npos ? path.size() - start : end - start);
        if (!safe_component(part) || part.size() > 255 || names.size() >= max_directories - 2) {
          return std::unexpected(error_e::unsafe);
        }
        names.emplace_back(part);
        if (end == path.npos) {
          break;
        }
        start = end + 1;
      }
      names.emplace_back(product_name);
      names.emplace_back(recovery_name);
      return names;
    }

    struct directory_snapshot_t {
      directory_id_t id;
      DWORD attributes {};
      std::optional<security_snapshot_t> security;
      bool operator==(const directory_snapshot_t &) const = default;
    };

    struct directory_t {
      directory_handle_t handle;
      std::wstring dos_path;
      std::wstring volume_path;
      std::optional<policy_e> policy;  ///< Omitted only for ancestors outside a private test fixture.
      directory_snapshot_t snapshot;
    };

    result_t<void> check_final_path(HANDLE handle, DWORD volume_mode, const std::wstring &expected) {
      std::array<wchar_t, max_path> path {};
      const auto length = GetFinalPathNameByHandleW(handle, path.data(), path.size(), FILE_NAME_NORMALIZED | volume_mode);
      if (!length || length >= path.size()) {
        return std::unexpected(error_e::unreadable);
      }
      if (!same_name(std::wstring_view {path.data(), length}, expected)) {
        return std::unexpected(error_e::unsafe);
      }
      return {};
    }

    result_t<directory_snapshot_t> observe_directory(const directory_t &directory, PSID fixture_owner) {
      const auto handle = directory.handle.get();
      FILE_ATTRIBUTE_TAG_INFO tags {};
      FILE_CASE_SENSITIVE_INFORMATION case_info {};
      FILE_ID_INFO id {};
      if (GetFileType(handle) != FILE_TYPE_DISK) {
        return std::unexpected(error_e::unsafe);
      }
      if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tags, sizeof(tags))) {
        return std::unexpected(error_e::unreadable);
      }
      if (!(tags.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || tags.ReparseTag) {
        return std::unexpected(error_e::unsafe);
      }
      if (!GetFileInformationByHandleEx(handle, FileCaseSensitiveInfo, &case_info, sizeof(case_info)) || !GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))) {
        return std::unexpected(error_e::unreadable);
      }
      if (case_info.Flags) {
        return std::unexpected(error_e::unsafe);
      }
      for (const auto mode : {VOLUME_NAME_DOS, VOLUME_NAME_GUID}) {
        const auto &expected = mode == VOLUME_NAME_DOS ? directory.dos_path : directory.volume_path;
        if (auto path = check_final_path(handle, mode, expected); !path) {
          return std::unexpected(path.error());
        }
      }

      directory_snapshot_t snapshot {};
      snapshot.attributes = tags.FileAttributes;
      snapshot.id.volume_serial = id.VolumeSerialNumber;
      std::ranges::copy(id.FileId.Identifier, snapshot.id.file_id.begin());
      if (!nonzero(snapshot.id.file_id)) {
        return std::unexpected(error_e::unsafe);
      }
      if (directory.policy) {
        auto security = observe_security(handle, SE_FILE_OBJECT, *directory.policy, fixture_owner);
        if (!security) {
          return std::unexpected(security.error());
        }
        snapshot.security = std::move(*security);
      }
      return snapshot;
    }

    result_t<std::vector<directory_t>> open_directories(const inputs_t &inputs, const std::filesystem::path &program_data, PSID fixture_owner) {
      const auto names = directory_names(program_data);
      if (!names) {
        return std::unexpected(names.error());
      }
      const auto &drive = names->front();
      if (GetDriveTypeW(drive.c_str()) != DRIVE_FIXED) {
        return std::unexpected(error_e::unsafe);
      }
      std::array<wchar_t, 50> volume {};
      if (!GetVolumeNameForVolumeMountPointW(drive.c_str(), volume.data(), volume.size())) {
        return std::unexpected(error_e::unreadable);
      }
      const std::wstring_view volume_name {volume.data()};
      if (volume_name.size() != 49 || !volume_name.starts_with(L"\\\\?\\Volume{") || !volume_name.ends_with(L"}\\")) {
        return std::unexpected(error_e::unsafe);
      }
      std::wstring dos_path = L"\\\\?\\" + drive;
      std::wstring volume_path {volume_name};
      const auto program_data_index = names->size() - 3;
      std::vector<directory_t> directories;
      directories.reserve(names->size());
      for (std::size_t index = 0; index < names->size(); ++index) {
        if (index != 0) {
          const auto separator = index == 1 ? L"" : L"\\";
          dos_path += separator + (*names)[index];
          volume_path += separator + (*names)[index];
        }
        const auto handle = CreateFileW(dos_path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
          const auto error = GetLastError();
          return std::unexpected(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? error_e::incomplete : error_e::unreadable);
        }
        std::optional policy {policy_e::windows_ancestor};
        if (index == program_data_index) {
          policy = policy_e::windows_program_data;
        } else if (index > program_data_index) {
          policy = policy_e::product;
        }
#ifdef SUNSHINE_TESTS
        if (inputs.fixture && index < program_data_index) {
          policy.reset();
        }
#endif
        directory_t directory {directory_handle_t {handle}, dos_path, volume_path, policy, {}};
        if (index == 0) {
          std::array<wchar_t, 16> filesystem {};
          if (!GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, nullptr, filesystem.data(), filesystem.size())) {
            return std::unexpected(error_e::unreadable);
          }
          if (std::wstring_view {filesystem.data()} != L"NTFS") {
            return std::unexpected(error_e::unsafe);
          }
        }
        auto snapshot = observe_directory(directory, fixture_owner);
        if (!snapshot) {
          return std::unexpected(snapshot.error());
        }
        if (index != 0 && snapshot->id.volume_serial != directories.front().snapshot.id.volume_serial) {
          return std::unexpected(error_e::unsafe);
        }
        directory.snapshot = std::move(*snapshot);
        directories.push_back(std::move(directory));
      }
      return directories;
    }
  }  // namespace

  struct observation_t::state_t {
    inputs_t inputs;
    local_ptr_t fixture_owner;
    std::vector<registry_key_t> registry;
    std::vector<directory_t> directories;
    installation_t manifest;
    std::filesystem::path journal;
  };

  struct inspector_t {
    using state_t = observation_t::state_t;

    static result_t<state_t> scan(const inputs_t &inputs) {
      local_ptr_t fixture_owner;
#ifdef SUNSHINE_TESTS
      if (inputs.fixture && !inputs.fixture->fixture_owner_sid.empty()) {
        PSID owner {};
        if (!ConvertStringSidToSidW(inputs.fixture->fixture_owner_sid.c_str(), &owner)) {
          return std::unexpected(error_e::unreadable);
        }
        fixture_owner.reset(owner);
      }
#endif
      auto registry = open_registry(inputs, fixture_owner.get());
      if (!registry) {
        return std::unexpected(registry.error());
      }
      const auto manifest = read_manifest(registry->back().handle.get());
      if (!manifest) {
        return std::unexpected(manifest.error());
      }

      const auto program_data = program_data_path(inputs);
      if (!program_data) {
        return std::unexpected(program_data.error());
      }
      auto directories = open_directories(inputs, *program_data, fixture_owner.get());
      if (!directories) {
        return std::unexpected(directories.error());
      }
      if (directories->back().snapshot.id != manifest->directory) {
        return std::unexpected(error_e::unsafe);
      }
      return state_t {inputs, std::move(fixture_owner), std::move(*registry), std::move(*directories), *manifest, *program_data / product_name / recovery_name / journal_name};
    }

    static bool still_matches(const state_t &retained) {
      const auto fresh = scan(retained.inputs);
      if (!fresh || fresh->manifest != retained.manifest || fresh->journal != retained.journal || fresh->registry.size() != retained.registry.size() || fresh->directories.size() != retained.directories.size()) {
        return false;
      }
      for (std::size_t index = 0; index < retained.registry.size(); ++index) {
        const auto &key = retained.registry[index];
        const auto &new_key = fresh->registry[index];
        auto snapshot = observe_key(key, retained.fixture_owner.get());
        if (!snapshot || *snapshot != key.snapshot || new_key.snapshot != key.snapshot || !same_name(key.name, new_key.name)) {
          return false;
        }
      }
      for (std::size_t index = 0; index < retained.directories.size(); ++index) {
        const auto &directory = retained.directories[index];
        const auto &new_directory = fresh->directories[index];
        auto snapshot = observe_directory(directory, retained.fixture_owner.get());
        if (!snapshot || *snapshot != directory.snapshot || new_directory.snapshot != directory.snapshot || directory.dos_path != new_directory.dos_path || directory.volume_path != new_directory.volume_path) {
          return false;
        }
      }
      const auto manifest = read_manifest(retained.registry.back().handle.get());
      return manifest && *manifest == retained.manifest;
    }

#ifdef SUNSHINE_TESTS
    static result_t<observation_t> inspect(const inputs_t &inputs, const std::function<void()> &between_scans = {}) {
#else
    static result_t<observation_t> inspect(const inputs_t &inputs) {
#endif
      auto first = scan(inputs);
    if (!first) {
      return std::unexpected(first.error());
    }
#ifdef SUNSHINE_TESTS
    if (between_scans) {
      between_scans();
    }
#endif
    if (!still_matches(*first)) {
      return std::unexpected(error_e::changed);
    }
    return observation_t {std::make_unique<state_t>(std::move(*first))};
  }
};  // namespace platf::recovery_storage

observation_t::observation_t(std::unique_ptr<state_t> state):
    state_ {std::move(state)} {}

observation_t::~observation_t() = default;
observation_t::observation_t(observation_t &&) noexcept = default;
observation_t &observation_t::operator=(observation_t &&) noexcept = default;

const installation_t &observation_t::installation() const {
  return state_->manifest;
}

const std::filesystem::path &observation_t::journal_path() const {
  return state_->journal;
}

std::expected<void, error_e> observation_t::recheck() const {
  if (!state_ || !inspector_t::still_matches(*state_)) {
    return std::unexpected(error_e::changed);
  }
  return {};
}

std::expected<observation_t, error_e> inspect_machine_storage() {
  return inspector_t::inspect({});
}

#ifdef SUNSHINE_TESTS
std::pair<HKEY, HANDLE> observation_t::test_handles() const {
  return {state_->registry.back().handle.get(), state_->directories.back().handle.get()};
}

std::expected<observation_t, error_e> test::inspect(const roots_t &roots, const std::function<void()> &between_scans) {
  return inspector_t::inspect({roots}, between_scans);
}
#endif
}  // namespace platf::recovery_storage
