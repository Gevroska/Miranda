/**
 * @file tests/unit/test_httpcommon.cpp
 * @brief Test src/httpcommon.*.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <cerrno>
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
    std::filesystem::remove_all(directory);
    BaseTest::TearDown();
  }
#ifdef _WIN32
  /**
   * @brief Read a complete owner, group and DACL snapshot from a fixture file.
   * @param path Test-owned file.
   * @return Self-relative descriptor, or an empty snapshot on failure.
   */
  std::vector<std::uint8_t> security_snapshot(const std::filesystem::path &path) {
    constexpr DWORD information = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
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

  /**
   * @brief Compare independently captured fixture snapshots through the production security gate.
   * @param expected Original descriptor.
   * @param current Descriptor under inspection.
   * @param replacement Allow only the evidenced final auto-inheritance marker addition.
   * @return Whether complete authority and every other control field is preserved.
   */
  bool same_security(std::vector<std::uint8_t> expected, std::vector<std::uint8_t> current, bool replacement = false) {
    return http::test_support::state_file_security_matches(std::move(expected), std::move(current), replacement);
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
#endif

  /**
   * @brief Read synthetic JSON without printing state contents in assertion diagnostics.
   * @return Fixture bytes.
   */
  std::string state_bytes() {
    std::ifstream input {file};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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
  EXPECT_TRUE(same_security(original, security_snapshot(file), true));
#else
  EXPECT_EQ(std::filesystem::status(file).permissions(), original);
  struct stat replacement_metadata {};
  ASSERT_EQ(::stat(file.c_str(), &replacement_metadata), 0);
  EXPECT_EQ(replacement_metadata.st_uid, original_metadata.st_uid);
  EXPECT_EQ(replacement_metadata.st_gid, original_metadata.st_gid);
#endif
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
}

#ifdef _WIN32
TEST_F(StateFileTransactionTest, ReplacementComparisonAllowsOnlyFinalAutoInheritanceMarkerAddition) {
  const auto original = security_from_sddl(L"O:SYG:BAD:P(A;;FA;;;OW)");
  const auto marked = security_from_sddl(L"O:SYG:BAD:PAI(A;;FA;;;OW)");
  ASSERT_FALSE(original.empty());
  ASSERT_FALSE(marked.empty());
  EXPECT_TRUE(same_security(original, original));
  EXPECT_FALSE(same_security(original, marked));
  EXPECT_TRUE(same_security(original, marked, true));
  EXPECT_FALSE(same_security(marked, original, true));

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
    EXPECT_FALSE(same_security(original, changed, true));
  }
  const auto ordered = security_from_sddl(L"O:SYG:BAD:P(A;;FA;;;OW)(A;;FR;;;SY)");
  const auto reordered = security_from_sddl(L"O:SYG:BAD:PAI(A;;FR;;;SY)(A;;FA;;;OW)");
  ASSERT_FALSE(ordered.empty());
  ASSERT_FALSE(reordered.empty());
  EXPECT_FALSE(same_security(ordered, reordered, true));
  const auto unprotected = security_from_sddl(L"O:SYG:BAD:(A;;FA;;;OW)");
  const auto unprotected_marked = security_from_sddl(L"O:SYG:BAD:AI(A;;FA;;;OW)");
  ASSERT_FALSE(unprotected.empty());
  ASSERT_FALSE(unprotected_marked.empty());
  EXPECT_FALSE(same_security(unprotected, unprotected_marked));
  EXPECT_TRUE(same_security(unprotected, unprotected_marked, true));
  EXPECT_FALSE(same_security(unprotected_marked, unprotected, true));
  const auto null_dacl = security_from_sddl(L"O:SYG:BAD:PNO_ACCESS_CONTROL");
  const auto empty_dacl = security_from_sddl(L"O:SYG:BAD:PAI");
  ASSERT_FALSE(null_dacl.empty());
  ASSERT_FALSE(empty_dacl.empty());
  EXPECT_FALSE(same_security(null_dacl, empty_dacl, true));
  EXPECT_FALSE(same_security({}, {}, true));
  EXPECT_FALSE(same_security(original, {}, true));
  EXPECT_FALSE(same_security(original, std::vector<std::uint8_t>(sizeof(SECURITY_DESCRIPTOR_RELATIVE)), true));
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
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "replacement");
  }));
  http::test_support::set_state_security_hook({});
  http::test_support::set_state_update_hook({});
  EXPECT_TRUE(saw_creation && saw_restored && saw_prebytes);
  const auto replaced = security_snapshot(file);
  EXPECT_TRUE(same_security(original, replaced, true));
  EXPECT_TRUE(unprotected(replaced));
  EXPECT_TRUE(inherited_read(replaced));
  EXPECT_TRUE(same_security(baseline, security_snapshot(sibling)));
  EXPECT_TRUE(state_bytes().contains("replacement"));

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
  EXPECT_TRUE(same_security(revoked, untouched_revoked));
  EXPECT_TRUE(unprotected(revoked) && unprotected(untouched_revoked));
  EXPECT_FALSE(inherited_read(revoked));
  EXPECT_FALSE(inherited_read(untouched_revoked));
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "saved after parent revocation");
  }));
  EXPECT_TRUE(same_security(revoked, security_snapshot(file), true));
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
  EXPECT_TRUE(same_security(original, security_snapshot(file), true));
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
    EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
      tree.put("fixture", "recovered");
    }));
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
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "must remain unpublished");
  }));
  EXPECT_TRUE(state_bytes() == original_bytes);
  EXPECT_TRUE(same_security(original, security_snapshot(file)));
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
  ASSERT_TRUE(CloseHandle(reader));
  reader = INVALID_HANDLE_VALUE;
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "recovered");
  }));
}

TEST_F(StateFileTransactionTest, DocumentedPartialReplaceFailuresRestoreOriginalBeforeReturningFailure) {
  const auto original = security(file);
  for (const auto error : {ERROR_UNABLE_TO_MOVE_REPLACEMENT, ERROR_UNABLE_TO_MOVE_REPLACEMENT_2}) {
    http::test_support::set_state_replace_hook([&](const auto &destination, const auto &, const auto &backup) -> std::optional<std::uint32_t> {
      if (error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
        EXPECT_TRUE(MoveFileExW(destination.c_str(), backup.c_str(), 0));
      }
      return error;
    });
    EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
      tree.put("fixture", "unpublished");
    }));
    std::ifstream input {file};
    const std::string bytes {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    EXPECT_TRUE(bytes.contains("original"));
    EXPECT_FALSE(bytes.contains("unpublished"));
    EXPECT_TRUE(security(file) == original);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
  }
  http::test_support::set_state_replace_hook({});
  EXPECT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "recovered update");
  }));
}

TEST_F(StateFileTransactionTest, FailedRestoreRetainsSecuredRecoveryMaterialAndBlocksFurtherWrites) {
  http::test_support::set_state_replace_hook([&](const auto &destination, const auto &, const auto &backup) -> std::optional<std::uint32_t> {
    EXPECT_TRUE(MoveFileExW(destination.c_str(), backup.c_str(), 0));
    std::filesystem::create_directory(destination);
    return ERROR_UNABLE_TO_MOVE_REPLACEMENT_2;
  });
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "unpublished");
  }));
  http::test_support::set_state_replace_hook({});
  std::filesystem::remove(file);
  std::filesystem::path backup;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().filename().string().starts_with("state.json.backup-")) {
      backup = entry.path();
    }
  }
  ASSERT_FALSE(backup.empty());
  EXPECT_TRUE(MoveFileExW(backup.c_str(), file.c_str(), 0));
  EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "must remain blocked");
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
