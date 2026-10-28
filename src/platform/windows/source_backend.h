/**
 * @file src/platform/windows/source_backend.h
 * @brief Inactive Windows native and owned-virtual source adapters.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// local includes
#include "src/display_device.h"
#include "src/rtsp.h"

namespace platf::source_startup {
  class coordinator_t;
  struct recovery_proof_t;
}  // namespace platf::source_startup

namespace platf::source_backend {
  /**
   * @brief Opaque lifetime of independently established recovery authorization.
   * @details A provider must retain protected storage/write access, the independent deployment
   * identity and exclusive driver control until this object is destroyed. It must not revoke or
   * release those guarantees while any recovery callback, heartbeat or cleanup can still run.
   * The protected constructor is a provider contract, not a journal/path/Boolean-based issuer.
   * There is no production provider in this component.
   */
  class recovery_authorization_t {
  public:
    virtual ~recovery_authorization_t() = default;
    recovery_authorization_t(const recovery_authorization_t &) = delete;
    recovery_authorization_t &operator=(const recovery_authorization_t &) = delete;

  protected:
    /** @brief Bind independently obtained deployment metadata to this provider's held lifetime. */
    explicit recovery_authorization_t(sudovda::recovery_authority_t authority);

  private:
    friend class recovery_context_t;
    const sudovda::recovery_authority_t authority_;
  };

  /**
   * @brief Future protected provisioning verifier, invoked only on the operation lane.
   * @details An implementation must independently verify protected provisioning and exclusive
   * deployment. Factory registration, a configuration Boolean, enumeration and the ownership
   * journal do not supply this authority. E1 provides no production verifier implementation.
   */
  class authority_verifier_t {
  public:
    virtual ~authority_verifier_t() = default;
    /** @brief Qualify this journal location and requested source before B intent or driver access. */
    virtual std::expected<sudovda::recovery_authority_t, sudovda::failure_t> verify(const std::filesystem::path &journal_path, const display_policy::source_request_t &request, std::uint64_t generation) = 0;
    /** @brief Default-deny recovery qualification without manufacturing a fresh source request. */
    virtual std::expected<std::shared_ptr<recovery_authorization_t>, sudovda::failure_t> verify_recovery(const std::filesystem::path &journal_path);
  };

  /** @brief Inert construction inputs; no storage or driver access occurs when they are copied. */
  struct options_t {
    std::filesystem::path journal_path;  ///< Future verifier must qualify the actual protected location.
    std::shared_ptr<authority_verifier_t> authority;  ///< Absent by default: Virtual fails closed, Native remains available.
  };

  /** @brief Distinct acquisition/cleanup/liveness diagnostics copied without exposing operational paths. */
  struct diagnostics_t {
    std::optional<sudovda::failure_t> acquisition;  ///< Original begin/acquisition or reconstruction failure.
    std::optional<sudovda::failure_t> cleanup;  ///< Independent cleanup failure.
    std::optional<sudovda::failure_t> liveness;  ///< Sticky acquisition or heartbeat failure.
  };

  /** @brief Read-only checked physical availability; an unavailable query is never headless evidence. */
  std::optional<bool> physical_source_available();

  /** @brief Construct an inactive dispatcher; copying options performs no storage or native work. */
  std::shared_ptr<rtsp_stream::source_backend_t> make_backend(options_t options = {});

  /**
   * @brief Retained reconstruction of one exact startup episode; this never represents a source.
   * @details Coordinator operations and an owner offer exclude one another without blocking the
   * owner. After adoption coordinator/caller aliases are inert. B, authorization and the exact
   * startup proof survive coordinator destruction. Completion and gate release remain unavailable.
   */
  class recovery_context_t {
  public:
    ~recovery_context_t();
    recovery_context_t(const recovery_context_t &) = delete;
    recovery_context_t &operator=(const recovery_context_t &) = delete;
    /** @brief Copy the original pending generation, including after partial reconstruction failure. */
    std::uint64_t generation() const;
    /** @brief Whether exact B and the required lease/receipt are retained; never capture readiness. */
    bool reconstructed() const;
    /** @brief Copy inert recovery progress without exposing a capture source or application claim. */
    rtsp_stream::source_backend_status_t status() const;
    /** @brief Copy independent acquisition, cleanup and liveness failures. */
    diagnostics_t diagnostics() const;

  private:
    friend class source_startup::coordinator_t;
    friend class rtsp_stream::rtsp_server_t;
    friend bool rtsp_stream::adopt_source_recovery(std::shared_ptr<recovery_context_t> &context);
#ifdef SUNSHINE_TESTS
    friend std::shared_ptr<rtsp_stream::source_backend_t> test_recovery_backend(const recovery_context_t &context);
    friend std::shared_ptr<recovery_context_t> test_recovery_context(std::shared_ptr<rtsp_stream::source_backend_t> backend, std::uint64_t generation, bool reconstructed);
#endif
    struct state_t;
    recovery_context_t(options_t options, std::shared_ptr<rtsp_stream::source_backend_t> backend, std::string expected_bytes, std::uint64_t generation, std::shared_ptr<source_startup::recovery_proof_t> proof);
#ifdef SUNSHINE_TESTS
    recovery_context_t(std::shared_ptr<rtsp_stream::source_backend_t> backend, std::uint64_t generation);
#endif
    /** @brief Reserve exclusive coordinator mutation; an owner offer or adoption refuses immediately. */
    bool begin_coordinator_operation();
    /** @brief Release only this coordinator operation without changing offered/adopted custody. */
    void end_coordinator_operation() noexcept;
    /** @brief Reserve a caller-owned offer before posting to the owner; never wait on its executor. */
    bool offer() noexcept;
    /** @brief Return an unconsumed offer to its caller after refusal or precommit dispatch failure. */
    void withdraw_offer() noexcept;
    /** @brief Nonthrowing custody commit, called only at the owner's admission ordering point. */
    bool adopt_offer() noexcept;
    /** @brief Copy the retained backend privately while the offer excludes coordinator mutation. */
    std::shared_ptr<rtsp_stream::source_backend_t> offered_backend() const;
    bool reconstruct();
    bool fence();
    std::unique_ptr<state_t> state_;
  };

#ifdef SUNSHINE_TESTS
  /** @brief Inert boundaries for exercising the concrete adapter with existing B and D fixtures. */
  struct test_dependencies_t {
    std::function<std::shared_ptr<display_device::WinApiLayerInterface>()> make_api;  ///< Deferred readback API construction.
    std::function<std::vector<std::string>()> capture_names;  ///< Existing capturable-output enumeration.
    std::function<void(const std::string &)> wake_native;  ///< Ordinary native display wake before detection; no virtual wake.
    std::function<std::optional<LUID>(const std::string &)> render_adapter;  ///< Exact configured rendering adapter selection.
    std::function<std::unique_ptr<sudovda::system_t>()> make_system;  ///< Deferred fake driver transport.
    std::function<std::expected<std::unique_ptr<display_device::owned_display_configuration_t>, sudovda::failure_t>(const std::filesystem::path &, const GUID &, std::uint64_t, const sudovda::recovery_authority_t &)> begin_owned;  ///< Existing B transaction with injected primitives.
    std::function<std::expected<std::unique_ptr<display_device::owned_display_configuration_t>, sudovda::failure_t>(const std::filesystem::path &, const sudovda::recovery_authority_t &, const std::string &)> recover_owned;  ///< Real B reconstruction through injected native primitives and expected journal bytes.
    std::function<int(const rtsp_stream::capture_source_t &, std::shared_ptr<rtsp_stream::capture_pin_t>)> probe;  ///< Inert probe boundary retaining the actual owner-issued ticket.
  };

  /** @brief Construct the same dispatcher with fake boundaries; callbacks are never invoked at construction. */
  std::shared_ptr<rtsp_stream::source_backend_t> test_make_backend(options_t options, test_dependencies_t dependencies);
  /** @brief Exercise the checked read-only availability query without native access. */
  std::optional<bool> test_physical_source_available(const display_device::WinApiLayerInterface &api);
  /** @brief Copy concrete diagnostics without entering native work. */
  diagnostics_t test_diagnostics(const rtsp_stream::source_backend_t &backend);
  /** @brief Model an independently held guarantee in tests only; no production provenance is claimed. */
  std::shared_ptr<recovery_authorization_t> test_recovery_authorization(sudovda::recovery_authority_t authority, std::shared_ptr<void> lifetime);
  /** @brief Inject an inert owner-only backend; this fixture issues no production authorization or startup proof. */
  std::shared_ptr<recovery_context_t> test_recovery_context(std::shared_ptr<rtsp_stream::source_backend_t> backend, std::uint64_t generation, bool reconstructed = true);
  /** @brief Inspect the retained concrete recovery backend in component fixtures only. */
  std::shared_ptr<rtsp_stream::source_backend_t> test_recovery_backend(const recovery_context_t &context);
#endif
}  // namespace platf::source_backend
