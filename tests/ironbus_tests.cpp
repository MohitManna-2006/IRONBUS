#include "ironbus/bus.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
void check(bool ok, const char *text) {
  if (!ok)
    throw std::runtime_error(text);
}
std::string name(const char *suffix) {
  return "test_" + std::to_string(getpid()) + "_" + suffix;
}
pid_t spawn(const std::string &exe, std::initializer_list<std::string> args) {
  auto pid = fork();
  if (pid == 0) {
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(exe.c_str()));
    for (const auto &arg : args)
      argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    execv(exe.c_str(), argv.data());
    _exit(127);
  }
  if (pid < 0)
    throw std::runtime_error("fork failed");
  return pid;
}
int wait_bounded(pid_t pid, std::chrono::seconds timeout) {
  auto until = std::chrono::steady_clock::now() + timeout;
  int status{};
  while (std::chrono::steady_clock::now() < until) {
    auto r = waitpid(pid, &status, WNOHANG);
    if (r == pid)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    std::this_thread::sleep_for(10ms);
  }
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  return 124;
}
struct Cleanup {
  std::string n;
  ~Cleanup() {
    try {
      ironbus::Bus::unlink(n);
    } catch (...) {
    }
  }
};

void wait_policy_configuration() {
  auto n = name("wait_policy");
  Cleanup cleanup{n};
  auto bus = ironbus::Bus::create(n, 2);
  bus.set_wait_strategy(ironbus::WaitStrategy::futex);
  check(bus.wait_strategy() == ironbus::WaitStrategy::futex,
        "wait policy selection persists on handle");
  bool bad_budget = false;
  try {
    bus.set_wait_strategy(ironbus::WaitStrategy::adaptive, 10'000'001);
  } catch (const ironbus::Error &e) {
    bad_budget = e.code() == ironbus::ErrorCode::invalid_argument;
  }
  check(bad_budget, "unbounded adaptive spin budget rejected");
  bool bad_policy = false;
  try {
    bus.set_wait_strategy(static_cast<ironbus::WaitStrategy>(99));
  } catch (const ironbus::Error &e) {
    bad_policy = e.code() == ironbus::ErrorCode::invalid_argument;
  }
  check(bad_policy, "unknown wait policy rejected");
  bus.claim(ironbus::Role::publisher);
  bool late_change = false;
  try {
    bus.set_wait_strategy(ironbus::WaitStrategy::busy_poll);
  } catch (const ironbus::Error &e) {
    late_change = e.code() == ironbus::ErrorCode::invalid_argument;
  }
  check(late_change, "role policy immutable after claim");
}

void basic_and_restart() {
  auto n = name("basic");
  Cleanup cleanup{n};
  auto pub = ironbus::Bus::create(n, 4);
  pub.claim(ironbus::Role::publisher);
  for (std::uint64_t i = 0; i < 4; ++i) {
    std::byte b{static_cast<unsigned char>(i)};
    check(pub.publish({&b, 1}, 100ms) == i + 1, "sequence order");
  }
  bool ack_timed_out = false;
  try {
    pub.wait_for_acknowledgement(1, 20ms);
  } catch (const ironbus::Error &e) {
    ack_timed_out = e.code() == ironbus::ErrorCode::timeout;
  }
  check(ack_timed_out, "publisher acknowledgement wait times out while subscriber is stalled");
  bool future_ack_rejected = false;
  try {
    pub.wait_for_acknowledgement(5, 20ms);
  } catch (const ironbus::Error &e) {
    future_ack_rejected = e.code() == ironbus::ErrorCode::invalid_argument;
  }
  check(future_ack_rejected, "publisher cannot wait for an unpublished sequence");
  bool timed = false;
  try {
    std::byte b{};
    pub.publish({&b, 1}, 30ms);
  } catch (const ironbus::Error &e) {
    timed = e.code() == ironbus::ErrorCode::timeout;
  }
  check(timed, "full ring backpressure timeout");
  // Publisher restart resumes at the persisted published cursor.
  pub = ironbus::Bus{};
  pub = ironbus::Bus::attach(n);
  pub.claim(ironbus::Role::publisher);
  auto sub = ironbus::Bus::attach(n);
  sub.claim(ironbus::Role::subscriber);
  bool early_ack = false;
  try {
    sub.acknowledge(1);
  } catch (const ironbus::Error &e) {
    early_ack = e.code() == ironbus::ErrorCode::invalid_argument;
  }
  check(early_ack, "cannot acknowledge before receive");
  for (std::uint64_t i = 0; i < 4; ++i) {
    auto m = sub.receive(100ms);
    check(m.sequence == i + 1 && m.size == 1 &&
              m.payload[0] == std::byte{static_cast<unsigned char>(i)},
          "integrity / wraparound");
    sub.acknowledge(m.sequence);
  }
  pub.wait_for_acknowledgement(4, 100ms);
  std::byte b{9};
  check(pub.publish({&b, 1}, 100ms) == 5, "publisher restart sequence");
  auto m = sub.receive(100ms);
  check(m.sequence == 5, "receive after wraparound");
  sub.acknowledge(5);
  bool empty = false;
  try {
    sub.receive(20ms);
  } catch (const ironbus::Error &e) {
    empty = e.code() == ironbus::ErrorCode::timeout;
  }
  check(empty, "empty ring timeout");
}

void process_attach_and_roles(const std::string &exe) {
  auto n = name("process");
  Cleanup cleanup{n};
  auto create = ironbus::Bus::create(n, 8);
  auto sub = spawn(exe, {"sub", "--name", n, "--count", "32", "--timeout-ms", "5000"});
  std::this_thread::sleep_for(100ms);
  auto competing = spawn(exe, {"sub", "--name", n, "--count", "1", "--timeout-ms", "500"});
  check(wait_bounded(competing, 2s) == 1, "concurrent subscriber role must be rejected");
  auto pub = spawn(exe, {"pub", "--name", n, "--count", "32", "--timeout-ms", "5000"});
  check(wait_bounded(pub, 7s) == 0, "independent publisher process");
  check(wait_bounded(sub, 7s) == 0, "independent subscriber process");

  // With no subscriber holding the role, this process fills the bounded ring
  // and remains alive under backpressure, giving the competing publisher a
  // deterministic window to attempt the same role.
  auto stalled_pub = spawn(exe, {"pub", "--name", n, "--count", "9", "--timeout-ms", "5000"});
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (create.stats().published < 40 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(5ms);
  check(create.stats().published == 40, "publisher reaches full ring while retaining its role");
  auto competing_pub = spawn(exe, {"pub", "--name", n, "--count", "1", "--timeout-ms", "500"});
  check(wait_bounded(competing_pub, 2s) == 1, "concurrent publisher role must be rejected");
  kill(stalled_pub, SIGKILL);
  check(wait_bounded(stalled_pub, 2s) == 128, "stalled publisher exits after crash simulation");
}

void publisher_process_restart(const std::string &exe) {
  auto n = name("pub_restart");
  Cleanup cleanup{n};
  auto creator = ironbus::Bus::create(n, 8);
  auto first = spawn(exe, {"pub", "--name", n, "--count", "4", "--timeout-ms", "2000"});
  check(wait_bounded(first, 4s) == 0, "first publisher process exits cleanly");
  auto second = spawn(exe, {"pub", "--name", n, "--count", "4", "--timeout-ms", "2000"});
  check(wait_bounded(second, 4s) == 0, "publisher process restarts and resumes");
  auto sub = spawn(exe, {"sub", "--name", n, "--count", "8", "--timeout-ms", "2000"});
  check(wait_bounded(sub, 4s) == 0, "subscriber validates sequences across publisher restart");
  check(creator.stats().published == 8 && creator.stats().acknowledged == 8,
        "publisher restart preserves cursors");
}

void wait_strategy_processes(const std::string &exe) {
  const char *strategies[] = {"busy", "futex", "adaptive"};
  for (const auto *strategy : strategies) {
    auto n = name(strategy);
    Cleanup cleanup{n};
    auto creator = ironbus::Bus::create(n, 8);
    auto sub = spawn(exe, {"sub", "--name", n, "--count", "64", "--timeout-ms", "3000",
                           "--strategy", strategy, "--spin-count", "32"});
    std::this_thread::sleep_for(40ms);
    auto pub = spawn(exe, {"pub", "--name", n, "--count", "64", "--timeout-ms", "3000",
                           "--strategy", strategy, "--spin-count", "32"});
    check(wait_bounded(pub, 5s) == 0, "publisher completes with selected wait strategy");
    check(wait_bounded(sub, 5s) == 0, "subscriber completes with selected wait strategy");
    check(creator.stats().published == 64 && creator.stats().acknowledged == 64,
          "wait strategy preserves all messages through repeated ring wraparound");
  }
}

void wait_strategy_timeouts() {
  const ironbus::WaitStrategy strategies[] = {ironbus::WaitStrategy::busy_poll,
                                              ironbus::WaitStrategy::futex,
                                              ironbus::WaitStrategy::adaptive};
  for (auto strategy : strategies) {
    auto n = name("wait_timeout");
    Cleanup cleanup{n};
    auto pub = ironbus::Bus::create(n, 2);
    pub.set_wait_strategy(strategy, 16);
    pub.claim(ironbus::Role::publisher);
    std::byte value{};
    pub.publish({&value, 1}, 100ms);
    pub.publish({&value, 1}, 100ms);
    bool full_timeout = false;
    try {
      pub.publish({&value, 1}, 25ms);
    } catch (const ironbus::Error &e) {
      full_timeout = e.code() == ironbus::ErrorCode::timeout;
    }
    check(full_timeout, "full-ring timeout is bounded for each policy");

    auto sub = ironbus::Bus::attach(n);
    sub.set_wait_strategy(strategy, 16);
    sub.claim(ironbus::Role::subscriber);
    auto message = sub.receive(100ms);
    sub.acknowledge(message.sequence);
    pub.publish({&value, 1}, 100ms);
    message = sub.receive(100ms);
    sub.acknowledge(message.sequence);
    message = sub.receive(100ms);
    sub.acknowledge(message.sequence);
    bool empty_timeout = false;
    try {
      sub.receive(25ms);
    } catch (const ironbus::Error &e) {
      empty_timeout = e.code() == ironbus::ErrorCode::timeout;
    }
    check(empty_timeout, "empty-ring timeout is bounded for each policy");
  }
}

void crash_redelivery(const std::string &exe) {
  auto n = name("crash");
  Cleanup cleanup{n};
  auto p = ironbus::Bus::create(n, 4);
  p.claim(ironbus::Role::publisher);
  std::uint64_t payload = 0;
  p.publish({reinterpret_cast<const std::byte *>(&payload), sizeof(payload)}, 100ms);
  (void)exe;
  auto dying = fork();
  if (dying == 0) {
    try {
      auto consumer = ironbus::Bus::attach(n);
      consumer.claim(ironbus::Role::subscriber);
      (void)consumer.receive(2s);
      kill(getpid(), SIGKILL); // Crash after copy and before ACK.
      _exit(2);
    } catch (...) {
      _exit(2);
    }
  }
  check(dying > 0, "fork crash-test subscriber");
  check(wait_bounded(dying, 4s) == 128, "subscriber crashes after receive and before ACK");
  auto recovered = ironbus::Bus::attach(n);
  recovered.claim(ironbus::Role::subscriber);
  auto redelivered = recovered.receive(2s);
  check(redelivered.sequence == 1 && redelivered.size == sizeof(payload),
        "unacknowledged message redelivered after subscriber restart");
  recovered.acknowledge(redelivered.sequence);
  check(p.stats().acknowledged == 1, "restarted subscriber ACK persisted");
}

void reject_incompatible() {
  auto n = name("bad");
  Cleanup cleanup{n};
  auto b = ironbus::Bus::create(n, 2);
  // Mutate through mapped ABI is intentionally omitted from public API. A
  // truncated object exercises safe initialization/layout rejection.
  auto raw = shm_open(ironbus::shm_name(n).c_str(), O_RDWR, 0);
  check(raw >= 0, "open raw shm");
  check(ftruncate(raw, 16) == 0, "truncate raw shm");
  close(raw);
  bool rejected = false;
  try {
    auto attached = ironbus::Bus::attach(n);
    (void)attached;
  } catch (const ironbus::Error &e) {
    rejected = e.code() == ironbus::ErrorCode::unsafe_initialization;
  }
  check(rejected, "incompatible/truncated segment rejected");
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("expected CLI executable path");
    wait_policy_configuration();
    basic_and_restart();
    process_attach_and_roles(argv[1]);
    publisher_process_restart(argv[1]);
    wait_strategy_processes(argv[1]);
    wait_strategy_timeouts();
    crash_redelivery(argv[1]);
    reject_incompatible();
    std::cout << "all IRONBUS tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << "test failure: " << e.what() << '\n';
    return 1;
  }
}
