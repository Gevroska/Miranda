/**
 * @file src/platform/windows/recovery_storage.h
 * @brief Read-only observation of the machine recovery installation and its bound storage.
 */
#pragma once

#ifdef SUNSHINE_TESTS
  // WinSock2.h must precede headers that can include Windows.h.
  #include <WinSock2.h>
#endif

// standard includes
#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>

#ifdef SUNSHINE_TESTS
  #include <functional>
  #include <string>
  #include <utility>
  #include <Windows.h>
#endif

namespace platf::recovery_storage {
  /** @brief Mode declared in the installed manifest. */
  enum class mode_e : std::uint8_t {
    native_only = 1,
    recovery_managed = 2
  };

  /** @brief Reasons a positive installation observation could not be made. */
  enum class error_e {
    no_install_record,  ///< The product registry branch is absent; deployment history is unknown.
    incomplete,  ///< A required part of the installation record or directory chain is missing.
    unsafe,  ///< The manifest, object binding, namespace or protection policy was rejected.
    changed,  ///< A retained observation and fresh observations did not agree.
    unreadable  ///< A required read or platform facility failed.
  };

  /** @brief NTFS identity as reported by FileIdInfo. */
  struct directory_id_t {
    std::uint64_t volume_serial {};  ///< Full volume serial from FileIdInfo.
    std::array<std::uint8_t, 16> file_id {};  ///< FileIdInfo identifier in byte order returned by Windows.
    bool operator==(const directory_id_t &) const = default;
  };

  /** @brief Fields read from the validated version-one installation manifest. */
  struct installation_t {
    mode_e mode;  ///< Installer-declared deployment mode.
    std::array<std::uint8_t, 16> deployment_id;  ///< Nonzero opaque UUID bytes.
    directory_id_t directory;  ///< Binding to the actual recovery directory.
    bool operator==(const installation_t &) const = default;
  };

  /**
   * @brief Move-only evidence from checked registry and directory handles.
   * @details The journal path is derived from the checked directory. The observation does not
   * inspect the journal, serialize an installer, grant writes or authorize startup or driver use.
   */
  class observation_t {
  public:
    ~observation_t();
    observation_t(observation_t &&) noexcept;
    observation_t &operator=(observation_t &&) noexcept;
    observation_t(const observation_t &) = delete;
    observation_t &operator=(const observation_t &) = delete;

    /** @brief Return the observed manifest fields. Requires a live, non-moved-from observation. */
    const installation_t &installation() const;

    /** @brief Return the derived journal path. Requires a live, non-moved-from observation. */
    const std::filesystem::path &journal_path() const;

    /**
     * @brief Reopen the fixed origins and compare the fresh and retained objects and state.
     * @details A failed recheck yields changed. The checks are bounded observations of mutable
     * state, so a later consumer must establish installer serialization when it requires it.
     */
    std::expected<void, error_e> recheck() const;

#ifdef SUNSHINE_TESTS
    /** @brief Borrow the checked terminal registry key and directory for native assertions. */
    std::pair<HKEY, HANDLE> test_handles() const;
#endif

  private:
    friend struct inspector_t;
    struct state_t;
    explicit observation_t(std::unique_ptr<state_t> state);
    std::unique_ptr<state_t> state_;
  };

  /** @brief Inspect the fixed machine installation using only read and security-query access. */
  std::expected<observation_t, error_e> inspect_machine_storage();

#ifdef SUNSHINE_TESTS
  namespace test {
    /** @brief Substitute private fixture roots and, when required, the fixture owner's SID. */
    struct roots_t {
      std::wstring registry_anchor;  ///< Single existing component directly under HKCU.
      std::filesystem::path program_data;  ///< Real fixture directory on a local fixed NTFS drive.
      std::wstring fixture_owner_sid;  ///< Optional fixture SID replacing SYSTEM in product ACLs.
    };

    /** @brief Run the real reader on private roots with an optional between-scan race hook. */
    std::expected<observation_t, error_e> inspect(const roots_t &roots, const std::function<void()> &between_scans = {});
  }  // namespace test
#endif
}  // namespace platf::recovery_storage
