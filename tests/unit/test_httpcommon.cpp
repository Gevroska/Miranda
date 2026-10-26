/**
 * @file tests/unit/test_httpcommon.cpp
 * @brief Test src/httpcommon.*.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

// lib includes
#include <curl/curl.h>
#ifdef _WIN32
  #include <aclapi.h>
  #include <sddl.h>
  #include <Windows.h>
  #include <winioctl.h>
  #include <winternl.h>
#else
  #include <sys/stat.h>
  #include <unistd.h>
  #ifdef __linux__
    #include <sys/xattr.h>
  #endif
#endif

// local includes
#include <src/httpcommon.h>
#include <src/utility.h>

struct UrlEscapeTest: BaseTest, testing::WithParamInterface<std::tuple<std::string, std::string>> {};

TEST_P(UrlEscapeTest, Run) {
  const auto &[input, expected] = GetParam();
  ASSERT_EQ(http::url_escape(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlEscapeTests,
  UrlEscapeTest,
  testing::Values(
    std::make_tuple("igdb_0123456789", "igdb_0123456789"),
    std::make_tuple("../../../", "..%2F..%2F..%2F"),
    std::make_tuple("..*\\", "..%2A%5C")
  )
);

struct UrlGetHostTest: BaseTest, testing::WithParamInterface<std::tuple<std::string, std::string>> {};

TEST_P(UrlGetHostTest, Run) {
  const auto &[input, expected] = GetParam();
  ASSERT_EQ(http::url_get_host(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlGetHostTests,
  UrlGetHostTest,
  testing::Values(
    std::make_tuple("https://images.igdb.com/example.txt", "images.igdb.com"),
    std::make_tuple("http://localhost:8080", "localhost"),
    std::make_tuple("nonsense!!}{::", "")
  )
);

struct DownloadFileTest: BaseTest, testing::WithParamInterface<std::tuple<std::string, std::string>> {};

TEST_P(DownloadFileTest, Run) {
  const auto &[url, filename] = GetParam();
  const std::string test_dir = platf::appdata().string() + "/tests/";
  std::string path = test_dir + filename;
  ASSERT_TRUE(http::download_file(url, path, CURL_SSLVERSION_TLSv1_0));
}

#ifdef SUNSHINE_BUILD_FLATPAK
// requires running `npm run serve` prior to running the tests
constexpr const char *URL_1 = "http://0.0.0.0:3000/hello.txt";
constexpr const char *URL_2 = "http://0.0.0.0:3000/hello-redirect.txt";
#else
constexpr const char *URL_1 = "https://httpbin.org/base64/aGVsbG8h";
constexpr const char *URL_2 = "https://httpbin.org/redirect-to?url=/base64/aGVsbG8h";
#endif

INSTANTIATE_TEST_SUITE_P(
  DownloadFileTests,
  DownloadFileTest,
  testing::Values(
    std::make_tuple(URL_1, "hello.txt"),
    std::make_tuple(URL_2, "hello-redirect.txt")
  )
);

/**
 * @brief Keep security metadata checks inside a dedicated fixture directory.
 */
class StateFileTransactionTest: public BaseTest {
protected:
  std::filesystem::path directory {std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "state_transaction_fixture"};  ///< Fixture-only files.
  std::filesystem::path file {directory / "state.json"};  ///< Synthetic combined state/credentials file.

  void SetUp() override {
    BaseTest::SetUp();
    static std::size_t next_fixture = 0;
    directory = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / ("state_transaction_" + std::to_string(++next_fixture));
    file = directory / "state.json";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    ASSERT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
      tree.put("fixture", "original");
    }));
  }

  void TearDown() override {
    http::test_support::set_state_update_hook({});
    http::test_support::set_state_replace_hook({});
    http::test_support::set_state_security_hook({});
    http::test_support::set_state_attributes_hook({});
#ifdef _WIN32
    http::test_support::set_state_file_hook({});
#endif
    std::filesystem::remove_all(directory);
    BaseTest::TearDown();
  }
#ifdef _WIN32
  /** @brief Read complete fixture bytes through a checked handle without changing its lifetime. */
  std::string read_fixture_handle(HANDLE handle) {
    LARGE_INTEGER beginning {};
    if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN)) {
      ADD_FAILURE() << "Fixture read could not seek";
      return {};
    }
    std::string bytes;
    std::array<char, 4096> buffer;
    for (;;) {
      DWORD count = 0;
      if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
        ADD_FAILURE() << "Fixture read failed";
        return {};
      }
      if (!count) {
        return bytes;
      }
      bytes.append(buffer.data(), count);
    }
  }

  /** @brief Read fixture bytes while allowing a live transaction's read/delete guard. */
  std::string read_fixture_bytes(const std::filesystem::path &path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      ADD_FAILURE() << "Fixture read could not open";
      return {};
    }
    auto close = util::fail_guard([&]() {
      EXPECT_TRUE(CloseHandle(handle));
    });
    return read_fixture_handle(handle);
  }

  /**
   * @brief Read a complete owner, group and DACL snapshot from a fixture file.
   * @param path Test-owned file.
   * @return Self-relative descriptor, or an empty snapshot on failure.
   */
  std::vector<std::uint8_t> security_snapshot(const std::filesystem::path &path, DWORD information = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION) {
    auto name = path.wstring();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, information, nullptr, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS) {
      return {};
    }
    auto release = util::fail_guard([&]() {
      LocalFree(descriptor);
    });
    const auto length = GetSecurityDescriptorLength(descriptor);
    const auto *bytes = static_cast<const std::uint8_t *>(descriptor);
    return {bytes, bytes + length};
  }

  /** @brief Install only a selected synthetic authority component without requesting audit access. */
  bool set_selected_security(const std::filesystem::path &path, std::vector<std::uint8_t> descriptor, DWORD selector) {
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    if (descriptor.empty() || !GetSecurityDescriptorSacl(descriptor.data(), &present, &acl, &defaulted) || !present || !acl) {
      return false;
    }
    auto name = path.wstring();
    return SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, selector, nullptr, nullptr, nullptr, acl) == ERROR_SUCCESS;
  }

  /**
   * @brief Compare independently captured fixture snapshots through the production security gate.
   * @param expected Original descriptor.
   * @param current Descriptor under inspection.
   * @return Whether complete authority and every other control field is preserved.
   */
  bool same_security(std::vector<std::uint8_t> expected, std::vector<std::uint8_t> current) {
    return http::test_support::state_file_security_matches(std::move(expected), std::move(current));
  }

  /**
   * @brief Describe fixture authority differences without disclosing SIDs or descriptor bytes.
   * @param expected Reference fixture snapshot.
   * @param current Snapshot under inspection.
   * @return Bounded component comparisons suitable for a failed assertion.
   */
  std::string security_difference(std::vector<std::uint8_t> expected, std::vector<std::uint8_t> current) {
    testing::Message details;
    const auto valid = [](auto &snapshot) {
      return snapshot.size() >= sizeof(SECURITY_DESCRIPTOR_RELATIVE) && IsValidSecurityDescriptor(snapshot.data()) && GetSecurityDescriptorLength(snapshot.data()) <= snapshot.size();
    };
    if (!valid(expected) || !valid(current)) {
      details << "valid expected/current=" << valid(expected) << '/' << valid(current);
      return details.GetString();
    }
    SECURITY_DESCRIPTOR_CONTROL expected_control {}, current_control {};
    DWORD expected_revision = 0, current_revision = 0;
    GetSecurityDescriptorControl(expected.data(), &expected_control, &expected_revision);
    GetSecurityDescriptorControl(current.data(), &current_control, &current_revision);
    details << "control expected/current=" << expected_control << '/' << current_control
            << "; revision=" << expected_revision << '/' << current_revision;
    const auto describe_sid = [&](const char *label, auto getter) {
      PSID expected_sid = nullptr, current_sid = nullptr;
      BOOL expected_defaulted = FALSE, current_defaulted = FALSE;
      const bool expected_read = getter(expected.data(), &expected_sid, &expected_defaulted);
      const bool current_read = getter(current.data(), &current_sid, &current_defaulted);
      const bool equal = !expected_sid || !current_sid ? expected_sid == current_sid :
                                                         IsValidSid(expected_sid) && IsValidSid(current_sid) && EqualSid(expected_sid, current_sid);
      details << "; " << label << " read=" << expected_read << '/' << current_read
              << " defaulted=" << expected_defaulted << '/' << current_defaulted << " SID_equal=" << equal;
    };
    describe_sid("owner", GetSecurityDescriptorOwner);
    describe_sid("group", GetSecurityDescriptorGroup);
    PACL expected_acl = nullptr, current_acl = nullptr;
    BOOL expected_present = FALSE, current_present = FALSE, expected_defaulted = FALSE, current_defaulted = FALSE;
    const bool expected_read = GetSecurityDescriptorDacl(expected.data(), &expected_present, &expected_acl, &expected_defaulted);
    const bool current_read = GetSecurityDescriptorDacl(current.data(), &current_present, &current_acl, &current_defaulted);
    details << "; DACL read=" << expected_read << '/' << current_read
            << " present=" << expected_present << '/' << current_present
            << " defaulted=" << expected_defaulted << '/' << current_defaulted
            << " nonnull=" << (expected_acl != nullptr) << '/' << (current_acl != nullptr);
    if (!expected_acl || !current_acl || !IsValidAcl(expected_acl) || !IsValidAcl(current_acl)) {
      return details.GetString();
    }
    details << " revision=" << static_cast<unsigned>(expected_acl->AclRevision) << '/' << static_cast<unsigned>(current_acl->AclRevision)
            << " ACE_count=" << expected_acl->AceCount << '/' << current_acl->AceCount;
    const auto count = std::min<DWORD>({expected_acl->AceCount, current_acl->AceCount, 16});
    for (DWORD index = 0; index < count; ++index) {
      void *expected_ace = nullptr, *current_ace = nullptr;
      if (!GetAce(expected_acl, index, &expected_ace) || !GetAce(current_acl, index, &current_ace)) {
        details << "; ACE[" << index << "] unreadable";
        continue;
      }
      const auto *left = static_cast<ACE_HEADER *>(expected_ace), *right = static_cast<ACE_HEADER *>(current_ace);
      details << "; ACE[" << index << "] type=" << static_cast<unsigned>(left->AceType) << '/' << static_cast<unsigned>(right->AceType)
              << " flags=" << static_cast<unsigned>(left->AceFlags) << '/' << static_cast<unsigned>(right->AceFlags)
              << " size=" << left->AceSize << '/' << right->AceSize;
      constexpr auto prefix_size = sizeof(ACE_HEADER) + sizeof(ACCESS_MASK);
      if (left->AceSize >= prefix_size && right->AceSize >= prefix_size) {
        ACCESS_MASK left_mask = 0, right_mask = 0;
        const auto *left_bytes = static_cast<const std::uint8_t *>(expected_ace), *right_bytes = static_cast<const std::uint8_t *>(current_ace);
        std::memcpy(&left_mask, left_bytes + sizeof(ACE_HEADER), sizeof(left_mask));
        std::memcpy(&right_mask, right_bytes + sizeof(ACE_HEADER), sizeof(right_mask));
        details << " mask=" << left_mask << '/' << right_mask
                << " remaining_bytes_equal=" << (left->AceSize == right->AceSize && !std::memcmp(left_bytes + prefix_size, right_bytes + prefix_size, left->AceSize - prefix_size));
      }
    }
    if (expected_acl->AceCount > count || current_acl->AceCount > count) {
      details << "; additional ACEs omitted";
    }
    return details.GetString();
  }

  /**
   * @brief Create an in-memory descriptor for comparison regressions without modifying file security.
   * @param text Synthetic descriptor using only well-known SIDs.
   * @return Self-relative descriptor, or an empty snapshot on failure.
   */
  std::vector<std::uint8_t> security_from_sddl(const wchar_t *text) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(text, SDDL_REVISION_1, &descriptor, nullptr)) {
      return {};
    }
    const auto size = GetSecurityDescriptorLength(descriptor);
    const auto *bytes = static_cast<const std::uint8_t *>(descriptor);
    std::vector<std::uint8_t> snapshot(bytes, bytes + size);
    LocalFree(descriptor);
    return snapshot;
  }

  /**
   * @brief Read fixture security metadata without displaying it in failure output.
   * @param path Test-owned file.
   * @return Canonical owner, group and DACL for equality checks.
   */
  std::wstring security(const std::filesystem::path &path) {
    constexpr DWORD information = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    auto descriptor = security_snapshot(path);
    if (descriptor.empty()) {
      return {};
    }
    LPWSTR text = nullptr;
    if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor.data(), SDDL_REVISION_1, information, &text, nullptr)) {
      return {};
    }
    const std::wstring value {text};
    LocalFree(text);
    return value;
  }

  /**
   * @brief Check true inheritance policy without relying on the legacy descriptor view.
   * @param descriptor Independently captured modern snapshot.
   * @return Whether a valid descriptor permits inheritance.
   */
  bool unprotected(std::vector<std::uint8_t> descriptor) {
    if (descriptor.empty() || !IsValidSecurityDescriptor(descriptor.data())) {
      return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control {};
    DWORD revision = 0;
    return GetSecurityDescriptorControl(descriptor.data(), &control, &revision) && !(control & SE_DACL_PROTECTED);
  }

  /**
   * @brief Observe only the synthetic inherited read grant used by the parent-revocation fixture.
   * @param descriptor Independently captured modern snapshot.
   * @return Whether the known fixture group retains an inherited read ACE.
   */
  bool inherited_read(std::vector<std::uint8_t> descriptor) {
    if (descriptor.empty() || !IsValidSecurityDescriptor(descriptor.data())) {
      return false;
    }
    std::array<BYTE, SECURITY_MAX_SID_SIZE> group_sid {};
    DWORD length = static_cast<DWORD>(group_sid.size());
    if (!CreateWellKnownSid(WinBuiltinUsersSid, nullptr, group_sid.data(), &length)) {
      return false;
    }
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(descriptor.data(), &present, &acl, &defaulted) || !present || !acl) {
      return false;
    }
    for (DWORD index = 0; index < acl->AceCount; ++index) {
      void *raw = nullptr;
      if (!GetAce(acl, index, &raw)) {
        return false;
      }
      const auto *header = static_cast<ACE_HEADER *>(raw);
      if (header->AceType == ACCESS_ALLOWED_ACE_TYPE && (header->AceFlags & INHERITED_ACE)) {
        auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(raw);
        if (EqualSid(&ace->SidStart, group_sid.data()) && (ace->Mask & FILE_READ_DATA)) {
          return true;
        }
      }
    }
    return false;
  }

  /**
   * @brief Verify successive real saves preserve the current policy before bytes and after replacement.
   * @param saves Number of additional successful transactions to exercise.
   */
  void expect_repeated_saves_preserve_security(unsigned saves) {
    for (unsigned iteration = 0; iteration < saves; ++iteration) {
      const auto original = security_snapshot(file);
      ASSERT_TRUE(same_security(original, original));
      const auto original_file_count = std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator());
      bool saw_restored = false, saw_prebytes = false;
      http::test_support::set_state_security_hook([&](auto stage, const auto &temporary) {
        if (stage == http::test_support::state_security_stage_e::verify) {
          EXPECT_EQ(std::filesystem::file_size(temporary), 0U);
          EXPECT_TRUE(same_security(original, security_snapshot(temporary)));
          saw_restored = true;
        }
      });
      http::test_support::set_state_update_hook([&]() {
        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
          if (entry.path().filename().string().starts_with(file.filename().string() + ".tmp-")) {
            EXPECT_EQ(std::filesystem::file_size(entry.path()), 0U);
            EXPECT_TRUE(same_security(original, security_snapshot(entry.path())));
            saw_prebytes = true;
          }
        }
      });
      EXPECT_TRUE(http::update_state_file(file.string(), [&](auto &tree) {
        tree.put("repeated_save", iteration);
      }));
      http::test_support::set_state_security_hook({});
      http::test_support::set_state_update_hook({});
      EXPECT_TRUE(saw_restored && saw_prebytes);
      EXPECT_TRUE(same_security(original, security_snapshot(file)));
      EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), original_file_count);
    }
  }
#endif

  /**
   * @brief Read synthetic JSON without printing state contents in assertion diagnostics.
   * @return Fixture bytes.
   */
  std::string state_bytes() {
#ifdef _WIN32
    return read_fixture_bytes(file);
#else
    std::ifstream input {file};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
#endif
  }
};

TEST_F(StateFileTransactionTest, RestrictsEmptyTemporaryBeforeWritingAndPreservesSecurityMetadata) {
#ifdef _WIN32
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr));
  const bool restricted = SetFileSecurityW(file.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
  LocalFree(descriptor);
  ASSERT_TRUE(restricted);
  const auto original = security_snapshot(file);
  ASSERT_FALSE(original.empty());
  ASSERT_TRUE(same_security(original, original));
#else
  std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  const auto original = std::filesystem::status(file).permissions();
  struct stat original_metadata {};
  ASSERT_EQ(::stat(file.c_str(), &original_metadata), 0);
#endif
  bool inspected = false;
  http::test_support::set_state_update_hook([&]() {
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
      if (entry.path().filename().string().starts_with("state.json.tmp-")) {
        inspected = true;
        EXPECT_EQ(std::filesystem::file_size(entry.path()), 0U);
#ifdef _WIN32
        EXPECT_TRUE(same_security(original, security_snapshot(entry.path())));
#else
        EXPECT_EQ(std::filesystem::status(entry.path()).permissions(), original);
        struct stat temporary_metadata {};
        ASSERT_EQ(::stat(entry.path().c_str(), &temporary_metadata), 0);
        EXPECT_EQ(temporary_metadata.st_uid, original_metadata.st_uid);
        EXPECT_EQ(temporary_metadata.st_gid, original_metadata.st_gid);
#endif
      }
    }
  });
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "replacement");
  }));
  http::test_support::set_state_update_hook({});
  EXPECT_TRUE(inspected);
#ifdef _WIN32
  EXPECT_TRUE(same_security(original, security_snapshot(file)));
#else
  EXPECT_EQ(std::filesystem::status(file).permissions(), original);
  struct stat replacement_metadata {};
  ASSERT_EQ(::stat(file.c_str(), &replacement_metadata), 0);
  EXPECT_EQ(replacement_metadata.st_uid, original_metadata.st_uid);
  EXPECT_EQ(replacement_metadata.st_gid, original_metadata.st_gid);
#endif
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
#ifdef _WIN32
  expect_repeated_saves_preserve_security(2);
#endif
}

#ifdef _WIN32
TEST_F(StateFileTransactionTest, SecurityComparisonRejectsEveryAuthorityOrControlChange) {
  const auto original = security_from_sddl(L"O:SYG:BAD:P(A;;FA;;;OW)");
  const auto marked = security_from_sddl(L"O:SYG:BAD:PAI(A;;FA;;;OW)");
  ASSERT_FALSE(original.empty());
  ASSERT_FALSE(marked.empty());
  EXPECT_TRUE(same_security(original, original));
  EXPECT_FALSE(same_security(original, marked));
  EXPECT_FALSE(same_security(marked, original));

  for (const auto text : {
         L"O:SYG:BAD:PAI(A;;FA;;;OW)(A;;FR;;;WD)",
         L"O:SYG:BAD:PAI(A;;FR;;;OW)",
         L"O:BAG:BAD:PAI(A;;FA;;;OW)",
         L"O:SYG:SYD:PAI(A;;FA;;;OW)",
         L"O:SYG:BAD:AI(A;;FA;;;OW)",
         L"O:SYG:BAD:PARAI(A;;FA;;;OW)",
         L"O:SYG:BAD:PAINO_ACCESS_CONTROL",
         L"O:SYG:BAD:PAI",
         L"O:SYG:BA",
       }) {
    const auto changed = security_from_sddl(text);
    ASSERT_FALSE(changed.empty());
    EXPECT_FALSE(same_security(original, changed));
  }
  const auto ordered = security_from_sddl(L"O:SYG:BAD:P(A;;FA;;;OW)(A;;FR;;;SY)");
  const auto reordered = security_from_sddl(L"O:SYG:BAD:PAI(A;;FR;;;SY)(A;;FA;;;OW)");
  ASSERT_FALSE(ordered.empty());
  ASSERT_FALSE(reordered.empty());
  EXPECT_FALSE(same_security(ordered, reordered));
  const auto unprotected = security_from_sddl(L"O:SYG:BAD:(A;;FA;;;OW)");
  const auto unprotected_marked = security_from_sddl(L"O:SYG:BAD:AI(A;;FA;;;OW)");
  ASSERT_FALSE(unprotected.empty());
  ASSERT_FALSE(unprotected_marked.empty());
  EXPECT_FALSE(same_security(unprotected, unprotected_marked));
  EXPECT_FALSE(same_security(unprotected_marked, unprotected));
  const auto null_dacl = security_from_sddl(L"O:SYG:BAD:PNO_ACCESS_CONTROL");
  const auto empty_dacl = security_from_sddl(L"O:SYG:BAD:PAI");
  ASSERT_FALSE(null_dacl.empty());
  ASSERT_FALSE(empty_dacl.empty());
  EXPECT_FALSE(same_security(null_dacl, empty_dacl));
  EXPECT_FALSE(same_security({}, {}));
  EXPECT_FALSE(same_security(original, {}));
  EXPECT_FALSE(same_security(original, std::vector<std::uint8_t>(sizeof(SECURITY_DESCRIPTOR_RELATIVE))));
}

TEST_F(StateFileTransactionTest, PreservesInheritedAuthorityAndLaterParentRevocationAgainstUntouchedSibling) {
  auto parent = security_from_sddl(L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)");
  ASSERT_FALSE(parent.empty());
  ASSERT_TRUE(SetFileSecurityW(directory.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, parent.data()));
  ASSERT_TRUE(std::filesystem::remove(file));
  const auto sibling = directory / "untouched.json";
  for (const auto &path : {file, sibling}) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    auto close = util::fail_guard([&]() {
      CloseHandle(handle);
    });
    const std::string bytes = "{\"fixture\":\"original\"}\n";
    DWORD written = 0;
    ASSERT_TRUE(WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
    ASSERT_EQ(written, bytes.size());
    ASSERT_TRUE(FlushFileBuffers(handle));
  }
  const auto original = security_snapshot(file);
  const auto baseline = security_snapshot(sibling);
  ASSERT_TRUE(same_security(original, original));
  ASSERT_TRUE(unprotected(original));
  ASSERT_TRUE(inherited_read(original));
  ASSERT_TRUE(same_security(original, baseline));
  bool saw_creation = false, saw_restored = false, saw_prebytes = false;
  http::test_support::set_state_security_hook([&](auto stage, const auto &temporary) {
    EXPECT_EQ(std::filesystem::file_size(temporary), 0U);
    if (stage == http::test_support::state_security_stage_e::restore) {
      auto protected_creation = original;
      ASSERT_TRUE(SetSecurityDescriptorControl(protected_creation.data(), SE_DACL_PROTECTED, SE_DACL_PROTECTED));
      EXPECT_TRUE(same_security(protected_creation, security_snapshot(temporary)));
      saw_creation = true;
    } else {
      EXPECT_TRUE(same_security(original, security_snapshot(temporary)));
      saw_restored = true;
    }
  });
  http::test_support::set_state_update_hook([&]() {
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
      if (entry.path().filename().string().starts_with("state.json.tmp-")) {
        EXPECT_EQ(std::filesystem::file_size(entry.path()), 0U);
        EXPECT_TRUE(same_security(original, security_snapshot(entry.path())));
        saw_prebytes = true;
      }
    }
  });
  std::vector<std::uint8_t> destination_before_replace, temporary_before_replace;
  http::test_support::set_state_replace_hook([&](const auto &destination, const auto &temporary, const auto &) -> std::optional<std::uint32_t> {
    destination_before_replace = security_snapshot(destination);
    temporary_before_replace = security_snapshot(temporary);
    return std::nullopt;
  });
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "replacement");
  }));
  http::test_support::set_state_security_hook({});
  http::test_support::set_state_update_hook({});
  http::test_support::set_state_replace_hook({});
  EXPECT_TRUE(saw_creation && saw_restored && saw_prebytes);
  const auto replaced = security_snapshot(file);
  EXPECT_TRUE(same_security(original, replaced))
    << security_difference(original, replaced)
    << "\nBefore replacement destination: " << security_difference(original, destination_before_replace)
    << "\nBefore replacement temporary: " << security_difference(original, temporary_before_replace);
  EXPECT_TRUE(unprotected(replaced));
  EXPECT_TRUE(inherited_read(replaced));
  EXPECT_TRUE(same_security(baseline, security_snapshot(sibling)));
  EXPECT_TRUE(state_bytes().contains("replacement"));
  expect_repeated_saves_preserve_security(2);
  EXPECT_TRUE(unprotected(security_snapshot(file)));
  EXPECT_TRUE(inherited_read(security_snapshot(file)));
  EXPECT_TRUE(same_security(baseline, security_snapshot(sibling)));

  auto reduced_parent = security_from_sddl(L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
  ASSERT_FALSE(reduced_parent.empty());
  PACL acl = nullptr;
  BOOL present = FALSE, defaulted = FALSE;
  ASSERT_TRUE(GetSecurityDescriptorDacl(reduced_parent.data(), &present, &acl, &defaulted));
  ASSERT_TRUE(present && acl);
  auto name = directory.wstring();
  ASSERT_EQ(SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr), ERROR_SUCCESS);
  const auto revoked = security_snapshot(file);
  const auto untouched_revoked = security_snapshot(sibling);
  EXPECT_TRUE(same_security(revoked, untouched_revoked)) << security_difference(revoked, untouched_revoked);
  EXPECT_TRUE(unprotected(revoked) && unprotected(untouched_revoked));
  EXPECT_FALSE(inherited_read(revoked));
  EXPECT_FALSE(inherited_read(untouched_revoked));
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "saved after parent revocation");
  }));
  EXPECT_TRUE(same_security(revoked, security_snapshot(file)));
  expect_repeated_saves_preserve_security(2);
  EXPECT_TRUE(same_security(untouched_revoked, security_snapshot(file))) << security_difference(untouched_revoked, security_snapshot(file));
  EXPECT_TRUE(unprotected(security_snapshot(file)));
  EXPECT_FALSE(inherited_read(security_snapshot(file)));
}

TEST_F(StateFileTransactionTest, PreservesUnprotectedExplicitAuthorityWhileTemporaryIsEmpty) {
  auto parent = security_from_sddl(L"D:P(A;;FA;;;OW)(A;;FA;;;SY)(A;;FA;;;BA)");
  auto explicit_policy = security_from_sddl(L"D:(A;;FA;;;OW)");
  ASSERT_FALSE(parent.empty());
  ASSERT_FALSE(explicit_policy.empty());
  ASSERT_TRUE(SetFileSecurityW(directory.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, parent.data()));
  ASSERT_TRUE(std::filesystem::remove(file));
  SECURITY_ATTRIBUTES attributes {sizeof(SECURITY_ATTRIBUTES), explicit_policy.data(), FALSE};
  HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  auto close = util::fail_guard([&]() {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  });
  const std::string bytes = "{\"fixture\":\"original\"}\n";
  DWORD written = 0;
  ASSERT_TRUE(WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
  ASSERT_EQ(written, bytes.size());
  ASSERT_TRUE(CloseHandle(handle));
  handle = INVALID_HANDLE_VALUE;
  const auto original = security_snapshot(file);
  ASSERT_TRUE(same_security(original, original));
  ASSERT_TRUE(unprotected(original));
  bool inspected = false;
  http::test_support::set_state_security_hook([&](auto stage, const auto &temporary) {
    if (stage == http::test_support::state_security_stage_e::verify) {
      EXPECT_EQ(std::filesystem::file_size(temporary), 0U);
      EXPECT_TRUE(same_security(original, security_snapshot(temporary)));
      inspected = true;
    }
  });
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "replacement");
  }));
  http::test_support::set_state_security_hook({});
  EXPECT_TRUE(inspected);
  EXPECT_TRUE(same_security(original, security_snapshot(file)));
  EXPECT_TRUE(unprotected(security_snapshot(file)));
  expect_repeated_saves_preserve_security(2);
  EXPECT_TRUE(unprotected(security_snapshot(file)));
}

TEST_F(StateFileTransactionTest, FailedEmptyTemporarySecuritySetOrVerificationPreservesOriginalAndRecovers) {
  for (const auto rejected_stage : {http::test_support::state_security_stage_e::restore, http::test_support::state_security_stage_e::verify}) {
    const auto original = security_snapshot(file);
    const auto original_bytes = state_bytes();
    ASSERT_TRUE(same_security(original, original));
    bool faulted = false, reached_bytes = false;
    http::test_support::set_state_security_hook([&](auto stage, const auto &temporary) {
      if (stage != rejected_stage) {
        return;
      }
      EXPECT_EQ(std::filesystem::file_size(temporary), 0U);
      auto changed = security_from_sddl(stage == http::test_support::state_security_stage_e::restore ? L"D:P(D;;WD;;;OW)(A;;FA;;;OW)" : L"D:P(A;;FA;;;OW)(A;;FR;;;BU)");
      ASSERT_FALSE(changed.empty());
      ASSERT_TRUE(SetFileSecurityW(temporary.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, changed.data()));
      faulted = true;
    });
    http::test_support::set_state_update_hook([&]() {
      reached_bytes = true;
    });
    EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
      tree.put("fixture", "must remain unpublished");
    }));
    http::test_support::set_state_security_hook({});
    http::test_support::set_state_update_hook({});
    EXPECT_TRUE(faulted);
    EXPECT_FALSE(reached_bytes);
    EXPECT_TRUE(state_bytes() == original_bytes);
    EXPECT_TRUE(same_security(original, security_snapshot(file)));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
    bool saw_recovered_policy = false;
    http::test_support::set_state_security_hook([&](auto stage, const auto &temporary) {
      if (stage == http::test_support::state_security_stage_e::verify) {
        EXPECT_EQ(std::filesystem::file_size(temporary), 0U);
        EXPECT_TRUE(same_security(original, security_snapshot(temporary)));
        saw_recovered_policy = true;
      }
    });
    EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
      tree.put("fixture", "recovered");
    }));
    http::test_support::set_state_security_hook({});
    EXPECT_TRUE(saw_recovered_policy);
    EXPECT_TRUE(same_security(original, security_snapshot(file)));
    expect_repeated_saves_preserve_security(1);
  }
}

TEST_F(StateFileTransactionTest, EncryptedOrUnreadableAttributesRefuseBeforeTemporaryCreationAndRecover) {
  const auto original = security_snapshot(file);
  const auto original_bytes = state_bytes();
  const std::array<DWORD, 2> refused_attributes {FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_ARCHIVE, INVALID_FILE_ATTRIBUTES};
  for (const auto attributes : refused_attributes) {
    bool reached_temporary = false, reached_bytes = false;
    http::test_support::set_state_attributes_hook([&](const auto &) -> std::optional<std::uint32_t> {
      return attributes;
    });
    http::test_support::set_state_security_hook([&](auto, const auto &) {
      reached_temporary = true;
    });
    http::test_support::set_state_update_hook([&]() {
      reached_bytes = true;
    });
    EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
      tree.put("fixture", "must remain unpublished");
    }));
    EXPECT_FALSE(reached_temporary || reached_bytes);
    EXPECT_TRUE(state_bytes() == original_bytes);
    EXPECT_TRUE(same_security(original, security_snapshot(file)));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
  }
  http::test_support::set_state_attributes_hook({});
  http::test_support::set_state_security_hook({});
  http::test_support::set_state_update_hook({});
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "recovered");
  }));
}

TEST_F(StateFileTransactionTest, ActualSharingDeniedReplacementLeavesOriginalAndRecovers) {
  const auto original = security_snapshot(file);
  const auto original_bytes = state_bytes();
  HANDLE reader = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(reader, INVALID_HANDLE_VALUE);
  auto close = util::fail_guard([&]() {
    if (reader != INVALID_HANDLE_VALUE) {
      CloseHandle(reader);
    }
  });
  bool mutated = false;
  EXPECT_FALSE(http::update_state_file(file.string(), [&](auto &tree) {
    mutated = true;
    tree.put("fixture", "must remain unpublished");
  }));
  EXPECT_FALSE(mutated);
  EXPECT_TRUE(state_bytes() == original_bytes);
  EXPECT_TRUE(same_security(original, security_snapshot(file)));
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
  ASSERT_TRUE(CloseHandle(reader));
  reader = INVALID_HANDLE_VALUE;
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "recovered");
  }));
}

TEST_F(StateFileTransactionTest, CompatibleReaderKeepsOldBytesWhileThePathPublishesCompleteNewBytes) {
  using stage_t = http::test_support::state_file_stage_e;
  const auto original = state_bytes();
  const auto policy = security_snapshot(file);
  HANDLE reader = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(reader, INVALID_HANDLE_VALUE);
  auto close = util::fail_guard([&]() {
    EXPECT_TRUE(CloseHandle(reader));
    http::test_support::set_state_file_hook({});
  });
  std::string prepared_bytes;
  bool verified = false;
  http::test_support::set_state_file_hook([&](auto stage, const auto &destination, const auto &source, const auto &) -> std::optional<std::uint32_t> {
    if (stage == stage_t::rename_api) {
      prepared_bytes = read_fixture_bytes(source);
      EXPECT_TRUE(prepared_bytes.contains("complete replacement"));
      EXPECT_TRUE(read_fixture_handle(reader) == original);
    }
    if (stage == stage_t::verify) {
      verified = true;
      EXPECT_TRUE(read_fixture_handle(reader) == original);
      EXPECT_TRUE(read_fixture_bytes(destination) == prepared_bytes);
    }
    return std::nullopt;
  });
  ASSERT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "complete replacement");
  }));
  http::test_support::set_state_file_hook({});
  EXPECT_TRUE(verified);
  EXPECT_TRUE(read_fixture_handle(reader) == original);
  EXPECT_TRUE(state_bytes() == prepared_bytes);
  EXPECT_TRUE(same_security(policy, security_snapshot(file)));
}

TEST_F(StateFileTransactionTest, OriginalAndBackupAliasesExcludeDataWritersThroughCommitAndRestore) {
  using stage_t = http::test_support::state_file_stage_e;
  const auto original = state_bytes();
  const auto policy = security_snapshot(file);
  const auto alias = directory / "original-alias.json";
  ASSERT_TRUE(CreateHardLinkW(alias.c_str(), file.c_str(), nullptr));
  const auto assert_writer_denied = [&](const auto &path) {
    HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto open_error = writer == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    EXPECT_EQ(writer, INVALID_HANDLE_VALUE);
    EXPECT_EQ(open_error, ERROR_SHARING_VIOLATION);
    if (writer != INVALID_HANDLE_VALUE) {
      EXPECT_TRUE(CloseHandle(writer));
    }
  };
  for (const auto &path : {file, alias}) {
    HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(writer, INVALID_HANDLE_VALUE);
    auto close = util::fail_guard([&]() {
      EXPECT_TRUE(CloseHandle(writer));
    });
    bool mutated = false;
    EXPECT_FALSE(http::update_state_file(file.string(), [&](auto &tree) {
      mutated = true;
      tree.put("fixture", "must remain unpublished");
    }));
    EXPECT_FALSE(mutated);
    EXPECT_TRUE(state_bytes() == original);
  }
  unsigned rename_sources = 0;
  bool saw_restore = false;
  auto clear = util::fail_guard([]() {
    http::test_support::set_state_file_hook({});
  });
  http::test_support::set_state_file_hook([&](auto stage, const auto &destination, const auto &source, const auto &backup) -> std::optional<std::uint32_t> {
    if (stage == stage_t::rename_api || stage == stage_t::restore) {
      assert_writer_denied(alias);
      assert_writer_denied(backup);
    }
    if (stage == stage_t::rename_api) {
      ++rename_sources;
      assert_writer_denied(source);
      if (source != backup) {
        assert_writer_denied(destination);
      }
    }
    if (stage == stage_t::restore) {
      saw_restore = true;
    }
    return stage == stage_t::verify ? std::optional<std::uint32_t> {ERROR_INVALID_DATA} : std::nullopt;
  });
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "rejected replacement");
  }));
  http::test_support::set_state_file_hook({});
  EXPECT_EQ(rename_sources, 2U);
  EXPECT_TRUE(saw_restore);
  EXPECT_TRUE(state_bytes() == original);
  EXPECT_TRUE(read_fixture_bytes(alias) == original);
  EXPECT_TRUE(same_security(policy, security_snapshot(file)));
  ASSERT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "recovered after guarded rollback");
  }));
  EXPECT_TRUE(read_fixture_bytes(alias) == original);
}

TEST_F(StateFileTransactionTest, NativeStageFailuresPreserveOrRestoreOriginalAndRecover) {
  using stage_t = http::test_support::state_file_stage_e;
  for (const auto rejected : {stage_t::backup, stage_t::write, stage_t::flush, stage_t::close, stage_t::rename, stage_t::rename_open, stage_t::rename_api, stage_t::rename_close, stage_t::verify}) {
    SCOPED_TRACE(static_cast<int>(rejected));
    const auto original = security_snapshot(file);
    const auto original_bytes = state_bytes();
    bool reached = false;
    http::test_support::set_state_file_hook([&](auto stage, const auto &, const auto &, const auto &) -> std::optional<std::uint32_t> {
      if (stage == rejected && !reached) {
        reached = true;
        return ERROR_WRITE_FAULT;
      }
      return std::nullopt;
    });
    EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
      tree.put("fixture", "unpublished");
    }));
    http::test_support::set_state_file_hook({});
    EXPECT_TRUE(reached);
    EXPECT_TRUE(state_bytes() == original_bytes);
    EXPECT_TRUE(same_security(original, security_snapshot(file)));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
    EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
      tree.put("fixture", "recovered update");
    }));
  }
}

TEST_F(StateFileTransactionTest, RollbackRenameFailuresRetainKnownMaterialAndBlockFurtherUpdates) {
  using stage_t = http::test_support::state_file_stage_e;
  auto clear = util::fail_guard([]() {
    http::test_support::set_state_file_hook({});
  });
  for (const auto rejected : {stage_t::rename_open, stage_t::rename_api, stage_t::rename_close}) {
    SCOPED_TRACE(static_cast<int>(rejected));
    const auto destination = directory / ("rollback-" + std::to_string(static_cast<int>(rejected)) + ".json");
    ASSERT_TRUE(http::update_state_file(destination.string(), [](auto &tree) {
      tree.put("fixture", "original rollback state");
    }));
    const auto original = read_fixture_bytes(destination);
    const auto policy = security_snapshot(destination);
    std::filesystem::path backup;
    bool restoring = false, reached = false;
    http::test_support::set_state_file_hook([&](auto stage, const auto &, const auto &, const auto &old_backup) -> std::optional<std::uint32_t> {
      if (stage == stage_t::restore) {
        restoring = true;
        backup = old_backup;
      }
      if (restoring && stage == rejected) {
        reached = true;
        return ERROR_INVALID_HANDLE;
      }
      return stage == stage_t::verify ? std::optional<std::uint32_t> {ERROR_INVALID_DATA} : std::nullopt;
    });
    EXPECT_FALSE(http::update_state_file(destination.string(), [](auto &tree) {
      tree.put("fixture", "unpublished rollback state");
    }));
    http::test_support::set_state_file_hook({});
    ASSERT_TRUE(restoring && reached);
    if (rejected == stage_t::rename_close) {
      // The real restore completed, but failed checked close must still refuse publication and block.
      EXPECT_TRUE(read_fixture_bytes(destination) == original);
      EXPECT_TRUE(same_security(policy, security_snapshot(destination)));
    } else {
      ASSERT_FALSE(backup.empty());
      EXPECT_TRUE(read_fixture_bytes(backup) == original);
      EXPECT_TRUE(same_security(policy, security_snapshot(backup)));
      ASSERT_TRUE(MoveFileExW(backup.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    }
    EXPECT_TRUE(read_fixture_bytes(destination) == original);
    EXPECT_TRUE(same_security(policy, security_snapshot(destination)));
    EXPECT_FALSE(http::update_state_file(destination.string(), [](auto &tree) {
      tree.put("fixture", "must remain blocked");
    }));
    EXPECT_TRUE(read_fixture_bytes(destination) == original);
  }
}

TEST_F(StateFileTransactionTest, InitialAbsenceFailuresRestoreAbsenceOrPreserveUnrelatedReplacementAndBlock) {
  using stage_t = http::test_support::state_file_stage_e;
  const auto original_bytes = state_bytes();
  const auto original_security = security_snapshot(file);
  const auto read_bytes = [&](const auto &path) {
    return read_fixture_bytes(path);
  };
  auto clear_hook = util::fail_guard([]() {
    http::test_support::set_state_file_hook({});
  });
  for (const auto rejected : {stage_t::write, stage_t::flush, stage_t::close, stage_t::rename, stage_t::rename_open, stage_t::rename_api, stage_t::rename_close, stage_t::verify}) {
    SCOPED_TRACE(static_cast<int>(rejected));
    const auto destination = directory / ("initial-" + std::to_string(static_cast<int>(rejected)) + ".json");
    ASSERT_FALSE(std::filesystem::exists(destination));
    const auto original_count = std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator());
    bool reached = false, saw_committed_candidate = false;
    http::test_support::set_state_file_hook([&](auto stage, const auto &target, const auto &, const auto &) -> std::optional<std::uint32_t> {
      if (stage == rejected) {
        reached = true;
        EXPECT_TRUE(target == destination);
        if (stage == stage_t::verify) {
          saw_committed_candidate = std::filesystem::exists(target) && read_bytes(target).contains("unpublished initial state");
        }
        return ERROR_WRITE_FAULT;
      }
      return std::nullopt;
    });
    EXPECT_FALSE(http::update_state_file(destination.string(), [](auto &tree) {
      tree.put("fixture", "unpublished initial state");
    }));
    http::test_support::set_state_file_hook({});
    EXPECT_TRUE(reached);
    if (rejected == stage_t::verify) {
      EXPECT_TRUE(saw_committed_candidate);
    }
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), original_count);
    EXPECT_TRUE(state_bytes() == original_bytes);
    EXPECT_TRUE(same_security(original_security, security_snapshot(file)));
    ASSERT_TRUE(http::update_state_file(destination.string(), [](auto &tree) {
      tree.put("fixture", "recovered initial state");
    }));
    EXPECT_TRUE(read_bytes(destination).contains("recovered initial state"));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), original_count + 1);
  }

  const auto destination = directory / "initial-uncertain.json";
  const auto external = directory / "initial-external.json";
  ASSERT_FALSE(std::filesystem::exists(destination));
  {
    std::ofstream output {external};
    output << "{\"fixture\":\"unrelated initial replacement\"}\n";
    ASSERT_TRUE(output.good());
  }
  const auto external_bytes = read_bytes(external);
  const auto external_security = security_snapshot(external);
  ASSERT_TRUE(same_security(external_security, external_security));
  const auto original_count = std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator());
  bool saw_committed_candidate = false, substituted = false;
  http::test_support::set_state_file_hook([&](auto stage, const auto &target, const auto &, const auto &) -> std::optional<std::uint32_t> {
    if (stage == stage_t::verify) {
      EXPECT_TRUE(target == destination);
      saw_committed_candidate = std::filesystem::exists(target) && read_bytes(target).contains("unpublished initial state");
      substituted = MoveFileExW(external.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
      return ERROR_INVALID_DATA;
    }
    return std::nullopt;
  });
  EXPECT_FALSE(http::update_state_file(destination.string(), [](auto &tree) {
    tree.put("fixture", "unpublished initial state");
  }));
  http::test_support::set_state_file_hook({});
  ASSERT_TRUE(saw_committed_candidate);
  ASSERT_TRUE(substituted);
  EXPECT_FALSE(std::filesystem::exists(external));
  EXPECT_TRUE(read_bytes(destination) == external_bytes);
  EXPECT_TRUE(same_security(external_security, security_snapshot(destination)));
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), original_count);
  EXPECT_FALSE(http::update_state_file(destination.string(), [](auto &tree) {
    tree.put("fixture", "must remain blocked");
  }));
  EXPECT_TRUE(read_bytes(destination) == external_bytes);
  EXPECT_TRUE(same_security(external_security, security_snapshot(destination)));
  EXPECT_TRUE(state_bytes() == original_bytes);
  EXPECT_TRUE(same_security(original_security, security_snapshot(file)));
}

TEST_F(StateFileTransactionTest, MandatorySelectorsFailClosedWithoutRequiringAuditPrivilege) {
  using stage_t = http::test_support::state_file_stage_e;
  HANDLE token = nullptr;
  ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
  auto close = util::fail_guard([&]() {
    CloseHandle(token);
  });
  DWORD required = 0;
  GetTokenInformation(token, TokenPrivileges, nullptr, 0, &required);
  std::vector<std::uint8_t> privileges(required);
  ASSERT_TRUE(GetTokenInformation(token, TokenPrivileges, privileges.data(), required, &required));
  LUID audit {};
  ASSERT_TRUE(LookupPrivilegeValueW(nullptr, L"SeSecurityPrivilege", &audit));
  const auto *entries = reinterpret_cast<TOKEN_PRIVILEGES *>(privileges.data());
  for (DWORD index = 0; index < entries->PrivilegeCount; ++index) {
    const auto &entry = entries->Privileges[index];
    if (entry.Luid.HighPart == audit.HighPart && entry.Luid.LowPart == audit.LowPart && (entry.Attributes & SE_PRIVILEGE_ENABLED)) {
      GTEST_SKIP() << "The ordinary-user case requires audit privilege to be disabled";
    }
  }
  for (const auto selector : {LABEL_SECURITY_INFORMATION, ATTRIBUTE_SECURITY_INFORMATION, SCOPE_SECURITY_INFORMATION}) {
    const auto policy = security_snapshot(file, selector);
    ASSERT_TRUE(http::test_support::state_selected_security_matches(policy, policy, selector == SCOPE_SECURITY_INFORMATION));
  }
  for (const auto rejected : {stage_t::label, stage_t::attributes, stage_t::scope, stage_t::metadata}) {
    const auto bytes = state_bytes();
    bool reached_bytes = false;
    http::test_support::set_state_update_hook([&]() {
      reached_bytes = true;
    });
    http::test_support::set_state_file_hook([&](auto stage, const auto &, const auto &, const auto &) -> std::optional<std::uint32_t> {
      return stage == rejected ? std::optional<std::uint32_t> {ERROR_ACCESS_DENIED} : std::nullopt;
    });
    EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
      tree.put("fixture", "refused selector");
    }));
    http::test_support::set_state_file_hook({});
    http::test_support::set_state_update_hook({});
    EXPECT_FALSE(reached_bytes);
    EXPECT_TRUE(state_bytes() == bytes);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
  }
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "ordinary user save");
  }));
  const auto label = security_from_sddl(L"S:(ML;;NW;;;LW)");
  const auto changed = security_from_sddl(L"S:(ML;;NR;;;LW)");
  const auto scope = security_from_sddl(L"S:(SP;;0x0;;;S-1-17-1)");
  ASSERT_FALSE(label.empty());
  ASSERT_FALSE(changed.empty());
  ASSERT_FALSE(scope.empty());
  EXPECT_TRUE(http::test_support::state_selected_security_matches(label, label));
  EXPECT_FALSE(http::test_support::state_selected_security_matches(label, changed));
  EXPECT_FALSE(http::test_support::state_selected_security_matches({}, {}));
  EXPECT_FALSE(http::test_support::state_selected_security_matches(scope, scope, true));
}

TEST_F(StateFileTransactionTest, PreservesExplicitLowLabelAndResourceAttributesBeforeBytes) {
  for (const auto selector : {LABEL_SECURITY_INFORMATION, ATTRIBUTE_SECURITY_INFORMATION}) {
    const auto descriptor = security_from_sddl(selector == LABEL_SECURITY_INFORMATION ? L"S:(ML;;NW;;;LW)" : L"S:(RA;;;;;WD;(\"Fixture\",TS,0x0,\"value\"))");
    ASSERT_FALSE(descriptor.empty());
    ASSERT_TRUE(set_selected_security(file, descriptor, selector));
    const auto original = security_snapshot(file, selector);
    ASSERT_TRUE(http::test_support::state_selected_security_matches(original, original));
    bool inspected = false;
    http::test_support::set_state_security_hook([&](auto stage, const auto &temporary) {
      if (stage == http::test_support::state_security_stage_e::verify) {
        EXPECT_EQ(std::filesystem::file_size(temporary), 0U);
        EXPECT_TRUE(http::test_support::state_selected_security_matches(original, security_snapshot(temporary, selector)));
        inspected = true;
      }
    });
    EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
      tree.put("fixture", "selected authority preserved");
    }));
    http::test_support::set_state_security_hook({});
    EXPECT_TRUE(inspected);
    EXPECT_TRUE(http::test_support::state_selected_security_matches(original, security_snapshot(file, selector)));
    expect_repeated_saves_preserve_security(2);
    EXPECT_TRUE(http::test_support::state_selected_security_matches(original, security_snapshot(file, selector)));
  }
}

TEST_F(StateFileTransactionTest, PreservesOrdinaryAttributesCreationTimeAndLongDefaultFilename) {
  file = directory / "sunshine_state.json";
  ASSERT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "long default filename");
  }));
  HANDLE handle = CreateFileW(file.c_str(), FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  FILETIME created {0xE8000000U, 0x01D4F54CU};
  ASSERT_TRUE(SetFileTime(handle, &created, nullptr, nullptr));
  ASSERT_TRUE(CloseHandle(handle));
  constexpr DWORD attributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
  ASSERT_TRUE(SetFileAttributesW(file.c_str(), attributes));
  expect_repeated_saves_preserve_security(3);
  EXPECT_EQ(GetFileAttributesW(file.c_str()), attributes);
  handle = CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  BY_HANDLE_FILE_INFORMATION final {};
  ASSERT_TRUE(GetFileInformationByHandle(handle, &final));
  ASSERT_TRUE(CloseHandle(handle));
  EXPECT_EQ(CompareFileTime(&created, &final.ftCreationTime), 0);
}

TEST_F(StateFileTransactionTest, RefusesNamedStreamsExtendedAttributesAndObjectIdsBeforeBytes) {
  bool reached_bytes = false;
  http::test_support::set_state_update_hook([&]() {
    reached_bytes = true;
  });
  const auto original = state_bytes();
  const auto stream = std::filesystem::path {file.wstring() + L":fixture"};
  {
    std::ofstream output {stream};
    output << "synthetic stream";
    ASSERT_TRUE(output.good());
  }
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "unpublished");
  }));
  EXPECT_FALSE(reached_bytes);
  EXPECT_TRUE(state_bytes() == original);
  ASSERT_TRUE(DeleteFileW(stream.c_str()));

  using set_ea_t = NTSTATUS(NTAPI *)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG);
  const auto set_ea = std::bit_cast<set_ea_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetEaFile"));
  ASSERT_NE(set_ea, nullptr);

  struct fixture_ea_t {
    ULONG next = 0;
    UCHAR flags = 0;
    UCHAR name_length = 7;
    USHORT value_length = 4;
    char name[8] = "Fixture";
    char value[4] = {'t', 'e', 's', 't'};
  } ea;

  HANDLE handle = CreateFileW(file.c_str(), FILE_WRITE_EA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  IO_STATUS_BLOCK status {};
  ASSERT_EQ(set_ea(handle, &status, &ea, sizeof(ea)), 0);
  ASSERT_TRUE(CloseHandle(handle));
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "unpublished");
  }));
  EXPECT_FALSE(reached_bytes);
  EXPECT_TRUE(state_bytes() == original);
  handle = CreateFileW(file.c_str(), FILE_WRITE_EA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  ea.value_length = 0;
  ASSERT_EQ(set_ea(handle, &status, &ea, offsetof(fixture_ea_t, value)), 0);
  ASSERT_TRUE(CloseHandle(handle));

  handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  FILE_OBJECTID_BUFFER object_id {};
  DWORD returned = 0;
  ASSERT_TRUE(DeviceIoControl(handle, FSCTL_CREATE_OR_GET_OBJECT_ID, nullptr, 0, &object_id, sizeof(object_id), &returned, nullptr));
  ASSERT_TRUE(CloseHandle(handle));
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "unpublished");
  }));
  EXPECT_FALSE(reached_bytes);
  EXPECT_TRUE(state_bytes() == original);
  handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  ASSERT_TRUE(DeviceIoControl(handle, FSCTL_DELETE_OBJECT_ID, nullptr, 0, nullptr, 0, &returned, nullptr));
  ASSERT_TRUE(CloseHandle(handle));
  http::test_support::set_state_update_hook({});
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "recovered");
  }));
}

TEST_F(StateFileTransactionTest, RefusesUnsupportedAttributesAndExistingDataWritersBeforeBytes) {
  bool reached_bytes = false;
  const auto original = state_bytes();
  http::test_support::set_state_update_hook([&]() {
    reached_bytes = true;
  });
  for (const auto attributes : {FILE_ATTRIBUTE_ENCRYPTED, FILE_ATTRIBUTE_COMPRESSED, FILE_ATTRIBUTE_SPARSE_FILE, FILE_ATTRIBUTE_REPARSE_POINT, FILE_ATTRIBUTE_READONLY, FILE_ATTRIBUTE_OFFLINE}) {
    http::test_support::set_state_attributes_hook([&](const auto &) -> std::optional<std::uint32_t> {
      return attributes;
    });
    EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
      tree.put("fixture", "unpublished");
    }));
    EXPECT_FALSE(reached_bytes);
    EXPECT_TRUE(state_bytes() == original);
  }
  http::test_support::set_state_attributes_hook({});
  ASSERT_TRUE(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE));
  auto writable = util::fail_guard([&]() {
    SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_ARCHIVE);
  });
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "read-only refusal");
  }));
  EXPECT_FALSE(reached_bytes);
  EXPECT_TRUE(state_bytes() == original);
  ASSERT_TRUE(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_ARCHIVE));
  HANDLE writer = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  ASSERT_NE(writer, INVALID_HANDLE_VALUE);
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "unpublished");
  }));
  EXPECT_FALSE(reached_bytes);
  EXPECT_TRUE(state_bytes() == original);
  ASSERT_TRUE(CloseHandle(writer));
  http::test_support::set_state_update_hook({});
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "recovered");
  }));
}

TEST_F(StateFileTransactionTest, MixedExplicitAndInheritedPolicySurvivesRepeatedSavesAndParentRevocation) {
  auto parent = security_from_sddl(L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)");
  ASSERT_FALSE(parent.empty());
  ASSERT_TRUE(SetFileSecurityW(directory.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, parent.data()));
  ASSERT_TRUE(std::filesystem::remove(file));
  const auto sibling = directory / "mixed-untouched.json";
  for (const auto &path : {file, sibling}) {
    {
      std::ofstream output {path};
      output << "{\"fixture\":\"mixed policy\"}\n";
      ASSERT_TRUE(output.good());
    }
    auto inherited = security_snapshot(path);
    ASSERT_TRUE(unprotected(inherited));
    ASSERT_TRUE(inherited_read(inherited));
    PACL old_acl = nullptr, mixed = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    ASSERT_TRUE(GetSecurityDescriptorDacl(inherited.data(), &present, &old_acl, &defaulted));
    ASSERT_TRUE(present && old_acl);
    std::array<BYTE, SECURITY_MAX_SID_SIZE> world {};
    DWORD size = static_cast<DWORD>(world.size());
    ASSERT_TRUE(CreateWellKnownSid(WinWorldSid, nullptr, world.data(), &size));
    EXPLICIT_ACCESS_W entry {};
    entry.grfAccessPermissions = FILE_READ_ATTRIBUTES;
    entry.grfAccessMode = GRANT_ACCESS;
    BuildTrusteeWithSidW(&entry.Trustee, world.data());
    ASSERT_EQ(SetEntriesInAclW(1, &entry, old_acl, &mixed), ERROR_SUCCESS);
    auto release = util::fail_guard([&]() {
      LocalFree(mixed);
    });
    auto name = path.wstring();
    ASSERT_EQ(SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, mixed, nullptr), ERROR_SUCCESS);
  }
  const auto original = security_snapshot(file);
  ASSERT_TRUE(same_security(original, security_snapshot(sibling)));
  PACL acl = nullptr;
  BOOL present = FALSE, defaulted = FALSE;
  auto copy = original;
  ASSERT_TRUE(GetSecurityDescriptorDacl(copy.data(), &present, &acl, &defaulted));
  bool saw_explicit = false, saw_inherited = false;
  for (DWORD index = 0; index < acl->AceCount; ++index) {
    void *raw = nullptr;
    ASSERT_TRUE(GetAce(acl, index, &raw));
    saw_inherited |= !!(static_cast<ACE_HEADER *>(raw)->AceFlags & INHERITED_ACE);
    saw_explicit |= !(static_cast<ACE_HEADER *>(raw)->AceFlags & INHERITED_ACE);
  }
  ASSERT_TRUE(saw_explicit && saw_inherited);
  expect_repeated_saves_preserve_security(3);
  EXPECT_TRUE(same_security(original, security_snapshot(file)));
  auto reduced = security_from_sddl(L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
  ASSERT_TRUE(GetSecurityDescriptorDacl(reduced.data(), &present, &acl, &defaulted));
  auto name = directory.wstring();
  ASSERT_EQ(SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr), ERROR_SUCCESS);
  EXPECT_FALSE(inherited_read(security_snapshot(file)));
  EXPECT_FALSE(inherited_read(security_snapshot(sibling)));
  EXPECT_TRUE(same_security(security_snapshot(sibling), security_snapshot(file)));
  expect_repeated_saves_preserve_security(3);
  EXPECT_TRUE(same_security(security_snapshot(sibling), security_snapshot(file)));
}

TEST_F(StateFileTransactionTest, BeforeCommitCleanupFailureIsDiagnosedAndOriginalRemainsRetryable) {
  using stage_t = http::test_support::state_file_stage_e;
  const auto original = state_bytes();
  const auto policy = security_snapshot(file);
  http::test_support::set_state_file_hook([](auto stage, const auto &, const auto &, const auto &) -> std::optional<std::uint32_t> {
    return stage == stage_t::write || stage == stage_t::cleanup ? std::optional<std::uint32_t> {ERROR_ACCESS_DENIED} : std::nullopt;
  });
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "unpublished");
  }));
  http::test_support::set_state_file_hook({});
  EXPECT_TRUE(state_bytes() == original);
  EXPECT_TRUE(same_security(policy, security_snapshot(file)));
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 3);
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "retry after cleanup refusal");
  }));
}

TEST_F(StateFileTransactionTest, BackupCollisionPreservesOriginalAndDoesNotDeleteTheCollision) {
  using stage_t = http::test_support::state_file_stage_e;
  const auto original = state_bytes();
  const auto policy = security_snapshot(file);
  std::filesystem::path collision;
  bool reached_bytes = false;
  http::test_support::set_state_update_hook([&]() {
    reached_bytes = true;
  });
  http::test_support::set_state_file_hook([&](auto stage, const auto &, const auto &, const auto &backup) -> std::optional<std::uint32_t> {
    if (stage == stage_t::backup) {
      collision = backup;
      std::ofstream output {collision};
      output << "synthetic unrelated collision";
      EXPECT_TRUE(output.good());
    }
    return std::nullopt;
  });
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "unpublished");
  }));
  http::test_support::set_state_file_hook({});
  http::test_support::set_state_update_hook({});
  ASSERT_FALSE(collision.empty());
  EXPECT_FALSE(reached_bytes);
  EXPECT_TRUE(state_bytes() == original);
  EXPECT_TRUE(same_security(policy, security_snapshot(file)));
  std::ifstream input {collision};
  const std::string bytes {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  input.close();
  EXPECT_TRUE(bytes == "synthetic unrelated collision");
  ASSERT_TRUE(std::filesystem::remove(collision));
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "recovered");
  }));
}

TEST_F(StateFileTransactionTest, RestoreNeverOverwritesAnUnrelatedExternalReplacement) {
  using stage_t = http::test_support::state_file_stage_e;
  const auto external = directory / "external-replacement.json";
  {
    std::ofstream output {external};
    output << "{\"fixture\":\"external replacement\"}\n";
    ASSERT_TRUE(output.good());
  }
  const auto original = state_bytes();
  http::test_support::set_state_file_hook([&](auto stage, const auto &destination, const auto &, const auto &) -> std::optional<std::uint32_t> {
    if (stage == stage_t::verify) {
      return ERROR_INVALID_DATA;
    }
    if (stage == stage_t::restore) {
      EXPECT_TRUE(MoveFileExW(external.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    }
    return std::nullopt;
  });
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "unpublished");
  }));
  http::test_support::set_state_file_hook({});
  EXPECT_TRUE(state_bytes().contains("external replacement"));
  bool saw_original = false;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().filename().string().starts_with("state.json.backup-")) {
      std::ifstream input {entry.path()};
      const std::string bytes {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
      saw_original = bytes == original;
    }
  }
  EXPECT_TRUE(saw_original);
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "must remain blocked");
  }));
}

TEST_F(StateFileTransactionTest, FailedRestoreRetainsSecuredRecoveryMaterialAndBlocksFurtherWrites) {
  using stage_t = http::test_support::state_file_stage_e;
  const auto original = security_snapshot(file);
  const auto original_bytes = state_bytes();
  http::test_support::set_state_file_hook([&](auto stage, const auto &, const auto &, const auto &) -> std::optional<std::uint32_t> {
    if (stage == stage_t::verify || stage == stage_t::restore) {
      return ERROR_ACCESS_DENIED;
    }
    return std::nullopt;
  });
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "unpublished");
  }));
  http::test_support::set_state_file_hook({});
  std::filesystem::path backup;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().filename().string().starts_with("state.json.backup-")) {
      backup = entry.path();
    }
  }
  ASSERT_FALSE(backup.empty());
  std::ifstream input {backup};
  const std::string backup_bytes {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  input.close();
  EXPECT_TRUE(backup_bytes == original_bytes);
  EXPECT_TRUE(same_security(original, security_snapshot(backup)));
  ASSERT_TRUE(MoveFileExW(backup.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
  EXPECT_TRUE(state_bytes() == original_bytes);
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "must remain blocked");
  }));
  const auto alias = directory / "recovery-alias.json";
  ASSERT_TRUE(CreateHardLinkW(alias.c_str(), file.c_str(), nullptr));
  EXPECT_FALSE(http::update_state_file(alias.string(), [](auto &tree) {
    tree.put("fixture", "must remain blocked through alias");
  }));
}

TEST_F(StateFileTransactionTest, VerifiedCommitCleanupFailureIsSuccessAndPreservesSecuredBackup) {
  using stage_t = http::test_support::state_file_stage_e;
  const auto original = security_snapshot(file);
  const auto original_bytes = state_bytes();
  http::test_support::set_state_file_hook([](auto stage, const auto &, const auto &, const auto &) -> std::optional<std::uint32_t> {
    return stage == stage_t::cleanup ? std::optional<std::uint32_t> {ERROR_ACCESS_DENIED} : std::nullopt;
  });
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "committed");
  }));
  http::test_support::set_state_file_hook({});
  EXPECT_TRUE(state_bytes().contains("committed"));
  EXPECT_TRUE(same_security(original, security_snapshot(file)));
  unsigned backups = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().filename().string().starts_with("state.json.backup-")) {
      ++backups;
      std::ifstream input {entry.path()};
      const std::string bytes {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
      EXPECT_TRUE(bytes == original_bytes);
      EXPECT_TRUE(same_security(original, security_snapshot(entry.path())));
    }
  }
  EXPECT_EQ(backups, 1U);
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "next valid commit");
  }));
}

#endif

#ifdef __linux__
TEST_F(StateFileTransactionTest, RefusesNamedAccessOrInheritedDefaultAclWithoutPublishingBytes) {
  // Linux's documented version 2 POSIX ACL xattr: owner rw, named user rw,
  // file group no access, mask rw and others no access. Its mode alone reports 0660.
  struct acl_entry_t {
    std::uint16_t tag;
    std::uint16_t permissions;
    std::uint32_t id;
  };

  struct acl_bytes_t {
    std::uint32_t version;
    std::array<acl_entry_t, 5> entries;
  };

  const auto le16 = [](std::uint16_t value) {
    return util::endian::little(value);
  };
  const auto le32 = [](std::uint32_t value) {
    return util::endian::little(value);
  };
  const auto nobody = static_cast<std::uint32_t>(::getuid()) == 65534U ? 65533U : 65534U;
  const acl_bytes_t acl {le32(2), {{{le16(1), le16(6), le32(0xFFFFFFFFU)}, {le16(2), le16(6), le32(nobody)}, {le16(4), le16(0), le32(0xFFFFFFFFU)}, {le16(16), le16(6), le32(0xFFFFFFFFU)}, {le16(32), le16(0), le32(0xFFFFFFFFU)}}}};
  if (::setxattr(file.c_str(), "system.posix_acl_access", &acl, sizeof(acl), 0)) {
    GTEST_SKIP() << "The fixture filesystem cannot represent a POSIX access ACL";
  }
  bool reached_bytes = false;
  http::test_support::set_state_update_hook([&]() {
    reached_bytes = true;
  });
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "unpublished");
  }));
  EXPECT_FALSE(reached_bytes);
  ASSERT_EQ(::removexattr(file.c_str(), "system.posix_acl_access"), 0);
  std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  ASSERT_EQ(::setxattr(directory.c_str(), "system.posix_acl_default", &acl, sizeof(acl), 0), 0);
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "unpublished");
  }));
  EXPECT_FALSE(reached_bytes);
  http::test_support::set_state_update_hook({});
  std::ifstream input {file};
  const std::string bytes {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  EXPECT_TRUE(bytes.contains("original"));
  EXPECT_FALSE(bytes.contains("unpublished"));
}
#endif
