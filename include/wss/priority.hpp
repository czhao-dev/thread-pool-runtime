#pragma once

#include <array>
#include <cstddef>

namespace wss {

// Scheduling priority for a task. Workers that honor priority (currently
// WorkStealingScheduler, optionally ThreadPerCoreScheduler) always prefer
// High work over Normal over Background, at every stage of scheduling.
enum class Priority : std::size_t {
    High = 0,
    Normal = 1,
    Background = 2,
};

inline constexpr std::size_t kPriorityCount = 3;

// Scan order workers use when looking for the next task.
inline constexpr std::array<Priority, kPriorityCount> kPriorityOrder{
    Priority::High,
    Priority::Normal,
    Priority::Background,
};

} // namespace wss
