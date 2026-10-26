/**
 * @file src/platform/windows/source_startup.h
 * @brief Inactive Windows startup inspection and ordinary-display exclusion.
 */
#pragma once

// standard includes
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#ifdef SUNSHINE_TESTS
  #include <functional>
  #include <string>
#endif

// platform includes
// WinSock2.h must precede Windows.h from libdisplaydevice.
#include <WinSock2.h>

// lib includes
#include <display_device/windows/types.h>

// local includes
#include "src/display_device.h"

namespace platf::source_startup {
  /** @brief Checked storage classification; pending does not confer ownership or driver authority. */
  enum class store_state_e {
    unchecked,
    absent,
    completed_empty,
    pending,
    unreadable,
    unsafe_path,
    malformed,
    transaction_residue
  };

  /** @brief Sanitized startup decision, separate from an owned journal's structural validity. */
  enum class disposition_e {
    uninspected,
    ordinary_clear,
    ordinary_recovery,
    owned_requires_authority,
    conflicting_recovery,
    recovery_proof_required,
    blocked
  };

  /** @brief Paths copied at construction; inspection requires absolute local, non-reparse locations. */
  struct paths_t {
    std::filesystem::path owned_journal;  ///< Canonical owned state and transaction-material namespace.
    std::filesystem::path ordinary_state;  ///< Existing display_device.state; never imported as Virtual intent.
  };

  /** @brief Copied result, available without storage, native APIs or scheduler access. */
  struct status_t {
    store_state_e owned {store_state_e::unchecked};  ///< Owned destination classification, including retained transaction material.
    store_state_e ordinary {store_state_e::unchecked};  ///< Existing library persistence classification.
    disposition_e disposition {disposition_e::uninspected};  ///< No Virtual authorization is produced by this stage.
    std::uint64_t owned_generation {};  ///< Structurally checked generation; not an adopted backend or capability.
    std::optional<display_device::SingleDisplayConfigState> ordinary_recovery;  ///< Exact parsed legacy state, copied before ordinary admission is exposed.
    bool exclusion_held {};  ///< Ordinary mutation remains excluded across refusal and failed future reconstruction.
    bool ordinary_released {};  ///< A fresh checked-safe transition released this coordinator's gate.
    bool recovery_proof_required {};  ///< Earlier owned/uncertain state cannot be discharged by later deletion or empty bytes.
  };

  /**
   * @brief Inspect startup state on the operation lane before initializing ordinary display management.
   * @details Construction and status are inert. Inspection acquires a sticky ordinary-only exclusion;
   * neither refusal nor destruction releases it. B recovery retains its own independent reservation.
   * Owned pending state remains unavailable until a later stage supplies independent provisioning
   * and reconstructs the actual episode. This component never initializes either settings manager,
   * repairs/deletes storage, manufactures authority or registers a production backend.
   */
  class coordinator_t {
  public:
    /** @brief Copy locations and allocate local state without native or storage access. */
    explicit coordinator_t(paths_t paths);
    /** @brief Release local state while DD_DATA retains any unresolved ordinary exclusion. */
    ~coordinator_t();
    coordinator_t(const coordinator_t &) = delete;
    coordinator_t &operator=(const coordinator_t &) = delete;

    /** @brief Acquire ordinary exclusion, fence previous ordinary callbacks and perform checked bounded reads. */
    status_t inspect();
    /**
     * @brief Reinspect and release only for checked-clear owned state and clear or valid legacy state.
     * @details Publishes the legacy recovery obligation before release. An existing manager or B
     * reservation refuses the transition. Pending owned intent needs later reconstruction/completion;
     * any earlier owned/uncertain observation requires a later recovery-proof seam, excluded from
     * this stage. Cached inspection, deletion, a caller Boolean or missing provisioning permits none.
     */
    bool release_for_ordinary();
    /** @brief Copy the last result without storage, scheduler or native access. */
    status_t status() const;

#ifdef SUNSHINE_TESTS
    /** @brief Storage outcomes injected only into the same classifier, never into production authority. */
    struct test_file_t {
      store_state_e access {store_state_e::absent};  ///< absent, completed_empty (read bytes), unreadable or unsafe_path.
      std::string bytes;  ///< Read bytes validated by the real pinned JSON converters.
    };

    /** @brief One deterministic observation of both stores and retained owned transaction material. */
    struct test_storage_t {
      test_file_t owned;  ///< Owned destination outcome.
      test_file_t ordinary;  ///< Legacy destination outcome.
      bool transaction_residue {};  ///< Retained owned .tmp-* or .backup-* material.
    };

    /** @brief Create an inert coordinator with a deterministic storage boundary and the real classifier. */
    static std::unique_ptr<coordinator_t> test_make(paths_t paths, std::function<test_storage_t()> read);
#endif

  private:
    struct state_t;
    status_t inspect_locked();
    std::unique_ptr<state_t> state_;
  };
}  // namespace platf::source_startup
