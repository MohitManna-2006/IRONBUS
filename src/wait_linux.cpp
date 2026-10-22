#include "wait_linux.hpp"

#include "ironbus/bus.hpp"

#include <cerrno>
#include <cstring>
#include <limits>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace ironbus::detail {
namespace {
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
static_assert(alignof(std::atomic<std::uint32_t>) >= alignof(std::uint32_t));

std::uint32_t *futex_address(std::atomic<std::uint32_t> &word) noexcept {
  return reinterpret_cast<std::uint32_t *>(&word);
}
} // namespace

void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
  __asm__ __volatile__("yield" ::: "memory");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

void futex_wait(std::atomic<std::uint32_t> &word, std::uint32_t expected,
                std::chrono::steady_clock::time_point deadline) {
  const auto remaining = deadline - std::chrono::steady_clock::now();
  if (remaining <= std::chrono::steady_clock::duration::zero())
    return;
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining);
  struct timespec timeout {
    static_cast<time_t>(ns.count() / 1'000'000'000), static_cast<long>(ns.count() % 1'000'000'000)
  };
  const auto result = syscall(SYS_futex, futex_address(word), FUTEX_WAIT,
                              static_cast<int>(expected), &timeout, nullptr, 0);
  if (result >= 0 || errno == EAGAIN || errno == EINTR || errno == ETIMEDOUT)
    return;
  throw Error(ErrorCode::system, std::string("FUTEX_WAIT: ") + std::strerror(errno));
}

void futex_wake_one(std::atomic<std::uint32_t> &word) {
  const auto result = syscall(SYS_futex, futex_address(word), FUTEX_WAKE, 1, nullptr, nullptr, 0);
  if (result < 0)
    throw Error(ErrorCode::system, std::string("FUTEX_WAKE: ") + std::strerror(errno));
}

void futex_wake_all(std::atomic<std::uint32_t> &word) {
  const auto result = syscall(SYS_futex, futex_address(word), FUTEX_WAKE,
                              std::numeric_limits<int>::max(), nullptr, nullptr, 0);
  if (result < 0)
    throw Error(ErrorCode::system, std::string("FUTEX_WAKE(all): ") + std::strerror(errno));
}

} // namespace ironbus::detail
