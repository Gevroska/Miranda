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
   * @brief Serialize a shared JSON read/modify/verified-replacement transaction.
   * @param file State or credentials file; separate configured paths are supported.
   * @param update Mutation preserving all unrelated JSON fields.
   * @return Whether replacement succeeded. Windows reports success after verified commit even if backup cleanup is deferred; uncertain recovery retains secured material and blocks further writes in this process.
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
     * @brief Native transaction stages available to synthetic-file failure regressions.
     */
    enum class state_file_stage_e {
      label,  ///< Read the mandatory label without requesting audit access.
      attributes,  ///< Read resource attributes without requesting audit access.
      scope,  ///< Read the central access policy selector.
      metadata,  ///< Inspect supported file metadata.
      backup,  ///< Create the original's hard-link backup.
      write,  ///< Write candidate bytes after the empty security gate.
      flush,  ///< Flush candidate bytes.
      close,  ///< Check closure of the prepared temporary.
      rename,  ///< Replace the destination with the prepared temporary.
      rename_open,  ///< Open the verified rename source with delete access.
      rename_api,  ///< Rename through the verified source handle.
      rename_close,  ///< Report checked source-handle closure after the rename.
      verify,  ///< Verify the committed candidate.
      restore,  ///< Restore a verified original backup.
      cleanup  ///< Delete a known transaction-owned file.
    };
    /**
     * @brief Fault a real Windows transaction stage using only fixture-owned paths.
     * @param hook Optional error after fixture inspection or mutation; empty restores native behavior.
     */
    void set_state_file_hook(std::function<std::optional<std::uint32_t>(state_file_stage_e, const std::filesystem::path &, const std::filesystem::path &, const std::filesystem::path &)> hook);
    /**
     * @brief Exercise the exact native prebytes security comparison.
     * @param expected Original modern security descriptor.
     * @param current Modern descriptor under inspection.
     * @return Whether complete authority and every other control field is unchanged.
     */
    bool state_file_security_matches(std::vector<std::uint8_t> expected, std::vector<std::uint8_t> current);
    /**
     * @brief Exercise selected SACL authority comparisons and the central-policy refusal gate.
     * @param expected Selected original descriptor.
     * @param current Selected descriptor under inspection.
     * @param scope Whether a nonempty central-policy ACL must be refused.
     * @return Whether the selected policy is representable and identical.
     */
    bool state_selected_security_matches(std::vector<std::uint8_t> expected, std::vector<std::uint8_t> current, bool scope = false);
  #endif
    /**
     * @brief Inspect or fault the replacing rename using only test-owned files.
     * @param hook Returns an error after optional fixture mutation, or no value for the native operation.
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
