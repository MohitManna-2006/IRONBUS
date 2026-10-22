#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ironbus {

inline constexpr std::size_t kPayloadBytes = 240;
inline constexpr std::size_t kSlotBytes = 256;

enum class ErrorCode {
  system,
  invalid_argument,
  incompatible_layout,
  unsafe_initialization,
  role_busy,
  full,
  empty,
  timeout,
  corrupt_message,
};

class Error final : public std::runtime_error {
public:
  Error(ErrorCode code, std::string message);
  [[nodiscard]] ErrorCode code() const noexcept {
    return code_;
  }

private:
  ErrorCode code_;
};

struct Message {
  std::uint64_t sequence{};
  std::uint32_t size{};
  std::byte payload[kPayloadBytes]{};
};

struct Stats {
  std::uint64_t published{};
  std::uint64_t acknowledged{};
  std::uint32_t capacity{};
  std::uint32_t slot_size{};
};

enum class Role { publisher, subscriber };
enum class WaitStrategy { busy_poll, futex, adaptive };

class Bus final {
public:
  Bus() noexcept;
  ~Bus();
  Bus(Bus &&) noexcept;
  Bus &operator=(Bus &&) noexcept;
  Bus(const Bus &) = delete;
  Bus &operator=(const Bus &) = delete;

  static Bus create(std::string_view name, std::uint32_t capacity);
  static Bus attach(std::string_view name);
  static void unlink(std::string_view name);

  void claim(Role role);
  // Wait policy is process-local and must be selected before claiming a role.
  // Adaptive mode spins for spin_budget predicate checks before sleeping.
  void set_wait_strategy(WaitStrategy strategy, std::uint32_t spin_budget = 1000);
  [[nodiscard]] WaitStrategy wait_strategy() const;
  [[nodiscard]] Stats stats() const;
  [[nodiscard]] std::uint32_t capacity() const;

  // Publish waits up to timeout for space. No allocation is performed on the
  // successful hot path. Throws Error(timeout) if backpressure persists.
  std::uint64_t publish(std::span<const std::byte> payload, std::chrono::milliseconds timeout);
  // Receive copies the next unacknowledged message. Repeated receive before
  // acknowledge returns the same message.
  Message receive(std::chrono::milliseconds timeout);
  void acknowledge(std::uint64_t sequence);

private:
  struct Impl;
  explicit Bus(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string shm_name(std::string_view name);

} // namespace ironbus
