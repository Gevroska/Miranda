/**
 * @file tests/unit/test_httpcommon.cpp
 * @brief Test src/httpcommon.*.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

// lib includes
#include <boost/property_tree/json_parser.hpp>
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
#include <src/app_catalog.h>
#include <src/config.h>
#include <src/crypto.h>
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
   * @brief Replace the state name with a distinct fixed fixture object while its original permits delete sharing.
   * @param source One of the two synthetic external-edit leaves created by the calling tests.
   * @return Whether one real extended rename changed the destination to the verified source identity.
   */
  bool replace_external_fixture(const std::filesystem::path &source) {
  #if defined(FILE_RENAME_FLAG_REPLACE_IF_EXISTS) && defined(FILE_RENAME_FLAG_POSIX_SEMANTICS) && NTDDI_VERSION >= 0x0A000002
    const auto refuse = [](const char *reason, DWORD native_error = ERROR_SUCCESS) {
      ADD_FAILURE() << reason << "; native_error=" << native_error;
      return false;
    };
    if (file != directory / "state.json" || (source != directory / "external-edit.bytes" && source != directory / "same-byte-external.json")) {
      return refuse("External replacement refused a path outside its fixed fixture leaves");
    }
    for (auto ancestor = std::filesystem::absolute(directory);; ancestor = ancestor.parent_path()) {
      const auto attributes = GetFileAttributesW(ancestor.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) {
        return refuse("External replacement could not inspect a fixture ancestor", GetLastError());
      }
      if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return refuse("External replacement requires plain fixture ancestors");
      }
      if (ancestor == ancestor.root_path()) {
        break;
      }
    }
    const auto same_identity = [](const auto &left, const auto &right) {
      return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber && left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow;
    };
    const auto inspect = [&](HANDLE handle, BY_HANDLE_FILE_INFORMATION &identity, bool single_link) {
      if (!GetFileInformationByHandle(handle, &identity)) {
        return refuse("External replacement could not inspect an opened fixture identity", GetLastError());
      }
      if ((identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || (single_link && identity.nNumberOfLinks != 1)) {
        return refuse("External replacement requires plain fixture files and one source link");
      }
      return true;
    };
    const auto close = [](HANDLE handle) {
      const auto closed = CloseHandle(handle);
      const auto native_error = closed ? ERROR_SUCCESS : GetLastError();
      EXPECT_TRUE(closed) << "External replacement fixture close failed; native_error=" << native_error;
    };
    const auto named_matches = [&](const std::filesystem::path &path, const auto &expected) {
      HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        return refuse("External replacement could not reopen a fixture name", GetLastError());
      }
      auto closing = util::fail_guard([&]() {
        close(handle);
      });
      BY_HANDLE_FILE_INFORMATION current {};
      return inspect(handle, current, false) && (same_identity(expected, current) || refuse("External replacement fixture name changed identity"));
    };
    HANDLE moving = CreateFileW(source.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (moving == INVALID_HANDLE_VALUE) {
      return refuse("External replacement could not open its source", GetLastError());
    }
    auto close_moving = util::fail_guard([&]() {
      close(moving);
    });
    HANDLE original = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (original == INVALID_HANDLE_VALUE) {
      return refuse("External replacement could not open its destination", GetLastError());
    }
    auto close_original = util::fail_guard([&]() {
      close(original);
    });
    BY_HANDLE_FILE_INFORMATION source_identity {}, original_identity {};
    if (!inspect(moving, source_identity, true) || !inspect(original, original_identity, false)) {
      return false;
    }
    if (source_identity.dwVolumeSerialNumber != original_identity.dwVolumeSerialNumber || same_identity(source_identity, original_identity)) {
      return refuse("External replacement requires distinct same-volume fixture identities");
    }
    if (!named_matches(source, source_identity) || !named_matches(file, original_identity)) {
      return false;
    }
    const auto target = std::filesystem::absolute(file).wstring();
    constexpr auto offset = offsetof(FILE_RENAME_INFO, FileName);
    if (target.size() > (MAXDWORD - offset - sizeof(wchar_t)) / sizeof(wchar_t)) {
      return refuse("External replacement target exceeds the native buffer limit");
    }
    const auto name_size = target.size() * sizeof(wchar_t);
    const auto buffer_size = std::max(sizeof(FILE_RENAME_INFO), offset + name_size + sizeof(wchar_t));
    static_assert(alignof(FILE_RENAME_INFO) <= alignof(std::max_align_t));
    auto *rename = static_cast<FILE_RENAME_INFO *>(std::calloc(1, buffer_size));
    if (!rename) {
      return refuse("External replacement could not allocate its aligned rename buffer");
    }
    auto free_rename = util::fail_guard([&]() {
      std::free(rename);
    });
    rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename->FileNameLength = static_cast<DWORD>(name_size);
    std::memcpy(rename->FileName, target.data(), name_size);
    const auto renamed = SetFileInformationByHandle(moving, FileRenameInfoEx, rename, static_cast<DWORD>(buffer_size));
    const auto rename_error = renamed ? ERROR_SUCCESS : GetLastError();
    if (!renamed) {
      return refuse("External replacement handle-bound rename was refused", rename_error);
    }
    BY_HANDLE_FILE_INFORMATION moved {}, retained_original {};
    if (!inspect(moving, moved, true) || !inspect(original, retained_original, false) || !same_identity(source_identity, moved) || !same_identity(original_identity, retained_original) || !named_matches(file, source_identity)) {
      return refuse("External replacement did not preserve its opened identities and publish the distinct source");
    }
    const auto attributes = GetFileAttributesW(source.c_str());
    const auto absence_error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    if (attributes != INVALID_FILE_ATTRIBUTES || absence_error != ERROR_FILE_NOT_FOUND) {
      return refuse("External replacement source name is not genuinely absent", absence_error);
    }
    return true;
  #else
    (void) source;
    ADD_FAILURE() << "External replacement requires the production baseline's extended handle-based rename support";
    return false;
  #endif
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

  /** @brief Independently captured original components for exact precommit failure checks. */
  struct fixture_snapshot_t {
    BY_HANDLE_FILE_INFORMATION identity {};  ///< Original file identity and creation time.
    std::string bytes;  ///< Synthetic bytes, never printed in assertion output.
    std::vector<std::uint8_t> security;  ///< Exact owner/group/ordered DACL and controls.
    std::array<std::vector<std::uint8_t>, 3> selected;  ///< Every separately selected policy.
  };

  /** @brief Observe a test-owned original without excluding compatible transaction guards. */
  fixture_snapshot_t fixture_snapshot(const std::filesystem::path &path) {
    fixture_snapshot_t result;
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      ADD_FAILURE() << "Fixture snapshot could not open";
      return result;
    }
    auto close = util::fail_guard([&]() {
      EXPECT_TRUE(CloseHandle(handle));
    });
    EXPECT_TRUE(GetFileInformationByHandle(handle, &result.identity));
    result.bytes = read_fixture_handle(handle);
    result.security = security_snapshot(path);
    EXPECT_TRUE(same_security(result.security, result.security));
    constexpr std::array<DWORD, 3> selectors {LABEL_SECURITY_INFORMATION, ATTRIBUTE_SECURITY_INFORMATION, SCOPE_SECURITY_INFORMATION};
    for (std::size_t index = 0; index < selectors.size(); ++index) {
      result.selected[index] = security_snapshot(path, selectors[index]);
      EXPECT_TRUE(http::test_support::state_selected_security_matches(result.selected[index], result.selected[index], index == 2));
    }
    return result;
  }

  /** @brief Require exact identity, bytes, creation time, attributes and every authority component. */
  void expect_fixture_snapshot(const std::filesystem::path &path, const fixture_snapshot_t &expected) {
    const auto current = fixture_snapshot(path);
    EXPECT_EQ(current.identity.dwVolumeSerialNumber, expected.identity.dwVolumeSerialNumber);
    EXPECT_EQ(current.identity.nFileIndexHigh, expected.identity.nFileIndexHigh);
    EXPECT_EQ(current.identity.nFileIndexLow, expected.identity.nFileIndexLow);
    EXPECT_EQ(CompareFileTime(&current.identity.ftCreationTime, &expected.identity.ftCreationTime), 0);
    EXPECT_EQ(current.identity.dwFileAttributes, expected.identity.dwFileAttributes);
    EXPECT_TRUE(current.bytes == expected.bytes);
    EXPECT_TRUE(same_security(expected.security, current.security));
    for (std::size_t index = 0; index < expected.selected.size(); ++index) {
      EXPECT_TRUE(http::test_support::state_selected_security_matches(expected.selected[index], current.selected[index], index == 2));
    }
  }

  /** @brief Prove an ordinary data writer cannot open an alias of the guarded original. */
  void expect_fixture_writer_denied(const std::filesystem::path &path) {
    HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto open_error = writer == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    EXPECT_EQ(writer, INVALID_HANDLE_VALUE);
    EXPECT_EQ(open_error, ERROR_SHARING_VIOLATION);
    if (writer != INVALID_HANDLE_VALUE) {
      EXPECT_TRUE(CloseHandle(writer));
    }
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

TEST_F(StateFileTransactionTest, CredentialEditsPreserveTypedSharedStateWithoutRequiringAnAppCatalog) {
  const auto original = proc::catalog::parse(R"({"username":"old","salt":"old salt","password":"old hash","root":{"uniqueid":"legacy host","named_devices":[{"enabled":"true","permissions":"0","display":{"force_virtual":true,"resolution":{"width":2560,"height":1440},"refresh_rate":{"numerator":60000,"denominator":1001}}}]},"foreign":{"flags":[false,null,{},[]],"fraction":1.25,"integer":9007199254740993}})");
  std::ofstream {file, std::ios::binary} << original.dump();
  ASSERT_EQ(http::save_user_creds(file.string(), "synthetic administrator", "synthetic password"), 0);
  const auto saved = proc::catalog::parse(state_bytes());
  EXPECT_EQ(saved["root"], original["root"]);
  EXPECT_EQ(saved["foreign"], original["foreign"]);
  EXPECT_EQ(saved["username"], "synthetic administrator");
  const auto salt = saved["salt"].get<std::string>();
  EXPECT_EQ(saved["password"], util::hex(crypto::hash("synthetic password" + salt)).to_string());
  const auto standalone = directory / "standalone-credentials.json";
  EXPECT_EQ(http::save_user_creds(standalone.string(), "standalone", "synthetic password"), 0);
}

TEST_F(StateFileTransactionTest, FailedCredentialEditLeavesEveryOriginalByteAndTypedState) {
  const std::string original = R"({"username":"old","salt":"original salt","password":"original hash","foreign":[true,null,{"number":1.25}]})";
  std::ofstream {file, std::ios::binary} << original;
  http::test_support::set_state_update_hook([]() {
    throw std::runtime_error {"fixture failure before replacing shared state"};
  });
  EXPECT_NE(http::save_user_creds(file.string(), "must not publish", "synthetic password"), 0);
  EXPECT_EQ(state_bytes(), original);
}

TEST_F(StateFileTransactionTest, NonobjectCredentialDocumentsReportFailureWithoutChangingBytesOrActiveCredentials) {
  const auto previous_username = config::sunshine.username;
  const auto previous_password = config::sunshine.password;
  const auto previous_salt = config::sunshine.salt;
  const auto restore_credentials = util::fail_guard([&]() {
    config::sunshine.username = previous_username;
    config::sunshine.password = previous_password;
    config::sunshine.salt = previous_salt;
  });
  ASSERT_EQ(http::save_user_creds(file.string(), "synthetic active administrator", "synthetic active password"), 0);
  ASSERT_EQ(http::reload_user_creds(file.string()), 0);
  const auto active_username = config::sunshine.username;
  const auto active_password = config::sunshine.password;
  const auto active_salt = config::sunshine.salt;
  const auto valid_bytes = state_bytes();
  for (const auto *malformed : {"[]", "null", "false", "17", R"("invalid document")", R"([{"username":"preserved","password":"synthetic old hash","salt":"synthetic old salt"}])"}) {
    {
      std::ofstream output {file, std::ios::binary};
      output << malformed;
      ASSERT_TRUE(output.good());
    }
    EXPECT_NE(http::save_user_creds(file.string(), "must not publish", "synthetic replacement password"), 0);
    EXPECT_EQ(state_bytes(), malformed);
    EXPECT_EQ(config::sunshine.username, active_username);
    EXPECT_EQ(config::sunshine.password, active_password);
    EXPECT_EQ(config::sunshine.salt, active_salt);
  }
  {
    std::ofstream output {file, std::ios::binary};
    output << valid_bytes;
    ASSERT_TRUE(output.good());
  }
  EXPECT_EQ(http::save_user_creds(file.string(), "synthetic recovered administrator", "synthetic replacement password"), 0);
  const auto saved = proc::catalog::parse(state_bytes());
  EXPECT_EQ(saved["username"], "synthetic recovered administrator");
  const auto salt = saved["salt"].get<std::string>();
  EXPECT_EQ(saved["password"], util::hex(crypto::hash("synthetic replacement password" + salt)).to_string());
}

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
  bool verified = false, old_only = false, overlapping = false, new_only = false;
  std::filesystem::path handoff_backup;
  http::test_support::set_state_file_hook([&](auto stage, const auto &destination, const auto &source, const auto &backup) -> std::optional<std::uint32_t> {
    if (stage == stage_t::guard_open) {
      handoff_backup = backup;
      old_only = true;
    }
    if (stage == stage_t::metadata && !handoff_backup.empty() && destination == handoff_backup && !overlapping) {
      overlapping = true;
      EXPECT_TRUE(read_fixture_handle(reader) == original);
      EXPECT_TRUE(read_fixture_bytes(file) == original);
      EXPECT_TRUE(read_fixture_bytes(handoff_backup) == original);
    }
    if (stage == stage_t::guard_open || stage == stage_t::guard_close) {
      EXPECT_TRUE(read_fixture_handle(reader) == original);
      EXPECT_TRUE(read_fixture_bytes(destination) == original);
      EXPECT_TRUE(read_fixture_bytes(backup) == original);
      new_only = stage == stage_t::guard_close;
    }
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
  EXPECT_TRUE(old_only && overlapping && new_only);
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
  bool saw_restore = false, old_only = false, overlapping = false, new_only = false;
  std::filesystem::path handoff_backup;
  auto clear = util::fail_guard([]() {
    http::test_support::set_state_file_hook({});
  });
  http::test_support::set_state_file_hook([&](auto stage, const auto &destination, const auto &source, const auto &backup) -> std::optional<std::uint32_t> {
    if (stage == stage_t::guard_open) {
      handoff_backup = backup;
      old_only = true;
    }
    if (stage == stage_t::metadata && !handoff_backup.empty() && destination == handoff_backup && !overlapping) {
      overlapping = true;
      for (const auto &path : {file, handoff_backup, alias}) {
        expect_fixture_writer_denied(path);
      }
    }
    if (stage == stage_t::guard_open || stage == stage_t::guard_close) {
      for (const auto &path : {destination, backup, alias}) {
        expect_fixture_writer_denied(path);
      }
      new_only = stage == stage_t::guard_close;
    }
    if (stage == stage_t::rename_api || stage == stage_t::restore) {
      expect_fixture_writer_denied(alias);
      expect_fixture_writer_denied(backup);
    }
    if (stage == stage_t::rename_api) {
      ++rename_sources;
      expect_fixture_writer_denied(source);
      if (source != backup) {
        expect_fixture_writer_denied(destination);
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
  EXPECT_TRUE(old_only && overlapping && new_only);
  EXPECT_TRUE(state_bytes() == original);
  EXPECT_TRUE(read_fixture_bytes(alias) == original);
  EXPECT_TRUE(same_security(policy, security_snapshot(file)));
  ASSERT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
    tree.put("fixture", "recovered after guarded rollback");
  }));
  EXPECT_TRUE(read_fixture_bytes(alias) == original);
}

TEST_F(StateFileTransactionTest, GuardOpenAndCheckedCloseFailuresRemainPreEffectAndPermitExactRetry) {
  using stage_t = http::test_support::state_file_stage_e;
  const auto alias = directory / "handoff-original-alias.json";
  ASSERT_TRUE(CreateHardLinkW(alias.c_str(), file.c_str(), nullptr));
  for (const auto rejected : {stage_t::guard_open, stage_t::guard_close}) {
    SCOPED_TRACE(static_cast<int>(rejected));
    const auto original = fixture_snapshot(file);
    std::filesystem::path candidate;
    bool reached = false, prepared = false, wrote_or_renamed = false;
    http::test_support::set_state_security_hook([&](auto, const auto &) {
      prepared = true;
    });
    http::test_support::set_state_update_hook([&]() {
      prepared = true;
    });
    http::test_support::set_state_file_hook([&](auto stage, const auto &destination, const auto &temporary, const auto &backup) -> std::optional<std::uint32_t> {
      if (stage == stage_t::guard_open) {
        candidate = temporary;
      }
      if (stage == stage_t::metadata && !candidate.empty() && destination == candidate) {
        prepared = true;
      }
      if (stage == stage_t::write || stage == stage_t::flush || stage == stage_t::close || stage == stage_t::rename || stage == stage_t::rename_open || stage == stage_t::rename_api || stage == stage_t::rename_close || stage == stage_t::verify) {
        wrote_or_renamed = true;
      }
      if (stage != rejected) {
        return std::nullopt;
      }
      reached = true;
      EXPECT_FALSE(std::filesystem::exists(temporary));
      for (const auto &path : {destination, backup, alias}) {
        expect_fixture_writer_denied(path);
        expect_fixture_snapshot(path, original);
      }
      return ERROR_INVALID_HANDLE;
    });
    EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
      tree.put("fixture", "unpublished guard failure");
    }));
    http::test_support::set_state_file_hook({});
    http::test_support::set_state_security_hook({});
    http::test_support::set_state_update_hook({});
    EXPECT_TRUE(reached);
    EXPECT_FALSE(prepared || wrote_or_renamed);
    expect_fixture_snapshot(file, original);
    expect_fixture_snapshot(alias, original);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 2);
    ASSERT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
      tree.put("fixture", "clean retry after guard failure");
    }));
    expect_fixture_snapshot(alias, original);
    ASSERT_TRUE(DeleteFileW(alias.c_str()));
    ASSERT_TRUE(CreateHardLinkW(alias.c_str(), file.c_str(), nullptr));
  }
}

TEST_F(StateFileTransactionTest, HandoffSelectorFailuresRejectBothOverlappingAndTransferredGuardsBeforePreparation) {
  using stage_t = http::test_support::state_file_stage_e;
  for (const auto boundary : {stage_t::guard_open, stage_t::guard_close}) {
    for (const auto rejected : {stage_t::metadata, stage_t::label, stage_t::attributes, stage_t::scope}) {
      SCOPED_TRACE(static_cast<int>(boundary));
      SCOPED_TRACE(static_cast<int>(rejected));
      const auto original = fixture_snapshot(file);
      std::filesystem::path backup;
      bool reached = false, prepared = false;
      http::test_support::set_state_security_hook([&](auto, const auto &) {
        prepared = true;
      });
      http::test_support::set_state_file_hook([&](auto stage, const auto &path, const auto &temporary, const auto &old_backup) -> std::optional<std::uint32_t> {
        if (stage == boundary) {
          backup = old_backup;
          EXPECT_FALSE(std::filesystem::exists(temporary));
        }
        if (!reached && !backup.empty() && stage == rejected && path == backup) {
          reached = true;
          expect_fixture_writer_denied(file);
          expect_fixture_writer_denied(backup);
          return ERROR_ACCESS_DENIED;
        }
        return std::nullopt;
      });
      EXPECT_FALSE(http::update_state_file(file.string(), [](auto &tree) {
        tree.put("fixture", "unreadable guard authority");
      }));
      http::test_support::set_state_file_hook({});
      http::test_support::set_state_security_hook({});
      EXPECT_TRUE(reached);
      EXPECT_FALSE(prepared);
      expect_fixture_snapshot(file, original);
      EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
      ASSERT_TRUE(http::update_state_file(file.string(), [](auto &tree) {
        tree.put("fixture", "clean retry after handoff selector refusal");
      }));
    }
  }
}

TEST_F(StateFileTransactionTest, HandoffRejectsRealOrderedDaclAndControlDriftWithoutRepairingAuthority) {
  using stage_t = http::test_support::state_file_stage_e;
  auto baseline = security_from_sddl(L"D:P(A;;FA;;;OW)(A;;FR;;;BU)");
  ASSERT_FALSE(baseline.empty());
  unsigned next = 0;
  for (const auto boundary : {stage_t::guard_open, stage_t::guard_close}) {
    for (const auto text : {L"D:P(A;;FA;;;OW)(A;;FW;;;BU)", L"D:P(A;;FR;;;BU)(A;;FA;;;OW)", L"D:(A;;FA;;;OW)(A;;FR;;;BU)"}) {
      SCOPED_TRACE(static_cast<int>(boundary));
      SCOPED_TRACE(next);
      const auto destination = directory / ("authority-drift-" + std::to_string(++next) + ".json");
      ASSERT_TRUE(http::update_state_file(destination.string(), [](auto &tree) {
        tree.put("fixture", "original authority drift state");
      }));
      ASSERT_TRUE(SetFileSecurityW(destination.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, baseline.data()));
      const auto original = fixture_snapshot(destination);
      ASSERT_FALSE(unprotected(original.security));
      auto changed = security_from_sddl(text);
      ASSERT_FALSE(changed.empty());
      const bool change_control = unprotected(changed);
      PACL changed_acl = nullptr;
      BOOL present = FALSE, defaulted = FALSE;
      ASSERT_TRUE(GetSecurityDescriptorDacl(changed.data(), &present, &changed_acl, &defaulted));
      ASSERT_TRUE(present && changed_acl);
      std::vector<std::uint8_t> changed_policy;
      std::filesystem::path backup;
      bool mutated = false, observed_drift = false, prepared = false;
      http::test_support::set_state_security_hook([&](auto, const auto &) {
        prepared = true;
      });
      http::test_support::set_state_file_hook([&](auto stage, const auto &path, const auto &temporary, const auto &old_backup) -> std::optional<std::uint32_t> {
        if (stage == boundary) {
          EXPECT_FALSE(std::filesystem::exists(temporary));
          if (change_control) {
            auto name = path.wstring();
            const auto result = SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, changed_acl, nullptr);
            EXPECT_EQ(result, ERROR_SUCCESS);
            mutated = result == ERROR_SUCCESS;
          } else {
            mutated = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, changed.data());
          }
          EXPECT_TRUE(mutated);
          changed_policy = security_snapshot(path);
          const bool valid_authority = same_security(changed_policy, changed_policy);
          EXPECT_TRUE(valid_authority);
          const bool same_authority = same_security(original.security, changed_policy);
          EXPECT_FALSE(same_authority);
          const bool control_changed = !change_control || unprotected(changed_policy);
          EXPECT_TRUE(control_changed);
          observed_drift = valid_authority && !same_authority && control_changed;
          backup = old_backup;
        }
        return std::nullopt;
      });
      const bool saved = http::update_state_file(destination.string(), [](auto &tree) {
        tree.put("fixture", "unpublished authority drift state");
      });
      http::test_support::set_state_file_hook({});
      http::test_support::set_state_security_hook({});
      ASSERT_TRUE(mutated);
      ASSERT_TRUE(observed_drift);
      EXPECT_FALSE(saved);
      EXPECT_FALSE(prepared);
      auto externally_changed = original;
      externally_changed.security = changed_policy;
      expect_fixture_snapshot(destination, externally_changed);
      expect_fixture_snapshot(backup, externally_changed);
      EXPECT_FALSE(http::update_state_file(destination.string(), [](auto &tree) {
        tree.put("fixture", "blocked after authority drift");
      }));
      expect_fixture_snapshot(destination, externally_changed);
    }
  }
}

TEST_F(StateFileTransactionTest, HandoffRefusesDeletedOrSubstitutedBackupBeforePreparingPayload) {
  using stage_t = http::test_support::state_file_stage_e;
  unsigned next = 0;
  for (const auto boundary : {stage_t::guard_open, stage_t::guard_close}) {
    for (const bool substitute : {false, true}) {
      SCOPED_TRACE(static_cast<int>(boundary));
      SCOPED_TRACE(substitute);
      const auto suffix = std::to_string(++next);
      const auto destination = directory / ("backup-target-" + suffix + ".json");
      const auto external = directory / ("backup-external-" + suffix + ".json");
      const auto displaced = directory / ("displaced-backup-" + suffix + ".json");
      ASSERT_TRUE(http::update_state_file(destination.string(), [](auto &tree) {
        tree.put("fixture", "original backup target");
      }));
      ASSERT_TRUE(http::update_state_file(external.string(), [](auto &tree) {
        tree.put("fixture", "unrelated backup substitute");
      }));
      const auto original = fixture_snapshot(destination);
      const auto unrelated = fixture_snapshot(external);
      std::filesystem::path backup, temporary;
      bool changed = false, prepared = false, published = false;
      http::test_support::set_state_security_hook([&](auto, const auto &) {
        prepared = true;
      });
      http::test_support::set_state_file_hook([&](auto stage, const auto &, const auto &candidate, const auto &old_backup) -> std::optional<std::uint32_t> {
        if (stage == stage_t::write || stage == stage_t::rename || stage == stage_t::rename_api || stage == stage_t::verify) {
          published = true;
        }
        if (stage == boundary) {
          backup = old_backup;
          temporary = candidate;
          EXPECT_FALSE(std::filesystem::exists(temporary));
          if (substitute) {
            changed = MoveFileExW(backup.c_str(), displaced.c_str(), MOVEFILE_WRITE_THROUGH) && CreateHardLinkW(backup.c_str(), external.c_str(), nullptr);
          } else {
            changed = DeleteFileW(backup.c_str());
          }
          EXPECT_TRUE(changed);
        }
        return std::nullopt;
      });
      EXPECT_FALSE(http::update_state_file(destination.string(), [](auto &tree) {
        tree.put("fixture", "unpublished after backup interference");
      }));
      http::test_support::set_state_file_hook({});
      http::test_support::set_state_security_hook({});
      ASSERT_TRUE(changed);
      EXPECT_FALSE(prepared || published);
      EXPECT_FALSE(std::filesystem::exists(temporary));
      expect_fixture_snapshot(destination, original);
      expect_fixture_snapshot(external, unrelated);
      if (substitute) {
        expect_fixture_snapshot(backup, unrelated);
        expect_fixture_snapshot(displaced, original);
      } else {
        EXPECT_FALSE(std::filesystem::exists(backup));
      }
    }
  }
}

TEST_F(StateFileTransactionTest, HandoffNeverOverwritesUnrelatedDestinationAndRetainsTheOriginalBackup) {
  using stage_t = http::test_support::state_file_stage_e;
  for (const auto boundary : {stage_t::guard_open, stage_t::guard_close}) {
    SCOPED_TRACE(static_cast<int>(boundary));
    const auto suffix = std::to_string(static_cast<int>(boundary));
    const auto destination = directory / ("destination-target-" + suffix + ".json");
    const auto external = directory / ("destination-external-" + suffix + ".json");
    const auto displaced = directory / ("displaced-destination-" + suffix + ".json");
    ASSERT_TRUE(http::update_state_file(destination.string(), [](auto &tree) {
      tree.put("fixture", "original handoff destination");
    }));
    ASSERT_TRUE(http::update_state_file(external.string(), [](auto &tree) {
      tree.put("fixture", "unrelated handoff destination");
    }));
    const auto original = fixture_snapshot(destination);
    const auto unrelated = fixture_snapshot(external);
    std::filesystem::path backup, temporary;
    bool substituted = false, prepared = false;
    http::test_support::set_state_security_hook([&](auto, const auto &) {
      prepared = true;
    });
    http::test_support::set_state_file_hook([&](auto stage, const auto &path, const auto &candidate, const auto &old_backup) -> std::optional<std::uint32_t> {
      if (stage == boundary) {
        backup = old_backup;
        temporary = candidate;
        EXPECT_FALSE(std::filesystem::exists(temporary));
        substituted = MoveFileExW(path.c_str(), displaced.c_str(), MOVEFILE_WRITE_THROUGH) && CreateHardLinkW(path.c_str(), external.c_str(), nullptr);
        EXPECT_TRUE(substituted);
      }
      return std::nullopt;
    });
    EXPECT_FALSE(http::update_state_file(destination.string(), [](auto &tree) {
      tree.put("fixture", "unpublished after destination interference");
    }));
    http::test_support::set_state_file_hook({});
    http::test_support::set_state_security_hook({});
    ASSERT_TRUE(substituted);
    EXPECT_FALSE(prepared);
    EXPECT_FALSE(std::filesystem::exists(temporary));
    expect_fixture_snapshot(destination, unrelated);
    expect_fixture_snapshot(external, unrelated);
    expect_fixture_snapshot(displaced, original);
    expect_fixture_snapshot(backup, original);
    EXPECT_FALSE(http::update_state_file(destination.string(), [](auto &tree) {
      tree.put("fixture", "must remain blocked after destination interference");
    }));
    expect_fixture_snapshot(destination, unrelated);
    expect_fixture_snapshot(backup, original);
  }
}

TEST_F(StateFileTransactionTest, InitialAbsencePerformsNoOriginalGuardHandoff) {
  using stage_t = http::test_support::state_file_stage_e;
  const auto destination = directory / "no-original-handoff.json";
  ASSERT_FALSE(std::filesystem::exists(destination));
  bool handoff = false;
  http::test_support::set_state_file_hook([&](auto stage, const auto &, const auto &, const auto &) -> std::optional<std::uint32_t> {
    if (stage == stage_t::guard_open || stage == stage_t::guard_close) {
      handoff = true;
      return ERROR_INVALID_HANDLE;
    }
    return std::nullopt;
  });
  EXPECT_TRUE(http::update_state_file(destination.string(), [](auto &tree) {
    tree.put("fixture", "initial creation without handoff");
  }));
  http::test_support::set_state_file_hook({});
  EXPECT_FALSE(handoff);
  EXPECT_TRUE(read_fixture_bytes(destination).contains("initial creation without handoff"));
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 2);
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

/** @brief Observe real first-save rename metadata in an exclusively owned system-temp fixture. */
class StateFileCreationTimeTunnelingTest: public StateFileTransactionTest {
protected:
  /** @brief Independently observed components of one plain fixture file. */
  struct snapshot_t {
    BY_HANDLE_FILE_INFORMATION identity {};  ///< Volume and file identity used for ownership checks.
    FILE_BASIC_INFO basic {};  ///< Exact creation time and ordinary attributes.
    std::string bytes;  ///< Synthetic unnamed payload, never printed in diagnostics.
    std::vector<std::uint8_t> security;  ///< Complete owner/group/DACL snapshot.
    std::array<std::vector<std::uint8_t>, 3> selected;  ///< Label, resource attributes and central policy.
  };

  /** @brief Read-only observations surrounding a real production rename. */
  struct attempt_t {
    std::optional<snapshot_t> prepared, committed;  ///< Closed temporary and renamed candidate.
    bool saved = false;  ///< Unmodified production result.
    bool closed = false;  ///< Real source-handle close was reached.
    bool verified = false;  ///< Final production verification was reached.
  };

  /** @brief Actual final metadata mismatches, applied only through a verified fixture handle. */
  enum class verification_fault_e {
    none,  ///< Observe without changing the committed candidate.
    attributes,  ///< Change one supported ordinary attribute.
    creation_time  ///< Change an existing-original candidate's creation time.
  };

  HANDLE directory_handle = INVALID_HANDLE_VALUE;  ///< Held identity of the uniquely created fixture directory.
  BY_HANDLE_FILE_INFORMATION directory_identity {};  ///< Exact created directory identity.
  std::vector<std::pair<std::filesystem::path, BY_HANDLE_FILE_INFORMATION>> owned;  ///< Exact entries eligible for nonrecursive cleanup.
  std::filesystem::path sentinel, retry;  ///< Fixed leaves; no external fixture paths are accepted.
  std::optional<snapshot_t> sentinel_original;  ///< Untouched sibling, including exact creation time.
  static constexpr std::array<DWORD, 3> selectors {LABEL_SECURITY_INFORMATION, ATTRIBUTE_SECURITY_INFORMATION, SCOPE_SECURITY_INFORMATION};  ///< Existing production selector coverage.

  /** @brief Compare only the native identity fields used by production and cleanup ownership. */
  bool same_identity(const BY_HANDLE_FILE_INFORMATION &left, const BY_HANDLE_FILE_INFORMATION &right) {
    return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber && left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow;
  }

  /** @brief Refuse missing, non-directory or reparse ancestors before touching an external fixture. */
  bool plain_ancestors(std::filesystem::path path) {
    for (;;) {
      const auto attributes = GetFileAttributesW(path.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return false;
      }
      if (path == path.root_path()) {
        return true;
      }
      path = path.parent_path();
    }
  }

  /** @brief Require the current plain directory path and held handle to identify the exact created directory. */
  bool verified_directory() {
    if (directory_handle == INVALID_HANDLE_VALUE || !plain_ancestors(directory)) {
      return false;
    }
    BY_HANDLE_FILE_INFORMATION held {}, named {};
    if (!GetFileInformationByHandle(directory_handle, &held) || !same_identity(directory_identity, held) || !(held.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (held.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      return false;
    }
    HANDLE handle = CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return false;
    }
    auto close = util::fail_guard([&]() {
      EXPECT_TRUE(CloseHandle(handle));
    });
    return GetFileInformationByHandle(handle, &named) && same_identity(directory_identity, named) && (named.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(named.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
  }

  /** @brief Change only the owned directory handle mode, checking the exact pathname before and after reopening. */
  void reopen_directory(DWORD access, DWORD sharing) {
    ASSERT_TRUE(verified_directory());
    const auto closing = std::exchange(directory_handle, INVALID_HANDLE_VALUE);
    ASSERT_TRUE(CloseHandle(closing));
    directory_handle = CreateFileW(directory.c_str(), access, sharing, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    ASSERT_NE(directory_handle, INVALID_HANDLE_VALUE);
    ASSERT_TRUE(verified_directory());
  }

  /** @brief Admit only fixed fixture leaves and production temporary names in the guarded directory. */
  bool fixture_path(const std::filesystem::path &path) {
    const auto leaf = path.filename().string();
    return path.parent_path() == directory && (path == file || path == retry || path == sentinel || leaf.starts_with(file.filename().string() + ".tmp-") || leaf.starts_with(retry.filename().string() + ".tmp-")) && verified_directory();
  }

  /** @brief Remember a newly observed owned entry without allowing links or reparse objects. */
  void remember(const std::filesystem::path &path, const BY_HANDLE_FILE_INFORMATION &identity) {
    ASSERT_TRUE(fixture_path(path));
    ASSERT_EQ(identity.dwVolumeSerialNumber, directory_identity.dwVolumeSerialNumber);
    ASSERT_EQ(identity.nNumberOfLinks, 1U);
    ASSERT_EQ(identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT), 0U);
    const auto found = std::find_if(owned.begin(), owned.end(), [&](const auto &entry) {
      return entry.first == path;
    });
    if (found == owned.end()) {
      owned.emplace_back(path, identity);
    } else {
      found->second = identity;
    }
  }

  /** @brief Capture fixture components without mutating timestamps, attributes or authority. */
  std::optional<snapshot_t> capture(const std::filesystem::path &path) {
    if (!fixture_path(path)) {
      ADD_FAILURE() << "Snapshot refused a path outside the owned fixture";
      return std::nullopt;
    }
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      ADD_FAILURE() << "Fixture snapshot could not open: " << GetLastError();
      return std::nullopt;
    }
    auto close = util::fail_guard([&]() {
      EXPECT_TRUE(CloseHandle(handle));
    });
    snapshot_t result;
    if (!GetFileInformationByHandle(handle, &result.identity) || !GetFileInformationByHandleEx(handle, FileBasicInfo, &result.basic, sizeof(result.basic)) || result.identity.nNumberOfLinks != 1 || (result.basic.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
      ADD_FAILURE() << "Fixture snapshot requires one plain owned file";
      return std::nullopt;
    }
    result.bytes = read_fixture_handle(handle);
    result.security = security_snapshot(path);
    if (!same_security(result.security, result.security)) {
      ADD_FAILURE() << "Fixture owner/group/DACL snapshot is unavailable";
      return std::nullopt;
    }
    for (std::size_t index = 0; index < selectors.size(); ++index) {
      result.selected[index] = security_snapshot(path, selectors[index]);
      if (!http::test_support::state_selected_security_matches(result.selected[index], result.selected[index], index == 2)) {
        ADD_FAILURE() << "Fixture selected-policy snapshot is unavailable: " << index;
        return std::nullopt;
      }
    }
    return result;
  }

  /** @brief Create synthetic JSON with an optional known creation time using CREATE_NEW only. */
  void create_fixture(const std::filesystem::path &path, std::uint64_t creation = 0) {
    ASSERT_TRUE(fixture_path(path));
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    auto close = util::fail_guard([&]() {
      EXPECT_TRUE(CloseHandle(handle));
    });
    BY_HANDLE_FILE_INFORMATION identity {};
    ASSERT_TRUE(GetFileInformationByHandle(handle, &identity));
    remember(path, identity);
    constexpr char bytes[] = "{\"fixture\":\"untouched synthetic state\"}\n";
    DWORD written = 0;
    ASSERT_TRUE(WriteFile(handle, bytes, sizeof(bytes) - 1, &written, nullptr));
    ASSERT_EQ(written, sizeof(bytes) - 1);
    ASSERT_TRUE(FlushFileBuffers(handle));
    if (creation) {
      const FILETIME time {static_cast<DWORD>(creation), static_cast<DWORD>(creation >> 32)};
      ASSERT_TRUE(SetFileTime(handle, &time, nullptr, nullptr));
    }
  }

  /** @brief Delete only the opened, identity-checked known fixture object; never recurse or follow links. */
  void remove_owned(const std::filesystem::path &path, const BY_HANDLE_FILE_INFORMATION &expected) {
    ASSERT_TRUE(fixture_path(path));
    ASSERT_TRUE(plain_ancestors(directory));
    HANDLE handle = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    auto close = util::fail_guard([&]() {
      EXPECT_TRUE(CloseHandle(handle));
    });
    BY_HANDLE_FILE_INFORMATION current {};
    ASSERT_TRUE(GetFileInformationByHandle(handle, &current));
    ASSERT_TRUE(same_identity(expected, current));
    ASSERT_EQ(current.nNumberOfLinks, 1U);
    ASSERT_EQ(current.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT), 0U);
    FILE_DISPOSITION_INFO disposition {TRUE};
    ASSERT_TRUE(SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)));
  }

  /** @brief Distinguish genuine absence from an inaccessible or delete-pending entry. */
  bool missing(const std::filesystem::path &path) {
    if (!fixture_path(path)) {
      ADD_FAILURE() << "Absence inspection refused an unverified fixture path";
      return false;
    }
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
  }

  void SetUp() override {
    BaseTest::SetUp();
    const auto parent = std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
    ASSERT_TRUE(plain_ancestors(parent));
    static std::size_t next_fixture = 0;
    directory = parent / ("miranda-state-tunneling-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(++next_fixture));
    ASSERT_TRUE(CreateDirectoryW(directory.c_str(), nullptr));
    directory_handle = CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    ASSERT_NE(directory_handle, INVALID_HANDLE_VALUE);
    ASSERT_TRUE(GetFileInformationByHandle(directory_handle, &directory_identity));
    ASSERT_TRUE(directory_identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    ASSERT_FALSE(directory_identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    file = directory / "initial-state-long-name.json";
    retry = directory / "close-retry-state-long-name.json";
    sentinel = directory / "untouched-sentinel.json";
    create_fixture(sentinel);
    ASSERT_FALSE(HasFatalFailure());
    sentinel_original = capture(sentinel);
    ASSERT_TRUE(sentinel_original.has_value());
    std::array<wchar_t, 64> filesystem {};
    DWORD flags = 0;
    ASSERT_TRUE(GetVolumeInformationByHandleW(directory_handle, nullptr, 0, nullptr, nullptr, &flags, filesystem.data(), static_cast<DWORD>(filesystem.size())));
    const std::wstring provider {filesystem.data()};
    RecordProperty("filesystem", std::string(provider.begin(), provider.end()));
    RecordProperty("hard_links_supported", (flags & FILE_SUPPORTS_HARD_LINKS) ? "true" : "false");
    if (provider != L"NTFS" || !(flags & FILE_SUPPORTS_HARD_LINKS)) {
      GTEST_SKIP() << "Actual-writer tunneling is unexercised: system temporary provider is outside the production NTFS contract";
    }
  }

  void TearDown() override {
    http::test_support::set_state_file_hook({});
    if (directory_handle != INVALID_HANDLE_VALUE) {
      if (verified_directory()) {
        for (const auto &[path, identity] : owned) {
          if (!missing(path)) {
            remove_owned(path, identity);
          }
        }
        std::error_code error;
        if (verified_directory() && std::filesystem::is_empty(directory, error) && !error) {
          HANDLE deleting = CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
          if (deleting != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION current {};
            if (GetFileInformationByHandle(deleting, &current) && same_identity(directory_identity, current) && (current.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(current.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
              FILE_DISPOSITION_INFO disposition {TRUE};
              EXPECT_TRUE(SetFileInformationByHandle(deleting, FileDispositionInfo, &disposition, sizeof(disposition)));
            } else {
              ADD_FAILURE() << "Fixture directory deletion refused an unverified opened identity";
            }
            EXPECT_TRUE(CloseHandle(deleting));
          } else {
            ADD_FAILURE() << "Fixture directory deletion could not open its known identity";
          }
        } else {
          ADD_FAILURE() << "Fixture cleanup retained an unverified or nonempty directory";
        }
      } else {
        ADD_FAILURE() << "Fixture cleanup refused a substituted or unverified directory path";
      }
      EXPECT_TRUE(CloseHandle(directory_handle));
    }
    BaseTest::TearDown();
  }

  /** @brief Assert exact identity, payload and complete authority using the production policy gates. */
  void expect_authority_and_payload(const snapshot_t &expected, const snapshot_t &current) {
    EXPECT_TRUE(same_identity(expected.identity, current.identity));
    EXPECT_TRUE(expected.bytes == current.bytes);
    EXPECT_TRUE(same_security(expected.security, current.security)) << security_difference(expected.security, current.security);
    for (std::size_t index = 0; index < selectors.size(); ++index) {
      SCOPED_TRACE(index);
      EXPECT_TRUE(http::test_support::state_selected_security_matches(expected.selected[index], current.selected[index], index == 2));
    }
  }

  /** @brief Assert every non-time production component separately; no snapshot acceptance predicate is relaxed. */
  void expect_components(const snapshot_t &expected, const snapshot_t &current) {
    expect_authority_and_payload(expected, current);
    EXPECT_EQ(expected.basic.FileAttributes, current.basic.FileAttributes);
  }

  /** @brief Require the untouched sibling's exact identity, bytes, attributes, authority and creation time. */
  void expect_sentinel_unchanged() {
    const auto current = capture(sentinel);
    ASSERT_TRUE(sentinel_original.has_value() && current.has_value());
    expect_components(*sentinel_original, *current);
    EXPECT_EQ(sentinel_original->basic.CreationTime.QuadPart, current->basic.CreationTime.QuadPart);
  }

  /** @brief Record short-alias presence without changing volume settings or printing fixture paths. */
  void record_alias(const std::filesystem::path &path, const std::string &property) {
    ASSERT_TRUE(fixture_path(path));
    std::array<wchar_t, 32768> name {};
    const auto length = GetShortPathNameW(path.c_str(), name.data(), static_cast<DWORD>(name.size()));
    ASSERT_TRUE(length && length < name.size());
    RecordProperty(property, std::filesystem::path {name.data()}.filename() != path.filename() ? "true" : "false");
  }

  /** @brief Alter only known candidate metadata to prove that final verification still rejects real mismatches. */
  void alter_committed_metadata(const std::filesystem::path &path, const snapshot_t &expected, verification_fault_e fault) {
    ASSERT_TRUE(fixture_path(path));
    ASSERT_TRUE(plain_ancestors(directory));
    HANDLE handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    auto close = util::fail_guard([&]() {
      EXPECT_TRUE(CloseHandle(handle));
    });
    BY_HANDLE_FILE_INFORMATION identity {};
    ASSERT_TRUE(GetFileInformationByHandle(handle, &identity));
    ASSERT_TRUE(same_identity(expected.identity, identity));
    ASSERT_EQ(identity.nNumberOfLinks, 1U);
    ASSERT_EQ(identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT), 0U);
    FILE_BASIC_INFO apply {};
    if (fault == verification_fault_e::attributes) {
      apply.FileAttributes = (expected.basic.FileAttributes ^ FILE_ATTRIBUTE_HIDDEN) & ~FILE_ATTRIBUTE_NORMAL;
      if (!apply.FileAttributes) {
        apply.FileAttributes = FILE_ATTRIBUTE_NORMAL;
      }
    } else {
      ASSERT_EQ(fault, verification_fault_e::creation_time);
      apply.CreationTime.QuadPart = 132223104010000000LL;
      ASSERT_NE(apply.CreationTime.QuadPart, expected.basic.CreationTime.QuadPart);
    }
    ASSERT_TRUE(SetFileInformationByHandle(handle, FileBasicInfo, &apply, sizeof(apply)));
  }

  /** @brief Observe real rename stages, with explicit close or verified final-metadata faults for negative cases. */
  attempt_t save(const std::filesystem::path &target, bool fault_close = false, verification_fault_e verification_fault = verification_fault_e::none) {
    using stage_t = http::test_support::state_file_stage_e;
    attempt_t attempt;
    auto clear = util::fail_guard([]() {
      http::test_support::set_state_file_hook({});
    });
    http::test_support::set_state_file_hook([&](auto stage, const auto &destination, const auto &temporary, const auto &) -> std::optional<std::uint32_t> {
      if (stage == stage_t::rename) {
        EXPECT_TRUE(destination == target);
        attempt.prepared = capture(temporary);
        if (attempt.prepared) {
          remember(temporary, attempt.prepared->identity);
        }
      }
      if (stage == stage_t::rename_close) {
        attempt.closed = true;
      }
      if (stage == stage_t::verify || (fault_close && stage == stage_t::rename_close)) {
        attempt.verified = stage == stage_t::verify;
        if (stage == stage_t::verify && attempt.prepared && verification_fault != verification_fault_e::none) {
          alter_committed_metadata(destination, *attempt.prepared, verification_fault);
        }
        attempt.committed = capture(destination);
        if (attempt.prepared && attempt.committed && same_identity(attempt.prepared->identity, attempt.committed->identity)) {
          remember(destination, attempt.committed->identity);
          record_alias(destination, fault_close ? "faulted_name_short_alias" : "committed_name_short_alias");
        }
      }
      return fault_close && stage == stage_t::rename_close ? std::optional<std::uint32_t> {ERROR_WRITE_FAULT} : std::nullopt;
    });
    attempt.saved = http::update_state_file(target.string(), [](auto &tree) {
      tree.put("fixture", "actual writer candidate");
    });
    return attempt;
  }

  /** @brief Require successful first-save publication while recording any native creation-time tunneling. */
  void expect_clean_attempt(const std::filesystem::path &target, const attempt_t &attempt, LONGLONG removed_creation) {
    ASSERT_TRUE(attempt.prepared.has_value() && attempt.committed.has_value());
    ASSERT_TRUE(attempt.closed && attempt.verified);
    expect_components(*attempt.prepared, *attempt.committed);
    EXPECT_TRUE(attempt.prepared->bytes.contains("actual writer candidate"));
    RecordProperty("removed_creation", std::to_string(removed_creation));
    RecordProperty("prepared_creation", std::to_string(attempt.prepared->basic.CreationTime.QuadPart));
    RecordProperty("committed_creation", std::to_string(attempt.committed->basic.CreationTime.QuadPart));
    RecordProperty("save_accepted", attempt.saved ? "true" : "false");
    const bool tunneled = attempt.committed->basic.CreationTime.QuadPart == removed_creation && attempt.prepared->basic.CreationTime.QuadPart != removed_creation;
    RecordProperty("tunneled_removed_creation", tunneled ? "true" : "false");
    if (!tunneled) {
      EXPECT_EQ(attempt.prepared->basic.CreationTime.QuadPart, attempt.committed->basic.CreationTime.QuadPart);
    }
    ASSERT_TRUE(attempt.saved);
    const auto published = capture(target);
    ASSERT_TRUE(published.has_value());
    expect_components(*attempt.committed, *published);
    EXPECT_EQ(attempt.committed->basic.CreationTime.QuadPart, published->basic.CreationTime.QuadPart);
    expect_sentinel_unchanged();
  }
};

TEST_F(StateFileCreationTimeTunnelingTest, SeededRemovedCreationTimeAllowsActualFirstSaveWithExactAuthorityAndPayload) {
  constexpr std::uint64_t old_creation = 132223104000000000ULL;
  create_fixture(file, old_creation);
  ASSERT_FALSE(HasFatalFailure());
  const auto original = capture(file);
  ASSERT_TRUE(original.has_value());
  ASSERT_EQ(original->basic.CreationTime.QuadPart, static_cast<LONGLONG>(old_creation));
  record_alias(file, "removed_name_short_alias");
  remove_owned(file, original->identity);
  ASSERT_FALSE(HasFatalFailure());
  ASSERT_TRUE(missing(file));
  const auto attempt = save(file);
  expect_clean_attempt(file, attempt, static_cast<LONGLONG>(old_creation));
  ASSERT_FALSE(HasFailure());
  if (attempt.prepared->basic.CreationTime.QuadPart == attempt.committed->basic.CreationTime.QuadPart) {
    GTEST_SKIP() << "Actual-writer creation-time tunneling is unexercised: the removed name did not replace the prepared creation time";
  }
}

TEST_F(StateFileCreationTimeTunnelingTest, RenameCloseFaultRestoresAbsenceThenCleanRetryPublishesExactAuthorityAndPayload) {
  ASSERT_TRUE(missing(retry));
  const auto faulted = save(retry, true);
  ASSERT_TRUE(faulted.prepared.has_value() && faulted.committed.has_value());
  ASSERT_TRUE(faulted.closed);
  EXPECT_FALSE(faulted.verified);
  EXPECT_FALSE(faulted.saved);
  expect_components(*faulted.prepared, *faulted.committed);
  EXPECT_EQ(faulted.prepared->basic.CreationTime.QuadPart, faulted.committed->basic.CreationTime.QuadPart);
  RecordProperty("faulted_prepared_creation", std::to_string(faulted.prepared->basic.CreationTime.QuadPart));
  RecordProperty("faulted_committed_creation", std::to_string(faulted.committed->basic.CreationTime.QuadPart));
  ASSERT_TRUE(missing(retry));
  expect_sentinel_unchanged();
  const auto clean = save(retry);
  expect_clean_attempt(retry, clean, faulted.committed->basic.CreationTime.QuadPart);
  ASSERT_FALSE(HasFailure());
  if (clean.prepared->basic.CreationTime.QuadPart == clean.committed->basic.CreationTime.QuadPart) {
    GTEST_SKIP() << "Actual-writer close-fault/retry tunneling is unexercised: the retry retained its prepared creation time";
  }
}

TEST_F(StateFileCreationTimeTunnelingTest, InitialAbsenceOrdinaryAttributeMismatchStillRejectsAndRestoresAbsence) {
  constexpr std::uint64_t old_creation = 132223104000000000ULL;
  create_fixture(file, old_creation);
  ASSERT_FALSE(HasFatalFailure());
  const auto original = capture(file);
  ASSERT_TRUE(original.has_value());
  ASSERT_EQ(original->basic.CreationTime.QuadPart, static_cast<LONGLONG>(old_creation));
  remove_owned(file, original->identity);
  ASSERT_FALSE(HasFatalFailure());
  ASSERT_TRUE(missing(file));
  const auto attempt = save(file, false, verification_fault_e::attributes);
  ASSERT_TRUE(attempt.prepared.has_value() && attempt.committed.has_value());
  ASSERT_TRUE(attempt.closed && attempt.verified);
  expect_authority_and_payload(*attempt.prepared, *attempt.committed);
  EXPECT_NE(attempt.prepared->basic.FileAttributes, attempt.committed->basic.FileAttributes);
  EXPECT_FALSE(attempt.saved);
  EXPECT_TRUE(missing(file));
  expect_sentinel_unchanged();
}

TEST_F(StateFileCreationTimeTunnelingTest, ExistingOriginalCreationTimeMismatchStillRejectsAndRestoresExactOriginal) {
  constexpr std::uint64_t old_creation = 132223104000000000ULL;
  create_fixture(file, old_creation);
  ASSERT_FALSE(HasFatalFailure());
  const auto original = capture(file);
  ASSERT_TRUE(original.has_value());
  ASSERT_EQ(original->basic.CreationTime.QuadPart, static_cast<LONGLONG>(old_creation));
  const auto attempt = save(file, false, verification_fault_e::creation_time);
  ASSERT_TRUE(attempt.prepared.has_value() && attempt.committed.has_value());
  ASSERT_TRUE(attempt.closed && attempt.verified);
  expect_components(*attempt.prepared, *attempt.committed);
  EXPECT_NE(attempt.prepared->basic.CreationTime.QuadPart, attempt.committed->basic.CreationTime.QuadPart);
  EXPECT_EQ(attempt.prepared->basic.CreationTime.QuadPart, original->basic.CreationTime.QuadPart);
  EXPECT_FALSE(attempt.saved);
  const auto restored = capture(file);
  ASSERT_TRUE(restored.has_value());
  expect_components(*original, *restored);
  EXPECT_EQ(original->basic.CreationTime.QuadPart, restored->basic.CreationTime.QuadPart);
  ASSERT_FALSE(HasFailure());
  remember(file, restored->identity);
  expect_sentinel_unchanged();
}

TEST_F(StateFileCreationTimeTunnelingTest, ParentHandleComparisonRequiresThePermissiveActualWriterToPublish) {
  ASSERT_TRUE(missing(file));
  ASSERT_TRUE(missing(retry));
  reopen_directory(FILE_READ_ATTRIBUTES | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE);
  ASSERT_FALSE(HasFatalFailure());
  const auto restricted = save(file);
  reopen_directory(FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
  ASSERT_FALSE(HasFatalFailure());
  const auto permissive = save(retry);
  RecordProperty("restricted_writer_saved", restricted.saved ? "true" : "false");
  RecordProperty("restricted_prepared_observed", restricted.prepared ? "true" : "false");
  RecordProperty("restricted_close_stage_observed", restricted.closed ? "true" : "false");
  RecordProperty("restricted_verify_stage_observed", restricted.verified ? "true" : "false");
  RecordProperty("permissive_writer_saved", permissive.saved ? "true" : "false");
  ASSERT_TRUE(permissive.saved);
  ASSERT_TRUE(permissive.prepared.has_value() && permissive.committed.has_value());
  ASSERT_TRUE(permissive.closed && permissive.verified);
  expect_components(*permissive.prepared, *permissive.committed);
  const auto published = capture(retry);
  ASSERT_TRUE(published.has_value());
  expect_components(*permissive.committed, *published);
  EXPECT_EQ(permissive.committed->basic.CreationTime.QuadPart, published->basic.CreationTime.QuadPart);
  if (restricted.saved) {
    ASSERT_TRUE(restricted.prepared.has_value() && restricted.committed.has_value());
    ASSERT_TRUE(restricted.closed && restricted.verified);
    expect_components(*restricted.prepared, *restricted.committed);
    const auto old_published = capture(file);
    ASSERT_TRUE(old_published.has_value());
    expect_components(*restricted.committed, *old_published);
    EXPECT_EQ(restricted.committed->basic.CreationTime.QuadPart, old_published->basic.CreationTime.QuadPart);
  } else {
    ASSERT_TRUE(restricted.prepared.has_value());
    EXPECT_TRUE(restricted.closed);
    EXPECT_FALSE(restricted.verified);
    EXPECT_FALSE(restricted.committed.has_value());
    EXPECT_TRUE(missing(file));
  }
  expect_sentinel_unchanged();
  ASSERT_FALSE(HasFailure());
  if (restricted.saved) {
    GTEST_SKIP() << "Parent restriction comparison is unexercised: both one-shot actual writer attempts published";
  }
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

TEST_F(StateFileTransactionTest, ByteReadOnlyAndThrowingPreparationDoNotCreateTemporaries) {
  const auto original = state_bytes();
  unsigned calls = 0;
  bool reached_temporary = false;
  http::test_support::set_state_update_hook([&]() {
    reached_temporary = true;
  });
  EXPECT_TRUE(http::update_state_file_bytes(file.string(), [&](const std::string &bytes) -> std::optional<std::string> {
    ++calls;
    EXPECT_EQ(bytes, original);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 1);
    return std::nullopt;
  }));
  EXPECT_EQ(calls, 1U);
  EXPECT_FALSE(reached_temporary);
  EXPECT_EQ(state_bytes(), original);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 1);
  calls = 0;
  EXPECT_FALSE(http::update_state_file_bytes(file.string(), [&](const auto &bytes) -> std::optional<std::string> {
    ++calls;
    EXPECT_EQ(bytes, original);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 1);
    throw std::runtime_error {"fixture preparation failure"};
  }));
  EXPECT_EQ(calls, 1U);
  EXPECT_FALSE(reached_temporary);
  EXPECT_EQ(state_bytes(), original);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 1);
  const auto missing = directory / "throwing-missing.bytes";
  calls = 0;
  EXPECT_FALSE(http::update_state_file_bytes(missing.string(), [&](const auto &bytes) -> std::optional<std::string> {
    ++calls;
    EXPECT_TRUE(bytes.empty());
    EXPECT_FALSE(std::filesystem::exists(missing));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 1);
    throw std::runtime_error {"fixture missing-file preparation failure"};
  }));
  EXPECT_EQ(calls, 1U);
  EXPECT_FALSE(reached_temporary);
  EXPECT_FALSE(std::filesystem::exists(missing));
  EXPECT_EQ(state_bytes(), original);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 1);
}

TEST_F(StateFileTransactionTest, MissingExistingEmptyAndExplicitEmptyReplacementRemainDistinct) {
  const auto missing = directory / "missing.json";
  unsigned calls = 0;
  EXPECT_TRUE(http::update_state_file_bytes(missing.string(), [&](const auto &bytes) -> std::optional<std::string> {
    ++calls;
    EXPECT_TRUE(bytes.empty());
    return std::nullopt;
  }));
  EXPECT_EQ(calls, 1U);
  EXPECT_FALSE(std::filesystem::exists(missing));
  EXPECT_TRUE(http::update_state_file_bytes(file.string(), [](const auto &) -> std::optional<std::string> {
    return std::string {};
  }));
  EXPECT_TRUE(std::filesystem::exists(file));
  EXPECT_EQ(std::filesystem::file_size(file), 0);
  EXPECT_TRUE(http::update_state_file_bytes(file.string(), [](const auto &bytes) -> std::optional<std::string> {
    EXPECT_TRUE(bytes.empty());
    return std::nullopt;
  }));
  bool updated_invalid_json = false;
  EXPECT_FALSE(http::update_state_file(file.string(), [&](auto &tree) {
    updated_invalid_json = true;
    tree.put("fixture", "must not replace invalid empty JSON");
  }));
  EXPECT_FALSE(updated_invalid_json);
  EXPECT_EQ(std::filesystem::file_size(file), 0);
  calls = 0;
  EXPECT_TRUE(http::update_state_file_bytes(missing.string(), [&](const auto &bytes) -> std::optional<std::string> {
    ++calls;
    EXPECT_TRUE(bytes.empty());
    return std::string {};
  }));
  EXPECT_EQ(calls, 1U);
  EXPECT_TRUE(std::filesystem::exists(missing));
  EXPECT_EQ(std::filesystem::file_size(missing), 0);
}

TEST_F(StateFileTransactionTest, BytePreparationReceivesCompleteOpaqueBytesOnceAndCommitsItsExactReplacement) {
  const std::string original {"opaque\0captured\r\nbytes", sizeof("opaque\0captured\r\nbytes") - 1};
  const std::string replacement {"replacement\0payload\r\n", sizeof("replacement\0payload\r\n") - 1};
  {
    std::ofstream output {file, std::ios::binary};
    output.write(original.data(), static_cast<std::streamsize>(original.size()));
    ASSERT_TRUE(output.good());
  }
  unsigned calls = 0;
  EXPECT_TRUE(http::update_state_file_bytes(file.string(), [&](const auto &bytes) -> std::optional<std::string> {
    ++calls;
    EXPECT_TRUE(bytes == original);
#ifdef _WIN32
    HANDLE writer = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const auto error = writer == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    EXPECT_EQ(writer, INVALID_HANDLE_VALUE);
    EXPECT_EQ(error, ERROR_SHARING_VIOLATION);
    if (writer != INVALID_HANDLE_VALUE) {
      EXPECT_TRUE(CloseHandle(writer));
    }
#endif
    return replacement;
  }));
  EXPECT_EQ(calls, 1U);
  EXPECT_TRUE(state_bytes() == replacement);
}

#ifndef _WIN32
TEST_F(StateFileTransactionTest, DirectoryReadErrorNeverInvokesBytePreparation) {
  const auto source = directory / "directory_source";
  std::filesystem::create_directory(source);
  const auto marker = source / "untouched.txt";
  std::ofstream {marker} << "original";
  bool called = false;
  EXPECT_FALSE(http::update_state_file_bytes(source.string(), [&](const auto &) -> std::optional<std::string> {
    called = true;
    return std::nullopt;
  }));
  EXPECT_FALSE(called);
  EXPECT_TRUE(std::filesystem::is_directory(source));
  EXPECT_EQ(std::filesystem::file_size(marker), 8);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 2);
}
#endif

TEST_F(StateFileTransactionTest, ObservedExternalEditIsNotOverwrittenAfterPreparation) {
#ifdef _WIN32
  const auto external = directory / "external-edit.bytes";
  {
    std::ofstream output {external, std::ios::binary};
    output << R"({"fixture":"external"})";
    ASSERT_TRUE(output.good());
  }
#endif
  http::test_support::set_state_update_hook([&]() {
#ifdef _WIN32
    ASSERT_TRUE(replace_external_fixture(external));
#else
    std::ofstream {file} << R"({"fixture":"external"})";
#endif
  });
  EXPECT_FALSE(http::update_state_file_bytes(file.string(), [](const auto &) -> std::optional<std::string> {
    return R"({"fixture":"prepared"})";
  }));
  EXPECT_EQ(state_bytes(), R"({"fixture":"external"})");
}

#ifdef _WIN32
TEST_F(StateFileTransactionTest, ByteNoOpAndReadOnlyRefusalAvoidBackupTemporaryAndGuardHandoff) {
  using stage_t = http::test_support::state_file_stage_e;
  const auto original_attributes = GetFileAttributesW(file.c_str());
  ASSERT_NE(original_attributes, INVALID_FILE_ATTRIBUTES);
  ASSERT_FALSE(original_attributes & FILE_ATTRIBUTE_READONLY);
  auto cleanup = util::fail_guard([&]() {
    http::test_support::set_state_file_hook({});
    http::test_support::set_state_security_hook({});
    EXPECT_TRUE(SetFileAttributesW(file.c_str(), original_attributes));
  });
  for (const bool read_only : {false, true}) {
    SCOPED_TRACE(read_only);
    ASSERT_TRUE(SetFileAttributesW(file.c_str(), original_attributes | (read_only ? FILE_ATTRIBUTE_READONLY : 0)));
    const auto original = fixture_snapshot(file);
    bool reached_effect = false, prepared = false;
    http::test_support::set_state_security_hook([&](auto, const auto &) {
      prepared = true;
    });
    http::test_support::set_state_file_hook([&](auto stage, const auto &, const auto &, const auto &) -> std::optional<std::uint32_t> {
      if (stage == stage_t::backup || stage == stage_t::guard_open || stage == stage_t::guard_close || stage == stage_t::write || stage == stage_t::flush || stage == stage_t::close || stage == stage_t::rename || stage == stage_t::rename_open || stage == stage_t::rename_api || stage == stage_t::rename_close || stage == stage_t::verify || stage == stage_t::restore || stage == stage_t::cleanup) {
        reached_effect = true;
        return ERROR_INVALID_FUNCTION;
      }
      return std::nullopt;
    });
    unsigned calls = 0;
    EXPECT_TRUE(http::update_state_file_bytes(file.string(), [&](const auto &bytes) -> std::optional<std::string> {
      ++calls;
      EXPECT_TRUE(bytes == original.bytes);
      EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
      return std::nullopt;
    }));
    EXPECT_EQ(calls, 1U);
    EXPECT_FALSE(reached_effect || prepared);
    expect_fixture_snapshot(file, original);
    if (read_only) {
      for (const auto &replacement : {std::string {}, std::string {"unpublished readonly replacement"}}) {
        calls = 0;
        EXPECT_FALSE(http::update_state_file_bytes(file.string(), [&](const auto &bytes) -> std::optional<std::string> {
          ++calls;
          EXPECT_TRUE(bytes == original.bytes);
          return replacement;
        }));
        EXPECT_EQ(calls, 1U);
        EXPECT_FALSE(reached_effect || prepared);
        expect_fixture_snapshot(file, original);
      }
    }
    http::test_support::set_state_file_hook({});
    http::test_support::set_state_security_hook({});
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
  }
}

TEST_F(StateFileTransactionTest, ByteNoOpRejectsActualAuthorityDriftWithoutGuardHandoffOrRepair) {
  using stage_t = http::test_support::state_file_stage_e;
  auto baseline = security_from_sddl(L"D:P(A;;FA;;;OW)(A;;FR;;;BU)");
  auto changed = security_from_sddl(L"D:P(A;;FA;;;OW)(A;;FW;;;BU)");
  ASSERT_FALSE(baseline.empty());
  ASSERT_FALSE(changed.empty());
  ASSERT_TRUE(SetFileSecurityW(file.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, baseline.data()));
  const auto original = fixture_snapshot(file);
  std::vector<std::uint8_t> changed_policy;
  bool reached_effect = false, prepared = false, mutated = false, observed_drift = false;
  http::test_support::set_state_security_hook([&](auto, const auto &) {
    prepared = true;
  });
  http::test_support::set_state_file_hook([&](auto stage, const auto &, const auto &, const auto &) -> std::optional<std::uint32_t> {
    if (stage == stage_t::backup || stage == stage_t::guard_open || stage == stage_t::guard_close || stage == stage_t::write || stage == stage_t::flush || stage == stage_t::close || stage == stage_t::rename || stage == stage_t::rename_api || stage == stage_t::verify) {
      reached_effect = true;
      return ERROR_INVALID_FUNCTION;
    }
    return std::nullopt;
  });
  unsigned calls = 0;
  const bool accepted = http::update_state_file_bytes(file.string(), [&](const auto &bytes) -> std::optional<std::string> {
    ++calls;
    EXPECT_TRUE(bytes == original.bytes);
    mutated = SetFileSecurityW(file.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, changed.data());
    EXPECT_TRUE(mutated);
    changed_policy = security_snapshot(file);
    const bool valid = same_security(changed_policy, changed_policy);
    const bool unchanged = same_security(original.security, changed_policy);
    EXPECT_TRUE(valid);
    EXPECT_FALSE(unchanged);
    observed_drift = valid && !unchanged;
    return std::nullopt;
  });
  http::test_support::set_state_file_hook({});
  http::test_support::set_state_security_hook({});
  ASSERT_TRUE(mutated);
  ASSERT_TRUE(observed_drift);
  EXPECT_EQ(calls, 1U);
  EXPECT_FALSE(accepted);
  EXPECT_FALSE(reached_effect || prepared);
  auto externally_changed = original;
  externally_changed.security = changed_policy;
  expect_fixture_snapshot(file, externally_changed);
  calls = 0;
  EXPECT_FALSE(http::update_state_file_bytes(file.string(), [&](const auto &) -> std::optional<std::string> {
    ++calls;
    return std::string {};
  }));
  EXPECT_EQ(calls, 0U);
  expect_fixture_snapshot(file, externally_changed);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
}

TEST_F(StateFileTransactionTest, ByteEngagedEmptyReplacementHandsOffBeforePreparingAndPublishesZeroBytes) {
  using stage_t = http::test_support::state_file_stage_e;
  const auto original = fixture_snapshot(file);
  const auto alias = directory / "empty-replacement-original.json";
  ASSERT_TRUE(CreateHardLinkW(alias.c_str(), file.c_str(), nullptr));
  for (const auto rejected : {stage_t::guard_open, stage_t::guard_close}) {
    SCOPED_TRACE(static_cast<int>(rejected));
    bool reached = false, prepared = false, published = false;
    unsigned calls = 0;
    http::test_support::set_state_security_hook([&](auto, const auto &) {
      prepared = true;
    });
    http::test_support::set_state_file_hook([&](auto stage, const auto &destination, const auto &temporary, const auto &backup) -> std::optional<std::uint32_t> {
      if (stage == stage_t::flush || stage == stage_t::close || stage == stage_t::rename || stage == stage_t::rename_api || stage == stage_t::verify) {
        published = true;
      }
      if (stage == rejected) {
        reached = true;
        EXPECT_EQ(calls, 1U);
        EXPECT_FALSE(std::filesystem::exists(temporary));
        for (const auto &path : {destination, backup, alias}) {
          expect_fixture_writer_denied(path);
          expect_fixture_snapshot(path, original);
        }
        return ERROR_INVALID_HANDLE;
      }
      return std::nullopt;
    });
    EXPECT_FALSE(http::update_state_file_bytes(file.string(), [&](const auto &bytes) -> std::optional<std::string> {
      ++calls;
      EXPECT_TRUE(bytes == original.bytes);
      return std::string {};
    }));
    http::test_support::set_state_file_hook({});
    http::test_support::set_state_security_hook({});
    EXPECT_EQ(calls, 1U);
    EXPECT_TRUE(reached);
    EXPECT_FALSE(prepared || published);
    expect_fixture_snapshot(file, original);
    expect_fixture_snapshot(alias, original);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 2);
  }
  unsigned calls = 0, backup_stages = 0, open_stages = 0, close_stages = 0, write_stages = 0, flush_stages = 0, temporary_close_stages = 0, rename_stages = 0, verify_stages = 0;
  bool overlap = false, prepared = false;
  std::filesystem::path handoff_backup;
  http::test_support::set_state_security_hook([&](auto, const auto &) {
    prepared = true;
    EXPECT_EQ(calls, 1U);
    EXPECT_EQ(open_stages, 1U);
    EXPECT_TRUE(overlap);
    EXPECT_EQ(close_stages, 1U);
  });
  http::test_support::set_state_file_hook([&](auto stage, const auto &destination, const auto &temporary, const auto &backup) -> std::optional<std::uint32_t> {
    if (stage == stage_t::backup) {
      ++backup_stages;
      EXPECT_EQ(calls, 1U);
    }
    if (stage == stage_t::guard_open) {
      ++open_stages;
      handoff_backup = backup;
    }
    if (stage == stage_t::metadata && !handoff_backup.empty() && destination == handoff_backup && !overlap) {
      overlap = true;
      for (const auto &path : {file, handoff_backup, alias}) {
        expect_fixture_writer_denied(path);
      }
    }
    if (stage == stage_t::guard_close) {
      ++close_stages;
    }
    if (stage == stage_t::guard_open || stage == stage_t::guard_close) {
      EXPECT_EQ(calls, 1U);
      EXPECT_FALSE(std::filesystem::exists(temporary));
      for (const auto &path : {destination, backup, alias}) {
        expect_fixture_writer_denied(path);
        expect_fixture_snapshot(path, original);
      }
    }
    write_stages += stage == stage_t::write;
    flush_stages += stage == stage_t::flush;
    temporary_close_stages += stage == stage_t::close;
    rename_stages += stage == stage_t::rename;
    verify_stages += stage == stage_t::verify;
    return std::nullopt;
  });
  EXPECT_TRUE(http::update_state_file_bytes(file.string(), [&](const auto &bytes) -> std::optional<std::string> {
    ++calls;
    EXPECT_TRUE(bytes == original.bytes);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 2);
    return std::string {};
  }));
  http::test_support::set_state_file_hook({});
  http::test_support::set_state_security_hook({});
  EXPECT_EQ(calls, 1U);
  EXPECT_EQ(backup_stages, 1U);
  EXPECT_EQ(open_stages, 1U);
  EXPECT_EQ(close_stages, 1U);
  EXPECT_TRUE(overlap && prepared);
  EXPECT_EQ(write_stages, 0U);
  EXPECT_EQ(flush_stages, 1U);
  EXPECT_EQ(temporary_close_stages, 1U);
  EXPECT_EQ(rename_stages, 1U);
  EXPECT_EQ(verify_stages, 1U);
  const auto committed = fixture_snapshot(file);
  EXPECT_TRUE(committed.bytes.empty());
  EXPECT_FALSE(committed.identity.dwVolumeSerialNumber == original.identity.dwVolumeSerialNumber && committed.identity.nFileIndexHigh == original.identity.nFileIndexHigh && committed.identity.nFileIndexLow == original.identity.nFileIndexLow);
  EXPECT_EQ(CompareFileTime(&committed.identity.ftCreationTime, &original.identity.ftCreationTime), 0);
  EXPECT_EQ(committed.identity.dwFileAttributes, original.identity.dwFileAttributes);
  EXPECT_TRUE(same_security(original.security, committed.security));
  for (std::size_t index = 0; index < original.selected.size(); ++index) {
    EXPECT_TRUE(http::test_support::state_selected_security_matches(original.selected[index], committed.selected[index], index == 2));
  }
  expect_fixture_snapshot(alias, original);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 2);
}

TEST_F(StateFileTransactionTest, ByteReadOnlyAttributeAllowsNoOpButRefusesReplacementBeforeTransactionFiles) {
  const auto original = state_bytes();
  const auto original_security = security_snapshot(file);
  const auto original_attributes = GetFileAttributesW(file.c_str());
  ASSERT_NE(original_attributes, INVALID_FILE_ATTRIBUTES);
  ASSERT_FALSE(original_attributes & FILE_ATTRIBUTE_READONLY);
  auto restore_attributes = util::fail_guard([&]() {
    EXPECT_TRUE(SetFileAttributesW(file.c_str(), original_attributes));
  });
  ASSERT_TRUE(SetFileAttributesW(file.c_str(), original_attributes | FILE_ATTRIBUTE_READONLY));
  const auto read_only_attributes = GetFileAttributesW(file.c_str());
  ASSERT_NE(read_only_attributes, INVALID_FILE_ATTRIBUTES);
  ASSERT_TRUE(read_only_attributes & FILE_ATTRIBUTE_READONLY);
  bool reached_temporary = false;
  http::test_support::set_state_update_hook([&]() {
    reached_temporary = true;
  });
  unsigned calls = 0;
  EXPECT_TRUE(http::update_state_file_bytes(file.string(), [&](const auto &bytes) -> std::optional<std::string> {
    ++calls;
    EXPECT_TRUE(bytes == original);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 1);
    return std::nullopt;
  }));
  EXPECT_EQ(calls, 1U);
  calls = 0;
  EXPECT_FALSE(http::update_state_file_bytes(file.string(), [&](const auto &bytes) -> std::optional<std::string> {
    ++calls;
    EXPECT_TRUE(bytes == original);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 1);
    return R"({"fixture":"must not replace readonly state"})";
  }));
  EXPECT_EQ(calls, 1U);
  calls = 0;
  EXPECT_FALSE(http::update_state_file_bytes(file.string(), [&](const auto &bytes) -> std::optional<std::string> {
    ++calls;
    EXPECT_TRUE(bytes == original);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 1);
    throw std::runtime_error {"readonly byte preparation failure"};
  }));
  EXPECT_EQ(calls, 1U);
  bool legacy_called = false;
  EXPECT_FALSE(http::update_state_file(file.string(), [&](auto &tree) {
    legacy_called = true;
    tree.put("fixture", "must not invoke legacy readonly update");
  }));
  EXPECT_FALSE(legacy_called);
  calls = 0;
  EXPECT_TRUE(http::update_state_file_bytes(file.string(), [&](const auto &bytes) -> std::optional<std::string> {
    ++calls;
    EXPECT_TRUE(bytes == original);
    return std::nullopt;
  }));
  EXPECT_EQ(calls, 1U);
  EXPECT_FALSE(reached_temporary);
  EXPECT_TRUE(state_bytes() == original);
  EXPECT_EQ(GetFileAttributesW(file.c_str()), read_only_attributes);
  EXPECT_TRUE(same_security(original_security, security_snapshot(file)));
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 1);
}

TEST_F(StateFileTransactionTest, ByteReadOnlyPreparationRejectsDeleteSharingReplacementWithoutCreatingTransactionFiles) {
  const auto original = state_bytes();
  const auto external = directory / "same-byte-external.json";
  ASSERT_TRUE(http::update_state_file_bytes(external.string(), [&](const auto &bytes) -> std::optional<std::string> {
    EXPECT_TRUE(bytes.empty());
    return original;
  }));
  bool reached_temporary = false, replaced = false;
  unsigned calls = 0;
  http::test_support::set_state_update_hook([&]() {
    reached_temporary = true;
  });
  const bool accepted = http::update_state_file_bytes(file.string(), [&](const auto &bytes) -> std::optional<std::string> {
    ++calls;
    EXPECT_TRUE(bytes == original);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 2);
    replaced = replace_external_fixture(external);
    EXPECT_TRUE(replaced);
    return std::nullopt;
  });
  EXPECT_EQ(calls, 1U);
  EXPECT_TRUE(replaced);
  EXPECT_FALSE(accepted);
  EXPECT_FALSE(reached_temporary);
  EXPECT_TRUE(state_bytes() == original);
  EXPECT_FALSE(std::filesystem::exists(external));
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 1);
}

TEST_F(StateFileTransactionTest, ByteReadOnlyInitialAbsenceRejectsAnAppearingNameWithoutCreatingTransactionFiles) {
  const auto original = state_bytes();
  const auto appeared = directory / "appeared-during-read.json";
  bool reached_temporary = false;
  unsigned calls = 0;
  http::test_support::set_state_update_hook([&]() {
    reached_temporary = true;
  });
  const bool accepted = http::update_state_file_bytes(appeared.string(), [&](const auto &bytes) -> std::optional<std::string> {
    ++calls;
    EXPECT_TRUE(bytes.empty());
    EXPECT_FALSE(std::filesystem::exists(appeared));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 1);
    std::ofstream output {appeared, std::ios::binary};
    output << R"({"fixture":"appeared during read-only preparation"})";
    EXPECT_TRUE(output.good());
    return std::nullopt;
  });
  EXPECT_EQ(calls, 1U);
  EXPECT_FALSE(accepted);
  EXPECT_FALSE(reached_temporary);
  EXPECT_TRUE(read_fixture_bytes(appeared) == R"({"fixture":"appeared during read-only preparation"})");
  EXPECT_TRUE(state_bytes() == original);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator {directory}, std::filesystem::directory_iterator {}), 2);
}
#endif

TEST_F(StateFileTransactionTest, ByteAndPropertyTreeAdaptersShareTheWholeTransactionLock) {
  std::promise<void> first_preparing;
  std::promise<void> release_first;
  auto released = release_first.get_future().share();
  std::promise<void> second_attempting;
  std::promise<void> second_preparing;
  auto second_entered = second_preparing.get_future();
  bool first_success = false, second_success = false;
  std::jthread bytes_worker {[&]() {
    first_success = http::update_state_file_bytes(file.string(), [&](const std::string &bytes) -> std::optional<std::string> {
      first_preparing.set_value();
      released.wait();
      boost::property_tree::ptree tree;
      std::istringstream input {bytes};
      boost::property_tree::read_json(input, tree);
      tree.put("byte-update", "retained");
      std::ostringstream output;
      boost::property_tree::write_json(output, tree);
      return output.str();
    });
  }};
  std::jthread tree_worker;
  // This guard releases the paused callback before either worker's destructor can join.
  bool signalled = false;
  auto cleanup = util::fail_guard([&]() {
    if (!signalled) {
      release_first.set_value();
    }
  });
  const auto first_ready = first_preparing.get_future().wait_for(std::chrono::seconds {2});
  EXPECT_EQ(first_ready, std::future_status::ready);
  tree_worker = std::jthread {[&]() {
    second_attempting.set_value();
    second_success = http::update_state_file(file.string(), [&](auto &tree) {
      second_preparing.set_value();
      tree.put("tree-update", "retained");
    });
  }};
  const auto attempted = second_attempting.get_future().wait_for(std::chrono::seconds {2});
  EXPECT_EQ(attempted, std::future_status::ready);
  EXPECT_EQ(second_entered.wait_for(std::chrono::milliseconds {100}), std::future_status::timeout);
  release_first.set_value();
  signalled = true;
  bytes_worker.join();
  tree_worker.join();
  EXPECT_TRUE(first_success);
  EXPECT_TRUE(second_success);
  EXPECT_EQ(second_entered.wait_for(std::chrono::seconds {0}), std::future_status::ready);
  boost::property_tree::ptree result;
  std::istringstream input {state_bytes()};
  boost::property_tree::read_json(input, result);
  EXPECT_EQ(result.get<std::string>("byte-update"), "retained");
  EXPECT_EQ(result.get<std::string>("tree-update"), "retained");
  EXPECT_EQ(result.get<std::string>("fixture"), "original");
}
