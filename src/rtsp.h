/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

// local includes
#include "crypto.h"
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
  };

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
   * @brief Serialize catalog work with admission/revocation; use direct fallback before owner startup.
   * @param operation Must never wait for a control worker or acquire authorization from a state-file callback.
   * @note Never call from stream control workers that owner shutdown can join.
   */
  void catalog_operation(std::function<void()> operation);

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
  }  // namespace test_support
#endif
}  // namespace rtsp_stream
