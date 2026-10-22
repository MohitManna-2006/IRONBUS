#include "ironbus/bus.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <sched.h>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
std::uint64_t stamp() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
}
std::uint64_t arg(char **argv, int i, std::uint64_t fallback) {
  if (!argv[i])
    return fallback;
  std::uint64_t parsed{};
  const std::string_view value(argv[i]);
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size())
    throw std::runtime_error("benchmark numeric arguments must be unsigned decimal integers");
  return parsed;
}
ironbus::WaitStrategy strategy(const char *value) {
  if (!value || std::string_view(value) == "busy")
    return ironbus::WaitStrategy::busy_poll;
  if (std::string_view(value) == "futex")
    return ironbus::WaitStrategy::futex;
  if (std::string_view(value) == "adaptive")
    return ironbus::WaitStrategy::adaptive;
  throw std::runtime_error("strategy must be busy, futex, or adaptive");
}
std::optional<int> cpu_arg(char **argv, int i) {
  if (!argv[i] || std::string_view(argv[i]) == "none")
    return std::nullopt;
  auto cpu = arg(argv, i, 0);
  if (cpu >= CPU_SETSIZE)
    throw std::runtime_error("CPU index exceeds CPU_SETSIZE");
  return static_cast<int>(cpu);
}
void validate_cpu(std::optional<int> cpu) {
  if (!cpu)
    return;
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
    throw std::runtime_error("sched_getaffinity failed while validating requested CPUs");
  if (!CPU_ISSET(static_cast<std::size_t>(*cpu), &allowed))
    throw std::runtime_error("requested CPU is outside the current process affinity mask");
}
void pin_to_cpu(std::optional<int> cpu) {
  if (!cpu)
    return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<std::size_t>(*cpu), &set);
  if (sched_setaffinity(0, sizeof(set), &set) != 0)
    throw std::runtime_error("sched_setaffinity failed for requested CPU");
}
double cpu_seconds(const rusage &usage) {
  return static_cast<double>(usage.ru_utime.tv_sec) +
         static_cast<double>(usage.ru_utime.tv_usec) / 1e6 +
         static_cast<double>(usage.ru_stime.tv_sec) +
         static_cast<double>(usage.ru_stime.tv_usec) / 1e6;
}
} // namespace
int main(int argc, char **argv) {
  auto n = "bench_" + std::to_string(getpid());
  try {
    if (argc > 9)
      throw std::runtime_error("too many benchmark arguments");
    auto count = argc > 1 ? arg(argv, 1, 100000) : 100000;
    auto raw_capacity = argc > 2 ? arg(argv, 2, 1024) : 1024;
    auto raw_payload_size = argc > 3 ? arg(argv, 3, 8) : 8;
    ironbus::WaitStrategy publisher_strategy{};
    ironbus::WaitStrategy subscriber_strategy{};
    auto raw_spin_count = argc > 6 ? arg(argv, 6, 1000) : 1000;
    if (raw_capacity > std::numeric_limits<std::uint32_t>::max() ||
        raw_payload_size > std::numeric_limits<std::uint32_t>::max() || raw_spin_count > 10'000'000)
      throw std::runtime_error("capacity, payload, or spin count is out of supported range");
    auto capacity = static_cast<std::uint32_t>(raw_capacity);
    auto payload_size = static_cast<std::uint32_t>(raw_payload_size);
    auto spin_count = static_cast<std::uint32_t>(raw_spin_count);
    std::optional<int> publisher_cpu, subscriber_cpu;
    if (count == 0 || payload_size < 8 || payload_size > ironbus::kPayloadBytes) {
      std::cerr
          << "usage: ironbus_bench [count] [capacity] [payload] [pub-strategy] [sub-strategy] "
             "[spin-count] [pub-cpu|none] [sub-cpu|none]\n";
      return 2;
    }
    struct utsname environment {};
    if (uname(&environment) != 0) {
      std::cerr << "benchmark error: unable to read kernel environment\n";
      return 1;
    }
    publisher_strategy = strategy(argc > 4 ? argv[4] : nullptr);
    subscriber_strategy = strategy(argc > 5 ? argv[5] : nullptr);
    publisher_cpu = cpu_arg(argv, 7);
    subscriber_cpu = cpu_arg(argv, 8);
    validate_cpu(publisher_cpu);
    validate_cpu(subscriber_cpu);
    auto creator = ironbus::Bus::create(n, capacity);
    auto child = fork();
    if (child < 0)
      throw std::runtime_error("fork failed");
    if (child == 0) {
      try {
        pin_to_cpu(subscriber_cpu);
        auto sub = ironbus::Bus::attach(n);
        sub.set_wait_strategy(subscriber_strategy, spin_count);
        sub.claim(ironbus::Role::subscriber);
        for (std::uint64_t i = 0; i < count; ++i) {
          auto m = sub.receive(std::chrono::seconds(60));
          // The subscriber performs the normal copy and ACK path. The
          // publisher records its observed ACK completion time.
          sub.acknowledge(m.sequence);
        }
        _exit(0);
      } catch (...) {
        _exit(2);
      }
    }
    pin_to_cpu(publisher_cpu);
    auto pub = ironbus::Bus::attach(n);
    pub.set_wait_strategy(publisher_strategy, spin_count);
    pub.claim(ironbus::Role::publisher);
    std::vector<std::uint64_t> latency;
    latency.reserve(static_cast<std::size_t>(count));
    auto begin = Clock::now();
    for (std::uint64_t i = 0; i < count; ++i) {
      std::array<std::byte, ironbus::kPayloadBytes> data{};
      auto sent = stamp();
      std::memcpy(data.data(), &sent, sizeof(sent));
      const auto sequence = pub.publish({data.data(), payload_size}, std::chrono::seconds(60));
      pub.wait_for_acknowledgement(sequence, std::chrono::seconds(60));
      // Round trip includes transport, subscriber copy/ACK, scheduling, and
      // publisher wake/observation under the configured policy.
      latency.push_back(stamp() - sent);
    }
    auto elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
    int status{};
    struct rusage child_usage {};
    if (wait4(child, &status, 0, &child_usage) < 0)
      throw std::runtime_error("wait4 failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      throw std::runtime_error("subscriber benchmark process failed");
    std::sort(latency.begin(), latency.end());
    auto percentile = [&](double p) {
      return latency[static_cast<std::size_t>(p * static_cast<double>(latency.size() - 1))];
    };
    struct rusage parent_usage {};
    if (getrusage(RUSAGE_SELF, &parent_usage) != 0)
      throw std::runtime_error("getrusage failed");
    std::cout
        << "messages=" << count << " payload_bytes=" << payload_size << " capacity=" << capacity
        << " system=" << environment.sysname << " kernel=" << environment.release
        << " machine=" << environment.machine
        << " logical_cpus=" << std::thread::hardware_concurrency() << " publisher_strategy="
        << (publisher_strategy == ironbus::WaitStrategy::busy_poll ? "busy"
            : publisher_strategy == ironbus::WaitStrategy::futex   ? "futex"
                                                                   : "adaptive")
        << " subscriber_strategy="
        << (subscriber_strategy == ironbus::WaitStrategy::busy_poll ? "busy"
            : subscriber_strategy == ironbus::WaitStrategy::futex   ? "futex"
                                                                    : "adaptive")
        << " spin_count=" << spin_count
        << " publisher_cpu=" << (publisher_cpu ? std::to_string(*publisher_cpu) : "none")
        << " subscriber_cpu=" << (subscriber_cpu ? std::to_string(*subscriber_cpu) : "none")
        << " clock=steady_clock timestamp_resolution=implementation-defined scheduling=uncontrolled"
        << " elapsed_s=" << elapsed
        << " messages_per_second=" << static_cast<double>(count) / elapsed
        << " publisher_to_ack_observed_ns_p50=" << percentile(.50) << " p99=" << percentile(.99)
        << " p999=" << percentile(.999)
        << " combined_cpu_seconds=" << cpu_seconds(parent_usage) + cpu_seconds(child_usage) << '\n';
    ironbus::Bus::unlink(n);
  } catch (const std::exception &e) {
    try {
      ironbus::Bus::unlink(n);
    } catch (...) {
    }
    std::cerr << "benchmark error: " << e.what() << '\n';
    return 1;
  }
}
