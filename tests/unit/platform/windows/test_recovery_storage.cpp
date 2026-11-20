/**
 * @file tests/unit/platform/windows/test_recovery_storage.cpp
 * @brief Exercise the read-only recovery observation with private registry and NTFS fixtures.
 */
// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <algorithm>
  #include <array>
  #include <filesystem>
  #include <fstream>
  #include <iterator>
  #include <memory>
  #include <string>
  #include <string_view>
  #include <type_traits>
  #include <vector>

  // platform includes
  // clang-format off
  #include <WinSock2.h>
  #include <Windows.h>
  #include <aclapi.h>
  #include <objbase.h>
  #include <sddl.h>
  #include <winternl.h>
  // clang-format on

  // local includes
  #include "src/platform/windows/recovery_storage.h"
  #include "src/utility.h"

extern "C" NTSTATUS NTAPI NtDeleteKey(HANDLE key);

namespace {
  using namespace platf::recovery_storage;
  namespace fs = std::filesystem;
  using registry_handle_t = std::unique_ptr<std::remove_pointer_t<HKEY>, util::Destroy<HKEY, LSTATUS, &RegCloseKey>>;
  using native_handle_t = std::unique_ptr<void, util::Destroy<HANDLE, BOOL, &CloseHandle>>;
  using local_ptr_t = util::safe_ptr_v2<void, HLOCAL, &LocalFree>;
  constexpr auto view = KEY_WOW64_64KEY;
  constexpr auto owner_dacl = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;

  static_assert(std::is_move_constructible_v<observation_t>);
  static_assert(!std::is_copy_constructible_v<observation_t>);

  std::wstring current_user_sid() {
    HANDLE raw {};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
      return {};
    }
    const native_handle_t token {raw};
    alignas(TOKEN_USER) std::array<BYTE, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> buffer {};
    DWORD bytes {};
    LPWSTR sid {};
    if (!GetTokenInformation(raw, TokenUser, buffer.data(), buffer.size(), &bytes) || !ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER *>(buffer.data())->User.Sid, &sid)) {
      return {};
    }
    const local_ptr_t owner {sid};
    return sid;
  }

  directory_id_t directory_id(HANDLE handle) {
    FILE_ID_INFO info {};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) {
      return {};
    }
    directory_id_t identity {};
    identity.volume_serial = info.VolumeSerialNumber;
    std::ranges::copy(info.FileId.Identifier, identity.file_id.begin());
    return identity;
  }

  std::wstring describe_security(HANDLE handle, SE_OBJECT_TYPE kind) {
    PSECURITY_DESCRIPTOR raw {};
    const auto status = GetSecurityInfo(handle, kind, owner_dacl, nullptr, nullptr, nullptr, nullptr, &raw);
    const local_ptr_t descriptor {raw};
    LPWSTR text {};
    if (status != ERROR_SUCCESS || !ConvertSecurityDescriptorToStringSecurityDescriptorW(raw, SDDL_REVISION_1, owner_dacl, &text, nullptr)) {
      return {};
    }
    const local_ptr_t owned_text {text};
    return text;
  }

  registry_handle_t create_key(HKEY parent, const wchar_t *name, const std::wstring &sddl, DWORD options = REG_OPTION_NON_VOLATILE, LSTATUS *failure = nullptr) {
    PSECURITY_DESCRIPTOR raw {};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr)) {
      if (failure) {
        *failure = GetLastError();
      }
      return {};
    }
    const local_ptr_t descriptor {raw};
    SECURITY_ATTRIBUTES security {sizeof(SECURITY_ATTRIBUTES), raw, FALSE};
    HKEY key {};
    DWORD disposition {};
    const auto status = RegCreateKeyExW(parent, name, 0, nullptr, options, KEY_ALL_ACCESS | view, &security, &key, &disposition);
    registry_handle_t result {key};
    if (failure) {
      *failure = status == ERROR_SUCCESS && disposition != REG_CREATED_NEW_KEY ? ERROR_ALREADY_EXISTS : status;
    }
    if (status != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
      return {};
    }
    return result;
  }

  bool create_directory(const fs::path &path, const std::wstring &sddl) {
    PSECURITY_DESCRIPTOR raw {};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr)) {
      return false;
    }
    const local_ptr_t descriptor {raw};
    SECURITY_ATTRIBUTES security {sizeof(SECURITY_ATTRIBUTES), raw, FALSE};
    return CreateDirectoryW(path.c_str(), &security);
  }

  native_handle_t open_directory(const fs::path &path, DWORD access = FILE_READ_ATTRIBUTES | READ_CONTROL) {
    const auto handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    return native_handle_t {handle == INVALID_HANDLE_VALUE ? nullptr : handle};
  }

  std::string read_file(const fs::path &path) {
    std::ifstream input {path, std::ios::binary};
    return {std::istreambuf_iterator<char> {input}, {}};
  }

  class RecoveryStorageTest: public BaseTest {
  protected:
    void SetUp() override {
      BaseTest::SetUp();
      GUID guid {};
      std::array<wchar_t, 39> id {};
      ASSERT_EQ(CoCreateGuid(&guid), S_OK);
      ASSERT_NE(StringFromGUID2(guid, id.data(), id.size()), 0);
      const auto name = std::wstring {L"MirandaRecoveryStorageTest-"} + id.data();
      const auto base = fs::temp_directory_path().make_preferred();
      std::array<wchar_t, 16> filesystem {};
      std::array<wchar_t, MAX_PATH> volume {};
      if (!GetVolumePathNameW(base.c_str(), volume.data(), volume.size()) || GetDriveTypeW(volume.data()) != DRIVE_FIXED || !GetVolumeInformationW(volume.data(), nullptr, 0, nullptr, nullptr, nullptr, filesystem.data(), filesystem.size()) || std::wstring_view {filesystem.data()} != L"NTFS") {
        GTEST_SKIP() << "A writable private fixture on a fixed NTFS drive is required";
      }

      roots = {name, base / name, current_user_sid()};
      ASSERT_FALSE(roots.fixture_owner_sid.empty());
      ASSERT_TRUE(create_directory(roots.program_data, directory_sddl()));
      directory_created = true;
      ASSERT_TRUE(create_directory(product_path(), directory_sddl()));
      ASSERT_TRUE(create_directory(recovery_path(), directory_sddl()));

      anchor = create_key(HKEY_CURRENT_USER, roots.registry_anchor.c_str(), registry_sddl());
      ASSERT_TRUE(anchor);
      software = create_key(anchor.get(), L"SOFTWARE", registry_sddl());
      ASSERT_TRUE(software);
      product = create_key(software.get(), L"Miranda", registry_sddl());
      ASSERT_TRUE(product);
      recovery = create_key(product.get(), L"DisplayRecovery", registry_sddl());
      ASSERT_TRUE(recovery);

      const auto directory = open_directory(recovery_path());
      ASSERT_TRUE(directory);
      ASSERT_NE(directory_id(directory.get()), directory_id_t {});
      ASSERT_EQ(write_manifest(manifest()), ERROR_SUCCESS);
    }

    void TearDown() override {
      recovery.reset();
      product.reset();
      software.reset();
      if (anchor) {
        anchor.reset();
        EXPECT_EQ(RegDeleteTreeW(HKEY_CURRENT_USER, roots.registry_anchor.c_str()), ERROR_SUCCESS);
      }
      if (directory_created) {
        std::error_code error;
        fs::remove_all(roots.program_data, error);
        EXPECT_FALSE(error) << error.message();
      }
      BaseTest::TearDown();
    }

    std::wstring directory_sddl(std::wstring_view extra = {}) const {
      return L"O:" + roots.fixture_owner_sid + L"D:P(A;;FA;;;" + roots.fixture_owner_sid + L")(A;;FA;;;BA)" + std::wstring {extra};
    }

    std::wstring registry_sddl() const {
      return L"O:" + roots.fixture_owner_sid + L"D:P(A;;KA;;;" + roots.fixture_owner_sid + L")(A;;KA;;;BA)";
    }

    fs::path product_path() const {
      return roots.program_data / L"Miranda";
    }

    fs::path recovery_path() const {
      return product_path() / L"DisplayRecovery";
    }

    std::array<BYTE, 64> manifest(mode_e mode = mode_e::native_only) const {
      const auto directory = open_directory(recovery_path());
      EXPECT_TRUE(directory);
      const auto id = directory_id(directory.get());
      const std::string_view family = "MirandaRecovery";
      std::array<BYTE, 64> data {};
      std::ranges::copy(family, data.begin());
      data[16] = 1;
      data[18] = 1;
      data[20] = static_cast<BYTE>(mode);
      data[24] = 0x3b;
      data[39] = 0xe5;
      for (std::size_t index = 0; index < 8; ++index) {
        data[40 + index] = (id.volume_serial >> (index * 8)) & 0xff;
      }
      std::ranges::copy(id.file_id, data.begin() + 48);
      return data;
    }

    template<class Bytes>
    LSTATUS write_manifest(const Bytes &bytes, DWORD type = REG_BINARY) const {
      return RegSetValueExW(recovery.get(), L"Manifest", 0, type, bytes.data(), bytes.size());
    }

    DWORD set_dacl(HANDLE handle, SE_OBJECT_TYPE type, const std::wstring &sddl, bool protect = true) const {
      PSECURITY_DESCRIPTOR raw {};
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr)) {
        return GetLastError();
      }
      const local_ptr_t descriptor {raw};
      PACL acl {};
      BOOL present {}, defaulted {};
      if (!GetSecurityDescriptorDacl(raw, &present, &acl, &defaulted) || !present) {
        return ERROR_INVALID_SECURITY_DESCR;
      }
      return SetSecurityInfo(handle, type, DACL_SECURITY_INFORMATION | (protect ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION), nullptr, nullptr, acl, nullptr);
    }

    void refuses(error_e error) const {
      const auto observation = test::inspect(roots);
      ASSERT_FALSE(observation);
      EXPECT_EQ(observation.error(), error);
    }

    test::roots_t roots;
    registry_handle_t anchor, software, product, recovery;
    bool directory_created {};
  };

  TEST_F(RecoveryStorageTest, DeclaredModesHaveActualHeldHandlesAndStableRecheck) {
    for (const auto mode : {mode_e::native_only, mode_e::recovery_managed}) {
      ASSERT_EQ(write_manifest(manifest(mode)), ERROR_SUCCESS);
      auto inspected = test::inspect(roots);
      ASSERT_TRUE(inspected) << static_cast<int>(inspected.error());
      auto observed = std::move(*inspected);
      EXPECT_FALSE(inspected->recheck());
      const auto [key, directory] = observed.test_handles();
      EXPECT_EQ(observed.installation().mode, mode);
      EXPECT_EQ(observed.installation().deployment_id.front(), 0x3b);
      EXPECT_EQ(observed.installation().deployment_id.back(), 0xe5);
      EXPECT_EQ(observed.installation().directory, directory_id(directory));
      EXPECT_EQ(observed.journal_path(), recovery_path() / L"owned.state");
      EXPECT_EQ(RegQueryValueExW(key, L"Manifest", nullptr, nullptr, nullptr, nullptr), ERROR_SUCCESS);
      EXPECT_EQ(RegSetValueExW(key, L"forbidden", 0, REG_BINARY, nullptr, 0), ERROR_ACCESS_DENIED);
      DWORD flags {};
      ASSERT_TRUE(GetHandleInformation(directory, &flags));
      EXPECT_EQ(flags & HANDLE_FLAG_INHERIT, 0);
      EXPECT_TRUE(observed.recheck());
      EXPECT_FALSE(fs::exists(observed.journal_path()));
    }
  }

  TEST_F(RecoveryStorageTest, RefusesMalformedAndMismatchedManifestsWithoutRepair) {
    const auto valid = manifest();
    for (const auto index : {0, 15, 16, 17, 18, 19, 20, 21, 22, 23, 40, 47, 48, 63}) {
      auto bytes = valid;
      bytes[index] ^= 0x80;
      ASSERT_EQ(write_manifest(bytes), ERROR_SUCCESS);
      refuses(error_e::unsafe);
    }
    auto null_uuid = valid;
    std::fill(null_uuid.begin() + 24, null_uuid.begin() + 40, 0);
    ASSERT_EQ(write_manifest(null_uuid), ERROR_SUCCESS);
    refuses(error_e::unsafe);
    ASSERT_EQ(write_manifest(valid, REG_NONE), ERROR_SUCCESS);
    refuses(error_e::unsafe);
    std::vector<BYTE> wrong_size {valid.begin(), valid.end() - 1};
    ASSERT_EQ(write_manifest(wrong_size), ERROR_SUCCESS);
    refuses(error_e::unsafe);
    wrong_size.resize(65);
    ASSERT_EQ(write_manifest(wrong_size), ERROR_SUCCESS);
    refuses(error_e::unsafe);
    ASSERT_EQ(RegDeleteValueW(recovery.get(), L"Manifest"), ERROR_SUCCESS);
    refuses(error_e::incomplete);
  }

  TEST_F(RecoveryStorageTest, ADirectoryAloneCannotStandInForARegistryRecord) {
    ASSERT_EQ(RegRenameKey(software.get(), L"Miranda", L"OtherRegistryBranch"), ERROR_SUCCESS);
    refuses(error_e::no_install_record);
    ASSERT_EQ(RegRenameKey(software.get(), L"OtherRegistryBranch", L"Miranda"), ERROR_SUCCESS);
    ASSERT_EQ(RegRenameKey(product.get(), L"DisplayRecovery", L"OtherRecord"), ERROR_SUCCESS);
    refuses(error_e::incomplete);
    ASSERT_EQ(RegRenameKey(product.get(), L"OtherRecord", L"DisplayRecovery"), ERROR_SUCCESS);
    ASSERT_TRUE(fs::remove(recovery_path()));
    refuses(error_e::incomplete);
    EXPECT_FALSE(fs::exists(recovery_path()));
  }

  TEST_F(RecoveryStorageTest, RejectsRegistryWritesFromUntrustedTrusteesAndReportsReadDenial) {
    ASSERT_EQ(set_dacl(product.get(), SE_REGISTRY_KEY, registry_sddl() + L"(A;;KW;;;BU)"), ERROR_SUCCESS);
    refuses(error_e::unsafe);
    ASSERT_EQ(set_dacl(product.get(), SE_REGISTRY_KEY, registry_sddl()), ERROR_SUCCESS);
    ASSERT_EQ(set_dacl(recovery.get(), SE_REGISTRY_KEY, L"D:P(D;;KR;;;" + roots.fixture_owner_sid + L")(A;;KA;;;BA)"), ERROR_SUCCESS);
    const auto denied = describe_security(recovery.get(), SE_REGISTRY_KEY);
    ASSERT_FALSE(denied.empty());
    refuses(error_e::unreadable);
    EXPECT_TRUE(denied == describe_security(recovery.get(), SE_REGISTRY_KEY));
    ASSERT_EQ(set_dacl(recovery.get(), SE_REGISTRY_KEY, registry_sddl()), ERROR_SUCCESS);
    ASSERT_TRUE(test::inspect(roots));

    ASSERT_EQ(set_dacl(software.get(), SE_REGISTRY_KEY, registry_sddl() + L"(A;;0x20;;;BU)"), ERROR_SUCCESS);
    refuses(error_e::unsafe);
  }

  TEST_F(RecoveryStorageTest, FixtureOwnerIsAcceptedOnlyWithTheTestPolicy) {
    if (roots.fixture_owner_sid == L"S-1-5-18") {
      GTEST_SKIP() << "The fixture is already owned by LocalSystem";
    }
    auto production_owner_policy = roots;
    production_owner_policy.fixture_owner_sid.clear();
    const auto observed = test::inspect(production_owner_policy);
    ASSERT_FALSE(observed);
    EXPECT_EQ(observed.error(), error_e::unsafe);
  }

  TEST_F(RecoveryStorageTest, RegistryLinksAtEitherProductComponentAreRefused) {
    const std::wstring absolute = L"\\REGISTRY\\USER\\" + roots.fixture_owner_sid + L"\\" + roots.registry_anchor + L"\\SOFTWARE\\";
    for (const bool parent_link : {true, false}) {
      const auto parent = parent_link ? software.get() : product.get();
      const auto component = parent_link ? L"Miranda" : L"DisplayRecovery";
      const auto renamed = L"LinkTarget";
      ASSERT_EQ(RegRenameKey(parent, component, renamed), ERROR_SUCCESS);
      LSTATUS error {};
      auto link = create_key(parent, component, registry_sddl(), REG_OPTION_CREATE_LINK, &error);
      if (!link) {
        ASSERT_EQ(RegRenameKey(parent, renamed, component), ERROR_SUCCESS);
        if (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD) {
          GTEST_SKIP() << "Registry symbolic-link creation requires permission on this Windows host";
        }
        FAIL() << "Could not create registry link fixture, error " << error;
      }
      const auto target = absolute + (parent_link ? L"" : L"Miranda\\") + renamed;
      const auto wrote = RegSetValueExW(link.get(), L"SymbolicLinkValue", 0, REG_LINK, reinterpret_cast<const BYTE *>(target.data()), target.size() * sizeof(wchar_t));
      if (wrote == ERROR_SUCCESS) {
        refuses(error_e::unsafe);
      }
      ASSERT_GE(NtDeleteKey(link.get()), 0);
      ASSERT_EQ(RegRenameKey(parent, renamed, component), ERROR_SUCCESS);
      ASSERT_EQ(wrote, ERROR_SUCCESS);
    }
  }

  TEST_F(RecoveryStorageTest, WindowsAncestorRightsDifferFromProductRights) {
    const auto ancestor = open_directory(roots.program_data, WRITE_DAC | READ_CONTROL);
    const auto controlled = open_directory(product_path(), WRITE_DAC | READ_CONTROL);
    ASSERT_TRUE(ancestor);
    ASSERT_TRUE(controlled);
    ASSERT_EQ(set_dacl(software.get(), SE_REGISTRY_KEY, registry_sddl() + L"(A;CI;KA;;;CO)(A;CI;KR;;;AC)(A;CI;KR;;;BU)"), ERROR_SUCCESS);
    ASSERT_EQ(set_dacl(ancestor.get(), SE_FILE_OBJECT, directory_sddl(L"(A;CI;0x116;;;BU)(A;OICIIO;FA;;;CO)(A;OICI;0x1200a9;;;BU)")), ERROR_SUCCESS);
    EXPECT_TRUE(test::inspect(roots));

    ASSERT_EQ(set_dacl(controlled.get(), SE_FILE_OBJECT, directory_sddl(L"(A;CI;0x116;;;BU)")), ERROR_SUCCESS);
    refuses(error_e::unsafe);
    ASSERT_EQ(set_dacl(controlled.get(), SE_FILE_OBJECT, directory_sddl()), ERROR_SUCCESS);

    ASSERT_EQ(set_dacl(ancestor.get(), SE_FILE_OBJECT, directory_sddl(L"(A;OICIIO;0x40;;;BU)")), ERROR_SUCCESS);
    EXPECT_TRUE(test::inspect(roots));
    for (const auto *extra : {L"(A;CI;0x156;;;BU)", L"(A;;0x10000;;;AU)", L"(A;;0x40000;;;AU)", L"(A;;0x80000;;;AU)", L"(A;;0x10;;;WD)", L"(A;;0x100;;;AU)"}) {
      ASSERT_EQ(set_dacl(ancestor.get(), SE_FILE_OBJECT, directory_sddl(extra)), ERROR_SUCCESS);
      refuses(error_e::unsafe);
    }
    ASSERT_EQ(set_dacl(ancestor.get(), SE_FILE_OBJECT, directory_sddl()), ERROR_SUCCESS);

    // An unsupported ACE form is rejected even where its access mask alone permits reading.
    ASSERT_EQ(set_dacl(controlled.get(), SE_FILE_OBJECT, directory_sddl(L"(OA;;FR;6d48b0c3-b996-4392-8ab9-f23005c528f0;;BU)")), ERROR_SUCCESS);
    refuses(error_e::unsafe);
    ASSERT_EQ(set_dacl(controlled.get(), SE_FILE_OBJECT, directory_sddl(), false), ERROR_SUCCESS);
    refuses(error_e::unsafe);
    ASSERT_EQ(SetSecurityInfo(controlled.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr), ERROR_SUCCESS);
    refuses(error_e::unsafe);
    ASSERT_EQ(set_dacl(controlled.get(), SE_FILE_OBJECT, directory_sddl()), ERROR_SUCCESS);
  }

  TEST_F(RecoveryStorageTest, RealDirectoryPinBlocksReplacementAndOldBindingRefusesNewDirectory) {
    const auto replacement = product_path() / L"RetainedDirectory";
    {
      const auto observed = test::inspect(roots);
      ASSERT_TRUE(observed) << static_cast<int>(observed.error());
      EXPECT_FALSE(MoveFileExW(recovery_path().c_str(), replacement.c_str(), 0));
      EXPECT_TRUE(observed->recheck());
    }
    ASSERT_TRUE(MoveFileExW(recovery_path().c_str(), replacement.c_str(), 0));
    ASSERT_TRUE(create_directory(recovery_path(), directory_sddl()));
    refuses(error_e::unsafe);
    EXPECT_EQ(write_manifest(manifest()), ERROR_SUCCESS);
    EXPECT_TRUE(test::inspect(roots));
  }

  TEST_F(RecoveryStorageTest, DirectorySymlinkIsRefusedEvenWhenTheTargetHasTheManifestIdentity) {
    const auto target = product_path() / L"RetainedDirectory";
    ASSERT_TRUE(MoveFileExW(recovery_path().c_str(), target.c_str(), 0));
    if (!CreateSymbolicLinkW(recovery_path().c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
      const auto error = GetLastError();
      if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_ACCESS_DENIED) {
        GTEST_SKIP() << "Creating a symlink requires Developer Mode or the applicable privilege";
      }
      FAIL() << "Could not create directory symlink fixture, error " << error;
    }
    refuses(error_e::unsafe);
    ASSERT_TRUE(RemoveDirectoryW(recovery_path().c_str()));
  }

  TEST_F(RecoveryStorageTest, SharingFailureIsUnreadable) {
    const auto writer = open_directory(recovery_path(), FILE_LIST_DIRECTORY | DELETE);
    ASSERT_TRUE(writer);
    refuses(error_e::unreadable);
  }

  TEST_F(RecoveryStorageTest, InitialAndExplicitRechecksDetectStateChanges) {
    const auto original = manifest();
    const auto managed = manifest(mode_e::recovery_managed);
    const auto changed_during_scan = test::inspect(roots, [&] {
      ASSERT_EQ(write_manifest(managed), ERROR_SUCCESS);
    });
    ASSERT_FALSE(changed_during_scan);
    EXPECT_EQ(changed_during_scan.error(), error_e::changed);
    auto observed = test::inspect(roots);
    ASSERT_TRUE(observed) << static_cast<int>(observed.error());
    ASSERT_EQ(write_manifest(original), ERROR_SUCCESS);
    const auto recheck = observed->recheck();
    ASSERT_FALSE(recheck);
    EXPECT_EQ(recheck.error(), error_e::changed);

    auto clean = test::inspect(roots);
    ASSERT_TRUE(clean) << static_cast<int>(clean.error());
    const auto ancestor = open_directory(roots.program_data, WRITE_DAC | READ_CONTROL);
    ASSERT_TRUE(ancestor);
    const auto before = describe_security(ancestor.get(), SE_FILE_OBJECT);
    ASSERT_FALSE(before.empty());
    ASSERT_EQ(set_dacl(ancestor.get(), SE_FILE_OBJECT, directory_sddl(L"(A;;FR;;;BU)")), ERROR_SUCCESS);
    const auto after = describe_security(ancestor.get(), SE_FILE_OBJECT);
    ASSERT_FALSE(after.empty());
    ASSERT_TRUE(before != after);
    ASSERT_TRUE(test::inspect(roots));
    const auto protection_change = clean->recheck();
    ASSERT_FALSE(protection_change);
    EXPECT_EQ(protection_change.error(), error_e::changed);
  }

  TEST_F(RecoveryStorageTest, PendingBytesAndResidueSurviveObservationsAndRefusals) {
    const auto input = manifest(mode_e::recovery_managed);
    ASSERT_EQ(write_manifest(input), ERROR_SUCCESS);
    constexpr std::array<std::pair<const wchar_t *, const char *>, 3> material {
      std::pair {L"owned.state", "pending state"},
      std::pair {L"owned.state.tmp-private", "staged replacement"},
      std::pair {L"owned.state.backup-private", "retained prior state"}
    };
    for (const auto &[name, content] : material) {
      std::ofstream output {recovery_path() / name, std::ios::binary};
      ASSERT_TRUE(output << content);
    }
    const auto directory = open_directory(recovery_path());
    ASSERT_TRUE(directory);
    const auto original_id = directory_id(directory.get());
    const auto directory_acl = describe_security(directory.get(), SE_FILE_OBJECT);
    const auto key_acl = describe_security(recovery.get(), SE_REGISTRY_KEY);
    ASSERT_FALSE(directory_acl.empty());
    ASSERT_FALSE(key_acl.empty());

    const auto check_unchanged = [&](const auto &expected_manifest) {
      for (const auto &[name, content] : material) {
        EXPECT_EQ(read_file(recovery_path() / name), content);
      }
      EXPECT_EQ(std::distance(fs::directory_iterator {recovery_path()}, fs::directory_iterator {}), material.size());
      EXPECT_EQ(directory_id(directory.get()), original_id);
      EXPECT_EQ(describe_security(directory.get(), SE_FILE_OBJECT), directory_acl);
      EXPECT_EQ(describe_security(recovery.get(), SE_REGISTRY_KEY), key_acl);
      std::array<BYTE, 64> bytes {};
      DWORD length = bytes.size();
      DWORD type {};
      ASSERT_EQ(RegQueryValueExW(recovery.get(), L"Manifest", nullptr, &type, bytes.data(), &length), ERROR_SUCCESS);
      EXPECT_EQ(type, REG_BINARY);
      EXPECT_EQ(length, bytes.size());
      EXPECT_EQ(bytes, expected_manifest);
    };

    const auto observed = test::inspect(roots);
    ASSERT_TRUE(observed) << static_cast<int>(observed.error());
    EXPECT_TRUE(observed->recheck());
    check_unchanged(input);

    auto unsupported = input;
    unsupported[18] = 2;
    ASSERT_EQ(write_manifest(unsupported), ERROR_SUCCESS);
    refuses(error_e::unsafe);
    check_unchanged(unsupported);
  }
}  // namespace
#endif
