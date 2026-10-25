/**
 * @file src/httpcommon.h
 * @brief Declarations for common HTTP.
 */
#pragma once

// standard includes
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <curl/curl.h>

// local includes
#include "network.h"
#include "thread_safe.h"

namespace http {

  /**
   * @brief Serialize a shared JSON read/modify/atomic-replace transaction.
   * @param file State or credentials file; separate configured paths are supported.
   * @param update Mutation preserving all unrelated JSON fields.
   * @return Whether replacement succeeded. An unrecoverable replacement failure retains secured recovery files and blocks further writes.
   */
  bool update_state_file(const std::string &file, const std::function<void(boost::property_tree::ptree &)> &update);

#ifdef SUNSHINE_TESTS
  namespace test_support {
    /**
     * @brief Pause a transaction after its serialized read for deterministic persistence races.
     * @param hook Callback, or empty to restore production behavior.
     */
    void set_state_update_hook(std::function<void()> hook);
    /**
     * @brief Native security gates reached only while the temporary file is empty.
     */
    enum class state_security_stage_e {
      restore,  ///< Before restoring the original policy.
      verify  ///< Before verifying the restored policy.
    };
    /**
     * @brief Inspect or fault an empty fixture temporary at a native security gate.
     * @param hook Callback, or empty to restore production behavior.
     */
    void set_state_security_hook(std::function<void(state_security_stage_e, const std::filesystem::path &)> hook);
    /**
     * @brief Override fixture attribute reads without creating an EFS key or changing encryption settings.
     * @param hook Optional attributes override, or empty for native reads.
     */
    void set_state_attributes_hook(std::function<std::optional<std::uint32_t>(const std::filesystem::path &)> hook);
  #ifdef _WIN32
    /**
     * @brief Exercise the exact native prebytes security comparison.
     * @param expected Original modern security descriptor.
     * @param current Modern descriptor under inspection.
     * @param replacement Allow only the final Windows-managed auto-inheritance marker addition.
     * @return Whether complete authority and every other control field is unchanged.
     */
    bool state_file_security_matches(std::vector<std::uint8_t> expected, std::vector<std::uint8_t> current, bool replacement = false);
  #endif
    /**
     * @brief Simulate a documented native replacement result using only test-owned files.
     * @param hook Returns a Windows error code after optional fixture moves, or no value for the native operation.
     */
    void set_state_replace_hook(std::function<std::optional<std::uint32_t>(const std::filesystem::path &, const std::filesystem::path &, const std::filesystem::path &)> hook);
  }  // namespace test_support
#endif

  /**
   * @brief Initialize shared HTTP client state.
   *
   * @return 0 when HTTP state initializes successfully; nonzero on failure.
   */
  int init();
  /**
   * @brief Generate HTTPS credential files from the provided key and certificate paths.
   *
   * @param pkey Private key PEM data or private key file path.
   * @param cert Certificate data or object used by the operation.
   * @return Created creds object or status.
   */
  int create_creds(const std::string &pkey, const std::string &cert);
  int save_user_creds(
    const std::string &file,
    const std::string &username,
    const std::string &password,
    bool run_our_mouth = false
  );

  /**
   * @brief Reload Web UI user credentials from disk.
   *
   * @param file Destination path for the downloaded content.
   * @return 0 when credentials reload successfully; nonzero on failure.
   */
  int reload_user_creds(const std::string &file);
  /**
   * @brief Download a URL to a local file using libcurl.
   *
   * @param url URL used for the HTTP request.
   * @param file Destination path for the downloaded content.
   * @param ssl_version libcurl TLS version selector for the request.
   * @return True when the file is downloaded successfully.
   */
  bool download_file(const std::string &url, const std::string &file, long ssl_version = CURL_SSLVERSION_TLSv1_2);
  /**
   * @brief Percent-encode a string for safe inclusion in a URL.
   *
   * @param url URL used for the HTTP request.
   * @return Percent-encoded URL component.
   */
  std::string url_escape(const std::string &url);
  /**
   * @brief Extract the host component from a URL.
   *
   * @param url URL used for the HTTP request.
   * @return Host name parsed from the URL, or an empty string when none is present.
   */
  std::string url_get_host(const std::string &url);

  extern std::string unique_id;
  extern net::net_e origin_web_ui_allowed;

}  // namespace http
