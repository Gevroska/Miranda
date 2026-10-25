/**
 * @file src/rtsp.cpp
 * @brief Definitions for RTSP streaming.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

extern "C" {
#include <moonlight-common-c/src/Limelight-internal.h>
#include <moonlight-common-c/src/Rtsp.h>
}

// standard includes
#include <array>
#include <cctype>
#include <format>
#include <future>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

// lib includes
#include <boost/asio.hpp>
#include <boost/bind.hpp>

// local includes
#include "config.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "rtsp.h"
#include "stream.h"
#include "sync.h"
#include "video.h"

namespace asio = boost::asio;

using asio::ip::tcp;
using asio::ip::udp;

using namespace std::literals;

namespace rtsp_stream {
  /**
   * @brief Release msg resources.
   *
   * @param msg Raw RTSP message buffer being parsed or encrypted.
   */
  void free_msg(PRTSP_MESSAGE msg) {
    freeMessage(msg);

    delete msg;
  }

  class rtsp_server_t;

  /**
   * @brief Owning pointer for an RTSP message allocated by moonlight-common-c.
   */
  using msg_t = util::safe_ptr<RTSP_MESSAGE, &free_msg>;
  /**
   * @brief RTSP command handler signature used by the server dispatch table.
   */
  using cmd_func_t = std::function<void(rtsp_server_t *server, tcp::socket &, launch_session_t &, msg_t &&)>;

  /**
   * @brief Write msg details to the log.
   *
   * @param msg Raw RTSP message buffer being parsed or encrypted.
   */
  void print_msg(PRTSP_MESSAGE msg);
  /**
   * @brief Send the RTSP response used for unsupported commands.
   *
   * @param sock Socket used to read or write the protocol message.
   * @param session Active streaming or pairing session for the request.
   * @param req Parsed RTSP request being handled.
   */
  void cmd_not_found(tcp::socket &sock, launch_session_t &, msg_t &&req);
  /**
   * @brief Send the protocol response for the current request.
   *
   * @param sock Socket used to read or write the protocol message.
   * @param session Active streaming or pairing session for the request.
   * @param options Request options or socket options to apply.
   * @param statuscode RTSP status code to send in the response.
   * @param status_msg Status msg.
   * @param seqn RTSP CSeq value associated with the request.
   * @param payload Optional payload body to include in the response.
   */
  void respond(tcp::socket &sock, launch_session_t &session, POPTION_ITEM options, int statuscode, const char *status_msg, int seqn, const std::string_view &payload);

  /**
   * @brief RTSP client socket state and receive buffer parser.
   */
  class socket_t: public std::enable_shared_from_this<socket_t> {
  public:
    /**
     * @brief Construct an RTSP socket wrapper around an accepted TCP connection.
     *
     * @param io_context Io context.
     * @param handle_data_fn Handle data.
     */
    socket_t(boost::asio::io_context &io_context, std::function<void(tcp::socket &sock, launch_session_t &, msg_t &&)> &&handle_data_fn):
        handle_data_fn {std::move(handle_data_fn)},
        sock {io_context} {
    }

    /**
     * @brief Queue an asynchronous read to begin the next message.
     */
    void read() {
      if (begin == std::end(msg_buf) || (session->rtsp_cipher && begin + sizeof(encrypted_rtsp_header_t) >= std::end(msg_buf))) {
        BOOST_LOG(error) << "RTSP: read(): Exceeded maximum rtsp packet size: "sv << msg_buf.size();

        respond(sock, *session, nullptr, 400, "BAD REQUEST", 0, {});

        boost::system::error_code ec;
        sock.close(ec);

        return;
      }

      if (session->rtsp_cipher) {
        // For encrypted RTSP, we will read the the entire header first
        boost::asio::async_read(sock, boost::asio::buffer(begin, sizeof(encrypted_rtsp_header_t)), boost::bind(&socket_t::handle_read_encrypted_header, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
      } else {
        sock.async_read_some(
          boost::asio::buffer(begin, (std::size_t) (std::end(msg_buf) - begin)),
          boost::bind(
            &socket_t::handle_read_plaintext,
            shared_from_this(),
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred
          )
        );
      }
    }

    /**
     * @brief Read the encrypted RTSP header before receiving the encrypted payload.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void handle_read_encrypted_header(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_read_encrypted_header(): Handle read of size: "sv << bytes << " bytes"sv;

      auto sock_close = util::fail_guard([&socket]() {
        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Couldn't close tcp socket: "sv << ec.message();
        }
      });

      if (ec || bytes < sizeof(encrypted_rtsp_header_t)) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Couldn't read from tcp socket: "sv << ec.message();

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      auto header = (encrypted_rtsp_header_t *) socket->begin;
      if (!header->is_encrypted()) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Rejecting unencrypted RTSP message"sv;

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      auto payload_length = header->payload_length();

      // Check if we have enough space to read this message
      if (socket->begin + sizeof(*header) + payload_length >= std::end(socket->msg_buf)) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Exceeded maximum rtsp packet size: "sv << socket->msg_buf.size();

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      sock_close.disable();

      // Read the remainder of the header and full encrypted payload
      boost::asio::async_read(socket->sock, boost::asio::buffer(socket->begin + bytes, payload_length), boost::bind(&socket_t::handle_read_encrypted_message, socket->shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
    }

    /**
     * @brief Read and decrypt the encrypted RTSP payload.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void handle_read_encrypted_message(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_read_encrypted(): Handle read of size: "sv << bytes << " bytes"sv;

      auto sock_close = util::fail_guard([&socket]() {
        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_read_encrypted_message(): Couldn't close tcp socket: "sv << ec.message();
        }
      });

      auto header = (encrypted_rtsp_header_t *) socket->begin;
      auto payload_length = header->payload_length();
      auto seq = util::endian::big<std::uint32_t>(header->sequenceNumber);

      if (ec || bytes < payload_length) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted(): Couldn't read from tcp socket: "sv << ec.message();

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
      // Section 8.2.1. The sequence number is our "invocation" field and the 'RC' in the
      // high bytes is the "fixed" field. Because each client provides their own unique
      // key, our values in the fixed field need only uniquely identify each independent
      // use of the client's key with AES-GCM in our code.
      //
      // The sequence number is 32 bits long which allows for 2^32 RTSP messages to be
      // received from each client before the IV repeats.
      crypto::aes_t iv(12);
      std::copy_n((uint8_t *) &seq, sizeof(seq), std::begin(iv));
      iv[10] = 'C';  // Client originated
      iv[11] = 'R';  // RTSP

      std::vector<uint8_t> plaintext;
      if (socket->session->rtsp_cipher->decrypt(std::string_view {(const char *) header->tag.data(), sizeof(header->tag) + bytes}, plaintext, &iv)) {
        BOOST_LOG(error) << "Failed to verify RTSP message tag"sv;

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      msg_t req {new msg_t::element_type {}};
      if (auto status = parseRtspMessage(req.get(), (char *) plaintext.data(), (int) plaintext.size())) {
        BOOST_LOG(error) << "Malformed RTSP message: ["sv << status << ']';

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      sock_close.disable();

      print_msg(req.get());

      socket->handle_data(std::move(req));
    }

    /**
     * @brief Queue an asynchronous read of the payload portion of a plaintext message.
     */
    void read_plaintext_payload() {
      if (begin == std::end(msg_buf)) {
        BOOST_LOG(error) << "RTSP: read_plaintext_payload(): Exceeded maximum rtsp packet size: "sv << msg_buf.size();

        respond(sock, *session, nullptr, 400, "BAD REQUEST", 0, {});

        boost::system::error_code ec;
        sock.close(ec);

        return;
      }

      sock.async_read_some(
        boost::asio::buffer(begin, (std::size_t) (std::end(msg_buf) - begin)),
        boost::bind(
          &socket_t::handle_plaintext_payload,
          shared_from_this(),
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred
        )
      );
    }

    /**
     * @brief Read the plaintext RTSP payload after the header is parsed.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void handle_plaintext_payload(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_plaintext_payload(): Handle read of size: "sv << bytes << " bytes"sv;

      auto sock_close = util::fail_guard([&socket]() {
        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_plaintext_payload(): Couldn't close tcp socket: "sv << ec.message();
        }
      });

      if (ec) {
        BOOST_LOG(error) << "RTSP: handle_plaintext_payload(): Couldn't read from tcp socket: "sv << ec.message();

        return;
      }

      auto end = socket->begin + bytes;
      msg_t req {new msg_t::element_type {}};
      if (auto status = parseRtspMessage(req.get(), socket->msg_buf.data(), (int) (end - socket->msg_buf.data()))) {
        BOOST_LOG(error) << "Malformed RTSP message: ["sv << status << ']';

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      sock_close.disable();

      auto fg = util::fail_guard([&socket]() {
        socket->read_plaintext_payload();
      });

      auto content_length = 0;
      for (auto option = req->options; option != nullptr; option = option->next) {
        if ("Content-length"sv == option->option) {
          BOOST_LOG(debug) << "Found Content-Length: "sv << option->content << " bytes"sv;

          // If content_length > bytes read, then we need to store current data read,
          // to be appended by the next read.
          std::string_view content {option->content};
          auto begin = std::ranges::find_if(content, [](auto ch) {
            return (bool) std::isdigit(ch);
          });

          content_length = (int) util::from_chars(begin, std::end(content));
          break;
        }
      }

      if (end - socket->crlf >= content_length) {
        if (end - socket->crlf > content_length) {
          BOOST_LOG(warning) << "(end - socket->crlf) > content_length -- "sv << (std::size_t) (end - socket->crlf) << " > "sv << content_length;
        }

        fg.disable();
        print_msg(req.get());

        socket->handle_data(std::move(req));
      }

      socket->begin = end;
    }

    /**
     * @brief Read and parse the plaintext RTSP request header.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void handle_read_plaintext(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_read_plaintext(): Handle read of size: "sv << bytes << " bytes"sv;

      if (ec) {
        BOOST_LOG(error) << "RTSP: handle_read_plaintext(): Couldn't read from tcp socket: "sv << ec.message();

        boost::system::error_code close_error;
        socket->sock.close(close_error);

        if (close_error) {
          BOOST_LOG(error) << "RTSP: handle_read_plaintext(): Couldn't close tcp socket: "sv << close_error.message();
        }

        return;
      }

      auto fg = util::fail_guard([&socket]() {
        socket->read();
      });

      auto begin = std::max(socket->begin - 4, socket->begin);
      auto buf_size = bytes + (begin - socket->begin);
      auto end = begin + buf_size;

      constexpr auto needle = "\r\n\r\n"sv;

      auto it = std::search(begin, begin + buf_size, std::begin(needle), std::end(needle));
      if (it == end) {
        socket->begin = end;

        return;
      }

      // Emulate read completion for payload data
      socket->begin = it + needle.size();
      socket->crlf = socket->begin;
      buf_size = end - socket->begin;

      fg.disable();
      handle_plaintext_payload(socket, ec, buf_size);
    }

    /**
     * @brief Dispatch a parsed RTSP request to the socket's command handler.
     *
     * @param req Parsed RTSP request being handled.
     */
    void handle_data(msg_t &&req) {
      handle_data_fn(sock, *session, std::move(req));
    }

    std::function<void(tcp::socket &sock, launch_session_t &, msg_t &&)> handle_data_fn;  ///< Command dispatcher installed by the RTSP server.

    tcp::socket sock;  ///< TCP socket connected to the RTSP client.

    std::array<char, 2048> msg_buf;  ///< Receive buffer for one RTSP request header and payload.

    char *crlf;  ///< Pointer to the next CRLF delimiter while parsing the receive buffer.
    char *begin = msg_buf.data();  ///< Start of the unparsed data currently in `msg_buf`.

    std::shared_ptr<launch_session_t> session;  ///< Launch session claimed by this RTSP socket.
  };

  /**
   * @brief RTSP listener that matches incoming clients to pending launch sessions.
   */
  class rtsp_server_t {
  public:
    ~rtsp_server_t() {
      clear();
    }

    /**
     * @brief Bind the underlying socket or graphics resource to its target.
     *
     * @param af Address family used for socket creation or binding.
     * @param port TCP or UDP port number.
     * @param ec Error code returned by the asynchronous operation.
     * @return Network operation status.
     */
    int bind(net::af_e af, std::uint16_t port, boost::system::error_code &ec) {
      af = net::get_effective_address_family(af);
      acceptor.open(af == net::IPV4 ? tcp::v4() : tcp::v6(), ec);
      if (ec) {
        return -1;
      }

      acceptor.set_option(boost::asio::socket_base::reuse_address {true});

      auto bind_addr_str = net::get_bind_address(af);
      const auto bind_addr = boost::asio::ip::make_address(bind_addr_str, ec);
      if (ec) {
        BOOST_LOG(error) << "Invalid bind address: "sv << bind_addr_str << " - " << ec.message();
        return -1;
      }

      acceptor.bind(tcp::endpoint(bind_addr, port), ec);
      if (ec) {
        return -1;
      }

      acceptor.listen(4096, ec);
      if (ec) {
        return -1;
      }

      next_socket = std::make_shared<socket_t>(io_context, [this](tcp::socket &sock, launch_session_t &session, msg_t &&msg) {
        handle_msg(sock, session, std::move(msg));
      });

      acceptor.async_accept(next_socket->sock, [this](const auto &ec) {
        handle_accept(ec);
      });

      return 0;
    }

    /**
     * @brief Dispatch a parsed RTSP request to its handler.
     *
     * @param sock Socket used to read or write the protocol message.
     * @param session Active streaming or pairing session for the request.
     * @param req Parsed RTSP request being handled.
     */
    void handle_msg(tcp::socket &sock, launch_session_t &session, msg_t &&req) {
      if (!authorized(session)) {
        respond(sock, session, nullptr, 403, "Forbidden", req->sequenceNumber, {});
        boost::system::error_code error;
        sock.shutdown(boost::asio::socket_base::shutdown_both, error);
        return;
      }
      if (auto func = _map_cmd_cb.find(req->message.request.command); func != std::end(_map_cmd_cb)) {
        func->second(this, sock, session, std::move(req));
      } else {
        cmd_not_found(sock, session, std::move(req));
      }

      boost::system::error_code ec;
      sock.shutdown(boost::asio::socket_base::shutdown_type::shutdown_both, ec);
    }

    /**
     * @brief Accept a pending connection and arm the server for the next client.
     *
     * @param ec Error code returned by the asynchronous operation.
     */
    void handle_accept(const boost::system::error_code &ec) {
      if (ec) {
        BOOST_LOG(error) << "Couldn't accept incoming connections: "sv << ec.message();

        // Stop server
        clear();
        return;
      }

      auto socket = std::move(next_socket);

      auto launch_session {launch_event.view(0s)};
      if (launch_session && launch_session->ready && authorized(*launch_session)) {
        // Associate the current RTSP session with this socket and start reading
        socket->session = launch_session;
        socket->read();
      } else {
        // This can happen due to normal things like port scanning, so let's not make these visible by default
        BOOST_LOG(debug) << "No pending session for incoming RTSP connection"sv;

        // If there is no session pending, close the connection immediately
        boost::system::error_code close_error;
        socket->sock.close(close_error);
      }

      // Queue another asynchronous accept for the next incoming connection
      next_socket = std::make_shared<socket_t>(io_context, [this](tcp::socket &sock, launch_session_t &session, msg_t &&msg) {
        handle_msg(sock, session, std::move(msg));
      });
      acceptor.async_accept(next_socket->sock, [this](const auto &ec) {
        handle_accept(ec);
      });
    }

    /**
     * @brief Register or visit handlers stored in the map.
     *
     * @param type Protocol, message, or resource type selector.
     * @param cb Callback invoked for each matching message or session.
     */
    void map(const std::string_view &type, cmd_func_t cb) {
      _map_cmd_cb.try_emplace(type, std::move(cb));
    }

    /**
     * @brief Launch a new streaming session.
     * @note If the client does not begin streaming within the ping_timeout,
     *       the session will be discarded.
     * @param launch_session Streaming session information.
     */
    bool session_raise(std::shared_ptr<launch_session_t> launch_session) {
      if (stopping || launch_event.view(0s) || !authorized(*launch_session, false)) {
        return false;
      }
      tokens.emplace_back(launch_session);
      std::erase_if(tokens, [](const auto &token) {
        return token.expired();
      });
      launch_event.raise(std::move(launch_session));
      raised_timer.expires_after(config::stream.ping_timeout);
      raised_timer.async_wait([this](const boost::system::error_code &ec) {
        if (!ec) {
          auto discarded = launch_event.pop(0s);
          if (discarded && !discarded->admitted) {
            discarded->cancelled = true;
          }
        }
      });
      return true;
    }

    /**
     * @brief Remove only the caller's reservation; accepted sockets retain a checked token.
     * @param launch_session_id Original reservation identifier.
     */
    void session_clear(uint32_t launch_session_id) {
      auto pending = launch_event.view(0s);
      if (pending && pending->id == launch_session_id) {
        if (!pending->admitted) {
          pending->cancelled = true;
        }
        raised_timer.cancel();
        launch_event.pop(0s);
      }
    }

    /**
     * @brief Check a launch's current authority, including held accepted-socket tokens.
     * @param session Original launch token.
     * @param require_ready Whether completed source preparation is required.
     * @return Whether the token may still dispatch RTSP.
     */
    bool authorized(const launch_session_t &session, bool require_ready = true) {
      const auto client = nvhttp::current_client(session.client_uuid, session.client_cert);
      return !session.cancelled && (!require_ready || session.ready) && client &&
             client->revision == session.authorization_revision && permissions::allows(client->permissions, permissions::view);
    }

    /**
     * @brief Claim the single stream admission before allocating any streaming resources.
     * @param session Original launch token.
     * @return Whether this is its first currently authorized ANNOUNCE.
     */
    bool claim(launch_session_t &session) {
      if (session.admitted || !authorized(session)) {
        return false;
      }
      session.admitted = true;
      return true;
    }

    /**
     * @brief Serialize control operations with socket dispatch without involving streaming threads.
     * @param operation Operation to run and await on the RTSP owner.
     * @return Operation result, if any.
     */
    template<class Function>
    auto sync(Function operation) {
      if (io_context.get_executor().running_in_this_thread()) {
        return operation();
      }
      std::unique_lock lock {lifecycle_mutex};
      if (stopping && shutdown_done.valid()) {
        const auto done = shutdown_done;
#ifdef SUNSHINE_TESTS
        notify_owner("stopping_wait");
#endif
        lock.unlock();
        done.wait();
        lock.lock();
      }
      if (!running) {
        return operation();
      }
      auto task = std::make_shared<std::packaged_task<decltype(operation())()>>(std::move(operation));
      auto result = task->get_future();
      asio::post(io_context, [task]() {
        (*task)();
      });
#ifdef SUNSHINE_TESTS
      notify_owner("operation_posted");
#endif
      lock.unlock();
      return result.get();
    }

    /**
     * @brief Post control-thread launch cleanup without waiting for an owner joining that thread.
     * @param id Launch reservation identifier.
     */
    void post_clear(uint32_t id) {
      std::lock_guard lock {lifecycle_mutex};
      if (stopping) {
        return;
      }
      if (!running) {
        session_clear(id);
      } else {
        asio::post(io_context, [this, id]() {
          session_clear(id);
        });
      }
    }

    /**
     * @brief Reconcile one pairing after its durable authorization change.
     * @param uuid Original pairing UUID.
     * @param cert Original exact paired certificate.
     * @param destroy Whether retained controllers must be destroyed.
     */
    void reconcile(std::string_view uuid, std::string_view cert, bool destroy) {
      for (auto &weak : tokens) {
        if (auto token = weak.lock(); token && token->client_uuid == uuid) {
          token->cancelled = true;
          session_clear(token->id);
        }
      }
      const auto client = nvhttp::current_client(uuid, cert);
      auto lock = _session_slots.lock();
      for (auto it = _session_slots->begin(); it != _session_slots->end();) {
        auto &session = **it;
        if (stream::session::client_uuid(session) != uuid) {
          ++it;
        } else if (!client || !permissions::allows(client->permissions, permissions::view)) {
          stream::session::stop(session);
          stream::session::join(session);
          it = _session_slots->erase(it);
        } else {
          stream::session::update_permissions(session, client->permissions);
          ++it;
        }
      }
      if (destroy) {
        input::terminate_gamepads(cert);
      }
    }

    /**
     * @brief Invalidate every token, including accepted sockets no longer in the pending slot.
     */
    void invalidate_launches() {
      for (auto &weak : tokens) {
        if (auto token = weak.lock()) {
          token->cancelled = true;
        }
      }
      raised_timer.cancel();
      launch_event.pop(0s);
    }

    /**
     * @brief Get the number of active sessions.
     * @return Count of active sessions.
     */
    int session_count() {
      auto lg = _session_slots.lock();
      return (int) _session_slots->size();
    }

    safe::event_t<std::shared_ptr<launch_session_t>> launch_event;  ///< Launch event.

    /**
     * @brief Clear launch sessions.
     * @param all If true, clear all sessions. Otherwise, only clear timed out and stopped sessions.
     * @examples
     * clear(false);
     * @examples_end
     */
    void clear(bool all = true) {
      auto lg = _session_slots.lock();

      for (auto i = _session_slots->begin(); i != _session_slots->end();) {
        auto &slot = *(*i);
        if (all || stream::session::state(slot) == stream::session::state_e::STOPPING) {
          stream::session::stop(slot);
          stream::session::join(slot);

          i = _session_slots->erase(i);
        } else {
          i++;
        }
      }
    }

    /**
     * @brief Clear by cert state.
     *
     * @param cert Certificate data or object used by the operation.
     */
    void clear_by_cert(std::string_view cert) {
      for (auto &weak : tokens) {
        if (auto token = weak.lock(); token && token->client_cert == cert) {
          token->cancelled = true;
          session_clear(token->id);
        }
      }

      auto lg = _session_slots.lock();
      for (auto i = _session_slots->begin(); i != _session_slots->end();) {
        auto &slot = *(*i);
        if (stream::session::client_cert(slot) == cert) {
          stream::session::stop(slot);
          stream::session::join(slot);
          i = _session_slots->erase(i);
        } else {
          i++;
        }
      }
    }

    /**
     * @brief Removes the provided session from the set of sessions.
     * @param session The session to remove.
     */
    void remove(const std::shared_ptr<stream::session_t> &session) {
      auto lg = _session_slots.lock();
      _session_slots->erase(session);
    }

    /**
     * @brief Inserts the provided session into the set of sessions.
     * @param session The session to insert.
     */
    void insert(const std::shared_ptr<stream::session_t> &session) {
      auto lg = _session_slots.lock();
      _session_slots->emplace(session);
      BOOST_LOG(info) << "New streaming session started [active sessions: "sv << _session_slots->size() << ']';
    }

    /**
     * @brief Runs an iteration of the RTSP server loop
     */
    void iterate() {
      // If we have a session, we will return to the server loop every
      // 500ms to allow session cleanup to happen.
      if (session_count() > 0) {
        io_context.run_one_for(500ms);
      } else {
        io_context.run_one();
      }
    }

    /**
     * @brief Stop the RTSP server.
     */
    void stop() {
      std::unique_lock lock {lifecycle_mutex};
      if (stopping) {
        const auto done = shutdown_done;
        lock.unlock();
        if (done.valid()) {
          done.wait();
        }
        return;
      }
      stopping = true;
      auto completion = std::make_shared<std::promise<void>>();
      shutdown_done = completion->get_future().share();
      const auto finish = [this, completion]() {
#ifdef SUNSHINE_TESTS
        notify_owner("stop_begin");
#endif
        invalidate_launches();
        boost::system::error_code error;
        acceptor.close(error);
        clear();
        {
          std::lock_guard lock {lifecycle_mutex};
          running = false;
          stopped = true;
        }
        completion->set_value();
      };
      if (running) {
        asio::post(io_context, finish);
        const auto done = shutdown_done;
        lock.unlock();
        done.wait();
      } else {
        lock.unlock();
        finish();
      }
      io_context.stop();
    }

    /**
     * @brief Publish the owner before requests may post and restart its context for isolated tests.
     */
    void begin() {
      std::lock_guard lock {lifecycle_mutex};
      io_context.restart();
      running = true;
      stopping = false;
      stopped = false;
      shutdown_done = {};
    }

#ifdef SUNSHINE_TESTS
    std::function<void(std::string_view)> owner_hook;  ///< Deterministic lifecycle boundaries for tests.
    std::mutex owner_hook_mutex;  ///< Protects test observer publication across the owner and posting threads.

    void notify_owner(std::string_view event) {
      std::function<void(std::string_view)> hook;
      {
        std::lock_guard lock {owner_hook_mutex};
        hook = owner_hook;
      }
      if (hook) {
        hook(event);
      }
    }

    void reset_test_owner() {
      std::lock_guard lock {lifecycle_mutex};
      stopping = false;
      stopped = false;
      shutdown_done = {};
      std::lock_guard observer {owner_hook_mutex};
      owner_hook = {};
    }

    auto test_work_guard() {
      return asio::make_work_guard(io_context);
    }
#endif

    std::atomic_bool stopping {false};
    std::atomic_bool stopped {false};  ///< Owner loop termination follows the serialized stop operation.

  private:
    std::unordered_map<std::string_view, cmd_func_t> _map_cmd_cb;

    sync_util::sync_t<std::set<std::shared_ptr<stream::session_t>>> _session_slots;

    boost::asio::io_context io_context;
    tcp::acceptor acceptor {io_context};
    boost::asio::steady_timer raised_timer {io_context};

    std::shared_ptr<socket_t> next_socket;
    std::vector<std::weak_ptr<launch_session_t>> tokens;  ///< Accepted-socket tokens that a global Stop must invalidate.
    std::recursive_mutex lifecycle_mutex;  ///< Protects offline direct execution versus owner dispatch.
    bool running = false;  ///< A live RTSP owner will drain posted operations.
    std::shared_future<void> shutdown_done;  ///< Completes all accepted work before offline calls may proceed.
  };

  rtsp_server_t server {};  ///< Singleton RTSP server used by GameStream launch sessions.

  /**
   * @brief Queue a launch session until the RTSP client connects.
   */
  bool launch_session_raise(std::shared_ptr<launch_session_t> launch_session) {
    return server.sync([&]() {
      launch_session->ready = true;
      return server.session_raise(launch_session);
    });
  }

  int prepare_launch(std::shared_ptr<launch_session_t> launch_session, std::function<int()> prepare, std::function<void()> rollback) {
    return server.sync([&]() {
      if (!server.authorized(*launch_session, false)) {
        return 403;
      }
      if (!server.session_raise(launch_session)) {
        return 503;
      }
      bool completed = false;
      auto guard = util::fail_guard([&]() {
        if (!completed) {
          launch_session->cancelled = true;
          server.session_clear(launch_session->id);
          if (rollback) {
            rollback();
          }
        }
      });
      const auto result = prepare();
      if (result != 200) {
        return result;
      }
      if (!server.authorized(*launch_session, false)) {
        return 403;
      }
      launch_session->ready = true;
      completed = true;
      return 200;
    });
  }

  bool authorize_operation(std::string_view uuid, std::string_view cert, permissions::mask_t required, std::function<void()> operation) {
    return server.sync([&]() {
      const auto client = nvhttp::current_client(uuid, cert);
      if (!client || (required && !permissions::allows(client->permissions, required))) {
        return false;
      }
      operation();
      return true;
    });
  }

  void reconcile_client(std::string_view uuid, std::string_view cert, bool destroy) {
    server.sync([&]() {
      server.reconcile(uuid, cert, destroy);
    });
  }

  void launch_session_clear(uint32_t launch_session_id) {
    server.post_clear(launch_session_id);
  }

  int session_count() {
    return server.sync([]() {
      server.clear(false);
      return server.session_count();
    });
  }

  void terminate_sessions() {
    stop_application({});
  }

  void stop_application(std::function<void()> stop) {
    server.sync([&]() {
      server.invalidate_launches();
      server.clear(true);
      input::terminate_gamepads();
      if (stop) {
        stop();
      }
    });
  }

  void terminate_sessions_by_cert(std::string_view cert) {
    server.sync([&]() {
      server.clear_by_cert(cert);
      input::terminate_gamepads(cert);
    });
  }

#ifdef SUNSHINE_TESTS
  namespace test_support {
    /**
     * @brief Own only the production admission executor, without network or media initialization.
     */
    class owner_t: public platf::deinit_t {
      decltype(server.test_work_guard()) guard {server.test_work_guard()};  ///< Keeps an idle test executor runnable.
      std::jthread thread;  ///< Actual single RTSP owner.

    public:
      owner_t() {
        server.begin();
        thread = std::jthread([]() {
          while (!server.stopped) {
            server.iterate();
          }
        });
      }

      ~owner_t() override {
        server.stop();
        thread.join();
        server.reset_test_owner();
      }
    };

    std::unique_ptr<platf::deinit_t> start_owner() {
      return std::make_unique<owner_t>();
    }

    void stop_owner() {
      server.stop();
    }

    void set_owner_hook(std::function<void(std::string_view)> hook) {
      server.sync([&]() {
        std::lock_guard lock {server.owner_hook_mutex};
        server.owner_hook = std::move(hook);
      });
    }

    bool token_authorized(const std::shared_ptr<launch_session_t> &launch) {
      return server.sync([&]() {
        return server.authorized(*launch);
      });
    }

    bool claim_launch(const std::shared_ptr<launch_session_t> &launch) {
      return server.sync([&]() {
        return server.claim(*launch);
      });
    }
  }  // namespace test_support
#endif

  /**
   * @brief Send the serialized response over the active socket.
   *
   * @param sock Socket used to read or write the protocol message.
   * @param sv String view containing the text to inspect.
   * @return Network operation status.
   */
  int send(tcp::socket &sock, const std::string_view &sv) {
    std::size_t bytes_send = 0;

    while (bytes_send != sv.size()) {
      boost::system::error_code ec;
      bytes_send += sock.send(boost::asio::buffer(sv.substr(bytes_send)), 0, ec);

      if (ec) {
        BOOST_LOG(error) << "RTSP: Couldn't send data over tcp socket: "sv << ec.message();
        return -1;
      }
    }

    return 0;
  }

  /**
   * @brief Send the protocol response for the current request.
   *
   * @param sock Socket used to read or write the protocol message.
   * @param session Active streaming or pairing session for the request.
   * @param resp RTSP response string to send to the client.
   */
  void respond(tcp::socket &sock, launch_session_t &session, msg_t &resp) {
    auto [payload, original_payload_length] = std::make_pair(resp->payload, resp->payloadLength);

    // Restore response message for proper destruction
    auto lg = util::fail_guard([&resp, &payload, &original_payload_length]() {
      resp->payload = payload;
      resp->payloadLength = original_payload_length;
    });

    resp->payload = nullptr;
    resp->payloadLength = 0;

    int serialized_len;
    util::c_ptr<char> raw_resp {serializeRtspMessage(resp.get(), &serialized_len)};
    BOOST_LOG(debug)
      << "---Begin Response---"sv << std::endl
      << std::string_view {raw_resp.get(), (std::size_t) serialized_len} << std::endl
      << std::string_view {payload, (std::size_t) original_payload_length} << std::endl
      << "---End Response---"sv << std::endl;

    // Encrypt the RTSP message if encryption is enabled
    if (session.rtsp_cipher) {
      // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
      // Section 8.2.1. The sequence number is our "invocation" field and the 'RH' in the
      // high bytes is the "fixed" field. Because each client provides their own unique
      // key, our values in the fixed field need only uniquely identify each independent
      // use of the client's key with AES-GCM in our code.
      //
      // The sequence number is 32 bits long which allows for 2^32 RTSP messages to be
      // sent to each client before the IV repeats.
      crypto::aes_t iv(12);
      session.rtsp_iv_counter++;
      std::copy_n((uint8_t *) &session.rtsp_iv_counter, sizeof(session.rtsp_iv_counter), std::begin(iv));
      iv[10] = 'H';  // Host originated
      iv[11] = 'R';  // RTSP

      // Allocate the message with an empty header and reserved space for the payload
      auto message_length = serialized_len + original_payload_length;
      std::vector<uint8_t> message(sizeof(encrypted_rtsp_header_t));
      message.reserve(message.size() + message_length);

      // Copy the complete plaintext into the message
      std::copy_n(raw_resp.get(), serialized_len, std::back_inserter(message));
      std::copy_n(payload, original_payload_length, std::back_inserter(message));

      // Initialize the message header
      auto header = (encrypted_rtsp_header_t *) message.data();
      header->typeAndLength = util::endian::big<std::uint32_t>(encrypted_rtsp_header_t::ENCRYPTED_MESSAGE_TYPE_BIT + message_length);
      header->sequenceNumber = util::endian::big<std::uint32_t>(session.rtsp_iv_counter);

      // Encrypt the RTSP message in place
      session.rtsp_cipher->encrypt(std::string_view {(const char *) header->payload(), (std::size_t) message_length}, header->tag.data(), &iv);

      // Send the full encrypted message
      send(sock, std::string_view {(char *) message.data(), message.size()});
    } else {
      std::string_view tmp_resp {raw_resp.get(), (size_t) serialized_len};

      // Send the plaintext RTSP message header
      if (send(sock, tmp_resp)) {
        return;
      }

      // Send the plaintext RTSP message payload (if present)
      send(sock, std::string_view {payload, (std::size_t) original_payload_length});
    }
  }

  /**
   * @brief Send the protocol response for the current request.
   */
  void respond(tcp::socket &sock, launch_session_t &session, POPTION_ITEM options, int statuscode, const char *status_msg, int seqn, const std::string_view &payload) {
    msg_t resp {new msg_t::element_type};
    createRtspResponse(resp.get(), nullptr, 0, const_cast<char *>("RTSP/1.0"), statuscode, const_cast<char *>(status_msg), seqn, options, const_cast<char *>(payload.data()), (int) payload.size());

    respond(sock, session, resp);
  }

  /**
   * @brief Handle an unsupported RTSP command.
   */
  void cmd_not_found(tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    respond(sock, session, nullptr, 404, "NOT FOUND", req->sequenceNumber, {});
  }

  /**
   * @brief Handle RTSP OPTIONS requests.
   * @details The common command signature supplies a server pointer that this handler does not need.
   *
   * @param sock Socket used to read or write the protocol message.
   * @param session Active streaming or pairing session for the request.
   * @param req Parsed RTSP request being handled.
   */
  void cmd_option(rtsp_server_t *, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    respond(sock, session, &option, 200, "OK", req->sequenceNumber, {});
  }

  /**
   * @brief Handle RTSP DESCRIBE requests.
   * @details The common command signature supplies a server pointer that this handler does not need.
   *
   * @param sock Client socket.
   * @param session Launch session.
   * @param req RTSP request message.
   */
  void cmd_describe(rtsp_server_t *, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    std::stringstream ss;

    // Tell the client about our supported features
    ss << "a=x-ss-general.featureFlags:" << (uint32_t) platf::get_capabilities() << std::endl;

    // Always request new control stream encryption if the client supports it
    uint32_t encryption_flags_supported = SS_ENC_CONTROL_V2 | SS_ENC_AUDIO;
    uint32_t encryption_flags_requested = SS_ENC_CONTROL_V2;

    // Determine the encryption desired for this remote endpoint
    if (auto encryption_mode = net::encryption_mode_for_address(sock.remote_endpoint().address()); encryption_mode != config::ENCRYPTION_MODE_NEVER) {
      // Advertise support for video encryption if it's not disabled
      encryption_flags_supported |= SS_ENC_VIDEO;

      // If it's mandatory, also request it to enable use if the client
      // didn't explicitly opt in, but it otherwise has support.
      if (encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
        encryption_flags_requested |= SS_ENC_VIDEO | SS_ENC_AUDIO;
      }
    }

    // Report supported and required encryption flags
    ss << "a=x-ss-general.encryptionSupported:" << encryption_flags_supported << std::endl;
    ss << "a=x-ss-general.encryptionRequested:" << encryption_flags_requested << std::endl;

    if (video::last_encoder_probe_supported_ref_frames_invalidation) {
      ss << "a=x-nv-video[0].refPicInvalidation:1"sv << std::endl;
    }

    if (video::active_hevc_mode != 1) {
      ss << "sprop-parameter-sets=AAAAAU"sv << std::endl;
    }

    if (video::active_av1_mode != 1) {
      ss << "a=rtpmap:98 AV1/90000"sv << std::endl;
    }

    if (!session.surround_params.empty()) {
      // If we have our own surround parameters, advertise them twice first
      ss << "a=fmtp:97 surround-params="sv << session.surround_params << std::endl;
      ss << "a=fmtp:97 surround-params="sv << session.surround_params << std::endl;
    }

    for (int x = 0; x < audio::MAX_STREAM_CONFIG; ++x) {
      auto &stream_config = audio::stream_configs[x];
      std::array<std::uint8_t, platf::speaker::MAX_SPEAKERS> mapping;

      auto mapping_p = stream_config.mapping;

      /**
       * GFE advertises incorrect mapping for normal quality configurations,
       * as a result, Moonlight rotates all channels from index '3' to the right
       * To work around this, rotate channels to the left from index '3'
       */
      if (x == audio::SURROUND51 || x == audio::SURROUND71) {
        std::copy_n(mapping_p, stream_config.channelCount, mapping.begin());
        std::rotate(mapping.begin() + 3, mapping.begin() + 4, mapping.begin() + audio::MAX_STREAM_CONFIG);

        mapping_p = mapping.data();
      }

      ss << "a=fmtp:97 surround-params="sv << stream_config.channelCount << stream_config.streams << stream_config.coupledStreams;

      std::for_each_n(mapping_p, stream_config.channelCount, [&ss](std::uint8_t digit) {
        ss << (char) (digit + '0');
      });

      ss << std::endl;
    }

    respond(sock, session, &option, 200, "OK", req->sequenceNumber, ss.str());
  }

  /**
   * @brief Handle RTSP SETUP requests and prepare transport state.
   *
   * @param rtsp_server RTSP server instance handling the request.
   * @param sock Socket used to read or write the protocol message.
   * @param session Active streaming or pairing session for the request.
   * @param req Parsed RTSP request being handled.
   */
  void cmd_setup(rtsp_server_t *rtsp_server, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    std::array<OPTION_ITEM, 4> options {};

    auto &seqn = options[0];
    auto &session_option = options[1];
    auto &port_option = options[2];
    auto &payload_option = options[3];

    seqn.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    seqn.content = const_cast<char *>(seqn_str.c_str());

    std::string_view target {req->message.request.target};
    auto begin = std::ranges::find(target, '=') + 1;
    auto end = std::find(begin, std::end(target), '/');
    std::string_view type {begin, (size_t) std::distance(begin, end)};

    std::uint16_t port;
    if (type == "audio"sv) {
      port = net::map_port(stream::AUDIO_STREAM_PORT);
    } else if (type == "video"sv) {
      port = net::map_port(stream::VIDEO_STREAM_PORT);
    } else if (type == "control"sv) {
      port = net::map_port(stream::CONTROL_PORT);
    } else {
      cmd_not_found(sock, session, std::move(req));

      return;
    }

    seqn.next = &session_option;

    session_option.option = const_cast<char *>("Session");
    session_option.content = const_cast<char *>("DEADBEEFCAFE;timeout = 90");

    session_option.next = &port_option;

    // Moonlight merely requires 'server_port=<port>'
    auto port_value = std::format("server_port={}", static_cast<int>(port));

    port_option.option = const_cast<char *>("Transport");
    port_option.content = port_value.data();

    // Send identifiers that will be echoed in the other connections
    auto connect_data = std::to_string(session.control_connect_data);
    if (type == "control"sv) {
      payload_option.option = const_cast<char *>("X-SS-Connect-Data");
      payload_option.content = connect_data.data();
    } else {
      payload_option.option = const_cast<char *>("X-SS-Ping-Payload");
      payload_option.content = session.av_ping_payload.data();
    }

    port_option.next = &payload_option;

    respond(sock, session, &seqn, 200, "OK", req->sequenceNumber, {});
  }

  /**
   * @brief Handle RTSP ANNOUNCE requests from the client.
   *
   * @param rtsp_server RTSP server instance handling the request.
   * @param sock Socket used to read or write the protocol message.
   * @param session Active streaming or pairing session for the request.
   * @param req Parsed RTSP request being handled.
   */
  void cmd_announce(rtsp_server_t *rtsp_server, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    std::string_view payload {req->payload, (size_t) req->payloadLength};

    std::vector<std::string_view> lines;

    auto whitespace = [](char ch) {
      return ch == '\n' || ch == '\r';
    };

    {
      auto pos = std::begin(payload);
      auto begin = pos;
      while (pos != std::end(payload)) {
        if (whitespace(*pos++)) {
          lines.emplace_back(begin, pos - begin - 1);

          while (pos != std::end(payload) && whitespace(*pos)) {
            ++pos;
          }
          begin = pos;
        }
      }
    }

    std::string_view client;
    std::unordered_map<std::string_view, std::string_view> args;

    for (auto line : lines) {
      auto type = line.substr(0, 2);
      if (type == "s="sv) {
        client = line.substr(2);
      } else if (type == "a=") {
        auto pos = line.find(':');

        auto name = line.substr(2, pos - 2);
        auto val = line.substr(pos + 1);

        if (val[val.size() - 1] == ' ') {
          val = val.substr(0, val.size() - 1);
        }
        args.try_emplace(name, val);
      }
    }

    // Initialize any omitted parameters to defaults
    args.try_emplace("x-nv-video[0].encoderCscMode"sv, "0"sv);
    args.try_emplace("x-nv-vqos[0].bitStreamFormat"sv, "0"sv);
    args.try_emplace("x-nv-video[0].dynamicRangeMode"sv, "0"sv);
    args.try_emplace("x-nv-aqos.packetDuration"sv, "5"sv);
    args.try_emplace("x-nv-general.useReliableUdp"sv, "1"sv);
    args.try_emplace("x-nv-vqos[0].fec.minRequiredFecPackets"sv, "0"sv);
    args.try_emplace("x-nv-general.featureFlags"sv, "135"sv);
    args.try_emplace("x-ml-general.featureFlags"sv, "0"sv);
    args.try_emplace("x-nv-vqos[0].qosTrafficType"sv, "5"sv);
    args.try_emplace("x-nv-aqos.qosTrafficType"sv, "4"sv);
    args.try_emplace("x-ml-video.configuredBitrateKbps"sv, "0"sv);
    args.try_emplace("x-ss-general.encryptionEnabled"sv, "0"sv);
    args.try_emplace("x-ss-video[0].chromaSamplingType"sv, "0"sv);
    args.try_emplace("x-ss-video[0].intraRefresh"sv, "0"sv);
    args.try_emplace("x-nv-video[0].clientRefreshRateX100"sv, "0"sv);

    stream::config_t config;

    std::int64_t configuredBitrateKbps;
    config.audio.flags[audio::config_t::HOST_AUDIO] = session.host_audio;
    try {
      config.audio.channels = (int) util::from_view(args.at("x-nv-audio.surround.numChannels"sv));
      config.audio.mask = (int) util::from_view(args.at("x-nv-audio.surround.channelMask"sv));
      config.audio.packetDuration = (int) util::from_view(args.at("x-nv-aqos.packetDuration"sv));

      config.audio.flags[audio::config_t::HIGH_QUALITY] =
        util::from_view(args.at("x-nv-audio.surround.AudioQuality"sv));

      config.controlProtocolType = (int) util::from_view(args.at("x-nv-general.useReliableUdp"sv));
      config.packetsize = (int) util::from_view(args.at("x-nv-video[0].packetSize"sv));
      config.minRequiredFecPackets = (int) util::from_view(args.at("x-nv-vqos[0].fec.minRequiredFecPackets"sv));
      config.mlFeatureFlags = (int) util::from_view(args.at("x-ml-general.featureFlags"sv));
      config.audioQosType = (int) util::from_view(args.at("x-nv-aqos.qosTrafficType"sv));
      config.videoQosType = (int) util::from_view(args.at("x-nv-vqos[0].qosTrafficType"sv));
      config.encryptionFlagsEnabled = (uint32_t) util::from_view(args.at("x-ss-general.encryptionEnabled"sv));

      // Legacy clients use nvFeatureFlags to indicate support for audio encryption
      if (util::from_view(args.at("x-nv-general.featureFlags"sv)) & 0x20) {
        config.encryptionFlagsEnabled |= SS_ENC_AUDIO;
      }

      // Limit the packetsize to avoid fragmentation with clients that cannot configure this value
      if (config::stream.packetsize && config::stream.packetsize < config.packetsize) {
        if (config::stream.packetsize < config::PACKETSIZE_MIN || config::stream.packetsize > config::PACKETSIZE_MAX) {
          BOOST_LOG(warning) << "packetsize range: ["sv << config::PACKETSIZE_MIN << "-"sv << config::PACKETSIZE_MAX
                             << "] invalid value: "sv << config::stream.packetsize;
        } else {
          if (config::stream.packetsize < config::PACKETSIZE_SMALL) {
            BOOST_LOG(info) << "packetsize is small < "sv << config::PACKETSIZE_SMALL << " bytes, reduce bitrate if the stream breaks"sv;
          } else if (config::stream.packetsize > config::PACKETSIZE_LARGE) {
            BOOST_LOG(info) << "packetsize is large > "sv << config::PACKETSIZE_LARGE << " bytes, jumbo frames may be used"sv;
          }

          BOOST_LOG(info) << "packetsize limit: "sv << config.packetsize << " -> "sv << config::stream.packetsize << " bytes"sv;
          config.packetsize = config::stream.packetsize;
        }
      }

      config.monitor.height = (int) util::from_view(args.at("x-nv-video[0].clientViewportHt"sv));
      config.monitor.width = (int) util::from_view(args.at("x-nv-video[0].clientViewportWd"sv));
      config.monitor.framerate = (int) util::from_view(args.at("x-nv-video[0].maxFPS"sv));
      config.monitor.framerateX100 = (int) util::from_view(args.at("x-nv-video[0].clientRefreshRateX100"sv));
      // Validate framerateX100 against framerate. Some clients (e.g. Moonlight Android) send the
      // client display's refresh rate as clientRefreshRateX100, which may differ from the requested
      // streaming framerate. Discard framerateX100 if the derived fps is not within 1% of framerate.
      if (config.monitor.framerateX100 > 0) {
        double fps_strict = config.monitor.framerateX100 / 100.0;
        double ratio = fps_strict / config.monitor.framerate;
        if (ratio < 0.99 || ratio > 1.01) {
          config.monitor.framerateX100 = 0;
        }
      }
      config.monitor.bitrate = (int) util::from_view(args.at("x-nv-vqos[0].bw.maximumBitrateKbps"sv));
      config.monitor.slicesPerFrame = (int) util::from_view(args.at("x-nv-video[0].videoEncoderSlicesPerFrame"sv));
      config.monitor.numRefFrames = (int) util::from_view(args.at("x-nv-video[0].maxNumReferenceFrames"sv));
      config.monitor.encoderCscMode = (int) util::from_view(args.at("x-nv-video[0].encoderCscMode"sv));
      config.monitor.videoFormat = (int) util::from_view(args.at("x-nv-vqos[0].bitStreamFormat"sv));
      config.monitor.dynamicRange = (int) util::from_view(args.at("x-nv-video[0].dynamicRangeMode"sv));
      config.monitor.chromaSamplingType = (int) util::from_view(args.at("x-ss-video[0].chromaSamplingType"sv));
      config.monitor.enableIntraRefresh = (int) util::from_view(args.at("x-ss-video[0].intraRefresh"sv));

      configuredBitrateKbps = util::from_view(args.at("x-ml-video.configuredBitrateKbps"sv));
    } catch (std::out_of_range &) {
      respond(sock, session, &option, 400, "BAD REQUEST", req->sequenceNumber, {});
      return;
    }

    // When using stereo audio, the audio quality is (strangely) indicated by whether the Host field
    // in the RTSP message matches a local interface's IP address. Fortunately, Moonlight always sends
    // 0.0.0.0 when it wants low quality, so it is easy to check without enumerating interfaces.
    if (config.audio.channels == 2) {
      for (auto request_option = req->options; request_option != nullptr; request_option = request_option->next) {
        if ("Host"sv == request_option->option) {
          std::string_view content {request_option->content};
          BOOST_LOG(debug) << "Found Host: "sv << content;
          config.audio.flags[audio::config_t::HIGH_QUALITY] = !content.contains("0.0.0.0"sv);
        }
      }
    } else if (session.surround_params.length() > 3) {
      // Channels
      std::uint8_t c = session.surround_params[0] - '0';
      // Streams
      std::uint8_t n = session.surround_params[1] - '0';
      // Coupled streams
      std::uint8_t m = session.surround_params[2] - '0';
      auto valid = false;
      if ((c == 6 || c == 8) && c == config.audio.channels && n + m == c && session.surround_params.length() == c + 3) {
        config.audio.customStreamParams.channelCount = c;
        config.audio.customStreamParams.streams = n;
        config.audio.customStreamParams.coupledStreams = m;
        valid = true;
        for (std::uint8_t i = 0; i < c; i++) {
          config.audio.customStreamParams.mapping[i] = session.surround_params[i + 3] - '0';
          if (config.audio.customStreamParams.mapping[i] >= c) {
            valid = false;
            break;
          }
        }
      }
      config.audio.flags[audio::config_t::CUSTOM_SURROUND_PARAMS] = valid;
    }
    if (session.continuous_audio) {
      BOOST_LOG(info) << "Client requested continuous audio"sv;
      config.audio.flags[audio::config_t::CONTINUOUS_AUDIO] = true;
    }

    // If the client sent a configured bitrate, we will choose the actual bitrate ourselves
    // by using FEC percentage and audio quality settings. If the calculated bitrate ends up
    // too low, we'll allow it to exceed the limits rather than reducing the encoding bitrate
    // down to nearly nothing.
    if (configuredBitrateKbps) {
      BOOST_LOG(debug) << "Client configured bitrate is "sv << configuredBitrateKbps << " Kbps"sv;

      // If the FEC percentage isn't too high, adjust the configured bitrate to ensure video
      // traffic doesn't exceed the user's selected bitrate when the FEC shards are included.
      if (config::stream.fec_percentage <= 80) {
        configuredBitrateKbps /= 100.f / (100 - config::stream.fec_percentage);
      }

      // Adjust the bitrate to account for audio traffic bandwidth usage (capped at 20% reduction).
      // The bitrate per channel is 256 Kbps for high quality mode and 96 Kbps for normal quality.
      auto audioBitrateAdjustment = (config.audio.flags[audio::config_t::HIGH_QUALITY] ? 256 : 96) * config.audio.channels;
      configuredBitrateKbps -= std::min((std::int64_t) audioBitrateAdjustment, configuredBitrateKbps / 5);

      // Reduce it by another 500Kbps to account for A/V packet overhead and control data
      // traffic (capped at 10% reduction).
      configuredBitrateKbps -= std::min((std::int64_t) 500, configuredBitrateKbps / 10);

      BOOST_LOG(debug) << "Final adjusted video encoding bitrate is "sv << configuredBitrateKbps << " Kbps"sv;
      config.monitor.bitrate = (int) configuredBitrateKbps;
    }

    if (config.monitor.videoFormat == 1 && video::active_hevc_mode == 1) {
      BOOST_LOG(warning) << "HEVC is disabled, yet the client requested HEVC"sv;

      respond(sock, session, &option, 400, "BAD REQUEST", req->sequenceNumber, {});
      return;
    }

    if (config.monitor.videoFormat == 2 && video::active_av1_mode == 1) {
      BOOST_LOG(warning) << "AV1 is disabled, yet the client requested AV1"sv;

      respond(sock, session, &option, 400, "BAD REQUEST", req->sequenceNumber, {});
      return;
    }

    // Check that any required encryption is enabled
    auto encryption_mode = net::encryption_mode_for_address(sock.remote_endpoint().address());
    if (encryption_mode == config::ENCRYPTION_MODE_MANDATORY && (config.encryptionFlagsEnabled & (SS_ENC_VIDEO | SS_ENC_AUDIO)) != (SS_ENC_VIDEO | SS_ENC_AUDIO)) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      respond(sock, session, &option, 403, "Forbidden", req->sequenceNumber, {});
      return;
    }

    if (!rtsp_server->claim(session)) {
      respond(sock, session, &option, 403, "Forbidden", req->sequenceNumber, {});
      return;
    }

    auto stream_session = stream::session::alloc(config, session);
    rtsp_server->insert(stream_session);

    if (stream::session::start(*stream_session, sock.remote_endpoint().address().to_string())) {
      BOOST_LOG(error) << "Failed to start a streaming session"sv;

      rtsp_server->remove(stream_session);
      session.cancelled = true;
      rtsp_server->session_clear(session.id);
      respond(sock, session, &option, 500, "Internal Server Error", req->sequenceNumber, {});
      return;
    }

    respond(sock, session, &option, 200, "OK", req->sequenceNumber, {});
  }

  /**
   * @brief Handle RTSP PLAY requests and start streaming.
   * @details The common command signature supplies a server pointer that this handler does not need.
   *
   * @param sock Socket used to read or write the protocol message.
   * @param session Active streaming or pairing session for the request.
   * @param req Parsed RTSP request being handled.
   */
  void cmd_play(rtsp_server_t *, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    respond(sock, session, &option, 200, "OK", req->sequenceNumber, {});
  }

  void start() {
    platf::set_thread_name("rtsp");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    server.map("OPTIONS"sv, &cmd_option);
    server.map("DESCRIBE"sv, &cmd_describe);
    server.map("SETUP"sv, &cmd_setup);
    server.map("ANNOUNCE"sv, &cmd_announce);
    server.map("PLAY"sv, &cmd_play);

    boost::system::error_code ec;
    if (server.bind(net::af_from_enum_string(config::sunshine.address_family), net::map_port(rtsp_stream::RTSP_SETUP_PORT), ec)) {
      BOOST_LOG(fatal) << "Couldn't bind RTSP server to port ["sv << net::map_port(rtsp_stream::RTSP_SETUP_PORT) << "], " << ec.message();
      shutdown_event->raise(true);

      return;
    }

    server.begin();
    std::jthread rtsp_thread {[] {
      platf::set_thread_name("rtsp::handler");
      auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);

      while (!server.stopped) {
        server.iterate();

        if (broadcast_shutdown_event->peek()) {
          server.clear();
        } else {
          // cleanup all stopped sessions
          server.clear(false);
        }
      }

      server.clear();
    }};

    // Wait for shutdown
    shutdown_event->view();

    // Stop the server and join the server thread
    server.stop();
    rtsp_thread.join();
  }

  /**
   * @brief Write msg details to the log.
   */
  void print_msg(PRTSP_MESSAGE msg) {
    std::string_view type = msg->type == TYPE_RESPONSE ? "RESPONSE"sv : "REQUEST"sv;

    std::string_view payload {msg->payload, (size_t) msg->payloadLength};
    std::string_view protocol {msg->protocol};
    auto seqnm = msg->sequenceNumber;
    std::string_view messageBuffer {msg->messageBuffer};

    BOOST_LOG(debug) << "type ["sv << type << ']';
    BOOST_LOG(debug) << "sequence number ["sv << seqnm << ']';
    BOOST_LOG(debug) << "protocol :: "sv << protocol;
    BOOST_LOG(debug) << "payload :: "sv << payload;

    if (msg->type == TYPE_RESPONSE) {
      auto &resp = msg->message.response;

      auto statuscode = resp.statusCode;
      std::string_view status {resp.statusString};

      BOOST_LOG(debug) << "statuscode :: "sv << statuscode;
      BOOST_LOG(debug) << "status :: "sv << status;
    } else {
      auto &req = msg->message.request;

      std::string_view command {req.command};
      std::string_view target {req.target};

      BOOST_LOG(debug) << "command :: "sv << command;
      BOOST_LOG(debug) << "target :: "sv << target;
    }

    for (auto option = msg->options; option != nullptr; option = option->next) {
      std::string_view content {option->content};
      std::string_view name {option->option};

      BOOST_LOG(debug) << name << " :: "sv << content;
    }

    BOOST_LOG(debug) << "---Begin MessageBuffer---"sv << std::endl
                     << messageBuffer << std::endl
                     << "---End MessageBuffer---"sv << std::endl;
  }
}  // namespace rtsp_stream
