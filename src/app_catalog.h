/**
 * @file src/app_catalog.h
 * @brief Typed, side-effect-free application identity validation and normalization.
 */
#pragma once

// standard includes
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace proc::catalog {
  using json = nlohmann::ordered_json;

  /**
   * @brief Reproduce legacy CRC signed magnitude with defined arithmetic, excluding zero and INT32_MIN.
   */
  inline std::uint32_t legacy_crc_id(std::uint32_t crc) {
    const auto magnitude = crc & 0x80000000U ? 0U - crc : crc;
    return magnitude > INT32_MAX ? 0U : magnitude;
  }

  /**
   * @brief Bounded public diagnostic containing field names and reasons, never configuration values.
   */
  struct error: std::runtime_error {
    int status;  ///< HTTP status appropriate to this failure.

    explicit error(std::string reason, int status = 400):
        std::runtime_error(std::move(reason)),
        status(status) {}
  };

  /**
   * @brief Canonicalize an ordinary application UUID, rejecting reserved protocol identities.
   */
  inline std::optional<std::string> canonical_uuid(std::string value) {
    if (value.size() != 36) {
      return std::nullopt;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (i == 8 || i == 13 || i == 18 || i == 23) {
        if (value[i] != '-') {
          return std::nullopt;
        }
      } else if (value[i] >= 'A' && value[i] <= 'F') {
        value[i] += 'a' - 'A';
      } else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) {
        return std::nullopt;
      }
    }
    static const std::set<std::string> reserved {
      "00000000-0000-0000-0000-000000000000",
      "8902cb19-674a-403d-a587-41b092e900ba",
      "eaac6159-089a-46a9-9e24-6436885f6610",
      "8cb5c136-da67-4f99-b4a1-f9cd35005cf4",
      "e16cbe1b-295d-4632-9a76-ec4180c857d3"
    };
    return reserved.contains(value) ? std::nullopt : std::optional {std::move(value)};
  }

  /**
   * @brief Read a canonical positive protocol integer without narrowing or accepting floats.
   */
  inline int positive_id(const json &value) {
    if (value.is_number_integer()) {
      if (value.is_number_unsigned()) {
        const auto id = value.get<std::uint64_t>();
        if (id && id <= INT32_MAX) {
          return static_cast<int>(id);
        }
      } else {
        const auto id = value.get<std::int64_t>();
        if (id > 0 && id <= INT32_MAX) {
          return static_cast<int>(id);
        }
      }
    } else if (value.is_string()) {
      const auto &text = value.get_ref<const std::string &>();
      int id = 0;
      const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), id);
      if (!text.empty() && text.front() != '0' && id > 0 && ec == std::errc {} && end == text.data() + text.size()) {
        return id;
      }
    }
    throw error {"apps.id: expected a positive 32-bit integer"};
  }

  /**
   * @brief Public SAX validation prevents duplicate keys and integer-to-float loss before DOM parsing.
   */
  class syntax_guard: public nlohmann::json_sax<json> {
    std::vector<std::optional<std::set<std::string>>> containers;

  public:
    bool null() override {
      return true;
    }

    bool boolean(bool) override {
      return true;
    }

    bool number_integer(number_integer_t) override {
      return true;
    }

    bool number_unsigned(number_unsigned_t) override {
      return true;
    }

    bool number_float(number_float_t, const string_t &token) override {
      return token.find_first_of(".eE") != std::string::npos;
    }

    bool string(string_t &) override {
      return true;
    }

    bool binary(binary_t &) override {
      return false;
    }

    bool start_object(std::size_t) override {
      containers.emplace_back(std::in_place);
      return containers.size() <= 128;
    }

    bool key(string_t &key) override {
      return containers.back()->insert(key).second;
    }

    bool end_object() override {
      containers.pop_back();
      return true;
    }

    bool start_array(std::size_t) override {
      containers.emplace_back(std::nullopt);
      return containers.size() <= 128;
    }

    bool end_array() override {
      containers.pop_back();
      return true;
    }

    bool parse_error(std::size_t, const std::string &, const json::exception &) override {
      return false;
    }
  };

  /**
   * @brief Parse typed JSON only after loss-preventing public SAX validation.
   */
  inline json parse(const std::string &bytes) {
    syntax_guard guard;
    if (!json::sax_parse(bytes, &guard)) {
      throw error {"catalog: invalid JSON, duplicate key, excessive depth or integer overflow"};
    }
    return json::parse(bytes);
  }

  /**
   * @brief Validate runtime-supported fields while retaining every unknown typed member.
   */
  inline void validate(const json &tree) {
    if (!tree.is_object() || !tree.contains("apps") || !tree["apps"].is_array() || !tree.contains("env") || !tree["env"].is_object()) {
      throw error {"catalog: expected apps array and env object"};
    }
    for (const auto &entry : tree["env"]) {
      if (!entry.is_string()) {
        throw error {"env: expected string values"};
      }
    }
    const auto valid_bool = [](const json &value) {
      return value.is_boolean() || (value.is_string() && (value == "true" || value == "false"));
    };
    for (const auto &app : tree["apps"]) {
      if (!app.is_object() || !app.contains("name") || !app["name"].is_string()) {
        throw error {"apps.name: expected a string"};
      }
      for (const auto *key : {"cmd", "output", "working-dir", "image-path"}) {
        if (app.contains(key) && !app[key].is_string()) {
          throw error {std::string {"apps."} + key + ": expected a string"};
        }
      }
      for (const auto *key : {"elevated", "exclude-global-prep-cmd", "auto-detach", "wait-all"}) {
        if (app.contains(key) && !valid_bool(app[key])) {
          throw error {std::string {"apps."} + key + ": expected a boolean"};
        }
      }
      if (app.contains("exit-timeout")) {
        const auto &value = app["exit-timeout"];
        int timeout = -1;
        if (value.is_number_integer() && !value.is_number_unsigned()) {
          auto number = value.get<std::int64_t>();
          if (number >= 0 && number <= INT32_MAX) {
            timeout = static_cast<int>(number);
          }
        } else if (value.is_number_unsigned()) {
          auto number = value.get<std::uint64_t>();
          if (number <= INT32_MAX) {
            timeout = static_cast<int>(number);
          }
        } else if (value.is_string()) {
          const auto &text = value.get_ref<const std::string &>();
          auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), timeout);
          if (ec != std::errc {} || end != text.data() + text.size()) {
            timeout = -1;
          }
        }
        if (timeout < 0) {
          throw error {"apps.exit-timeout: expected a nonnegative 32-bit integer"};
        }
      }
      if (app.contains("detached")) {
        if (!app["detached"].is_array()) {
          throw error {"apps.detached: expected an array"};
        }
        for (const auto &cmd : app["detached"]) {
          if (!cmd.is_string()) {
            throw error {"apps.detached: expected string entries"};
          }
        }
      }
      if (app.contains("prep-cmd")) {
        if (!app["prep-cmd"].is_array()) {
          throw error {"apps.prep-cmd: expected an array"};
        }
        for (const auto &cmd : app["prep-cmd"]) {
          if (!cmd.is_object()) {
            throw error {"apps.prep-cmd: expected object entries"};
          }
          for (const auto *key : {"do", "undo"}) {
            if (cmd.contains(key) && !cmd[key].is_string()) {
              throw error {"apps.prep-cmd: expected command strings"};
            }
          }
          if (cmd.contains("elevated") && !valid_bool(cmd["elevated"])) {
            throw error {"apps.prep-cmd.elevated: expected a boolean"};
          }
        }
      }
    }
  }

  /**
   * @brief Inject allocation inputs so collisions and exhaustion can be tested without filesystem effects.
   */
  struct allocation {
    std::function<std::string()> uuid;  ///< Fresh application UUID generator.
    std::function<std::uint32_t(const std::string &)> candidate;  ///< Initial positive-ID probe seed.
    std::vector<int> legacy;  ///< Previously calculated targets in stored array order.
    int maximum = INT32_MAX;  ///< Finite target space, reduced only by fixtures.
    std::optional<std::pair<int, std::string>> active;  ///< Captured active pair reserves authority after deletion.
  };

  /**
   * @brief Normalize identities and the retired allocation bijection without I/O or runtime publication.
   */
  inline void normalize(json &tree, const allocation &inputs) {
    validate(tree);
    if (inputs.maximum <= 0) {
      throw error {"apps.id: allocation space exhausted"};
    }
    std::map<int, std::string> ids;
    std::map<std::string, int> uuids;
    auto reserve = [&](int id, const std::string &uuid) {
      if (id <= 0 || id > inputs.maximum || (ids.contains(id) && ids.at(id) != uuid) || (uuids.contains(uuid) && uuids.at(uuid) != id)) {
        throw error {"miranda-app-identity: conflicting ID/UUID allocation"};
      }
      ids[id] = uuid;
      uuids[uuid] = id;
    };
    if (!tree.contains("miranda-app-identity")) {
      tree["miranda-app-identity"] = {{"version", 1}, {"allocated-ids", json::object()}};
    }
    auto &metadata = tree["miranda-app-identity"];
    if (!metadata.is_object() || !metadata.contains("version") || !metadata["version"].is_number_integer() || metadata["version"] != 1 || !metadata.contains("allocated-ids") || !metadata["allocated-ids"].is_object()) {
      throw error {"miranda-app-identity: unsupported schema"};
    }
    for (const auto &[key, value] : metadata["allocated-ids"].items()) {
      const auto uuid = value.is_string() ? canonical_uuid(value.get<std::string>()) : std::nullopt;
      if (!uuid) {
        throw error {"miranda-app-identity.allocated-ids: invalid UUID"};
      }
      reserve(positive_id(key), *uuid);
    }
    if (inputs.active && !inputs.active->second.empty()) {
      reserve(inputs.active->first, inputs.active->second);
    }
    auto &apps = tree["apps"];
    std::set<std::string> live_uuids;
    // Reserve every explicit UUID before requesting any generated UUID.
    for (auto &app : apps) {
      if (app.contains("uuid") && app["uuid"] != "") {
        const auto uuid = app["uuid"].is_string() ? canonical_uuid(app["uuid"].get<std::string>()) : std::nullopt;
        if (!uuid || !live_uuids.insert(*uuid).second) {
          throw error {"apps.uuid: invalid or duplicate UUID"};
        }
        app["uuid"] = *uuid;
      }
    }
    for (auto &app : apps) {
      if (!app.contains("uuid") || app["uuid"] == "") {
        std::optional<std::string> uuid;
        for (int attempt = 0; attempt < 128; ++attempt) {
          uuid = canonical_uuid(inputs.uuid());
          if (uuid && !uuids.contains(*uuid) && live_uuids.insert(*uuid).second) {
            break;
          }
          uuid.reset();
        }
        if (!uuid) {
          throw error {"apps.uuid: unique allocation failed"};
        }
        app["uuid"] = *uuid;
      }
    }
    std::set<int> live_ids;
    for (auto &app : apps) {
      const auto uuid = app["uuid"].get<std::string>();
      if (app.contains("id")) {
        const auto id = positive_id(app["id"]);
        if (!live_ids.insert(id).second) {
          throw error {"apps.id: duplicate application ID"};
        }
        reserve(id, uuid);
        app["id"] = std::to_string(id);
      } else if (uuids.contains(uuid)) {
        const auto id = uuids.at(uuid);
        if (!live_ids.insert(id).second) {
          throw error {"apps.id: duplicate application ID"};
        }
        app["id"] = std::to_string(id);
      }
    }
    // Collect all unambiguous old targets before a fallback can consume any of them.
    std::map<int, int> legacy_counts;
    for (std::size_t i = 0; i < apps.size() && i < inputs.legacy.size(); ++i) {
      if (!apps[i].contains("id")) {
        ++legacy_counts[inputs.legacy[i]];
      }
    }
    for (std::size_t i = 0; i < apps.size() && i < inputs.legacy.size(); ++i) {
      const auto id = inputs.legacy[i];
      if (!apps[i].contains("id") && id > 0 && id <= inputs.maximum && legacy_counts[id] == 1 && !ids.contains(id)) {
        reserve(id, apps[i]["uuid"].get<std::string>());
        apps[i]["id"] = std::to_string(id);
      }
    }
    for (auto &app : apps) {
      if (app.contains("id")) {
        continue;
      }
      const auto uuid = app["uuid"].get<std::string>();
      if (ids.size() >= static_cast<std::size_t>(inputs.maximum)) {
        throw error {"apps.id: allocation space exhausted"};
      }
      auto id = 1 + static_cast<int>(inputs.candidate(uuid) % static_cast<std::uint32_t>(inputs.maximum));
      // At most occupied+1 probes are needed; never iterate the entire integer space on exhaustion.
      for (std::size_t probe = 0; probe <= ids.size(); ++probe) {
        if (!ids.contains(id)) {
          break;
        }
        id = id == inputs.maximum ? 1 : id + 1;
      }
      if (ids.contains(id)) {
        throw error {"apps.id: allocation space exhausted"};
      }
      reserve(id, uuid);
      app["id"] = std::to_string(id);
    }
    auto ledger = json::object();
    for (const auto &[id, uuid] : ids) {
      ledger[std::to_string(id)] = uuid;
    }
    metadata["allocated-ids"] = std::move(ledger);
  }

  /**
   * @brief Merge editor-supported fields while preserving retained nested metadata or refusing ambiguous loss.
   */
  inline void merge_edit(json &retained, const json &edit) {
    if (!edit.is_object()) {
      throw error {"application: expected an object"};
    }
    auto fields = edit;
    if (!fields.contains("name")) {
      fields["name"] = retained.at("name");
    }
    validate(json {{"env", json::object()}, {"apps", json::array({fields})}});
    for (const auto *key : {"id", "uuid"}) {
      if (edit.contains(key) && (!retained.contains(key) || edit[key] != retained[key])) {
        throw error {"application identity is immutable"};
      }
    }
    for (const auto *key : {"name", "cmd", "output", "working-dir", "image-path", "elevated", "exclude-global-prep-cmd", "auto-detach", "wait-all", "exit-timeout", "detached"}) {
      if (edit.contains(key)) {
        retained[key] = edit[key];
      }
    }
    if (edit.contains("prep-cmd")) {
      if (!edit["prep-cmd"].is_array()) {
        throw error {"apps.prep-cmd: expected an array"};
      }
      const auto previous = retained.value("prep-cmd", json::array());
      auto commands = edit["prep-cmd"];
      std::set<std::size_t> matched;
      std::vector<std::size_t> unmatched;
      for (std::size_t current = 0; current < commands.size(); ++current) {
        auto &command = commands[current];
        if (!command.is_object()) {
          throw error {"apps.prep-cmd: expected object entries"};
        }
        std::vector<std::size_t> matches;
        for (std::size_t i = 0; i < previous.size(); ++i) {
          if (!matched.contains(i) && previous[i].value("do", "") == command.value("do", "") && previous[i].value("undo", "") == command.value("undo", "")) {
            matches.push_back(i);
          }
        }
        if (matches.size() == 1) {
          auto merged = previous[matches[0]];
          for (const auto &[key, value] : command.items()) {
            merged[key] = value;
          }
          command = std::move(merged);
          matched.insert(matches[0]);
        } else {
          unmatched.push_back(current);
        }
      }
      // Match the entire retained list first, so reordering or adding a command cannot hide a later match.
      for (const auto current : unmatched) {
        for (std::size_t i = 0; i < previous.size(); ++i) {
          if (matched.contains(i)) {
            continue;
          }
          for (const auto &[key, value] : previous[i].items()) {
            if (key != "do" && key != "undo" && key != "elevated" && !commands[current].contains(key)) {
              throw error {"apps.prep-cmd: retain nested metadata or use an unambiguous retained command"};
            }
          }
        }
      }
      retained["prep-cmd"] = std::move(commands);
    }
  }

  /**
   * @brief Resolve one supplied selector pair against live or captured active identities.
   * @param numeric Exact numeric query value, when present.
   * @param uuid UUID query value; an empty value is absent for numeric compatibility.
   * @param apps Live configured pairs only; the allocation ledger is never a launch catalog.
   * @param active Copied active pair after checking liveness.
   * @param resuming Supplied selectors must agree with the captured active pair.
   */
  inline std::pair<int, std::string> resolve(const std::optional<std::string> &numeric, const std::optional<std::string> &uuid, const std::vector<std::pair<int, std::string>> &apps, const std::pair<int, std::string> &active, bool resuming) {
    std::optional<int> id;
    std::optional<std::string> canonical;
    if (numeric) {
      int parsed = 0;
      const auto [end, ec] = std::from_chars(numeric->data(), numeric->data() + numeric->size(), parsed);
      if (numeric->empty() || ec != std::errc {} || end != numeric->data() + numeric->size() || parsed < -1 || std::to_string(parsed) != *numeric) {
        throw error {"appid: invalid numeric selector"};
      }
      id = parsed;
    }
    if (uuid && !uuid->empty()) {
      canonical = canonical_uuid(*uuid);
      if (!canonical) {
        throw error {"appuuid: invalid UUID selector"};
      }
    }
    if (id == -1 && !canonical) {
      throw error {"appid: UUID sentinel requires a nonempty valid appuuid"};
    }
    if (resuming && active.first <= 0) {
      throw error {"No active application to resume", 503};
    }
    if (!id && !canonical) {
      if (resuming && !uuid) {
        return active;
      }
      throw error {"Application selector is required"};
    }
    const auto find_pair = [&](auto predicate) -> std::pair<int, std::string> {
      if (active.first > 0 && predicate(active)) {
        return active;
      }
      const auto it = std::find_if(apps.begin(), apps.end(), predicate);
      if (it == apps.end()) {
        throw error {"Application selector is unavailable", 404};
      }
      return *it;
    };
    std::optional<std::pair<int, std::string>> by_id;
    if (id && *id >= 0) {
      by_id = *id == 0 ? std::pair<int, std::string> {0, ""} : find_pair([&](const auto &pair) {
        return pair.first == *id;
      });
    }
    std::optional<std::pair<int, std::string>> by_uuid;
    if (canonical) {
      by_uuid = find_pair([&](const auto &pair) {
        return pair.second == *canonical;
      });
    }
    if (by_id && by_uuid && *by_id != *by_uuid) {
      throw error {"Application selectors conflict"};
    }
    const auto selected = by_id ? *by_id : *by_uuid;
    if (resuming && selected != active) {
      throw error {"Resume selectors do not match the active application"};
    }
    return selected;
  }
}  // namespace proc::catalog
