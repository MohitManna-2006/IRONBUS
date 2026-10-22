#include "ironbus/bus.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>

namespace {
using namespace std::chrono_literals;
std::uint64_t number(std::string_view s, std::string_view label) {
  std::uint64_t n{};
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (ec != std::errc{} || end != s.data() + s.size())
    throw ironbus::Error(ironbus::ErrorCode::invalid_argument, "invalid " + std::string(label));
  return n;
}
std::string_view optional_option(int argc, char **argv, std::string_view key) {
  for (int i = 2; i + 1 < argc; ++i)
    if (argv[i] == key)
      return argv[i + 1];
  return {};
}
std::string_view option(int argc, char **argv, std::string_view key) {
  auto value = optional_option(argc, argv, key);
  if (value.empty())
    throw ironbus::Error(ironbus::ErrorCode::invalid_argument,
                         "missing option " + std::string(key));
  return value;
}
void usage() {
  std::cerr
      << "Usage:\n  ironbus create --name NAME --capacity POWER_OF_TWO [--max-subscribers N]\n"
         "  ironbus pub --name NAME --count N [--timeout-ms N] [--strategy "
         "busy|futex|adaptive] [--spin-count N]\n"
         "  ironbus sub --name NAME --id STABLE_ID --count N [--timeout-ms N] [--ack-delay-ms N] "
         "[--strategy busy|futex|adaptive] [--spin-count N]\n"
         "  ironbus stats --name NAME\n  ironbus forget --name NAME --id STABLE_ID\n"
         "  ironbus unlink --name NAME\n";
}
ironbus::WaitStrategy wait_strategy(int argc, char **argv) {
  const auto value = optional_option(argc, argv, "--strategy");
  if (value.empty() || value == "busy")
    return ironbus::WaitStrategy::busy_poll;
  if (value == "futex")
    return ironbus::WaitStrategy::futex;
  if (value == "adaptive")
    return ironbus::WaitStrategy::adaptive;
  throw ironbus::Error(ironbus::ErrorCode::invalid_argument,
                       "strategy must be busy, futex, or adaptive");
}
std::uint32_t spin_count(int argc, char **argv) {
  const auto value = optional_option(argc, argv, "--spin-count");
  if (value.empty())
    return 1000;
  const auto parsed = number(value, "spin count");
  if (parsed > std::numeric_limits<std::uint32_t>::max())
    throw ironbus::Error(ironbus::ErrorCode::invalid_argument, "spin count exceeds uint32 range");
  return static_cast<std::uint32_t>(parsed);
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc < 2) {
      usage();
      return 2;
    }
    const std::string_view cmd(argv[1]);
    if (cmd == "create") {
      const auto requested_capacity = number(option(argc, argv, "--capacity"), "capacity");
      if (requested_capacity > std::numeric_limits<std::uint32_t>::max())
        throw ironbus::Error(ironbus::ErrorCode::invalid_argument, "capacity exceeds uint32 range");
      std::uint64_t requested_subscribers = 16;
      if (auto value = optional_option(argc, argv, "--max-subscribers"); !value.empty())
        requested_subscribers = number(value, "max subscribers");
      if (requested_subscribers > std::numeric_limits<std::uint32_t>::max())
        throw ironbus::Error(ironbus::ErrorCode::invalid_argument,
                             "max subscribers exceeds uint32 range");
      auto bus = ironbus::Bus::create(option(argc, argv, "--name"),
                                      static_cast<std::uint32_t>(requested_capacity),
                                      static_cast<std::uint32_t>(requested_subscribers));
      std::cout << "created " << ironbus::shm_name(option(argc, argv, "--name"))
                << " capacity=" << bus.capacity() << " max_subscribers=" << requested_subscribers
                << '\n';
    } else if (cmd == "unlink") {
      ironbus::Bus::unlink(option(argc, argv, "--name"));
      std::cout << "unlinked\n";
    } else if (cmd == "stats") {
      auto bus = ironbus::Bus::attach(option(argc, argv, "--name"));
      auto s = bus.stats();
      std::cout << "published=" << s.published << " acknowledged=" << s.acknowledged
                << " outstanding=" << s.published - s.acknowledged << " capacity=" << s.capacity
                << " slot_size=" << s.slot_size
                << " registered_subscribers=" << s.registered_subscribers
                << " membership_sealed=" << (s.membership_sealed ? "true" : "false") << '\n';
    } else if (cmd == "forget") {
      auto bus = ironbus::Bus::attach(option(argc, argv, "--name"));
      bus.forget_subscriber(option(argc, argv, "--id"));
      std::cout << "forgotten subscriber\n";
    } else if (cmd == "pub") {
      auto bus = ironbus::Bus::attach(option(argc, argv, "--name"));
      bus.set_wait_strategy(wait_strategy(argc, argv), spin_count(argc, argv));
      bus.claim(ironbus::Role::publisher);
      auto count = number(option(argc, argv, "--count"), "count");
      std::uint64_t timeout = 30000;
      if (auto value = optional_option(argc, argv, "--timeout-ms"); !value.empty())
        timeout = number(value, "timeout");
      const auto first_value = bus.stats().published;
      for (std::uint64_t i = 0; i < count; ++i) {
        const auto value = first_value + i;
        std::array<std::byte, sizeof(value)> payload{};
        std::memcpy(payload.data(), &value, sizeof(value));
        bus.publish(payload, std::chrono::milliseconds(timeout));
      }
      std::cout << "published=" << count << '\n';
    } else if (cmd == "sub") {
      auto bus = ironbus::Bus::attach(option(argc, argv, "--name"));
      bus.set_wait_strategy(wait_strategy(argc, argv), spin_count(argc, argv));
      const auto subscriber_id = optional_option(argc, argv, "--id");
      bus.claim(ironbus::Role::subscriber, subscriber_id.empty() ? "default" : subscriber_id);
      auto count = number(option(argc, argv, "--count"), "count");
      std::uint64_t timeout = 30000, ack_delay = 0;
      if (auto value = optional_option(argc, argv, "--timeout-ms"); !value.empty())
        timeout = number(value, "timeout");
      if (auto value = optional_option(argc, argv, "--ack-delay-ms"); !value.empty())
        ack_delay = number(value, "ack delay");
      for (std::uint64_t i = 0; i < count; ++i) {
        auto message = bus.receive(std::chrono::milliseconds(timeout));
        if (message.size != sizeof(std::uint64_t))
          throw ironbus::Error(ironbus::ErrorCode::corrupt_message, "unexpected CLI message size");
        std::uint64_t value{};
        std::memcpy(&value, message.payload, sizeof(value));
        if (value != message.sequence - 1)
          throw ironbus::Error(ironbus::ErrorCode::corrupt_message,
                               "CLI payload/sequence mismatch");
        if (ack_delay)
          std::this_thread::sleep_for(std::chrono::milliseconds(ack_delay));
        bus.acknowledge(message.sequence);
      }
      std::cout << "acknowledged=" << count << '\n';
    } else {
      usage();
      return 2;
    }
  } catch (const ironbus::Error &e) {
    std::cerr << "ironbus error [" << static_cast<int>(e.code()) << "]: " << e.what() << '\n';
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "ironbus error: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
