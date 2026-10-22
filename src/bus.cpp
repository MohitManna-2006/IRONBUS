#include "ironbus/bus.hpp"

#include "wait_linux.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ironbus {
namespace {
constexpr std::uint64_t kMagic = 0x49524f4e42555331ULL; // IRONBUS1
constexpr std::uint32_t kVersion = 3;
constexpr std::uint32_t kInitializing = 1;
constexpr std::uint32_t kReady = 2;
constexpr std::uint32_t kMembershipOpen = 0;
constexpr std::uint32_t kMembershipSealed = 1;
constexpr std::uint32_t kMemberFree = 0;
constexpr std::uint32_t kMemberInitializing = 1;
constexpr std::uint32_t kMemberRegistered = 2;
constexpr std::uint32_t kMaxCapacity = 1U << 20;
constexpr std::uint32_t kMaxSubscribers = 256;
constexpr std::size_t kCacheLine = 64;
constexpr off_t kPublisherLockOffset = 0;
constexpr off_t kRegistryLockOffset = 1;
constexpr off_t kSubscriberLockBase = 16;

struct alignas(kCacheLine) Cursor {
  std::atomic<std::uint64_t> value{0};
  std::byte padding[kCacheLine - sizeof(std::atomic<std::uint64_t>)]{};
};
static_assert(sizeof(Cursor) == kCacheLine);

struct alignas(kCacheLine) Generation {
  std::atomic<std::uint32_t> value{0};
  std::byte padding[kCacheLine - sizeof(std::atomic<std::uint32_t>)]{};
};
static_assert(sizeof(Generation) == kCacheLine);

struct alignas(kCacheLine) SubscriberMetadata {
  std::atomic<std::uint32_t> state{kMemberFree};
  std::uint32_t id_size{};
  char id[kSubscriberIdBytes + 1]{};
  std::byte padding[kCacheLine * 2 - sizeof(std::atomic<std::uint32_t>) - sizeof(std::uint32_t) -
                    kSubscriberIdBytes - 1]{};
};
static_assert(sizeof(SubscriberMetadata) == kCacheLine * 2);

struct SubscriberEntry {
  SubscriberMetadata metadata;
  Cursor acknowledged;
};
static_assert(sizeof(SubscriberEntry) == kCacheLine * 3);

struct alignas(kCacheLine) Header {
  std::uint64_t magic{};
  std::uint32_t version{};
  std::uint32_t header_size{};
  std::uint32_t capacity{};
  std::uint32_t slot_size{};
  std::uint32_t max_subscribers{};
  std::uint32_t reserved{};
  std::uint64_t total_size{};
  std::uint64_t subscribers_offset{};
  std::uint64_t slots_offset{};
  std::atomic<std::uint32_t> state{0};
  std::atomic<std::uint32_t> membership_state{kMembershipOpen};
  std::atomic<std::uint32_t> registered_subscribers{0};
  Cursor published;
  Cursor retention_watermark;
  Generation data_generation;
  Generation space_generation;
};
static_assert(sizeof(Header) % kCacheLine == 0);

struct Slot {
  std::uint64_t sequence;
  std::uint32_t size;
  std::uint32_t reserved;
  std::byte payload[kPayloadBytes];
};
static_assert(sizeof(Slot) == kSlotBytes);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "uint64 atomics must be always lock-free");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "uint32 atomics must be always lock-free");

std::string os_error(std::string_view action) {
  return std::string(action) + ": " + std::strerror(errno);
}

std::string validate_name(std::string_view name) {
  if (name.empty() || name.size() > 180) {
    throw Error(ErrorCode::invalid_argument, "name must contain 1 to 180 characters");
  }
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '-')) {
      throw Error(ErrorCode::invalid_argument,
                  "name may contain only letters, digits, underscore, and hyphen");
    }
  }
  return "/ironbus_" + std::string(name);
}

std::size_t mapping_size(std::uint32_t capacity, std::uint32_t max_subscribers) {
  if (capacity < 2 || capacity > kMaxCapacity || (capacity & (capacity - 1)) != 0) {
    throw Error(ErrorCode::invalid_argument,
                "capacity must be a power of two between 2 and 1048576");
  }
  if (max_subscribers == 0 || max_subscribers > kMaxSubscribers)
    throw Error(ErrorCode::invalid_argument,
                "max_subscribers must be between 1 and 256");
  return sizeof(Header) + static_cast<std::size_t>(max_subscribers) * sizeof(SubscriberEntry) +
         static_cast<std::size_t>(capacity) * sizeof(Slot);
}

std::uint64_t minimum_acknowledged(Header *header, SubscriberEntry *subscribers) {
  bool found = false;
  auto minimum = header->published.value.load(std::memory_order_acquire);
  for (std::uint32_t i = 0; i < header->max_subscribers; ++i) {
    auto &entry = subscribers[i];
    if (entry.metadata.state.load(std::memory_order_acquire) != kMemberRegistered)
      continue;
    const auto acknowledged = entry.acknowledged.value.load(std::memory_order_acquire);
    if (!found || acknowledged < minimum)
      minimum = acknowledged;
    found = true;
  }
  return minimum;
}

std::uint32_t registered_member_count(Header *header, SubscriberEntry *subscribers) {
  std::uint32_t count = 0;
  for (std::uint32_t i = 0; i < header->max_subscribers; ++i)
    if (subscribers[i].metadata.state.load(std::memory_order_acquire) == kMemberRegistered)
      ++count;
  return count;
}

void reconcile_member_count(Header *header, SubscriberEntry *subscribers) {
  header->registered_subscribers.store(registered_member_count(header, subscribers),
                                       std::memory_order_release);
}

std::string_view validate_subscriber_id(std::string_view id) {
  if (id.empty() || id.size() > kSubscriberIdBytes || id.find('\0') != std::string_view::npos)
    throw Error(ErrorCode::invalid_argument, "subscriber ID must contain 1 to 63 non-NUL bytes");
  return id;
}

bool matches_id(const SubscriberMetadata &metadata, std::string_view id) {
  return metadata.state.load(std::memory_order_acquire) == kMemberRegistered &&
         metadata.id_size == id.size() && std::memcmp(metadata.id, id.data(), id.size()) == 0;
}

void clear_incomplete_member(SubscriberEntry &entry) noexcept {
  if (entry.metadata.state.load(std::memory_order_acquire) == kMemberInitializing) {
    entry.metadata.id_size = 0;
    entry.acknowledged.value.store(0, std::memory_order_relaxed);
    entry.metadata.state.store(kMemberFree, std::memory_order_release);
  }
}

void lock_role(int fd, off_t offset, bool wait, std::string_view busy_message) {
#ifndef F_OFD_SETLK
#error "Linux OFD locks are required"
#endif
  struct flock lock {};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = offset;
  lock.l_len = 1;
  const auto command = wait ? F_OFD_SETLKW : F_OFD_SETLK;
  while (fcntl(fd, command, &lock) != 0) {
    if (wait && errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EACCES) {
      throw Error(ErrorCode::role_busy, std::string(busy_message));
    }
    throw Error(ErrorCode::system, os_error("acquire role lock"));
  }
}

void unlock_role(int fd, off_t offset) noexcept {
  struct flock lock {};
  lock.l_type = F_UNLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = offset;
  lock.l_len = 1;
  (void)fcntl(fd, F_OFD_SETLK, &lock);
}

class ScopedFileLock {
public:
  ScopedFileLock(int fd, off_t offset, std::string_view busy_message)
      : fd_(fd), offset_(offset) {
    lock_role(fd_, offset_, true, busy_message);
  }
  ~ScopedFileLock() {
    if (fd_ >= 0)
      unlock_role(fd_, offset_);
  }
  ScopedFileLock(const ScopedFileLock &) = delete;
  ScopedFileLock &operator=(const ScopedFileLock &) = delete;

private:
  int fd_;
  off_t offset_;
};

template <typename Predicate>
void wait_for_predicate(WaitStrategy strategy, std::uint32_t spin_budget,
                        std::atomic<std::uint32_t> &generation,
                        std::chrono::steady_clock::time_point deadline, Predicate &&ready,
                        std::string_view timeout_message) {
  auto spins_remaining = spin_budget;
  while (!ready()) {
    if (std::chrono::steady_clock::now() >= deadline)
      throw Error(ErrorCode::timeout, std::string(timeout_message));
    if (strategy == WaitStrategy::busy_poll ||
        (strategy == WaitStrategy::adaptive && spins_remaining != 0)) {
      if (strategy == WaitStrategy::adaptive)
        --spins_remaining;
      detail::cpu_relax();
      continue;
    }
    const auto observed = generation.load(std::memory_order_acquire);
    if (ready())
      return;
    detail::futex_wait(generation, observed, deadline);
  }
}
} // namespace

Error::Error(ErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

std::string shm_name(std::string_view name) {
  return validate_name(name);
}

struct Bus::Impl {
  int fd{-1};
  void *mapping{MAP_FAILED};
  std::size_t bytes{};
  Header *header{};
  bool role_claimed{};
  bool cleanup_created{};
  std::string created_name;
  Role role{Role::publisher};
  std::uint32_t member_slot{};
  SubscriberEntry *member{};
  std::string subscriber_id;
  std::uint64_t pending{};
  bool delivered{};
  WaitStrategy wait_strategy{WaitStrategy::busy_poll};
  std::uint32_t spin_budget{1000};

  ~Impl() {
    if (mapping != MAP_FAILED)
      munmap(mapping, bytes);
    if (fd >= 0)
      close(fd);
    if (cleanup_created)
      shm_unlink(created_name.c_str());
  }
  Slot *slots() const {
    return reinterpret_cast<Slot *>(static_cast<std::byte *>(mapping) + header->slots_offset);
  }
  SubscriberEntry *subscribers() const {
    return reinterpret_cast<SubscriberEntry *>(static_cast<std::byte *>(mapping) +
                                                header->subscribers_offset);
  }
};

Bus::Bus() noexcept = default;
Bus::Bus(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Bus::~Bus() = default;
Bus::Bus(Bus &&) noexcept = default;
Bus &Bus::operator=(Bus &&) noexcept = default;

Bus Bus::create(std::string_view name, std::uint32_t capacity, std::uint32_t max_subscribers) {
  auto bytes = mapping_size(capacity, max_subscribers);
  auto impl = std::make_unique<Impl>();
  auto path = validate_name(name);
  impl->fd = shm_open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (impl->fd < 0)
    throw Error(ErrorCode::system, os_error("create shared memory (already exists?)"));
  impl->cleanup_created = true;
  impl->created_name = path;
  if (ftruncate(impl->fd, static_cast<off_t>(bytes)) != 0)
    throw Error(ErrorCode::system, os_error("size shared memory"));
  impl->mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, impl->fd, 0);
  if (impl->mapping == MAP_FAILED)
    throw Error(ErrorCode::system, os_error("map shared memory"));
  impl->bytes = bytes;
  impl->header = new (impl->mapping) Header;
  auto *h = impl->header;
  h->state.store(kInitializing, std::memory_order_relaxed);
  h->magic = kMagic;
  h->version = kVersion;
  h->header_size = sizeof(Header);
  h->capacity = capacity;
  h->slot_size = sizeof(Slot);
  h->max_subscribers = max_subscribers;
  h->total_size = bytes;
  h->subscribers_offset = sizeof(Header);
  h->slots_offset = sizeof(Header) +
                    static_cast<std::uint64_t>(max_subscribers) * sizeof(SubscriberEntry);
  h->published.value.store(0, std::memory_order_relaxed);
  h->retention_watermark.value.store(0, std::memory_order_relaxed);
  h->data_generation.value.store(0, std::memory_order_relaxed);
  h->space_generation.value.store(0, std::memory_order_relaxed);
  auto *entries = reinterpret_cast<SubscriberEntry *>(static_cast<std::byte *>(impl->mapping) +
                                                       h->subscribers_offset);
  for (std::uint32_t i = 0; i < max_subscribers; ++i)
    new (&entries[i]) SubscriberEntry;
  h->state.store(kReady, std::memory_order_release);
  impl->cleanup_created = false;
  return Bus(std::move(impl));
}

Bus Bus::attach(std::string_view name) {
  auto impl = std::make_unique<Impl>();
  auto path = validate_name(name);
  impl->fd = shm_open(path.c_str(), O_RDWR | O_CLOEXEC, 0);
  if (impl->fd < 0)
    throw Error(ErrorCode::system, os_error("open shared memory"));
  struct stat st {};
  if (fstat(impl->fd, &st) != 0)
    throw Error(ErrorCode::system, os_error("stat shared memory"));
  if (st.st_size < static_cast<off_t>(sizeof(Header)))
    throw Error(ErrorCode::unsafe_initialization,
                "shared memory is truncated or still initializing");
  impl->bytes = static_cast<std::size_t>(st.st_size);
  impl->mapping = mmap(nullptr, impl->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, impl->fd, 0);
  if (impl->mapping == MAP_FAILED)
    throw Error(ErrorCode::system, os_error("map shared memory"));
  impl->header = static_cast<Header *>(impl->mapping);
  auto *h = impl->header;
  if (!h->state.is_lock_free() || !h->membership_state.is_lock_free() ||
      !h->published.value.is_lock_free() || !h->retention_watermark.value.is_lock_free() ||
      !h->data_generation.value.is_lock_free() || !h->space_generation.value.is_lock_free())
    throw Error(ErrorCode::incompatible_layout, "required shared atomics are not lock-free");
  if (h->state.load(std::memory_order_acquire) != kReady)
    throw Error(ErrorCode::unsafe_initialization,
                "shared memory is not fully initialized; unlink it before retrying");
  if (h->magic != kMagic || h->version != kVersion || h->header_size != sizeof(Header) ||
      h->slot_size != sizeof(Slot) || h->total_size != impl->bytes || h->capacity < 2 ||
      h->capacity > kMaxCapacity || (h->capacity & (h->capacity - 1)) != 0 ||
      h->max_subscribers == 0 || h->max_subscribers > kMaxSubscribers ||
      h->subscribers_offset != sizeof(Header) ||
      h->slots_offset != sizeof(Header) +
                              static_cast<std::uint64_t>(h->max_subscribers) *
                                  sizeof(SubscriberEntry) ||
      h->slots_offset + static_cast<std::uint64_t>(h->capacity) * sizeof(Slot) != impl->bytes ||
      h->membership_state.load(std::memory_order_acquire) > kMembershipSealed) {
    throw Error(ErrorCode::incompatible_layout, "shared-memory layout is incompatible or corrupt");
  }
  auto *entries = impl->subscribers();
  const auto published = h->published.value.load(std::memory_order_acquire);
  for (std::uint32_t i = 0; i < h->max_subscribers; ++i) {
    const auto member_state = entries[i].metadata.state.load(std::memory_order_acquire);
    if (!entries[i].metadata.state.is_lock_free() || !entries[i].acknowledged.value.is_lock_free() ||
        member_state > kMemberRegistered || entries[i].metadata.id_size > kSubscriberIdBytes ||
        (member_state == kMemberRegistered &&
         (entries[i].metadata.id_size == 0 ||
          entries[i].acknowledged.value.load(std::memory_order_acquire) > published)))
      throw Error(ErrorCode::incompatible_layout,
                  "subscriber table contains an incompatible or corrupt entry");
  }
  return Bus(std::move(impl));
}

void Bus::unlink(std::string_view name) {
  auto path = validate_name(name);
  if (shm_unlink(path.c_str()) != 0)
    throw Error(ErrorCode::system, os_error("unlink shared memory"));
}

void Bus::claim(Role role, std::string_view subscriber_id) {
  if (!impl_)
    throw Error(ErrorCode::invalid_argument, "bus is not open");
  if (impl_->role_claimed)
    throw Error(ErrorCode::invalid_argument, "a role is already claimed by this handle");
  if (role == Role::publisher) {
    lock_role(impl_->fd, kPublisherLockOffset, false, "publisher role is already held");
    impl_->role_claimed = true;
    impl_->role = role;
    return;
  }
  if (role != Role::subscriber)
    throw Error(ErrorCode::invalid_argument, "unknown process role");
  validate_subscriber_id(subscriber_id);
  impl_->subscriber_id = std::string(subscriber_id);

  auto *entries = impl_->subscribers();
  auto lock_member = [&](std::uint32_t index) {
    lock_role(impl_->fd, kSubscriberLockBase + static_cast<off_t>(index), false,
              "subscriber identity is already active");
  };
  auto bind_existing = [&]() -> bool {
    for (std::uint32_t i = 0; i < impl_->header->max_subscribers; ++i) {
      if (!matches_id(entries[i].metadata, subscriber_id))
        continue;
      lock_member(i);
      if (!matches_id(entries[i].metadata, subscriber_id)) {
        unlock_role(impl_->fd, kSubscriberLockBase + static_cast<off_t>(i));
        continue;
      }
      impl_->member_slot = i;
      impl_->member = &entries[i];
      const auto acknowledged = impl_->member->acknowledged.value.load(std::memory_order_acquire);
      impl_->pending = acknowledged == std::numeric_limits<std::uint64_t>::max()
                           ? 0
                           : acknowledged + 1;
      impl_->role_claimed = true;
      impl_->role = Role::subscriber;
      return true;
    }
    return false;
  };
  auto register_new = [&](std::uint64_t start_sequence) -> bool {
    for (std::uint32_t i = 0; i < impl_->header->max_subscribers; ++i) {
      auto &entry = entries[i];
      clear_incomplete_member(entry);
      if (entry.metadata.state.load(std::memory_order_acquire) != kMemberFree)
        continue;
      lock_member(i);
      if (entry.metadata.state.load(std::memory_order_acquire) != kMemberFree) {
        unlock_role(impl_->fd, kSubscriberLockBase + static_cast<off_t>(i));
        continue;
      }
      entry.metadata.state.store(kMemberInitializing, std::memory_order_relaxed);
      entry.metadata.id_size = static_cast<std::uint32_t>(subscriber_id.size());
      std::memset(entry.metadata.id, 0, sizeof(entry.metadata.id));
      std::memcpy(entry.metadata.id, subscriber_id.data(), subscriber_id.size());
      entry.acknowledged.value.store(start_sequence, std::memory_order_relaxed);
      entry.metadata.state.store(kMemberRegistered, std::memory_order_release);
      impl_->member_slot = i;
      impl_->member = &entry;
      impl_->pending = start_sequence == std::numeric_limits<std::uint64_t>::max()
                           ? 0
                           : start_sequence + 1;
      impl_->role_claimed = true;
      impl_->role = Role::subscriber;
      return true;
    }
    return false;
  };

  {
    ScopedFileLock registry(impl_->fd, kRegistryLockOffset,
                            "unable to serialize subscriber registration");
    if (bind_existing())
      return;
    if (impl_->header->membership_state.load(std::memory_order_acquire) == kMembershipOpen) {
      if (!register_new(0))
        throw Error(ErrorCode::subscriber_limit, "subscriber table is full");
      return;
    }
  }

  // A new identity joining a sealed roster starts at the current tail. Holding
  // the publisher lock makes that tail snapshot atomic with respect to publish.
  ScopedFileLock publisher(impl_->fd, kPublisherLockOffset,
                           "new subscribers may join only while publisher is stopped");
  ScopedFileLock registry(impl_->fd, kRegistryLockOffset,
                          "unable to serialize subscriber registration");
  if (bind_existing())
    return;
  const auto tail = impl_->header->published.value.load(std::memory_order_acquire);
  if (!register_new(tail))
    throw Error(ErrorCode::subscriber_limit, "subscriber table is full");
  impl_->header->space_generation.value.fetch_add(1, std::memory_order_release);
  detail::futex_wake_one(impl_->header->space_generation.value);
}

void Bus::forget_subscriber(std::string_view subscriber_id) {
  if (!impl_)
    throw Error(ErrorCode::invalid_argument, "bus is not open");
  if (impl_->role_claimed)
    throw Error(ErrorCode::invalid_argument,
                "forget_subscriber requires a handle without a claimed role");
  validate_subscriber_id(subscriber_id);
  ScopedFileLock publisher(impl_->fd, kPublisherLockOffset,
                           "stop the publisher before forgetting a subscriber");
  ScopedFileLock registry(impl_->fd, kRegistryLockOffset,
                          "unable to serialize subscriber removal");
  auto *entries = impl_->subscribers();
  for (std::uint32_t i = 0; i < impl_->header->max_subscribers; ++i) {
    auto &entry = entries[i];
    clear_incomplete_member(entry);
    if (!matches_id(entry.metadata, subscriber_id))
      continue;
    ScopedFileLock member(impl_->fd, kSubscriberLockBase + static_cast<off_t>(i),
                          "stop the subscriber before forgetting its identity");
    entry.metadata.state.store(kMemberFree, std::memory_order_release);
    entry.metadata.id_size = 0;
    std::memset(entry.metadata.id, 0, sizeof(entry.metadata.id));
    const auto minimum = minimum_acknowledged(impl_->header, entries);
    impl_->header->retention_watermark.value.store(minimum, std::memory_order_release);
    return;
  }
  throw Error(ErrorCode::invalid_argument, "subscriber identity is not registered");
}

void Bus::set_wait_strategy(WaitStrategy strategy, std::uint32_t spin_budget) {
  if (!impl_)
    throw Error(ErrorCode::invalid_argument, "bus is not open");
  if (impl_->role_claimed)
    throw Error(ErrorCode::invalid_argument, "wait strategy must be set before claiming a role");
  if (strategy != WaitStrategy::busy_poll && strategy != WaitStrategy::futex &&
      strategy != WaitStrategy::adaptive)
    throw Error(ErrorCode::invalid_argument, "unknown wait strategy");
  if (spin_budget > 10'000'000)
    throw Error(ErrorCode::invalid_argument, "spin budget exceeds 10000000 checks");
  impl_->wait_strategy = strategy;
  impl_->spin_budget = spin_budget;
}

WaitStrategy Bus::wait_strategy() const {
  if (!impl_)
    throw Error(ErrorCode::invalid_argument, "bus is not open");
  return impl_->wait_strategy;
}

Stats Bus::stats() const {
  if (!impl_)
    throw Error(ErrorCode::invalid_argument, "bus is not open");
  auto *h = impl_->header;
  auto *entries = impl_->subscribers();
  return {h->published.value.load(std::memory_order_acquire),
          minimum_acknowledged(h, entries),
          h->capacity,
          h->slot_size,
          registered_member_count(h, entries),
          h->membership_state.load(std::memory_order_acquire) == kMembershipSealed};
}
std::uint32_t Bus::capacity() const {
  return stats().capacity;
}

std::uint64_t Bus::publish(std::span<const std::byte> payload, std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->role_claimed || impl_->role != Role::publisher)
    throw Error(ErrorCode::invalid_argument, "publisher role must be claimed before publishing");
  if (payload.size() > kPayloadBytes)
    throw Error(ErrorCode::invalid_argument, "payload exceeds 240 bytes");
  auto deadline = std::chrono::steady_clock::now() + timeout;
  auto *h = impl_->header;
  if (h->membership_state.load(std::memory_order_acquire) == kMembershipOpen) {
    ScopedFileLock registry(impl_->fd, kRegistryLockOffset,
                            "unable to seal subscriber membership before publish");
    if (h->membership_state.load(std::memory_order_acquire) == kMembershipOpen) {
      auto *entries = impl_->subscribers();
      if (registered_member_count(h, entries) == 0)
        throw Error(ErrorCode::no_subscribers,
                    "register at least one subscriber before publishing the first message");
      const auto minimum = minimum_acknowledged(h, entries);
      h->retention_watermark.value.store(minimum, std::memory_order_relaxed);
      h->membership_state.store(kMembershipSealed, std::memory_order_release);
    }
  }
  auto published = h->published.value.load(std::memory_order_relaxed);
  if (published == std::numeric_limits<std::uint64_t>::max())
    throw Error(ErrorCode::system, "sequence space exhausted");
  const auto next = published + 1;
  auto retained = h->retention_watermark.value.load(std::memory_order_acquire);
  while (next - retained > h->capacity) {
    const auto observed_minimum = minimum_acknowledged(h, impl_->subscribers());
    if (observed_minimum > retained) {
      retained = observed_minimum;
      h->retention_watermark.value.store(retained, std::memory_order_release);
    }
    if (next - retained <= h->capacity)
      break;
    if (std::chrono::steady_clock::now() >= deadline)
      throw Error(ErrorCode::timeout,
                  "publisher timed out waiting for the slowest subscriber (ring full)");
    const auto observed_generation = h->space_generation.value.load(std::memory_order_acquire);
    if (minimum_acknowledged(h, impl_->subscribers()) >= next - h->capacity) {
      const auto refreshed = minimum_acknowledged(h, impl_->subscribers());
      if (refreshed > retained) {
        retained = refreshed;
        h->retention_watermark.value.store(retained, std::memory_order_release);
      }
      continue;
    }
    wait_for_predicate(
        impl_->wait_strategy, impl_->spin_budget, h->space_generation.value, deadline,
        [&] {
          return h->space_generation.value.load(std::memory_order_acquire) != observed_generation;
        },
        "publisher timed out waiting for the slowest subscriber (ring full)");
  }
  auto &slot = impl_->slots()[(next - 1) & (h->capacity - 1)];
  slot.sequence = next;
  slot.size = static_cast<std::uint32_t>(payload.size());
  slot.reserved = 0;
  if (!payload.empty())
    std::memcpy(slot.payload, payload.data(), payload.size());
  h->published.value.store(next, std::memory_order_release);
  h->data_generation.value.fetch_add(1, std::memory_order_release);
  detail::futex_wake_all(h->data_generation.value);
  return next;
}

void Bus::wait_for_acknowledgement(std::uint64_t sequence, std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->role_claimed || impl_->role != Role::publisher)
    throw Error(ErrorCode::invalid_argument,
                "publisher role must be claimed before waiting for acknowledgement");
  if (sequence == 0 || sequence > impl_->header->published.value.load(std::memory_order_acquire))
    throw Error(ErrorCode::invalid_argument, "cannot wait for an unpublished sequence");
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto *h = impl_->header;
  wait_for_predicate(
      impl_->wait_strategy, impl_->spin_budget, h->space_generation.value, deadline,
      [&] { return minimum_acknowledged(h, impl_->subscribers()) >= sequence; },
      "publisher timed out waiting for message acknowledgement");
}

Message Bus::receive(std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->role_claimed || impl_->role != Role::subscriber)
    throw Error(ErrorCode::invalid_argument, "subscriber role must be claimed before receiving");
  if (impl_->pending == 0)
    throw Error(ErrorCode::system, "sequence space exhausted");
  auto deadline = std::chrono::steady_clock::now() + timeout;
  auto *h = impl_->header;
  wait_for_predicate(
      impl_->wait_strategy, impl_->spin_budget, h->data_generation.value, deadline,
      [&] { return h->published.value.load(std::memory_order_acquire) >= impl_->pending; },
      "subscriber timed out waiting for a message");
  const auto seq = impl_->pending;
  auto &slot = impl_->slots()[(seq - 1) & (impl_->header->capacity - 1)];
  if (slot.sequence != seq || slot.size > kPayloadBytes)
    throw Error(ErrorCode::corrupt_message, "slot sequence or payload length is invalid");
  Message out{};
  out.sequence = seq;
  out.size = slot.size;
  std::memcpy(out.payload, slot.payload, slot.size);
  impl_->delivered = true;
  return out;
}

void Bus::acknowledge(std::uint64_t sequence) {
  if (!impl_ || !impl_->role_claimed || impl_->role != Role::subscriber)
    throw Error(ErrorCode::invalid_argument,
                "subscriber role must be claimed before acknowledgement");
  if (!impl_->delivered || sequence != impl_->pending ||
      sequence > impl_->header->published.value.load(std::memory_order_acquire))
    throw Error(ErrorCode::invalid_argument,
                "acknowledgement must match the next delivered sequence");
  impl_->member->acknowledged.value.store(sequence, std::memory_order_release);
  impl_->header->space_generation.value.fetch_add(1, std::memory_order_release);
  detail::futex_wake_one(impl_->header->space_generation.value);
  impl_->pending = sequence == std::numeric_limits<std::uint64_t>::max() ? 0 : sequence + 1;
  impl_->delivered = false;
}

} // namespace ironbus
