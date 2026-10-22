#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ironbus::detail {

void cpu_relax() noexcept;
void futex_wait(std::atomic<std::uint32_t> &word, std::uint32_t expected,
                std::chrono::steady_clock::time_point deadline);
void futex_wake_one(std::atomic<std::uint32_t> &word);

} // namespace ironbus::detail
