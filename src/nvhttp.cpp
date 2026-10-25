/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <utility>

// lib includes
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "app_catalog.h"
#include "config.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "string_hash.h"
#include "system_tray.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"

using namespace std::literals;

namespace SimpleWeb {
  Server<nvhttp::SunshineHTTPS>::Server(const std::string &certification_file, const std::string &private_key_file):
      ServerBase<nvhttp::SunshineHTTPS>::ServerBase(443) {
    // Disabling TLS 1.0 and 1.1 (see RFC 8996)
    context.set_options(boost::asio::ssl::context::no_tlsv1);
    context.set_options(boost::asio::ssl::context::no_tlsv1_1);
    context.use_certificate_chain_file(certification_file);
    context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
  }

  std::optional<nvhttp::verified_client_t> Server<nvhttp::SunshineHTTPS>::connection_identity(const std::shared_ptr<Request> &request) {
    if (request) {
      if (const auto connection = request->connection.lock()) {
        return connection->socket->verified_client;
      }
    }
    return std::nullopt;
  }

  void Server<nvhttp::SunshineHTTPS>::after_bind() {
    if (verify) {
      context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
      context.set_verify_callback([](int, boost::asio::ssl::verify_context &) {
        // To respond with an error message, a connection must be established
        return 1;
      });
    }
  }

  void Server<nvhttp::SunshineHTTPS>::handle_handshake(const std::shared_ptr<Session> &session, const error_code &ec) {
    if (!ec) {
      if (verify) {
        session->connection->socket->verified_client = verify(session->connection->socket->native_handle());
        if (!session->connection->socket->verified_client) {
          this->write(session, on_verify_failed);
          return;
        }
      }
      this->read(session);
    } else if (this->on_error) {
      this->on_error(session->request, ec);
    }
  }

  void Server<nvhttp::SunshineHTTPS>::accept() {
    auto connection = create_connection(*io_service, context);

    acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const error_code &ec) {
      if (auto lock = connection->handler_runner->continue_lock(); lock) {
        if (ec != error::operation_aborted) {
          this->accept();
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          error_code option_error;
          session->connection->socket->lowest_layer().set_option(option, option_error);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const error_code &ec) {
            session->connection->cancel_timeout();
            if (auto lock = session->connection->handler_runner->continue_lock(); lock) {
              this->handle_handshake(session, ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      }
    });
  }
}  // namespace SimpleWeb

namespace nvhttp {
#ifdef SUNSHINE_TESTS
  namespace {
    std::function<void(const rtsp_stream::launch_session_t &)> source_configure_hook;  ///< Fake-only display setup; never establishes retained-source lifetime.
    std::function<int()> source_probe_hook;  ///< Fake-only encoder qualification result.
    std::function<void()> source_rollback_hook;  ///< Matching fake cleanup without touching hardware.
    std::function<int()> source_sessions_hook;  ///< Fake-only existing-viewer count for HTTP component fixtures.
  }  // namespace
#endif

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  crypto::cert_chain_t cert_chain;  ///< Enabled paired-client certificates accepted by Sunshine's GameStream HTTPS server.

  /**
   * @brief Get the mutex protecting paired-client authorization state.
   *
   * @return Mutex serializing paired-client state and certificate authorization changes.
   */
  std::mutex &client_auth_mutex() {
    static std::mutex mutex;
    return mutex;
  }

  /**
   * @brief HTTPS server type used for GameStream endpoints requiring TLS.
   */
  using SunshineHTTPSServer = SimpleWeb::Server<SunshineHTTPS>;
  using https_server_t = SunshineHTTPSServer;
  /**
   * @brief Plain HTTP server type used for GameStream endpoints without TLS.
   */
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  /**
   * @brief Internal HTTPS credential paths for the configuration server.
   */
  struct conf_intern_t {
    std::string servercert;  ///< Server certificate PEM string.
    std::string pkey;  ///< Private key PEM string or path.
  };

  conf_intern_t conf_intern;  ///< TLS credential paths loaded from Sunshine's runtime configuration.

  /**
   * @brief Certificate entry associated with a client name and UUID.
   */
  struct named_cert_t {
    std::string name;  ///< Human-readable name for this item.
    std::string uuid;  ///< Persistent Moonlight client UUID associated with the certificate.
    std::string cert;  ///< Certificate PEM string or path.
    bool enabled = true;  ///< Whether this persisted client entry may connect.
    permissions::mask_t permissions = 0;  ///< Explicit implemented rights; pairing alone grants nothing.
    std::uint64_t revision = 0;  ///< Monotonic in-process authorization revision.
  };

  /**
   * @brief Persisted pairing data for one Moonlight client.
   */
  struct client_t {
    std::vector<named_cert_t> named_devices;  ///< Persisted Moonlight clients allowed to pair or reconnect.
  };

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t, util::transparent_string_hash, std::equal_to<>> map_id_sess;  ///< Pairing sessions keyed by temporary unique ID.

  /**
   * @brief Get the mutex protecting pairing-session state.
   *
   * @return Mutex serializing pairing-session storage and lifecycle transitions.
   */
  std::mutex &map_id_sess_mutex() {
    static std::mutex mutex;
    return mutex;
  }

  std::mutex client_mutation_mutex;  ///< Serializes persistence, publication and completed revocation barriers.
  std::uint64_t authorization_revision = 0;  ///< Next revision, guarded by client_auth_mutex().

  client_t client_root;  ///< In-memory representation of the paired-client database.
  std::atomic<uint32_t> session_id_counter;  ///< Monotonic counter used to allocate GameStream session IDs.

  /**
   * @brief Case-insensitive map used for HTTP headers and query parameters.
   */
  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  /**
   * @brief Shared HTTPS response object passed to GameStream handlers.
   */
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  /**
   * @brief Shared HTTPS request object received by GameStream handlers.
   */
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
  /**
   * @brief Shared HTTP response object passed to redirect and discovery handlers.
   */
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  /**
   * @brief Shared HTTP request object received by redirect and discovery handlers.
   */
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  /**
   * @brief Certificate operations supported by the pairing API.
   */
  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

  /**
   * @brief Read a named query argument from the HTTP request map.
   *
   * @param args Parsed query-string argument map.
   * @param name Query parameter name to read.
   * @param default_value Value returned when the parameter is absent.
   * @return Query parameter value, default value, or an empty string.
   */
  std::string get_arg(const args_t &args, const char *name, const char *default_value = nullptr) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  /**
   * @brief Persist the current state to its backing store.
   */
  bool save_state(const client_t &client = client_root) {
    return http::update_state_file(config::nvhttp.file_state, [&](auto &root) {
      root.put("root.uniqueid", http::unique_id);
      root.get_child("root").erase("devices");
      pt::ptree devices;
      for (const auto &device : client.named_devices) {
        pt::ptree node;
        node.put("name", device.name);
        node.put("cert", device.cert);
        node.put("uuid", device.uuid);
        node.put("enabled", device.enabled);
        node.put("permissions", device.permissions);
        devices.push_back(std::make_pair(""s, node));
      }
      root.put_child("root.named_devices", devices);
    });
  }

  /**
   * @brief Convert a PEM certificate to Sunshine's canonical OpenSSL serialization.
   *
   * @param cert_pem PEM-encoded certificate to canonicalize.
   * @return Canonical PEM text, or an empty string when the certificate is invalid.
   */
  std::string canonical_certificate_pem(const std::string_view cert_pem) {
    auto certificate = crypto::x509(cert_pem);
    return certificate ? crypto::pem(certificate) : std::string {};
  }

  /**
   * @brief Rebuild the GameStream trust stores from enabled paired-client records.
   *
   * @note The caller must hold `client_auth_mutex()`.
   */
  void rebuild_client_cert_chain() {
    cert_chain.clear();
    for (const auto &named_cert : client_root.named_devices) {
      if (!named_cert.enabled) {
        continue;
      }

      auto certificate = crypto::x509(named_cert.cert);
      if (!certificate) {
        BOOST_LOG(warning) << "Ignoring invalid paired-client certificate"sv;
        continue;
      }

      cert_chain.add(std::move(certificate));
    }
  }

  /**
   * @brief Check whether a certificate exactly matches an enabled paired-client record.
   *
   * @param certificate Parsed client certificate to compare by canonical X.509 identity.
   * @return `true` only when exactly one matching paired-client record is enabled.
   * @note The caller must hold `client_auth_mutex()`.
   */
  bool is_client_enabled(const X509 *certificate) {
    bool matched = false;
    for (const auto &named_cert : client_root.named_devices) {
      if (auto stored_certificate = crypto::x509(named_cert.cert); !stored_certificate || X509_cmp(stored_certificate.get(), certificate) != 0) {
        continue;
      }

      if (matched || !named_cert.enabled) {
        return false;
      }
      matched = true;
    }
    return matched;
  }

  /**
   * @brief Verify a client certificate against the exact enabled paired identity.
   *
   * @param certificate Parsed client certificate presented during the TLS handshake.
   * @return `nullptr` when authorized, otherwise a non-sensitive error string.
   * @note The caller must hold `client_auth_mutex()`.
   */
  const char *verify_client_certificate(X509 *certificate) {
    if (const auto verification_error = cert_chain.verify(certificate); verification_error) {
      return verification_error;
    }
    return is_client_enabled(certificate) ? nullptr : "Client certificate identity is not enabled";
  }

  /**
   * @brief Resolve one exact certificate to its enabled persistent pairing.
   * @param certificate Client certificate to authenticate.
   * @param original_uuid Original handshake UUID, or an empty value during initial authentication.
   * @return Authorized paired identity, or no value for missing, disabled, duplicate or stale records.
   * @note The caller must hold client_auth_mutex().
   */
  std::optional<verified_client_t> resolve_client_identity(X509 *certificate, const std::string_view original_uuid = {}) {
    if (const auto verification_error = verify_client_certificate(certificate); verification_error) {
      BOOST_LOG(warning) << "SSL Verification error :: "sv << verification_error;
      return std::nullopt;
    }

    for (const auto &client : client_root.named_devices) {
      auto stored_certificate = crypto::x509(client.cert);
      if (stored_certificate && X509_cmp(stored_certificate.get(), certificate) == 0) {
        if (client.uuid.empty() || (!original_uuid.empty() && client.uuid != original_uuid)) {
          return std::nullopt;
        }
        return verified_client_t {client.uuid, crypto::pem(stored_certificate), client.name, client.permissions, client.revision};
      }
    }
    return std::nullopt;
  }

  std::optional<verified_client_t> current_client(const std::string_view uuid, const std::string_view cert) {
    if (uuid.empty() || cert.empty()) {
      return std::nullopt;
    }
    auto certificate = crypto::x509(cert);
    if (!certificate) {
      return std::nullopt;
    }
    std::lock_guard lock {client_auth_mutex()};
    return resolve_client_identity(certificate.get(), uuid);
  }

  /**
   * @brief Revalidate the request's original TLS identity against current paired-client state.
   * @param request Request whose connection supplied the original identity.
   * @return Current paired identity when its original UUID and exact certificate remain enabled.
   */
  std::optional<verified_client_t> request_client_identity(const req_https_t &request) {
    const auto original = https_server_t::connection_identity(request);
    if (!original || original->uuid.empty()) {
      return std::nullopt;
    }
    auto certificate = crypto::x509(original->cert);
    if (!certificate) {
      return std::nullopt;
    }
    std::lock_guard lock {client_auth_mutex()};
    return resolve_client_identity(certificate.get(), original->uuid);
  }

  /**
   * @brief Return the protocol's unauthorized response without dispatching a protected handler.
   * @param response Response to populate and close.
   * @param request Request rejected by client authorization.
   */
  void unauthorized(resp_https_t response, req_https_t request) {
    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 401);
    tree.put("root.<xmlattr>.query", request->path);
    tree.put("root.<xmlattr>.status_message", "The client is not authorized. Certificate verification failed.");
    std::ostringstream data;
    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  /**
   * @brief Gate each HTTPS dispatch on the connection's original, currently enabled pairing.
   * @param handler Handler receiving a copy of the currently authorized paired identity.
   * @return Route handler that rejects missing and stale authentication before any protected operation.
   */
  template<class Handler>
  auto authenticated(Handler handler) {
    return [handler = std::move(handler)](resp_https_t response, req_https_t request) {
      const auto identity = request_client_identity(request);
      if (!identity) {
        unauthorized(std::move(response), std::move(request));
        return;
      }
      handler(std::move(response), std::move(request), *identity);
    };
  }

  /**
   * @brief Require a current effective action right at the RTSP serialization point.
   * @param required Effective right required by this route.
   * @param handler Protected handler executed only while authorized.
   * @return Authenticated route with host-side action enforcement.
   */
  template<class Handler>
  auto permitted(permissions::mask_t required, Handler handler) {
    return authenticated([required, handler = std::move(handler)](auto response, auto request, const auto &identity) {
      if (!rtsp_stream::authorize_operation(identity.uuid, identity.cert, required, [&]() {
            handler(response, request, identity);
          })) {
        pt::ptree tree;
        tree.put("root.<xmlattr>.status_code", 403);
        tree.put("root.<xmlattr>.status_message", "Permission denied");
        std::ostringstream data;
        pt::write_xml(data, tree);
        response->write(data.str());
        response->close_connection_after_response = true;
      }
    });
  }

  /**
   * @brief Authenticate the TLS peer and retain its original persistent pairing identity.
   * @param ssl Connection whose peer certificate was supplied during the guarded handshake.
   * @return Exact enabled paired identity, or no value when authorization fails.
   */
  std::optional<verified_client_t> verify_connection(SSL *ssl) {
    crypto::x509_t certificate {
#if OPENSSL_VERSION_MAJOR >= 3
      SSL_get1_peer_certificate(ssl)
#else
      SSL_get_peer_certificate(ssl)
#endif
    };
    if (!certificate) {
      BOOST_LOG(info) << "unknown -- denied"sv;
      return std::nullopt;
    }

    std::lock_guard lock {client_auth_mutex()};
    const auto identity = resolve_client_identity(certificate.get());
    std::array<char, 256> subject_name;
    X509_NAME_oneline(X509_get_subject_name(certificate.get()), subject_name.data(), subject_name.size());
    BOOST_LOG(debug) << subject_name.data() << " -- "sv << (identity ? "verified"sv : "denied"sv);
    return identity;
  }

  /**
   * @brief Load state from its backing store.
   */
  void load_state() {
    if (!fs::exists(config::nvhttp.file_state)) {
      BOOST_LOG(info) << "File "sv << config::nvhttp.file_state << " doesn't exist"sv;
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }

    pt::ptree tree;
    try {
      pt::read_json(config::nvhttp.file_state, tree);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();

      return;
    }

    auto unique_id_p = tree.get_optional<std::string>("root.uniqueid");
    if (!unique_id_p) {
      // This file doesn't contain moonlight credentials
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }
    http::unique_id = std::move(*unique_id_p);

    auto root = tree.get_child("root");
    client_t client;

    // Import from old format
    if (root.get_child_optional("devices")) {
      auto device_nodes = root.get_child("devices");
      for (auto &[_, device_node] : device_nodes) {
        auto uniqID = device_node.get<std::string>("uniqueid");

        if (device_node.count("certs")) {
          for (auto &[_, el] : device_node.get_child("certs")) {
            named_cert_t named_cert;
            named_cert.name = ""s;
            named_cert.cert = el.get_value<std::string>();
            named_cert.uuid = uuid_util::uuid_t::generate().string();
            client.named_devices.emplace_back(named_cert);
          }
        }
      }
    }

    if (root.count("named_devices")) {
      for (auto &[_, el] : root.get_child("named_devices")) {
        named_cert_t named_cert;
        named_cert.name = el.get_child("name").get_value<std::string>();
        named_cert.cert = el.get_child("cert").get_value<std::string>();
        named_cert.uuid = el.get_child("uuid").get_value<std::string>();
        named_cert.enabled = el.get<bool>("enabled", true);
        const auto grant = permissions::parse(el.get<std::string>("permissions", "0"));
        named_cert.permissions = grant.value_or(0);
        if (!grant) {
          BOOST_LOG(warning) << "Invalid paired-client permission mask requires an explicit administrator grant"sv;
        }
        client.named_devices.emplace_back(named_cert);
      }
    }

    for (auto &named_cert : client.named_devices) {
      auto canonical_certificate = canonical_certificate_pem(named_cert.cert);
      if (!canonical_certificate.empty()) {
        named_cert.cert = std::move(canonical_certificate);
      }
    }

    std::lock_guard lock {client_auth_mutex()};
    for (auto &device : client.named_devices) {
      device.revision = ++authorization_revision;
    }
    client_root = std::move(client);
    rebuild_client_cert_chain();
  }

  /**
   * @brief Add authorized client data.
   *
   * A completed pairing replaces all records with the same exact X.509 identity so legacy duplicate records cannot make the newly paired client fail authorization.
   *
   * @param name Human-readable name to assign.
   * @param cert Certificate data or object used by the operation.
   * @return Persistent UUID for the added client, or an empty string when the certificate is invalid.
   */
  std::string add_authorized_client(const std::string &name, std::string &&cert) {
    auto certificate = crypto::x509(cert);
    if (!certificate) {
      return {};
    }

    named_cert_t named_cert;
    named_cert.name = name;
    named_cert.cert = crypto::pem(certificate);
    named_cert.uuid = uuid_util::uuid_t::generate().string();
    const auto uuid = named_cert.uuid;

    std::lock_guard mutation {client_mutation_mutex};
    std::vector<named_cert_t> replaced;
    {
      std::lock_guard lock {client_auth_mutex()};
      auto candidate = client_root;
      std::erase_if(candidate.named_devices, [&](const named_cert_t &existing) {
        auto stored = crypto::x509(existing.cert);
        if (stored && X509_cmp(stored.get(), certificate.get()) == 0) {
          replaced.push_back(existing);
          return true;
        }
        return false;
      });
      named_cert.revision = ++authorization_revision;
      candidate.named_devices.emplace_back(std::move(named_cert));
      if (!config::sunshine.flags[config::flag::FRESH_STATE] && !save_state(candidate)) {
        return {};
      }
      client_root = std::move(candidate);
      rebuild_client_cert_chain();
    }
    for (const auto &old : replaced) {
      rtsp_stream::reconcile_client(old.uuid, old.cert, true);
    }
    return uuid;
  }

  /**
   * @brief Create launch session.
   *
   * @param host_audio Host audio.
   * @param args Arguments forwarded to the callable or parser.
   * @param identity Verified paired identity associated with this request.
   * @return Constructed launch session object.
   */
  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(bool host_audio, const args_t &args, const verified_client_t &identity) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;

    auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
    std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

    launch_session->host_audio = host_audio;
    auto mode = std::stringstream(get_arg(args, "mode", "0x0x0"));
    // Split mode by the char "x", to populate width/height/fps
    int x = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (x == 0) {
        launch_session->width = atoi(segment.c_str());
      }
      if (x == 1) {
        launch_session->height = atoi(segment.c_str());
      }
      if (x == 2) {
        launch_session->fps = atoi(segment.c_str());
      }
      x++;
    }
    launch_session->unique_id = (get_arg(args, "uniqueid", "unknown"));
    launch_session->appid = 0;  // Resolved only inside authorized serialized preparation.
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = (int) util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->continuous_audio = util::from_view(get_arg(args, "continuousAudio", "0"));
    launch_session->gcmap = (int) util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));

    // Encrypted RTSP is enabled with client reported corever >= 1
    auto corever = util::from_view(get_arg(args, "corever", "0"));
    if (corever >= 1) {
      launch_session->rtsp_cipher = crypto::cipher::gcm_t {
        launch_session->gcm_key,
        false
      };
      launch_session->rtsp_iv_counter = 0;
    }
    launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;
    launch_session->client_uuid = identity.uuid;
    launch_session->client_cert = identity.cert;
    launch_session->client_name = identity.name;
    launch_session->authorization_revision = identity.revision;

    // Generate the unique identifiers for this connection that we will send later during RTSP handshake
    std::array<unsigned char, 8> raw_payload;
    RAND_bytes(raw_payload.data(), raw_payload.size());
    launch_session->av_ping_payload = util::hex_vec(raw_payload);
    RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

    launch_session->iv.resize(16);
    uint32_t prepend_iv = util::endian::big<uint32_t>((int) util::from_view(get_arg(args, "rikeyid")));
    auto prepend_iv_p = (uint8_t *) &prepend_iv;
    std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
    return launch_session;
  }

  /**
   * @brief Write a completed response to the client waiting for PIN approval.
   *
   * @param sess Pairing session that owns the waiting response.
   * @param tree XML response body to write.
   * @return `true` when a live response was available.
   */
  bool write_pairing_response(pair_session_t &sess, const pt::ptree &tree) {
    std::ostringstream data;
    pt::write_xml(data, tree);

    auto &response = sess.async_insert_pin.response;
    if (response.has_left() && response.left()) {
      response.left()->close_connection_after_response = true;
      response.left()->write(data.str());
    } else if (response.has_right() && response.right()) {
      response.right()->close_connection_after_response = true;
      response.right()->write(data.str());
    } else {
      return false;
    }

    response = std::monostate {};
    return true;
  }

  /**
   * @brief Publish the final result of a pairing handshake.
   *
   * @param sess Pairing session whose waiter should be notified.
   * @param success Whether the client completed the authenticated handshake.
   */
  void complete_pairing(pair_session_t &sess, const bool success) {
    {
      std::lock_guard lock {sess.completion->mutex};
      if (sess.completion->result.has_value()) {
        return;
      }
      sess.completion->result = success;
    }
    sess.completion->condition.notify_all();
  }

  /**
   * @brief Expire stale pairing sessions while the session mutex is held.
   *
   * @param now Monotonic time used to evaluate session deadlines.
   */
  void expire_pair_sessions_unlocked(const std::chrono::steady_clock::time_point now) {
    for (auto it = map_id_sess.begin(); it != map_id_sess.end();) {
      if (it->second.async_insert_pin.expires_at > now) {
        ++it;
        continue;
      }

      pt::ptree tree;
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 408);
      tree.put("root.<xmlattr>.status_message", "Pairing session expired");
      write_pairing_response(it->second, tree);
      complete_pairing(it->second, false);
      it = map_id_sess.erase(it);
    }
  }

  pair_session_insert_e insert_pair_session(pair_session_t sess, std::string &pairing_id) {
    std::scoped_lock lock {map_id_sess_mutex()};
    const auto now = std::chrono::steady_clock::now();
    expire_pair_sessions_unlocked(now);
    pairing_id.clear();

    if (map_id_sess.contains(sess.client.uniqueID)) {
      return pair_session_insert_e::ALREADY_EXISTS;
    }
    if (map_id_sess.size() >= MAX_PENDING_PAIRING_SESSIONS) {
      return pair_session_insert_e::FULL;
    }

    do {
      pairing_id = crypto::rand_alphabet(PAIRING_ID_SIZE, "0123456789abcdef"sv);
    } while (std::ranges::any_of(map_id_sess, [&](const auto &entry) {
      return entry.second.async_insert_pin.id == pairing_id;
    }));

    sess.async_insert_pin.id = pairing_id;
    sess.async_insert_pin.expires_at = now + PAIRING_SESSION_TIMEOUT;
    map_id_sess.try_emplace(sess.client.uniqueID, std::move(sess));
    return pair_session_insert_e::ADDED;
  }

  bool evict_abandoned_pair_session(const std::string &unique_id) {
    std::scoped_lock lock {map_id_sess_mutex()};
    const auto sess_it = map_id_sess.find(unique_id);
    if (sess_it == map_id_sess.end() || sess_it->second.last_phase == PAIR_PHASE::NONE) {
      return false;
    }

    complete_pairing(sess_it->second, false);
    map_id_sess.erase(sess_it);
    return true;
  }

  bool is_valid_pairing_id(const std::string_view pairing_id) {
    return pairing_id.size() == PAIRING_ID_SIZE && std::ranges::all_of(pairing_id, [](const char character) {
             return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
           });
  }

  bool is_valid_pairing_pin(const std::string_view pin) {
    return pin.size() == 4 && std::ranges::all_of(pin, [](const char character) {
             return character >= '0' && character <= '9';
           });
  }

  bool is_valid_pairing_name(const std::string_view name) {
    return !name.empty() && name.size() <= MAX_PAIRING_CLIENT_NAME_SIZE;
  }

  void expire_pair_sessions(const std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(now);
  }

  std::vector<pending_pairing_t> get_pending_pairings() {
    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());

    std::vector<const pair_session_t *> pending_sessions;
    pending_sessions.reserve(map_id_sess.size());
    for (const auto &[_, sess] : map_id_sess) {
      if (sess.last_phase == PAIR_PHASE::NONE) {
        pending_sessions.push_back(&sess);
      }
    }
    std::ranges::sort(pending_sessions, {}, [](const pair_session_t *sess) {
      return sess->async_insert_pin.expires_at;
    });

    std::vector<pending_pairing_t> result;
    result.reserve(pending_sessions.size());
    for (const auto *sess : pending_sessions) {
      result.push_back({
        .id = sess->async_insert_pin.id,
        .name = sess->async_insert_pin.device_name,
        .address = sess->async_insert_pin.address,
      });
    }
    return result;
  }

  bool cancel_pairing(const std::string_view pairing_id) {
    if (!is_valid_pairing_id(pairing_id)) {
      return false;
    }

    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());

    const auto sess_it = std::ranges::find_if(map_id_sess, [&](const auto &entry) {
      return entry.second.last_phase == PAIR_PHASE::NONE && entry.second.async_insert_pin.id == pairing_id;
    });
    if (sess_it == map_id_sess.end()) {
      return false;
    }

    pt::ptree tree;
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", "Pairing request cancelled by operator");
    write_pairing_response(sess_it->second, tree);
    complete_pairing(sess_it->second, false);
    map_id_sess.erase(sess_it);
    return true;
  }

  void remove_session(const pair_session_t &sess) {
    std::scoped_lock lock {map_id_sess_mutex()};
    if (const auto sess_it = map_id_sess.find(sess.client.uniqueID); sess_it != map_id_sess.end()) {
      complete_pairing(sess_it->second, false);
      map_id_sess.erase(sess_it);
    }
  }

  /**
   * @brief Return the GameStream pairing failure response.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param status_msg Status msg.
   */
  void fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string &status_msg) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", status_msg);
    sess.failed = true;
    complete_pairing(sess, false);
  }

  /**
   * @brief Return the server certificate text for pairing responses.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param pin PIN supplied by the client during pairing.
   */
  void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
    if (sess.last_phase != PAIR_PHASE::NONE) {
      fail_pair(sess, tree, "Out of order call to getservercert");
      return;
    }
    sess.last_phase = PAIR_PHASE::GETSERVERCERT;

    if (sess.async_insert_pin.salt.size() < 32) {
      fail_pair(sess, tree, "Salt too short");
      return;
    }

    std::string_view salt_view {sess.async_insert_pin.salt.data(), 32};

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  /**
   * @brief Handle the client-challenge phase of GameStream pairing.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param challenge Client challenge bytes from the pairing request.
   */
  void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
    if (sess.last_phase != PAIR_PHASE::GETSERVERCERT) {
      fail_pair(sess, tree, "Out of order call to clientchallenge");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTCHALLENGE;

    if (!sess.cipher_key) {
      fail_pair(sess, tree, "Cipher key not set");
      return;
    }
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({(char *) decrypted.data(), decrypted.size()});
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  /**
   * @brief Handle the server-challenge response phase of GameStream pairing.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param encrypted_response Encrypted response.
   */
  void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
    if (sess.last_phase != PAIR_PHASE::CLIENTCHALLENGE) {
      fail_pair(sess, tree, "Out of order call to serverchallengeresp");
      return;
    }
    sess.last_phase = PAIR_PHASE::SERVERCHALLENGERESP;

    if (!sess.cipher_key || sess.serversecret.empty()) {
      fail_pair(sess, tree, "Cipher key or serversecret not set");
      return;
    }

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  /**
   * @brief Handle the client pairing-secret phase of GameStream pairing.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param client_pairing_secret Client pairing secret.
   */
  void clientpairingsecret(pair_session_t &sess, pt::ptree &tree, const std::string &client_pairing_secret) {
    if (sess.last_phase != PAIR_PHASE::SERVERCHALLENGERESP) {
      fail_pair(sess, tree, "Out of order call to clientpairingsecret");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTPAIRINGSECRET;

    auto &client = sess.client;

    if (client_pairing_secret.size() <= 16) {
      fail_pair(sess, tree, "Client pairing secret too short");
      return;
    }

    std::string_view secret {client_pairing_secret.data(), 16};
    std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

    auto x509 = crypto::x509(client.cert);
    if (!x509) {
      fail_pair(sess, tree, "Invalid client certificate");
      return;
    }
    auto x509_sign = crypto::signature(x509);

    std::string data;
    data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

    data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
    data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
    data.insert(std::end(data), std::begin(secret), std::end(secret));

    auto hash = crypto::hash(data);

    // if hash not correct, probably MITM
    bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
    auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    bool paired = false;
    if (same_hash && verify) {
      // The client is now successfully paired and will be authorized to connect
      paired = !add_authorized_client(client.name, std::move(client.cert)).empty();
    }

    tree.put("root.paired", paired ? 1 : 0);
    tree.put("root.<xmlattr>.status_code", 200);
    complete_pairing(sess, paired);
  }

  template<class T>
  struct tunnel;

  /**
   * @brief HTTPS tunnel session used for encrypted client requests.
   */
  template<>
  struct tunnel<SunshineHTTPS> {
    static auto constexpr to_string = "HTTPS"sv;  ///< To string.
  };

  /**
   * @brief Plain HTTP server wrapper used for non-TLS endpoints.
   */
  template<>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;  ///< To string.
  };

  /**
   * @brief Write req details to the log.
   *
   * @param request HTTP request data from the client.
   */
  template<class T>
  void print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    BOOST_LOG(debug) << "TUNNEL :: "sv << tunnel<T>::to_string;

    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  /**
   * @brief Return a GameStream HTTP not-found response.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  template<class T>
  void not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());

    *response
      << "HTTP/1.1 404 NOT FOUND\r\n"
      << data.str();

    response->close_connection_after_response = true;
  }

  /**
   * @brief Start a GameStream pairing session for a `getservercert` request.
   *
   * @tparam T HTTP server transport type.
   * @param response HTTP response retained while Web UI PIN approval is pending.
   * @param request HTTP request containing the client pairing parameters.
   * @param args Parsed request query parameters.
   * @param tree XML property tree used for immediate responses.
   * @param unique_id Client-provided pairing-session identifier.
   * @return `true` when the response remains pending for Web UI PIN approval.
   */
  template<class T>
  bool start_pairing_session(
    const std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> &response,
    const std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> &request,
    const args_t &args,
    pt::ptree &tree,
    const std::string &unique_id
  ) {
    const bool pin_stdin = config::sunshine.flags[config::flag::PIN_STDIN];
    auto make_session = [&]() {
      pair_session_t fresh_sess;
      fresh_sess.client.uniqueID = unique_id;
      fresh_sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);
      fresh_sess.async_insert_pin.salt = get_arg(args, "salt");
      fresh_sess.async_insert_pin.device_name = get_arg(args, "devicename");
      fresh_sess.async_insert_pin.address = net::addr_to_normalized_string(request->remote_endpoint().address());
      if (!pin_stdin) {
        fresh_sess.async_insert_pin.response = response;
      }

      BOOST_LOG(debug) << fresh_sess.client.cert;
      return fresh_sess;
    };

    std::string pairing_id;
    auto insert_result = insert_pair_session(make_session(), pairing_id);
    if (insert_result == pair_session_insert_e::ALREADY_EXISTS && evict_abandoned_pair_session(unique_id)) {
      // The previous handshake was abandoned mid-flight; the old session cannot resume,
      // so replace it with a fresh one and retry the insert once.
      insert_result = insert_pair_session(make_session(), pairing_id);
    }
    switch (insert_result) {
      using enum pair_session_insert_e;

      case ALREADY_EXISTS:
        tree.put("root.paired", 0);
        tree.put("root.<xmlattr>.status_code", 409);
        tree.put("root.<xmlattr>.status_message", "A pairing session with this uniqueid already exists");
        return false;
      case FULL:
        tree.put("root.paired", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Too many pending pairing sessions");
        return false;
      case ADDED:
        break;
      default:
        // All enumerators are handled above; any other value cannot occur.
        std::unreachable();
    }

    if (!pin_stdin) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_require_pin();
#endif
      return true;
    }

    std::string pin;
    std::cout << "Please insert pin: "sv;
    std::getline(std::cin, pin);

    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());
    const auto sess_it = map_id_sess.find(unique_id);
    if (sess_it == map_id_sess.end() || sess_it->second.async_insert_pin.id != pairing_id) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 408);
      tree.put("root.<xmlattr>.status_message", "Pairing session expired");
      return false;
    }

    getservercert(sess_it->second, tree, pin);
    if (sess_it->second.failed) {
      map_id_sess.erase(sess_it);
    }
    return false;
  }

  /**
   * @brief Dispatch the top-level GameStream pairing request by phase.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  template<class T>
  void pair(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;

    auto fg = util::fail_guard([&tree, &response]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID {get_arg(args, "uniqueid")};

    const auto phrase_it = args.find("phrase");
    if (phrase_it != std::end(args) && phrase_it->second == "getservercert"sv) {
      if (start_pairing_session<T>(response, request, args, tree, uniqID)) {
        fg.disable();
      }
      return;
    }
    if (phrase_it != std::end(args) && phrase_it->second == "pairchallenge"sv) {
      tree.put("root.paired", 1);
      tree.put("root.<xmlattr>.status_code", 200);
      return;
    }

    std::scoped_lock lock {map_id_sess_mutex()};
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());
    auto sess_it = map_id_sess.find(uniqID);
    if (sess_it == std::end(map_id_sess)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");

      return;
    }

    bool pairing_complete = false;
    if (const auto challenge_it = args.find("clientchallenge"); challenge_it != std::end(args)) {
      auto challenge = util::from_hex_vec(challenge_it->second, true);
      clientchallenge(sess_it->second, tree, challenge);
    } else if (const auto response_it = args.find("serverchallengeresp"); response_it != std::end(args)) {
      auto encrypted_response = util::from_hex_vec(response_it->second, true);
      serverchallengeresp(sess_it->second, tree, encrypted_response);
    } else if (const auto secret_it = args.find("clientpairingsecret"); secret_it != std::end(args)) {
      auto pairingsecret = util::from_hex_vec(secret_it->second, true);
      clientpairingsecret(sess_it->second, tree, pairingsecret);
      pairing_complete = true;
    } else {
      fail_pair(sess_it->second, tree, "Invalid pairing request");
    }

    if (pairing_complete || sess_it->second.failed) {
      map_id_sess.erase(sess_it);
    }
  }

  bool pin(const std::string_view pairing_id, const std::string &pin, std::string name) {
    if (!is_valid_pairing_id(pairing_id) || !is_valid_pairing_pin(pin) || !is_valid_pairing_name(name)) {
      return false;
    }

    std::shared_ptr<pairing_completion_t> completion;
    auto completion_deadline = std::chrono::steady_clock::time_point::min();
    {
      std::scoped_lock lock {map_id_sess_mutex()};
      const auto now = std::chrono::steady_clock::now();
      expire_pair_sessions_unlocked(now);
      const auto sess_it = std::ranges::find_if(map_id_sess, [&](const auto &entry) {
        return entry.second.last_phase == PAIR_PHASE::NONE && entry.second.async_insert_pin.id == pairing_id;
      });
      if (sess_it == map_id_sess.end()) {
        return false;
      }

      auto &sess = sess_it->second;
      pt::ptree tree;
      getservercert(sess, tree, pin);
      if (!sess.failed) {
        sess.client.name = std::move(name);
      }

      if (!write_pairing_response(sess, tree) || sess.failed) {
        complete_pairing(sess, false);
        map_id_sess.erase(sess_it);
        return false;
      }

      completion = sess.completion;
      completion_deadline = std::min(sess.async_insert_pin.expires_at, now + config::stream.ping_timeout);
    }

    {
      std::unique_lock lock {completion->mutex};
      if (completion->condition.wait_until(lock, completion_deadline, [&completion]() {
            return completion->result.has_value();
          })) {
        return *completion->result;
      }
    }

    std::scoped_lock lock {map_id_sess_mutex()};
    if (const auto sess_it = std::ranges::find_if(map_id_sess, [&](const auto &entry) {
          return entry.second.async_insert_pin.id == pairing_id && entry.second.completion == completion;
        });
        sess_it != map_id_sess.end()) {
      complete_pairing(sess_it->second, false);
      map_id_sess.erase(sess_it);
    }
    return false;
  }

  uint32_t get_codec_mode_flags() {
    uint32_t codec_mode_flags = SCM_H264;
    if (video::last_encoder_probe_supported_yuv444_for_codec[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (video::active_hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if ((video::active_hevc_mode == 3 || video::active_hevc_mode == 5) && video::last_encoder_probe_supported_dynamic_range_for_codec[1]) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
    }
    if ((video::active_hevc_mode == 4 || video::active_hevc_mode == 5) && video::last_encoder_probe_supported_dynamic_range_yuv444_for_codec[1]) {
      codec_mode_flags |= SCM_HEVC_REXT10_444;
    }

    if (video::active_av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH8_444;
      }
    }
    if ((video::active_av1_mode == 3 || video::active_av1_mode == 5) && video::last_encoder_probe_supported_dynamic_range_for_codec[2]) {
      codec_mode_flags |= SCM_AV1_MAIN10;
    }
    if ((video::active_av1_mode == 4 || video::active_av1_mode == 5) && video::last_encoder_probe_supported_dynamic_range_yuv444_for_codec[2]) {
      codec_mode_flags |= SCM_AV1_HIGH10_444;
    }
    return codec_mode_flags;
  }

  /**
   * @brief Build the GameStream server-info response.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  template<class T>
  void serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    int pair_status = 0;
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      auto args = request->parse_query_string();
      auto clientID = args.find("uniqueid"s);

      if (clientID != std::end(args)) {
        pair_status = 1;
      }
    }

    auto local_endpoint = request->local_endpoint();

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", video::active_hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    } else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    const uint32_t codec_mode_flags = get_codec_mode_flags();
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);

    if (!config::nvhttp.external_ip.empty()) {
      tree.put("root.ExternalIP", config::nvhttp.external_ip);
    }

    permissions::mask_t grant = 0;
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      if (const auto identity = request_client_identity(request)) {
        grant = identity->permissions;
      }
    }
    tree.put("root.Permission", grant);
    std::pair<int, std::string> current_application {0, ""};
    if (permissions::allows(grant, permissions::view)) {
      rtsp_stream::catalog_operation([&]() {
        current_application = proc::proc.active_identity();
      });
    }
    const auto current_appid = current_application.first;
    tree.put("root.PairStatus", pair_status);
    tree.put("root.currentgame", current_appid);
    tree.put("root.currentgameuuid", current_application.second);
    tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  nlohmann::json get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    std::lock_guard lock {client_auth_mutex()};
    for (const auto &named_cert : client_root.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert.name;
      named_cert_node["uuid"] = named_cert.uuid;
      named_cert_node["enabled"] = named_cert.enabled;
      named_cert_node["permissions"] = named_cert.permissions;
      named_cert_nodes.push_back(named_cert_node);
    }

    return named_cert_nodes;
  }

  /**
   * @brief Build the GameStream application list response.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void applist(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);
    proc::refresh(config::stream.file_apps);

    pt::ptree tree;

    auto g = util::fail_guard([&tree, &response]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    apps.put("<xmlattr>.status_code", 200);

    std::size_t index = 0;
    for (const auto &proc : proc::proc.apps_snapshot()) {
      pt::ptree app;

      app.put("IsHdrSupported"s, video::active_hevc_mode >= 3 ? 1 : 0);
      app.put("AppTitle"s, proc.name);
      app.put("ID", proc.id);
      app.put("UUID", proc.uuid);
      app.put("IDX", index++);

      apps.push_back(std::make_pair("App", app));
    }
  }

  /**
   * @brief Prepare an authorized launch or join before exposing an RTSP admission.
   * @param resuming Whether the request resumes the currently selected application.
   * @param host_audio Host audio preference retained across resume requests.
   * @param response Protocol response.
   * @param request Original authenticated HTTPS request.
   * @param identity Request-bound original paired identity.
   */
  void prepare_stream(bool resuming, bool &host_audio, resp_https_t response, req_https_t request, const verified_client_t &identity) {
    print_req<SunshineHTTPS>(request);
    pt::ptree tree;
    const auto result_field = resuming ? "root.resume" : "root.gamesession";
    tree.put(result_field, 0);
    auto respond = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });
    auto args = request->parse_query_string();
    int target = 0;
    if (!args.contains("rikey") || !args.contains("rikeyid") || (!resuming && ((!args.contains("appid") && !args.contains("appuuid")) || !args.contains("localAudioPlayMode")))) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required stream parameter");
      return;
    }
    std::shared_ptr<rtsp_stream::launch_session_t> session;
    try {
      session = make_launch_session(host_audio, args, identity);
    } catch (const std::exception &) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid stream parameters");
      return;
    }
    bool configured = false;
    bool started_application = false;
    const auto status = rtsp_stream::prepare_launch(session, [&]() {
      const auto client = current_client(identity.uuid, identity.cert);
      if (!client || !permissions::allows(client->permissions, permissions::view)) {
        return 403;
      }
      if (args.count("appid") > 1 || args.count("appuuid") > 1) {
        return 400;
      }
      proc::refresh(config::stream.file_apps);
      const auto current_identity = proc::proc.active_identity();
      const auto current = current_identity.first;
      const bool active =
#ifdef SUNSHINE_TESTS
        source_sessions_hook ? source_sessions_hook() > 0 :
#endif
                               rtsp_stream::session_count() > 0;
      std::vector<std::pair<int, std::string>> applications;
      for (const auto &app : proc::proc.apps_snapshot()) applications.emplace_back(std::stoi(app.id), app.uuid);
      std::pair<int, std::string> selected;
      try {
        selected = proc::catalog::resolve(args.contains("appid") ? std::optional {get_arg(args, "appid")} : std::nullopt, args.contains("appuuid") ? std::optional {get_arg(args, "appuuid")} : std::nullopt, applications, current_identity, resuming);
      } catch (const proc::catalog::error &error) {
        return error.status;
      }
      target = selected.first;
      const bool same_application = selected == current_identity && (current > 0 || active);
      if (!same_application && !permissions::allows(client->permissions, permissions::launch))
        return 403;
      if (active && !same_application)
        return 503;
      session->appid = target;
      session->app_uuid = std::move(selected.second);
      session->joining = active;
      if (!active && args.contains("localAudioPlayMode")) {
        host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
      }
      session->host_audio = host_audio;
      const auto encryption = net::encryption_mode_for_address(request->remote_endpoint().address());
      if (!session->rtsp_cipher && encryption == config::ENCRYPTION_MODE_MANDATORY) {
        return 403;
      }
      if (current > 0 && !same_application) {
        proc::proc.terminate();
      }
      if (!active) {
        configured = true;
#ifdef SUNSHINE_TESTS
        if (source_configure_hook) {
          source_configure_hook(*session);
        } else
#endif
        {
          display_device::configure_display(config::video, *session);
        }
        const auto probe_result =
#ifdef SUNSHINE_TESTS
          source_probe_hook ? source_probe_hook() :
#endif
                              video::probe_encoders();
        if (probe_result)
          return 503;
      }
      if (target > 0 && !same_application) {
        const auto error = proc::proc.execute(target, session);
        if (error) {
          return error;
        }
        started_application = true;
      }
      return 200;
    },
                                                    [&]() {
                                                      if (started_application) {
                                                        proc::proc.terminate();
                                                      }
                                                      if (configured) {
#ifdef SUNSHINE_TESTS
                                                        if (source_rollback_hook) {
                                                          source_rollback_hook();
                                                        } else
#endif
                                                        {
                                                          display_device::revert_configuration();
                                                        }
                                                      }
                                                    });
    tree.put("root.<xmlattr>.status_code", status);
    if (status != 200) {
      tree.put("root.<xmlattr>.status_message", status == 403 ? "Permission denied or required encryption unavailable" : "Stream preparation failed or another session is pending");
      return;
    }
    tree.put("root.sessionUrl0", std::format("{}{}:{}", session->rtsp_url_scheme, net::addr_to_url_escaped_string(request->local_endpoint().address()), static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))));
    tree.put(result_field, 1);
  }

  /**
   * @brief Start an application or join the already selected target without replacing it.
   * @param host_audio Host audio preference.
   * @param response Protocol response.
   * @param request Authenticated request.
   * @param identity Original paired identity.
   */
  void launch(bool &host_audio, resp_https_t response, req_https_t request, const verified_client_t &identity) {
    prepare_stream(false, host_audio, std::move(response), std::move(request), identity);
  }

  /**
   * @brief Join the current application with effective viewing authority.
   * @param host_audio Host audio preference.
   * @param response Protocol response.
   * @param request Authenticated request.
   * @param identity Original paired identity.
   */
  void resume(bool &host_audio, resp_https_t response, req_https_t request, const verified_client_t &identity) {
    prepare_stream(true, host_audio, std::move(response), std::move(request), identity);
  }

  /**
   * @brief Check whether cel.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void cancel(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&tree, &response]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    tree.put("root.cancel", 0);
    tree.put("root.<xmlattr>.status_code", 500);

    rtsp_stream::stop_application([]() {
      if (proc::proc.running() > 0) {
        proc::proc.terminate();
      }
      display_device::revert_configuration();
    });
    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  /**
   * @brief Return an application asset requested by the client.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void appasset(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto args = request->parse_query_string();
    auto app_image = proc::proc.get_app_image((int) util::from_view(get_arg(args, "appid")));

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
  }

  /**
   * @brief Install protected routes once for the production listener and native loopback regressions.
   * @param https_server Configured HTTPS listener.
   * @param host_audio Retained launch/resume host audio preference.
   */
  void configure_gamestream_routes(https_server_t &https_server, bool &host_audio) {
    https_server.default_resource["GET"] = authenticated([](auto response, auto request, const auto &) {
      not_found<SunshineHTTPS>(response, request);
    });
    https_server.resource["^/serverinfo$"]["GET"] = permitted(0, [](auto response, auto request, const auto &) {
      serverinfo<SunshineHTTPS>(response, request);
    });
    https_server.resource["^/pair$"]["GET"] = authenticated([](auto response, auto request, const auto &) {
      pair<SunshineHTTPS>(response, request);
    });
    https_server.resource["^/applist$"]["GET"] = permitted(permissions::list, [](auto response, auto request, const auto &) {
      applist(response, request);
    });
    https_server.resource["^/appasset$"]["GET"] = permitted(permissions::list, [](auto response, auto request, const auto &) {
      appasset(response, request);
    });
    https_server.resource["^/launch$"]["GET"] = authenticated([&host_audio](auto response, auto request, const auto &identity) {
      launch(host_audio, response, request, identity);
    });
    https_server.resource["^/resume$"]["GET"] = authenticated([&host_audio](auto response, auto request, const auto &identity) {
      resume(host_audio, response, request, identity);
    });
    https_server.resource["^/cancel$"]["GET"] = permitted(permissions::launch, [](auto response, auto request, const auto &) {
      cancel(response, request);
    });
  }

  void start() {
    platf::set_thread_name("nvhttp");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      load_state();
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;

    // Authenticate once per TLS connection, then revalidate its original pairing for every request.
    https_server.verify = verify_connection;
    https_server.on_verify_failed = unauthorized;

    configure_gamestream_routes(https_server, host_audio);

    https_server.config.reuse_address = true;
    https_server.config.address = net::get_bind_address(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = [](auto resp, auto req) {
      pair<SimpleWeb::HTTP>(resp, req);
    };

    http_server.config.reuse_address = true;
    http_server.config.address = net::get_bind_address(address_family);
    http_server.config.port = port_http;

    auto accept_and_run = [&](auto *web_server) {
      try {
        std::string name = "nvhttp::" + std::to_string(web_server->config.port);
        platf::set_thread_name(name);
        web_server->start();
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::jthread ssl {accept_and_run, &https_server};
    std::jthread tcp {accept_and_run, &http_server};

    // Wait for any event
    shutdown_event->view();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
  }

  bool erase_all_clients() {
    std::lock_guard mutation {client_mutation_mutex};
    std::vector<named_cert_t> removed;
    {
      std::lock_guard lock {client_auth_mutex()};
      if (!save_state(client_t {})) {
        return false;
      }
      removed = std::move(client_root.named_devices);
      client_root = {};
      cert_chain.clear();
    }
    for (const auto &device : removed) {
      rtsp_stream::reconcile_client(device.uuid, device.cert, true);
    }
    return true;
  }

  bool unpair_client(const std::string_view uuid) {
    std::lock_guard mutation {client_mutation_mutex};
    std::string cert;
    {
      std::lock_guard lock {client_auth_mutex()};
      auto candidate = client_root;
      const auto it = std::ranges::find(candidate.named_devices, uuid, &named_cert_t::uuid);
      if (it == candidate.named_devices.end()) {
        return false;
      }
      cert = it->cert;
      std::erase_if(candidate.named_devices, [&](const auto &device) {
        return device.uuid == uuid;
      });
      if (!save_state(candidate)) {
        return false;
      }
      client_root = std::move(candidate);
      rebuild_client_cert_chain();
    }
    rtsp_stream::reconcile_client(uuid, cert, true);
    return true;
  }

  bool update_client(const std::string_view uuid, const std::optional<bool> enabled, const std::optional<permissions::mask_t> grant) {
    if ((grant && !permissions::valid(*grant)) || (!grant && !enabled)) {
      return false;
    }
    std::lock_guard mutation {client_mutation_mutex};
    std::string cert;
    bool destroy = false;
    {
      std::lock_guard lock {client_auth_mutex()};
      auto candidate = client_root;
      const auto it = std::ranges::find(candidate.named_devices, uuid, &named_cert_t::uuid);
      if (it == candidate.named_devices.end()) {
        return false;
      }
      if (enabled) {
        it->enabled = *enabled;
      }
      if (grant) {
        it->permissions = *grant;
      }
      it->revision = ++authorization_revision;
      cert = it->cert;
      destroy = !it->enabled;
      if (!save_state(candidate)) {
        return false;
      }
      client_root = std::move(candidate);
      rebuild_client_cert_chain();
    }
    rtsp_stream::reconcile_client(uuid, cert, destroy);
    return true;
  }

  bool set_client_enabled(const std::string_view uuid, bool enabled) {
    return update_client(uuid, enabled, std::nullopt);
  }

  /**
   * @brief Get cert by UUID.
   */
  std::string get_cert_by_uuid(const std::string_view uuid) {
    std::lock_guard lock {client_auth_mutex()};
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert.uuid == uuid) {
        return named_cert.cert;
      }
    }
    return {};
  }

#ifdef SUNSHINE_TESTS
  namespace test_support {
    void set_source_preparation_hooks(std::function<void(const rtsp_stream::launch_session_t &)> configure, std::function<int()> probe, std::function<void()> rollback, std::function<int()> sessions) {
      rtsp_stream::catalog_operation([&]() {
        source_configure_hook = std::move(configure);
        source_probe_hook = std::move(probe);
        source_rollback_hook = std::move(rollback);
        source_sessions_hook = std::move(sessions);
      });
    }

    void set_gamestream_routes(SimpleWeb::Server<SunshineHTTPS> &server, bool &host_audio) {
      configure_gamestream_routes(server, host_audio);
    }

    std::unique_ptr<SimpleWeb::Server<SunshineHTTPS>> make_https_server(
      const std::string &certificate_file,
      const std::string &private_key_file,
      std::shared_ptr<SimpleWeb::ScopeRunner> &handler_scope,
      bool authenticate_clients
    ) {
      /**
       * @brief Expose the production HTTPS handler scope to lifetime regression tests.
       */
      class test_https_server_t: public SunshineHTTPSServer {
      public:
        using SunshineHTTPSServer::handler_runner;
        using SunshineHTTPSServer::SunshineHTTPSServer;
      };

      auto server = std::make_unique<test_https_server_t>(certificate_file, private_key_file);
      handler_scope = server->handler_runner;
      if (authenticate_clients) {
        server->verify = verify_connection;
        server->on_verify_failed = unauthorized;
      }
      return server;
    }

    void set_authenticated_handler(
      SimpleWeb::ServerBase<SunshineHTTPS> &server,
      const std::string &path,
      std::function<void(resp_https_t, req_https_t, const verified_client_t &)> handler
    ) {
      server.resource[path]["GET"] = authenticated(std::move(handler));
    }

    std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(const verified_client_t &identity, const SimpleWeb::CaseInsensitiveMultimap &args) {
      return nvhttp::make_launch_session(false, args, identity);
    }

    void pair_http(
      std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response> response,
      std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request> request
    ) {
      pair<SimpleWeb::HTTP>(std::move(response), std::move(request));
    }

    void reset_client_state() {
      std::lock_guard lock {client_auth_mutex()};
      client_root = {};
      cert_chain.clear();
    }

    std::string add_client(const std::string &name, std::string cert, bool enabled) {
      auto uuid = add_authorized_client(name, std::move(cert));
      if (!uuid.empty() && !enabled) {
        set_client_enabled(uuid, false);
      }
      return uuid;
    }

    bool duplicate_client(const std::string_view uuid) {
      std::lock_guard lock {client_auth_mutex()};
      const auto client_it = std::ranges::find(client_root.named_devices, uuid, &named_cert_t::uuid);
      if (client_it == client_root.named_devices.end()) {
        return false;
      }

      auto duplicate = *client_it;
      duplicate.uuid = uuid_util::uuid_t::generate().string();
      client_root.named_devices.emplace_back(std::move(duplicate));
      rebuild_client_cert_chain();
      save_state();
      return true;
    }

    bool authorize_client_certificate(const std::string_view cert) {
      auto certificate = crypto::x509(cert);
      if (!certificate) {
        return false;
      }

      std::lock_guard lock {client_auth_mutex()};
      return verify_client_certificate(certificate.get()) == nullptr;
    }

    bool complete_pairing(const std::string_view pairing_id, const bool success) {
      std::scoped_lock lock {map_id_sess_mutex()};
      const auto sess_it = std::ranges::find_if(map_id_sess, [&](const auto &entry) {
        return entry.second.async_insert_pin.id == pairing_id;
      });
      if (sess_it == map_id_sess.end()) {
        return false;
      }

      nvhttp::complete_pairing(sess_it->second, success);
      map_id_sess.erase(sess_it);
      return true;
    }

    void reload_client_state() {
      load_state();
    }
  }  // namespace test_support
#endif
}  // namespace nvhttp
