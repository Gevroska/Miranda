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
#include <deque>
#include <format>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

// lib includes
#include <boost/asio.hpp>
#include <boost/bind.hpp>

// local includes
#include "config.h"
#include "display_device.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "process.h"
#include "rtsp.h"
#include "stream.h"
#include "sync.h"
#include "video.h"

namespace asio = boost::asio;

using asio::ip::tcp;
using asio::ip::udp;

using namespace std::literals;

namespace rtsp_stream {
  thread_local bool process_worker_context = false;  ///< The process lane never waits for this owner's executor.

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
    rtsp_server_t() {
      source_emitter->owner = this;
    }
    /** @brief Owner-only source phases; in-flight work always drains before rollback or replacement. */
    enum class source_phase_e {
      preparing,
      applying,
      probing,
      executing,
      ready,
      retained,
      checking,
      draining,
      restoring,
      cleanup_pending
    };
    enum class source_work_e {
      claim,
      unclaim,
      release_claim,
      liveness,
      begin,
      validate,
      probe,
      execute,
      rollback,
      restore,
      cleanup,
      complete,
      progress
    };

    struct admission_t {
      std::shared_ptr<launch_session_t> launch;  ///< Strong token retention is explicit prepared accounting.
      std::shared_ptr<std::promise<int>> result;  ///< Awaited by HTTPS outside this owner.
      std::function<int()> execute;  ///< Copied-input application operation.
      std::function<void()> rollback;  ///< Copied-input rollback after worker completion.
      bool answered = false;  ///< Cancellation answers immediately even while an operation drains.
      bool executed = false;  ///< Failed/partial application execution also requires rollback.
    };

    struct preflight_t {
      std::shared_ptr<launch_session_t> launch;
      std::function<std::pair<int, std::shared_ptr<const display_policy::source_request_t>>(const source_snapshot_t &)> plan;
      std::function<int()> execute;
      std::function<void()> rollback;
      std::shared_ptr<std::promise<int>> result;
      std::future<void> work;
      bool answered = false;
    };

    struct source_work_result_t {
      std::uint64_t generation;  ///< Source fence copied before dispatch.
      std::uint64_t revision;  ///< Operation fence copied before dispatch.
      source_work_e kind;  ///< Completion continuation.
      int status;  ///< Explicit operation result, including caught exceptions.
      bool entered = false;  ///< False proves the operation was rejected before any backend/process effect.
    };

    struct source_record_t {
      std::uint64_t generation {};  ///< Monotonic source identity.
      std::uint64_t revision {};  ///< Monotonic operation identity within this source.
      std::uint64_t epoch {};  ///< Fresh or retained-reconnect capture incarnation.
      std::uint64_t application_instance {};  ///< Genuine process lifetime, distinct from catalog UUID.
      int appid {};  ///< Selected protocol application.
      std::string app_uuid;  ///< Selected immutable application identity.
      std::shared_ptr<const display_policy::source_request_t> request;  ///< Frozen P3a policy.
      std::shared_ptr<const capture_source_t> metadata;  ///< Never owns a capture pin.
      std::shared_ptr<source_backend_t> backend;  ///< Holds lease and recovery proof through cleanup failures.
      source_phase_e phase = source_phase_e::preparing;
      std::unordered_map<std::uint32_t, std::shared_ptr<launch_session_t>> prepared;  ///< Released by explicit owner transitions.
      std::unordered_map<std::uint32_t, std::shared_ptr<launch_session_t>> media;  ///< Removed only after stop/join and slot erasure.
      std::set<std::uint64_t> capture;  ///< Actual-resource pins, never inferred from stream joins.
      std::shared_ptr<admission_t> admission;  ///< Sole staged first/reconnect admission.
      std::optional<std::future<source_work_result_t>> work;  ///< Owner polls ready state; it never waits on backend work.
      bool failed = false;  ///< Sticky heartbeat/source failure prohibits retention or joins.
      bool cleanup_blocked = false;  ///< A failed native cleanup requires an explicit retry, not a polling loop.
      bool completion_pending = false;  ///< Retirement succeeded but recovery completion still needs retry.
      bool backend_unentered = false;
      bool retire_requested = false;  ///< Explicit Stop, shutdown and heartbeat failure never turn back into retention.
      std::chrono::steady_clock::time_point next_liveness {};
      std::chrono::steady_clock::time_point next_backend_progress {};  ///< Bound native readback polling without blocking the owner.
      std::vector<std::shared_future<void>> stops;  ///< Restoration waits for deferred Stop work.
      std::shared_ptr<std::optional<proc::source_claim_t>> process_claim = std::make_shared<std::optional<proc::source_claim_t>>();  ///< Written only by the drained operation worker.
    };

    /** @brief A capture destructor may only post while this owner's event emitter is alive. */
    struct source_emitter_t {
      std::mutex mutex;
      rtsp_server_t *owner = nullptr;  ///< Protected pointer; no resource owns the source record.
    };

    ~rtsp_server_t() {
      {
        std::lock_guard lock {source_emitter->mutex};
        source_emitter->owner = nullptr;
      }
      clear();
    }

    /** @brief Return copied source state at the admission ordering point. */
    source_snapshot_t snapshot() const {
      if (!source) {
        return {.busy = recovery_work.has_value() || recovery_retry_requested, .recovery_pending = static_cast<bool>(recovery_backend)};
      }
      return {true, source->phase != source_phase_e::ready && source->phase != source_phase_e::retained, source->phase == source_phase_e::retained, source->appid, source->app_uuid, source->application_instance, source->request, source->metadata, source->prepared.size(), source->media.size(), source->capture.size(), source->cleanup_blocked};
    }

    /** @brief All blocking source operations have copied ownership and a tagged completion. */
    void dispatch_source(source_work_e kind, std::function<int()> operation) {
      const auto generation = source->generation;
      const auto revision = ++source->revision;
      const bool guarded = kind == source_work_e::claim || kind == source_work_e::begin || kind == source_work_e::validate || kind == source_work_e::probe || kind == source_work_e::execute;
      const auto launch = guarded && source->admission ? source->admission->launch : nullptr;
      const auto deadline = raised_timer.expiry();
      auto task = [this, generation, revision, kind, launch, deadline, operation = std::move(operation)]() {
        int status = 503;
        bool entered = false;
        try {
#ifdef SUNSHINE_TESTS
          if (kind == source_work_e::begin)
            notify_owner("source_before_entry");
          if (kind == source_work_e::validate)
            notify_owner("source_validate_before_entry");
#endif
          if (launch) {
            const auto client = nvhttp::current_client(launch->client_uuid, launch->client_cert);
            if (!client || client->revision != launch->authorization_revision || !permissions::allows(client->permissions, permissions::view) || launch->source_cancel->load()) {
              return source_work_result_t {generation, revision, kind, 403};
            }
            if (std::chrono::steady_clock::now() >= deadline) {
              return source_work_result_t {generation, revision, kind, 503};
            }
          }
          entered = true;
          status = operation();
          if (process_worker_context)
            proc::proc.refresh_published();
        } catch (...) {
          BOOST_LOG(error) << "Managed source operation failed";
        }
        return source_work_result_t {generation, revision, kind, status, entered};
      };
      if (kind == source_work_e::claim || kind == source_work_e::unclaim || kind == source_work_e::release_claim || kind == source_work_e::liveness || kind == source_work_e::execute || kind == source_work_e::rollback) {
        auto packaged = std::make_shared<std::packaged_task<source_work_result_t()>>(std::move(task));
        source->work.emplace(packaged->get_future());
        queue_process([packaged]() {
          (*packaged)();
        });
      } else {
        source->work.emplace(std::async(std::launch::async, std::move(task)));
      }
      arm_source_timer();
    }

    /** @brief Ensure retained-app exit and backend progress are observed even without media workers. */
    void arm_source_timer() {
      if (source_timer_armed || (!source && !preflight && !process_work && process_jobs.empty() && !recovery_work && (!recovery_backend || stopping))) {
        return;
      }
      source_timer_armed = true;
      source_timer.expires_after(10ms);
      source_timer.async_wait([this](const boost::system::error_code &error) {
        source_timer_armed = false;
        if (!error) {
          poll_process();
          poll_preflight();
          poll_source();
          poll_retired_recovery();
          if (source || preflight || process_work || !process_jobs.empty() || (recovery_backend && !stopping) || recovery_work) {
            arm_source_timer();
          }
        }
      });
    }

    /** @brief Finish an HTTPS result exactly once; cancellation must not await native cancellation. */
    void answer_admission(int status) {
      if (source && source->admission && !source->admission->answered) {
        source->admission->answered = true;
        source->admission->result->set_value(status);
      }
    }

    /**
     * @brief Idempotent release shared by timeout, revocation, handshake clear and slot removal.
     * @param media_release True only after successful media stop/join and slot erasure.
     */
    void release_source_pin(launch_session_t &launch, bool media_release = false, int status = 403) {
      if (preflight && preflight->launch->id == launch.id && !preflight->answered) {
        preflight->answered = true;
        launch.cancelled = true;
        launch.source_cancel->store(true);
        preflight->result->set_value(status);
      }
      if (!source || launch.source_generation != source->generation) {
        return;
      }
      if (launch.source_media) {
        if (!media_release) {
          return;
        }
        source->media.erase(launch.id);
        launch.source_media = false;
      } else {
        source->prepared.erase(launch.id);
      }
      if (source->admission && source->admission->launch->id == launch.id) {
        launch.cancelled = true;
        if (launch.source_cancel) {
          launch.source_cancel->store(true);
        }
        answer_admission(status);
      }
      arm_source_timer();
    }

    /** @brief A start claim is not media ownership until all start operations succeed. */
    bool source_started(launch_session_t &launch, bool success) {
      if (!launch.source_generation) {
        return success;
      }
      if (!source || source->generation != launch.source_generation) {
        return false;
      }
      if (!success || !authorized(launch) || !source->prepared.contains(launch.id)) {
        launch.cancelled = true;
        release_source_pin(launch);
        return false;
      }
      const auto token = source->prepared.at(launch.id);
      source->prepared.erase(launch.id);
      source->media.emplace(launch.id, token);
      launch.source_media = true;
      return true;
    }

    /** @brief Route every slot erasure through the same explicit pin-release transition. */
    void release_source_slot(std::uint32_t id) {
      if (!source) {
        return;
      }
      if (const auto it = source->media.find(id); it != source->media.end()) {
        const auto token = it->second;
        release_source_pin(*token, true);
      }
    }

    /** @brief Reject stale, incomplete or differently mapped backend descriptors. */
    bool valid_source(const std::shared_ptr<const capture_source_t> &metadata) const {
      if (!source || !metadata) {
        return false;
      }
      const auto &request = *source->request;
      if (metadata->generation != source->generation || metadata->epoch != source->epoch || metadata->requested_hdr != request.requested_hdr || metadata->selector.empty() || metadata->device_id.empty() || !metadata->width || !metadata->height || !metadata->rate_numerator || !metadata->rate_denominator) {
        return false;
      }
#ifdef _WIN32
      if (!metadata->windows_output || metadata->windows_output->monitor_path_utf8.empty()) {
        return false;
      }
#endif
      if (!request.virtual_display) {
        return metadata->kind == capture_source_t::kind_e::native && metadata->monitor_uuid.empty();
      }
      const auto canonical = display_policy::canonical_client_uuid(metadata->monitor_uuid);
      return metadata->kind == capture_source_t::kind_e::owned_virtual && canonical && *canonical == metadata->monitor_uuid && metadata->monitor_uuid == request.monitor_uuid &&
             metadata->width == request.mode.resolution.width && metadata->height == request.mode.resolution.height &&
             static_cast<std::uint64_t>(metadata->rate_numerator) * request.mode.refresh_rate.denominator == static_cast<std::uint64_t>(request.mode.refresh_rate.numerator) * metadata->rate_denominator &&
             (!request.requested_hdr || metadata->observed_hdr == request.requested_hdr);
    }

    /** @brief Cancel admissions now; rollback and restoration wait for the current operation to drain. */
    void fail_source(int status) {
      if (!source) {
        return;
      }
      source->failed = true;
      source->phase = source_phase_e::draining;
      if (source->admission) {
        auto token = source->admission->launch;
        token->cancelled = true;
        release_source_pin(*token, false, status);
      }
      arm_source_timer();
    }

    /** @brief Start a first-source or retained-source admission while holding its reservation. */
    void prepare_source(std::shared_ptr<launch_session_t> launch, std::function<std::pair<int, std::shared_ptr<const display_policy::source_request_t>>(const source_snapshot_t &)> plan, std::function<int()> execute, std::function<void()> rollback, std::shared_ptr<std::promise<int>> result, std::function<void()> refresh) {
      if (!backend_factory || !running || stopping || !authorized(*launch, false)) {
        result->set_value(backend_factory && !stopping ? 403 : 503);
        return;
      }
      if (preflight || recovery_work || recovery_retry_requested || (source && snapshot().busy) || !session_raise(launch)) {
        result->set_value(503);
        return;
      }
      if (source) {
        launch->source_generation = source->generation;
        source->prepared.emplace(launch->id, launch);
        if (source->phase == source_phase_e::retained && !refresh) {
          refresh = []() {
            proc::proc.running();
            proc::proc.refresh_published();
          };
        }
      }
      if (refresh) {
        launch->source_cancel = std::make_shared<std::atomic_bool>(false);
        auto task = std::make_shared<std::packaged_task<void()>>(std::move(refresh));
        preflight = std::make_shared<preflight_t>(preflight_t {launch, std::move(plan), std::move(execute), std::move(rollback), result, task->get_future()});
        queue_process([task]() {
          (*task)();
        });
        arm_source_timer();
        return;
      }
      complete_preflight(std::move(launch), std::move(plan), std::move(execute), std::move(rollback), std::move(result));
    }

    void poll_preflight() {
      if (!preflight) {
        return;
      }
      if (!authorized(*preflight->launch, false)) {
        release_source_pin(*preflight->launch, false, 403);
      }
      if (preflight->work.wait_for(0ms) != std::future_status::ready) {
        return;
      }
      const auto current = std::exchange(preflight, nullptr);
      int status = 200;
      try {
        current->work.get();
      } catch (...) {
        status = 503;
      }
      if (current->answered || status != 200 || !authorized(*current->launch, false)) {
        const auto permitted = authorized(*current->launch, false);
        current->launch->cancelled = true;
        session_clear(current->launch->id);
        if (!current->answered) {
          current->result->set_value(permitted ? status : 403);
        }
      } else {
        complete_preflight(current->launch, std::move(current->plan), std::move(current->execute), std::move(current->rollback), current->result);
      }
      finish_stop();
    }

    void complete_preflight(std::shared_ptr<launch_session_t> launch, std::function<std::pair<int, std::shared_ptr<const display_policy::source_request_t>>(const source_snapshot_t &)> plan, std::function<int()> execute, std::function<void()> rollback, std::shared_ptr<std::promise<int>> result) {
      std::pair<int, std::shared_ptr<const display_policy::source_request_t>> planned;
      try {
        planned = plan(snapshot());
      } catch (...) {
        planned.first = 503;
      }
      const auto permitted = authorized(*launch, false);
      if (planned.first != 200 || !planned.second || !permitted) {
        launch->cancelled = true;
        session_clear(launch->id);
        result->set_value(!permitted ? 403 : planned.first == 200 ? 503 :
                                                                    planned.first);
        return;
      }
      if (source && source->phase == source_phase_e::retained) {
        const auto observed = proc::proc.published_snapshot()->lifetime;
        if (!observed.tracked || observed.instance != source->application_instance || observed.app_uuid != source->app_uuid) {
          launch->cancelled = true;
          session_clear(launch->id);
          source->phase = source_phase_e::draining;
          result->set_value(503);
          arm_source_timer();
          return;
        }
      }
      if (source && (source->appid != launch->appid || source->app_uuid != launch->app_uuid)) {
        launch->cancelled = true;
        session_clear(launch->id);
        result->set_value(503);
        return;
      }
      if (source && source->phase == source_phase_e::ready) {
        launch->source_generation = source->generation;
        launch->application_instance = source->application_instance;
        launch->source = source->metadata;
        launch->joining = true;
        source->prepared.emplace(launch->id, launch);
        launch->ready = true;
        result->set_value(200);
        return;
      }
      const bool retained = source.has_value();
      if (!retained) {
        std::shared_ptr<source_backend_t> candidate;
        try {
          candidate = recovery_backend ? recovery_backend : backend_factory();
          if (!candidate || recovery_work || recovery_retry_requested || !candidate->admits(*planned.second) || next_source_generation == UINT64_MAX) {
            candidate.reset();
          }
        } catch (...) {
          candidate.reset();
        }
        if (!candidate) {
          launch->cancelled = true;
          session_clear(launch->id);
          result->set_value(503);
          return;
        }
        source.emplace();
        source->generation = ++next_source_generation;
        source->appid = launch->appid;
        source->app_uuid = launch->app_uuid;
        source->request = std::move(planned.second);
        source->backend = std::move(candidate);
        if (!source->backend) {
          source.reset();
          launch->cancelled = true;
          session_clear(launch->id);
          result->set_value(503);
          return;
        }
      }
      source->phase = source_phase_e::preparing;
      source->epoch = ++next_capture_epoch;
      source->admission = std::make_shared<admission_t>(admission_t {launch, result, std::move(execute), std::move(rollback)});
      source->prepared.emplace(launch->id, launch);
      launch->source_generation = source->generation;
      launch->source_cancel = std::make_shared<std::atomic_bool>(false);
      launch->joining = retained;
      const auto backend = source->backend;
      const auto epoch = source->epoch;
      if (retained) {
        const auto metadata = source->metadata;
        dispatch_source(source_work_e::validate, [backend, metadata, epoch]() {
          return backend->validate_retained(*metadata, epoch) ? 200 : 503;
        });
      } else {
        const auto generation = source->generation;
        const auto app_uuid = source->app_uuid;
        const auto claim = source->process_claim;
        dispatch_source(source_work_e::claim, [generation, app_uuid, claim]() {
          const auto app = proc::proc.lifetime_snapshot();
          if (app.tracked && app.app_uuid == app_uuid) {
            *claim = proc::proc.claim_source(app.instance, generation);
          }
          return 200;
        });
      }
    }

    /** @brief Read the progress clock; test-only virtual time changes no admission deadline. */
    std::chrono::steady_clock::time_point source_progress_now() const {
#ifdef SUNSHINE_TESTS
      if (source_progress_clock) {
        return source_progress_clock();
      }
#endif
      return std::chrono::steady_clock::now();
    }

    /** @brief Continue staged backend work without sleeping or waiting for apply, restore or probe. */
    void poll_source() {
      if (!source) {
        return;
      }
      if (source->admission && !authorized(*source->admission->launch, false)) {
        fail_source(403);
      }
      if (source->metadata && !source->failed) {
        const auto observed = source->backend->status();
        if (observed.generation == source->generation && observed.heartbeat_failed) {
          source->retire_requested = true;
          fail_source(503);
          invalidate_launches();
          clear();
        }
      }
      if (source->work) {
        if (source->work->wait_for(0ms) != std::future_status::ready) {
          return;
        }
        const auto result = source->work->get();
        source->work.reset();
        if (result.generation != source->generation || result.revision != source->revision) {
          fail_source(503);
        } else if (result.kind == source_work_e::validate && !result.entered && !source->retire_requested) {
          const auto launch = source->admission->launch;
          release_source_pin(*launch, false, result.status);
          session_clear(launch->id);
          source->admission.reset();
          source->epoch = source->metadata->epoch;
          source->failed = false;
          source->phase = source_phase_e::retained;
          source->next_liveness = std::chrono::steady_clock::now();
          return;
        } else if (((result.kind == source_work_e::begin || result.kind == source_work_e::claim) && !result.entered) || (result.kind == source_work_e::claim && result.status != 200)) {
          fail_source(result.status);
          source->backend_unentered = true;
          const auto claim = source->process_claim;
          if (*claim) {
            dispatch_source(source_work_e::unclaim, [claim]() {
              proc::proc.restore_source_claim(**claim);
              return 200;
            });
            return;
          }
          const auto launch = source->admission->launch;
          session_clear(launch->id);
          source->admission.reset();
          source.reset();
          finish_stop();
          return;
        } else if (result.kind == source_work_e::unclaim) {
          if (result.status != 200) {
            source->cleanup_blocked = true;
            source->phase = source_phase_e::cleanup_pending;
            return;
          }
          const auto launch = source->admission->launch;
          session_clear(launch->id);
          source.reset();
          finish_stop();
          return;
        } else if (result.kind == source_work_e::release_claim) {
          if (result.status != 200) {
            source->cleanup_blocked = true;
            source->phase = source_phase_e::cleanup_pending;
            return;
          }
          finish_retirement();
          return;
        } else if (result.kind == source_work_e::claim) {
          const auto backend = source->backend;
          const auto request = source->request;
          const auto generation = source->generation;
          const auto epoch = source->epoch;
          dispatch_source(source_work_e::begin, [backend, request, generation, epoch]() {
            return backend->begin(*request, generation, epoch) ? 200 : 503;
          });
          return;
        } else if (result.kind == source_work_e::liveness) {
          const auto app = proc::proc.published_snapshot()->lifetime;
          if (!stopping && !source->failed && source->request->retain_while_app_running && app.tracked && app.instance == source->application_instance && app.app_uuid == source->app_uuid) {
            source->phase = source_phase_e::retained;
            source->next_liveness = std::chrono::steady_clock::now() + 500ms;
          } else {
            source->phase = source_phase_e::draining;
          }
        } else if (result.kind == source_work_e::rollback) {
          source->admission.reset();
          source->phase = source_phase_e::draining;
        } else if (result.kind == source_work_e::cleanup) {
          source->phase = source_phase_e::cleanup_pending;
          source->cleanup_blocked = result.status != 200;
          if (result.status == 200) {
            const auto status = source->backend->status();
            if (status.generation == source->generation && status.source_released) {
              const auto claim = source->process_claim;
              if (*claim) {
                dispatch_source(source_work_e::release_claim, [claim]() {
                  proc::proc.restore_source_claim(**claim);
                  return 200;
                });
              } else {
                finish_retirement();
              }
              return;
            }
            source->cleanup_blocked = true;
          }
        } else if (result.kind == source_work_e::complete) {
          if (result.status == 200) {
            recovery_backend.reset();
            source.reset();
            finish_stop();
            return;
          }
          source->phase = source_phase_e::cleanup_pending;
          source->cleanup_blocked = true;
          source->completion_pending = true;
        } else if (result.kind == source_work_e::restore) {
          source->phase = result.status == 200 ? source_phase_e::restoring : source_phase_e::cleanup_pending;
          source->cleanup_blocked = result.status != 200;
        } else if (result.kind == source_work_e::progress) {
          source->next_backend_progress = source_progress_now() + 100ms;
          if (result.status != 200) {
            if (source->phase == source_phase_e::applying) {
              fail_source(503);
            } else {
              source->phase = source_phase_e::cleanup_pending;
              source->cleanup_blocked = true;
            }
          }
        } else if (source->failed || (source->admission && source->admission->answered)) {
          source->phase = source_phase_e::draining;
        } else if (result.status != 200) {
          fail_source(result.status);
        } else if (result.kind == source_work_e::begin || result.kind == source_work_e::validate) {
          source->phase = source_phase_e::applying;
        } else if (result.kind == source_work_e::probe) {
          if (!authorized(*source->admission->launch, false)) {
            fail_source(403);
          } else if (source->admission->execute && !source->admission->launch->joining) {
            source->phase = source_phase_e::executing;
            source->admission->executed = true;
            dispatch_source(source_work_e::execute, source->admission->execute);
            return;
          } else {
            publish_source_ready();
          }
        } else if (result.kind == source_work_e::execute) {
          publish_source_ready();
        }
      }
      if (!source || source->work) {
        return;
      }
      std::erase_if(source->stops, [](auto &stop) {
        return stop.wait_for(0ms) == std::future_status::ready;
      });
      if (!source->stops.empty()) {
        return;
      }
      if (source->backend_unentered) {
        return;
      }
      const auto status = source->backend->status();
      if (status.generation != source->generation) {
        fail_source(503);
        source->phase = source_phase_e::cleanup_pending;
        source->cleanup_blocked = true;
        return;
      } else if (status.heartbeat_failed && !source->failed) {
        source->retire_requested = true;
        fail_source(503);
        invalidate_launches();
        clear();
      }
      if (source->phase == source_phase_e::applying) {
        std::shared_ptr<const capture_source_t> candidate;
        if (status.phase == source_backend_status_t::phase_e::ready && status.source) {
          auto metadata = std::make_shared<capture_source_t>(*status.source);
          // The admission owns this policy constraint; backend observations cannot replace it.
          metadata->requested_hdr = source->request->requested_hdr;
          candidate = std::move(metadata);
        }
        if (candidate && valid_source(candidate)) {
          source->metadata = candidate;
          source->admission->launch->source = candidate;
          source->phase = source_phase_e::probing;
          const auto backend = source->backend;
          const auto metadata = source->metadata;
          const auto pin = pin_capture(metadata);
          if (!pin) {
            fail_source(503);
            return;
          }
          dispatch_source(source_work_e::probe, [backend, metadata, pin]() {
            return backend->probe(*metadata, pin);
          });
          return;
        }
        if (status.phase == source_backend_status_t::phase_e::failed || status.phase == source_backend_status_t::phase_e::ready) {
          fail_source(503);
        }
      }
      // Consume completed ready/failure/removal observations before another refresh. Its
      // polling deadline is measured from completion, never from a potentially slow dispatch.
      if (!source->cleanup_blocked && source_progress_now() >= source->next_backend_progress && ((source->phase == source_phase_e::applying && status.phase == source_backend_status_t::phase_e::preparing) || (source->phase == source_phase_e::restoring && status.phase == source_backend_status_t::phase_e::reverting))) {
        const auto backend = source->backend;
        const auto generation = source->generation;
        dispatch_source(source_work_e::progress, [backend, generation]() {
          backend->progress(generation);
          return 200;
        });
        return;
      }
      if (source->phase == source_phase_e::draining && source->admission) {
        const auto admission = source->admission;
        if (admission->executed && admission->rollback) {
          admission->executed = false;
          dispatch_source(source_work_e::rollback, [admission]() {
            admission->rollback();
            return 200;
          });
          return;
        }
        source->admission.reset();
      }
      if (!source->prepared.empty() || !source->media.empty() || !source->capture.empty()) {
        return;
      }
      if (source->phase == source_phase_e::ready || source->phase == source_phase_e::retained) {
        if (source->phase == source_phase_e::retained && std::chrono::steady_clock::now() < source->next_liveness) {
          return;
        }
        if (!stopping && !source->failed && source->request->retain_while_app_running && source->application_instance) {
          source->phase = source_phase_e::checking;
          dispatch_source(source_work_e::liveness, []() {
            proc::proc.running();
            proc::proc.refresh_published();
            return 200;
          });
          return;
        }
        source->phase = source_phase_e::draining;
      }
      if (source->phase == source_phase_e::draining) {
        const auto backend = source->backend;
        const auto generation = source->generation;
        const auto delay = source->request->restore_delay;
        source->phase = source_phase_e::restoring;
        dispatch_source(source_work_e::restore, [backend, generation, delay]() {
          return backend->restore(generation, delay) ? 200 : 503;
        });
      } else if (!source->cleanup_blocked && source->completion_pending) {
        const auto backend = source->backend;
        const auto generation = source->generation;
        dispatch_source(source_work_e::complete, [backend, generation]() {
          return backend->complete(generation) ? 200 : 503;
        });
      } else if (!source->cleanup_blocked && (source->phase == source_phase_e::restoring || source->phase == source_phase_e::cleanup_pending) && (status.phase == source_backend_status_t::phase_e::removal_ready || status.phase == source_backend_status_t::phase_e::restored || status.phase == source_backend_status_t::phase_e::cleanup_pending)) {
        const auto backend = source->backend;
        const auto generation = source->generation;
        dispatch_source(source_work_e::cleanup, [backend, generation]() {
          if (!backend->cleanup_and_retire(generation)) {
            return 503;
          }
          return 200;
        });
      }
    }

    void finish_retirement() {
      const auto status = source->backend->status();
      if (status.generation != source->generation || !status.source_released) {
        source->cleanup_blocked = true;
        return;
      }
      recovery_backend = source->backend;
      recovery_generation = source->generation;
      recovery_blocked = false;
      recovery_retry_requested = false;
      if (status.recovery_pending) {
        source.reset();
        finish_stop();
        return;
      }
      const auto backend = source->backend;
      const auto generation = source->generation;
      dispatch_source(source_work_e::complete, [backend, generation]() {
        return backend->complete(generation) ? 200 : 503;
      });
    }

    /** @brief Continue a retired recovery episode on the operation lane, fencing admissions during completion. */
    void poll_retired_recovery() {
      if (recovery_work) {
        if (recovery_work->wait_for(0ms) != std::future_status::ready) {
          return;
        }
        const auto result = recovery_work->get();
        recovery_work.reset();
        if (result == 1) {
          recovery_backend.reset();
          recovery_generation = 0;
          recovery_retry_requested = false;
        } else if (result < 0) {
          recovery_blocked = !recovery_retry_requested;
        }
        finish_stop();
      }
      if (source || preflight || stopping || !recovery_backend || recovery_blocked || std::chrono::steady_clock::now() < next_recovery_progress) {
        return;
      }
      const auto backend = recovery_backend;
      const auto generation = recovery_generation.load();
      const auto retry = std::exchange(recovery_retry_requested, false);
      next_recovery_progress = std::chrono::steady_clock::now() + 100ms;
      recovery_work.emplace(std::async(std::launch::async, [backend, generation, retry]() {
        try {
          if (retry && !backend->retry_recovery(generation)) {
            return -1;
          }
          backend->progress(generation);
          const auto state = backend->status();
          if (state.generation != generation || !state.source_released) {
            return -1;
          }
          if (state.recovery_retry_required) {
            return -1;
          }
          if (state.recovery_pending) {
            return 0;
          }
          return backend->complete(generation) ? 1 : -1;
        } catch (...) {
          return -1;
        }
      }));
    }

    /** @brief Recheck authorization before publishing ready; only a tracked app may extend source lifetime. */
    void publish_source_ready() {
      auto admission = source->admission;
      if (!authorized(*admission->launch, false)) {
        fail_source(403);
        return;
      }
      const auto app = proc::proc.published_snapshot()->lifetime;
      source->application_instance = app.tracked && app.app_uuid == source->app_uuid ? app.instance : 0;
      admission->launch->application_instance = source->application_instance;
      admission->launch->ready = true;
      source->phase = source_phase_e::ready;
      answer_admission(200);
      source->admission.reset();
    }

    /** @brief Publish a process event without acquiring its state lock or waiting for the owner. */
    void post_application_exit(std::uint64_t instance) {
      asio::post(io_context, [this, instance]() {
        if (source && source->application_instance == instance) {
          source->application_instance = 0;
          arm_source_timer();
        }
      });
    }

    /** @brief Each capture-resource lifetime explicitly increments the owner table. */
    std::shared_ptr<capture_pin_t> pin_capture(const std::shared_ptr<const capture_source_t> &metadata) {
      if (!source || !metadata || source->metadata != metadata || (source->phase != source_phase_e::ready && source->phase != source_phase_e::probing) || source->failed) {
        return {};
      }
      const auto id = ++next_capture_pin;
      const auto generation = source->generation;
      const auto epoch = source->epoch;
      source->capture.emplace(id);
      const std::weak_ptr<source_emitter_t> emitter = source_emitter;
      return std::shared_ptr<capture_pin_t>(new capture_pin_t([emitter, id, generation, epoch]() {
        if (const auto gate = emitter.lock()) {
          std::lock_guard lock {gate->mutex};
          if (auto owner = gate->owner) {
            asio::post(owner->io_context, [owner, id, generation, epoch]() {
              if (owner->source && owner->source->generation == generation && owner->source->epoch == epoch) {
                owner->source->capture.erase(id);
                owner->arm_source_timer();
              }
            });
          }
        }
      }));
    }

    /** @brief Configure the injection seam only while no source or operation owns the backend. */
    bool set_backend(std::function<std::shared_ptr<source_backend_t>()> factory) {
      if (source || preflight || recovery_backend || session_count() || launch_event.view(0s)) {
        return false;
      }
      backend_factory = std::move(factory);
      return true;
    }

    bool seed_generation(std::uint64_t generation) {
      if (source || preflight || recovery_work || recovery_retry_requested || session_count() || launch_event.view(0s) || generation == UINT64_MAX) {
        return false;
      }
      next_source_generation = std::max(next_source_generation.load(), generation);
      return true;
    }

    bool has_backend() const {
      return static_cast<bool>(backend_factory);
    }

    /** @brief Start one owned FIFO process job; its completion never calls back synchronously. */
    void poll_process() {
      if (process_work) {
        if (process_work->wait_for(0ms) != std::future_status::ready) {
          return;
        }
        try {
          process_work->get();
        } catch (...) {
          BOOST_LOG(error) << "Process worker completion failed";
        }
        process_work.reset();
      }
      if (!process_jobs.empty()) {
        auto job = std::move(process_jobs.front());
        process_jobs.pop_front();
        process_work.emplace(std::async(std::launch::async, [job = std::move(job)]() {
          process_worker_context = true;
          auto guard = util::fail_guard([]() {
            process_worker_context = false;
          });
          try {
            proc::proc.publish_operation(true);
            job();
            proc::proc.refresh_published();
            proc::proc.publish_operation(false);
          } catch (...) {
            try {
              proc::proc.publish_operation(false);
            } catch (...) {}
            BOOST_LOG(error) << "Process operation failed";
          }
        }));
      }
      finish_stop();
    }

    /** @brief Internal enqueue is owner-only and remains available to accepted work during shutdown. */
    void queue_process(std::function<void()> operation) {
      process_jobs.emplace_back(std::move(operation));
      poll_process();
      arm_source_timer();
    }

    std::future<void> post_process(std::function<void()> operation) {
      auto task = std::make_shared<std::packaged_task<void()>>(std::move(operation));
      auto result = task->get_future();
      if (on_owner()) {
        queue_process([task]() {
          (*task)();
        });
        return result;
      }
      std::unique_lock lock {lifecycle_mutex};
      if (stopping) {
        auto denied = std::make_shared<std::packaged_task<void()>>([]() {
          throw std::runtime_error {"Process lane is shutting down"};
        });
        auto rejected = denied->get_future();
        (*denied)();
        return rejected;
      }
      if (!running) {
        lock.unlock();
        const auto previous = process_worker_context;
        process_worker_context = true;
        auto guard = util::fail_guard([previous]() {
          process_worker_context = previous;
        });
        (*task)();
      } else {
        asio::post(io_context, [this, task]() {
#ifdef SUNSHINE_TESTS
          notify_owner("process_operation_queued");
#endif
          queue_process([task]() {
            (*task)();
          });
        });
      }
      return result;
    }

    bool on_owner() {
      return io_context.get_executor().running_in_this_thread();
    }

    bool process_busy() const {
      return process_work.has_value() || !process_jobs.empty();
    }

    std::future<void> queue_stop(std::function<void(bool, std::uint64_t, std::uint64_t)> operation) {
      const auto managed = source.has_value();
      const auto generation = managed ? source->generation : 0;
      const auto instance = proc::proc.published_snapshot()->lifetime.instance;
      if (source) {
        source->retire_requested = true;
      }
      invalidate_launches();
      clear(true);
      fail_source(503);
      input::terminate_gamepads();
      auto task = std::make_shared<std::packaged_task<void()>>([operation = std::move(operation), managed, instance, generation]() {
        operation(managed, instance, generation);
      });
      auto result = task->get_future();
      if (!running) {
        (*task)();
        return result;
      }
      if (source) {
        auto done = std::make_shared<std::promise<void>>();
        source->stops.push_back(done->get_future().share());
        queue_process([task, done]() {
          (*task)();
          done->set_value();
        });
      } else {
        queue_process([task]() {
          (*task)();
        });
      }
#ifdef SUNSHINE_TESTS
      notify_owner("application_stop_queued");
#endif
      return result;
    }

    void post_liveness() {
      if (liveness_queued.exchange(true)) {
        return;
      }
      asio::post(io_context, [this]() {
        if (stopping) {
          liveness_queued = false;
          return;
        }
        queue_process([this]() {
          proc::proc.running();
          proc::proc.refresh_published();
          liveness_queued = false;
        });
      });
    }

    void retry_cleanup() {
      if (!source || source->work || source->phase != source_phase_e::cleanup_pending) {
        return;
      }
      source->cleanup_blocked = false;
      if (source->backend_unentered) {
        const auto claim = source->process_claim;
        dispatch_source(source_work_e::unclaim, [claim]() {
          if (*claim) {
            proc::proc.restore_source_claim(**claim);
          }
          return 200;
        });
        return;
      }
      const auto status = source->backend->status();
      if (status.generation == source->generation && status.source_released && *source->process_claim && !source->completion_pending) {
        const auto claim = source->process_claim;
        dispatch_source(source_work_e::release_claim, [claim]() {
          proc::proc.restore_source_claim(**claim);
          return 200;
        });
        return;
      }
      if (!source->completion_pending && status.phase != source_backend_status_t::phase_e::removal_ready && status.phase != source_backend_status_t::phase_e::cleanup_pending && status.phase != source_backend_status_t::phase_e::restored) {
        source->phase = source_phase_e::draining;
      }
      arm_source_timer();
    }

    /** @brief Recovery must remain postable while shutdown is draining, without waiting on its fence. */
    void post_retry_cleanup() {
      const auto generation = next_source_generation.load();
      const auto retired_generation = recovery_generation.load();
      asio::post(io_context, [this, generation, retired_generation]() {
        if (source && source->generation == generation) {
          retry_cleanup();
        } else if (!source && recovery_backend && recovery_generation == retired_generation) {
          recovery_blocked = false;
          recovery_retry_requested = true;
          next_recovery_progress = {};
          arm_source_timer();
        }
      });
    }

    /** @brief Shutdown drains source completion events and capture releases before stopping its executor. */
    void finish_stop() {
      if (!stopping || source || preflight || process_work || !process_jobs.empty() || recovery_work || !shutdown_completion) {
#ifdef SUNSHINE_TESTS
        if (stopping && !source && !preflight && process_work) {
          notify_owner("stop_process_work_pending");
        }
#endif
        return;
      }
      {
        std::lock_guard lock {lifecycle_mutex};
        running = false;
        stopped = true;
      }
      shutdown_completion->set_value();
      shutdown_completion.reset();
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
#ifdef SUNSHINE_TESTS
            notify_owner("launch_expired");
#endif
            discarded->cancelled = true;
            release_source_pin(*discarded, false, 503);
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
          release_source_pin(*pending);
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
          release_source_pin(*token);
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
          const auto id = stream::session::launch_id(session);
          it = _session_slots->erase(it);
          release_source_slot(id);
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
          release_source_pin(*token);
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
          const auto id = stream::session::launch_id(slot);
          i = _session_slots->erase(i);
          release_source_slot(id);
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
          release_source_pin(*token);
          session_clear(token->id);
        }
      }

      auto lg = _session_slots.lock();
      for (auto i = _session_slots->begin(); i != _session_slots->end();) {
        auto &slot = *(*i);
        if (stream::session::client_cert(slot) == cert) {
          stream::session::stop(slot);
          stream::session::join(slot);
          const auto id = stream::session::launch_id(slot);
          i = _session_slots->erase(i);
          release_source_slot(id);
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
      release_source_slot(stream::session::launch_id(*session));
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
      shutdown_completion = completion;
      shutdown_done = completion->get_future().share();
      const auto finish = [this, completion]() {
#ifdef SUNSHINE_TESTS
        notify_owner("stop_begin");
#endif
        invalidate_launches();
        boost::system::error_code error;
        acceptor.close(error);
        clear();
        if (source) {
          source->retire_requested = true;
          fail_source(503);
          poll_source();
        }
        finish_stop();
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
    std::function<std::chrono::steady_clock::time_point()> source_progress_clock;  ///< Virtual progress time; admission deadlines retain their real clock.
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
      source_progress_clock = {};
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
    boost::asio::steady_timer source_timer {io_context};  ///< Staged backend polling and retained-app liveness.
    bool source_timer_armed = false;
    std::optional<source_record_t> source;  ///< The sole owner of source accounting; metadata owns no capture pin.
    std::shared_ptr<preflight_t> preflight;  ///< Reservation and owned refresh continuation; no backend effect yet.
    std::deque<std::function<void()>> process_jobs;  ///< Owner-ordered process/catalog mutations and observations.
    std::optional<std::future<void>> process_work;  ///< Sole in-flight process operation, always polled.
    std::atomic_bool liveness_queued {false};
    std::shared_ptr<source_backend_t> recovery_backend;  ///< Retired unavailable-display recovery outlives a source.
    std::atomic_uint64_t recovery_generation {0};  ///< Retired logical generation remains fenced after the source is dropped.
    std::optional<std::future<int>> recovery_work;  ///< Sole retired operation, never concurrent with a new source.
    bool recovery_blocked = false;  ///< Failed completion requires an explicit retry.
    bool recovery_retry_requested = false;  ///< A generation-fenced public request survives an in-flight observation and restarts stopped work.
    std::chrono::steady_clock::time_point next_recovery_progress {};  ///< Bound idle polling while recovery waits for hardware.
    std::function<std::shared_ptr<source_backend_t>()> backend_factory;  ///< Empty until explicitly integrated and qualified.
    std::shared_ptr<source_emitter_t> source_emitter = std::make_shared<source_emitter_t>();
    std::atomic_uint64_t next_source_generation {0};
    std::uint64_t next_capture_epoch = 0;
    std::uint64_t next_capture_pin = 0;

    std::shared_ptr<socket_t> next_socket;
    std::vector<std::weak_ptr<launch_session_t>> tokens;  ///< Accepted-socket tokens that a global Stop must invalidate.
    std::recursive_mutex lifecycle_mutex;  ///< Protects offline direct execution versus owner dispatch.
    bool running = false;  ///< A live RTSP owner will drain posted operations.
    std::shared_future<void> shutdown_done;  ///< Completes all accepted work before offline calls may proceed.
    std::shared_ptr<std::promise<void>> shutdown_completion;  ///< Source drain keeps the owner runnable through shutdown.
  };

  rtsp_server_t server {};  ///< Singleton RTSP server used by GameStream launch sessions.

  bool set_source_backend(std::function<std::shared_ptr<source_backend_t>()> factory) {
    return server.sync([factory = std::move(factory)]() mutable {
      return server.set_backend(std::move(factory));
    });
  }

  bool seed_source_generation(std::uint64_t recovered_generation) {
    return server.sync([recovered_generation]() {
      return server.seed_generation(recovered_generation);
    });
  }

  bool has_source_backend() {
    return server.sync([]() {
      return server.has_backend();
    });
  }

  void retry_source_cleanup() {
    server.post_retry_cleanup();
  }

  source_snapshot_t source_snapshot() {
    return server.sync([]() {
      return server.snapshot();
    });
  }

  int prepare_source_launch(std::shared_ptr<launch_session_t> launch, std::function<std::pair<int, std::shared_ptr<const display_policy::source_request_t>>(const source_snapshot_t &)> plan, std::function<int()> execute, std::function<void()> rollback, std::function<void()> refresh) {
    if (server.on_owner()) {
      return 503;
    }
    auto result = std::make_shared<std::promise<int>>();
    auto future = result->get_future();
    server.sync([launch = std::move(launch), plan = std::move(plan), execute = std::move(execute), rollback = std::move(rollback), refresh = std::move(refresh), result]() mutable {
      server.prepare_source(std::move(launch), std::move(plan), std::move(execute), std::move(rollback), result, std::move(refresh));
    });
    return future.get();
  }

  std::shared_ptr<capture_pin_t> acquire_capture_pin(const std::shared_ptr<const capture_source_t> &source) {
    return server.on_owner() ? server.pin_capture(source) : nullptr;
  }

  void application_exited(std::uint64_t instance) {
    server.post_application_exit(instance);
  }

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
      if (server.process_busy()) {
        return 503;
      }
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

  void catalog_operation(std::function<void()> operation) {
    if (process_worker_context) {
      operation();
      return;
    }
    if (server.on_owner()) {
      if (server.process_busy()) {
        throw std::logic_error {"Owner must use a process continuation"};
      }
      operation();
      return;
    }
    process_operation(std::move(operation)).get();
  }

  std::future<void> process_operation(std::function<void()> operation) {
    return server.post_process(std::move(operation));
  }

  bool on_process_worker() {
    return process_worker_context;
  }

  bool on_owner_thread() {
    return server.on_owner();
  }

  void refresh_process_liveness() {
    server.post_liveness();
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
    auto completion = server.sync([stop = std::move(stop)]() mutable {
      return server.queue_stop([stop = std::move(stop)](bool, std::uint64_t, std::uint64_t) {
        if (stop) {
          stop();
        }
      });
    });
    if (!server.on_owner()) {
      completion.get();
    }
  }

  bool stop_process_application(std::optional<process_authority_t> authority) {
    if (server.on_owner()) {
      throw std::logic_error {"Stop completion must be awaited outside the owner"};
    }
    auto allowed = std::make_shared<std::atomic_bool>(true);
    auto completion = server.sync([authority, allowed]() {
      if (authority) {
        const auto current = nvhttp::current_client(authority->uuid, authority->cert);
        if (!current || current->revision != authority->revision || !permissions::allows(current->permissions, authority->required)) {
          allowed->store(false);
          std::promise<void> done;
          done.set_value();
          return done.get_future();
        }
      }
      return server.queue_stop([authority, allowed](bool managed, std::uint64_t instance, std::uint64_t generation) {
        const auto state = proc::proc.lock_state();
        if (authority) {
          const auto current = nvhttp::current_client(authority->uuid, authority->cert);
          if (!current || current->revision != authority->revision || !permissions::allows(current->permissions, authority->required)) {
            allowed->store(false);
            return;
          }
        }
        proc::proc.terminate_source(instance, generation);
        if (!managed) {
          display_device::revert_configuration();
        }
      });
    });
    completion.get();
    if (authority) {
      const auto current = nvhttp::current_client(authority->uuid, authority->cert);
      if (!current || current->revision != authority->revision || !permissions::allows(current->permissions, authority->required)) {
        return false;
      }
    }
    return allowed->load();
  }

  void terminate_sessions_by_cert(std::string_view cert) {
    server.sync([&]() {
      server.clear_by_cert(cert);
      input::terminate_gamepads(cert);
    });
  }

#ifdef SUNSHINE_TESTS
  namespace test_support {
    void owner_operation(std::function<void()> operation) {
      server.sync([operation = std::move(operation)]() {
        assert(on_owner_thread());
        operation();
      });
    }

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

    void set_source_progress_clock(std::function<std::chrono::steady_clock::time_point()> clock) {
      server.sync([&]() {
        server.source_progress_clock = std::move(clock);
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

    std::shared_ptr<launch_session_t> pending_launch() {
      return server.sync([&]() {
        return server.launch_event.view(0s);
      });
    }

    bool start_media(const std::shared_ptr<launch_session_t> &launch, bool success) {
      return server.sync([launch, success]() {
        return server.source_started(*launch, success);
      });
    }

    void release_media(const std::shared_ptr<launch_session_t> &launch) {
      server.sync([launch]() {
        server.release_source_pin(*launch, true);
      });
    }

    void poll_source() {
      server.sync([]() {
        server.poll_source();
        server.poll_retired_recovery();
      });
    }

    std::shared_ptr<capture_pin_t> pin_capture(const std::shared_ptr<const capture_source_t> &source) {
      return server.sync([source]() {
        return server.pin_capture(source);
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
    auto admission_guard = util::fail_guard([&]() {
      if (!session.source_media) {
        session.cancelled = true;
        rtsp_server->release_source_pin(session);
        rtsp_server->session_clear(session.id);
      }
    });
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

    std::shared_ptr<stream::session_t> stream_session;
    try {
      stream_session = stream::session::alloc(config, session);
      rtsp_server->insert(stream_session);
      if (stream::session::start(*stream_session, sock.remote_endpoint().address().to_string()) || !rtsp_server->source_started(session, true)) {
        throw std::runtime_error {"Stream start failed"};
      }
    } catch (...) {
      if (stream_session) {
        stream::session::abort_start(*stream_session);
        rtsp_server->remove(stream_session);
      }
      rtsp_server->source_started(session, false);
      respond(sock, session, &option, 500, "Internal Server Error", req->sequenceNumber, {});
      return;
    }
    admission_guard.disable();
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
