/**
 * @file tests/unit/test_nvhttp_client_auth.cpp
 * @brief Test exact paired-client certificate authorization and persistence.
 */

// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// lib includes
#include <boost/property_tree/json_parser.hpp>

// platform includes
#ifdef _WIN32
  // Winsock must precede Windows.h.
  // clang-format off
  #include <WinSock2.h>
  #include <Windows.h>
// clang-format on
#else
  #include <fcntl.h>
  #include <sys/file.h>
  #include <unistd.h>
#endif

// local includes
#include <src/app_catalog.h>
#include <src/config.h>
#include <src/httpcommon.h>
#include <src/nvhttp.h>
#include <src/rtsp.h>
#include <src/utility.h>
#include <src/uuid.h>

namespace fs = std::filesystem;

static_assert(std::is_same_v<decltype(&nvhttp::pin), bool (*)(std::string_view, const std::string &, std::string)>);

/**
 * @brief Isolate paired-client authorization tests from the user's Sunshine state.
 */
class ClientAuthorizationTest: public BaseTest {
protected:
  /**
   * @brief Redirect persisted client state to the test build directory.
   */
  void SetUp() override {
    BaseTest::SetUp();
    drain_owner(rtsp_stream::test_support::start_owner());
    original_state_file = config::nvhttp.file_state;
    original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    original_host_id = http::unique_id;
    state_file = fs::path {SUNSHINE_TEST_BIN_DIR} / "client_authorization_state.json";

    config::nvhttp.file_state = state_file.string();
    config::sunshine.flags[config::flag::FRESH_STATE] = false;
    nvhttp::test_support::reset_client_state();

    std::error_code remove_error;
    fs::remove(state_file, remove_error);
  }

  /**
   * @brief Remove test state and restore the caller's configuration.
   */
  void TearDown() override {
    http::test_support::set_state_update_hook({});
    http::test_support::set_state_replace_hook({});
#ifdef _WIN32
    http::test_support::set_state_file_hook({});
#endif
    rtsp_stream::stop_application({});
    nvhttp::test_support::reset_client_state();
    std::error_code remove_error;
    fs::remove(state_file, remove_error);

    config::nvhttp.file_state = original_state_file;
    config::sunshine.flags[config::flag::FRESH_STATE] = true;
    nvhttp::initialize_state();
    http::unique_id = original_host_id;
    config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
    BaseTest::TearDown();
  }

  /**
   * @brief Drain prior process observations before replacing fixture state or starting a fresh owner.
   * @param owner Isolated owner whose shutdown consumes all accepted process completions.
   */
  static void drain_owner(std::unique_ptr<platf::deinit_t> owner) {
    rtsp_stream::test_support::owner_operation([]() {
    });
    owner.reset();
  }

  static std::string read_state() {
    std::ifstream file {config::nvhttp.file_state, std::ios::binary};
    return {std::istreambuf_iterator<char> {file}, std::istreambuf_iterator<char> {}};
  }

  static void write_state(std::string_view bytes) {
    std::ofstream {config::nvhttp.file_state, std::ios::binary} << bytes;
  }

private:
  fs::path state_file;  ///< Task-specific persisted state fixture.
  std::string original_state_file;  ///< State-file setting restored after each test.
  std::string original_host_id;  ///< Previous GameStream identifier restored after each test.
  bool original_fresh_state;  ///< Fresh-state flag restored after each test.
};

TEST_F(ClientAuthorizationTest, MissingHostUuidPersistsBeforePairingAndSurvivesRestart) {
  ASSERT_FALSE(fs::exists(config::nvhttp.file_state));

  nvhttp::initialize_state();
  ASSERT_TRUE(nvhttp::shortcut_host_uuid());
  const auto first_id = http::unique_id;
  EXPECT_EQ(*nvhttp::shortcut_host_uuid(), first_id);
  EXPECT_TRUE(nvhttp::get_all_clients().empty());
  const auto bytes = read_state();
  const nlohmann::json expected {{"root", {{"uniqueid", first_id}}}};
  EXPECT_EQ(nlohmann::json::parse(bytes), expected);

  nvhttp::initialize_state();
  ASSERT_TRUE(nvhttp::shortcut_host_uuid());
  EXPECT_EQ(*nvhttp::shortcut_host_uuid(), first_id);
  EXPECT_EQ(http::unique_id, first_id);
  EXPECT_EQ(read_state(), bytes);
}

TEST_F(ClientAuthorizationTest, ReadOnlyHostIdentityPreservesExactBytesAndCase) {
  constexpr std::string_view id = "aB123456-ABcd-4111-8222-00EF12345678";
  const auto bytes = std::string {"  { \"root\": { \"uniqueid\": \""} + std::string {id} + "\" } }  \n";
  write_state(bytes);
  const fs::path path {config::nvhttp.file_state};
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  ASSERT_NE(attributes, INVALID_FILE_ATTRIBUTES);
  ASSERT_NE(SetFileAttributesW(path.c_str(), attributes | FILE_ATTRIBUTE_READONLY), FALSE);
  const auto restore = util::fail_guard([&]() {
    EXPECT_NE(SetFileAttributesW(path.c_str(), attributes), FALSE);
  });
#else
  const auto permissions = fs::status(path).permissions();
  fs::permissions(path, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write, fs::perm_options::remove);
  const auto restore = util::fail_guard([&]() {
    std::error_code error;
    fs::permissions(path, permissions, error);
    EXPECT_FALSE(error);
  });
#endif
  int writes = 0;
  http::test_support::set_state_update_hook([&]() {
    ++writes;
  });

  nvhttp::initialize_state();
  http::test_support::set_state_update_hook({});
  ASSERT_TRUE(nvhttp::shortcut_host_uuid());
  EXPECT_EQ(*nvhttp::shortcut_host_uuid(), id);
  EXPECT_EQ(http::unique_id, id);
  EXPECT_EQ(read_state(), bytes);
  EXPECT_EQ(writes, 0);
}

TEST_F(ClientAuthorizationTest, MissingHostUuidPreservesCredentialsAndExistingPairings) {
  const auto paired = test_utils::certificates::generate_ca_credentials("Named fixture client");
  const auto legacy = test_utils::certificates::generate_ca_credentials("Legacy fixture client");
  const auto uuid = nvhttp::test_support::add_client("named", paired.x509, true);
  ASSERT_FALSE(uuid.empty());
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view));
  ASSERT_EQ(http::save_user_creds(config::nvhttp.file_state, "fixture administrator", "fixture password"), 0);
  auto original = nlohmann::ordered_json::parse(read_state());
  original["foreign"] = nlohmann::ordered_json::parse(R"([null,false,[],{"nested":9007199254740993}])");
  original["root"]["foreign"] = "root value";
  original["root"]["named_devices"][0]["foreign"] = nlohmann::ordered_json::array({true, 2.5});
  original["root"]["devices"] = nlohmann::ordered_json::array({{{"uniqueid", "legacy"}, {"certs", nlohmann::ordered_json::array({legacy.x509})}}});
  original["root"].erase("uniqueid");
  write_state(original.dump());
  nvhttp::test_support::reset_client_state();

  nvhttp::initialize_state();
  ASSERT_TRUE(nvhttp::shortcut_host_uuid());
  auto saved = nlohmann::ordered_json::parse(read_state());
  EXPECT_EQ(saved["root"]["uniqueid"], http::unique_id);
  saved["root"].erase("uniqueid");
  EXPECT_TRUE(saved == original);
  const auto client = nvhttp::current_client(uuid, paired.x509);
  ASSERT_TRUE(client);
  EXPECT_EQ(client->permissions, permissions::view);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(legacy.x509));
}

TEST_F(ClientAuthorizationTest, MissingHostUuidPreservesCredentialOnlyState) {
  ASSERT_EQ(http::save_user_creds(config::nvhttp.file_state, "fixture administrator", "fixture password"), 0);
  const auto original = nlohmann::ordered_json::parse(read_state());
  ASSERT_FALSE(original.contains("root"));

  nvhttp::initialize_state();
  ASSERT_TRUE(nvhttp::shortcut_host_uuid());
  auto saved = nlohmann::ordered_json::parse(read_state());
  EXPECT_EQ(saved["root"], (nlohmann::ordered_json {{"uniqueid", http::unique_id}}));
  saved.erase("root");
  EXPECT_TRUE(saved == original);
}

TEST_F(ClientAuthorizationTest, HostIdentityBootstrapAcceptsLegacyEmptyDeviceCollection) {
  write_state(R"({"root":{"named_devices":"","devices":"","foreign":true}})");

  nvhttp::initialize_state();
  ASSERT_TRUE(nvhttp::shortcut_host_uuid());
  const auto saved = nlohmann::ordered_json::parse(read_state());
  EXPECT_EQ(saved["root"]["uniqueid"], http::unique_id);
  EXPECT_EQ(saved["root"]["named_devices"], "");
  EXPECT_EQ(saved["root"]["devices"], "");
  EXPECT_TRUE(saved["root"]["foreign"].get<bool>());
  EXPECT_TRUE(nvhttp::get_all_clients().empty());
}

#ifndef _WIN32
TEST_F(ClientAuthorizationTest, ReadOnlyCredentialStateKeepsItsBytesAndAnEphemeralHostIdentity) {
  constexpr std::string_view original = R"({"username":"fixture","salt":"fixture","password":"fixture","foreign":true})";
  write_state(original);
  const fs::path path {config::nvhttp.file_state};
  fs::permissions(path, fs::perms::owner_read);
  bool prepared_temporary = false;
  http::test_support::set_state_update_hook([&]() {
    prepared_temporary = true;
  });

  nvhttp::initialize_state();
  http::test_support::set_state_update_hook({});
  EXPECT_TRUE(proc::catalog::canonical_uuid(http::unique_id));
  EXPECT_FALSE(nvhttp::shortcut_host_uuid());
  EXPECT_FALSE(prepared_temporary);
  EXPECT_TRUE(read_state() == original);
  EXPECT_EQ(fs::status(path).permissions(), fs::perms::owner_read);
}
#endif

TEST_F(ClientAuthorizationTest, InvalidHostStateAndFreshStateLeaveShortcutsUnavailable) {
  for (const auto bytes : {
         "",
         "{",
         R"({"root":[]})",
         R"({"root":{"uniqueid":null}})",
         R"({"root":{"uniqueid":"aB123456-ABcd-4111-8222-00EF1234567G"}})"
       }) {
    SCOPED_TRACE(bytes);
    write_state(bytes);
    nvhttp::initialize_state();
    EXPECT_FALSE(nvhttp::shortcut_host_uuid());
    EXPECT_EQ(read_state(), bytes);
  }
  EXPECT_EQ(http::unique_id, "aB123456-ABcd-4111-8222-00EF1234567G");

  constexpr std::string_view ordinary = R"({"root":{"uniqueid":"aB123456-ABcd-4111-8222-00EF12345678"}})";
  write_state(ordinary);
  nvhttp::initialize_state();
  ASSERT_TRUE(nvhttp::shortcut_host_uuid());
  config::sunshine.flags[config::flag::FRESH_STATE] = true;
  http::unique_id = "14a88f00-5a44-4e23-a562-9c50e16fec9f";
  nvhttp::initialize_state();
  EXPECT_FALSE(nvhttp::shortcut_host_uuid());
  EXPECT_EQ(http::unique_id, "14a88f00-5a44-4e23-a562-9c50e16fec9f");
  EXPECT_EQ(read_state(), ordinary);

  config::sunshine.flags[config::flag::FRESH_STATE] = false;
  ASSERT_TRUE(fs::remove(config::nvhttp.file_state));
  ASSERT_TRUE(fs::create_directory(config::nvhttp.file_state));
  nvhttp::initialize_state();
  EXPECT_FALSE(nvhttp::shortcut_host_uuid());
  EXPECT_TRUE(fs::is_directory(config::nvhttp.file_state));
}

TEST_F(ClientAuthorizationTest, InvalidPairingStateKeepsOriginalBytesAndUsesAnEphemeralIdentity) {
  for (const auto bytes : {
         R"({"root":{"named_devices":false}})",
         R"({"root":{"named_devices":{}}})",
         R"({"root":{"named_devices":[{"name":"incomplete"}]}})"
       }) {
    SCOPED_TRACE(bytes);
    write_state(bytes);
    nvhttp::initialize_state();
    EXPECT_TRUE(proc::catalog::canonical_uuid(http::unique_id));
    EXPECT_FALSE(nvhttp::shortcut_host_uuid());
    EXPECT_EQ(read_state(), bytes);
    EXPECT_TRUE(nvhttp::get_all_clients().empty());
  }
}

TEST_F(ClientAuthorizationTest, MiskeyedPairingCollectionCannotRestoreAuthority) {
  const auto credentials = test_utils::certificates::generate_ca_credentials("Malformed collection fixture");
  constexpr auto uuid = "42bded4a-4c4e-45b2-9e07-58c2f20e7cee";
  const nlohmann::ordered_json device {
    {"name", "Synthetic client"},
    {"cert", credentials.x509},
    {"uuid", uuid},
    {"enabled", "true"},
    {"permissions", std::to_string(permissions::launch)},
  };
  const auto bytes = nlohmann::ordered_json {{"root", {{"named_devices", {{"miskeyed", device}}}}}}.dump();
  write_state(bytes);

  nvhttp::initialize_state();
  EXPECT_TRUE(proc::catalog::canonical_uuid(http::unique_id));
  EXPECT_FALSE(nvhttp::shortcut_host_uuid());
  EXPECT_TRUE(nvhttp::get_all_clients().empty());
  EXPECT_FALSE(nvhttp::current_client(uuid, credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
  EXPECT_TRUE(read_state() == bytes);
}

TEST_F(ClientAuthorizationTest, IdlessPairingMigrationValidatesEnableFlagsAndGrantMasks) {
  const auto credentials = test_utils::certificates::generate_ca_credentials("Enable flag fixture");
  constexpr auto uuid = "4d5a49eb-1ee7-4611-8400-382a16681826";
  nlohmann::ordered_json device {
    {"name", "Synthetic client"},
    {"cert", credentials.x509},
    {"uuid", uuid},
    {"permissions", std::to_string(permissions::launch)},
  };
  const auto state = [&]() {
    return nlohmann::ordered_json {{"root", {{"named_devices", nlohmann::ordered_json::array({device})}}}};
  };
  const auto expect_refused = [&]() {
    const auto bytes = state().dump();
    write_state(bytes);

    nvhttp::initialize_state();
    EXPECT_TRUE(proc::catalog::canonical_uuid(http::unique_id));
    EXPECT_FALSE(nvhttp::shortcut_host_uuid());
    EXPECT_TRUE(nvhttp::get_all_clients().empty());
    EXPECT_FALSE(nvhttp::current_client(uuid, credentials.x509));
    EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
    EXPECT_TRUE(read_state() == bytes);
  };
  for (const auto enabled : {R"("invalid")", "null", "{}", "2", R"("2true")", R"("2 true")", R"(["true"])"}) {
    SCOPED_TRACE(enabled);
    device["enabled"] = nlohmann::ordered_json::parse(enabled);
    expect_refused();
  }

  device.erase("enabled");
  for (const auto grant : {"{}", "65536"}) {
    SCOPED_TRACE(grant);
    device["permissions"] = nlohmann::ordered_json::parse(grant);
    expect_refused();
  }
  device["permissions"] = std::to_string(permissions::launch);
  for (const auto disabled : {"false", "0", R"("false")", R"("0")"}) {
    SCOPED_TRACE(disabled);
    device["enabled"] = nlohmann::ordered_json::parse(disabled);
    const auto original = state();
    write_state(original.dump());
    nvhttp::initialize_state();
    ASSERT_TRUE(nvhttp::shortcut_host_uuid());
    EXPECT_FALSE(nvhttp::current_client(uuid, credentials.x509));
    EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
    const auto saved = nvhttp::get_all_clients();
    ASSERT_EQ(saved.size(), 1U);
    EXPECT_EQ(saved[0]["enabled"], false);
    EXPECT_EQ(saved[0]["permissions"], permissions::launch);
    auto committed = nlohmann::ordered_json::parse(read_state());
    committed["root"].erase("uniqueid");
    EXPECT_TRUE(committed == original);
  }

  device.erase("enabled");
  const auto legacy = state();
  write_state(legacy.dump());
  nvhttp::initialize_state();
  ASSERT_TRUE(nvhttp::shortcut_host_uuid());
  const auto client = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(client);
  EXPECT_EQ(client->permissions, permissions::launch);
  auto committed = nlohmann::ordered_json::parse(read_state());
  committed["root"].erase("uniqueid");
  EXPECT_TRUE(committed == legacy);
}

TEST_F(ClientAuthorizationTest, IdentifiedHostKeepsMalformedEnableFlagVisibleButDeniesClientAuthority) {
  const auto credentials = test_utils::certificates::generate_ca_credentials("Identified host enable flag fixture");
  constexpr auto host_uuid = "aB123456-ABcd-4111-8222-00EF12345678";
  constexpr auto client_uuid = "4d5a49eb-1ee7-4611-8400-382a16681826";
  const nlohmann::ordered_json device {
    {"name", "Synthetic client"},
    {"cert", credentials.x509},
    {"uuid", client_uuid},
    {"enabled", "2true"},
    {"permissions", std::to_string(permissions::launch)},
  };
  const auto bytes = nlohmann::ordered_json {{"root", {{"uniqueid", host_uuid}, {"named_devices", nlohmann::ordered_json::array({device})}}}}.dump();
  write_state(bytes);

  nvhttp::initialize_state();
  ASSERT_TRUE(nvhttp::shortcut_host_uuid());
  EXPECT_EQ(*nvhttp::shortcut_host_uuid(), host_uuid);
  EXPECT_EQ(http::unique_id, host_uuid);
  EXPECT_TRUE(read_state() == bytes);
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
  EXPECT_FALSE(nvhttp::current_client(client_uuid, credentials.x509));
  EXPECT_FALSE(rtsp_stream::authorize_operation(client_uuid, credentials.x509, permissions::launch, []() {
    ADD_FAILURE() << "Disabled client ran an operation";
  }));
  const auto clients = nvhttp::get_all_clients();
  ASSERT_EQ(clients.size(), 1U);
  EXPECT_EQ(clients[0]["uuid"], client_uuid);
  EXPECT_EQ(clients[0]["enabled"], false);
  EXPECT_EQ(clients[0]["permissions"], permissions::launch);
}

TEST_F(ClientAuthorizationTest, MiskeyedLegacyCertificateCollectionsCannotRestoreAnIdlessPairing) {
  const auto credentials = test_utils::certificates::generate_ca_credentials("Legacy collection fixture");
  const auto valid_certs = nlohmann::ordered_json::array({credentials.x509});
  const nlohmann::ordered_json keyed_certs {{"miskeyed", credentials.x509}};
  const nlohmann::ordered_json legacy_device {{"uniqueid", "legacy device"}, {"certs", valid_certs}};
  const std::array malformed {
    nlohmann::ordered_json {{"miskeyed", legacy_device}},
    nlohmann::ordered_json::array({{{"uniqueid", "legacy device"}, {"certs", keyed_certs}}}),
  };
  for (const auto &devices : malformed) {
    const auto bytes = nlohmann::ordered_json {{"root", {{"devices", devices}}}}.dump();
    write_state(bytes);

    nvhttp::initialize_state();
    EXPECT_TRUE(proc::catalog::canonical_uuid(http::unique_id));
    EXPECT_FALSE(nvhttp::shortcut_host_uuid());
    EXPECT_TRUE(nvhttp::get_all_clients().empty());
    EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
    EXPECT_TRUE(read_state() == bytes);
  }
}

TEST_F(ClientAuthorizationTest, RefusedHostIdCommitLeavesStateUnchangedAndGameStreamUsable) {
  constexpr std::string_view original = R"({"root":{"foreign":true},"other":[2,null]})";
  write_state(original);
  http::test_support::set_state_update_hook([]() {
    throw std::runtime_error {"fixture refusal before replacement"};
  });

  nvhttp::initialize_state();
  http::test_support::set_state_update_hook({});
  EXPECT_FALSE(nvhttp::shortcut_host_uuid());
  EXPECT_FALSE(http::unique_id.empty());
  EXPECT_EQ(read_state(), original);
}

TEST_F(ClientAuthorizationTest, ConcurrentlyAppearingHostIdentityIsNeverOverwritten) {
  const auto directory = fs::path {SUNSHINE_TEST_BIN_DIR} / ("host-id-race-" + uuid_util::uuid_t::generate().string());
  ASSERT_TRUE(fs::create_directory(directory));
  const auto previous_state_file = std::exchange(config::nvhttp.file_state, (directory / "state.json").string());
  const auto cleanup = util::fail_guard([&]() {
    config::nvhttp.file_state = previous_state_file;
    std::error_code error;
    fs::remove_all(directory, error);
    EXPECT_FALSE(error);
  });

  constexpr std::string_view committed_id = "aB123456-ABcd-4111-8222-00EF12345678";
  const auto concurrent = nlohmann::ordered_json {{"root", {{"uniqueid", committed_id}}}, {"foreign", true}}.dump();
  ASSERT_FALSE(fs::exists(config::nvhttp.file_state));
#ifdef _WIN32
  http::test_support::set_state_update_hook([&]() {
    write_state(concurrent);
  });
#else
  http::test_support::set_state_replace_hook([&](const auto &, const auto &, const auto &) -> std::optional<std::uint32_t> {
    write_state(concurrent);
    return std::nullopt;
  });
#endif

  nvhttp::initialize_state();
  http::test_support::set_state_update_hook({});
  http::test_support::set_state_replace_hook({});
  EXPECT_FALSE(nvhttp::shortcut_host_uuid());
  EXPECT_EQ(http::unique_id, committed_id);
  EXPECT_EQ(read_state(), concurrent);

#ifdef _WIN32
  // Windows blocks further protected operations on this pathname for the life of the process.
  nvhttp::initialize_state();
  EXPECT_FALSE(nvhttp::shortcut_host_uuid());
  EXPECT_EQ(http::unique_id, committed_id);
  EXPECT_EQ(read_state(), concurrent);
#endif
}

TEST_F(ClientAuthorizationTest, NondurableStartupCannotSaveOverAnotherHostIdentityOrItsAuthority) {
  const auto credentials = test_utils::certificates::generate_ca_credentials("Competing host identity fixture");
  constexpr auto client_uuid = "4d5a49eb-1ee7-4611-8400-382a16681826";
  const nlohmann::ordered_json device {
    {"name", "Synthetic client"},
    {"cert", credentials.x509},
    {"uuid", client_uuid},
    {"enabled", true},
    {"permissions", permissions::view},
  };
  auto document = nlohmann::ordered_json {{"root", {{"named_devices", nlohmann::ordered_json::array({device})}}}, {"foreign", true}};
  const auto idless_bytes = document.dump();
  write_state(idless_bytes);
  http::test_support::set_state_update_hook([]() {
    throw std::runtime_error {"fixture unavailable startup commit"};
  });
  nvhttp::initialize_state();
  http::test_support::set_state_update_hook({});
  ASSERT_FALSE(nvhttp::shortcut_host_uuid());
  ASSERT_TRUE(proc::catalog::canonical_uuid(http::unique_id));
  const auto before = nvhttp::current_client(client_uuid, credentials.x509);
  ASSERT_TRUE(before);
  ASSERT_EQ(before->permissions, permissions::view);

#ifndef _WIN32
  {
    const auto parent = fs::path {config::nvhttp.file_state}.parent_path();
    const auto contender = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ASSERT_GE(contender, 0);
    const auto close = util::fail_guard([&]() {
      ::close(contender);
    });
    ASSERT_EQ(::flock(contender, LOCK_EX | LOCK_NB), 0);
    EXPECT_FALSE(nvhttp::update_client(client_uuid, std::nullopt, permissions::launch));
    EXPECT_TRUE(read_state() == idless_bytes);
    const auto during = nvhttp::current_client(client_uuid, credentials.x509);
    ASSERT_TRUE(during);
    EXPECT_EQ(during->permissions, before->permissions);
    EXPECT_EQ(during->revision, before->revision);
  }
#endif

  constexpr auto winner = "aB123456-ABcd-4111-8222-00EF12345678";
  ASSERT_NE(http::unique_id, winner);
  document["root"]["uniqueid"] = winner;
  const auto committed = document.dump();
  write_state(committed);
  EXPECT_FALSE(nvhttp::update_client(client_uuid, std::nullopt, permissions::launch));
  EXPECT_FALSE(nvhttp::unpair_client(client_uuid));
  const auto after = nvhttp::current_client(client_uuid, credentials.x509);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->permissions, before->permissions);
  EXPECT_EQ(after->revision, before->revision);
  EXPECT_TRUE(read_state() == committed);
  EXPECT_FALSE(nvhttp::shortcut_host_uuid());
}

TEST_F(ClientAuthorizationTest, CanonicalIdentityFailsClosedAndTracksEnableState) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto crlf_certificate = test_utils::certificates::to_crlf_pem(paired_credentials.x509);
  const auto unknown_credentials = crypto::gen_creds("Sunshine Unknown Client", 2048);
  const auto uuid = nvhttp::test_support::add_client("paired", crlf_certificate, true);

  EXPECT_TRUE(nvhttp::test_support::add_client("invalid", "not a certificate", true).empty());
  ASSERT_FALSE(uuid.empty());
  EXPECT_EQ(nvhttp::get_cert_by_uuid(uuid), paired_credentials.x509);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(crlf_certificate));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(unknown_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate("not a certificate"));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DerivedLeafDoesNotInheritPairedAuthorization) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto derived_credentials = test_utils::certificates::generate_derived_leaf(paired_credentials);
  const auto uuid = nvhttp::test_support::add_client("paired issuer", paired_credentials.x509, true);

  ASSERT_FALSE(uuid.empty());
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));
}

TEST_F(ClientAuthorizationTest, MultipleClientsPersistAndUnpairIndependently) {
  const auto enabled_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Enabled Client");
  const auto disabled_credentials = crypto::gen_creds("Sunshine Disabled Client", 2048);
  const auto expired_credentials = test_utils::certificates::expire_credentials(
    test_utils::certificates::generate_ca_credentials("Sunshine Expired Client")
  );

  const auto enabled_uuid = nvhttp::test_support::add_client("enabled", enabled_credentials.x509, true);
  const auto disabled_uuid = nvhttp::test_support::add_client("disabled", disabled_credentials.x509, false);
  const auto expired_uuid = nvhttp::test_support::add_client("expired", expired_credentials.x509, true);
  ASSERT_FALSE(enabled_uuid.empty());
  ASSERT_FALSE(disabled_uuid.empty());
  ASSERT_FALSE(expired_uuid.empty());
  EXPECT_EQ(nvhttp::get_all_clients().size(), 3);

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  nvhttp::test_support::reset_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  nvhttp::test_support::reload_client_state();

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(disabled_uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  ASSERT_TRUE(nvhttp::unpair_client(enabled_uuid));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));

  nvhttp::erase_all_clients();
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DuplicateCertificateIdentityFailsClosed) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("first", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  EXPECT_FALSE(nvhttp::test_support::duplicate_client("missing"));
  ASSERT_TRUE(nvhttp::test_support::duplicate_client(uuid));

  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(ClientAuthorizationTest, RePairingReplacesDuplicateCertificateIdentity) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto other_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Other Client");
  const auto original_uuid = nvhttp::test_support::add_client("original", paired_credentials.x509, true);
  const auto other_uuid = nvhttp::test_support::add_client("other", other_credentials.x509, true);
  ASSERT_FALSE(original_uuid.empty());
  ASSERT_FALSE(other_uuid.empty());
  ASSERT_TRUE(nvhttp::test_support::duplicate_client(original_uuid));
  ASSERT_EQ(nvhttp::get_all_clients().size(), 3);
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));

  const auto repaired_uuid = nvhttp::test_support::add_client(
    "repaired",
    test_utils::certificates::to_crlf_pem(paired_credentials.x509),
    true
  );

  ASSERT_FALSE(repaired_uuid.empty());
  EXPECT_NE(repaired_uuid, original_uuid);
  EXPECT_EQ(nvhttp::get_all_clients().size(), 2);
  EXPECT_EQ(nvhttp::get_cert_by_uuid(repaired_uuid), paired_credentials.x509);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(other_credentials.x509));

  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_EQ(nvhttp::get_all_clients().size(), 2);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(other_credentials.x509));
}

TEST_F(ClientAuthorizationTest, ConcurrentStateChangesRemainConsistent) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("concurrent", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());

  std::atomic_bool operations_succeeded {true};
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&operations_succeeded, uuid, certificate = credentials.x509, worker]() {
      for (std::size_t iteration = 0; iteration < 8; ++iteration) {
        if (!nvhttp::set_client_enabled(uuid, (worker + iteration) % 2 == 0)) {
          operations_succeeded = false;
        }
        static_cast<void>(nvhttp::test_support::authorize_client_certificate(certificate));
      }
    });
  }
  workers.clear();

  EXPECT_TRUE(operations_succeeded);
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(ClientAuthorizationTest, EveryPairingStartsWithZeroGrantsAndActionInclusionIsExplicit) {
  for (const auto &name : {"first owner", "guest"}) {
    const auto credentials = test_utils::certificates::generate_ca_credentials(name);
    const auto uuid = nvhttp::test_support::add_client(name, credentials.x509, true);
    ASSERT_FALSE(uuid.empty());
    const auto client = nvhttp::current_client(uuid, credentials.x509);
    ASSERT_TRUE(client);
    EXPECT_EQ(client->permissions, 0U);
    for (const auto right : {permissions::list, permissions::view, permissions::launch, permissions::keyboard, permissions::mouse, permissions::controller, permissions::pen, permissions::touch}) {
      bool dispatched = false;
      EXPECT_FALSE(rtsp_stream::authorize_operation(uuid, credentials.x509, right, [&]() {
        dispatched = true;
      }));
      EXPECT_FALSE(dispatched);
    }
  }
  EXPECT_TRUE(permissions::allows(permissions::launch, permissions::view));
  EXPECT_TRUE(permissions::allows(permissions::view, permissions::list));
  EXPECT_TRUE(permissions::allows(permissions::launch, permissions::list));
  EXPECT_FALSE(permissions::allows(permissions::list, permissions::view));
  EXPECT_FALSE(permissions::allows(permissions::view, permissions::launch));
  EXPECT_FALSE(permissions::allows(permissions::view, permissions::keyboard));
}

TEST_F(ClientAuthorizationTest, ExplicitGrantsPersistAndOmittedEnabledPreservesDisabledGate) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("paired", credentials.x509, true);
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view | permissions::keyboard));
  const auto devices = nvhttp::get_all_clients();
  ASSERT_EQ(devices.size(), 1U);
  EXPECT_FALSE(devices[0]["enabled"].get<bool>());
  EXPECT_EQ(devices[0]["permissions"], permissions::view | permissions::keyboard);
  EXPECT_FALSE(nvhttp::current_client(uuid, credentials.x509));
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_FALSE(nvhttp::current_client(uuid, credentials.x509));
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, true));
  const auto client = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(client);
  EXPECT_EQ(client->permissions, permissions::view | permissions::keyboard);
  EXPECT_TRUE(rtsp_stream::authorize_operation(uuid, credentials.x509, permissions::list, []() {
  }));
  EXPECT_TRUE(rtsp_stream::authorize_operation(uuid, credentials.x509, permissions::keyboard, []() {
  }));
  EXPECT_FALSE(rtsp_stream::authorize_operation(uuid, credentials.x509, permissions::launch, []() {
  }));
}

TEST_F(ClientAuthorizationTest, CurrentActionAuthorityAppliesInclusionWithoutGrantingInputImplicitly) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("explicit grants", credentials.x509, true);
  for (const auto grant : {permissions::list, permissions::view, permissions::launch}) {
    ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, grant));
    for (const auto action : {permissions::list, permissions::view, permissions::launch, permissions::inputs}) {
      bool effect = false;
      const bool expected = action == permissions::list || (action == permissions::view && grant != permissions::list) || (action == permissions::launch && grant == permissions::launch);
      EXPECT_EQ(rtsp_stream::authorize_operation(uuid, credentials.x509, action, [&]() {
                  effect = true;
                }),
                expected);
      EXPECT_EQ(effect, expected);
    }
  }
}

TEST_F(ClientAuthorizationTest, MissingMalformedAndUnsupportedPersistedMasksRequireExplicitGrants) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("legacy", credentials.x509, true);
  for (const auto value : {"missing", "", "-1", "+256", "256x", "1.5", " 256", "4294967296", "65536", "1"}) {
    ASSERT_TRUE(http::update_state_file(config::nvhttp.file_state, [&](auto &tree) {
      for (auto &[key, node] : tree.get_child("root.named_devices")) {
        if (std::string_view(value) == "missing") {
          node.erase("permissions");
        } else {
          node.put("permissions", value);
        }
      }
    }));
    nvhttp::test_support::reset_client_state();
    nvhttp::test_support::reload_client_state();
    const auto client = nvhttp::current_client(uuid, credentials.x509);
    ASSERT_TRUE(client);
    EXPECT_EQ(client->permissions, 0U) << value;
  }
  EXPECT_FALSE(nvhttp::update_client(uuid, std::nullopt, 0x00010000U));
  EXPECT_FALSE(nvhttp::update_client(uuid, std::nullopt, 1U));
  EXPECT_FALSE(nvhttp::update_client(uuid, std::nullopt, std::nullopt));
  EXPECT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::supported));
}

TEST_F(ClientAuthorizationTest, FailedPersistencePublishesNeitherGrantNorRevocationAndCanRecover) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("paired", credentials.x509, true);
  const auto state = config::nvhttp.file_state;
  config::nvhttp.file_state = state + "/missing/state.json";
  EXPECT_FALSE(nvhttp::update_client(uuid, std::nullopt, permissions::supported));
  ASSERT_TRUE(nvhttp::current_client(uuid, credentials.x509));
  EXPECT_EQ(nvhttp::current_client(uuid, credentials.x509)->permissions, 0U);
  config::nvhttp.file_state = state;
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view));
  config::nvhttp.file_state = state + "/missing/state.json";
  EXPECT_FALSE(nvhttp::update_client(uuid, false, 0U));
  EXPECT_FALSE(nvhttp::unpair_client(uuid));
  EXPECT_FALSE(nvhttp::erase_all_clients());
  ASSERT_TRUE(nvhttp::current_client(uuid, credentials.x509));
  EXPECT_EQ(nvhttp::current_client(uuid, credentials.x509)->permissions, permissions::view);
  config::nvhttp.file_state = state;
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  ASSERT_TRUE(nvhttp::current_client(uuid, credentials.x509));
  EXPECT_EQ(nvhttp::current_client(uuid, credentials.x509)->permissions, permissions::view);
  EXPECT_TRUE(nvhttp::update_client(uuid, std::nullopt, 0U));
}

TEST_F(ClientAuthorizationTest, DisplayPreferencesPersistWithTypedMetadataWithoutGrantingAnyRights) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("paired", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  const auto foreign = display_policy::json::parse(R"({"flags":[false,null,{},[]],"fraction":1.25,"integer":9007199254740993})");
  ASSERT_TRUE(http::update_state_file_bytes(config::nvhttp.file_state, [&](const std::string &original) -> std::optional<std::string> {
    auto tree = proc::catalog::parse(original);
    tree["foreign"] = foreign;
    tree["root"]["named_devices"][0]["foreign"] = foreign;
    tree["root"]["named_devices"][0]["display"]["foreign"] = foreign;
    return tree.dump();
  }));
  auto preferences = display_policy::parse_device(display_policy::json::parse(R"({"force_virtual":true,"resolution":{"width":2560,"height":1440},"refresh_rate":{"numerator":60000,"denominator":1001}})"));
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, std::nullopt, preferences));
  ASSERT_EQ(http::save_user_creds(config::nvhttp.file_state, "synthetic administrator", "synthetic password"), 0);
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  const auto paired = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(paired);
  EXPECT_EQ(paired->permissions, 0U);
  EXPECT_EQ(paired->display, preferences);
  EXPECT_TRUE(paired->display_valid);
  std::ifstream saved {config::nvhttp.file_state, std::ios::binary};
  const auto tree = display_policy::json::parse(saved);
  EXPECT_EQ(tree["foreign"], foreign);
  EXPECT_EQ(tree["root"]["named_devices"][0]["foreign"], foreign);
  EXPECT_EQ(tree["root"]["named_devices"][0]["display"]["foreign"], foreign);
  EXPECT_TRUE(tree["root"]["named_devices"][0]["enabled"].is_string());
  EXPECT_TRUE(tree["root"]["named_devices"][0]["permissions"].is_string());
  EXPECT_TRUE(tree["root"]["named_devices"][0]["display"]["force_virtual"].is_boolean());
}

TEST_F(ClientAuthorizationTest, InvalidSavedDisplayPreferencesStayUntouchedUntilExplicitAdminRepair) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("paired", credentials.x509, true);
  const auto malformed = display_policy::json::parse(R"({"force_virtual":true,"refresh_rate":{"numerator":60000,"denominator":0},"foreign":[false,null]})");
  ASSERT_TRUE(http::update_state_file_bytes(config::nvhttp.file_state, [&](const std::string &original) -> std::optional<std::string> {
    auto tree = proc::catalog::parse(original);
    tree["root"]["named_devices"][0]["display"] = malformed;
    return tree.dump();
  }));
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  auto paired = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(paired);
  EXPECT_FALSE(paired->display_valid);
  EXPECT_FALSE(nvhttp::get_all_clients()[0]["display_valid"].get<bool>());
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view));
  {
    std::ifstream saved {config::nvhttp.file_state, std::ios::binary};
    const auto tree = display_policy::json::parse(saved);
    EXPECT_EQ(tree["root"]["named_devices"][0]["display"], malformed);
  }
  display_policy::device_preferences_t repaired;
  repaired.force_virtual = true;
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, std::nullopt, repaired));
  paired = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(paired);
  EXPECT_TRUE(paired->display_valid);
  EXPECT_EQ(paired->display, repaired);
  EXPECT_EQ(paired->permissions, permissions::view);
}

TEST_F(ClientAuthorizationTest, FailedDisplayEditPreservesExactBytesAndPreviousAuthorityAndPreferences) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("paired", credentials.x509, true);
  auto read = [&]() {
    std::ifstream saved {config::nvhttp.file_state, std::ios::binary};
    return std::string {std::istreambuf_iterator<char> {saved}, std::istreambuf_iterator<char> {}};
  };
  const auto original = read();
  const auto before = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(before);
  display_policy::device_preferences_t preferences;
  preferences.force_virtual = true;
  http::test_support::set_state_update_hook([]() {
    throw std::runtime_error {"fixture failure before publication"};
  });
  EXPECT_FALSE(nvhttp::update_client(uuid, false, permissions::launch, preferences));
  http::test_support::set_state_update_hook({});
  EXPECT_EQ(read(), original);
  const auto after = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->permissions, before->permissions);
  EXPECT_EQ(after->revision, before->revision);
  EXPECT_EQ(after->display, before->display);
  preferences.refresh_rate = {60, 0};
  EXPECT_FALSE(nvhttp::update_client(uuid, std::nullopt, permissions::launch, preferences));
  EXPECT_EQ(read(), original);
}

TEST_F(ClientAuthorizationTest, CredentialAndPermissionTransactionsPreserveBothUpdatesInEitherOrder) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("paired", credentials.x509, true);
  for (const bool credentials_first : {true, false}) {
    std::promise<void> first_read;
    std::promise<void> release_first;
    auto released = release_first.get_future().share();
    std::atomic_bool first {true};
    http::test_support::set_state_update_hook([&]() {
      if (first.exchange(false)) {
        first_read.set_value();
        released.wait();
      }
    });
    bool saved_credentials = false;
    bool saved_permissions = false;
    const auto grant = credentials_first ? permissions::view : permissions::launch;
    const auto save_credentials = [&]() {
      saved_credentials = http::save_user_creds(config::nvhttp.file_state, "test administrator", "test password") == 0;
    };
    const auto save_permissions = [&]() {
      saved_permissions = nvhttp::update_client(uuid, std::nullopt, grant);
    };
    std::jthread writer1 {[&]() {
      if (credentials_first) {
        save_credentials();
      } else {
        save_permissions();
      }
    }};
    const auto reached_read = first_read.get_future().wait_for(std::chrono::seconds {5});
    std::jthread writer2 {[&]() {
      if (credentials_first) {
        save_permissions();
      } else {
        save_credentials();
      }
    }};
    release_first.set_value();
    writer1.join();
    writer2.join();
    http::test_support::set_state_update_hook({});
    ASSERT_EQ(reached_read, std::future_status::ready);
    EXPECT_TRUE(saved_credentials);
    EXPECT_TRUE(saved_permissions);
    boost::property_tree::ptree persisted;
    boost::property_tree::read_json(config::nvhttp.file_state, persisted);
    EXPECT_EQ(persisted.get<std::string>("username"), "test administrator");
    const auto salt = persisted.get<std::string>("salt");
    EXPECT_EQ(persisted.get<std::string>("password"), util::hex(crypto::hash("test password" + salt)).to_string());
    EXPECT_EQ(persisted.get_child("root.named_devices").front().second.get<permissions::mask_t>("permissions"), grant);
  }
}

namespace {
  /**
   * @brief Build real launch metadata from the currently authorized original pairing.
   * @param uuid Original paired UUID.
   * @param cert Exact paired certificate.
   * @return Launch admission token.
   */
  std::shared_ptr<rtsp_stream::launch_session_t> permission_launch(const std::string &uuid, const std::string &cert) {
    const SimpleWeb::CaseInsensitiveMultimap args {{"rikey", "00112233445566778899aabbccddeeff"}, {"rikeyid", "1"}, {"appid", "7"}};
    return nvhttp::test_support::make_launch_session(*nvhttp::current_client(uuid, cert), args);
  }
}  // namespace

TEST_F(ClientAuthorizationTest, FreshStateDeviceAdministrationNeverPersistsPairingsOrGrants) {
  const auto credentials = test_utils::certificates::generate_ca_credentials("Ephemeral pairing fixture");
  constexpr auto ordinary_id = "aB123456-ABcd-4111-8222-00EF12345678";
  constexpr auto ephemeral_id = "14a88f00-5a44-4e23-a562-9c50e16fec9f";
  constexpr auto ordinary_client = "4d5a49eb-1ee7-4611-8400-382a16681826";
  const nlohmann::ordered_json named_device {
    {"name", "Ordinary device"},
    {"cert", credentials.x509},
    {"uuid", ordinary_client},
    {"enabled", "true"},
    {"permissions", std::to_string(permissions::launch)},
  };
  const auto ordinary = nlohmann::ordered_json {{"root", {{"uniqueid", ordinary_id}, {"named_devices", nlohmann::ordered_json::array({named_device})}}}}.dump();
  write_state(ordinary);
  config::sunshine.flags[config::flag::FRESH_STATE] = true;
  http::unique_id = ephemeral_id;
  nvhttp::initialize_state();
  EXPECT_FALSE(nvhttp::shortcut_host_uuid());
  EXPECT_EQ(http::unique_id, ephemeral_id);
  EXPECT_TRUE(nvhttp::get_all_clients().empty());
  EXPECT_FALSE(nvhttp::current_client(ordinary_client, credentials.x509));

  const auto uuid = nvhttp::test_support::add_client("Ephemeral guest", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  const auto paired = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(paired);
  EXPECT_EQ(paired->permissions, 0U);
  EXPECT_FALSE(rtsp_stream::authorize_operation(uuid, credentials.x509, permissions::view, []() {
  }));

  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view));
  const auto granted = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(granted);
  EXPECT_GT(granted->revision, paired->revision);
  EXPECT_EQ(granted->permissions, permissions::view);
  const auto token = permission_launch(uuid, credentials.x509);
  ASSERT_EQ(rtsp_stream::prepare_launch(token, []() {
              return 200;
            }),
            200);
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, 0U));
  EXPECT_FALSE(rtsp_stream::test_support::token_authorized(token));
  EXPECT_FALSE(rtsp_stream::test_support::pending_launch());
  EXPECT_TRUE(nvhttp::unpair_client(uuid));
  EXPECT_FALSE(nvhttp::current_client(uuid, credentials.x509));

  ASSERT_FALSE(nvhttp::test_support::add_client("Another ephemeral guest", credentials.x509, true).empty());
  EXPECT_TRUE(nvhttp::erase_all_clients());
  EXPECT_TRUE(nvhttp::get_all_clients().empty());
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
  EXPECT_TRUE(read_state() == ordinary);
  EXPECT_FALSE(nvhttp::shortcut_host_uuid());

  ASSERT_TRUE(fs::remove(config::nvhttp.file_state));
  const auto absent = nvhttp::test_support::add_client("Ephemeral guest without ordinary state", credentials.x509, true);
  ASSERT_FALSE(absent.empty());
  EXPECT_TRUE(nvhttp::update_client(absent, std::nullopt, permissions::view));
  EXPECT_TRUE(nvhttp::erase_all_clients());
  EXPECT_FALSE(fs::exists(config::nvhttp.file_state));
}

TEST_F(ClientAuthorizationTest, DisplayOnlyEditsPreserveAuthorizationRevisionAndPreparedVideoTokens) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("paired", credentials.x509, true);
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view));
  const auto before = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(before);
  const auto token = permission_launch(uuid, credentials.x509);
  ASSERT_EQ(rtsp_stream::prepare_launch(token, []() {
              return 200;
            }),
            200);
  display_policy::device_preferences_t preferences;
  preferences.force_virtual = true;
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, std::nullopt, preferences));
  const auto after = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->revision, before->revision);
  EXPECT_EQ(after->permissions, before->permissions);
  EXPECT_EQ(after->display, preferences);
  EXPECT_TRUE(rtsp_stream::test_support::token_authorized(token));
  EXPECT_TRUE(rtsp_stream::test_support::claim_launch(token));
  rtsp_stream::launch_session_clear(token->id);
}

TEST_F(ClientAuthorizationTest, InvalidPersistedDocumentShapesCannotPublishDeviceUpdatesOrCancelPreparedTokens) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("paired", credentials.x509, true);
  display_policy::device_preferences_t original_preferences;
  original_preferences.force_virtual = true;
  original_preferences.resolution = {2560, 1440};
  original_preferences.refresh_rate = {60000, 1001};
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view, original_preferences));
  const auto before = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(before);
  auto read = [&]() {
    std::ifstream input {config::nvhttp.file_state, std::ios::binary};
    return std::string {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
  };
  const auto valid_bytes = read();
  auto duplicated = proc::catalog::parse(valid_bytes);
  auto &devices = duplicated["root"]["named_devices"];
  const auto duplicate = devices[0];
  devices.push_back(duplicate);
  const std::vector<std::string> malformed_documents {
    "[]",
    "null",
    R"({"root":false})",
    R"({"root":null})",
    R"({"root":{"named_devices":false}})",
    R"({"root":{"named_devices":{}}})",
    duplicated.dump(),
  };
  const auto token = permission_launch(uuid, credentials.x509);
  ASSERT_EQ(rtsp_stream::prepare_launch(token, []() {
              return 200;
            }),
            200);
  display_policy::device_preferences_t changed_preferences;
  changed_preferences.resolution = {3840, 2160};
  for (std::size_t shape = 0; shape < malformed_documents.size(); ++shape) {
    SCOPED_TRACE(shape);
    const auto &malformed = malformed_documents[shape];
    {
      std::ofstream output {config::nvhttp.file_state, std::ios::binary};
      output << malformed;
      ASSERT_TRUE(output.good());
    }
    const auto expect_rejected = [&](std::optional<bool> enabled, std::optional<permissions::mask_t> grant, std::optional<display_policy::device_preferences_t> display) {
      EXPECT_FALSE(nvhttp::update_client(uuid, enabled, grant, display));
      EXPECT_EQ(read(), malformed);
      const auto after = nvhttp::current_client(uuid, credentials.x509);
      ASSERT_TRUE(after);
      EXPECT_EQ(after->permissions, before->permissions);
      EXPECT_EQ(after->revision, before->revision);
      EXPECT_EQ(after->display, before->display);
      EXPECT_EQ(after->display_valid, before->display_valid);
      EXPECT_TRUE(rtsp_stream::test_support::token_authorized(token));
    };
    expect_rejected(std::nullopt, std::nullopt, changed_preferences);
    expect_rejected(std::nullopt, permissions::launch, std::nullopt);
    expect_rejected(false, permissions::launch, changed_preferences);
  }
  {
    std::ofstream output {config::nvhttp.file_state, std::ios::binary};
    output << valid_bytes;
    ASSERT_TRUE(output.good());
  }
  EXPECT_TRUE(nvhttp::update_client(uuid, std::nullopt, std::nullopt, changed_preferences));
  const auto recovered = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->display, changed_preferences);
  EXPECT_EQ(recovered->revision, before->revision);
  EXPECT_TRUE(rtsp_stream::test_support::claim_launch(token));
  rtsp_stream::launch_session_clear(token->id);
}

TEST_F(ClientAuthorizationTest, LegacyEmptyPersistedDeviceCollectionStillAcceptsAValidPreferenceSave) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("paired", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  {
    std::ifstream input {config::nvhttp.file_state, std::ios::binary};
    auto legacy = display_policy::json::parse(input);
    input.close();
    legacy["root"]["named_devices"] = "";
    std::ofstream output {config::nvhttp.file_state, std::ios::binary};
    output << legacy.dump();
    ASSERT_TRUE(output.good());
  }
  display_policy::device_preferences_t preferences;
  preferences.force_virtual = true;
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, std::nullopt, preferences));
  const auto paired = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(paired);
  EXPECT_EQ(paired->permissions, 0U);
  EXPECT_EQ(paired->display, preferences);
  std::ifstream input {config::nvhttp.file_state, std::ios::binary};
  const auto saved = display_policy::json::parse(input);
  ASSERT_TRUE(saved["root"]["named_devices"].is_array());
  ASSERT_EQ(saved["root"]["named_devices"].size(), 1U);
  EXPECT_EQ(saved["root"]["named_devices"][0]["uuid"], uuid);
}

#ifdef _WIN32
TEST_F(ClientAuthorizationTest, NativePersistenceOutcomesPreserveOrPublishRealAuthorityAndAdmissions) {
  using stage_t = http::test_support::state_file_stage_e;
  const auto state = config::nvhttp.file_state;
  auto restore = util::fail_guard([&]() {
    http::test_support::set_state_file_hook({});
    config::nvhttp.file_state = state;
    const fs::path base {state};
    for (const auto &entry : fs::directory_iterator(base.parent_path())) {
      if (entry.path().filename().string().starts_with(base.filename().string() + ".")) {
        std::error_code error;
        fs::remove(entry.path(), error);
      }
    }
  });
  const auto read_bytes = [](const std::string &path) {
    std::ifstream input {path};
    return std::string {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  };
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("native state outcomes", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view));
  for (const auto rejected : {stage_t::backup, stage_t::write, stage_t::flush, stage_t::close, stage_t::rename, stage_t::rename_open, stage_t::rename_api, stage_t::rename_close, stage_t::verify}) {
    SCOPED_TRACE(static_cast<int>(rejected));
    const auto original = *nvhttp::current_client(uuid, credentials.x509);
    const auto bytes = read_bytes(state);
    const auto token = permission_launch(uuid, credentials.x509);
    ASSERT_EQ(rtsp_stream::prepare_launch(token, []() {
                return 200;
              }),
              200);
    unsigned closes = 0;
    http::test_support::set_state_file_hook([&](auto stage, const auto &, const auto &, const auto &) -> std::optional<std::uint32_t> {
      if (stage == rejected && (rejected != stage_t::rename_close || ++closes % 2 == 1)) {
        return ERROR_WRITE_FAULT;
      }
      return std::nullopt;
    });
    EXPECT_FALSE(nvhttp::update_client(uuid, std::nullopt, permissions::view | permissions::keyboard));
    EXPECT_FALSE(nvhttp::update_client(uuid, std::nullopt, 0U));
    http::test_support::set_state_file_hook({});
    const auto after = nvhttp::current_client(uuid, credentials.x509);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->permissions, original.permissions);
    EXPECT_EQ(after->revision, original.revision);
    EXPECT_TRUE(read_bytes(state) == bytes);
    EXPECT_TRUE(rtsp_stream::test_support::token_authorized(token));
    ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view | permissions::keyboard));
    EXPECT_FALSE(rtsp_stream::test_support::token_authorized(token));
    ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view));
  }

  const auto before_cleanup = *nvhttp::current_client(uuid, credentials.x509);
  http::test_support::set_state_file_hook([](auto stage, const auto &, const auto &, const auto &) -> std::optional<std::uint32_t> {
    return stage == stage_t::cleanup ? std::optional<std::uint32_t> {ERROR_ACCESS_DENIED} : std::nullopt;
  });
  EXPECT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view | permissions::keyboard));
  http::test_support::set_state_file_hook({});
  const auto committed = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(committed);
  EXPECT_EQ(committed->permissions, permissions::view | permissions::keyboard);
  EXPECT_NE(committed->revision, before_cleanup.revision);
  boost::property_tree::ptree saved;
  boost::property_tree::read_json(state, saved);
  EXPECT_EQ(saved.get_child("root.named_devices").front().second.get<permissions::mask_t>("permissions"), committed->permissions);

  // Keep the intentionally blocked recovery key separate from every other authorization fixture.
  config::nvhttp.file_state = state + ".uncertain";
  ASSERT_TRUE(http::update_state_file(config::nvhttp.file_state, [&](auto &tree) {
    tree = saved;
  }));
  const auto original = *nvhttp::current_client(uuid, credentials.x509);
  const auto old_bytes = read_bytes(config::nvhttp.file_state);
  const auto token = permission_launch(uuid, credentials.x509);
  ASSERT_EQ(rtsp_stream::prepare_launch(token, []() {
              return 200;
            }),
            200);
  http::test_support::set_state_file_hook([](auto stage, const auto &, const auto &, const auto &) -> std::optional<std::uint32_t> {
    return stage == stage_t::verify || stage == stage_t::restore ? std::optional<std::uint32_t> {ERROR_ACCESS_DENIED} : std::nullopt;
  });
  EXPECT_FALSE(nvhttp::update_client(uuid, std::nullopt, 0U));
  http::test_support::set_state_file_hook({});
  auto after = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->permissions, original.permissions);
  EXPECT_EQ(after->revision, original.revision);
  EXPECT_TRUE(rtsp_stream::test_support::token_authorized(token));
  fs::path backup;
  const fs::path uncertain {config::nvhttp.file_state};
  for (const auto &entry : fs::directory_iterator(uncertain.parent_path())) {
    if (entry.path().filename().string().starts_with(uncertain.filename().string() + ".backup-")) {
      backup = entry.path();
    }
  }
  ASSERT_FALSE(backup.empty());
  EXPECT_TRUE(read_bytes(backup.string()) == old_bytes);
  ASSERT_TRUE(MoveFileExW(backup.c_str(), uncertain.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
  EXPECT_TRUE(read_bytes(uncertain.string()) == old_bytes);
  EXPECT_FALSE(nvhttp::update_client(uuid, std::nullopt, 0U));
  after = nvhttp::current_client(uuid, credentials.x509);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->permissions, original.permissions);
  EXPECT_EQ(after->revision, original.revision);
  EXPECT_TRUE(rtsp_stream::test_support::token_authorized(token));
}
#endif

TEST_F(ClientAuthorizationTest, ReservationsFailExplicitlyInvalidateHeldTokensAndClaimAnnounceOnce) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("paired", credentials.x509, true);
  int prepared = 0;
  auto denied = permission_launch(uuid, credentials.x509);
  EXPECT_EQ(rtsp_stream::prepare_launch(denied, [&]() {
              ++prepared;
              return 200;
            }),
            403);
  EXPECT_EQ(prepared, 0);
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view));
  auto pending = permission_launch(uuid, credentials.x509);
  EXPECT_EQ(rtsp_stream::prepare_launch(pending, [&]() {
              ++prepared;
              return 200;
            }),
            200);
  auto blocked = permission_launch(uuid, credentials.x509);
  EXPECT_EQ(rtsp_stream::prepare_launch(blocked, [&]() {
              ++prepared;
              return 200;
            }),
            503);
  EXPECT_EQ(prepared, 1);
  EXPECT_TRUE(rtsp_stream::test_support::claim_launch(pending));
  EXPECT_FALSE(rtsp_stream::test_support::claim_launch(pending));
  rtsp_stream::launch_session_clear(pending->id);
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, 0U));
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view));
  EXPECT_FALSE(rtsp_stream::test_support::token_authorized(pending));
  EXPECT_FALSE(rtsp_stream::test_support::claim_launch(pending));
  auto failed = permission_launch(uuid, credentials.x509);
  int rollback = 0;
  EXPECT_EQ(rtsp_stream::prepare_launch(failed, []() {
              return 500;
            },
                                        [&]() {
                                          ++rollback;
                                        }),
            500);
  EXPECT_EQ(rollback, 1);
  EXPECT_FALSE(rtsp_stream::test_support::token_authorized(failed));
  auto retry = permission_launch(uuid, credentials.x509);
  EXPECT_EQ(rtsp_stream::prepare_launch(retry, []() {
              return 200;
            }),
            200);
  rtsp_stream::launch_session_clear(retry->id);
  EXPECT_FALSE(rtsp_stream::test_support::token_authorized(retry));
}

TEST_F(ClientAuthorizationTest, ARevokeDoesNotInvalidateAnotherPrincipalsPendingAdmission) {
  const auto a = test_utils::certificates::generate_ca_credentials("client A");
  const auto b = test_utils::certificates::generate_ca_credentials("client B");
  const auto ua = nvhttp::test_support::add_client("A", a.x509, true);
  const auto ub = nvhttp::test_support::add_client("B", b.x509, true);
  ASSERT_TRUE(nvhttp::update_client(ua, std::nullopt, permissions::view));
  ASSERT_TRUE(nvhttp::update_client(ub, std::nullopt, permissions::view));
  const auto active_token = permission_launch(ua, a.x509);
  ASSERT_EQ(rtsp_stream::prepare_launch(active_token, []() {
              return 200;
            }),
            200);
  ASSERT_TRUE(rtsp_stream::test_support::claim_launch(active_token));
  rtsp_stream::launch_session_clear(active_token->id);
  const auto other = permission_launch(ub, b.x509);
  ASSERT_EQ(rtsp_stream::prepare_launch(other, []() {
              return 200;
            }),
            200);
  ASSERT_TRUE(nvhttp::update_client(ua, std::nullopt, 0U));
  EXPECT_FALSE(rtsp_stream::test_support::token_authorized(active_token));
  EXPECT_TRUE(rtsp_stream::test_support::token_authorized(other));
  rtsp_stream::stop_application({});
  EXPECT_FALSE(rtsp_stream::test_support::token_authorized(other));
}

TEST_F(ClientAuthorizationTest, FixtureDrainConsumesDeferredLivenessAndPreservesInflightProcessBusyRejection) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("paired", credentials.x509, true);
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view));
  rtsp_stream::refresh_process_liveness();
  auto owner = rtsp_stream::test_support::start_owner();
  std::promise<void> work_entered;
  std::promise<void> release_work;
  const auto released = release_work.get_future().share();
  auto blocking = rtsp_stream::process_operation([&]() {
    work_entered.set_value();
    released.wait();
  });
  bool work_released = false;
  const auto release = [&]() {
    if (!work_released) {
      release_work.set_value();
      work_released = true;
    }
  };
  auto release_on_failure = util::fail_guard([&]() {
    release();
    blocking.wait();
  });
  ASSERT_EQ(work_entered.get_future().wait_for(std::chrono::seconds {5}), std::future_status::ready);
  const auto blocked = permission_launch(uuid, credentials.x509);
  int prepared = 0;
  int rolled_back = 0;
  EXPECT_EQ(rtsp_stream::prepare_launch(blocked, [&]() {
              ++prepared;
              return 200;
            },
                                        [&]() {
                                          ++rolled_back;
                                        }),
            503);
  EXPECT_EQ(prepared, 0);
  EXPECT_EQ(rolled_back, 0);
  std::promise<void> process_drain_blocked;
  std::atomic_bool drain_observed {false};
  rtsp_stream::test_support::set_owner_hook([&](std::string_view event) {
    if (event == "stop_process_work_pending" && !drain_observed.exchange(true)) {
      process_drain_blocked.set_value();
    }
  });
  auto clear_hook = util::fail_guard([]() {
    rtsp_stream::test_support::set_owner_hook({});
  });
  auto drained = std::async(std::launch::async, [owner = std::move(owner)]() mutable {
    drain_owner(std::move(owner));
  });
  auto release_before_join = util::fail_guard(release);
  ASSERT_EQ(process_drain_blocked.get_future().wait_for(std::chrono::seconds {5}), std::future_status::ready);
  EXPECT_EQ(drained.wait_for(std::chrono::seconds {0}), std::future_status::timeout);
  release();
  ASSERT_EQ(drained.wait_for(std::chrono::seconds {5}), std::future_status::ready);
  drained.get();
  release_before_join.disable();
  release_on_failure.disable();
  clear_hook.disable();
  blocking.get();
  auto fresh_owner = rtsp_stream::test_support::start_owner();
  const auto admitted = permission_launch(uuid, credentials.x509);
  EXPECT_EQ(rtsp_stream::prepare_launch(admitted, [&]() {
              ++prepared;
              return 200;
            }),
            200);
  EXPECT_EQ(prepared, 1);
  EXPECT_EQ(rolled_back, 0);
  rtsp_stream::launch_session_clear(admitted->id);
}

TEST_F(ClientAuthorizationTest, RevocationDuringPreparationRollsBackBeforeAcknowledgingUpdate) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("paired", credentials.x509, true);
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view));
  auto owner = rtsp_stream::test_support::start_owner();
  std::promise<void> preparing;
  std::promise<void> continue_prepare;
  const auto continued = continue_prepare.get_future().share();
  std::promise<void> mutation_posted;
  std::atomic_bool observe_mutation {false};
  std::atomic_bool observed {false};
  rtsp_stream::test_support::set_owner_hook([&](std::string_view event) {
    if (event == "operation_posted" && observe_mutation && !observed.exchange(true)) {
      mutation_posted.set_value();
    }
  });
  const auto token = permission_launch(uuid, credentials.x509);
  int result = 0;
  int rolled_back = 0;
  std::jthread launch {[&]() {
    result = rtsp_stream::prepare_launch(token, [&]() {
      preparing.set_value();
      continued.wait();
      return 200;
    },
                                         [&]() {
                                           ++rolled_back;
                                         });
  }};
  const auto reached_prepare = preparing.get_future().wait_for(std::chrono::seconds {5});
  observe_mutation = true;
  bool saved = false;
  std::jthread revoke {[&]() {
    saved = nvhttp::update_client(uuid, std::nullopt, 0U);
  }};
  const auto published = mutation_posted.get_future().wait_for(std::chrono::seconds {5});
  continue_prepare.set_value();
  launch.join();
  revoke.join();
  EXPECT_EQ(reached_prepare, std::future_status::ready);
  EXPECT_EQ(published, std::future_status::ready);
  EXPECT_EQ(result, 403);
  EXPECT_EQ(rolled_back, 1);
  EXPECT_TRUE(saved);
  EXPECT_FALSE(rtsp_stream::test_support::token_authorized(token));
  rtsp_stream::test_support::set_owner_hook({});
}

TEST_F(ClientAuthorizationTest, OperationsArrivingDuringOwnerShutdownWaitForCleanupAndRejectNewAdmissions) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("paired", credentials.x509, true);
  ASSERT_TRUE(nvhttp::update_client(uuid, std::nullopt, permissions::view));
  auto owner = rtsp_stream::test_support::start_owner();
  const auto token = permission_launch(uuid, credentials.x509);
  ASSERT_EQ(rtsp_stream::prepare_launch(token, []() {
              return 200;
            }),
            200);
  std::promise<void> stop_started;
  std::promise<void> continue_stop;
  auto continued = continue_stop.get_future().share();
  std::promise<void> waiter_arrived;
  std::atomic_bool arrived {false};
  rtsp_stream::test_support::set_owner_hook([&](std::string_view event) {
    if (event == "stop_begin") {
      stop_started.set_value();
      continued.wait();
    }
    if (event == "stopping_wait" && !arrived.exchange(true)) {
      waiter_arrived.set_value();
    }
  });
  std::jthread stop {[]() {
    rtsp_stream::test_support::stop_owner();
  }};
  const auto reached_stop = stop_started.get_future().wait_for(std::chrono::seconds {5});
  auto waiting = std::async(std::launch::async, [&]() {
    return rtsp_stream::test_support::token_authorized(token);
  });
  const auto reached_waiter = waiter_arrived.get_future().wait_for(std::chrono::seconds {5});
  EXPECT_EQ(waiting.wait_for(std::chrono::seconds {0}), std::future_status::timeout);
  continue_stop.set_value();
  stop.join();
  EXPECT_EQ(reached_stop, std::future_status::ready);
  EXPECT_EQ(reached_waiter, std::future_status::ready);
  EXPECT_FALSE(waiting.get());
  const auto replacement = permission_launch(uuid, credentials.x509);
  EXPECT_EQ(rtsp_stream::prepare_launch(replacement, []() {
              return 200;
            }),
            503);
}
