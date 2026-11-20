/**
 * @file tests/unit/test_process.cpp
 * @brief Test src/process.* functions.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <stdexcept>
#include <thread>

// lib imports
#include <boost/algorithm/string.hpp>
#include <boost/process/v1.hpp>

// local includes
#include <src/app_catalog.h>
#include <src/display_device.h>
#include <src/httpcommon.h>
#include <src/logging.h>
#include <src/process.h>
#include <src/stream.h>
#include <src/utility.h>

namespace fs = std::filesystem;

namespace proc {
  /**
   * @brief Resolve a command's working directory, returning an empty path for URLs.
   *
   * @param cmd Command line to inspect.
   * @param env Environment used for executable lookup.
   * @return Working directory, or an empty path when the command is a URL.
   */
  boost::filesystem::path find_working_directory(const std::string &cmd, boost::process::v1::environment &env);

  /**
   * @brief Expand environment references while preserving Windows variable-name casing.
   *
   * @param env Environment whose variables are expanded.
   * @param val_raw Text containing variable references.
   * @return Text with environment references expanded.
   */
  std::string parse_env_val(boost::process::v1::environment &env, const std::string_view &val_raw);

  /**
   * @brief Assign configured values with the same platform semantics as both preparation paths.
   */
  void assign_environment(boost::process::v1::environment &env, const std::string &name, const std::string &value);
}  // namespace proc

#ifdef _WIN32
TEST(ProcessTest, ExpandsEnvironmentNamesWithoutChangingTheirCase) {
  boost::process::v1::environment env {boost::this_process::environment()};
  const std::string key = "SUNSHINE_RANGE_SEARCH_REGRESSION";
  env[key] = "case-preserved";

  EXPECT_EQ(proc::parse_env_val(env, "before-$(sunshine_range_search_regression)-after"), "before-case-preserved-after");
  EXPECT_EQ(proc::parse_env_val(env, "$(SUNSHINE_RANGE_SEARCH_REGRESSION)"), "case-preserved");
}
#endif

TEST(ProcessTest, UrlCommandsDoNotResolveAnExecutableWorkingDirectory) {
  boost::process::v1::environment env;
  EXPECT_TRUE(proc::find_working_directory("https://example.com", env).empty());
  EXPECT_TRUE(proc::find_working_directory("steam://run/123", env).empty());
  EXPECT_TRUE(proc::find_working_directory("custom+scheme://host/path", env).empty());
}

TEST(ProcessTest, PrepareCommand) {
#ifdef SUNSHINE_BUILD_FLATPAK
  EXPECT_EQ(proc::prepare_command("steam"), "flatpak-spawn --host steam");
  EXPECT_EQ(proc::prepare_command("flatpak-spawn --host steam"), "flatpak-spawn --host steam");
  EXPECT_EQ(proc::prepare_command("  flatpak-spawn --host steam  "), "flatpak-spawn --host steam");
  EXPECT_EQ(proc::prepare_command("  steam  "), "flatpak-spawn --host steam");
  EXPECT_EQ(proc::prepare_command(""), "");
  EXPECT_EQ(proc::prepare_command("  \t"), "");
#else
  EXPECT_EQ(proc::prepare_command("steam"), "steam");
  EXPECT_EQ(proc::prepare_command("  steam  "), "  steam  ");
  EXPECT_EQ(proc::prepare_command("flatpak-spawn --host steam"), "flatpak-spawn --host steam");
  EXPECT_EQ(proc::prepare_command(""), "");
#endif
}

TEST(ProcessTest, LocalCommandScopeRestoresNestedPreparationModes) {
  const std::string command = "/bin/sh \"fixture script.sh\"";
#ifdef SUNSHINE_BUILD_FLATPAK
  const auto default_command = "flatpak-spawn --host " + command;
#else
  const auto default_command = command;
#endif
  EXPECT_EQ(proc::prepare_command(command), default_command);
  {
    proc::test_support::scoped_local_commands_t local;
    EXPECT_EQ(proc::prepare_command(command), command);
    {
      proc::test_support::scoped_local_commands_t nested;
      EXPECT_EQ(proc::prepare_command(command), command);
    }
    EXPECT_EQ(proc::prepare_command(command), command);
    {
      proc::test_support::scoped_local_commands_t host {false};
      EXPECT_EQ(proc::prepare_command(command), default_command);
    }
    EXPECT_EQ(proc::prepare_command(command), command);
  }
  EXPECT_EQ(proc::prepare_command(command), default_command);
}

TEST(ProcessTest, LocalCommandScopeRestoresPreparationAfterException) {
  const auto default_command = proc::prepare_command("fixture");
  EXPECT_THROW(
    {
      proc::test_support::scoped_local_commands_t local;
      EXPECT_EQ(proc::prepare_command("fixture"), "fixture");
      throw std::runtime_error {"fixture failure"};
    },
    std::runtime_error
  );
  EXPECT_EQ(proc::prepare_command("fixture"), default_command);
}

class ProcessPNGTest: public BaseTest {  // NOSONAR(cpp:S3656): protected members are intentional for test fixture subclassing
protected:
  void SetUp() override {
    BaseTest::SetUp();
    // Create test directory
    test_dir = fs::temp_directory_path() / "sunshine_process_png_test";  // NOSONAR(cpp:S5443): safe for tests
    fs::create_directories(test_dir);
  }

  void TearDown() override {
    // Clean up test directory
    if (fs::exists(test_dir)) {
      fs::remove_all(test_dir);
    }
    BaseTest::TearDown();
  }

  // Helper function to create a file with specific content
  void createTestFile(const fs::path &path, const std::vector<unsigned char> &content) const {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(content.data()), content.size());
    file.close();
  }

  fs::path test_dir;
};

// Tests for check_valid_png function
TEST_F(ProcessPNGTest, CheckValidPNG_ValidSignature) {
  // Valid PNG signature
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,  // PNG signature
    // Add some dummy data to make it more realistic
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "valid.png";
  createTestFile(test_file, valid_png_data);

  EXPECT_TRUE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_WrongSignature) {
  // Invalid PNG signature (wrong magic bytes)
  const std::vector<unsigned char> invalid_png_data = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "invalid.png";
  createTestFile(test_file, invalid_png_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_TooShort) {
  // File too short (less than 8 bytes)
  const std::vector<unsigned char> short_data = {
    0x89,
    0x50,
    0x4E,
    0x47
  };

  const fs::path test_file = test_dir / "short.png";
  createTestFile(test_file, short_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_EmptyFile) {
  // Empty file
  const std::vector<unsigned char> empty_data = {};

  const fs::path test_file = test_dir / "empty.png";
  createTestFile(test_file, empty_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_NonExistentFile) {
  // File doesn't exist
  const fs::path test_file = test_dir / "nonexistent.png";

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_RealFile) {
  // Test with the actual sunshine.png from the project root

  // Only run this test if the file exists
  if (const fs::path sunshine_png = fs::path(SUNSHINE_SOURCE_DIR) / "sunshine.png"; fs::exists(sunshine_png)) {
    EXPECT_TRUE(proc::check_valid_png(sunshine_png));
  } else {
    GTEST_SKIP() << "sunshine.png not found in project root";
  }
}

TEST_F(ProcessPNGTest, CheckValidPNG_JPEGFile) {
  // JPEG signature (not PNG)
  const std::vector<unsigned char> jpeg_data = {
    0xFF,
    0xD8,
    0xFF,
    0xE0,
    0x00,
    0x10,
    0x4A,
    0x46
  };

  const fs::path test_file = test_dir / "fake.png";
  createTestFile(test_file, jpeg_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_PartialSignature) {
  // Partial PNG signature (first 4 bytes correct, rest wrong)
  const std::vector<unsigned char> partial_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "partial.png";
  createTestFile(test_file, partial_png_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

// Tests for validate_app_image_path function
TEST_F(ProcessPNGTest, ValidateAppImagePath_EmptyPath) {
  // Empty path should return default
  const std::string result = proc::validate_app_image_path("");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_NonPNGExtension) {
  // Non-PNG extension should return default
  const std::string result = proc::validate_app_image_path("image.jpg");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_CaseInsensitiveExtension) {
  // Test that .PNG (uppercase) is recognized
  // Create a valid PNG file
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "test.PNG";
  createTestFile(test_file, valid_png_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  // Should accept uppercase .PNG extension
  EXPECT_NE(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_NonExistentFile) {
  // Non-existent PNG file should return default
  const std::string result = proc::validate_app_image_path("/nonexistent/path/image.png");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_InvalidPNGSignature) {
  // File with .png extension but invalid signature should return default
  const std::vector<unsigned char> invalid_data = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "invalid.png";
  createTestFile(test_file, invalid_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_ValidPNG) {
  // Valid PNG file should return the path
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "valid.png";
  createTestFile(test_file, valid_png_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  EXPECT_EQ(result, test_file.string());
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_OldSteamDefault) {
  // Test the special case for old steam image path
  const std::string result = proc::validate_app_image_path("./assets/steam.png");
  EXPECT_EQ(result, SUNSHINE_ASSETS_DIR "/steam.png");
}

/**
 * @brief Test fixture for proc_t::update_apps_and_env and proc::refresh.
 */
class ProcessRefreshTest: public BaseTest {  // NOSONAR(cpp:S3656): protected members are intentional for test fixture subclassing
protected:
  void SetUp() override {
    BaseTest::SetUp();
    test_dir = fs::temp_directory_path() / "sunshine_process_refresh_test";  // NOSONAR(cpp:S5443): safe for tests
    fs::create_directories(test_dir);
  }

  void TearDown() override {
    if (fs::exists(test_dir)) {
      fs::remove_all(test_dir);
    }
    BaseTest::TearDown();
  }

  /**
   * @brief Write a minimal valid apps.json file.
   *
   * @param path Target path for the JSON file.
   * @param app_names List of application names to include.
   */
  void writeAppsJson(const fs::path &path, const std::vector<std::string> &app_names) const {
    std::ofstream file(path);
    file << "{\n  \"env\": {},\n  \"apps\": [\n";
    for (size_t i = 0; i < app_names.size(); ++i) {
      file << R"(    { "name": ")" << app_names[i] << R"(" })";
      if (i + 1 < app_names.size()) {
        file << ",";
      }
      file << "\n";
    }
    file << "  ]\n}\n";
    file.close();

    // Artificially advance the modification time to guarantee strict monotonicity
    // across tests. This prevents the static last_apps_file_update in refresh()
    // from skipping parses when tests execute rapidly in the same clock tick.
    static int test_time_offset = 1;
    auto new_time = fs::file_time_type::clock::now() + std::chrono::seconds(test_time_offset++);
    fs::last_write_time(path, new_time);
  }

  fs::path test_dir;
};

/** @brief Real successful launcher exits observed at deterministic publication boundaries, without a source backend. */
class ProcessPublicationTest: public ProcessRefreshTest {
protected:
  proc::test_support::scoped_local_commands_t local_commands;

  void SetUp() override {
    ProcessRefreshTest::SetUp();
    saved_proc = std::move(proc::proc);
    proc::proc = proc::proc_t {};
    proc::test_support::set_legacy_restore_hook([this]() {
      ++legacy_restores;
    });
    gate = test_dir / "publication.release";
    ASSERT_FALSE(rtsp_stream::has_source_backend());
  }

  void TearDown() override {
    proc::test_support::set_refresh_published_hook({});
    std::ofstream {gate} << "release";
    proc::proc.terminate();
    proc::test_support::set_legacy_restore_hook({});
    proc::proc = std::move(saved_proc);
    ProcessRefreshTest::TearDown();
  }

  void configure_launcher(bool gated) {
    boost::process::v1::environment env = boost::this_process::environment();
    env["P3C_PUBLICATION_GATE"] = gate.string();
    proc::ctx_t app {};
    app.id = "43";
    app.uuid = "33333333-3333-4333-8333-333333333333";
    app.name = "Short launcher";
    app.auto_detach = true;
    app.wait_all = false;
#ifdef _WIN32
    app.cmd = "cmd.exe /D /C exit 0";
    const auto script = test_dir / "publication.cmd";
    if (gated) {
      std::ofstream {script} << "@echo off\r\n:wait\r\nif exist \"%P3C_PUBLICATION_GATE%\" exit /b 0\r\nping -n 2 127.0.0.1 >nul\r\ngoto wait\r\n";
      app.cmd = "cmd.exe /D /C \"" + script.string() + "\"";
    }
#else
    const auto script = test_dir / "publication.sh";
    if (gated) {
      std::ofstream {script} << "while [ ! -e \"$P3C_PUBLICATION_GATE\" ]; do sleep 0.01; done\nexit 0\n";
    } else {
      std::ofstream {script} << "exit 0\n";
    }
    app.cmd = "/bin/sh \"" + script.string() + "\"";
#endif
    std::vector<proc::ctx_t> apps;
    apps.emplace_back(std::move(app));
    proc::proc = proc::proc_t {std::move(env), std::move(apps)};
  }

  void expect_placeholder_before_another_refresh() {
    const auto published = proc::proc.published_snapshot();
    ASSERT_EQ(published->lifetime.appid, 43);
    EXPECT_EQ(published->lifetime.app_uuid, "33333333-3333-4333-8333-333333333333");
    EXPECT_TRUE(published->lifetime.placeholder);
    EXPECT_FALSE(published->lifetime.tracked);
    EXPECT_FALSE(stream::test_support::control_would_terminate(false));
    EXPECT_FALSE(stream::test_support::control_would_terminate(true));
    EXPECT_FALSE(stream::test_support::last_session_would_revert(false));
    EXPECT_TRUE(stream::test_support::last_session_would_revert(true));
    EXPECT_EQ(proc::proc.published_snapshot(), published);
    EXPECT_EQ(legacy_restores, 0);
  }

  proc::proc_t saved_proc;
  fs::path gate;
  int legacy_restores = 0;
};

TEST_F(ProcessPublicationTest, SuccessfulExitBeforeInitialPublicationPreservesLogicalLauncher) {
  configure_launcher(false);
  int observations = 0;
  bool exited_before_publication = false;
  proc::test_support::set_refresh_published_hook([&]() {
    ++observations;
    exited_before_publication = proc::proc.wait_for_exit_for_testing(std::chrono::seconds {2});
  });
  ASSERT_EQ(proc::proc.execute(43, std::make_shared<rtsp_stream::launch_session_t>()), 0);
  ASSERT_EQ(observations, 1);
  ASSERT_TRUE(exited_before_publication);
  expect_placeholder_before_another_refresh();
  EXPECT_EQ(observations, 1);
}

TEST_F(ProcessPublicationTest, SuccessfulExitAfterLiveObservationBeforePublicationPreservesLogicalLauncher) {
  configure_launcher(true);
  ASSERT_EQ(proc::proc.execute(43, std::make_shared<rtsp_stream::launch_session_t>()), 0);
  ASSERT_EQ(proc::proc.running(), 43);
  ASSERT_TRUE(proc::proc.published_snapshot()->lifetime.tracked);
  int observations = 0;
  bool exited_before_publication = false;
  proc::test_support::set_refresh_published_hook([&]() {
    ++observations;
    std::ofstream {gate} << "release";
    exited_before_publication = proc::proc.wait_for_exit_for_testing(std::chrono::seconds {2});
  });
  proc::proc.refresh_published();
  ASSERT_EQ(observations, 1);
  ASSERT_TRUE(exited_before_publication);
  expect_placeholder_before_another_refresh();
  EXPECT_EQ(observations, 1);
}

/**
 * @brief Test fixture for testing updates to process apps and environment.
 */
class ProcessUpdateTest: public ProcessRefreshTest {  // NOSONAR(cpp:S3656): protected members are intentional for test fixture subclassing
protected:
  proc::proc_t saved_global_proc;

  void SetUp() override {
    ProcessRefreshTest::SetUp();

    saved_global_proc = std::move(proc::proc);

    boost::process::v1::environment env = boost::this_process::environment();
    std::vector<proc::ctx_t> apps_initial;
    proc::ctx_t ctx;
    ctx.name = "Desktop";
    ctx.id = "42";
    apps_initial.push_back(std::move(ctx));

    proc::proc_t target(std::move(env), std::move(apps_initial));
    proc::proc = std::move(target);

    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();
    launch_session->width = 1920;
    launch_session->height = 1080;
    launch_session->fps = 60;
    launch_session->gcmap = 0;
    launch_session->enable_hdr = false;
    launch_session->host_audio = false;
    launch_session->enable_sops = false;
    launch_session->surround_info = 2;
    int rc = proc::proc.execute(42, launch_session);
    ASSERT_EQ(rc, 0);
  }

  void TearDown() override {
    proc::proc.terminate();
    proc::proc = std::move(saved_global_proc);
    ProcessRefreshTest::TearDown();
  }

  /**
   * @brief Build a simulated source environment and app list for the update.
   *
   * @return A constructed proc_t containing the new applications and environment.
   */
  proc::proc_t buildNewSource() const {
    boost::process::v1::environment env2 = boost::this_process::environment();
    std::vector<proc::ctx_t> apps_new;
    proc::ctx_t ctx_new;
    ctx_new.name = "NewApp";
    ctx_new.id = "99";
    apps_new.push_back(std::move(ctx_new));
    return proc::proc_t(std::move(env2), std::move(apps_new));
  }
};

TEST_F(ProcessRefreshTest, UpdateAppsAndEnv_UpdatesAppsList) {
  // Build an initial proc_t with one app
  boost::process::v1::environment env = boost::this_process::environment();
  std::vector<proc::ctx_t> apps_initial;
  proc::ctx_t ctx_a;
  ctx_a.name = "AppA";
  ctx_a.id = "100";
  apps_initial.push_back(std::move(ctx_a));

  proc::proc_t target(std::move(env), std::move(apps_initial));
  ASSERT_EQ(target.get_apps().size(), 1u);
  EXPECT_EQ(target.get_apps()[0].name, "AppA");

  // Build a replacement proc_t with two apps
  boost::process::v1::environment env2 = boost::this_process::environment();
  std::vector<proc::ctx_t> apps_new;
  proc::ctx_t ctx_b;
  ctx_b.name = "AppB";
  ctx_b.id = "200";
  apps_new.push_back(std::move(ctx_b));
  proc::ctx_t ctx_c;
  ctx_c.name = "AppC";
  ctx_c.id = "300";
  apps_new.push_back(std::move(ctx_c));

  proc::proc_t source(std::move(env2), std::move(apps_new));

  // Act
  target.update_apps_and_env(std::move(source));

  // Assert — apps list must now contain the new apps
  ASSERT_EQ(target.get_apps().size(), 2u);
  EXPECT_EQ(target.get_apps()[0].name, "AppB");
  EXPECT_EQ(target.get_apps()[1].name, "AppC");
}

TEST_F(ProcessUpdateTest, UpdateAppsAndEnv_PreservesRunningState) {
  proc::proc.update_apps_and_env(buildNewSource());

  ASSERT_EQ(proc::proc.get_apps().size(), 1u);
  EXPECT_EQ(proc::proc.get_apps()[0].name, "NewApp");
  EXPECT_EQ(proc::proc.running(), 42) << "update_apps_and_env must not reset _app_id or placebo";
}

TEST_F(ProcessUpdateTest, LifetimeSnapshotExcludesPlaceboAndDistinguishesApplicationInstances) {
  const auto first = proc::proc.lifetime_snapshot();
  EXPECT_EQ(first.appid, 42);
  EXPECT_NE(first.instance, 0U);
  EXPECT_TRUE(first.placeholder);
  EXPECT_FALSE(first.tracked);
  auto launch = std::make_shared<rtsp_stream::launch_session_t>();
  ASSERT_EQ(proc::proc.execute(42, launch), 0);
  const auto second = proc::proc.lifetime_snapshot();
  EXPECT_GT(second.instance, first.instance);
  EXPECT_TRUE(second.placeholder);
  EXPECT_FALSE(second.tracked);
}

TEST_F(ProcessUpdateTest, PublishedObservationAndBusyUpdatesRemainIndependentOfProcessState) {
  using observation_t = std::shared_ptr<const proc::published_snapshot_t>;
  const auto original = proc::proc.published_snapshot();
  auto prepared = std::make_shared<proc::published_snapshot_t>(*original);
  prepared->app_name = "Published replacement";
  const observation_t replacement = std::move(prepared);
  std::future<std::array<observation_t, 3>> observer;
  bool ready = false;
  {
    const auto process_state = proc::proc.lock_state();
    observer = std::async(std::launch::async, [replacement]() {
      const auto initial = proc::proc.published_snapshot();
      proc::proc.publish_operation(true);
      const auto busy = proc::proc.published_snapshot();
      proc::proc.publish_snapshot(replacement);
      proc::proc.publish_operation(false);
      return std::array<observation_t, 3> {initial, busy, proc::proc.published_snapshot()};
    });
    ready = observer.wait_for(std::chrono::seconds {2}) == std::future_status::ready;
  }
  ASSERT_TRUE(ready) << "Pointer publication waited for the process-state guard";
  const auto observations = observer.get();
  EXPECT_EQ(observations[0], original);
  EXPECT_TRUE(observations[1]->operation_in_progress);
  EXPECT_EQ(observations[1]->app_name, original->app_name);
  EXPECT_FALSE(observations[2]->operation_in_progress);
  EXPECT_EQ(observations[2]->app_name, replacement->app_name);
  EXPECT_EQ(observations[2]->lifetime.instance, original->lifetime.instance);
  EXPECT_EQ(observations[2]->apps, original->apps);
  EXPECT_FALSE(original->operation_in_progress);
}

TEST_F(ProcessUpdateTest, AttemptOutputAndExactRollbackDoNotOwnAnEarlierIncarnation) {
  const auto original = proc::proc.lifetime_snapshot().instance;
  auto rejected = std::make_shared<rtsp_stream::launch_session_t>();
  rejected->source_generation = 100;
  rejected->source_cancel = std::make_shared<std::atomic_bool>(true);
  std::uint64_t created = 123;
  EXPECT_EQ(proc::proc.execute(99, rejected, &created), 403);
  EXPECT_EQ(created, 0U);
  EXPECT_EQ(proc::proc.lifetime_snapshot().instance, original);
  EXPECT_FALSE(proc::proc.terminate_instance(original + 1));
  EXPECT_EQ(proc::proc.running(), 42);
}

TEST_F(ProcessUpdateTest, OldManagementClaimCannotUnmarkReplacementOrLaterClaimant) {
  const auto original = proc::proc.lifetime_snapshot().instance;
  const auto old = proc::proc.claim_source(original, 100);
  ASSERT_TRUE(old);
  const auto newer = proc::proc.claim_source(original, 101);
  ASSERT_TRUE(newer);
  EXPECT_FALSE(proc::proc.restore_source_claim(*old));
  EXPECT_TRUE(proc::proc.restore_source_claim(*newer));
  EXPECT_TRUE(proc::proc.restore_source_claim(*old));
  auto launch = std::make_shared<rtsp_stream::launch_session_t>();
  ASSERT_EQ(proc::proc.execute(42, launch), 0);
  EXPECT_FALSE(proc::proc.restore_source_claim(*old));
}

TEST_F(ProcessUpdateTest, PublishesExactClaimsAcrossRollbackCatalogAndOperationSnapshots) {
  const auto instance = proc::proc.lifetime_snapshot().instance;
  const auto claim = proc::proc.claim_source(instance, 901);
  ASSERT_TRUE(claim);
  EXPECT_EQ(proc::proc.published_snapshot()->managed_source_instance, instance);
  EXPECT_EQ(proc::proc.published_snapshot()->managed_source_generation, 901u);
  proc::proc.publish_operation(true);
  proc::proc.update_apps_and_env(buildNewSource());
  proc::proc.refresh_published();
  EXPECT_TRUE(proc::proc.published_snapshot()->operation_in_progress);
  EXPECT_EQ(proc::proc.published_snapshot()->managed_source_generation, 901u);
  proc::proc.publish_operation(false);
  const auto newer = proc::proc.claim_source(instance, 902);
  ASSERT_TRUE(newer);
  EXPECT_FALSE(proc::proc.restore_source_claim(*claim));
  EXPECT_EQ(proc::proc.published_snapshot()->managed_source_generation, 902u);
  ASSERT_TRUE(proc::proc.restore_source_claim(*newer));
  EXPECT_EQ(proc::proc.published_snapshot()->managed_source_generation, 901u);
  ASSERT_TRUE(proc::proc.restore_source_claim(*claim));
  EXPECT_EQ(proc::proc.published_snapshot()->managed_source_instance, 0u);
  EXPECT_EQ(proc::proc.published_snapshot()->managed_source_generation, 0u);
  ASSERT_TRUE(proc::proc.claim_source(instance, 903));
  proc::proc.terminate();
  EXPECT_EQ(proc::proc.published_snapshot()->managed_source_instance, 0u);
  EXPECT_EQ(proc::proc.published_snapshot()->managed_source_generation, 0u);
}

TEST_F(ProcessUpdateTest, UpdateAppsAndEnv_PreservesSessionEnvironment) {
  proc::proc.update_apps_and_env(buildNewSource());

  const auto &active_env = proc::proc.get_env();
  EXPECT_TRUE(active_env.count("SUNSHINE_APP_NAME") > 0)
    << "SUNSHINE_APP_NAME was wiped from the environment during update_apps_and_env";

  if (active_env.count("SUNSHINE_APP_NAME") > 0) {
    EXPECT_EQ(active_env.at("SUNSHINE_APP_NAME").to_string(), "Desktop");
  }
}

TEST_F(ProcessRefreshTest, Refresh_ParsesFileOnFirstCall) {
  // The first call to refresh() must always parse the file, regardless
  // of what file_time_type{} compares to relative to the file timestamp.
  //
  // NOTE: Because refresh() uses static local variables (has_parsed, last_apps_file_update),
  // this test exercises the very first call in this test process.  Subsequent
  // tests that call refresh() share the same static state.
  const fs::path apps_file = test_dir / "apps_initial.json";
  writeAppsJson(apps_file, {"TestApp1", "TestApp2"});

  // Save and clear global proc
  auto saved = std::move(proc::proc);
  proc::proc = proc::proc_t {};

  ASSERT_TRUE(proc::proc.get_apps().empty());

  // Act
  proc::refresh(apps_file.string());

  // Assert — must have been parsed
  ASSERT_EQ(proc::proc.get_apps().size(), 2u);
  EXPECT_EQ(proc::proc.get_apps()[0].name, "TestApp1");
  EXPECT_EQ(proc::proc.get_apps()[1].name, "TestApp2");

  // Cleanup
  proc::proc = std::move(saved);
}

TEST_F(ProcessRefreshTest, Refresh_SkipsUnchangedFile) {
  const fs::path apps_file = test_dir / "apps_skip.json";
  writeAppsJson(apps_file, {"OriginalApp"});

  auto saved = std::move(proc::proc);
  proc::proc = proc::proc_t {};

  // First refresh — should parse
  proc::refresh(apps_file.string());
  ASSERT_EQ(proc::proc.get_apps().size(), 1u);

  // Mutate apps in-memory to detect whether the second refresh re-parses
  proc::proc.get_apps()[0].name = "Mutated";

  // Second refresh with unchanged file — should be skipped
  proc::refresh(apps_file.string());
  EXPECT_EQ(proc::proc.get_apps()[0].name, "Mutated")
    << "refresh() should skip re-parse when the file timestamp has not changed";

  // Cleanup
  proc::proc = std::move(saved);
}

TEST_F(ProcessRefreshTest, Refresh_ReparseAfterFileModified) {
  const fs::path apps_file = test_dir / "apps_modified.json";
  writeAppsJson(apps_file, {"BeforeEdit"});

  auto saved = std::move(proc::proc);
  proc::proc = proc::proc_t {};

  proc::refresh(apps_file.string());
  ASSERT_EQ(proc::proc.get_apps().size(), 1u);
  EXPECT_EQ(proc::proc.get_apps()[0].name, "BeforeEdit");

  // Rewrite to ensure a new timestamp
  writeAppsJson(apps_file, {"AfterEdit"});

  proc::refresh(apps_file.string());
  ASSERT_EQ(proc::proc.get_apps().size(), 1u);
  EXPECT_EQ(proc::proc.get_apps()[0].name, "AfterEdit");

  // Cleanup
  proc::proc = std::move(saved);
}

TEST_F(ProcessRefreshTest, Refresh_ReparseAfterFileMtimeDecreased) {
  const fs::path apps_file = test_dir / "apps_reverted.json";
  writeAppsJson(apps_file, {"Initial"});

  auto saved = std::move(proc::proc);
  proc::proc = proc::proc_t {};

  proc::refresh(apps_file.string());
  ASSERT_EQ(proc::proc.get_apps().size(), 1u);
  EXPECT_EQ(proc::proc.get_apps()[0].name, "Initial");

  // Write a new file but artificially set its timestamp to the PAST
  // simulating a backup restoration or git checkout
  writeAppsJson(apps_file, {"OlderBackup"});
  auto past_time = fs::file_time_type::clock::now() - std::chrono::hours(1);
  fs::last_write_time(apps_file, past_time);

  proc::refresh(apps_file.string());
  ASSERT_EQ(proc::proc.get_apps().size(), 1u);
  EXPECT_EQ(proc::proc.get_apps()[0].name, "OlderBackup")
    << "refresh() must re-parse if the modification time differs, even if it is strictly earlier";

  proc::proc = std::move(saved);
}

TEST_F(ProcessRefreshTest, Refresh_PreservesRunningAppDuringReparse) {
  // This is the critical regression test: refreshing the app list while an
  // app is running must not lose the active process state.
  const fs::path apps_file = test_dir / "apps_running.json";
  writeAppsJson(apps_file, {"Desktop"});

  auto saved = std::move(proc::proc);
  proc::proc = proc::proc_t {};

  // Initial parse
  proc::refresh(apps_file.string());
  ASSERT_FALSE(proc::proc.get_apps().empty());

  // Find the ID of "Desktop" as assigned by parse
  auto desktop_id = std::stoi(proc::proc.get_apps()[0].id);

  // Execute it in placebo mode (empty cmd)
  auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();
  launch_session->width = 1920;
  launch_session->height = 1080;
  launch_session->fps = 60;
  launch_session->gcmap = 0;
  launch_session->enable_hdr = false;
  launch_session->host_audio = false;
  launch_session->enable_sops = false;
  launch_session->surround_info = 2;
  int rc = proc::proc.execute(desktop_id, launch_session);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(proc::proc.running(), desktop_id);

  // Simulate an external edit to the apps file
  writeAppsJson(apps_file, {"Desktop", "NewApp"});

  // Act — refresh while the app is "running"
  proc::refresh(apps_file.string());

  // Assert — app list is updated but the running state is preserved
  ASSERT_EQ(proc::proc.get_apps().size(), 2u);
  EXPECT_EQ(proc::proc.running(), desktop_id)
    << "refresh() during a running session must preserve active process state";

  // Cleanup — terminate before restoring
  proc::proc.terminate();
  proc::proc = std::move(saved);
}

TEST_F(ProcessRefreshTest, FileTimeType_DefaultValueComparison) {
  // This test documents the behavior that triggered the timestamp guard
  // regression. On some toolchains (MSYS2 UCRT64 GCC 16.2), file timestamps
  // and clock::now() can compare less-or-equal to file_time_type{}.
  //
  // The fix uses std::optional<file_time_type> instead of relying on
  // file_time_type{} as a "never parsed" indicator.
  const auto default_time = std::filesystem::file_time_type {};
  const auto now_time = std::filesystem::file_time_type::clock::now();

  // We cannot assert the comparison direction since it is platform-dependent.
  // Instead, just log and document both cases.
  if (now_time <= default_time) {
    BOOST_LOG(info) << "file_time_type::clock::now() <= file_time_type{} is TRUE on this platform "
                    << "(this is the UCRT64 case that requires the optional sentinel)";
  } else {
    BOOST_LOG(info) << "file_time_type::clock::now() > file_time_type{} on this platform "
                    << "(default-init guard would have worked, but optional is still safer)";
  }

  // The actual correctness is verified by Refresh_ParsesFileOnFirstCall above —
  // if that test passes, the optional fix works regardless of platform behavior.
  SUCCEED();
}

namespace {
  const std::string APP_UUID_A = "11111111-1111-1111-1111-111111111111";
  const std::string APP_UUID_B = "22222222-2222-2222-2222-222222222222";
  const std::string APP_UUID_C = "33333333-3333-3333-3333-333333333333";

  /**
   * @brief Deterministic pure allocation inputs avoid image I/O and random collision searches.
   */
  proc::catalog::allocation catalog_inputs(std::vector<int> legacy = {}) {
    auto next = std::make_shared<unsigned>(0);
    return {
      [next]() {
        return std::vector {APP_UUID_A, APP_UUID_B, APP_UUID_C}.at((*next)++);
      },
      [](const std::string &) {
        return 0U;
      },
      std::move(legacy),
      INT32_MAX,
      std::nullopt
    };
  }
}  // namespace

TEST(AppCatalogTest, TypedUnknownValuesSurviveNormalizationAndRestart) {
  auto tree = proc::catalog::parse(R"({"env":{},"apps":[{"name":"A","foreign":{"array":[null,true,5,2.5,{},[]]}}],"version":17,"foreign":false})");
  const auto foreign = tree["apps"][0]["foreign"];
  proc::catalog::normalize(tree, catalog_inputs({42}));
  const auto once = tree;
  proc::catalog::normalize(tree, catalog_inputs({99}));
  EXPECT_EQ(tree, once);
  EXPECT_EQ(tree["apps"][0]["foreign"], foreign);
  EXPECT_EQ(tree["apps"][0]["id"], "42");
  EXPECT_EQ(tree["version"], 17);
  EXPECT_EQ(tree["foreign"], false);
}

TEST(AppCatalogTest, EveryUnambiguousLegacyTargetIsReservedBeforeFallback) {
  auto tree = proc::catalog::parse(R"({"env":{},"apps":[{"name":"new"},{"name":"legacy"}]})");
  proc::catalog::normalize(tree, catalog_inputs({0, 1}));
  EXPECT_EQ(tree["apps"][0]["id"], "2");
  EXPECT_EQ(tree["apps"][1]["id"], "1");
}

TEST(AppCatalogTest, CollisionProbingExhaustionAndRetiredTargetsAreDeterministic) {
  auto tree = proc::catalog::parse(R"({"env":{},"apps":[{"name":"A"},{"name":"B"}]})");
  auto inputs = catalog_inputs();
  inputs.maximum = 2;
  proc::catalog::normalize(tree, inputs);
  EXPECT_EQ(tree["apps"][0]["id"], "1");
  EXPECT_EQ(tree["apps"][1]["id"], "2");
  tree["apps"].erase(0);
  tree["apps"].push_back({{"name", "C"}, {"uuid", APP_UUID_C}});
  EXPECT_THROW(proc::catalog::normalize(tree, inputs), proc::catalog::error);
  EXPECT_EQ(tree["miranda-app-identity"]["allocated-ids"].size(), 2);
}

TEST(AppCatalogTest, LedgerMustBeBijectiveIncludingCapturedActiveIdentity) {
  auto tree = proc::catalog::parse(R"({"env":{},"apps":[],"miranda-app-identity":{"version":1,"allocated-ids":{"1":"11111111-1111-1111-1111-111111111111","2":"11111111-1111-1111-1111-111111111111"}}})");
  EXPECT_THROW(proc::catalog::normalize(tree, catalog_inputs()), proc::catalog::error);
  tree["miranda-app-identity"]["allocated-ids"].erase("2");
  auto inputs = catalog_inputs();
  inputs.active = std::pair {2, APP_UUID_A};
  EXPECT_THROW(proc::catalog::normalize(tree, inputs), proc::catalog::error);
}

TEST(AppCatalogTest, ExplicitReservationsCannotBeStolenByGeneratedIdentities) {
  auto tree = proc::catalog::parse(R"({"env":{},"apps":[{"name":"new"},{"name":"explicit","uuid":"11111111-1111-1111-1111-111111111111","id":1}]})");
  proc::catalog::normalize(tree, catalog_inputs());
  EXPECT_EQ(tree["apps"][0]["uuid"], APP_UUID_B);
  EXPECT_EQ(tree["apps"][0]["id"], "2");
  EXPECT_EQ(tree["apps"][1]["id"], "1");
}

TEST(AppCatalogTest, RejectsDuplicateKeysOverflowAndUnsupportedSchemaWithoutLoss) {
  for (const std::string bytes : {
         R"({"env":{},"apps":[],"env":{}})",
         R"({"env":{},"apps":[{"name":"A","foreign":{"x":0,"x":1}}]})",
         R"({"env":{},"apps":[],"unknown":18446744073709551616})",
         R"({"env":{},"apps":[],"unknown":-9223372036854775809})"
       }) {
    EXPECT_THROW(proc::catalog::parse(bytes), proc::catalog::error);
  }
  auto tree = proc::catalog::parse(R"({"env":{},"apps":[],"miranda-app-identity":{"version":2,"allocated-ids":{}}})");
  const auto before = tree;
  EXPECT_THROW(proc::catalog::normalize(tree, catalog_inputs()), proc::catalog::error);
  EXPECT_EQ(tree, before);
}

TEST(AppCatalogTest, RejectsWrongTypesDuplicateAndReservedIdentities) {
  for (const std::string identity : {"0", "-1", "2147483648", "true", "1.0", "null", R"("01")", R"("+1")"}) {
    EXPECT_THROW(proc::catalog::positive_id(proc::catalog::parse(identity)), proc::catalog::error) << identity;
  }
  for (const auto *uuid : {"00000000-0000-0000-0000-000000000000", "8CB5C136-DA67-4F99-B4A1-F9CD35005CF4", "malformed"}) {
    EXPECT_FALSE(proc::catalog::canonical_uuid(uuid));
  }
  auto tree = proc::catalog::parse(R"({"env":{},"apps":[{"name":"A","id":7,"uuid":"11111111-1111-1111-1111-111111111111"},{"name":"B","id":7,"uuid":"22222222-2222-2222-2222-222222222222"}]})");
  EXPECT_THROW(proc::catalog::normalize(tree, catalog_inputs()), proc::catalog::error);
  tree["apps"][1]["id"] = "8";
  tree["apps"][1]["uuid"] = APP_UUID_A;
  EXPECT_THROW(proc::catalog::normalize(tree, catalog_inputs()), proc::catalog::error);
}

TEST(AppCatalogTest, LegacyCrcBoundaryHasDefinedPositiveResults) {
  EXPECT_EQ(proc::catalog::legacy_crc_id(0), 0);
  EXPECT_EQ(proc::catalog::legacy_crc_id(0x80000000U), 0);
  EXPECT_EQ(proc::catalog::legacy_crc_id(0xffffffffU), 1);
  EXPECT_EQ(proc::catalog::legacy_crc_id(0x80000001U), INT32_MAX);
}

TEST(AppCatalogTest, EditorRetainsNestedMetadataAndRefusesAmbiguousLoss) {
  auto retained = proc::catalog::parse(R"({"name":"A","prep-cmd":[{"do":"one","undo":"undo","foreign":{"flags":[false,null]}}],"foreign":{}})");
  auto edit = proc::catalog::parse(R"({"name":"renamed","prep-cmd":[{"do":"one","undo":"undo","elevated":true}]})");
  proc::catalog::merge_edit(retained, edit);
  EXPECT_EQ(retained["prep-cmd"][0]["foreign"]["flags"], proc::catalog::json::array({false, nullptr}));
  EXPECT_TRUE(retained["foreign"].is_object());
  auto reordered = proc::catalog::parse(R"({"prep-cmd":[{"do":"added","undo":""},{"do":"one","undo":"undo"}]})");
  proc::catalog::merge_edit(retained, reordered);
  EXPECT_EQ(retained["prep-cmd"][1]["foreign"]["flags"], proc::catalog::json::array({false, nullptr}));
  // Return to one retained foreign command before exercising an ambiguous replacement.
  retained["prep-cmd"].erase(0);
  edit["prep-cmd"][0]["do"] = "changed";
  EXPECT_THROW(proc::catalog::merge_edit(retained, edit), proc::catalog::error);
  edit["prep-cmd"] = proc::catalog::json::array();
  proc::catalog::merge_edit(retained, edit);
  EXPECT_TRUE(retained["prep-cmd"].empty());
}

/**
 * @brief Exercise complete serialized disk/runtime transactions using synthetic empty-command apps.
 */
class AppCatalogTransactionTest: public ProcessRefreshTest {
protected:
  std::optional<proc::test_support::scoped_local_commands_t> local_commands;
  proc::proc_t saved;
  fs::path file;

  void SetUp() override {
    ProcessRefreshTest::SetUp();
    saved = std::move(proc::proc);
    proc::proc = proc::proc_t {};
    file = test_dir / "identity_catalog.json";
    writeAppsJson(file, {"Desktop", "Other"});
  }

  void TearDown() override {
    http::test_support::set_state_security_hook({});
    http::test_support::set_state_replace_hook({});
    http::test_support::set_state_attributes_hook({});
    http::test_support::set_state_update_hook({});
    proc::test_support::set_undo_environment_hook({});
    rtsp_stream::test_support::set_owner_hook({});
    proc::proc.terminate();
    proc::proc = std::move(saved);
    ProcessRefreshTest::TearDown();
    local_commands.reset();
  }

  std::string bytes() const {
    std::ifstream in {file, std::ios::binary};
    return {std::istreambuf_iterator<char> {in}, std::istreambuf_iterator<char> {}};
  }

  proc::catalog::json read() const {
    const auto result = proc::read_catalog(file.string());
    EXPECT_EQ(result.status, 200) << result.body;
    return proc::catalog::parse(result.body);
  }
};

TEST_F(AppCatalogTransactionTest, RenameArtworkAndReorderRetainBothIdentitiesThroughRestart) {
  const auto initial = read();
  auto edit = initial["apps"][0];
  const auto uuid = edit["uuid"];
  const auto id = edit["id"];
  edit["name"] = "Zulu renamed";
  edit["image-path"] = "synthetic.png";
  const auto result = proc::save_catalog_app(file.string(), initial["revision"], edit.dump());
  ASSERT_EQ(result.status, 200) << result.body;
  proc::proc = proc::proc_t {};
  const auto restarted = read();
  EXPECT_EQ(restarted["apps"][1]["uuid"], uuid);
  EXPECT_EQ(restarted["apps"][1]["id"], id);
}

TEST_F(AppCatalogTransactionTest, VirtualDisplayPreferenceRetainsIdentityAndRejectsInvalidEditsAtomically) {
  const auto initial = read();
  auto edit = initial["apps"][0];
  const auto uuid = edit["uuid"];
  edit["force-virtual-display"] = true;
  ASSERT_EQ(proc::save_catalog_app(file.string(), initial["revision"], edit.dump()).status, 200);
  EXPECT_TRUE(proc::proc.apps_snapshot()[0].force_virtual_display);
  proc::proc = proc::proc_t {};
  auto current = read();
  EXPECT_EQ(current["apps"][0]["uuid"], uuid);
  EXPECT_TRUE(current["apps"][0]["force-virtual-display"].get<bool>());
  EXPECT_TRUE(proc::proc.apps_snapshot()[0].force_virtual_display);
  const auto original = bytes();
  edit["force-virtual-display"] = "yes";
  EXPECT_EQ(proc::save_catalog_app(file.string(), current["revision"], edit.dump()).status, 400);
  EXPECT_EQ(bytes(), original);
  EXPECT_TRUE(proc::proc.apps_snapshot()[0].force_virtual_display);
  edit["force-virtual-display"] = false;
  ASSERT_EQ(proc::save_catalog_app(file.string(), current["revision"], edit.dump()).status, 200);
  EXPECT_FALSE(proc::proc.apps_snapshot()[0].force_virtual_display);
}

TEST_F(AppCatalogTransactionTest, StaleSaveAndDeleteAreRejectedAgainstExactBytes) {
  const auto first_tab = read();
  const auto second_tab = first_tab;
  auto edit = first_tab["apps"][0];
  edit["name"] = "Changed";
  ASSERT_EQ(proc::save_catalog_app(file.string(), first_tab["revision"], edit.dump()).status, 200);
  const auto committed = bytes();
  EXPECT_EQ(proc::save_catalog_app(file.string(), second_tab["revision"], edit.dump()).status, 409);
  EXPECT_EQ(proc::delete_catalog_app(file.string(), second_tab["revision"], second_tab["apps"][0]["uuid"]).status, 409);
  EXPECT_EQ(proc::save_catalog_app(file.string(), "", edit.dump()).status, 428);
  EXPECT_EQ(proc::delete_catalog_app(file.string(), "", second_tab["apps"][0]["uuid"]).status, 428);
  EXPECT_EQ(bytes(), committed);
}

TEST_F(AppCatalogTransactionTest, SameMtimeAndDifferentPathsDoNotSuppressChangedContent) {
  const auto initial = read();
  const auto time = fs::last_write_time(file);
  auto changed = proc::catalog::parse(bytes());
  changed["apps"][0]["name"] = "Same timestamp";
  std::ofstream {file} << changed.dump();
  fs::last_write_time(file, time);
  proc::refresh(file.string());
  EXPECT_EQ(proc::proc.apps_snapshot()[0].name, "Same timestamp");
  const auto other = test_dir / "other_catalog.json";
  writeAppsJson(other, {"Different path"});
  fs::last_write_time(other, time);
  proc::refresh(other.string());
  EXPECT_EQ(proc::proc.apps_snapshot()[0].name, "Different path");
}

TEST_F(AppCatalogTransactionTest, InvalidExternalInputRetainsLastGoodBytesAndRuntime) {
  read();
  const auto original_apps = proc::proc.apps_snapshot();
  const std::string invalid = R"({"env":{},"apps":[{"name":"bad","id":0}]})";
  std::ofstream {file} << invalid;
  EXPECT_EQ(proc::read_catalog(file.string()).status, 400);
  EXPECT_EQ(bytes(), invalid);
  EXPECT_EQ(proc::proc.apps_snapshot()[0].uuid, original_apps[0].uuid);
}

TEST_F(AppCatalogTransactionTest, ActiveEditDeletionAndEnvironmentPublicationPreserveCapturedContext) {
  const auto initial = read();
  const auto target = initial["apps"][0];
  const auto id = std::stoi(target["id"].get<std::string>());
  auto session = std::make_shared<rtsp_stream::launch_session_t>();
  session->surround_info = 2;
  ASSERT_EQ(proc::proc.execute(id, session), 0);
  const auto captured = proc::proc.active_identity();
  auto edit = target;
  edit["name"] = "After launch";
  const auto saved_result = proc::save_catalog_app(file.string(), initial["revision"], edit.dump());
  ASSERT_EQ(saved_result.status, 200) << saved_result.body;
  EXPECT_EQ(proc::proc.active_identity(), captured);
  EXPECT_EQ(proc::proc.get_env().at("SUNSHINE_APP_NAME").to_string(), "Desktop");
  const auto current = proc::catalog::parse(saved_result.body);
  ASSERT_EQ(proc::delete_catalog_app(file.string(), current["revision"], target["uuid"]).status, 200);
  EXPECT_EQ(proc::proc.active_identity(), captured);
  const std::vector<std::pair<int, std::string>> no_live_apps;
  EXPECT_EQ(proc::catalog::resolve("-1", captured.second, no_live_apps, captured, true), captured);
  proc::proc.terminate();
  EXPECT_THROW(proc::catalog::resolve("-1", captured.second, no_live_apps, proc::proc.active_identity(), false), proc::catalog::error);
}

TEST_F(AppCatalogTransactionTest, PreallocatedCatalogPublicationPreservesTheActualSourceClaim) {
  const auto initial = read();
  auto session = std::make_shared<rtsp_stream::launch_session_t>();
  session->surround_info = 2;
  const auto target = initial["apps"][0];
  ASSERT_EQ(proc::proc.execute(std::stoi(target["id"].get<std::string>()), session), 0);
  const auto instance = proc::proc.lifetime_snapshot().instance;
  const auto claim = proc::proc.claim_source(instance, 701);
  ASSERT_TRUE(claim);
  auto edit = target;
  edit["name"] = "Changed while claimed";
  const auto result = proc::save_catalog_app(file.string(), initial["revision"], edit.dump());
  ASSERT_EQ(result.status, 200) << result.body;
  EXPECT_EQ(proc::proc.published_snapshot()->managed_source_instance, instance);
  EXPECT_EQ(proc::proc.published_snapshot()->managed_source_generation, 701u);
  EXPECT_EQ(proc::proc.published_snapshot()->lifetime.instance, instance);
  ASSERT_TRUE(proc::proc.restore_source_claim(*claim));
  EXPECT_EQ(proc::proc.published_snapshot()->managed_source_generation, 0u);
}

#ifdef _WIN32
TEST_F(AppCatalogTransactionTest, NativeSecurityAndReplacementFailuresRetainBytesAndRuntime) {
  const auto initial = read();
  const auto original = bytes();
  const auto identity = proc::proc.apps_snapshot()[0].uuid;
  auto edit = initial["apps"][0];
  edit["name"] = "Must not publish";
  for (const auto stage : {http::test_support::state_security_stage_e::restore, http::test_support::state_security_stage_e::verify}) {
    http::test_support::set_state_security_hook([&](auto current, const auto &temporary) {
      EXPECT_EQ(fs::file_size(temporary), 0);
      if (current == stage) {
        throw std::runtime_error {"fixture security failure"};
      }
    });
    EXPECT_EQ(proc::save_catalog_app(file.string(), initial["revision"], edit.dump()).status, 500);
    EXPECT_EQ(bytes(), original);
    EXPECT_EQ(proc::proc.apps_snapshot()[0].uuid, identity);
    http::test_support::set_state_security_hook({});
  }
  http::test_support::set_state_replace_hook([](const auto &, const auto &, const auto &) {
    return std::optional<std::uint32_t> {5};
  });
  EXPECT_EQ(proc::save_catalog_app(file.string(), initial["revision"], edit.dump()).status, 500);
  EXPECT_EQ(bytes(), original);
  EXPECT_EQ(proc::proc.apps_snapshot()[0].name, "Desktop");
}

TEST_F(AppCatalogTransactionTest, UnwritableLegacyInputNeverAdvertisesTransientIdentity) {
  const auto original = bytes();
  http::test_support::set_state_attributes_hook([](const auto &) {
    return std::optional<std::uint32_t> {0x4000};
  });
  EXPECT_EQ(proc::read_catalog(file.string()).status, 500);
  EXPECT_EQ(bytes(), original);
  EXPECT_TRUE(proc::proc.apps_snapshot().empty());
}
#endif

TEST(AppSelectorTest, NumericUuidSentinelAndResumeMatrix) {
  const std::vector<std::pair<int, std::string>> apps {{7, APP_UUID_A}, {8, APP_UUID_B}};
  const std::pair<int, std::string> active {7, APP_UUID_A};
  EXPECT_EQ(proc::catalog::resolve("7", std::nullopt, apps, active, false), active);
  EXPECT_EQ(proc::catalog::resolve("7", "", apps, active, false), active);
  EXPECT_EQ(proc::catalog::resolve(std::nullopt, APP_UUID_A, apps, active, false), active);
  EXPECT_EQ(proc::catalog::resolve("-1", APP_UUID_A, apps, active, false), active);
  EXPECT_EQ(proc::catalog::resolve("-1", APP_UUID_A, apps, active, true), active);
  EXPECT_EQ(proc::catalog::resolve(std::nullopt, std::nullopt, apps, active, true), active);
  EXPECT_EQ(proc::catalog::resolve("7", APP_UUID_A, apps, active, true), active);
  for (const auto &id : {"0", "8", "99", "-2", "2147483648", "-01", "", "7x"}) {
    EXPECT_THROW(proc::catalog::resolve(id, APP_UUID_A, apps, active, true), proc::catalog::error) << id;
  }
  for (const auto &uuid : {"", "invalid", "33333333-3333-3333-3333-333333333333"}) {
    EXPECT_THROW(proc::catalog::resolve("-1", uuid, apps, active, false), proc::catalog::error) << uuid;
  }
  EXPECT_THROW(proc::catalog::resolve("-1", APP_UUID_B, apps, active, true), proc::catalog::error);
  EXPECT_THROW(proc::catalog::resolve("7", APP_UUID_B, apps, active, false), proc::catalog::error);
  EXPECT_THROW(proc::catalog::resolve(std::nullopt, std::nullopt, apps, active, false), proc::catalog::error);
}

TEST_F(AppCatalogTransactionTest, ConcurrentTabsCannotBothCommitTheSameRevision) {
  const auto initial = read();
  auto first = initial["apps"][0];
  auto second = first;
  first["name"] = "First writer";
  second["name"] = "Second writer";
  std::promise<void> start;
  auto ready = start.get_future().share();
  proc::catalog_response left, right;
  std::jthread first_tab {[&]() {
    ready.wait();
    left = proc::save_catalog_app(file.string(), initial["revision"], first.dump());
  }};
  std::jthread second_tab {[&]() {
    ready.wait();
    right = proc::save_catalog_app(file.string(), initial["revision"], second.dump());
  }};
  start.set_value();
  first_tab.join();
  second_tab.join();
  EXPECT_TRUE((left.status == 200 && right.status == 409) || (left.status == 409 && right.status == 200));
  const auto committed = read();
  const auto it = std::find_if(committed["apps"].begin(), committed["apps"].end(), [&](const auto &app) {
    return app["uuid"] == initial["apps"][0]["uuid"];
  });
  ASSERT_NE(it, committed["apps"].end());
  EXPECT_EQ((*it)["id"], initial["apps"][0]["id"]);
  EXPECT_TRUE((*it)["name"] == "First writer" || (*it)["name"] == "Second writer");
}

namespace {
  /**
   * @brief Seed a fixture environment override and restore its value/spelling even after an assertion failure.
   */
  class native_variable_scope {
    std::string name;
    std::optional<std::string> previous;

  public:
    native_variable_scope(std::string name, const std::optional<std::string> &value):
        name(std::move(name)) {
      logging::log_flush();
      auto parent = boost::this_process::environment();
#ifdef _WIN32
      const auto found = std::find_if(parent.begin(), parent.end(), [&](const auto &entry) {
        return boost::iequals(entry.get_name(), this->name);
      });
#else
      const auto found = parent.find(this->name);
#endif
      if (found != parent.end()) {
        this->name = found->get_name();
        previous = found->to_string();
      }
      if (value) {
        parent[this->name] = *value;
      } else {
        parent[this->name].clear();
      }
    }

    ~native_variable_scope() {
      logging::log_flush();
      auto parent = boost::this_process::environment();
      if (previous) {
        parent[name] = *previous;
      } else {
        parent[name].clear();
      }
    }

    static std::optional<std::string> read(const std::string &name) {
      const auto parent = boost::this_process::environment();
#ifdef _WIN32
      const auto found = std::find_if(parent.begin(), parent.end(), [&](const auto &entry) {
        return boost::iequals(entry.get_name(), name);
      });
#else
      const auto found = parent.find(name);
#endif
      return found == parent.end() ? std::nullopt : std::optional {found->to_string()};
    }
  };
}  // namespace

TEST_F(AppCatalogTransactionTest, OwnedPreparationNeverMutatesParentEnvironmentOnRejectedOrSuccessfulInput) {
  native_variable_scope marker {"P1C_REVIEW_MARKER", "parent"};
  native_variable_scope path {"P1C_REVIEW_PATH", "base"};
  read();
  const auto runtime_before = proc::proc.get_env();
  const auto apps_before = proc::proc.apps_snapshot();
  const std::string malformed = R"({"env":{"P1C_REVIEW_MARKER":"candidate","P1C_REVIEW_PATH":"$(P1C_REVIEW_PATH):once"},"apps":[{"name":"A","uuid":"invalid"}]})";
  std::ofstream {file} << malformed;
  EXPECT_EQ(proc::read_catalog(file.string()).status, 400);
  EXPECT_EQ(bytes(), malformed);
  EXPECT_EQ(native_variable_scope::read("P1C_REVIEW_MARKER"), "parent");
  EXPECT_EQ(native_variable_scope::read("P1C_REVIEW_PATH"), "base");
  EXPECT_EQ(proc::proc.get_env().at("P1C_REVIEW_MARKER").to_string(), runtime_before.at("P1C_REVIEW_MARKER").to_string());
  EXPECT_EQ(proc::proc.apps_snapshot()[0].uuid, apps_before[0].uuid);

  const std::string valid = R"({"env":{"P1C_REVIEW_MARKER":"candidate","P1C_REVIEW_PATH":"$(P1C_REVIEW_PATH):once"},"apps":[{"name":"A"}]})";
  std::ofstream {file} << valid;
  http::test_support::set_state_update_hook([&]() {
    // Observe the prebytes interval as well as postchecks: owned evaluation never exposes a transient native assignment.
    EXPECT_EQ(native_variable_scope::read("P1C_REVIEW_MARKER"), "parent");
    EXPECT_EQ(native_variable_scope::read("P1C_REVIEW_PATH"), "base");
  });
#ifdef _WIN32
  http::test_support::set_state_replace_hook([](const auto &, const auto &, const auto &) {
    return std::optional<std::uint32_t> {5};
  });
  EXPECT_EQ(proc::read_catalog(file.string()).status, 500);
  EXPECT_EQ(bytes(), valid);
  EXPECT_EQ(proc::proc.apps_snapshot()[0].uuid, apps_before[0].uuid);
  EXPECT_EQ(proc::proc.get_env().at("P1C_REVIEW_MARKER").to_string(), "parent");
  EXPECT_EQ(native_variable_scope::read("P1C_REVIEW_MARKER"), "parent");
  EXPECT_EQ(native_variable_scope::read("P1C_REVIEW_PATH"), "base");
  http::test_support::set_state_replace_hook({});
#endif
  const auto committed = read();
  EXPECT_EQ(proc::proc.get_env().at("P1C_REVIEW_MARKER").to_string(), "candidate");
  EXPECT_EQ(proc::proc.get_env().at("P1C_REVIEW_PATH").to_string(), "base:once");
  EXPECT_EQ(native_variable_scope::read("P1C_REVIEW_MARKER"), "parent");
  EXPECT_EQ(native_variable_scope::read("P1C_REVIEW_PATH"), "base");
  const auto committed_bytes = bytes();
  const auto unchanged = read();
  EXPECT_EQ(unchanged["revision"], committed["revision"]);
  EXPECT_EQ(bytes(), committed_bytes);
  auto edit = committed["apps"][0];
  edit["output"] = "";
  EXPECT_EQ(proc::save_catalog_app(file.string(), committed["revision"], edit.dump()).status, 200);
  EXPECT_EQ(proc::proc.get_env().at("P1C_REVIEW_PATH").to_string(), "base:once");
  EXPECT_EQ(native_variable_scope::read("P1C_REVIEW_PATH"), "base");
}

TEST_F(AppCatalogTransactionTest, OrderedDependentEnvironmentSurvivesNormalizationSaveAndCleanRestart) {
  native_variable_scope root {"ZZ_P1C_ROOT", std::nullopt};
  native_variable_scope alias {"AA_P1C_ALIAS", std::nullopt};
  const std::string legacy = R"json({"env":{"ZZ_P1C_ROOT":"desired","AA_P1C_ALIAS":"$(ZZ_P1C_ROOT)"
}

, "apps": [{"name": "$(AA_P1C_ALIAS)", "cmd": "$(AA_P1C_ALIAS)", "foreign": {"array": [null, false, {}, []]}}]
})json";
  std::ofstream {file} << legacy;
  const auto before = proc::parse(file.string());
  ASSERT_TRUE(before);
  const auto expected_id = before->apps_snapshot()[0].id;
  ASSERT_EQ(before->apps_snapshot()[0].name, "desired");
  ASSERT_EQ(before->apps_snapshot()[0].cmd, "desired");
  const auto normalized = read();
  EXPECT_EQ(proc::proc.apps_snapshot()[0].id, expected_id);
  EXPECT_EQ(proc::proc.apps_snapshot()[0].name, "desired");
  EXPECT_EQ(proc::proc.apps_snapshot()[0].cmd, "desired");
  EXPECT_EQ(proc::proc.get_env().at("AA_P1C_ALIAS").to_string(), "desired");
  EXPECT_LT(bytes().find("ZZ_P1C_ROOT"), bytes().find("AA_P1C_ALIAS"));
  EXPECT_FALSE(native_variable_scope::read("ZZ_P1C_ROOT"));
  EXPECT_FALSE(native_variable_scope::read("AA_P1C_ALIAS"));
  auto edit = normalized["apps"][0];
  edit["wait-all"] = false;
  ASSERT_EQ(proc::save_catalog_app(file.string(), normalized["revision"], edit.dump()).status, 200);
  proc::proc = proc::proc_t {};
  const auto restarted = read();
  EXPECT_EQ(proc::proc.apps_snapshot()[0].id, expected_id);
  EXPECT_EQ(proc::proc.apps_snapshot()[0].name, "desired");
  EXPECT_EQ(proc::proc.apps_snapshot()[0].cmd, "desired");
  EXPECT_EQ(proc::proc.get_env().at("AA_P1C_ALIAS").to_string(), "desired");
  EXPECT_EQ(restarted["apps"][0]["foreign"], normalized["apps"][0]["foreign"]);
  EXPECT_LT(bytes().find("ZZ_P1C_ROOT"), bytes().find("AA_P1C_ALIAS"));
  EXPECT_FALSE(native_variable_scope::read("ZZ_P1C_ROOT"));
  EXPECT_FALSE(native_variable_scope::read("AA_P1C_ALIAS"));
}

TEST_F(AppCatalogTransactionTest, RealExitedChildCleanupSerializesWithProcessLanePublicationAndRunsUndoOnce) {
  local_commands.emplace();
  // No settings manager is installed: real process cleanup must not configure host displays.
  display_device::test_reset_display_device();
  native_variable_scope parent_marker {"P1C_UNDO_ENVIRONMENT", "parent"};
  auto tree = proc::catalog::parse(R"({"env":{"P1C_UNDO_ENVIRONMENT":"captured"},"apps":[{"name":"Owned child","auto-detach":false,"wait-all":false,"exit-timeout":0,"prep-cmd":[{"do":"","undo":""}]}]})");
  const auto output = test_dir / "owned_child_undo.txt";
#ifdef _WIN32
  tree["apps"][0]["cmd"] = "cmd.exe /D /C exit 0";
  tree["apps"][0]["prep-cmd"][0]["undo"] = "cmd.exe /D /C echo %P1C_UNDO_ENVIRONMENT%";
#else
  std::ofstream {test_dir / "p1c-exit.sh"} << "exit 0\n";
  std::ofstream {test_dir / "p1c-undo.sh"} << "printf '%s\\n' \"$P1C_UNDO_ENVIRONMENT\"\n";
  tree["apps"][0]["cmd"] = "/bin/sh p1c-exit.sh";
  tree["apps"][0]["prep-cmd"][0]["undo"] = "/bin/sh p1c-undo.sh";
  tree["apps"][0]["working-dir"] = test_dir.string();
#endif
  tree["apps"][0]["output"] = output.string();
  std::ofstream {file} << tree.dump();
  const auto initial = read();
  const auto id = std::stoi(initial["apps"][0]["id"].get<std::string>());
  auto session = std::make_shared<rtsp_stream::launch_session_t>();
  session->surround_info = 2;
  ASSERT_EQ(proc::proc.execute(id, session), 0);
  ASSERT_TRUE(proc::proc.wait_for_exit_for_testing(std::chrono::seconds {2}));
  auto owner = rtsp_stream::test_support::start_owner();
  std::promise<void> consuming;
  std::promise<void> release;
  const auto released = release.get_future().share();
  std::promise<void> posted;
  std::atomic_bool observed_post {false};
  std::atomic_int undos {0};
  proc::test_support::set_undo_environment_hook([&](const auto &environment) {
    EXPECT_EQ(environment.at("P1C_UNDO_ENVIRONMENT").to_string(), "captured");
    EXPECT_EQ(environment.at("SUNSHINE_APP_ID").to_string(), std::to_string(id));
    if (++undos == 1) {
      consuming.set_value();
    }
    released.wait();
    EXPECT_EQ(environment.at("P1C_UNDO_ENVIRONMENT").to_string(), "captured");
  });
  rtsp_stream::test_support::set_owner_hook([&](std::string_view event) {
    if (event == "process_operation_queued" && !observed_post.exchange(true)) {
      EXPECT_TRUE(rtsp_stream::on_owner_thread());
      posted.set_value();
    }
  });
  int running = -1;
  proc::catalog_response saved_result;
  std::promise<void> saved;
  auto published = saved.get_future();
  std::jthread cleanup {[&]() {
    running = proc::proc.running();
  }};
  std::jthread writer;
  bool signalled = false;
  auto unblock = util::fail_guard([&]() {
    if (!signalled) {
      release.set_value();
    }
  });
  EXPECT_EQ(consuming.get_future().wait_for(std::chrono::seconds {2}), std::future_status::ready);
  auto edit = initial["apps"][0];
  edit["name"] = "Committed after cleanup";
  writer = std::jthread {[&]() {
    saved_result = proc::save_catalog_app(file.string(), initial["revision"], edit.dump());
    saved.set_value();
  }};
  EXPECT_EQ(posted.get_future().wait_for(std::chrono::seconds {2}), std::future_status::ready);
  EXPECT_EQ(published.wait_for(std::chrono::milliseconds {100}), std::future_status::timeout);
  release.set_value();
  signalled = true;
  cleanup.join();
  writer.join();
  proc::test_support::set_undo_environment_hook({});
  rtsp_stream::test_support::set_owner_hook({});
  EXPECT_EQ(running, 0);
  EXPECT_EQ(undos, 1);
  EXPECT_EQ(saved_result.status, 200) << saved_result.body;
  EXPECT_EQ(proc::proc.apps_snapshot()[0].name, "Committed after cleanup");
  EXPECT_EQ(proc::proc.running(), 0);
  std::ifstream undo_output {output};
  std::string first, extra;
  std::getline(undo_output, first);
  std::getline(undo_output, extra);
  if (!first.empty() && first.back() == '\r') {
    first.pop_back();
  }
  EXPECT_EQ(first, "captured");
  EXPECT_TRUE(extra.empty());
  EXPECT_EQ(native_variable_scope::read("P1C_UNDO_ENVIRONMENT"), "parent");
  auto stopped = std::async(std::launch::async, [&]() {
    owner.reset();
  });
  EXPECT_EQ(stopped.wait_for(std::chrono::seconds {2}), std::future_status::ready);
  stopped.get();
}

TEST(ProcessStateTest, GuardRetainsMutexStorageAcrossQuiescentProcessMoves) {
  std::optional<proc::state_guard_t> retained;
  {
    proc::proc_t original;
    retained.emplace(original.lock_state());
    proc::proc_t moved {std::move(original)};
    const auto nested = moved.lock_state();
    EXPECT_EQ(retained->lifetime, nested.lifetime);
    EXPECT_TRUE(retained->lock.owns_lock());
  }
  EXPECT_TRUE(retained->lock.owns_lock());
  retained.reset();
}

TEST(ProcessEnvironmentTest, OwnedReplacementPreservesWindowsSpellingAndPosixDistinctNames) {
  const auto parent = native_variable_scope::read("PATH");
  boost::process::v1::environment owned;
  owned["Path"] = "base";
  proc::assign_environment(owned, "PATH", proc::parse_env_val(owned, "$(Path);extra"));
  proc::assign_environment(owned, "path", "last");
  std::size_t case_matches = 0;
  for (const auto &entry : owned) {
    if (boost::iequals(entry.get_name(), "PATH")) {
      ++case_matches;
    }
  }
#ifdef _WIN32
  EXPECT_EQ(case_matches, 1);
  EXPECT_EQ(owned.at("Path").to_string(), "last");
  EXPECT_EQ(proc::parse_env_val(owned, "$(PATH)"), "last");
#else
  EXPECT_EQ(case_matches, 3);
  EXPECT_EQ(owned.at("Path").to_string(), "base");
  EXPECT_EQ(owned.at("PATH").to_string(), "base;extra");
  EXPECT_EQ(owned.at("path").to_string(), "last");
#endif
  EXPECT_EQ(native_variable_scope::read("PATH"), parent);
}

TEST_F(AppCatalogTransactionTest, CaseVariantOverridesAndDependentLegacyIdentityUseOneWindowsEntry) {
  native_variable_scope parent_path {"PATH", "parent"};
#ifndef _WIN32
  native_variable_scope other_path {"path", std::nullopt};
  native_variable_scope mixed_path {"Path", std::nullopt};
#endif
  const std::string legacy = R"json({"env":{"PATH":"first","path":"last","P1C_PATH_ALIAS":"$(PATH)"
}

, "apps": [{"name": "$(P1C_PATH_ALIAS)", "cmd": "$(P1C_PATH_ALIAS)"}]
})json";
  std::ofstream {file} << legacy;
#ifdef _WIN32
  const std::string expected = "last";
  const std::size_t expected_entries = 1;
#else
  const std::string expected = "first";
  const std::size_t expected_entries = 2;
#endif
  const auto original_id = std::get<0>(proc::calculate_app_id(expected, "", 0));
  const auto normalized = read();
  EXPECT_EQ(proc::proc.apps_snapshot()[0].name, expected);
  EXPECT_EQ(proc::proc.apps_snapshot()[0].cmd, expected);
  EXPECT_EQ(proc::proc.apps_snapshot()[0].id, original_id);
  auto owned = proc::proc.get_env();
  std::size_t entries = 0;
  for (const auto &entry : owned) {
    if (boost::iequals(entry.get_name(), "PATH")) {
      ++entries;
    }
  }
  EXPECT_EQ(entries, expected_entries);
  EXPECT_EQ(proc::parse_env_val(owned, "$(PATH)"), expected);
  EXPECT_EQ(native_variable_scope::read("PATH"), "parent");
  proc::proc = proc::proc_t {};
  const auto restarted = read();
  EXPECT_EQ(restarted["apps"][0]["uuid"], normalized["apps"][0]["uuid"]);
  EXPECT_EQ(proc::proc.apps_snapshot()[0].id, original_id);
  EXPECT_EQ(proc::proc.apps_snapshot()[0].cmd, expected);
  EXPECT_EQ(native_variable_scope::read("PATH"), "parent");
}

TEST_F(AppCatalogTransactionTest, ConfiguredPathSelectsTheExecutableWithInferredAndExplicitWorkingDirectories) {
  local_commands.emplace();
  display_device::test_reset_display_device();
  const auto configured = fs::absolute(test_dir / "configured bin");
  const auto native = fs::absolute(test_dir / "native-bin");
  const auto unrelated = fs::absolute(test_dir / "unrelated working");
  const auto empty = fs::absolute(test_dir / "empty-bin");
  for (const auto &directory : {configured, native, unrelated, empty}) {
    fs::create_directories(directory);
  }
#ifdef _WIN32
  const std::string name = "p1c_lookup_helper.exe";
  const auto configured_binary = boost::process::v1::search_path("cmd.exe");
  const auto native_binary = boost::process::v1::search_path("where.exe");
  ASSERT_FALSE(configured_binary.empty());
  ASSERT_FALSE(native_binary.empty());
  fs::copy_file(configured_binary.string(), configured / name);
  fs::copy_file(native_binary.string(), native / name);
  const auto command = name + " /D /C echo configured";
  const std::string expected = "configured";
#else
  const std::string name = "p1c_lookup_helper";
  std::ofstream {configured / name} << "#!/bin/sh\nprintf 'configured:%s:%s\\n' \"$1\" \"$2\"\n";
  std::ofstream {native / name} << "#!/bin/sh\nprintf 'native\\n'\n";
  fs::permissions(configured / name, fs::perms::owner_exec, fs::perm_options::add);
  fs::permissions(native / name, fs::perms::owner_exec, fs::perm_options::add);
  const auto command = "\"" + name + "\" \"two words\" suffix";
  const std::string expected = "configured:two words:suffix";
#endif
  native_variable_scope native_path {"PATH", native.string()};
  const auto output = fs::absolute(test_dir / "configured_execution.txt");
  auto catalog = proc::catalog::parse(R"({"env":{},"apps":[{"name":"PATH fixture","auto-detach":false,"wait-all":false,"exit-timeout":0}]})");
  catalog["env"]["PATH"] = configured.string();
  catalog["apps"][0]["cmd"] = command;
  catalog["apps"][0]["output"] = output.string();
  {
    native_variable_scope configured_only {"PATH", empty.string()};
    std::ofstream {file} << catalog.dump();
    const auto loaded = read();
    auto owned = proc::proc.get_env();
    EXPECT_EQ(proc::find_working_directory(command, owned), boost::filesystem::path(configured.string()));
    EXPECT_EQ(native_variable_scope::read("PATH"), empty.string());
    std::ofstream {output};
    auto session = std::make_shared<rtsp_stream::launch_session_t>();
    session->surround_info = 2;
    ASSERT_EQ(proc::proc.execute(std::stoi(loaded["apps"][0]["id"].get<std::string>()), session), 0);
    ASSERT_TRUE(proc::proc.wait_for_exit_for_testing(std::chrono::seconds {2}));
    EXPECT_EQ(proc::proc.running(), 0);
    std::ifstream result {output};
    std::string line;
    std::getline(result, line);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    EXPECT_EQ(line, expected);
    EXPECT_EQ(native_variable_scope::read("PATH"), empty.string());
    catalog = proc::catalog::parse(bytes());
  }
  for (const bool explicit_directory : {false, true}) {
    catalog["apps"][0]["working-dir"] = explicit_directory ? unrelated.string() : "";
    std::ofstream {file} << catalog.dump();
    const auto loaded = read();
    auto owned = proc::proc.get_env();
    EXPECT_EQ(proc::find_working_directory(command, owned), boost::filesystem::path(configured.string()));
    EXPECT_EQ(native_variable_scope::read("PATH"), native.string());
    std::ofstream {output};
    auto session = std::make_shared<rtsp_stream::launch_session_t>();
    session->surround_info = 2;
    ASSERT_EQ(proc::proc.execute(std::stoi(loaded["apps"][0]["id"].get<std::string>()), session), 0);
    ASSERT_TRUE(proc::proc.wait_for_exit_for_testing(std::chrono::seconds {2}));
    EXPECT_EQ(proc::proc.running(), 0);
    std::ifstream result {output};
    std::string line;
    std::getline(result, line);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    EXPECT_EQ(line, expected);
    EXPECT_EQ(native_variable_scope::read("PATH"), native.string());
    catalog = proc::catalog::parse(bytes());
  }
#ifndef _WIN32
  // Preserve existing absolute/relative executable syntax and the original quoted argument suffix.
  for (const auto &syntax : {"\"" + (configured / name).string() + "\" \"two words\" suffix", "./" + name + " \"two words\" suffix"}) {
    catalog["apps"][0]["cmd"] = syntax;
    catalog["apps"][0]["working-dir"] = configured.string();
    std::ofstream {file} << catalog.dump();
    const auto loaded = read();
    std::ofstream {output};
    auto session = std::make_shared<rtsp_stream::launch_session_t>();
    session->surround_info = 2;
    ASSERT_EQ(proc::proc.execute(std::stoi(loaded["apps"][0]["id"].get<std::string>()), session), 0);
    ASSERT_TRUE(proc::proc.wait_for_exit_for_testing(std::chrono::seconds {2}));
    EXPECT_EQ(proc::proc.running(), 0);
    std::ifstream result {output};
    std::string line;
    std::getline(result, line);
    EXPECT_EQ(line, expected);
    EXPECT_EQ(native_variable_scope::read("PATH"), native.string());
    catalog = proc::catalog::parse(bytes());
  }
#endif
  catalog["apps"][0]["cmd"] = command;
  catalog["env"]["PATH"] = empty.string();
  catalog["apps"][0]["working-dir"] = unrelated.string();
  std::ofstream {file} << catalog.dump();
  const auto missing = read();
  auto owned = proc::proc.get_env();
  EXPECT_TRUE(proc::find_working_directory(command, owned).empty());
  std::ofstream {output};
  auto session = std::make_shared<rtsp_stream::launch_session_t>();
  session->surround_info = 2;
  EXPECT_NE(proc::proc.execute(std::stoi(missing["apps"][0]["id"].get<std::string>()), session), 0);
  EXPECT_EQ(proc::proc.running(), 0);
  EXPECT_EQ(fs::file_size(output), 0);
  EXPECT_EQ(native_variable_scope::read("PATH"), native.string());
}
