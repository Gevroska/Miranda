/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>

// local includes
#include "capture_source.h"
#include "crypto.h"
#include "display_policy.h"
#include "permissions.h"
#include "thread_safe.h"

namespace platf {
  class deinit_t;
}

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;  ///< GameStream base-port offset used for the RTSP setup listener.

#pragma pack(push, 1)

  /**
   * @brief Encrypted RTSP message header used by GameStream.
   */
  struct encrypted_rtsp_header_t {
    // We set the MSB in encrypted RTSP messages to allow format-agnostic
    // parsing code to be able to tell encrypted from plaintext messages.
    static constexpr std::uint32_t ENCRYPTED_MESSAGE_TYPE_BIT = 0x80000000;  ///< Protocol or platform constant for encrypted message type bit.

    /**
     * @brief Return a pointer to the protocol payload following the packet header.
     *
     * @return Parsed or serialized payload data.
     */
    uint8_t *payload() {
      return (uint8_t *) (this + 1);
    }

    /**
     * @brief Decode the RTSP interleaved-frame payload length.
     *
     * @return Payload byte count with the encrypted-message flag masked out.
     */
    std::uint32_t payload_length() const {
      return util::endian::big<std::uint32_t>(typeAndLength) & ~ENCRYPTED_MESSAGE_TYPE_BIT;
    }

    /**
     * @brief Inspect the encrypted-message flag without modifying the wire header.
     *
     * @return True when the encrypted-message flag is set.
     */
    bool is_encrypted() const {
      return !!(util::endian::big<std::uint32_t>(typeAndLength) & ENCRYPTED_MESSAGE_TYPE_BIT);
    }

    // This field is the length of the payload + ENCRYPTED_MESSAGE_TYPE_BIT in big-endian
    std::uint32_t typeAndLength;  ///< Type and length.

    // This field is the number used to initialize the bottom 4 bytes of the AES IV in big-endian
    std::uint32_t sequenceNumber;  ///< Sequence number.

    // This field is the AES GCM authentication tag
    std::array<std::uint8_t, 16> tag;  ///< Authentication tag appended to the encrypted payload.
  };

#pragma pack(pop)

  static_assert(sizeof(encrypted_rtsp_header_t) == 24);
  static_assert(offsetof(encrypted_rtsp_header_t, tag) == 8);

  /**
   * @brief RTSP launch session state shared with stream setup.
   */
  struct launch_session_t {
    uint32_t id;  ///< RTSP launch-session identifier assigned before stream startup.

    crypto::aes_t gcm_key;  ///< AES-GCM key negotiated for encrypted RTSP messages.
    crypto::aes_t iv;  ///< Initial RTSP AES-GCM IV supplied by the client.

    std::string av_ping_payload;  ///< AV ping payload.
    uint32_t control_connect_data;  ///< Client-provided token used when connecting the control channel.

    bool host_audio;  ///< Whether host audio should be played locally.
    std::string unique_id;  ///< Moonlight client unique identifier for this launch request.
    int width;  ///< Frame or display width in pixels.
    int height;  ///< Frame or display height in pixels.
    int fps;  ///< Requested video frame rate.
    int gcmap;  ///< Game controller mapping requested by the client.
    int appid;  ///< Application ID requested for launch or resume.
    std::string app_uuid;  ///< Server-resolved application UUID, never a client authority token.
    int surround_info;  ///< Encoded GameStream surround-sound capability flags.
    std::string surround_params;  ///< Client-provided surround-sound layout parameters.
    bool continuous_audio;  ///< Whether audio packets continue during silence.
    bool enable_hdr;  ///< Whether HDR streaming is requested.
    bool enable_sops;  ///< Whether sequence output protection is requested.
    std::string client_name;  ///< Friendly client name from initial pairing.

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;  ///< AES-GCM cipher used once encrypted RTSP is negotiated.
    std::string rtsp_url_scheme;  ///< URL scheme selected by the RTSP SETUP flow.
    uint32_t rtsp_iv_counter;  ///< Counter value mixed into encrypted RTSP IVs.
    std::string client_uuid;  ///< Persistent UUID of the authenticated pairing that requested this launch.
    std::string client_cert;  ///< Canonical PEM of the authenticated paired Moonlight client certificate.
    std::uint64_t authorization_revision = 0;  ///< Original authorization revision of this admission.
    bool cancelled = false;  ///< Token invalidation, accessed only on the RTSP owner thread.
    bool ready = false;  ///< Preparation succeeded before sockets may use this token.
    bool admitted = false;  ///< At most one ANNOUNCE may allocate a stream from this token.
    bool joining = false;  ///< This launch joins an existing source without preparing or executing it.
    std::shared_ptr<const capture_source_t> source;  ///< Immutable binding; D must acquire actual capture-resource pins separately.
    std::uint64_t source_generation = 0;  ///< Explicit owner accounting, never inferred from token destruction.
    std::uint64_t application_instance = 0;  ///< Genuinely tracked process incarnation associated with this source.
    bool source_media = false;  ///< Prepared ownership converted only after successful stream start.
    std::shared_ptr<std::atomic_bool> source_cancel;  ///< Worker-visible cancellation without reading owner-only token flags.
  };

  /** @brief Copyable B-shaped progress observed without entering blocking backend operations. */
  struct source_backend_status_t {
    enum class phase_e {
      preparing,
      ready,
      reverting,
      removal_ready,
      cleanup_pending,
      restored,
      failed
    };
    std::uint64_t generation {};  ///< Stale backend snapshots cannot affect another source.
    phase_e phase = phase_e::preparing;  ///< Existing display transaction progress.
    bool heartbeat_failed = false;  ///< Sticky failure; the owner stops affected media and retains recovery.
    bool source_released = false;  ///< Checked logical retirement; owned virtual additionally requires its issued cleanup receipt.
    bool recovery_pending = false;  ///< Retired library recovery must survive the source.
    std::shared_ptr<const capture_source_t> source;  ///< Verified exact mapping after apply or retained validation.
    bool recovery_retry_required = false;  ///< Retired restoration stopped; waiting for unavailable hardware alone does not set this.
  };

  /**
   * @brief Narrow portable seam for the owned display transaction and lease; no production implementation is registered here.
   * @details Blocking methods run on a serialized operation worker. status() only copies already published
   * state. An owned-virtual backend owns B's journal, lease and unforgeable cleanup receipt. begin uses begin_next when
   * retaining a recovery episode; cleanup_and_retire performs checked refresh, cleanup, receipt validation
   * and retire_source in that order. False retains ownership. complete never erases pending library recovery.
   * A native backend restores/retires only its logical source and never fabricates SudoVDA ownership or
   * receipts. Registration supplies no deployment authority: each production path needs its corresponding
   * integration/qualification; native availability must not depend on virtual-driver provisioning.
   */
  class source_backend_t {
  public:
    /** @brief Preserve owned recovery on destruction; failed cleanup is never implied to have succeeded. */
    virtual ~source_backend_t() = default;

    /** @brief Inspect copied recovery state before allocation, claiming or generation mutation; never perform I/O. */
    virtual bool admits(const display_policy::source_request_t &) const {
      return true;
    }

    /** @brief Refresh native readback and copied progress on the serialized operation lane, never on the owner. */
    virtual void progress(std::uint64_t) {}

    /** @brief Restart stopped retired restoration on the operation lane, preserving its episode and removal receipt. */
    virtual bool retry_recovery(std::uint64_t) {
      return true;
    }

    /** @brief Begin/acquire/bind/prepare, or begin_next on the retained recovery episode, outside the owner. */
    virtual bool begin(const display_policy::source_request_t &request, std::uint64_t generation, std::uint64_t epoch) = 0;
    /** @brief Refresh/read back the exact retained identity/mode and publish the newly assigned epoch. */
    virtual bool validate_retained(const capture_source_t &source, std::uint64_t epoch) = 0;
    /** @brief Copy already-published transaction/heartbeat progress without waiting for native work. */
    virtual source_backend_status_t status() const = 0;
    /** @brief Probe the exact binding using its preissued ticket; D must retain it through actual teardown. */
    virtual int probe(const capture_source_t &source, std::shared_ptr<capture_pin_t> pin) = 0;
    /** @brief Schedule B restoration with the frozen delay; progress is observed through status(). */
    virtual bool restore(std::uint64_t generation, std::chrono::milliseconds delay) = 0;
    /** @brief Refresh/read back, clean up the exact lease, validate its issued receipt, then retire_source. */
    virtual bool cleanup_and_retire(std::uint64_t generation) = 0;
    /** @brief Complete only an already-retired episode whose library persistence is empty. */
    virtual bool complete(std::uint64_t generation) = 0;
  };

  /** @brief Copied admission view; process liveness alone never creates a retained source. */
  struct source_snapshot_t {
    bool managed = false;  ///< An explicit managed source record exists.
    bool busy = false;  ///< Preparation, drain or cleanup prohibits a new admission.
    bool retained = false;  ///< A zero-viewer ready source requires exact validation and a new probe epoch.
    int appid = 0;  ///< Frozen application selection.
    std::string app_uuid;  ///< Frozen P1c application identity.
    std::uint64_t application_instance {};  ///< Process incarnation, not merely app ID.
    std::shared_ptr<const display_policy::source_request_t> request;  ///< Frozen P3a policy.
    std::shared_ptr<const capture_source_t> source;  ///< Metadata carries no capture pin.
    std::size_t prepared_pins {};  ///< Live prepared admissions.
    std::size_t media_pins {};  ///< Successfully started stream slots.
    std::size_t capture_pins {};  ///< Explicit actual-resource lifetimes.
    bool cleanup_blocked = false;  ///< Failed restoration/removal/retirement requires an explicit retry.
    bool recovery_pending = false;  ///< A retired backend still owns physical recovery independently of a source.
  };

  /** @brief Install an inert-by-default, allocation-only factory at a quiescent boundary; reject replacement while owned. */
  bool set_source_backend(std::function<std::shared_ptr<source_backend_t>()> factory);
  /** @brief Internal startup seam: reserve all generations through a recovered journal at a quiescent boundary. */
  bool seed_source_generation(std::uint64_t recovered_generation);
  /** @brief Return the owner record; never infer source ownership from connection count or process running(). */
  source_snapshot_t source_snapshot();
  /**
   * @brief Reserve and asynchronously prepare an authorized managed admission, awaiting only outside the owner.
   * @param launch Admission token carrying the original authorization revision.
   * @param plan Owner-only pure/catalog preflight, setting selected app and returning a frozen P3a request.
   * @param execute Copied-input application operation dispatched only after another authority check.
   * @param rollback Copied-input application rollback after an in-flight operation has drained.
   * @return Protocol status. No registered backend returns 503 without side effects.
   */
  int prepare_source_launch(std::shared_ptr<launch_session_t> launch, std::function<std::pair<int, std::shared_ptr<const display_policy::source_request_t>>(const source_snapshot_t &)> plan, std::function<int()> execute = {}, std::function<void()> rollback = {}, std::function<void()> refresh = {});
  /** @brief Whether a managed factory is installed; this does not assert deployment qualification. */
  bool has_source_backend();
  /** @brief Post a generation-fenced cleanup retry without waiting, including while shutdown drains. */
  void retry_source_cleanup();
  /** @brief Owner-only: issue a pin before dispatch; workers must share/transfer this existing pin and only post release. */
  std::shared_ptr<capture_pin_t> acquire_capture_pin(const std::shared_ptr<const capture_source_t> &source);
  /** @brief Publish process exit without waiting for an owner that may be joining this caller. */
  void application_exited(std::uint64_t instance);

  /**
   * @brief Queue a launch session until the RTSP client connects.
   *
   * @param launch_session Session state prepared by the GameStream launch handler.
   * @return Whether the authorized pending reservation was accepted.
   */
  bool launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Reserve the pending slot and prepare a launch on the RTSP owner thread.
   * @param launch_session Caller-owned admission token.
   * @param prepare Operation returning a protocol status after authorized preparation.
   * @param rollback Caller-owned cleanup after preparation fails or authority changes.
   * @return Explicit success, denied, busy or preparation-failure status.
   */
  int prepare_launch(std::shared_ptr<launch_session_t> launch_session, std::function<int()> prepare, std::function<void()> rollback = {});

  /**
   * @brief Run a protected operation at the same ordering point as admissions and revocations.
   * @param uuid Original paired UUID.
   * @param cert Exact paired certificate.
   * @param required Effective action right required by the operation.
   * @param operation Operation executed only after checking current authority.
   * @return Whether current authority allowed dispatch.
   */
  bool authorize_operation(std::string_view uuid, std::string_view cert, permissions::mask_t required, std::function<void()> operation);

  /**
   * @brief Order owned catalog work on the process FIFO and await outside the owner.
   * @param operation Must never wait for the owner or a control worker.
   * @note The quiescent legacy owner/startup path may execute directly. A busy owner must use an explicit continuation.
   */
  void catalog_operation(std::function<void()> operation);
  /** @brief Queue an owned process/catalog operation in the owner's FIFO; wait only outside the owner. */
  std::future<void> process_operation(std::function<void()> operation);
  /** @brief Whether the current caller is the owner; blocking process/catalog wrappers reject owner waits. */
  bool on_owner_thread();
  /** @brief True only inside the dedicated process lane; its operations must never await the owner. */
  bool on_process_worker();
  /** @brief Coalesce an off-owner process liveness observation and cleanup. */
  void refresh_process_liveness();

  /** @brief Exact paired authority carried through a deferred process operation. */
  struct process_authority_t {
    std::string uuid;  ///< Original paired identity.
    std::string cert;  ///< Original exact paired certificate.
    std::uint64_t revision {};  ///< Admission authority revision, checked again after queue wait.
    permissions::mask_t required {};  ///< Effective operation rights.
  };

  /** @brief Invalidate immediately, then perform exact-incarnation Stop on the process lane and await outside the owner. */
  bool stop_process_application(std::optional<process_authority_t> authority = std::nullopt);

  /**
   * @brief Reconcile a completed authorization mutation on the admission owner thread.
   * @param uuid Changed original pairing UUID.
   * @param cert Original paired certificate for retained-controller cleanup.
   * @param destroy Retained devices must be destroyed after disable/unpair.
   */
  void reconcile_client(std::string_view uuid, std::string_view cert, bool destroy);

  /**
   * @brief Run an explicit authorized global Stop at the admission serialization point.
   * @param stop Application/display cleanup after all connections and pending tokens are stopped.
   */
  void stop_application(std::function<void()> stop);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();
  /**
   * @brief Terminate active sessions associated with a client certificate.
   *
   * @param cert Certificate data or object used by the operation.
   */
  void terminate_sessions_by_cert(std::string_view cert);

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();

#ifdef SUNSHINE_TESTS
  namespace test_support {
    /** @brief Run a synchronous fixture callback on the actual owner, never on the process lane. */
    void owner_operation(std::function<void()> operation);
    /**
     * @brief Run the actual RTSP owner without binding a listener or starting media workers.
     * @return Handle stopping and joining that owner on destruction.
     */
    std::unique_ptr<platf::deinit_t> start_owner();
    /**
     * @brief Observe real owner ordering boundaries for deterministic race tests.
     * @param hook Boundary observer, or empty to remove it.
     */
    void set_owner_hook(std::function<void(std::string_view)> hook);
    /** @brief Replace progress polling time for barrier-driven tests; admission deadlines keep the real steady clock. */
    void set_source_progress_clock(std::function<std::chrono::steady_clock::time_point()> clock);
    /**
     * @brief Stop the isolated owner through its production shutdown fence.
     */
    void stop_owner();
    /**
     * @brief Check the same token guard used for every RTSP dispatch.
     * @param launch Original retained launch object.
     * @return Whether this token remains currently authorized.
     */
    bool token_authorized(const std::shared_ptr<launch_session_t> &launch);
    /**
     * @brief Exercise the one-admission guard used immediately before stream allocation.
     * @param launch Original retained launch object.
     * @return Whether a first authorized ANNOUNCE can claim it.
     */
    bool claim_launch(const std::shared_ptr<launch_session_t> &launch);
    /**
     * @brief Inspect the actual pending admission for protocol identity and rollback regressions.
     */
    std::shared_ptr<launch_session_t> pending_launch();
    /** @brief Convert/release fake media through the same production ownership transitions. */
    bool start_media(const std::shared_ptr<launch_session_t> &launch, bool success);
    /** @brief Release fake media after its simulated stop/join/slot removal. */
    void release_media(const std::shared_ptr<launch_session_t> &launch);
    /** @brief Poll the owner without waiting for a timer in deterministic component fixtures. */
    void poll_source();
    /** @brief Acquire on the owner for delayed resource-lifetime component fixtures. */
    std::shared_ptr<capture_pin_t> pin_capture(const std::shared_ptr<const capture_source_t> &source);
  }  // namespace test_support
#endif
}  // namespace rtsp_stream
