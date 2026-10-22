#include "ironbus/bus.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
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
  return argv[i] ? std::strtoull(argv[i], nullptr, 10) : fallback;
}
} // namespace
int main(int argc, char **argv) {
  auto count = argc > 1 ? arg(argv, 1, 100000) : 100000;
  auto capacity = static_cast<std::uint32_t>(argc > 2 ? arg(argv, 2, 1024) : 1024);
  auto payload_size = static_cast<std::uint32_t>(argc > 3 ? arg(argv, 3, 8) : 8);
  if (count == 0 || payload_size < 8 || payload_size > ironbus::kPayloadBytes) {
    std::cerr << "usage: ironbus_bench [count>0] [power-of-two capacity] [payload 8..240]\n";
    return 2;
  }
  struct utsname environment {};
  if (uname(&environment) != 0) {
    std::cerr << "benchmark error: unable to read kernel environment\n";
    return 1;
  }
  auto n = "bench_" + std::to_string(getpid());
  try {
    auto creator = ironbus::Bus::create(n, capacity);
    auto child = fork();
    if (child < 0)
      throw std::runtime_error("fork failed");
    if (child == 0) {
      try {
        auto sub = ironbus::Bus::attach(n);
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
    auto pub = ironbus::Bus::attach(n);
    pub.claim(ironbus::Role::publisher);
    std::vector<std::uint64_t> latency;
    latency.reserve(static_cast<std::size_t>(count));
    auto begin = Clock::now();
    for (std::uint64_t i = 0; i < count; ++i) {
      std::array<std::byte, ironbus::kPayloadBytes> data{};
      auto sent = stamp();
      std::memcpy(data.data(), &sent, sizeof(sent));
      pub.publish({data.data(), payload_size}, std::chrono::seconds(60));
      while (pub.stats().acknowledged < i + 1)
        std::this_thread::yield();
      // This round-trip timestamp measures the observed ACK completion time.
      // It bounds end-to-end transport plus scheduling and ACK polling.
      latency.push_back(stamp() - sent);
    }
    auto elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
    int status{};
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      throw std::runtime_error("subscriber benchmark process failed");
    std::sort(latency.begin(), latency.end());
    auto percentile = [&](double p) {
      return latency[static_cast<std::size_t>(p * static_cast<double>(latency.size() - 1))];
    };
    std::cout
        << "messages=" << count << " payload_bytes=" << payload_size << " capacity=" << capacity
        << " system=" << environment.sysname << " kernel=" << environment.release
        << " machine=" << environment.machine
        << " logical_cpus=" << std::thread::hardware_concurrency()
        << " clock=steady_clock timestamp_resolution=implementation-defined scheduling=uncontrolled"
        << " elapsed_s=" << elapsed
        << " messages_per_second=" << static_cast<double>(count) / elapsed
        << " publisher_to_ack_observed_ns_p50=" << percentile(.50) << " p99=" << percentile(.99)
        << " p999=" << percentile(.999) << " cpu_usage=not_collected (use /usr/bin/time -v)\n";
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
