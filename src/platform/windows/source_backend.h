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

namespace platf::source_backend {
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
  };

  /** @brief Inert construction inputs; no storage or driver access occurs when they are copied. */
  struct options_t {
    std::filesystem::path journal_path;  ///< Future verifier must qualify the actual protected location.
    std::shared_ptr<authority_verifier_t> authority;  ///< Absent by default: Virtual fails closed, Native remains available.
  };

  /** @brief Distinct acquisition/cleanup/liveness diagnostics copied without exposing operational paths. */
  struct diagnostics_t {
    std::optional<sudovda::failure_t> acquisition;  ///< Original begin/acquisition failure.
    std::optional<sudovda::failure_t> cleanup;  ///< Independent cleanup failure.
    std::optional<sudovda::failure_t> liveness;  ///< Sticky acquisition or heartbeat failure.
  };

  /** @brief Read-only checked physical availability; an unavailable query is never headless evidence. */
  std::optional<bool> physical_source_available();

  /** @brief Construct an inactive dispatcher; selection and all native work begin only through begin(). */
  std::shared_ptr<rtsp_stream::source_backend_t> make_backend(options_t options = {});

#ifdef SUNSHINE_TESTS
  /** @brief Inert boundaries for exercising the concrete adapter with existing B and D fixtures. */
  struct test_dependencies_t {
    std::function<std::shared_ptr<display_device::WinApiLayerInterface>()> make_api;  ///< Deferred readback API construction.
    std::function<std::vector<std::string>()> capture_names;  ///< Existing capturable-output enumeration.
    std::function<void(const std::string &)> wake_native;  ///< Ordinary native display wake before detection; no virtual wake.
    std::function<std::optional<LUID>(const std::string &)> render_adapter;  ///< Exact configured rendering adapter selection.
    std::function<std::unique_ptr<sudovda::system_t>()> make_system;  ///< Deferred fake driver transport.
    std::function<std::expected<std::unique_ptr<display_device::owned_display_configuration_t>, sudovda::failure_t>(const std::filesystem::path &, const GUID &, std::uint64_t, const sudovda::recovery_authority_t &)> begin_owned;  ///< Existing B transaction with injected primitives.
    std::function<int(const rtsp_stream::capture_source_t &, std::shared_ptr<rtsp_stream::capture_pin_t>)> probe;  ///< Inert probe boundary retaining the actual owner-issued ticket.
  };

  /** @brief Construct the same dispatcher with fake boundaries; callbacks are never invoked at construction. */
  std::shared_ptr<rtsp_stream::source_backend_t> test_make_backend(options_t options, test_dependencies_t dependencies);
  /** @brief Exercise the checked read-only availability query without native access. */
  std::optional<bool> test_physical_source_available(const display_device::WinApiLayerInterface &api);
  /** @brief Copy concrete diagnostics without entering native work. */
  diagnostics_t test_diagnostics(const rtsp_stream::source_backend_t &backend);
#endif
}  // namespace platf::source_backend
