/**
 * @file src/process.cpp
 * @brief Definitions for the startup and shutdown of the apps started by a streaming Session.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/token_functions.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

// local includes
#include "app_catalog.h"
#include "config.h"
#include "crypto.h"
#include "display_device.h"
#include "httpcommon.h"
#include "input.h"
#include "logging.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "system_tray.h"
#include "utility.h"
#include "uuid.h"

#ifdef _WIN32
  // from_utf8() string conversion function
  #include "platform/windows/utf_utils.h"

  // _SH constants for _wfsopen()
  #include <share.h>
#endif

namespace proc {
  using namespace std::literals;
  namespace pt = boost::property_tree;

  proc_t proc;  ///< Global process registry used to track and terminate child processes.
#ifdef SUNSHINE_TESTS
  namespace {
    std::atomic_bool local_commands {false};  ///< Scoped real-child fixture mode; production preparation is the default.
    std::function<void(const boost::process::v1::environment &)> undo_environment_hook;  ///< Fixture observer installed only when no command consumer is active.
    std::function<lifetime_snapshot_t()> lifetime_snapshot_hook;  ///< Synthetic process lifetime for isolated owner cases.
    std::function<void()> execute_entry_hook;
    std::function<void()> legacy_restore_hook;
    std::function<void()> refresh_published_hook;
  }  // namespace

  namespace test_support {
    scoped_local_commands_t::scoped_local_commands_t(bool local):
        previous_ {local_commands.exchange(local)} {
    }

    scoped_local_commands_t::~scoped_local_commands_t() {
      local_commands.exchange(previous_);
    }

    void set_undo_environment_hook(std::function<void(const boost::process::v1::environment &)> hook) {
      undo_environment_hook = std::move(hook);
    }

    void set_lifetime_snapshot_hook(std::function<lifetime_snapshot_t()> hook) {
      lifetime_snapshot_hook = std::move(hook);
    }

    void set_execute_entry_hook(std::function<void()> hook) {
      execute_entry_hook = std::move(hook);
    }

    void set_legacy_restore_hook(std::function<void()> hook) {
      legacy_restore_hook = std::move(hook);
    }

    void set_refresh_published_hook(std::function<void()> hook) {
      refresh_published_hook = std::move(hook);
    }
  }  // namespace test_support
#endif

  /**
   * @brief RAII helper that runs shutdown cleanup when destroyed.
   */
  class deinit_t: public platf::deinit_t {
  public:
    /**
     * @brief Destroy the process subsystem deinitializer.
     */
    ~deinit_t() {
      proc.terminate();
    }
  };

  std::unique_ptr<platf::deinit_t> init() {
    return std::make_unique<deinit_t>();
  }

  /**
   * @brief Find an owned environment key using the platform's replacement semantics.
   */
  static std::string environment_key([[maybe_unused]] const boost::process::v1::environment &env, const std::string &name) {
#ifdef _WIN32
    const auto found = boost::range::find_if(env, [&](const auto &entry) {
      return boost::iequals(entry.get_name(), name);
    });
    if (found != env.cend()) {
      return found->get_name();
    }
#endif
    return name;
  }

  /**
   * @brief Assign an owned value without creating a differently cased Windows duplicate.
   */
  void assign_environment(boost::process::v1::environment &env, const std::string &name, const std::string &value) {
    env[environment_key(env, name)] = value;
  }

  /**
   * @brief Read search directories only from the prepared child environment.
   */
  static std::vector<boost::filesystem::path> environment_paths(const boost::process::v1::environment &env) {
    std::vector<boost::filesystem::path> paths;
    const auto entry = env.find(environment_key(env, "PATH"));
    if (entry != env.cend()) {
      for (const auto &path : entry->to_vector()) {
        paths.emplace_back(path);
      }
    }
    return paths;
  }

  /**
   * @brief Resolve a POSIX command prefix without changing the pinned Boost argument grammar or suffix.
   */
  static std::optional<std::string> configured_command(const std::string &command, [[maybe_unused]] const boost::process::v1::environment &env) {
#ifdef _WIN32
    // Windows keeps its existing association handling and runtime PATH search boundary.
    return command;
#else
    const auto begin = command.find_first_not_of(' ');
    if (begin == std::string::npos) {
      return std::nullopt;
    }
    auto end = begin;
    bool quoted = false;
    for (; end < command.size(); ++end) {
      if (command[end] == '"') {
        quoted = !quoted;
      }
      if (command[end] == ' ' && !quoted) {
        break;
      }
    }
    auto target = command.substr(begin, end - begin);
    if (target.size() >= 2 && target.front() == '"' && target.back() == '"') {
      target = target.substr(1, target.size() - 2);
    }
    boost::replace_all(target, "\\\"", "\"");
    const boost::filesystem::path executable {target};
    if (executable.has_parent_path() || executable.is_absolute()) {
      return command;
    }
    const auto found = boost::process::v1::search_path(executable, environment_paths(env));
    if (found.empty()) {
      return std::nullopt;
    }
    auto absolute = boost::filesystem::absolute(found).string();
    boost::replace_all(absolute, "\"", "\\\"");
    return command.substr(0, begin) + '"' + absolute + '"' + command.substr(end);
#endif
  }

  /**
   * @brief Execute using configured lookup while keeping environment references pinned by the process guard.
   */
  static boost::process::v1::child run_configured_command(bool elevated, bool interactive, const std::string &command, boost::filesystem::path &directory, const boost::process::v1::environment &env, FILE *output, std::error_code &error, boost::process::v1::group *group) {
    const auto resolved = configured_command(command, env);
    if (!resolved) {
      error = std::make_error_code(std::errc::no_such_file_or_directory);
      return {};
    }
    return platf::run_command(elevated, interactive, *resolved, directory, env, output, error, group);
  }

  void terminate_process_group(boost::process::v1::child &child_process, boost::process::v1::group &group, std::chrono::seconds exit_timeout) {
    if (group.valid() && platf::process_group_running((std::uintptr_t) group.native_handle())) {
      if (exit_timeout.count() > 0) {
        // Request processes in the group to exit gracefully
        if (platf::request_process_group_exit((std::uintptr_t) group.native_handle())) {
          // If the request was successful, wait for a little while for them to exit.
          BOOST_LOG(info) << "Successfully requested the app to exit. Waiting up to "sv << exit_timeout.count() << " seconds for it to close."sv;

          // group::wait_for() and similar functions are broken and deprecated, so we use a simple polling loop
          while (platf::process_group_running((std::uintptr_t) group.native_handle()) && (--exit_timeout).count() >= 0) {
            std::this_thread::sleep_for(1s);
          }

          if (exit_timeout.count() < 0) {
            BOOST_LOG(warning) << "App did not fully exit within the timeout. Terminating the app's remaining processes."sv;
          } else {
            BOOST_LOG(info) << "All app processes have successfully exited."sv;
          }
        } else {
          BOOST_LOG(info) << "App did not respond to a graceful termination request. Forcefully terminating the app's processes."sv;
        }
      } else {
        BOOST_LOG(info) << "No graceful exit timeout was specified for this app. Forcefully terminating the app's processes."sv;
      }

      // We always call terminate() even if we waited successfully for all processes above.
      // This ensures the process group state is consistent with the OS in boost.
      std::error_code ec;
      group.terminate(ec);
      group.detach();
    }

    if (child_process.valid()) {
      // avoid zombie process
      child_process.detach();
    }
  }

  /**
   * @brief Resolve the working directory for a configured command.
   *
   * @param cmd Command line to execute or inspect.
   * @param env Environment variables for the child process.
   * @return Directory used to launch the command, falling back to PATH lookup when needed.
   */
  boost::filesystem::path find_working_directory(const std::string &cmd, boost::process::v1::environment &env) {
    // Parse the raw command string into parts to get the actual command portion
    std::vector<std::string> parts;
    try {
#ifdef _WIN32
      parts = boost::program_options::split_winmain(cmd);
#else
      parts = boost::program_options::split_unix(cmd);
#endif
    } catch (boost::escaped_list_error &err) {
      BOOST_LOG(error) << "Boost failed to parse command ["sv << cmd << "] because " << err.what();
      return boost::filesystem::path();
    }
    if (parts.empty()) {
      BOOST_LOG(error) << "Unable to parse command: "sv << cmd;
      return boost::filesystem::path();
    }

    BOOST_LOG(debug) << "Parsed target ["sv << parts.at(0) << "] from command ["sv << cmd << ']';

    // If the target is a URL, don't parse any further here
    if (parts.at(0).contains("://")) {
      return boost::filesystem::path();
    }

    // If the cmd path is not an absolute path, resolve it using our PATH variable
    boost::filesystem::path cmd_path(parts.at(0));
    if (!cmd_path.is_absolute()) {
      cmd_path = boost::process::v1::search_path(parts.at(0), environment_paths(env));
      if (cmd_path.empty()) {
        BOOST_LOG(error) << "Unable to find executable ["sv << parts.at(0) << "]. Is it in the configured PATH?"sv;
        return boost::filesystem::path();
      }
    }

    BOOST_LOG(debug) << "Resolved target ["sv << parts.at(0) << "] to path ["sv << cmd_path << ']';

    // Now that we have a complete path, we can just use parent_path()
    return cmd_path.parent_path();
  }

  std::string prepare_command(const std::string &command) {
#ifdef SUNSHINE_TESTS
    if (local_commands.load()) {
      return command;
    }
#endif
#ifdef SUNSHINE_BUILD_FLATPAK
    constexpr auto host_command_prefix = "flatpak-spawn --host "sv;
    const auto stripped_command = boost::trim_copy(command);
    if (stripped_command.empty() || stripped_command.starts_with(host_command_prefix)) {
      if (!stripped_command.empty()) {
        BOOST_LOG(warning) << "Command ["sv << stripped_command << "] already includes [flatpak-spawn --host], which is no longer required for Flatpak builds."sv;
      }
      return stripped_command;
    }

    return std::string {host_command_prefix} + stripped_command;
#else
    return command;
#endif
  }

  int proc_t::execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session, std::uint64_t *created_instance) {
    if (created_instance) {
      *created_instance = 0;
    }
#ifdef SUNSHINE_TESTS
    if (execute_entry_hook) {
      execute_entry_hook();
    }
#endif
    const auto state = lock_state();
    if (launch_session->source_generation) {
      if (launch_session->source_cancel && launch_session->source_cancel->load()) {
        return 403;
      }
      const auto authority = nvhttp::current_client(launch_session->client_uuid, launch_session->client_cert);
      if (!authority || authority->revision != launch_session->authorization_revision || !permissions::allows(authority->permissions, permissions::launch)) {
        return 403;
      }
      const auto selected = std::find_if(_apps.begin(), _apps.end(), [app_id, &launch_session](const auto &app) {
        return app.id == std::to_string(app_id) && app.uuid == launch_session->app_uuid;
      });
      if (selected == _apps.end()) {
        return 404;
      }
      // The owner already owns display mutation for this replacement's preparation.
      _managed_source_instance = _instance;
      _managed_source_generation = launch_session->source_generation;
    }
    // Ensure starting from a clean slate
    terminate();

    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });

    if (iter == _apps.end()) {
      BOOST_LOG(error) << "Couldn't find app with ID ["sv << app_id << ']';
      return 404;
    }

    _app_id = app_id;
    static std::atomic_uint64_t next_instance {0};
    _instance = ++next_instance;
    _managed_source_instance = launch_session->source_generation ? _instance : 0;
    _managed_source_generation = launch_session->source_generation;
    _app = *iter;
    _app_prep_begin = std::begin(_app.prep_cmds);
    _app_prep_it = _app_prep_begin;
    const auto admission_cancelled = [&]() {
      if (!launch_session->source_generation) {
        return false;
      }
      const auto current = nvhttp::current_client(launch_session->client_uuid, launch_session->client_cert);
      return (launch_session->source_cancel && launch_session->source_cancel->load()) || !current || current->revision != launch_session->authorization_revision || !permissions::allows(current->permissions, permissions::launch);
    };

    // Add Stream-specific environment variables
    (*_env)["SUNSHINE_APP_ID"] = std::to_string(_app_id);
    (*_env)["SUNSHINE_APP_NAME"] = _app.name;
    (*_env)["SUNSHINE_CLIENT_NAME"] = launch_session->client_name;
    (*_env)["SUNSHINE_CLIENT_WIDTH"] = std::to_string(launch_session->width);
    (*_env)["SUNSHINE_CLIENT_HEIGHT"] = std::to_string(launch_session->height);
    (*_env)["SUNSHINE_CLIENT_FPS"] = std::to_string(launch_session->fps);
    (*_env)["SUNSHINE_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    (*_env)["SUNSHINE_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    (*_env)["SUNSHINE_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    (*_env)["SUNSHINE_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";
    int channelCount = launch_session->surround_info & 65535;
    switch (channelCount) {
      case 2:
        (*_env)["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        break;
      case 6:
        (*_env)["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        break;
      case 8:
        (*_env)["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        break;
    }
    (*_env)["SUNSHINE_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;

    if (!_app.output.empty() && _app.output != "null"sv) {
#ifdef _WIN32
      // fopen() interprets the filename as an ANSI string on Windows, so we must convert it
      // to UTF-16 and use the wchar_t variants for proper Unicode log file path support.
      auto woutput = utf_utils::from_utf8(_app.output);

      // Use _SH_DENYNO to allow us to open this log file again for writing even if it is
      // still open from a previous execution. This is required to handle the case of a
      // detached process executing again while the previous process is still running.
      _pipe.reset(_wfsopen(woutput.c_str(), L"a", _SH_DENYNO));
#else
      _pipe.reset(fopen(_app.output.c_str(), "a"));
#endif
    }

    std::error_code ec;
    // Executed when returning from function
    auto fg = util::fail_guard([this]() {
      terminate();
    });

    for (; _app_prep_it != std::end(_app.prep_cmds); ++_app_prep_it) {
      if (admission_cancelled()) {
        return 403;
      }
      auto &cmd = *_app_prep_it;

      // Skip empty commands
      if (cmd.do_cmd.empty()) {
        continue;
      }

      const auto command = prepare_command(cmd.do_cmd);
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(command, *_env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Do Cmd: ["sv << command << ']';
      auto child = run_configured_command(cmd.elevated, true, command, working_dir, *_env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(error) << "Couldn't run ["sv << command << "]: System: "sv << ec.message();
        // We don't want any prep commands failing launch of the desktop.
        // This is to prevent the issue where users reboot their PC and need to log in with Sunshine.
        // permission_denied is typically returned when the user impersonation fails, which can happen when user is not signed in yet.
        if (!(_app.cmd.empty() && ec == std::errc::permission_denied)) {
          return -1;
        }
      }

      child.wait(ec);
      if (ec) {
        BOOST_LOG(error) << '[' << command << "] wait failed with error code ["sv << ec << ']';
        return -1;
      }
      auto ret = child.exit_code();
      if (ret != 0) {
        BOOST_LOG(error) << '[' << command << "] exited with code ["sv << ret << ']';
        return -1;
      }
    }

    if (admission_cancelled()) {
      return 403;
    }
    for (auto &cmd : _app.detached) {
      if (admission_cancelled()) {
        return 403;
      }
      const auto command = prepare_command(cmd);
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(command, *_env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Spawning ["sv << command << "] in ["sv << working_dir << ']';
      auto child = run_configured_command(_app.elevated, true, command, working_dir, *_env, _pipe.get(), ec, nullptr);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't spawn ["sv << command << "]: System: "sv << ec.message();
      } else {
        child.detach();
      }
    }

    if (admission_cancelled()) {
      return 403;
    }
    if (_app.cmd.empty()) {
      BOOST_LOG(info) << "Executing [Desktop]"sv;
      placebo = true;
    } else {
      const auto command = prepare_command(_app.cmd);
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(command, *_env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing: ["sv << command << "] in ["sv << working_dir << ']';
      _process = run_configured_command(_app.elevated, true, command, working_dir, *_env, _pipe.get(), ec, &_process_group);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't run ["sv << command << "]: System: "sv << ec.message();
        return -1;
      }
    }

    _app_launch_time = std::chrono::steady_clock::now();

    fg.disable();

    if (created_instance) {
      *created_instance = _instance;
    }
    refresh_published();

    return 0;
  }

  int proc_t::running() {
    const auto state = lock_state();
    return observe_lifetime(true).appid;
  }

  lifetime_snapshot_t proc_t::observe_lifetime(bool cleanup) {
#ifdef SUNSHINE_TESTS
    if (lifetime_snapshot_hook) {
      auto observation = lifetime_snapshot_hook();
      if (!observation.tracked && !observation.placeholder) {
        observation.appid = 0;
        observation.app_uuid.clear();
      }
      return observation;
    }
#endif
#ifndef _WIN32
    // On POSIX OSes, we must periodically wait for our children to avoid
    // them becoming zombies. This must be synchronized carefully with
    // calls to bp::wait() and platf::process_group_running() which both
    // invoke waitpid() under the hood.
    auto reaper = util::fail_guard([]() {
      while (waitpid(-1, nullptr, WNOHANG) > 0);
    });
#endif

    if (_app_id <= 0) {
      return {0, {}, _instance, false, false};
    }
    if (placebo) {
      return {_app_id, _app.uuid, _instance, false, true};
    }
    // The same observation decides logical identity and genuine lifetime. Never reread the
    // child after running() and turn a successful launcher into a false terminal publication.
    const bool tracked = (_app.wait_all && _process_group && platf::process_group_running((std::uintptr_t) _process_group.native_handle())) || _process.running();
    if (tracked) {
      return {_app_id, _app.uuid, _instance, true, false};
    }
    if (_app.auto_detach && _process && _process.native_exit_code() == 0 && std::chrono::steady_clock::now() - _app_launch_time < 5s) {
      BOOST_LOG(info) << "App exited gracefully within 5 seconds of launch. Treating the app as a detached command."sv;
      BOOST_LOG(info) << "Adjust this behavior in the Applications tab or apps.json if this is not what you want."sv;
      placebo = true;
      return {_app_id, _app.uuid, _instance, false, true};
    }

    // Perform cleanup actions now if needed
    if (cleanup && _process) {
      BOOST_LOG(info) << "App exited with code ["sv << _process.native_exit_code() << ']';
      terminate();
    }

    return {0, {}, _instance, false, false};
  }

  lifetime_snapshot_t proc_t::lifetime_snapshot() {
    const auto state = lock_state();
    return observe_lifetime(false);
  }

  std::shared_ptr<const published_snapshot_t> proc_t::published_snapshot() const {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    return _publication->snapshot.load();
#else
    return std::atomic_load(&_publication->snapshot);
#endif
  }

  void proc_t::publish_snapshot(std::shared_ptr<const published_snapshot_t> snapshot) noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    _publication->snapshot.store(std::move(snapshot));
#else
    std::atomic_store(&_publication->snapshot, std::move(snapshot));
#endif
  }

  void proc_t::publish_operation(bool busy) {
    auto previous = published_snapshot();
    for (;;) {
      auto next = std::make_shared<published_snapshot_t>(*previous);
      next->operation_in_progress = busy;
      std::shared_ptr<const published_snapshot_t> immutable = std::move(next);
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
      const bool published = _publication->snapshot.compare_exchange_weak(previous, immutable);
#else
      const bool published = std::atomic_compare_exchange_weak(&_publication->snapshot, &previous, immutable);
#endif
      if (published) {
        return;
      }
    }
  }

  void proc_t::refresh_published() {
    const auto state = lock_state();
#ifdef SUNSHINE_TESTS
    if (_app_id > 0 && refresh_published_hook) {
      refresh_published_hook();
    }
#endif
    auto next = std::make_shared<published_snapshot_t>();
    next->lifetime = observe_lifetime(false);
    next->managed_source_instance = _managed_source_instance;
    next->managed_source_generation = _managed_source_generation;
    next->operation_in_progress = published_snapshot()->operation_in_progress;
    next->app_name = _app.name;
    next->apps = std::make_shared<const std::vector<ctx_t>>(_apps);
    next->catalog_path = _catalog_path;
    next->catalog_revision = _catalog_revision;
    publish_snapshot(std::move(next));
  }

  std::optional<source_claim_t> proc_t::claim_source(std::uint64_t instance, std::uint64_t generation) {
    const auto state = lock_state();
    if (!instance || !generation || _instance != instance || _app_id <= 0) {
      return std::nullopt;
    }
    const source_claim_t claim {instance, generation, _managed_source_instance, _managed_source_generation};
    auto publication = std::make_shared<published_snapshot_t>(*published_snapshot());
    publication->lifetime = observe_lifetime(false);
    publication->managed_source_instance = instance;
    publication->managed_source_generation = generation;
    _managed_source_instance = instance;
    _managed_source_generation = generation;
    publish_snapshot(std::move(publication));
    return claim;
  }

  bool proc_t::restore_source_claim(const source_claim_t &claim) {
    const auto state = lock_state();
    if (_instance != claim.instance || _managed_source_instance != claim.instance || _managed_source_generation != claim.generation) {
      return false;
    }
    auto publication = std::make_shared<published_snapshot_t>(*published_snapshot());
    publication->lifetime = observe_lifetime(false);
    publication->managed_source_instance = claim.previous_instance;
    publication->managed_source_generation = claim.previous_generation;
    _managed_source_instance = claim.previous_instance;
    _managed_source_generation = claim.previous_generation;
    publish_snapshot(std::move(publication));
    return true;
  }

  void proc_t::terminate() {
    const auto state = lock_state();
    const auto managed_instance = _managed_source_instance;
    _managed_source_instance = 0;
    _managed_source_generation = 0;
    input::terminate_gamepads();
    std::error_code ec;
    placebo = false;
    terminate_process_group(_process, _process_group, _app.exit_timeout);
    _process = boost::process::v1::child();
    _process_group = boost::process::v1::group();

    for (; _app_prep_it != _app_prep_begin; --_app_prep_it) {
      auto &cmd = *(_app_prep_it - 1);

      if (cmd.undo_cmd.empty()) {
        continue;
      }

      const auto command = prepare_command(cmd.undo_cmd);
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(command, *_env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Undo Cmd: ["sv << command << ']';
#ifdef SUNSHINE_TESTS
      if (undo_environment_hook) {
        undo_environment_hook(*_env);
      }
#endif
      auto child = run_configured_command(cmd.elevated, true, command, working_dir, *_env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(warning) << "System: "sv << ec.message();
        continue;
      }

      child.wait();
      auto ret = child.exit_code();

      if (ret != 0) {
        BOOST_LOG(warning) << "Return code ["sv << ret << ']';
      }
    }

    _pipe.reset();

    bool has_run = _app_id > 0;

    // Only show the Stopped notification if we actually have an app to stop
    // Since terminate() is always run when a new app has started
    if (proc::proc.get_last_run_app_name().length() > 0 && has_run) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_stopped(proc::proc.get_last_run_app_name());
#endif

      if (!managed_instance) {
#ifdef SUNSHINE_TESTS
        if (legacy_restore_hook) {
          legacy_restore_hook();
        } else
#endif
          display_device::revert_configuration();
      }
    }

    _app_id = -1;
    refresh_published();
    if (managed_instance) {
      try {
        rtsp_stream::application_exited(managed_instance);
      } catch (...) {
        // The owner's retained-app timer also observes exit if event allocation fails.
      }
    }
  }

  bool proc_t::terminate_instance(std::uint64_t instance) {
    const auto state = lock_state();
    if (!instance || _instance != instance || _app_id <= 0) {
      return false;
    }
    terminate();
    return true;
  }

  bool proc_t::terminate_source(std::uint64_t instance, std::uint64_t generation) {
    const auto state = lock_state();
    if (_app_id <= 0 || !((instance && _instance == instance) || (generation && _managed_source_generation == generation))) {
      return false;
    }
    terminate();
    return true;
  }

  state_guard_t proc_t::lock_state() const {
    return state_guard_t {_state_mutex};
  }

#ifdef SUNSHINE_TESTS
  const std::vector<ctx_t> &proc_t::get_apps() const {
    return _apps;
  }

  std::vector<ctx_t> &proc_t::get_apps() {
    return _apps;
  }

  boost::process::v1::environment proc_t::get_env() const {
    const auto state = lock_state();
    return *_env;
  }

  bool proc_t::wait_for_exit_for_testing(std::chrono::milliseconds timeout) {
    const auto state = lock_state();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (_process.running()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return false;
      }
      std::this_thread::sleep_until(std::min(deadline, now + std::chrono::milliseconds {1}));
    }
    return true;
  }
#endif

  // Gets application image from application list.
  // Returns image from assets directory if found there.
  // Returns default image if image configuration is not set.
  // Returns http content-type header compatible image type.
  std::string proc_t::get_app_image(int app_id) {
    const auto state = lock_state();
    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });
    auto app_image_path = iter == _apps.end() ? std::string() : iter->image_path;

    return validate_app_image_path(app_image_path);
  }

  std::string proc_t::get_last_run_app_name() {
    const auto state = lock_state();
    return _app.name;
  }

  std::pair<int, std::string> proc_t::active_identity() {
    const auto state = lock_state();
    const auto id = running();
    return {id, id > 0 ? _app.uuid : std::string {}};
  }

  std::vector<ctx_t> proc_t::apps_snapshot() const {
    const auto state = lock_state();
    return _apps;
  }

  void proc_t::prepare_apps_and_env(proc_t &other) const {
    const auto state = lock_state();
    if (_app_id > 0 || placebo) {
      for (const auto &var : *_env) {
        const auto name = var.get_name();
        if (name.starts_with("SUNSHINE_APP_") || name.starts_with("SUNSHINE_CLIENT_")) {
          (*other._env)[name] = var.to_string();
        }
      }
    }
  }

  void proc_t::publish_apps_and_env(proc_t &other, const state_guard_t &guard) noexcept {
    assert(guard.lifetime == _state_mutex && guard.lock.owns_lock());
    // Pointer/vector swaps avoid Boost.Process environment moves or allocation after commit.
    static_assert(noexcept(_env.swap(other._env)) && noexcept(_apps.swap(other._apps)));
    _env.swap(other._env);
    _apps.swap(other._apps);
  }

  void proc_t::publish_catalog_revision(std::string &path, std::string &revision, const state_guard_t &guard) noexcept {
    assert(guard.lifetime == _state_mutex && guard.lock.owns_lock());
    static_assert(noexcept(_catalog_path.swap(path)) && noexcept(_catalog_revision.swap(revision)));
    _catalog_path.swap(path);
    _catalog_revision.swap(revision);
  }

  bool proc_t::catalog_matches(const std::string &path, const std::string &revision) const {
    const auto state = lock_state();
    return _catalog_path == path && _catalog_revision == revision;
  }

  void proc_t::update_apps_and_env(proc_t &&other) {
    const auto state = lock_state();
    prepare_apps_and_env(other);
    publish_apps_and_env(other, state);
  }

  proc_t::~proc_t() {
    // It's not safe to call terminate() here because our proc_t is a static variable
    // that may be destroyed after the Boost loggers have been destroyed. Instead,
    // we return a deinit_t to main() to handle termination when we're exiting.
    // Once we reach this point here, termination must have already happened.
    assert(!placebo);
    assert(!_process.running());
  }

  /**
   * @brief Find the closing parenthesis for an environment-variable expression.
   *
   * @param begin Iterator positioned at the opening parenthesis.
   * @param end End iterator for the expression being scanned.
   * @return Iterator for the matching closing parenthesis, or end when unmatched.
   */
  std::string_view::iterator find_match(std::string_view::iterator begin, std::string_view::iterator end) {
    int stack = 0;

    --begin;
    do {
      ++begin;
      switch (*begin) {
        case '(':
          ++stack;
          break;
        case ')':
          --stack;
      }
    } while (begin != end && stack != 0);

    if (begin == end) {
      throw std::out_of_range("Missing closing bracket \')\'");
    }
    return begin;
  }

  /**
   * @brief Parse env val.
   *
   * @param env Environment variables for the child process.
   * @param val_raw Raw value that may contain $(NAME) substitutions.
   * @return Value with recognized environment-variable substitutions expanded.
   */
  std::string parse_env_val(boost::process::v1::environment &env, const std::string_view &val_raw) {
    auto pos = std::begin(val_raw);
    auto dollar = std::find(pos, std::end(val_raw), '$');

    std::stringstream ss;

    while (dollar != std::end(val_raw)) {
      auto next = dollar + 1;
      if (next != std::end(val_raw)) {
        switch (*next) {
          case '(':
            {
              ss.write(pos, (dollar - pos));
              auto var_begin = next + 1;
              auto var_end = find_match(next, std::end(val_raw));
              auto var_name = std::string {var_begin, var_end};

#ifdef _WIN32
              // Windows treats environment variable names in a case-insensitive manner,
              // so we look for a case-insensitive match here. This is critical for
              // correctly appending to PATH on Windows.
              auto itr = boost::range::find_if(std::as_const(env), [&var_name](const auto &e) {
                return boost::iequals(e.get_name(), var_name);
              });
              if (itr != env.cend()) {
                // Use an existing case-insensitive match
                var_name = itr->get_name();
              }
#endif

              ss << env[var_name].to_string();

              pos = var_end + 1;
              next = var_end;

              break;
            }
          case '$':
            ss.write(pos, (next - pos));
            pos = next + 1;
            ++next;
            break;
        }

        dollar = std::find(next, std::end(val_raw), '$');
      } else {
        dollar = next;
      }
    }

    ss.write(pos, (dollar - pos));

    return ss.str();
  }

  /**
   * @brief Validates a path whether it is a valid PNG.
   * @param path The path to the PNG file.
   * @return true if the file has a valid PNG signature, false otherwise.
   */
  bool check_valid_png(const std::filesystem::path &path) {
    // PNG signature as defined in PNG specification
    // http://www.libpng.org/pub/png/spec/1.2/PNG-Structure.html
    static constexpr std::array<unsigned char, 8> PNG_SIGNATURE = {
      0x89,
      0x50,
      0x4E,
      0x47,
      0x0D,
      0x0A,
      0x1A,
      0x0A
    };

    std::ifstream file(path, std::ios::binary);
    if (!file) {
      return false;
    }

    std::array<unsigned char, 8> header;
    file.read(reinterpret_cast<char *>(header.data()), 8);

    if (file.gcount() != 8) {
      return false;
    }

    return header == PNG_SIGNATURE;
  }

  /**
   * @brief Validate app image path.
   */
  std::string validate_app_image_path(std::string app_image_path) {
    if (app_image_path.empty()) {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // get the image extension and convert it to lowercase
    auto image_extension = std::filesystem::path(app_image_path).extension().string();
    boost::to_lower(image_extension);

    // return the default box image if the extension is not "png"
    if (image_extension != ".png") {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // check if image is in assets directory
    if (auto full_image_path = std::filesystem::path(SUNSHINE_ASSETS_DIR) / app_image_path; std::filesystem::exists(full_image_path)) {
      // Validate PNG signature
      if (!check_valid_png(full_image_path)) {
        BOOST_LOG(warning) << "Invalid PNG file at path ["sv << full_image_path << ']';
        return DEFAULT_APP_IMAGE_PATH;
      }
      return full_image_path.string();
    }

    if (app_image_path == "./assets/steam.png") {
      // handle old default steam image definition
      return SUNSHINE_ASSETS_DIR "/steam.png";
    }

    // check if specified image exists
    if (std::error_code code; !std::filesystem::exists(app_image_path, code)) {
      // return default box image if image does not exist
      BOOST_LOG(warning) << "Couldn't find app image at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    // Validate PNG signature
    if (!check_valid_png(app_image_path)) {
      BOOST_LOG(warning) << "Invalid PNG file at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    // image is a png, and not in assets directory
    // return only "content-type" http header compatible image type
    return app_image_path;
  }

  /**
   * @brief Calculate the SHA-256 digest for a file.
   *
   * @param filename File path whose contents should be hashed.
   * @return Lowercase hexadecimal SHA-256 digest, or std::nullopt on read/hash failure.
   */
  std::optional<std::string> calculate_sha256(const std::string &filename) {
    crypto::md_ctx_t ctx {EVP_MD_CTX_create()};
    if (!ctx) {
      return std::nullopt;
    }

    if (!EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr)) {
      return std::nullopt;
    }

    // Read file and update calculated SHA
    std::array<char, 1024 * 16> buf;
    std::ifstream file(filename, std::ifstream::binary);
    while (file.good()) {
      file.read(buf.data(), buf.size());
      if (!EVP_DigestUpdate(ctx.get(), buf.data(), file.gcount())) {
        return std::nullopt;
      }
    }
    file.close();

    std::array<unsigned char, SHA256_DIGEST_LENGTH> result;
    if (!EVP_DigestFinal_ex(ctx.get(), result.data(), nullptr)) {
      return std::nullopt;
    }

    // Transform byte-array to string
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &byte : result) {
      ss << std::setw(2) << (int) byte;
    }
    return ss.str();
  }

  /**
   * @brief Calculate the CRC-32 checksum for a string.
   *
   * @param input Bytes to include in the checksum.
   * @return CRC-32 value for the input bytes.
   */
  uint32_t calculate_crc32(const std::string &input) {
    boost::crc_32_type result;
    result.process_bytes(input.data(), input.length());
    return result.checksum();
  }

  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index) {
    // Generate id by hashing name with image data if present
    std::vector<std::string> to_hash;
    to_hash.push_back(app_name);
    auto file_path = validate_app_image_path(app_image_path);
    if (file_path != DEFAULT_APP_IMAGE_PATH) {
      auto file_hash = calculate_sha256(file_path);
      if (file_hash) {
        to_hash.push_back(file_hash.value());
      } else {
        // Fallback to just hashing image path
        to_hash.push_back(file_path);
      }
    }

    // Create combined strings for hash
    std::stringstream ss;
    for_each(to_hash.begin(), to_hash.end(), [&ss](const std::string &s) {
      ss << s;
    });
    auto input_no_index = ss.str();
    ss << index;
    auto input_with_index = ss.str();

    // CRC32 then truncate to signed 32-bit range due to client limitations
    auto id_no_index = std::to_string(catalog::legacy_crc_id(calculate_crc32(input_no_index)));
    auto id_with_index = std::to_string(catalog::legacy_crc_id(calculate_crc32(input_with_index)));

    return std::make_tuple(id_no_index, id_with_index);
  }

  /**
   * @brief Parse serialized text into the corresponding runtime representation.
   */
  static std::optional<proc::proc_t> parse_text(const std::string &bytes) {
    pt::ptree tree;

    try {
      std::istringstream input {bytes};
      pt::read_json(input, tree);

      auto &apps_node = tree.get_child("apps"s);
      auto &env_vars = tree.get_child("env"s);

      boost::process::v1::environment this_env {boost::this_process::environment()};

      for (auto &[name, val] : env_vars) {
        assign_environment(this_env, name, parse_env_val(this_env, val.get_value<std::string>()));
      }

      std::set<std::string, std::less<>> ids;
      std::vector<proc::ctx_t> apps;
      int i = 0;
      for (auto &[_, app_node] : apps_node) {
        proc::ctx_t ctx;

        auto prep_nodes_opt = app_node.get_child_optional("prep-cmd"s);
        auto detached_nodes_opt = app_node.get_child_optional("detached"s);
        auto exclude_global_prep = app_node.get_optional<bool>("exclude-global-prep-cmd"s);
        auto output = app_node.get_optional<std::string>("output"s);
        auto name = parse_env_val(this_env, app_node.get<std::string>("name"s));
        auto cmd = app_node.get_optional<std::string>("cmd"s);
        auto image_path = app_node.get_optional<std::string>("image-path"s);
        auto working_dir = app_node.get_optional<std::string>("working-dir"s);
        auto elevated = app_node.get_optional<bool>("elevated"s);
        auto auto_detach = app_node.get_optional<bool>("auto-detach"s);
        auto wait_all = app_node.get_optional<bool>("wait-all"s);
        auto exit_timeout = app_node.get_optional<int>("exit-timeout"s);

        std::vector<proc::cmd_t> prep_cmds;
        if (!exclude_global_prep.value_or(false)) {
          prep_cmds.reserve(config::sunshine.prep_cmds.size());
          for (auto &prep_cmd : config::sunshine.prep_cmds) {
            auto do_cmd = parse_env_val(this_env, prep_cmd.do_cmd);
            auto undo_cmd = parse_env_val(this_env, prep_cmd.undo_cmd);

            prep_cmds.emplace_back(
              std::move(do_cmd),
              std::move(undo_cmd),
              std::move(prep_cmd.elevated)
            );
          }
        }

        if (prep_nodes_opt) {
          auto &prep_nodes = *prep_nodes_opt;

          prep_cmds.reserve(prep_cmds.size() + prep_nodes.size());
          for (auto &[_, prep_node] : prep_nodes) {
            auto do_cmd = prep_node.get_optional<std::string>("do"s);
            auto undo_cmd = prep_node.get_optional<std::string>("undo"s);
            auto elevated = prep_node.get_optional<bool>("elevated");

            prep_cmds.emplace_back(
              parse_env_val(this_env, do_cmd.value_or("")),
              parse_env_val(this_env, undo_cmd.value_or("")),
              std::move(elevated.value_or(false))
            );
          }
        }

        std::vector<std::string> detached;
        if (detached_nodes_opt) {
          auto &detached_nodes = *detached_nodes_opt;

          detached.reserve(detached_nodes.size());
          for (auto &[_, detached_val] : detached_nodes) {
            detached.emplace_back(parse_env_val(this_env, detached_val.get_value<std::string>()));
          }
        }

        if (output) {
          ctx.output = parse_env_val(this_env, *output);
        }

        if (cmd) {
          ctx.cmd = parse_env_val(this_env, *cmd);
        }

        if (working_dir) {
          ctx.working_dir = parse_env_val(this_env, *working_dir);
#ifdef _WIN32
          // The working directory, unlike the command itself, should not be quoted
          // when it contains spaces. Unlike POSIX, Windows forbids quotes in paths,
          // so we can safely strip them all out here to avoid confusing the user.
          boost::erase_all(ctx.working_dir, "\"");
#endif
        }

        if (image_path) {
          ctx.image_path = parse_env_val(this_env, *image_path);
        }

        ctx.elevated = elevated.value_or(false);
        ctx.auto_detach = auto_detach.value_or(true);
        ctx.wait_all = wait_all.value_or(true);
        ctx.exit_timeout = std::chrono::seconds {exit_timeout.value_or(5)};

        if (const auto id = app_node.get_optional<std::string>("id")) {
          ctx.id = *id;
        } else {
          auto possible_ids = calculate_app_id(name, ctx.image_path, i);
          ctx.id = ids.contains(std::get<0>(possible_ids)) ? std::get<1>(possible_ids) : std::get<0>(possible_ids);
        }
        ++i;
        ids.insert(ctx.id);
        ctx.uuid = app_node.get<std::string>("uuid", "");
        ctx.force_virtual_display = app_node.get<bool>("force-virtual-display", false);

        ctx.name = std::move(name);
        ctx.prep_cmds = std::move(prep_cmds);
        ctx.detached = std::move(detached);

        apps.emplace_back(std::move(ctx));
      }

      return proc::proc_t {
        std::move(this_env),
        std::move(apps)
      };
    } catch (const std::exception &) {
      BOOST_LOG(error) << "Application runtime preparation failed";
    }

    return std::nullopt;
  }

  std::optional<proc::proc_t> parse(const std::string &file_name) {
    std::ifstream input {file_name, std::ios::binary};
    if (!input) {
      return std::nullopt;
    }
    const std::string bytes {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
    return input.bad() ? std::nullopt : parse_text(bytes);
  }

  namespace {
    /**
     * @brief Collect legacy image/environment-derived candidates separately from pure normalization.
     */
    catalog::allocation allocation_inputs(const catalog::json &tree, const std::pair<int, std::string> &active) {
      catalog::allocation inputs;
      inputs.uuid = []() {
        return uuid_util::uuid_t::generate().string();
      };
      inputs.candidate = [](const std::string &uuid) {
        return calculate_crc32(uuid);
      };
      if (active.first > 0 && !active.second.empty()) {
        inputs.active = active;
      }
      if (std::ranges::all_of(tree.at("apps"), [](const auto &app) {
            return app.contains("id");
          })) {
        return inputs;
      }
      boost::process::v1::environment env {boost::this_process::environment()};
      for (const auto &[name, value] : tree.at("env").items()) {
        assign_environment(env, name, parse_env_val(env, value.get<std::string>()));
      }
      std::set<std::string> old_ids;
      int index = 0;
      for (const auto &app : tree.at("apps")) {
        const auto name = parse_env_val(env, app.at("name").get<std::string>());
        const auto image = parse_env_val(env, app.value("image-path", ""));
        const auto [primary, indexed] = calculate_app_id(name, image, index++);
        const auto chosen = old_ids.contains(primary) ? indexed : primary;
        old_ids.insert(chosen);
        inputs.legacy.push_back(std::stoi(chosen));
      }
      return inputs;
    }

    /**
     * @brief Execute process lane -> process state -> state-file preparation, replacement and nonthrowing publication.
     */
    catalog_response transact_catalog_now(const std::string &file, const std::optional<std::string> &expected, const std::function<void(catalog::json &)> &mutation) {
      catalog_response response;
      try {
        // The owner orders this lane; only the worker acquires process state and the state-file guard.
        const auto state = proc.lock_state();
        auto path = std::filesystem::absolute(file).lexically_normal().string();
        std::string revision;
        std::optional<proc_t> prepared;
        std::shared_ptr<const published_snapshot_t> publication;
        bool ready = false;
        const auto active = proc.active_identity();
        const auto success = http::update_state_file_bytes(file, [&](const std::string &original) -> std::optional<std::string> {
          try {
            const auto current_revision = util::hex(crypto::hash(original)).to_string();
            if (expected && *expected != current_revision) {
              throw catalog::error {"Application catalog changed; reload before saving", 409};
            }
            auto tree = catalog::parse(original);
            catalog::validate(tree);
            const auto before = tree;
            catalog::normalize(tree, allocation_inputs(tree, active));
            if (mutation) {
              mutation(tree);
              catalog::normalize(tree, allocation_inputs(tree, active));
            }
            const auto changed = tree != before;
            auto bytes = changed ? tree.dump(4) + "\n" : original;
            revision = util::hex(crypto::hash(bytes)).to_string();
            if (!proc.catalog_matches(path, revision)) {
              prepared = parse_text(bytes);
              if (!prepared) {
                throw catalog::error {"Application runtime settings could not be prepared"};
              }
              proc.prepare_apps_and_env(*prepared);
            }
            // Response and every fallible runtime/environment allocation precede replacement.
            tree["revision"] = revision;
            tree["status"] = true;
            // Legacy scalar conversion is response-only; persisted typed/foreign values survive.
            for (auto &app : tree["apps"]) {
              for (const auto *key : {"elevated", "exclude-global-prep-cmd", "auto-detach", "wait-all"}) {
                if (app.contains(key) && app[key].is_string()) {
                  app[key] = app[key] == "true";
                }
              }
              if (app.contains("exit-timeout") && app["exit-timeout"].is_string()) {
                app["exit-timeout"] = std::stoi(app["exit-timeout"].get<std::string>());
              }
              if (app.contains("prep-cmd")) {
                for (auto &cmd : app["prep-cmd"]) {
                  if (cmd.contains("elevated") && cmd["elevated"].is_string()) {
                    cmd["elevated"] = cmd["elevated"] == "true";
                  }
                }
              }
            }
            response = {200, tree.dump()};
            // The process-state guard also fences claim changes. Preserve the exact published
            // claim in this preallocated catalog snapshot rather than creating a claim-free view.
            auto observation = std::make_shared<published_snapshot_t>(*proc.published_snapshot());
            observation->lifetime = proc.lifetime_snapshot();
            observation->app_name = proc.get_last_run_app_name();
            observation->apps = std::make_shared<const std::vector<ctx_t>>(prepared ? prepared->apps_snapshot() : proc.apps_snapshot());
            observation->catalog_path = path;
            observation->catalog_revision = revision;
            publication = std::move(observation);
            ready = true;
            return changed ? std::optional {std::move(bytes)} : std::nullopt;
          } catch (const catalog::error &error) {
            response = {error.status, catalog::json {{"status", false}, {"error", error.what()}}.dump()};
            return std::nullopt;
          }
        });
        if (!success || !ready) {
          // The core consumes exceptions to preserve existing permission-writer behavior.
          if (response.status == 200 || response.body.empty()) {
            response = {500, R"({"status":false,"error":"Application catalog could not be saved; previous runtime retained"})"};
          }
          return response;
        }
        if (prepared) {
          proc.publish_apps_and_env(*prepared, state);
        }
        proc.publish_catalog_revision(path, revision, state);
        proc.publish_snapshot(std::move(publication));
      } catch (const catalog::error &error) {
        response = {error.status, catalog::json {{"status", false}, {"error", error.what()}}.dump()};
      } catch (const std::exception &) {
        response = {500, R"({"status":false,"error":"Application catalog unavailable; previous runtime retained"})"};
      }
      return response;
    }

    catalog_response transact_catalog(std::string file, std::optional<std::string> expected, std::function<void(catalog::json &)> mutation) {
      if (rtsp_stream::on_process_worker()) {
        return transact_catalog_now(file, expected, mutation);
      }
      auto task = std::make_shared<std::packaged_task<catalog_response()>>([file = std::move(file), expected = std::move(expected), mutation = std::move(mutation)]() {
        return transact_catalog_now(file, expected, mutation);
      });
      auto result = task->get_future();
      rtsp_stream::catalog_operation([task]() {
        (*task)();
      });
      return result.get();
    }

    template<class Function>
    std::future<catalog_response> queue_catalog(Function function) {
      auto task = std::make_shared<std::packaged_task<catalog_response()>>(std::move(function));
      auto result = task->get_future();
      rtsp_stream::process_operation([task]() {
        (*task)();
      });
      return result;
    }
  }  // namespace

  catalog_response read_catalog(const std::string &file) {
    return transact_catalog(file, std::nullopt, {});
  }

  catalog_response read_catalog_now(const std::string &file) {
    return transact_catalog_now(file, std::nullopt, {});
  }

  std::future<catalog_response> read_catalog_async(std::string file) {
    return queue_catalog([file = std::move(file)]() {
      return read_catalog_now(file);
    });
  }

  std::future<catalog_response> save_catalog_app_async(std::string file, std::string revision, std::string edit) {
    return queue_catalog([file = std::move(file), revision = std::move(revision), edit = std::move(edit)]() {
      return save_catalog_app(file, revision, edit);
    });
  }

  std::future<catalog_response> delete_catalog_app_async(std::string file, std::string revision, std::string uuid) {
    return queue_catalog([file = std::move(file), revision = std::move(revision), uuid = std::move(uuid)]() {
      return delete_catalog_app(file, revision, uuid);
    });
  }

  catalog_response save_catalog_app(const std::string &file, const std::string &revision, const std::string &edit) {
    if (revision.empty()) {
      return {428, R"({"status":false,"error":"Application catalog revision is required"})"};
    }
    return transact_catalog(file, revision, [edit](auto &tree) {
      auto input = catalog::parse(edit);
      if (!input.is_object()) {
        throw catalog::error {"application: expected an object"};
      }
      auto &apps = tree["apps"];
      if (input.contains("uuid")) {
        const auto uuid = input["uuid"].is_string() ? catalog::canonical_uuid(input["uuid"].template get<std::string>()) : std::nullopt;
        if (!uuid) {
          throw catalog::error {"apps.uuid: invalid UUID"};
        }
        input["uuid"] = *uuid;
        const auto it = std::find_if(apps.begin(), apps.end(), [&](const auto &app) {
          return app["uuid"] == *uuid;
        });
        if (it == apps.end()) {
          throw catalog::error {"Application no longer exists", 404};
        }
        catalog::merge_edit(*it, input);
      } else {
        if (!input.contains("index") || input["index"] != -1 || input.contains("id")) {
          throw catalog::error {"New applications require server-generated identity"};
        }
        auto app = input;
        for (const auto *key : {"index", "revision", "launching", "status"}) {
          app.erase(key);
        }
        apps.push_back(std::move(app));
      }
      catalog::validate(tree);
      std::stable_sort(apps.begin(), apps.end(), [](const auto &left, const auto &right) {
        return left.at("name").template get<std::string>() < right.at("name").template get<std::string>();
      });
    });
  }

  catalog_response delete_catalog_app(const std::string &file, const std::string &revision, const std::string &uuid) {
    if (revision.empty()) {
      return {428, R"({"status":false,"error":"Application catalog revision is required"})"};
    }
    return transact_catalog(file, revision, [uuid](auto &tree) {
      const auto target = catalog::canonical_uuid(uuid);
      if (!target) {
        throw catalog::error {"apps.uuid: invalid UUID"};
      }
      auto &apps = tree["apps"];
      const auto it = std::find_if(apps.begin(), apps.end(), [&](const auto &app) {
        return app["uuid"] == *target;
      });
      if (it == apps.end()) {
        throw catalog::error {"Application no longer exists", 404};
      }
      apps.erase(it);
    });
  }

  void refresh(const std::string &file_name) {
    const auto result = read_catalog(file_name);
    if (result.status != 200) {
      BOOST_LOG(error) << "Application catalog refresh failed; previous runtime retained";
    }
  }
}  // namespace proc
