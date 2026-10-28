/**
 * @file tests/unit/test_nvhttp_https.cpp
 * @brief Verify HTTPS callbacks retain the guard protecting server lifetime.
 */

// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// local includes
#include <src/app_catalog.h>
#include <src/config.h>
#include <src/crypto.h>
#include <src/httpcommon.h>
#include <src/nvhttp.h>
#include <src/process.h>
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
      stop_server();
      nvhttp::test_support::reset_client_state();
      config::nvhttp.file_state = original_state_file;
      config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
      std::error_code cleanup_error;
      std::filesystem::remove(state_file, cleanup_error);
      std::filesystem::remove(certificate_file, cleanup_error);
      std::filesystem::remove(private_key_file, cleanup_error);
      BaseTest::TearDown();
    }

    /** @brief Close client peers and quiesce every server worker before restoring fixture-owned state. */
    void stop_server() {
      if (server_cleanup_complete) {
        return;
      }
      for (const auto &client : clients) {
        boost::system::error_code close_error;
        client->socket.lowest_layer().close(close_error);
      }
      if (server) {
        // The fixture owns this event loop; SimpleWeb::stop only stops an internal loop.
        const auto server_io = server->io_service;
        if (server_io) {
          server_io->stop();
        }
        if (before_server_join && (server_thread.joinable() || !extra_servers.empty())) {
          before_server_join();
        }
        for (auto &thread : extra_servers) {
          if (thread.joinable()) {
            thread.join();
          }
        }
        extra_servers.clear();
        if (server_thread.joinable()) {
          server_thread.join();
        }
        handler_scope->stop();
        server->stop();
        if (server_io) {
          server_io->restart();
          server_io->poll();
        }
        server.reset();
      }
      clients.clear();
      rtsp_stream::stop_application({});
      if (observing_catalog) {
        http::test_support::set_state_security_hook({});
        http::test_support::set_state_update_hook({});
        observing_catalog = false;
      }
      if (restore_isolated_catalog) {
        auto restore = std::move(restore_isolated_catalog);
        restore_isolated_catalog = {};
        restore();
      }
      server_cleanup_complete = true;
    }

    /** @brief Own a valid empty catalog and process context until the server has quiesced. */
    void isolate_catalog() {
      const auto original_apps_file = config::stream.file_apps;
      const auto apps_file = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "https_zero_grant_apps.json";
      auto original_proc = std::make_shared<proc::proc_t>(std::move(proc::proc));
      restore_isolated_catalog = [original_proc = std::move(original_proc), original_apps_file, apps_file]() {
        proc::proc = std::move(*original_proc);
        config::stream.file_apps = original_apps_file;
        std::error_code error;
        std::filesystem::remove(apps_file, error);
      };
      proc::proc = proc::proc_t {};
      config::stream.file_apps = apps_file.string();
      std::ofstream {apps_file} << R"({"env":{},"apps":[]})";
    }

    /** @brief Observe catalog progress without changing transaction inputs or waiting inside a callback. */
    void observe_catalog_completion() {
      applist_entered = false;
      applist_returned = false;
      catalog_stages = 0;
      catalog_failure_reported = false;
      observing_catalog = true;
      http::test_support::set_state_security_hook([this](auto stage, const auto &) {
        catalog_stages.fetch_or(stage == http::test_support::state_security_stage_e::restore ? 1U : 2U);
      });
      http::test_support::set_state_update_hook([this]() {
        catalog_stages.fetch_or(4U);
      });
    }

    /** @brief Return finite, credential-free stage markers only for the first observed failure. */
    std::string catalog_failure_markers() {
      if (!observing_catalog || catalog_failure_reported) {
        return {};
      }
      catalog_failure_reported = true;
      const auto stages = catalog_stages.load();
      std::ostringstream markers;
      markers << " [catalog completion: loop_returned=" << server_loop_returned.load()
              << " io_stopped=" << server->io_service->stopped()
              << " applist_entered=" << applist_entered.load()
              << " applist_returned=" << applist_returned.load()
              << " security_restore_entered=" << ((stages & 1U) != 0)
              << " security_verify_entered=" << ((stages & 2U) != 0)
              << " pre_write_entered=" << ((stages & 4U) != 0) << ']';
      return markers.str();
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
        auto &applist = server->resource["^/applist$"]["GET"];
        applist = [this, handler = std::move(applist)](auto response, auto request) {
          applist_entered = true;
          auto returned = util::fail_guard([this]() {
            applist_returned = true;
          });
          handler(std::move(response), std::move(request));
        };
        auto completed = std::make_shared<std::promise<void>>();
        launch_handler_returned = completed->get_future().share();
        auto observed = std::make_shared<std::atomic_bool>(false);
        auto &launch = server->resource["^/launch$"]["GET"];
        launch = [handler = std::move(launch), completed, observed](auto response, auto request) {
          auto returned = util::fail_guard([completed, observed]() {
            if (!observed->exchange(true)) {
              completed->set_value();
            }
          });
          handler(std::move(response), std::move(request));
        };
      }
      server->start([this](unsigned short assigned_port) {
        port = assigned_port;
      });
      server->io_service->poll();
      ASSERT_NE(port, 0);
      server_thread = std::jthread([this]() {
        server->io_service->run();
        server_loop_returned = true;
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
      }))
        << catalog_failure_markers();
      if (credentials) {
        EXPECT_FALSE(completion->error) << completion->error.message() << catalog_failure_markers();
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
      }))
        << catalog_failure_markers();
      ASSERT_FALSE(completion->error) << completion->error.message() << catalog_failure_markers();
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
          })) {
        ADD_FAILURE() << "HTTP header read timed out: callback_completed=" << completion->done
                      << " native_error=" << static_cast<bool>(completion->error) << catalog_failure_markers();
        return {};
      }
      if (completion->error) {
        ADD_FAILURE() << "HTTP header read failed: " << completion->error.message() << catalog_failure_markers();
        return {};
      }
      std::istream response {&client.incoming};
      std::string line;
      std::getline(response, line);
      EXPECT_EQ(line, "HTTP/1.1 200 OK\r") << catalog_failure_markers();
      std::size_t content_length = 0;
      bool has_content_length = false;
      while (std::getline(response, line) && line != "\r") {
        if (line.starts_with("Content-Length: ")) {
          content_length = std::stoul(line.substr(16));
          has_content_length = true;
        }
      }
      EXPECT_TRUE(has_content_length) << catalog_failure_markers();
      if (client.incoming.size() < content_length) {
        completion = std::make_shared<completion_t>();
        boost::asio::async_read(client.socket, client.incoming, boost::asio::transfer_exactly(content_length - client.incoming.size()), [completion](const auto &error, std::size_t) {
          completion->error = error;
          completion->done = true;
        });
        if (!run_until([&]() {
              return completion->done;
            })) {
          ADD_FAILURE() << "HTTP body read timed out: callback_completed=" << completion->done
                        << " native_error=" << static_cast<bool>(completion->error) << catalog_failure_markers();
          return {};
        }
        if (completion->error) {
          ADD_FAILURE() << "HTTP body read failed: " << completion->error.message() << catalog_failure_markers();
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
    std::vector<std::jthread> extra_servers;  ///< Additional workers share the same peer-close and join boundary.
    std::function<void()> before_server_join;  ///< Nonblocking event observer for deterministic shutdown coverage.
    std::function<void()> restore_isolated_catalog;  ///< Owns original catalog/process state through server quiescence.
    bool server_cleanup_complete = false;  ///< Prevents explicit regression shutdown from restoring state twice.
    bool observing_catalog = false;  ///< Hooks remain installed until all server callbacks have quiesced.
    bool catalog_failure_reported = false;  ///< Finite stage output is emitted only on the first observed failure.
    std::atomic_bool server_loop_returned {false};  ///< Distinguishes executor exit from a blocked handler.
    std::atomic_bool applist_entered {false};  ///< Actual protected callback entry, including authorization.
    std::atomic_bool applist_returned {false};  ///< Actual callback exit, including exception unwinding.
    std::shared_future<void> launch_handler_returned;  ///< First actual protected launch callback exit, regardless of result.
    std::atomic_uint catalog_stages {0};  ///< Read-only restore, verify and pre-write observations, bits 0-2.
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

  TEST_F(HttpsClientIdentityTest, IsolatedCatalogRestorationWaitsForCallbackJoinAndStoppedScope) {
    const auto original_apps_file = config::stream.file_apps;
    const auto original_snapshot = proc::proc.published_snapshot();
    isolate_catalog();
    const auto isolated_snapshot = proc::proc.published_snapshot();
    const auto isolated_file = config::stream.file_apps;
    start_server();

    auto entered = std::make_shared<std::promise<void>>();
    auto entered_event = entered->get_future();
    auto release = std::make_shared<std::promise<void>>();
    const auto released = release->get_future().share();

    struct cleanup_observation_t {
      std::atomic_bool scope_acquired {false};
      std::atomic_bool callback_returned {false};
      bool after_callback = false;
      bool after_join = false;
      bool after_scope_stop = false;
      bool after_server_release = false;
    };

    auto observed = std::make_shared<cleanup_observation_t>();
    auto release_on_exit = util::fail_guard([release]() {
      release->set_value();
    });
    boost::asio::post(*server->io_service, [scope = handler_scope, entered, released, observed]() {
      auto lock = scope->continue_lock();
      observed->scope_acquired = static_cast<bool>(lock);
      entered->set_value();
      released.wait();
      observed->callback_returned = true;
    });
    ASSERT_EQ(entered_event.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(observed->scope_acquired);

    auto restore = std::move(restore_isolated_catalog);
    restore_isolated_catalog = [this, restore = std::move(restore), observed]() {
      observed->after_callback = observed->callback_returned.load();
      observed->after_join = !server_thread.joinable();
      observed->after_scope_stop = !handler_scope->continue_lock();
      observed->after_server_release = !server;
      restore();
    };
    auto joining = std::make_shared<std::promise<void>>();
    auto join_event = joining->get_future();
    before_server_join = [joining]() {
      joining->set_value();
    };
    auto stopping = std::async(std::launch::async, [this]() {
      stop_server();
    });
    auto release_before_join = util::fail_guard([release]() {
      release->set_value();
    });
    release_on_exit.disable();
    EXPECT_EQ(join_event.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(stopping.wait_for(0ms), std::future_status::timeout);
    EXPECT_EQ(config::stream.file_apps, isolated_file);
    EXPECT_EQ(proc::proc.published_snapshot(), isolated_snapshot);
    EXPECT_TRUE(std::filesystem::exists(isolated_file));
    release->set_value();
    release_before_join.disable();
    EXPECT_EQ(stopping.wait_for(2s), std::future_status::ready);
    stopping.get();
    EXPECT_TRUE(observed->after_callback);
    EXPECT_TRUE(observed->after_join);
    EXPECT_TRUE(observed->after_scope_stop);
    EXPECT_TRUE(observed->after_server_release);
    EXPECT_EQ(config::stream.file_apps, original_apps_file);
    EXPECT_EQ(proc::proc.published_snapshot(), original_snapshot);
    EXPECT_FALSE(std::filesystem::exists(isolated_file));
  }

  TEST_F(HttpsClientIdentityTest, UnreadClientClosesBeforeAdditionalServerWorkersJoin) {
    start_server();
    for (int i = 0; i < 2; ++i) {
      extra_servers.emplace_back([io = server->io_service]() {
        io->run();
      });
    }
    auto client = connect_client(&credentials_a);
    send(*client, request("close"));

    auto entered = std::make_shared<std::promise<void>>();
    auto entered_event = entered->get_future();
    auto release = std::make_shared<std::promise<void>>();
    const auto released = release->get_future().share();
    auto returned = std::make_shared<std::atomic_bool>(false);
    auto scope_acquired = std::make_shared<std::atomic_bool>(false);
    auto release_on_exit = util::fail_guard([release]() {
      release->set_value();
    });
    boost::asio::post(*server->io_service, [scope = handler_scope, entered, released, returned, scope_acquired]() {
      auto lock = scope->continue_lock();
      *scope_acquired = static_cast<bool>(lock);
      entered->set_value();
      released.wait();
      *returned = true;
    });
    ASSERT_EQ(entered_event.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(scope_acquired->load());

    auto joining = std::make_shared<std::promise<void>>();
    auto join_event = joining->get_future();
    auto peer_closed = std::make_shared<std::atomic_bool>(false);
    before_server_join = [client, joining, peer_closed]() {
      *peer_closed = !client->socket.lowest_layer().is_open();
      joining->set_value();
    };
    auto stopping = std::async(std::launch::async, [this]() {
      stop_server();
    });
    auto release_before_join = util::fail_guard([release]() {
      release->set_value();
    });
    release_on_exit.disable();
    EXPECT_EQ(join_event.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(peer_closed->load());
    EXPECT_FALSE(returned->load());
    EXPECT_EQ(stopping.wait_for(0ms), std::future_status::timeout);
    release->set_value();
    release_before_join.disable();
    EXPECT_EQ(stopping.wait_for(2s), std::future_status::ready);
    stopping.get();
    EXPECT_TRUE(returned->load());
    EXPECT_FALSE(client->socket.lowest_layer().is_open());
    EXPECT_TRUE(extra_servers.empty());
    EXPECT_FALSE(server_thread.joinable());
    EXPECT_FALSE(handler_scope->continue_lock());
    EXPECT_FALSE(server);
  }

  TEST_F(HttpsClientIdentityTest, ZeroGrantProductionRoutesDenyActionsAndHideRestrictedMetadata) {
    isolate_catalog();
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
    observe_catalog_completion();
    const auto listed = exchange("/applist");
    EXPECT_TRUE(listed.contains("status_code=\"200\"")) << listed << catalog_failure_markers();
    EXPECT_FALSE(listed.contains("AppTitle"));
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

  /**
   * @brief Exercise real selector admission without media workers using a captured empty-command app.
   */
  class HttpsApplicationIdentityTest: public HttpsClientIdentityTest {
  protected:
    proc::proc_t previous_proc;
    std::string previous_file;
    proc::catalog::json initial;
    std::pair<int, std::string> selected;
    int source_preparations = 0;
    int source_rollbacks = 0;
    int encoder_probes = 0;
    std::atomic_bool existing_viewer {false};

    void SetUp() override {
      HttpsClientIdentityTest::SetUp();
      previous_proc = std::move(proc::proc);
      proc::proc = proc::proc_t {};
      previous_file = config::stream.file_apps;
      config::stream.file_apps = (std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "https_application_catalog.json").string();
      std::ofstream {config::stream.file_apps} << R"({"env":{},"apps":[{"name":"Active"},{"name":"Other"}]})";
      const auto loaded = proc::read_catalog(config::stream.file_apps);
      ASSERT_EQ(loaded.status, 200) << loaded.body;
      initial = proc::catalog::parse(loaded.body);
      const auto &app = initial["apps"][0];
      selected = {std::stoi(app["id"].get<std::string>()), app["uuid"].get<std::string>()};
      auto session = std::make_shared<rtsp_stream::launch_session_t>();
      session->surround_info = 2;
      ASSERT_EQ(proc::proc.execute(selected.first, session), 0);
      nvhttp::test_support::set_source_preparation_hooks(
        [&](const auto &) {
          ++source_preparations;
        },
        [&]() {
          ++encoder_probes;
          return 0;
        },
        [&]() {
          ++source_rollbacks;
        },
        [&]() {
          return existing_viewer ? 1 : 0;
        }
      );
      start_server(true, true);
    }

    void TearDown() override {
      rtsp_stream::stop_application({});
      nvhttp::test_support::set_source_preparation_hooks();
      proc::proc.terminate();
      proc::proc = std::move(previous_proc);
      std::filesystem::remove(config::stream.file_apps);
      config::stream.file_apps = previous_file;
      HttpsClientIdentityTest::TearDown();
    }

    std::string exchange(const std::string &path) {
      auto client = connect_client(&credentials_a);
      send(*client, "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
      return read_response(*client);
    }

    std::string stream_path(const std::string &route, const std::string &selectors) const {
      return route + "?rikey=00112233445566778899aabbccddeeff&rikeyid=1&corever=1&localAudioPlayMode=0" + selectors;
    }
  };

  TEST_F(HttpsApplicationIdentityTest, ListAndRestrictedCurrentIdentityUseTheSameDurablePair) {
    const auto hidden = exchange("/serverinfo");
    EXPECT_TRUE(hidden.contains("<currentgame>0</currentgame>"));
    EXPECT_FALSE(hidden.contains(selected.second));
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, permissions::view));
    const auto visible = exchange("/serverinfo");
    EXPECT_TRUE(visible.contains("<currentgameuuid>" + selected.second + "</currentgameuuid>"));
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, permissions::list));
    const auto listed = exchange("/applist");
    EXPECT_TRUE(listed.contains("<UUID>" + selected.second + "</UUID>"));
    EXPECT_TRUE(listed.contains("<ID>" + std::to_string(selected.first) + "</ID>"));
    EXPECT_TRUE(listed.contains("<IDX>0</IDX>"));
  }

  TEST_F(HttpsApplicationIdentityTest, ViewOnlySameAppSelectorsPrepareFreshSourceWithoutExecutingEditedCommands) {
    auto edit = initial["apps"][0];
    edit["cmd"] = "nonexistent_identity_regression_executable";
    ASSERT_EQ(proc::save_catalog_app(config::stream.file_apps, initial["revision"], edit.dump()).status, 200);
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, permissions::view));
    for (const auto &selectors : {
           "&appid=" + std::to_string(selected.first),
           "&appid=" + std::to_string(selected.first) + "&appuuid=",
           "&appuuid=" + selected.second,
           "&appid=-1&appuuid=" + selected.second
         }) {
      const auto response = exchange(stream_path("/launch", selectors));
      EXPECT_TRUE(response.contains("status_code=\"200\"")) << selectors;
      const auto token = rtsp_stream::test_support::pending_launch();
      ASSERT_TRUE(token);
      EXPECT_EQ(token->appid, selected.first);
      EXPECT_EQ(token->app_uuid, selected.second);
      EXPECT_EQ(token->client_uuid, uuid_a);
      EXPECT_EQ(token->client_cert, credentials_a.x509);
      EXPECT_FALSE(token->joining);  // No media source exists; the source hook must still run.
      EXPECT_GT(source_preparations, 0);
      EXPECT_EQ(encoder_probes, source_preparations);
      EXPECT_TRUE(rtsp_stream::test_support::token_authorized(token));
      EXPECT_EQ(proc::proc.active_identity(), selected);
      EXPECT_EQ(proc::proc.get_last_run_app_name(), "Active");
      rtsp_stream::stop_application({});
    }
    const auto prepared_before_denial = source_preparations;
    const auto other = initial["apps"][1]["uuid"].get<std::string>();
    EXPECT_TRUE(exchange(stream_path("/launch", "&appid=-1&appuuid=" + other)).contains("status_code=\"403\""));
    EXPECT_FALSE(rtsp_stream::test_support::pending_launch());
    EXPECT_EQ(source_preparations, prepared_before_denial);
    EXPECT_EQ(encoder_probes, prepared_before_denial);
  }

  TEST_F(HttpsApplicationIdentityTest, FakeExistingViewerJoinPreservesSourceAndApplication) {
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, permissions::view));
    existing_viewer = true;
    EXPECT_TRUE(exchange(stream_path("/launch", "&appid=-1&appuuid=" + selected.second)).contains("status_code=\"200\""));
    const auto token = rtsp_stream::test_support::pending_launch();
    ASSERT_TRUE(token);
    EXPECT_TRUE(token->joining);
    EXPECT_EQ(token->appid, selected.first);
    EXPECT_EQ(token->app_uuid, selected.second);
    EXPECT_EQ(source_preparations, 0);
    EXPECT_EQ(encoder_probes, 0);
    EXPECT_EQ(proc::proc.active_identity(), selected);
    rtsp_stream::stop_application({});
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, permissions::launch));
    const auto other = initial["apps"][1]["uuid"].get<std::string>();
    EXPECT_TRUE(exchange(stream_path("/launch", "&appid=-1&appuuid=" + other)).contains("status_code=\"503\""));
    EXPECT_FALSE(rtsp_stream::test_support::pending_launch());
    EXPECT_EQ(source_preparations, 0);
    EXPECT_EQ(encoder_probes, 0);
  }

  TEST_F(HttpsApplicationIdentityTest, RejectedSelectorMatrixLeavesNoUsableAdmission) {
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, permissions::launch));
    const auto prepared_before_denial = source_preparations;
    for (const auto &route : {"/launch", "/resume"}) {
      for (const auto &selectors : {
             "&appid=-1",
             "&appid=-1&appuuid=",
             "&appid=-2",
             "&appid=2147483648",
             "&appid=7&appid=8",
             "&appuuid=invalid",
             "&appuuid=&appuuid=",
             "&appid=-1&appuuid=33333333-3333-3333-3333-333333333333"
           }) {
        const auto response = exchange(stream_path(route, selectors));
        EXPECT_FALSE(response.contains("status_code=\"200\"")) << selectors;
        EXPECT_FALSE(rtsp_stream::test_support::pending_launch());
        EXPECT_EQ(proc::proc.active_identity(), selected);
      }
      const auto other = initial["apps"][1]["uuid"].get<std::string>();
      EXPECT_TRUE(exchange(stream_path(route, "&appid=" + std::to_string(selected.first) + "&appuuid=" + other)).contains("status_code=\"400\""));
      EXPECT_TRUE(exchange(stream_path(route, "&appid=0&appuuid=" + selected.second)).contains("status_code=\"400\""));
      EXPECT_FALSE(rtsp_stream::test_support::pending_launch());
    }
    EXPECT_EQ(source_preparations, prepared_before_denial);
    EXPECT_EQ(encoder_probes, prepared_before_denial);
  }

  TEST_F(HttpsApplicationIdentityTest, DeletedActiveUuidResumesUntilStoppedAndRevocationInvalidatesIt) {
    ASSERT_EQ(proc::delete_catalog_app(config::stream.file_apps, initial["revision"], selected.second).status, 200);
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, permissions::view));
    for (const auto &selectors : {std::string {}, "&appid=-1&appuuid=" + selected.second}) {
      EXPECT_TRUE(exchange(stream_path("/resume", selectors)).contains("status_code=\"200\""));
      const auto token = rtsp_stream::test_support::pending_launch();
      ASSERT_TRUE(token);
      EXPECT_EQ(token->app_uuid, selected.second);
      rtsp_stream::stop_application({});
    }
    EXPECT_TRUE(exchange(stream_path("/resume", "&appid=-1&appuuid=" + selected.second)).contains("status_code=\"200\""));
    const auto token = rtsp_stream::test_support::pending_launch();
    ASSERT_TRUE(token);
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, 0U));
    EXPECT_FALSE(rtsp_stream::test_support::token_authorized(token));
    EXPECT_FALSE(rtsp_stream::test_support::pending_launch());
    proc::proc.terminate();
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, permissions::launch));
    EXPECT_TRUE(exchange(stream_path("/launch", "&appid=-1&appuuid=" + selected.second)).contains("status_code=\"404\""));
    EXPECT_FALSE(rtsp_stream::test_support::pending_launch());
  }

  TEST_F(HttpsApplicationIdentityTest, RepairedClientCannotUseUuidSentinelOnAnOriginalTlsConnection) {
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, permissions::launch));
    auto original = connect_client(&credentials_a);
    ASSERT_TRUE(nvhttp::unpair_client(uuid_a));
    const auto repaired = nvhttp::test_support::add_client("repaired selector fixture", credentials_a.x509, true);
    ASSERT_TRUE(nvhttp::update_client(repaired, std::nullopt, permissions::launch));
    const auto path = stream_path("/launch", "&appid=-1&appuuid=" + selected.second + "&uniqueid=" + repaired);
    send(*original, "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    EXPECT_TRUE(read_response(*original).contains("status_code=\"401\""));
    EXPECT_FALSE(rtsp_stream::test_support::pending_launch());
    EXPECT_EQ(source_preparations, 0);
    EXPECT_EQ(proc::proc.active_identity(), selected);
  }

  /** @brief Inert transaction backend used with the actual managed HTTPS callbacks and process registry. */
  class managed_http_backend_t: public rtsp_stream::source_backend_t {
  public:
    std::shared_future<void> gate;
    std::promise<void> began;
    std::atomic_int begins {0};

    bool begin(const display_policy::source_request_t &request, std::uint64_t generation, std::uint64_t epoch) override {
      if (++begins == 1) {
        began.set_value();
      }
      if (gate.valid()) {
        gate.wait_for(5s);
      }
      auto source = std::make_shared<rtsp_stream::capture_source_t>();
      source->kind = request.virtual_display ? rtsp_stream::capture_source_t::kind_e::owned_virtual : rtsp_stream::capture_source_t::kind_e::native;
      source->generation = generation;
      source->epoch = epoch;
      source->selector = "managed-http-selector";
      source->device_id = "managed-http-device";
      source->monitor_uuid = request.virtual_display ? request.monitor_uuid : "";
      source->windows_output = rtsp_stream::windows_output_t {{1, 0}, 9, "managed-http-monitor-path"};
      source->width = request.mode.resolution.width;
      source->height = request.mode.resolution.height;
      source->rate_numerator = request.mode.refresh_rate.numerator;
      source->rate_denominator = request.mode.refresh_rate.denominator;
      source->observed_hdr = request.requested_hdr;
      std::lock_guard lock {mutex};
      state = {generation, rtsp_stream::source_backend_status_t::phase_e::ready, false, false, false, source};
      return true;
    }

    bool validate_retained(const rtsp_stream::capture_source_t &source, std::uint64_t epoch) override {
      std::lock_guard lock {mutex};
      if (!state.source || *state.source != source) {
        return false;
      }
      auto next = std::make_shared<rtsp_stream::capture_source_t>(source);
      next->epoch = epoch;
      state.source = next;
      return true;
    }

    rtsp_stream::source_backend_status_t status() const override {
      std::lock_guard lock {mutex};
      return state;
    }

    int probe(const rtsp_stream::capture_source_t &, std::shared_ptr<rtsp_stream::capture_pin_t>) override {
      return 200;
    }

    bool restore(std::uint64_t, std::chrono::milliseconds) override {
      std::lock_guard lock {mutex};
      state.phase = rtsp_stream::source_backend_status_t::phase_e::removal_ready;
      return true;
    }

    bool cleanup_and_retire(std::uint64_t) override {
      std::lock_guard lock {mutex};
      state.source_released = true;
      state.phase = rtsp_stream::source_backend_status_t::phase_e::restored;
      return true;
    }

    bool complete(std::uint64_t) override {
      return true;
    }

  private:
    mutable std::mutex mutex;
    rtsp_stream::source_backend_status_t state;
  };

  /** @brief Real TLS routes, real process state and file-gated commands; no display or capture hardware. */
  class ManagedHttpsProcessTest: public HttpsClientIdentityTest {
  protected:
    void SetUp() override {
      HttpsClientIdentityTest::SetUp();
      previous_proc = std::move(proc::proc);
      proc::proc = proc::proc_t {};
      previous_file = config::stream.file_apps;
      directory = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "p3c_managed_https";
      previous_timeout = config::stream.ping_timeout;
      std::filesystem::create_directories(directory);
      gate = directory / "release";
      entered = directory / "entered";
      std::error_code error;
      std::filesystem::remove(gate, error);
      std::filesystem::remove(entered, error);
      config::stream.file_apps = (directory / "apps.json").string();
      auto tree = proc::catalog::parse(R"({"env":{"P3C_CATALOG_ENV":"captured"},"apps":[{"name":"A","auto-detach":false,"wait-all":false,"exit-timeout":0},{"name":"B","auto-detach":false,"wait-all":false,"exit-timeout":0}]})");
#ifdef _WIN32
      command = "cmd.exe /D /C ping -n 60 127.0.0.1";
      undo_command = "cmd.exe /D /C exit 0";
      script = directory / "wait.cmd";
      std::ofstream {script} << "@echo off\r\n> \"%P3C_ENTERED%\" echo entered\r\n:wait\r\nif exist \"%P3C_GATE%\" exit /b 0\r\nping -n 2 127.0.0.1 >nul\r\ngoto wait\r\n";
      wait_command = "cmd.exe /D /C \"" + script.string() + "\"";
#else
      command = "/bin/sleep 60";
      undo_command = "/bin/true";
      script = directory / "wait.sh";
      std::ofstream {script} << "printf entered > \"$P3C_ENTERED\"\nwhile [ ! -e \"$P3C_GATE\" ]; do sleep 0.01; done\n";
      wait_command = "/bin/sh \"" + script.string() + "\"";
#endif
      tree["env"]["P3C_ENTERED"] = entered.string();
      tree["env"]["P3C_GATE"] = gate.string();
      for (auto &app : tree["apps"]) {
        app["cmd"] = command;
        app["output"] = "null";
      }
      tree["apps"][0]["prep-cmd"] = proc::catalog::json::array({{{"do", ""}, {"undo", undo_command}}});
      std::ofstream {config::stream.file_apps} << tree.dump();
      const auto loaded = proc::read_catalog(config::stream.file_apps);
      ASSERT_EQ(loaded.status, 200);
      initial = proc::catalog::parse(loaded.body);
      app_a = {std::stoi(initial["apps"][0]["id"].get<std::string>()), initial["apps"][0]["uuid"].get<std::string>()};
      app_b = {std::stoi(initial["apps"][1]["id"].get<std::string>()), initial["apps"][1]["uuid"].get<std::string>()};
      auto launch = std::make_shared<rtsp_stream::launch_session_t>();
      launch->surround_info = 2;
      ASSERT_EQ(proc::proc.execute(app_a.first, launch, &a_instance), 0);
      ASSERT_TRUE(proc::proc.lifetime_snapshot().tracked);
      proc::test_support::set_undo_environment_hook([count = undo_count, records = undo_records, mutex = undo_mutex](const auto &environment) {
        ++*count;
        std::lock_guard lock {*mutex};
        records->emplace_back(environment.at("SUNSHINE_APP_ID").to_string(), environment.at("P3C_CATALOG_ENV").to_string());
      });
      backend = std::make_shared<managed_http_backend_t>();
      ASSERT_TRUE(rtsp_stream::set_source_backend([backend = backend]() {
        return backend;
      }));
      owner = rtsp_stream::test_support::start_owner();
      nvhttp::test_support::set_source_preparation_hooks({}, {}, {}, {}, []() {
        return std::optional<bool> {true};
      });
      ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, permissions::view | permissions::launch | permissions::list));
      ASSERT_TRUE(nvhttp::update_client(uuid_b, std::nullopt, permissions::view | permissions::launch | permissions::list));
      start_server(true, true);
      for (int i = 0; i < 4; ++i) {
        extra_servers.emplace_back([io = server->io_service]() {
          io->run();
        });
      }
    }

    void TearDown() override {
      std::ofstream {gate} << "release";
      rtsp_stream::stop_process_application();
      owner.reset();
      EXPECT_TRUE(rtsp_stream::set_source_backend({}));
      nvhttp::test_support::set_source_preparation_hooks();
      proc::test_support::set_execute_entry_hook({});
      proc::test_support::set_undo_environment_hook({});
      proc::test_support::set_legacy_restore_hook({});
      stop_server();
      proc::proc.terminate();
      proc::proc = std::move(previous_proc);
      std::error_code error;
      for (const auto &file : {gate, entered, script, std::filesystem::path {config::stream.file_apps}}) {
        std::filesystem::remove(file, error);
      }
      config::stream.file_apps = previous_file;
      config::stream.ping_timeout = previous_timeout;
      HttpsClientIdentityTest::TearDown();
    }

    std::shared_ptr<client_t> begin_request(const std::string &path, bool second = false) {
      auto client = connect_client(second ? &credentials_b : &credentials_a);
      send(*client, "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
      return client;
    }

    std::string launch_path(const std::string &uuid) const {
      return "/launch?rikey=00112233445566778899aabbccddeeff&rikeyid=1&corever=1&localAudioPlayMode=0&mode=1920x1080x60&appid=-1&appuuid=" + uuid;
    }

    bool drain_source() {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (std::chrono::steady_clock::now() < deadline) {
        rtsp_stream::test_support::poll_source();
        if (!rtsp_stream::source_snapshot().managed) {
          return true;
        }
        std::this_thread::yield();
      }
      return false;
    }

    bool wait_for_file(const std::filesystem::path &file) {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(file)) {
          return true;
        }
        std::this_thread::yield();
      }
      return false;
    }

    proc::proc_t previous_proc;
    std::string previous_file, command, undo_command, wait_command;
    decltype(config::stream.ping_timeout) previous_timeout;
    std::filesystem::path directory, gate, entered, script;
    proc::catalog::json initial;
    std::pair<int, std::string> app_a, app_b;
    std::uint64_t a_instance {};
    std::shared_ptr<std::atomic_int> undo_count = std::make_shared<std::atomic_int>(0);
    std::shared_ptr<std::mutex> undo_mutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<std::pair<std::string, std::string>>> undo_records = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
    std::shared_ptr<managed_http_backend_t> backend;
    std::unique_ptr<platf::deinit_t> owner;
  };

  TEST_F(ManagedHttpsProcessTest, RemovedSelectedAppBeforeExecutePreservesOriginalIncarnationAndUndo) {
    std::promise<void> release;
    backend->gate = release.get_future().share();
    auto launching = begin_request(launch_path(app_b.second));
    ASSERT_EQ(backend->began.get_future().wait_for(2s), std::future_status::ready);
    ASSERT_EQ(proc::delete_catalog_app(config::stream.file_apps, initial["revision"], app_b.second).status, 200);
    release.set_value();
    EXPECT_TRUE(read_response(*launching).contains("status_code=\"404\""));
    ASSERT_TRUE(drain_source());
    EXPECT_EQ(proc::proc.lifetime_snapshot().instance, a_instance);
    EXPECT_TRUE(proc::proc.lifetime_snapshot().tracked);
    EXPECT_EQ(undo_count->load(), 0);
  }

  TEST_F(ManagedHttpsProcessTest, LaunchRightRetainsItsViewingInclusionDuringDeferredExecution) {
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, permissions::launch));
    ASSERT_TRUE(launch_handler_returned.valid());
    ASSERT_GT(config::stream.ping_timeout.count(), 0);
    auto launching = begin_request(launch_path(app_b.second));
    ASSERT_EQ(launch_handler_returned.wait_for(config::stream.ping_timeout), std::future_status::ready)
      << "Real launch preparation did not return within the configured admission limit";
    EXPECT_TRUE(read_response(*launching).contains("status_code=\"200\""));
    const auto current = proc::proc.lifetime_snapshot();
    EXPECT_TRUE(current.tracked);
    EXPECT_EQ(current.app_uuid, app_b.second);
    EXPECT_NE(current.instance, a_instance);
    EXPECT_EQ(undo_count->load(), 1);
  }

  TEST_F(ManagedHttpsProcessTest, CancellationAtActualProcessLockBoundaryCannotClaimOriginalApplication) {
    auto reached = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::promise<void>>();
    const auto proceed = release->get_future().share();
    proc::test_support::set_execute_entry_hook([reached, proceed]() {
      reached->set_value();
      proceed.wait_for(3s);
    });
    auto launching = begin_request(launch_path(app_b.second));
    ASSERT_EQ(reached->get_future().wait_for(2s), std::future_status::ready);
    {
      const auto state = proc::proc.lock_state();
      release->set_value();
      ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, 0U));
      EXPECT_TRUE(read_response(*launching).contains("status_code=\"403\""));
    }
    ASSERT_TRUE(drain_source());
    proc::test_support::set_execute_entry_hook({});
    EXPECT_EQ(proc::proc.lifetime_snapshot().instance, a_instance);
    EXPECT_TRUE(proc::proc.lifetime_snapshot().tracked);
    EXPECT_EQ(undo_count->load(), 0);
  }

  TEST_F(ManagedHttpsProcessTest, NullOrThrowingBackendFactoryDoesNotStealLegacyRestoration) {
    auto restores = std::make_shared<std::atomic_int>(0);
    proc::test_support::set_legacy_restore_hook([restores]() {
      ++*restores;
    });
    for (const bool throwing : {false, true}) {
      ASSERT_TRUE(rtsp_stream::set_source_backend([throwing]() -> std::shared_ptr<rtsp_stream::source_backend_t> {
        if (throwing) {
          throw std::runtime_error {"allocation failed"};
        }
        return {};
      }));
      auto launching = begin_request(launch_path(app_a.second));
      EXPECT_TRUE(read_response(*launching).contains("status_code=\"503\""));
      EXPECT_FALSE(rtsp_stream::source_snapshot().managed);
      EXPECT_EQ(proc::proc.lifetime_snapshot().instance, a_instance);
      EXPECT_TRUE(proc::proc.lifetime_snapshot().tracked);
    }
    rtsp_stream::process_operation([]() {
      proc::proc.terminate();
    }).get();
    EXPECT_EQ(restores->load(), 1);
    EXPECT_EQ(undo_count->load(), 1);
  }

  TEST_F(ManagedHttpsProcessTest, SkippedBeginRestoresExactExistingProcessClaimAndLegacyCleanup) {
    auto reached = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::promise<void>>();
    const auto proceed = release->get_future().share();
    auto restores = std::make_shared<std::atomic_int>(0);
    proc::test_support::set_legacy_restore_hook([restores]() {
      ++*restores;
    });
    rtsp_stream::test_support::set_owner_hook([reached, proceed](std::string_view event) {
      if (event == "source_before_entry") {
        reached->set_value();
        proceed.wait_for(3s);
      }
    });
    auto launching = begin_request(launch_path(app_a.second));
    ASSERT_EQ(reached->get_future().wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(nvhttp::update_client(uuid_a, std::nullopt, 0U));
    EXPECT_TRUE(read_response(*launching).contains("status_code=\"403\""));
    release->set_value();
    ASSERT_TRUE(drain_source());
    rtsp_stream::test_support::set_owner_hook({});
    EXPECT_EQ(backend->begins, 0);
    EXPECT_EQ(proc::proc.lifetime_snapshot().instance, a_instance);
    EXPECT_TRUE(proc::proc.lifetime_snapshot().tracked);
    rtsp_stream::process_operation([]() {
      proc::proc.terminate();
    }).get();
    EXPECT_EQ(restores->load(), 1);
  }

  TEST_F(ManagedHttpsProcessTest, RealConfiguredCommandCannotBlockSnapshotsCatalogQueueRevocationOrShutdownDispatch) {
    auto edit = initial["apps"][1];
    edit["prep-cmd"] = proc::catalog::json::array({{{"do", wait_command}, {"undo", undo_command}}});
    const auto changed = proc::save_catalog_app(config::stream.file_apps, initial["revision"], edit.dump());
    ASSERT_EQ(changed.status, 200);
    const auto revision = proc::catalog::parse(changed.body)["revision"].get<std::string>();
    config::stream.ping_timeout = 1500ms;
    auto expired = std::make_shared<std::promise<void>>();
    rtsp_stream::test_support::set_owner_hook([expired](std::string_view event) {
      if (event == "launch_expired") {
        expired->set_value();
      }
    });
    auto launching = begin_request(launch_path(app_b.second));
    ASSERT_TRUE(wait_for_file(entered));
    EXPECT_TRUE(proc::proc.published_snapshot()->operation_in_progress);
    auto info = begin_request("/serverinfo", true);
    EXPECT_TRUE(read_response(*info).contains("status_code=\"200\""));
    auto apps = begin_request("/applist", true);
    EXPECT_TRUE(read_response(*apps).contains("<UUID>" + app_b.second + "</UUID>"));
    edit["name"] = "B changed while executing";
    edit["cmd"] = "nonexistent_catalog_command_must_not_replace_captured_execution";
    auto mutation = proc::save_catalog_app_async(config::stream.file_apps, revision, edit.dump());
    auto catalog_read = proc::read_catalog_async(config::stream.file_apps);
    EXPECT_EQ(mutation.wait_for(0ms), std::future_status::timeout);
    auto marker = std::make_shared<std::promise<void>>();
    rtsp_stream::authorize_operation(uuid_b, credentials_b.x509, permissions::view, [marker]() {
      marker->set_value();
    });
    EXPECT_EQ(marker->get_future().wait_for(1s), std::future_status::ready);
    ASSERT_EQ(expired->get_future().wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(read_response(*launching).contains("status_code=\"503\""));
    auto stop_queued = std::make_shared<std::promise<void>>();
    auto shutdown_entered = std::make_shared<std::promise<void>>();
    auto observed_stop = std::make_shared<std::atomic_bool>(false);
    rtsp_stream::test_support::set_owner_hook([stop_queued, shutdown_entered, observed_stop](std::string_view event) {
      if (event == "application_stop_queued" && !observed_stop->exchange(true)) {
        stop_queued->set_value();
      }
      if (event == "stop_begin") {
        shutdown_entered->set_value();
      }
    });
    auto stopping_client = begin_request("/cancel", true);
    ASSERT_EQ(stop_queued->get_future().wait_for(1s), std::future_status::ready);
    auto persisted = std::make_shared<std::promise<void>>();
    auto persisted_ready = persisted->get_future();
    auto proceed = std::make_shared<std::promise<void>>();
    const auto may_reconcile = proceed->get_future().share();
    bool reconciliation_released = false;
    const auto release_reconciliation = [&]() {
      if (!reconciliation_released) {
        proceed->set_value();
        reconciliation_released = true;
      }
    };
    bool command_released = false;
    const auto release_command = [&]() {
      if (!command_released) {
        std::ofstream {gate} << "release";
        command_released = true;
      }
    };
    // Declared before the future so hook reset cannot race an in-flight callback.
    auto reset_reconcile_hook = util::fail_guard([]() {
      nvhttp::test_support::set_client_pre_reconcile_hook();
    });
    nvhttp::test_support::set_client_pre_reconcile_hook([persisted, may_reconcile]() {
      persisted->set_value();
      may_reconcile.wait();
    });
    auto revoked = std::async(std::launch::async, [uuid = uuid_a]() {
      return nvhttp::update_client(uuid, std::nullopt, 0U);
    });
    // Release both gates before joining the future on any assertion or exception path.
    auto release_before_revocation_join = util::fail_guard([&]() {
      release_reconciliation();
      release_command();
    });
    ASSERT_EQ(persisted_ready.wait_for(3s), std::future_status::ready)
      << "Durable authorization preparation did not finish before the owner check";
    ASSERT_FALSE(std::filesystem::exists(gate));
    release_reconciliation();
    ASSERT_EQ(revoked.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(revoked.get());
    EXPECT_FALSE(std::filesystem::exists(gate));
    nvhttp::test_support::set_client_pre_reconcile_hook();
    reset_reconcile_hook.disable();
    auto shutdown = std::async(std::launch::async, []() {
      rtsp_stream::test_support::stop_owner();
    });
    auto release_before_shutdown_join = util::fail_guard([&]() {
      release_command();
    });
    ASSERT_EQ(shutdown_entered->get_future().wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(std::filesystem::exists(gate));
    EXPECT_EQ(shutdown.wait_for(0ms), std::future_status::timeout);
    release_command();
    release_before_shutdown_join.disable();
    release_before_revocation_join.disable();
    ASSERT_EQ(mutation.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(mutation.get().status, 200);
    ASSERT_EQ(catalog_read.wait_for(3s), std::future_status::ready);
    const auto final_catalog = catalog_read.get();
    EXPECT_EQ(final_catalog.status, 200);
    EXPECT_TRUE(final_catalog.body.contains("nonexistent_catalog_command_must_not_replace_captured_execution"));
    EXPECT_TRUE(read_response(*stopping_client).contains("status_code=\"200\""));
    ASSERT_EQ(shutdown.wait_for(3s), std::future_status::ready);
    shutdown.get();
    EXPECT_EQ(undo_count->load(), 2);
    EXPECT_EQ(proc::proc.get_env().at("P3C_CATALOG_ENV").to_string(), "captured");
    {
      std::lock_guard lock {*undo_mutex};
      EXPECT_NE(std::find(undo_records->begin(), undo_records->end(), std::pair {std::to_string(app_b.first), std::string {"captured"}}), undo_records->end());
    }
    EXPECT_EQ(proc::proc.running(), 0);
    EXPECT_FALSE(rtsp_stream::source_snapshot().managed);
  }

}  // namespace
