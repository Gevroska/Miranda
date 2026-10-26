/**
 * @file src/permissions.h
 * @brief Implemented paired-device rights, retaining Apollo's protocol bit values.
 */
#pragma once

// standard includes
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace permissions {
  using mask_t = std::uint32_t;  ///< Persisted and protocol permission mask.

  constexpr mask_t controller = 0x00000100;  ///< Controller input, including auxiliary packets.
  constexpr mask_t touch = 0x00000200;  ///< Touchscreen input.
  constexpr mask_t pen = 0x00000400;  ///< Pen input.
  constexpr mask_t mouse = 0x00000800;  ///< Mouse movement, buttons and scrolling.
  constexpr mask_t keyboard = 0x00001000;  ///< Keyboard and Unicode input.
  constexpr mask_t inputs = controller | touch | pen | mouse | keyboard;  ///< Implemented input categories.
  constexpr mask_t list = 0x01000000;  ///< Application list and artwork.
  constexpr mask_t view = 0x02000000;  ///< Join an existing application stream.
  constexpr mask_t launch = 0x04000000;  ///< Start applications and explicitly stop all viewers.
  constexpr mask_t supported = inputs | list | view | launch;  ///< Only rights implemented by this host.

  /**
   * @brief Check that a grant contains no reserved or unimplemented rights.
   * @param mask Proposed grant.
   * @return Whether every bit is supported.
   */
  constexpr bool valid(mask_t mask) {
    return (mask & ~supported) == 0;
  }

  /**
   * @brief Apply the reference action inclusion hierarchy.
   * @param mask Current explicit grants.
   * @param required Requested right or effective action group.
   * @return Whether the requested operation is allowed.
   */
  constexpr bool allows(mask_t mask, mask_t required) {
    if (required == list) {
      return (mask & (list | view | launch)) != 0;
    }
    if (required == view) {
      return (mask & (view | launch)) != 0;
    }
    return required != 0 && (mask & required) == required;
  }

  /**
   * @brief Parse a persisted decimal grant without accepting partial or unsupported masks.
   * @param value Complete persisted scalar.
   * @return Valid grant, or no value for malformed or unsupported input.
   */
  inline std::optional<mask_t> parse(std::string_view value) {
    mask_t mask {};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), mask);
    if (value.empty() || error != std::errc {} || end != value.data() + value.size() || !valid(mask)) {
      return std::nullopt;
    }
    return mask;
  }
}  // namespace permissions
