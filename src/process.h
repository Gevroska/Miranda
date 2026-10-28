/**
 * @file src/process.h
 * @brief Declarations for the startup and shutdown of the apps started by a streaming Session.
 */
#pragma once

#ifndef __kernel_entry
  /**
   * @def __kernel_entry
   * @brief Macro for kernel entry.
   */
  #define __kernel_entry
#endif

// standard includes
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

// local includes
#include "config.h"
#include "platform/common.h"
#include "rtsp.h"
#include "src/boost_process_compat.h"
#include "utility.h"

/**
 * @def DEFAULT_APP_IMAGE_PATH
 * @brief Macro for DEFAULT APP IMAGE PATH.
 */
#define DEFAULT_APP_IMAGE_PATH SUNSHINE_ASSETS_DIR "/box.png"

namespace proc {
  /**
   * @brief Retain the process-state mutex itself as well as its lock across moves of proc_t storage.
   */
  struct state_guard_t {
    std::shared_ptr<std::recursive_mutex> lifetime;  ///< Stable synchronization storage retained until unlock.
    std::unique_lock<std::recursive_mutex> lock;  ///< Released before the retained mutex is destroyed.

    /**
     * @brief Retain synchronization storage before acquiring its lock.
     * @param mutex Stable storage shared with the process manager.
     */
    explicit state_guard_t(std::shared_ptr<std::recursive_mutex> mutex):
        lifetime(std::move(mutex)),
        lock(*lifetime) {}
  };

  /**
   * @brief Boost.Process pipe stream used for child-process I/O.
   */
  using file_t = util::safe_ptr_v2<FILE, int, &fclose>;

  /**
   * @brief Parsed command arguments used when launching a child process.
   */
  using cmd_t = config::prep_cmd_t;

  /**
   * pre_cmds -- guaranteed to be executed unless any of the commands fail.
   * detached -- commands detached from Sunshine
   * cmd -- Runs indefinitely until:
   *    No session is running and a different set of commands it to be executed
   *    Command exits
   * working_dir -- the process working directory. This is required for some games to run properly.
   * cmd_output --
   *    empty    -- The output of the commands are appended to the output of sunshine
   *    "null"   -- The output of the commands are discarded
   *    filename -- The output of the commands are appended to filename
   */
  struct ctx_t {
    std::vector<cmd_t> prep_cmds;  ///< Prep cmds.

    /**
     * Some applications, such as Steam, either exit quickly, or keep running indefinitely.
     *
     * Apps that launch normal child processes and terminate will be handled by the process
     * grouping logic (wait_all). However, apps that launch child processes indirectly or
     * into another process group (such as UWP apps) can only be handled by the auto-detach
     * heuristic which catches processes that exit 0 very quickly, but we won't have proper
     * process tracking for those.
     *
     * For cases where users just want to kick off a background process and never manage the
     * lifetime of that process, they can use detached commands for that.
     */
    std::vector<std::string> detached;

    std::string name;  ///< Human-readable name for this item.
    std::string cmd;  ///< Command line used to launch the application.
    std::string working_dir;  ///< Working dir.
    std::string output;  ///< Captured output from the launched process.
    std::string image_path;  ///< Image path.
    std::string id;  ///< Stable identifier for the configured application.
    std::string uuid;  ///< Durable application UUID, separate from paired-client authority.
    bool force_virtual_display = false;  ///< Saved additive virtual-source preference, applied only to fresh sources.
    bool elevated;  ///< Whether the process should be launched elevated.
    bool auto_detach;  ///< Whether the process should detach automatically.
    bool wait_all;  ///< Whether Sunshine waits for all child processes.
    std::chrono::seconds exit_timeout;  ///< Exit timeout.
  };

  /** @brief Logical application identity with independent genuine process lifetime for source retention. */
  struct lifetime_snapshot_t {
    int appid {};  ///< Logical active application selection, or zero when stopped.
    std::string app_uuid;  ///< Stable application identity.
    std::uint64_t instance {};  ///< Process incarnation; app UUID reuse cannot match another execution.
    bool tracked = false;  ///< Main process or selected process group is genuinely live.
    bool placeholder = false;  ///< Placebo/auto-detached state never retains a source.
  };

  /** @brief Exact management transition that can be restored only while its incarnation and generation still match. */
  struct source_claim_t {
    std::uint64_t instance {};  ///< Exact current process incarnation.
    std::uint64_t generation {};  ///< Source generation issuing the claim.
    std::uint64_t previous_instance {};  ///< Previous cleanup owner, or zero for legacy cleanup.
    std::uint64_t previous_generation {};  ///< Previous source generation, retained for exact rollback.
  };

  /** @brief Immutable observation readable without the process mutex, including while a configured command waits. */
  struct published_snapshot_t {
    lifetime_snapshot_t lifetime;  ///< Last confirmed process/group observation.
    std::uint64_t managed_source_instance {};  ///< Actual retained source cleanup claim, independent of process liveness.
    std::uint64_t managed_source_generation {};  ///< Exact current claim generation, including an otherwise idle process.
    bool operation_in_progress = false;  ///< A configured command may currently hold process state.
    std::string app_name;  ///< Copied application name for platform notifications.
    std::shared_ptr<const std::vector<ctx_t>> apps = std::make_shared<const std::vector<ctx_t>>();  ///< Immutable published catalog.
    std::string catalog_path;  ///< Path of the successfully published catalog.
    std::string catalog_revision;  ///< Exact-content catalog precondition.
  };

  /**
   * @brief Tracks launched child processes and terminates them during shutdown.
   * @note Move process managers only while their consumers are quiescent; retained guards keep mutex storage alive.
   */
  class proc_t {
  public:
    KITTY_DEFAULT_CONSTR_MOVE_THROW(proc_t)

    /**
     * @brief Construct a process manager.
     *
     * @param env Environment used when launching processes.
     * @param apps Application launch contexts.
     */
    proc_t(
      boost::process::v1::environment &&env,
      std::vector<ctx_t> &&apps
    ):
        _app_id(0),
        _env(std::make_unique<boost::process::v1::environment>(std::move(env))),
        _apps(std::move(apps)) {
      refresh_published();
    }

    /**
     * @brief Launch the configured application process.
     *
     * @param app_id App ID.
     * @param launch_session Launch session.
     * @param created_instance Optional exact incarnation successfully created by this attempt; zero on rejection/failure.
     * @return Process exit code or launch error status.
     */
    int execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session, std::uint64_t *created_instance = nullptr);

    /**
     * @return `_app_id` if a process is running, otherwise returns `0`
     */
    int running();
    /** @brief Observe logical running state and genuine tracking together, resolving auto-detach without cleanup or owner dispatch. */
    lifetime_snapshot_t lifetime_snapshot();
    /** @brief Copy the published pointer; never takes process state or waits for configured commands. */
    std::shared_ptr<const published_snapshot_t> published_snapshot() const;
    /** @brief Process-worker observation after transitions; caller may already hold the recursive process guard. */
    void refresh_published();
    /** @brief Publish only the lane-busy marker without taking process state. */
    void publish_operation(bool busy);
    /** @brief Publish a preallocated complete observation after a catalog commit without allocation. */
    void publish_snapshot(std::shared_ptr<const published_snapshot_t> snapshot) noexcept;
    /** @brief Claim only an exact current incarnation after backend reservation succeeds. */
    std::optional<source_claim_t> claim_source(std::uint64_t instance, std::uint64_t generation);
    /** @brief Restore a claim's previous state only if no replacement or later claimant has superseded it. */
    bool restore_source_claim(const source_claim_t &claim);

    /**
     * @brief Acquire process state locally; never hold this guard while dispatching to the RTSP owner or joining a stream worker.
     */
    state_guard_t lock_state() const;

    /**
     * @brief Copy active identity under process state; call on the process lane or quiescent legacy owner only.
     */
    std::pair<int, std::string> active_identity();

    /**
     * @brief Return a value snapshot for serialization at the owner boundary.
     */
    std::vector<ctx_t> apps_snapshot() const;

    ~proc_t();

#ifdef SUNSHINE_TESTS
    /**
     * @brief Return a fixture-only reference; callers must keep the manager quiescent throughout use.
     */
    const std::vector<ctx_t> &get_apps() const;
    /**
     * @brief Return a mutable fixture-only reference under the same quiescence requirement.
     */
    std::vector<ctx_t> &get_apps();
#endif
    /**
     * @brief Get app image.
     *
     * @param app_id App ID.
     * @return Validated image path for the requested application.
     */
    std::string get_app_image(int app_id);
    /**
     * @brief Get last run app name.
     *
     * @return Name of the most recently launched application.
     */
    std::string get_last_run_app_name();
    /**
     * @brief Terminate the launched application process.
     */
    void terminate();
    /** @brief Match and terminate one exact incarnation under the same process-state guard. */
    bool terminate_instance(std::uint64_t instance);
    /** @brief Stop the captured incarnation or the pending incarnation owned by the captured source generation. */
    bool terminate_source(std::uint64_t instance, std::uint64_t generation);

    /**
     * @brief Update the cached app list and environment without disturbing running process state.
     *
     * This replaces only the application configuration data (_apps and _env) from a newly parsed
     * proc_t, preserving the current _app_id, _process, _process_group, and other fields that
     * track an in-flight streaming session.
     *
     * @param other The newly parsed proc_t whose apps and env will be moved into this instance.
     */
    void update_apps_and_env(proc_t &&other);

    /**
     * @brief Prebuild session-variable merging before committing a catalog transaction.
     */
    void prepare_apps_and_env(proc_t &other) const;

    /**
     * @brief Publish prepared catalog storage without allocation or changing the active context.
     */
    void publish_apps_and_env(proc_t &other, const state_guard_t &guard) noexcept;

    /**
     * @brief Publish prepared content-cache identity without allocation.
     */
    void publish_catalog_revision(std::string &path, std::string &revision, const state_guard_t &guard) noexcept;

    /**
     * @brief Check the last successfully published path/content pair.
     */
    bool catalog_matches(const std::string &path, const std::string &revision) const;

#ifdef SUNSHINE_TESTS
    /**
     * @brief Get the current process environment (strictly for unit tests).
     */
    boost::process::v1::environment get_env() const;

    /**
     * @brief Wait only for the synthetic main child in the exited-process regression, without triggering cleanup.
     */
    bool wait_for_exit_for_testing(std::chrono::milliseconds timeout);
#endif

  private:
    /** @brief Resolve one guarded liveness observation; cleanup is permitted only on the process worker or quiescent legacy caller. */
    lifetime_snapshot_t observe_lifetime(bool cleanup);

    int _app_id = 0;
    std::uint64_t _instance = 0;  ///< New value for every actual execution attempt.
    std::uint64_t _managed_source_instance = 0;  ///< Matching cleanup skips legacy direct display restoration.
    std::uint64_t _managed_source_generation = 0;  ///< A failed older admission cannot unmark a later claimant.

    struct publication_t {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
      std::atomic<std::shared_ptr<const published_snapshot_t>> snapshot {std::make_shared<const published_snapshot_t>()};
#else
      std::shared_ptr<const published_snapshot_t> snapshot {std::make_shared<const published_snapshot_t>()};  ///< Accessed only through shared-pointer atomic operations.
#endif
    };

    std::shared_ptr<publication_t> _publication = std::make_shared<publication_t>();

    std::shared_ptr<std::recursive_mutex> _state_mutex = std::make_shared<std::recursive_mutex>();  ///< Process consumers and catalog publication share this guard; never used to wait for the owner.

    std::unique_ptr<boost::process::v1::environment> _env = std::make_unique<boost::process::v1::environment>();
    std::vector<ctx_t> _apps;
    std::string _catalog_path;  ///< Canonical path of the successful content publication.
    std::string _catalog_revision;  ///< SHA-256 of exactly the committed bytes.
    ctx_t _app;
    std::chrono::steady_clock::time_point _app_launch_time;

    // If no command associated with _app_id, yet it's still running
    bool placebo {};

    boost::process::v1::child _process;
    boost::process::v1::group _process_group;

    file_t _pipe;
    std::vector<cmd_t>::const_iterator _app_prep_it;
    std::vector<cmd_t>::const_iterator _app_prep_begin;
  };

  /**
   * @brief Calculate a stable id based on name and image data
   * @return Tuple of id calculated without index (for use if no collision) and one with.
   *
   * @param app_name App name.
   * @param app_image_path App image path.
   * @param index Zero-based index of the item being addressed.
   */
  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index);

  /**
   * @brief Prepare a configured command for execution.
   *
   * @param command Configured command line.
   * @return Command line with any package-specific launcher prefix applied.
   */
  std::string prepare_command(const std::string &command);

  bool check_valid_png(const std::filesystem::path &path);
  /**
   * @brief Validate app image path.
   *
   * @param app_image_path Candidate image path from the application configuration.
   * @return Existing PNG path, or the default application image when validation fails.
   */
  std::string validate_app_image_path(std::string app_image_path);
  /**
   * @brief Refresh cached platform state from the operating system.
   *
   * @param file_name File name.
   */
  void refresh(const std::string &file_name);
  /**
   * @brief Parse serialized text into the corresponding runtime representation.
   *
   * @param file_name File name.
   * @return Parsed value or parse status.
   */
  std::optional<proc::proc_t> parse(const std::string &file_name);

  /**
   * @brief Prebuilt response returned by a serialized catalog operation.
   */
  struct catalog_response {
    int status = 500;  ///< Explicit HTTP outcome.
    std::string body;  ///< Serialized safe response, prepared before replacement.
  };

  /**
   * @brief Normalize, commit and return the typed application catalog on the owner.
   */
  catalog_response read_catalog(const std::string &file);
  /** @brief Queue an owned catalog read/normalization; callers await outside the owner. */
  std::future<catalog_response> read_catalog_async(std::string file);
  /** @brief Worker-only normalization core used by the admission refresh continuation. */
  catalog_response read_catalog_now(const std::string &file);
  /** @brief Queue an owned catalog mutation preserving its exact-content precondition and environment publication. */
  std::future<catalog_response> save_catalog_app_async(std::string file, std::string revision, std::string edit);
  /** @brief Queue an owned UUID deletion preserving its exact-content precondition. */
  std::future<catalog_response> delete_catalog_app_async(std::string file, std::string revision, std::string uuid);
  /**
   * @brief Create or edit an application using immutable UUID identity and an exact-content precondition.
   */
  catalog_response save_catalog_app(const std::string &file, const std::string &revision, const std::string &edit);
  /**
   * @brief Delete a UUID target while retaining its numeric allocation and any active copied context.
   */
  catalog_response delete_catalog_app(const std::string &file, const std::string &revision, const std::string &uuid);

#ifdef SUNSHINE_TESTS
  namespace test_support {
    /**
     * @brief Keep real fixture commands in the test process's execution environment.
     * @details The process-wide mode is read atomically by workers. Serialize scope ownership and nest
     * scopes lexically; retain the scope until all command workers and undo cleanup have completed.
     */
    class scoped_local_commands_t {
    public:
      /** @brief Select local execution, or temporarily restore host preparation in a nested scope. */
      explicit scoped_local_commands_t(bool local = true);
      /** @brief Restore the preceding command-preparation mode, including during stack unwinding. */
      ~scoped_local_commands_t();
      scoped_local_commands_t(const scoped_local_commands_t &) = delete;
      scoped_local_commands_t &operator=(const scoped_local_commands_t &) = delete;

    private:
      bool previous_;  ///< Command-preparation mode restored when this scope ends.
    };

    /**
     * @brief Observe a real undo consumer while the process state and referenced environment remain locked.
     * @param hook Fixture observer; empty restores production behavior. Install/reset only with no active consumer.
     */
    void set_undo_environment_hook(std::function<void(const boost::process::v1::environment &)> hook);
    /** @brief Supply synthetic liveness for owner component cases; install/reset only with consumers quiescent. */
    void set_lifetime_snapshot_hook(std::function<lifetime_snapshot_t()> hook);
    /** @brief Observe the actual execute boundary before acquiring process state; install only while quiescent. */
    void set_execute_entry_hook(std::function<void()> hook);
    /** @brief Observe an actual unmanaged termination's restoration responsibility without configuring a display. */
    void set_legacy_restore_hook(std::function<void()> hook);
    /** @brief Pause an active application's publication before its authoritative observation; install/reset only while quiescent. */
    void set_refresh_published_hook(std::function<void()> hook);
  }  // namespace test_support
#endif

  /**
   * @brief Initialize proc functions
   * @return Unique pointer to `deinit_t` to manage cleanup
   */
  std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Terminates all child processes in a process group.
   * @param child_process The child process itself.
   * @param group The group of all children in the process tree.
   * @param exit_timeout The timeout to wait for the process group to gracefully exit.
   */
  void terminate_process_group(boost::process::v1::child &child_process, boost::process::v1::group &group, std::chrono::seconds exit_timeout);

  extern proc_t proc;
}  // namespace proc
