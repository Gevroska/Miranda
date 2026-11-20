/**
 * @file tests/unit/platform/windows/test_misc.cpp
 * @brief Tests for Windows process creation and thread priority mapping.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <array>
  #include <optional>
  #include <utility>

  // platform includes
  #include <Windows.h>

  // local includes
  #include <src/platform/common.h>
  #include <src/utility.h>

namespace platf::test_support {
  /** @brief Apply the production flag decision to a checked job limit or unknown policy. */
  DWORD creation_flags_for_job(DWORD flags, std::optional<DWORD> known_job_limits);
}  // namespace platf::test_support

TEST(WindowsProcessCreationFlagsTest, UnknownParentPolicyPreservesExistingFlags) {
  constexpr DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB;
  EXPECT_EQ(platf::test_support::creation_flags_for_job(flags, std::nullopt), flags);
}

TEST(WindowsProcessCreationFlagsTest, RestrictiveParentPreservesConsoleUnicodeAndExtendedStartupFlags) {
  constexpr DWORD retained = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE;
  EXPECT_EQ(platf::test_support::creation_flags_for_job(retained | CREATE_BREAKAWAY_FROM_JOB, DWORD {0}), retained);
}

TEST(WindowsProcessCreationFlagsTest, ExplicitBreakawayPermissionPreservesTheRequest) {
  constexpr DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB;
  EXPECT_EQ(platf::test_support::creation_flags_for_job(flags, DWORD {JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE}), flags);
}

TEST(WindowsProcessCreationFlagsTest, SilentBreakawayUsesParentPolicyWithoutAnExplicitRequest) {
  constexpr DWORD retained = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
  EXPECT_EQ(platf::test_support::creation_flags_for_job(retained | CREATE_BREAKAWAY_FROM_JOB, DWORD {JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK}), retained);
}

TEST(WindowsProcessCreationFlagsTest, RestrictivePolicyDoesNotAddAnAbsentBreakawayRequest) {
  constexpr DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE;
  EXPECT_EQ(platf::test_support::creation_flags_for_job(flags, DWORD {JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE}), flags);
}

TEST(WindowsThreadPriorityTest, MapsEveryPriorityAndIgnoresUnknownValues) {
  using enum platf::thread_priority_e;
  const auto thread = GetCurrentThread();
  const auto original_priority = GetThreadPriority(thread);
  ASSERT_NE(original_priority, THREAD_PRIORITY_ERROR_RETURN);
  const auto restore_priority = util::fail_guard([thread, original_priority]() {
    EXPECT_TRUE(SetThreadPriority(thread, original_priority));
  });
  const std::array cases {
    std::pair {low, THREAD_PRIORITY_BELOW_NORMAL},
    std::pair {normal, THREAD_PRIORITY_NORMAL},
    std::pair {high, THREAD_PRIORITY_ABOVE_NORMAL},
    std::pair {critical, THREAD_PRIORITY_HIGHEST},
  };
  for (const auto &[priority, expected] : cases) {
    platf::adjust_thread_priority(priority);
    EXPECT_EQ(GetThreadPriority(thread), expected);
  }
  platf::adjust_thread_priority(static_cast<platf::thread_priority_e>(-1));
  EXPECT_EQ(GetThreadPriority(thread), THREAD_PRIORITY_HIGHEST);
}
#endif
