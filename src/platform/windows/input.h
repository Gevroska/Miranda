/**
 * @file src/platform/windows/input.h
 * @brief Windows input identity and ViGEm report helpers that do not submit input to a device.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>

// platform includes
#include <Windows.h>

// lib includes
#include <ViGEm/Client.h>

// local includes
#include "src/platform/common.h"

namespace platf {
  /**
   * @brief Resolve held-key ownership using the current keyboard path and static layout translation.
   * @param input Current shared keyboard backend.
   * @param code Mapped Windows virtual key.
   * @param flags Protocol keyboard flags.
   * @param extended Client extended-key selection.
   * @return Effective output identity, retaining separate scan-code and unmappable virtual-key domains.
   */
  std::uint32_t keyboard_key_identity(input_t &input, std::uint16_t code, std::uint8_t flags, bool extended);
#ifdef SUNSHINE_TESTS
  namespace testing {
    /**
     * @brief Select deterministic static-layout translations without changing the host keyboard or driver.
     * @param mapper Virtual-key scan mapping receiving true for direct-VK extended mode, or empty for MapVirtualKeyW.
     * @param hid Optional current keyboard path override; omission inspects the actual device nodes.
     */
    void set_keyboard_identity_environment(std::function<std::uint16_t(std::uint16_t, bool)> mapper, std::optional<bool> hid = {});
  }  // namespace testing
#endif
}  // namespace platf

namespace platf::vigem {
  /**
   * @brief Scale an 8-bit rumble amplitude to the established 16-bit range.
   *
   * @param strength Rumble amplitude reported by ViGEm.
   * @return Amplitude multiplied by 256.
   */
  std::uint16_t scale_rumble(std::uint8_t strength);

  /**
   * @brief Update a DualShock 4 touch report and its pointer allocation state.
   *
   * @param ds4_report Report receiving the touch event.
   * @param pointer_id_map Mapping from client pointer IDs to the two report slots.
   * @param available_pointers Byte mask of unallocated report slots.
   * @param touch Client touch event.
   * @return True when the updated report should be submitted to ViGEm.
   */
  bool update_touch(DS4_REPORT_EX &ds4_report, std::map<std::uint32_t, std::uint8_t> &pointer_id_map, std::byte &available_pointers, const gamepad_touch_t &touch);

  /**
   * @brief Update DualShock 4 battery fields while preserving unknown values.
   *
   * @param ds4_report Report receiving the battery event.
   * @param battery Client battery state and percentage.
   */
  void update_battery(DS4_REPORT_EX &ds4_report, const gamepad_battery_t &battery);
#ifdef SUNSHINE_TESTS
  /**
   * @brief Bind a driver-free feedback destination through the production generation registry.
   * @param id Existing callback ID or zero for a new target.
   * @param queue Test mailbox.
   * @param index Client-relative controller index.
   * @return Opaque callback ID.
   */
  std::uintptr_t bind_feedback_for_testing(std::uintptr_t id, feedback_queue_t queue, std::uint8_t index);
  /**
   * @brief Exercise production target invalidation without a driver.
   * @param id Original callback ID.
   */
  void unbind_feedback_for_testing(std::uintptr_t id);
  /**
   * @brief Capture actual rumble/RGB work before rebind or target removal.
   * @param id Original callback ID.
   * @param large Large motor strength.
   * @param small Small motor strength.
   * @return Actual deferred production operation.
   */
  std::function<void()> capture_feedback_for_testing(std::uintptr_t id, std::uint8_t large, std::uint8_t small);
#endif
}  // namespace platf::vigem
