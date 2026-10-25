/**
 * @file tests/unit/test_nvhttp_client_auth.cpp
 * @brief Test exact paired-client certificate authorization and persistence.
 */

// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <filesystem>
#include <future>
#include <thread>
#include <type_traits>
#include <vector>

// lib includes
#include <boost/property_tree/json_parser.hpp>

// local includes
#include <src/config.h>
#include <src/httpcommon.h>
#include <src/nvhttp.h>
#include <src/rtsp.h>

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
    original_state_file = config::nvhttp.file_state;
    original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
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
    rtsp_stream::stop_application({});
    nvhttp::test_support::reset_client_state();
    std::error_code remove_error;
    fs::remove(state_file, remove_error);

    config::nvhttp.file_state = original_state_file;
    config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
    BaseTest::TearDown();
  }

private:
  fs::path state_file;  ///< Task-specific persisted state fixture.
  std::string original_state_file;  ///< State-file setting restored after each test.
  bool original_fresh_state;  ///< Fresh-state flag restored after each test.
};

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
