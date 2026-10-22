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
#include <utility>
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
  check(bus.wait_strategy() == ironbus::WaitStrategy::busy_poll,
        "default wait policy preserves Phase 1 busy polling");
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
  auto sub = ironbus::Bus::attach(n);
  sub.claim(ironbus::Role::subscriber);
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
  auto sub = spawn(
      exe, {"sub", "--name", n, "--count", "32", "--timeout-ms", "5000", "--strategy", "futex"});
  std::this_thread::sleep_for(100ms);
  auto competing = spawn(
      exe, {"sub", "--name", n, "--count", "1", "--timeout-ms", "500", "--strategy", "futex"});
  check(wait_bounded(competing, 2s) == 1, "concurrent subscriber role must be rejected");
  auto pub = spawn(
      exe, {"pub", "--name", n, "--count", "32", "--timeout-ms", "5000", "--strategy", "futex"});
  check(wait_bounded(pub, 7s) == 0, "independent publisher process");
  check(wait_bounded(sub, 7s) == 0, "independent subscriber process");

  // With no subscriber holding the role, this process fills the bounded ring
  // and remains alive under backpressure, giving the competing publisher a
  // deterministic window to attempt the same role.
  auto stalled_pub = spawn(
      exe, {"pub", "--name", n, "--count", "9", "--timeout-ms", "5000", "--strategy", "futex"});
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (create.stats().published < 40 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(5ms);
  check(create.stats().published == 40, "publisher reaches full ring while retaining its role");
  auto competing_pub = spawn(
      exe, {"pub", "--name", n, "--count", "1", "--timeout-ms", "500", "--strategy", "futex"});
  check(wait_bounded(competing_pub, 2s) == 1, "concurrent publisher role must be rejected");
  kill(stalled_pub, SIGKILL);
  check(wait_bounded(stalled_pub, 2s) == 128, "stalled publisher exits after crash simulation");
}

void publisher_process_restart(const std::string &exe) {
  auto n = name("pub_restart");
  Cleanup cleanup{n};
  auto creator = ironbus::Bus::create(n, 8);
  auto sub = spawn(
      exe, {"sub", "--name", n, "--count", "8", "--timeout-ms", "5000", "--strategy", "futex"});
  std::this_thread::sleep_for(40ms);
  auto first = spawn(
      exe, {"pub", "--name", n, "--count", "4", "--timeout-ms", "2000", "--strategy", "futex"});
  check(wait_bounded(first, 4s) == 0, "first publisher process exits cleanly");
  auto second = spawn(
      exe, {"pub", "--name", n, "--count", "4", "--timeout-ms", "2000", "--strategy", "futex"});
  check(wait_bounded(second, 4s) == 0, "publisher process restarts and resumes");
  check(wait_bounded(sub, 6s) == 0, "subscriber validates sequences across publisher restart");
  check(creator.stats().published == 8 && creator.stats().acknowledged == 8,
        "publisher restart preserves cursors");
}

void cli_rejects_bad_wait_options(const std::string &exe) {
  auto n = name("bad_wait_options");
  Cleanup cleanup{n};
  auto creator = ironbus::Bus::create(n, 4);
  for (const auto &role : {std::string("pub"), std::string("sub")}) {
    for (const auto &[strategy, spins] : std::vector<std::pair<std::string, std::string>>{
             {"unknown", "100"}, {"futex", "10000001"}, {"adaptive", "invalid"}}) {
      auto child = spawn(
          exe, {role, "--name", n, "--count", "1", "--strategy", strategy, "--spin-count", spins});
      check(wait_bounded(child, 2s) == 1, "invalid CLI wait option is rejected promptly");
    }
  }
  check(creator.stats().published == 0, "invalid options do not mutate the shared ring");
}

void publisher_ack_wait_wakeup(const std::string &exe) {
  const char *strategies[] = {"busy", "futex", "adaptive"};
  for (const auto *strategy : strategies) {
    auto n = name((std::string("ack_wake_") + strategy).c_str());
    Cleanup cleanup{n};
    auto pub = ironbus::Bus::create(n, 2);
    pub.set_wait_strategy(strategy == std::string_view("busy") ? ironbus::WaitStrategy::busy_poll
                          : strategy == std::string_view("futex")
                              ? ironbus::WaitStrategy::futex
                              : ironbus::WaitStrategy::adaptive);
    pub.claim(ironbus::Role::publisher);
    auto sub = spawn(exe, {"sub", "--name", n, "--count", "1", "--timeout-ms", "2000", "--strategy",
                           strategy, "--spin-count", "0"});
    const auto register_deadline = std::chrono::steady_clock::now() + 2s;
    while (pub.stats().registered_subscribers == 0 &&
           std::chrono::steady_clock::now() < register_deadline)
      std::this_thread::sleep_for(1ms);
    check(pub.stats().registered_subscribers == 1, "subscriber registered before first publish");
    std::uint64_t payload = 0;
    const auto sequence =
        pub.publish({reinterpret_cast<const std::byte *>(&payload), sizeof(payload)}, 1s);
    pub.wait_for_acknowledgement(sequence, 2s);
    check(wait_bounded(sub, 3s) == 0, "subscriber ACK wakes publisher under selected policy");
  }
}

void wait_strategy_processes(const std::string &exe) {
  const char *strategies[] = {"busy", "futex", "adaptive"};
  std::uint32_t case_number = 0;
  for (const auto *publisher_strategy : strategies) {
    for (const auto *subscriber_strategy : strategies) {
      auto n = name(("wait_matrix_" + std::to_string(case_number++)).c_str());
      Cleanup cleanup{n};
      auto creator = ironbus::Bus::create(n, 8);
      auto sub = spawn(exe, {"sub", "--name", n, "--count", "64", "--timeout-ms", "3000",
                             "--strategy", subscriber_strategy, "--spin-count", "32"});
      std::this_thread::sleep_for(40ms);
      auto pub = spawn(exe, {"pub", "--name", n, "--count", "64", "--timeout-ms", "3000",
                             "--strategy", publisher_strategy, "--spin-count", "32"});
      check(wait_bounded(pub, 5s) == 0, "publisher completes with selected wait strategy");
      check(wait_bounded(sub, 5s) == 0, "subscriber completes with selected wait strategy");
      check(creator.stats().published == 64 && creator.stats().acknowledged == 64,
            "wait policy pair preserves messages through ring wraparound");
    }
  }
}

void multi_subscriber_process_fanout(const std::string &exe) {
  auto n = name("fanout_process");
  Cleanup cleanup{n};
  auto creator = ironbus::Bus::create(n, 16, 4);
  std::vector<pid_t> subscribers;
  for (const auto &id :
       {std::string("strategy-a"), std::string("strategy-b"), std::string("strategy-c")})
    subscribers.push_back(spawn(exe, {"sub", "--name", n, "--id", id, "--count", "64",
                                      "--timeout-ms", "5000", "--strategy", "futex"}));
  const auto admission_deadline = std::chrono::steady_clock::now() + 3s;
  while (creator.stats().registered_subscribers != subscribers.size() &&
         std::chrono::steady_clock::now() < admission_deadline)
    std::this_thread::sleep_for(1ms);
  check(creator.stats().registered_subscribers == subscribers.size(),
        "all independent subscriber identities register before publishing");
  auto publisher = spawn(exe, {"pub", "--name", n, "--count", "64", "--timeout-ms", "5000",
                               "--strategy", "adaptive", "--spin-count", "32"});
  check(wait_bounded(publisher, 8s) == 0, "single publisher broadcasts through full fanout ring");
  for (const auto subscriber : subscribers)
    check(wait_bounded(subscriber, 8s) == 0,
          "each independent subscriber sees ordered and integrity-checked stream");
  const auto stats = creator.stats();
  check(stats.published == 64 && stats.acknowledged == 64 &&
            stats.registered_subscribers == subscribers.size() && stats.membership_sealed,
        "fanout stats report the slowest reader's completed sequence");
}

void slowest_subscriber_controls_backpressure() {
  auto n = name("slow_reader");
  Cleanup cleanup{n};
  auto pub = ironbus::Bus::create(n, 2, 2);
  auto fast = ironbus::Bus::attach(n);
  auto slow = ironbus::Bus::attach(n);
  fast.claim(ironbus::Role::subscriber, "fast");
  slow.claim(ironbus::Role::subscriber, "slow");
  pub.claim(ironbus::Role::publisher);
  for (std::uint64_t value = 0; value < 2; ++value) {
    std::byte payload{static_cast<unsigned char>(value)};
    pub.publish({&payload, 1}, 100ms);
    auto message = fast.receive(100ms);
    check(message.sequence == value + 1 && message.payload[0] == payload,
          "fast subscriber receives correct slot");
    fast.acknowledge(message.sequence);
  }
  std::byte third{2};
  bool stalled = false;
  try {
    pub.publish({&third, 1}, 25ms);
  } catch (const ironbus::Error &e) {
    stalled = e.code() == ironbus::ErrorCode::timeout;
  }
  check(stalled && pub.stats().published == 2,
        "fast ACKs cannot let the publisher overwrite the slow reader's backlog");
  auto first = slow.receive(100ms);
  check(first.sequence == 1 && first.payload[0] == std::byte{0},
        "slow subscriber retains its oldest message during backpressure");
  slow.acknowledge(first.sequence);
  check(pub.publish({&third, 1}, 100ms) == 3,
        "one slot is released after every reader ACKs that sequence");
  auto slow_second = slow.receive(100ms);
  auto fast_third = fast.receive(100ms);
  check(slow_second.sequence == 2 && slow_second.payload[0] == std::byte{1} &&
            fast_third.sequence == 3 && fast_third.payload[0] == third,
        "ring reuse preserves each reader's independent sequence");
  slow.acknowledge(slow_second.sequence);
  fast.acknowledge(fast_third.sequence);
  auto slow_third = slow.receive(100ms);
  check(slow_third.sequence == 3 && slow_third.payload[0] == third,
        "slow reader later receives the same broadcast message");
  slow.acknowledge(slow_third.sequence);
  pub.wait_for_acknowledgement(3, 100ms);
}

void subscriber_restart_late_join_and_forget() {
  auto n = name("membership_lifecycle");
  Cleanup cleanup{n};
  auto pub = ironbus::Bus::create(n, 8, 2);
  auto alpha = ironbus::Bus::attach(n);
  alpha.claim(ironbus::Role::subscriber, "alpha");
  pub.claim(ironbus::Role::publisher);
  std::byte payload{1};
  pub.publish({&payload, 1}, 100ms);
  auto message = alpha.receive(100ms);
  alpha.acknowledge(message.sequence);
  payload = std::byte{2};
  pub.publish({&payload, 1}, 100ms);
  message = alpha.receive(100ms);
  check(message.sequence == 2 && message.payload[0] == payload,
        "existing member receives its next independent sequence");
  alpha = ironbus::Bus{}; // Simulate a crash after receive and before ACK.
  bool active_publisher_blocks_join = false;
  try {
    auto beta = ironbus::Bus::attach(n);
    beta.claim(ironbus::Role::subscriber, "beta");
  } catch (const ironbus::Error &e) {
    active_publisher_blocks_join = e.code() == ironbus::ErrorCode::role_busy;
  }
  check(active_publisher_blocks_join, "late join cannot race a running publisher");
  auto restarted = ironbus::Bus::attach(n);
  restarted.claim(ironbus::Role::subscriber, "alpha");
  message = restarted.receive(100ms);
  check(message.sequence == 2 && message.payload[0] == std::byte{2},
        "same identity resumes and redelivers its unacknowledged message");
  restarted.acknowledge(message.sequence);
  payload = std::byte{3};
  pub.publish({&payload, 1}, 100ms);

  pub = ironbus::Bus{};
  auto admin = ironbus::Bus::attach(n);
  bool active_member_cannot_be_forgotten = false;
  try {
    admin.forget_subscriber("alpha");
  } catch (const ironbus::Error &e) {
    active_member_cannot_be_forgotten = e.code() == ironbus::ErrorCode::role_busy;
  }
  check(active_member_cannot_be_forgotten, "live member cannot be removed");
  auto beta = ironbus::Bus::attach(n);
  beta.claim(ironbus::Role::subscriber, "beta");
  bool table_full = false;
  try {
    auto gamma = ironbus::Bus::attach(n);
    gamma.claim(ironbus::Role::subscriber, "gamma");
  } catch (const ironbus::Error &e) {
    table_full = e.code() == ironbus::ErrorCode::subscriber_limit;
  }
  check(table_full, "bounded subscriber table rejects excess identities");
  pub = ironbus::Bus::attach(n);
  pub.claim(ironbus::Role::publisher);
  payload = std::byte{4};
  check(pub.publish({&payload, 1}, 100ms) == 4, "publisher resumes after quiescent late join");
  auto alpha_third = restarted.receive(100ms);
  auto beta_fourth = beta.receive(100ms);
  check(alpha_third.sequence == 3 && beta_fourth.sequence == 4 && beta_fourth.payload[0] == payload,
        "late identity begins at the admission tail while old identity resumes its cursor");
  restarted.acknowledge(alpha_third.sequence);
  auto alpha_fourth = restarted.receive(100ms);
  check(alpha_fourth.sequence == 4 && alpha_fourth.payload[0] == payload,
        "existing member continues receiving broadcasts after late admission");
  restarted.acknowledge(alpha_fourth.sequence);
  beta.acknowledge(beta_fourth.sequence);
  pub.wait_for_acknowledgement(4, 100ms);
  bool live_publisher_prevents_forget = false;
  try {
    admin.forget_subscriber("beta");
  } catch (const ironbus::Error &e) {
    live_publisher_prevents_forget = e.code() == ironbus::ErrorCode::role_busy;
  }
  check(live_publisher_prevents_forget, "publisher must stop before subscriber removal");
  pub = ironbus::Bus{};
  bool live_subscriber_prevents_forget = false;
  try {
    admin.forget_subscriber("beta");
  } catch (const ironbus::Error &e) {
    live_subscriber_prevents_forget = e.code() == ironbus::ErrorCode::role_busy;
  }
  check(live_subscriber_prevents_forget, "subscriber role lock prevents unsafe reclamation");
  beta = ironbus::Bus{};
  admin.forget_subscriber("beta");
  check(admin.stats().registered_subscribers == 1, "explicit removal frees one registry slot");
  restarted = ironbus::Bus{};
  admin.forget_subscriber("alpha");
  auto empty_roster = admin.stats();
  check(empty_roster.registered_subscribers == 0 && empty_roster.acknowledged == 4,
        "forgetting all readers releases retention only by explicit action");
  pub = ironbus::Bus::attach(n);
  pub.claim(ironbus::Role::publisher);
  bool no_members = false;
  try {
    payload = std::byte{5};
    pub.publish({&payload, 1}, 100ms);
  } catch (const ironbus::Error &e) {
    no_members = e.code() == ironbus::ErrorCode::no_subscribers;
  }
  check(no_members && pub.stats().published == 4,
        "publisher refuses to advance after explicit removal of every reader");
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
    auto sub = ironbus::Bus::attach(n);
    sub.set_wait_strategy(strategy, 16);
    sub.claim(ironbus::Role::subscriber);
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
    bool acknowledgement_timeout = false;
    try {
      pub.wait_for_acknowledgement(1, 25ms);
    } catch (const ironbus::Error &e) {
      acknowledgement_timeout = e.code() == ironbus::ErrorCode::timeout;
    }
    check(acknowledgement_timeout, "ACK wait timeout is bounded for each policy");

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

void publisher_requires_initial_subscriber(const std::string &exe) {
  const char *strategies[] = {"busy", "futex", "adaptive"};
  for (const auto *strategy : strategies) {
    auto n = name((std::string("publisher_first_") + strategy).c_str());
    Cleanup cleanup{n};
    auto creator = ironbus::Bus::create(n, 4);
    auto premature_pub = spawn(
        exe, {"pub", "--name", n, "--count", "1", "--timeout-ms", "1000", "--strategy", strategy});
    check(wait_bounded(premature_pub, 2s) == 1,
          "publisher cannot silently publish before any subscriber registers");
    check(creator.stats().published == 0 && !creator.stats().membership_sealed,
          "rejected first publish leaves membership open and sequence unchanged");
    auto sub = spawn(exe, {"sub", "--name", n, "--count", "1", "--timeout-ms", "2000", "--strategy",
                           strategy, "--spin-count", "8"});
    std::this_thread::sleep_for(40ms);
    auto pub = spawn(exe, {"pub", "--name", n, "--count", "1", "--timeout-ms", "2000", "--strategy",
                           strategy, "--spin-count", "8"});
    check(wait_bounded(pub, 4s) == 0, "publisher succeeds after subscriber admission");
    check(wait_bounded(sub, 4s) == 0, "subscriber receives first message after roster seal");
    check(creator.stats().published == 1 && creator.stats().acknowledged == 1,
          "initial subscriber receives the first published message");
  }
}

void crash_redelivery(const std::string &exe) {
  auto n = name("crash");
  Cleanup cleanup{n};
  auto p = ironbus::Bus::create(n, 4);
  p.claim(ironbus::Role::publisher);
  std::uint64_t payload = 0;
  (void)exe;
  int ready_pipe[2]{};
  check(pipe(ready_pipe) == 0, "create subscriber readiness pipe");
  auto dying = fork();
  if (dying == 0) {
    close(ready_pipe[0]);
    try {
      auto consumer = ironbus::Bus::attach(n);
      consumer.set_wait_strategy(ironbus::WaitStrategy::futex);
      consumer.claim(ironbus::Role::subscriber);
      const char ready = 'R';
      if (write(ready_pipe[1], &ready, sizeof(ready)) != static_cast<ssize_t>(sizeof(ready)))
        _exit(2);
      (void)consumer.receive(2s);
      kill(getpid(), SIGKILL); // Crash after copy and before ACK.
      _exit(2);
    } catch (...) {
      _exit(2);
    }
  }
  close(ready_pipe[1]);
  check(dying > 0, "fork crash-test subscriber");
  char ready{};
  check(read(ready_pipe[0], &ready, sizeof(ready)) == static_cast<ssize_t>(sizeof(ready)) &&
            ready == 'R',
        "subscriber registers before first publication");
  close(ready_pipe[0]);
  p.publish({reinterpret_cast<const std::byte *>(&payload), sizeof(payload)}, 100ms);
  check(wait_bounded(dying, 4s) == 128, "subscriber crashes after receive and before ACK");
  auto recovered = ironbus::Bus::attach(n);
  recovered.set_wait_strategy(ironbus::WaitStrategy::futex);
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
    cli_rejects_bad_wait_options(argv[1]);
    publisher_ack_wait_wakeup(argv[1]);
    wait_strategy_processes(argv[1]);
    multi_subscriber_process_fanout(argv[1]);
    slowest_subscriber_controls_backpressure();
    subscriber_restart_late_join_and_forget();
    wait_strategy_timeouts();
    publisher_requires_initial_subscriber(argv[1]);
    crash_redelivery(argv[1]);
    reject_incompatible();
    std::cout << "all IRONBUS tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << "test failure: " << e.what() << '\n';
    return 1;
  }
}
