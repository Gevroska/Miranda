/**
 * @file tests/unit/test_nvhttp_https.cpp
 * @brief Verify HTTPS callbacks retain the guard protecting server lifetime.
 */

// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// local includes
#include <src/config.h>
#include <src/crypto.h>
#include <src/nvhttp.h>
#include <src/rtsp.h>

namespace {
  using namespace std::chrono_literals;
  using boost::asio::ip::tcp;

  /**
   * @brief Run real HTTPS callbacks against an isolated loopback listener.
   */
  struct HttpsHandlerLifetimeTest: BaseTest {
    /**
     * @brief Create credentials and start the production HTTPS accept handler.
     */
    void SetUp() override {
      BaseTest::SetUp();
      const auto credentials = crypto::gen_creds("Sunshine HTTPS lifetime test", 2048);
      certificate_file = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "https_lifetime_cert.pem";
      private_key_file = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "https_lifetime_key.pem";
      std::ofstream {certificate_file} << credentials.x509;
      std::ofstream {private_key_file} << credentials.pkey;

      server = nvhttp::test_support::make_https_server(certificate_file.string(), private_key_file.string(), handler_scope);
      server->io_service = std::make_shared<boost::asio::io_context>();
      server->config.address = "127.0.0.1";
      server->config.port = 0;
      server->start([this](unsigned short assigned_port) {
        port = assigned_port;
      });
      server->io_service->poll();
      ASSERT_NE(port, 0);
    }

    /**
     * @brief Drain canceled callbacks and remove the generated credentials.
     */
    void TearDown() override {
      if (server) {
        server->stop();
        server->io_service->restart();
        server->io_service->poll();
        server.reset();
      }
      if (stop_thread.joinable()) {
        stop_thread.join();
      }
      std::error_code cleanup_error;
      std::filesystem::remove(certificate_file, cleanup_error);
      std::filesystem::remove(private_key_file, cleanup_error);
      BaseTest::TearDown();
    }

    /**
     * @brief Check that stopping the scope runner waits until the error callback exits.
     *
     * @param callback_error Error delivered by the real accept or TLS handshake operation.
     */
    void check_handler_guard(const boost::system::error_code &callback_error) {
      EXPECT_TRUE(callback_error);
      ++error_callbacks;
      std::promise<void> stopping;
      auto stopping_future = stopping.get_future();
      std::promise<void> stopped;
      stopped_future = stopped.get_future();
      stop_thread = std::jthread([this, stopping = std::move(stopping), stopped = std::move(stopped)]() mutable {
        stopping.set_value();
        handler_scope->stop();
        stopped.set_value();
      });
      stopping_future.wait();
      EXPECT_EQ(stopped_future.wait_for(50ms), std::future_status::timeout);
    }

    std::unique_ptr<SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>> server;  ///< Server running production accept and handshake callbacks.
    std::shared_ptr<SimpleWeb::ScopeRunner> handler_scope;  ///< Guard used by the server destructor.
    std::jthread stop_thread;  ///< Concurrent attempt to stop the handler scope.
    std::future<void> stopped_future;  ///< Signals that all callback guards have been released.
    unsigned short port = 0;  ///< Assigned loopback port.
    int error_callbacks = 0;  ///< Number of errors delivered while testing a handler.
    std::filesystem::path certificate_file;  ///< Generated server certificate.
    std::filesystem::path private_key_file;  ///< Generated server private key.
  };

  /**
   * @brief Canceling a pending accept holds the server guard through its error callback.
   */
  TEST_F(HttpsHandlerLifetimeTest, AcceptErrorRetainsGuardUntilCallbackReturns) {
    server->on_error = [this](auto, const auto &callback_error) {
      check_handler_guard(callback_error);
    };
    server->stop();
    server->io_service->run_for(2s);
    ASSERT_EQ(error_callbacks, 1);
    ASSERT_TRUE(stopped_future.valid());
    EXPECT_EQ(stopped_future.wait_for(2s), std::future_status::ready);
  }

  /**
   * @brief A failed TLS handshake holds the server guard through its error callback.
   */
  TEST_F(HttpsHandlerLifetimeTest, HandshakeErrorRetainsGuardUntilCallbackReturns) {
    server->on_error = [this](auto, const auto &callback_error) {
      check_handler_guard(callback_error);
    };
    boost::asio::io_context client_io;
    tcp::socket client {client_io};
    client.connect({boost::asio::ip::address_v4::loopback(), port});
    client.close();
    server->io_service->run_for(2s);
    ASSERT_EQ(error_callbacks, 1);
    ASSERT_TRUE(stopped_future.valid());
    EXPECT_EQ(stopped_future.wait_for(2s), std::future_status::ready);
  }

  /**
   * @brief A stopped server scope prevents a canceled accept from dispatching an error.
   */
  TEST_F(HttpsHandlerLifetimeTest, StoppedScopeSkipsAcceptCallback) {
    server->on_error = [this](auto, const auto &) {
      ++error_callbacks;
    };
    handler_scope->stop();
    server->stop();
    server->io_service->run_for(2s);
    EXPECT_EQ(error_callbacks, 0);
  }

  /**
   * @brief A stopped server scope prevents an outstanding handshake from dispatching an error.
   */
  TEST_F(HttpsHandlerLifetimeTest, StoppedScopeSkipsHandshakeCallback) {
    server->on_error = [this](auto, const auto &) {
      ++error_callbacks;
    };
    boost::asio::io_context client_io;
    tcp::socket client {client_io};
    client.connect({boost::asio::ip::address_v4::loopback(), port});
    server->io_service->poll();
    handler_scope->stop();
    client.close();
    server->io_service->run_for(2s);
    EXPECT_EQ(error_callbacks, 0);
  }

  /**
   * @brief A stalled handshake retains its server guard through the timeout error callback.
   */
  TEST_F(HttpsHandlerLifetimeTest, HandshakeTimeoutRetainsGuardUntilCallbackReturns) {
    server->config.timeout_request = 1;
    server->on_error = [this](auto, const auto &callback_error) {
      check_handler_guard(callback_error);
    };
    boost::asio::io_context client_io;
    tcp::socket client {client_io};
    client.connect({boost::asio::ip::address_v4::loopback(), port});
    server->io_service->run_for(2s);
    ASSERT_EQ(error_callbacks, 1);
    ASSERT_TRUE(stopped_future.valid());
    EXPECT_EQ(stopped_future.wait_for(2s), std::future_status::ready);
  }

  /**
   * @brief Exercise the production TLS verifier and authorization wrapper over real loopback connections.
   */
  struct HttpsClientIdentityTest: BaseTest {
    using server_t = SimpleWeb::Server<nvhttp::SunshineHTTPS>;
    using request_t = std::shared_ptr<server_t::Request>;

    /**
     * @brief Completion state retained safely if a bounded asynchronous operation times out.
     */
    struct completion_t {
      bool done = false;  ///< Whether the operation's callback has run.
      boost::system::error_code error;  ///< Error reported by the asynchronous operation.
    };

    /**
     * @brief TLS client that retains response bytes across keepalive and pipelined exchanges.
     */
    struct client_t {
      boost::asio::ssl::context context {boost::asio::ssl::context::tls_client};  ///< Isolated client credentials.
      SimpleWeb::HTTPS socket;  ///< Real TLS connection to the production test listener.
      boost::asio::streambuf incoming;  ///< Buffered response bytes, including a following pipelined response.
      std::size_t pending_responses = 0;  ///< Number of sent requests whose responses have not been consumed.
      bool close_expected = false;  ///< Whether the final queued request asks the server to close the connection.

      /**
       * @brief Load client credentials before the TLS stream copies its context.
       * @param credentials Optional client certificate and private key.
       * @return Configured client TLS context.
       */
      static boost::asio::ssl::context make_context(const crypto::creds_t *credentials) {
        boost::asio::ssl::context context {boost::asio::ssl::context::tls_client};
        context.set_verify_mode(boost::asio::ssl::verify_none);
        if (credentials) {
          context.use_certificate_chain(boost::asio::buffer(credentials->x509));
          context.use_private_key(boost::asio::buffer(credentials->pkey), boost::asio::ssl::context::pem);
        }
        return context;
      }

      /**
       * @brief Prepare a client with optional credentials and disabled server verification.
       * @param io Client event loop used to await bounded asynchronous completions.
       * @param credentials Client certificate/key, or no value to test missing peer certificates.
       */
      client_t(boost::asio::io_context &io, const crypto::creds_t *credentials):
          context(make_context(credentials)),
          socket(io, context) {
      }
    };

    /**
     * @brief Isolate client records and prepare two independently paired TLS clients.
     */
    void SetUp() override {
      BaseTest::SetUp();
      original_state_file = config::nvhttp.file_state;
      original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
      state_file = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "https_identity_state.json";
      config::nvhttp.file_state = state_file.string();
      config::sunshine.flags[config::flag::FRESH_STATE] = true;
      nvhttp::test_support::reset_client_state();
      std::error_code cleanup_error;
      std::filesystem::remove(state_file, cleanup_error);

      server_credentials = crypto::gen_creds("Sunshine HTTPS identity server", 2048);
      credentials_a = test_utils::certificates::generate_ca_credentials("Sunshine HTTPS client A");
      credentials_b = test_utils::certificates::generate_ca_credentials("Sunshine HTTPS client B");
      uuid_a = nvhttp::test_support::add_client("client A", credentials_a.x509, true);
      uuid_b = nvhttp::test_support::add_client("client B", credentials_b.x509, true);
      ASSERT_FALSE(uuid_a.empty());
      ASSERT_FALSE(uuid_b.empty());
      certificate_file = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "https_identity_cert.pem";
      private_key_file = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "https_identity_key.pem";
      std::ofstream {certificate_file} << server_credentials.x509;
      std::ofstream {private_key_file} << server_credentials.pkey;
    }

    /**
     * @brief Cancel connections and callbacks before removing credentials and restoring client state.
     */
    void TearDown() override {
      for (const auto &client : clients) {
        boost::system::error_code close_error;
        client->socket.lowest_layer().close(close_error);
      }
      if (server) {
        server->stop();
        if (server_thread.joinable()) {
          server_thread.join();
        }
        server.reset();
      }
      clients.clear();
      rtsp_stream::stop_application({});
      nvhttp::test_support::reset_client_state();
      config::nvhttp.file_state = original_state_file;
      config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
      std::error_code cleanup_error;
      std::filesystem::remove(state_file, cleanup_error);
      std::filesystem::remove(certificate_file, cleanup_error);
      std::filesystem::remove(private_key_file, cleanup_error);
      BaseTest::TearDown();
    }

    /**
     * @brief Start a production listener whose test route creates real launch metadata.
     * @param authenticate Whether to install the production TLS certificate verifier.
     * @param gamestream Whether to install the actual protected GameStream routes.
     */
    void start_server(bool authenticate = true, bool gamestream = false) {
      server = nvhttp::test_support::make_https_server(certificate_file.string(), private_key_file.string(), handler_scope, authenticate);
      server->io_service = std::make_shared<boost::asio::io_context>();
      server->config.address = "127.0.0.1";
      server->config.port = 0;
      if (server->verify) {
        auto verifier = server->verify;
        server->verify = [this, verifier = std::move(verifier)](SSL *ssl) {
          const auto identity = verifier(ssl);
          ++verified_handshakes;
          boost::asio::post(client_io, []() {
          });
          return identity;
        };
      }
      server->on_error = [this](auto, const auto &) {
        ++error_callbacks;
        boost::asio::post(client_io, []() {
        });
      };
      nvhttp::test_support::set_authenticated_handler(*server, "^/identity$", [this](auto response, auto request, const auto &identity) {
        {
          std::lock_guard lock {capture_mutex};
          captured_launches.emplace_back(nvhttp::test_support::make_launch_session(identity, request->parse_query_string()));
          captured_requests.emplace_back(request);
        }
        response->write(identity.uuid);
      });
      if (gamestream) {
        nvhttp::test_support::set_gamestream_routes(*server, host_audio);
      }
      server->start([this](unsigned short assigned_port) {
        port = assigned_port;
      });
      server->io_service->poll();
      ASSERT_NE(port, 0);
      server_thread = std::jthread([this]() {
        server->io_service->run();
      });
    }

    /**
     * @brief Drive callbacks until an observable completion or a bounded deadline.
     * @param complete Predicate for the event being awaited.
     * @return Whether the event completed before the deadline.
     */
    template<class Predicate>
    bool run_until(Predicate complete) {
      auto timed_out = std::make_shared<bool>(false);
      boost::asio::steady_timer deadline {client_io, 2s};
      deadline.async_wait([timed_out](const auto &error) {
        if (!error) {
          *timed_out = true;
        }
      });
      client_io.restart();
      while (!complete() && !*timed_out) {
        client_io.run_one();
      }
      const bool completed = complete() && !*timed_out;
      deadline.cancel();
      client_io.poll();
      return completed;
    }

    /**
     * @brief Perform a client handshake and wait for the production verifier to bind its identity.
     * @param credentials Client credentials to present, or no value to omit a certificate.
     * @return Connected client retained until fixture cleanup.
     */
    std::shared_ptr<client_t> connect_client(const crypto::creds_t *credentials) {
      auto client = std::make_shared<client_t>(client_io, credentials);
      clients.emplace_back(client);
      client->socket.lowest_layer().connect({boost::asio::ip::address_v4::loopback(), port});
      const auto expected_handshakes = verified_handshakes.load() + (server->verify ? 1 : 0);
      const auto initial_errors = error_callbacks.load();
      auto completion = std::make_shared<completion_t>();
      client->socket.async_handshake(boost::asio::ssl::stream_base::client, [completion](const auto &error) {
        completion->error = error;
        completion->done = true;
      });
      EXPECT_TRUE(run_until([&]() {
        return completion->done && ((credentials || !server->verify) ? verified_handshakes == expected_handshakes : error_callbacks > initial_errors);
      }));
      if (credentials) {
        EXPECT_FALSE(completion->error) << completion->error.message();
      }
      return client;
    }

    /**
     * @brief Send one or more complete protocol requests with a bounded asynchronous completion.
     * @param client TLS client that sends the requests.
     * @param bytes Complete request bytes, optionally containing a pipeline.
     */
    void send(client_t &client, std::string bytes) {
      for (std::size_t offset = 0; (offset = bytes.find("GET ", offset)) != std::string::npos; offset += 4) {
        ++client.pending_responses;
      }
      client.close_expected = bytes.find("Connection: close") != std::string::npos;
      auto buffer = std::make_shared<std::string>(std::move(bytes));
      auto completion = std::make_shared<completion_t>();
      boost::asio::async_write(client.socket, boost::asio::buffer(*buffer), [completion, buffer](const auto &error, std::size_t) {
        completion->error = error;
        completion->done = true;
      });
      ASSERT_TRUE(run_until([&]() {
        return completion->done;
      }));
      ASSERT_FALSE(completion->error) << completion->error.message();
    }

    /**
     * @brief Read one HTTP response while preserving any buffered pipelined successor.
     * @param client Client receiving the response.
     * @return Response body, or an empty value after a failed assertion.
     */
    std::string read_response(client_t &client) {
      auto completion = std::make_shared<completion_t>();
      boost::asio::async_read_until(client.socket, client.incoming, "\r\n\r\n", [completion](const auto &error, std::size_t) {
        completion->error = error;
        completion->done = true;
      });
      if (!run_until([&]() {
            return completion->done;
          }) ||
          completion->error) {
        ADD_FAILURE() << "HTTP header read failed: " << completion->error.message();
        return {};
      }
      std::istream response {&client.incoming};
      std::string line;
      std::getline(response, line);
      EXPECT_EQ(line, "HTTP/1.1 200 OK\r");
      std::size_t content_length = 0;
      bool has_content_length = false;
      while (std::getline(response, line) && line != "\r") {
        if (line.starts_with("Content-Length: ")) {
          content_length = std::stoul(line.substr(16));
          has_content_length = true;
        }
      }
      EXPECT_TRUE(has_content_length);
      if (client.incoming.size() < content_length) {
        completion = std::make_shared<completion_t>();
        boost::asio::async_read(client.socket, client.incoming, boost::asio::transfer_exactly(content_length - client.incoming.size()), [completion](const auto &error, std::size_t) {
          completion->error = error;
          completion->done = true;
        });
        if (!run_until([&]() {
              return completion->done;
            }) ||
            completion->error) {
          ADD_FAILURE() << "HTTP body read failed: " << completion->error.message();
          return {};
        }
      }
      std::string body(content_length, '\0');
      response.read(body.data(), static_cast<std::streamsize>(body.size()));
      if (client.pending_responses > 0) {
        --client.pending_responses;
      }
      if ((client.close_expected && client.pending_responses == 0) || body.find("status_code=\"401\"") != std::string::npos) {
        // Release the production socket's synchronous graceful shutdown before awaiting server progress.
        boost::system::error_code close_error;
        client.socket.lowest_layer().close(close_error);
        auto synchronized = std::make_shared<std::promise<void>>();
        auto ready = synchronized->get_future();
        boost::asio::post(*server->io_service, [synchronized]() {
          synchronized->set_value();
        });
        EXPECT_EQ(ready.wait_for(2s), std::future_status::ready);
      }
      {
        std::lock_guard lock {capture_mutex};
        launches = captured_launches;
        requests = captured_requests;
      }
      return body;
    }

    /**
     * @brief Build an identity probe containing an intentionally untrusted client identifier.
     * @param connection Optional Connection header value.
     * @return Complete HTTP/1.1 request with valid launch-key arguments.
     */
    std::string request(const std::string &connection = {}) {
      return "GET /identity?uniqueid=forged-client&rikey=00112233445566778899aabbccddeeff&rikeyid=1&appid=7 HTTP/1.1\r\nHost: localhost\r\n" +
             (connection.empty() ? "" : "Connection: " + connection + "\r\n") + "\r\n";
    }

    boost::asio::io_context client_io;  ///< Client event loop driven with bounded completion events.
    std::jthread server_thread;  ///< Independent server loop, including production synchronous TLS shutdown.
    std::mutex capture_mutex;  ///< Protects metadata produced by the server thread.
    std::vector<std::shared_ptr<rtsp_stream::launch_session_t>> captured_launches;  ///< Metadata guarded by capture_mutex.
    std::vector<request_t> captured_requests;  ///< Requests guarded by capture_mutex.
    std::unique_ptr<server_t> server;  ///< Production HTTPS specialization under test.
    std::shared_ptr<SimpleWeb::ScopeRunner> handler_scope;  ///< Server callback lifetime guard.
    std::vector<std::shared_ptr<client_t>> clients;  ///< Connections closed before the event loop is destroyed.
    std::vector<std::shared_ptr<rtsp_stream::launch_session_t>> launches;  ///< Actual metadata constructed by authorized requests.
    std::vector<request_t> requests;  ///< Requests retained to test expired connection identity access.
    bool host_audio = false;  ///< Test-owned host audio preference used by real launch/resume routes.
    crypto::creds_t server_credentials;  ///< Server certificate/key fixture.
    crypto::creds_t credentials_a;  ///< First paired client credentials.
    crypto::creds_t credentials_b;  ///< Second paired client credentials.
    std::string uuid_a;  ///< Persistent identity of client A.
    std::string uuid_b;  ///< Persistent identity of client B.
    std::filesystem::path state_file;  ///< Isolated paired-client state file.
    std::filesystem::path certificate_file;  ///< Generated server certificate file.
    std::filesystem::path private_key_file;  ///< Generated server key file.
    std::string original_state_file;  ///< Configuration restored after each test.
    bool original_fresh_state = false;  ///< Fresh-state setting restored after each test.
    unsigned short port = 0;  ///< Assigned loopback listener port.
    std::atomic_int verified_handshakes {0};  ///< Number of completed production verifier calls.
    std::atomic_int error_callbacks {0};  ///< Number of failed server operations.
  };

  TEST_F(HttpsClientIdentityTest, InterleavedHandshakesRetainEachLaunchIdentity) {
    start_server();
    for (const bool reverse : {false, true}) {
      auto first = connect_client(reverse ? &credentials_b : &credentials_a);
      auto second = connect_client(reverse ? &credentials_a : &credentials_b);
      send(*first, request());
      EXPECT_EQ(read_response(*first), reverse ? uuid_b : uuid_a);
      send(*second, request());
      EXPECT_EQ(read_response(*second), reverse ? uuid_a : uuid_b);
    }
    ASSERT_EQ(launches.size(), 4);
    for (std::size_t index = 0; index < launches.size(); ++index) {
      const bool is_a = index == 0 || index == 3;
      EXPECT_EQ(launches[index]->client_uuid, is_a ? uuid_a : uuid_b);
      EXPECT_EQ(launches[index]->client_cert, is_a ? credentials_a.x509 : credentials_b.x509);
      EXPECT_EQ(launches[index]->client_name, is_a ? "client A" : "client B");
      EXPECT_EQ(launches[index]->unique_id, "forged-client");
    }
  }

  TEST_F(HttpsClientIdentityTest, KeepaliveAndPipelineRetainConnectionIdentity) {
    start_server();
    auto client_a = connect_client(&credentials_a);
    send(*client_a, request("keep-alive"));
    EXPECT_EQ(read_response(*client_a), uuid_a);
    auto client_b = connect_client(&credentials_b);
    send(*client_b, request());
    EXPECT_EQ(read_response(*client_b), uuid_b);
    send(*client_a, request());
    EXPECT_EQ(read_response(*client_a), uuid_a);
    send(*client_a, request("keep-alive") + request("close"));
    EXPECT_EQ(read_response(*client_a), uuid_a);
    EXPECT_EQ(read_response(*client_a), uuid_a);
    ASSERT_EQ(launches.size(), 5);
    EXPECT_EQ(verified_handshakes.load(), 2);
    for (const auto index : {0, 2, 3, 4}) {
      EXPECT_EQ(launches[index]->client_uuid, uuid_a);
      EXPECT_EQ(launches[index]->client_cert, credentials_a.x509);
      EXPECT_EQ(launches[index]->client_name, "client A");
    }
    EXPECT_NE(requests[0], requests[2]);
    EXPECT_NE(requests[2], requests[3]);
    EXPECT_NE(requests[3], requests[4]);
    EXPECT_EQ(server_t::connection_identity({}), std::nullopt);
    EXPECT_TRUE(run_until([&]() {
      return !server_t::connection_identity(requests.back());
    }));
  }

  TEST_F(HttpsClientIdentityTest, DisabledPairingRejectsNextKeepaliveRequest) {
    start_server();
    auto client = connect_client(&credentials_a);
    send(*client, request());
    EXPECT_EQ(read_response(*client), uuid_a);
    ASSERT_TRUE(nvhttp::set_client_enabled(uuid_a, false));
    send(*client, request());
    EXPECT_NE(read_response(*client).find("status_code=\"401\""), std::string::npos);
    EXPECT_EQ(launches.size(), 1);
  }

  TEST_F(HttpsClientIdentityTest, UnpairedIdentityRejectsRequestAfterHandshake) {
    start_server();
    auto client = connect_client(&credentials_a);
    ASSERT_TRUE(nvhttp::unpair_client(uuid_a));
    send(*client, request());
    EXPECT_NE(read_response(*client).find("status_code=\"401\""), std::string::npos);
    EXPECT_TRUE(launches.empty());
  }

  TEST_F(HttpsClientIdentityTest, RepairedCertificateCannotReplaceOriginalConnectionUuid) {
    start_server();
    auto original = connect_client(&credentials_a);
    const auto repaired_uuid = nvhttp::test_support::add_client("repaired A", credentials_a.x509, true);
    ASSERT_NE(repaired_uuid, uuid_a);
    send(*original, request());
    EXPECT_NE(read_response(*original).find("status_code=\"401\""), std::string::npos);
    EXPECT_TRUE(launches.empty());
    auto repaired = connect_client(&credentials_a);
    send(*repaired, request());
    EXPECT_EQ(read_response(*repaired), repaired_uuid);
    ASSERT_EQ(launches.size(), 1);
    EXPECT_EQ(launches[0]->client_uuid, repaired_uuid);
    EXPECT_EQ(launches[0]->client_name, "repaired A");
  }

  TEST_F(HttpsClientIdentityTest, DuplicateCertificateRejectsRequestAfterHandshake) {
    start_server();
    auto client = connect_client(&credentials_a);
    ASSERT_TRUE(nvhttp::test_support::duplicate_client(uuid_a));
    send(*client, request());
    EXPECT_NE(read_response(*client).find("status_code=\"401\""), std::string::npos);
    EXPECT_TRUE(launches.empty());
  }

  TEST_F(HttpsClientIdentityTest, ExpiredPairedCertificateRetainsCompatibility) {
    const auto expired = test_utils::certificates::expire_credentials(credentials_a);
    const auto expired_uuid = nvhttp::test_support::add_client("expired A", expired.x509, true);
    ASSERT_FALSE(expired_uuid.empty());
    start_server();
    auto client = connect_client(&expired);
    send(*client, request());
    EXPECT_EQ(read_response(*client), expired_uuid);
    ASSERT_EQ(launches.size(), 1);
    EXPECT_EQ(launches[0]->client_cert, expired.x509);
  }

  TEST_F(HttpsClientIdentityTest, MissingSocketIdentityNeverDispatchesHandler) {
    start_server(false);
    auto client = connect_client(&credentials_a);
    send(*client, request());
    EXPECT_NE(read_response(*client).find("status_code=\"401\""), std::string::npos);
    EXPECT_TRUE(launches.empty());
    EXPECT_EQ(verified_handshakes.load(), 0);
  }

  TEST_F(HttpsClientIdentityTest, UnknownCertificateNeverDispatchesHandler) {
    start_server();
    const auto unknown = crypto::gen_creds("Sunshine HTTPS unknown client", 2048);
    auto client = connect_client(&unknown);
    EXPECT_NE(read_response(*client).find("status_code=\"401\""), std::string::npos);
    EXPECT_TRUE(launches.empty());
  }

  TEST_F(HttpsClientIdentityTest, MissingPeerCertificateFailsHandshakeWithoutDispatch) {
    start_server();
    connect_client(nullptr);
    EXPECT_EQ(verified_handshakes.load(), 0);
    EXPECT_EQ(error_callbacks.load(), 1);
    EXPECT_TRUE(launches.empty());
  }

  TEST_F(HttpsClientIdentityTest, ZeroGrantProductionRoutesDenyActionsAndHideRestrictedMetadata) {
    start_server(true, true);
    const auto exchange = [&](const std::string &path) {
      auto client = connect_client(&credentials_a);
      send(*client, "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
      return read_response(*client);
    };
    const auto metadata = exchange("/serverinfo?uniqueid=forged-owner");
    EXPECT_TRUE(metadata.contains("<Permission>0</Permission>"));
    EXPECT_TRUE(metadata.contains("<currentgame>0</currentgame>"));
    EXPECT_TRUE(metadata.contains("SUNSHINE_SERVER_FREE"));
    EXPECT_FALSE(metadata.contains("ServerCommand"));
    EXPECT_FALSE(metadata.contains("InputOnly"));
    const std::string stream_args = "rikey=00112233445566778899aabbccddeeff&rikeyid=1&appid=7&localAudioPlayMode=0";
    for (const auto &path : {"/applist", "/appasset?appid=7", "/cancel"}) {
      const auto body = exchange(path);
      EXPECT_TRUE(body.contains("status_code=\"403\"")) << path;
      EXPECT_FALSE(body.contains("AppTitle"));
    }
    EXPECT_TRUE(exchange("/launch?" + stream_args).contains("status_code=\"403\""));
    EXPECT_TRUE(exchange("/resume?" + stream_args).contains("status_code=\"403\""));
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, permissions::list));
    const auto listed = exchange("/applist");
    EXPECT_TRUE(listed.contains("status_code=\"200\""));
    EXPECT_TRUE(exchange("/resume?" + stream_args).contains("status_code=\"403\""));
    EXPECT_TRUE(exchange("/cancel").contains("status_code=\"403\""));
  }

  TEST_F(HttpsClientIdentityTest, ExplicitLaunchStopInvalidatesEveryAdmissionWhileDeniedStopHasNoEffect) {
    start_server(true, true);
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, permissions::launch));
    ASSERT_TRUE(nvhttp::update_client(uuid_b, std::nullopt, permissions::view));
    const auto admission = [&](const auto &uuid, const auto &credentials) {
      const auto identity = nvhttp::current_client(uuid, credentials.x509);
      const SimpleWeb::CaseInsensitiveMultimap args {{"rikey", "00112233445566778899aabbccddeeff"}, {"rikeyid", "1"}};
      return nvhttp::test_support::make_launch_session(*identity, args);
    };
    auto first = admission(uuid_a, credentials_a);
    ASSERT_EQ(rtsp_stream::prepare_launch(first, []() {
                return 200;
              }),
              200);
    ASSERT_TRUE(rtsp_stream::test_support::claim_launch(first));
    rtsp_stream::launch_session_clear(first->id);
    auto second = admission(uuid_b, credentials_b);
    ASSERT_EQ(rtsp_stream::prepare_launch(second, []() {
                return 200;
              }),
              200);
    const auto stop = [&](const auto &credentials) {
      auto client = connect_client(&credentials);
      send(*client, "GET /cancel HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
      return read_response(*client);
    };
    EXPECT_TRUE(stop(credentials_b).contains("status_code=\"403\""));
    EXPECT_TRUE(rtsp_stream::test_support::token_authorized(first));
    EXPECT_TRUE(rtsp_stream::test_support::token_authorized(second));
    EXPECT_TRUE(stop(credentials_a).contains("<cancel>1</cancel>"));
    EXPECT_FALSE(rtsp_stream::test_support::token_authorized(first));
    EXPECT_FALSE(rtsp_stream::test_support::token_authorized(second));
  }

}  // namespace
