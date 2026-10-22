#include "ironbus/bus.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace ironbus {
namespace {
constexpr std::uint64_t kMagic = 0x49524f4e42555331ULL; // IRONBUS1
constexpr std::uint32_t kVersion = 2;
constexpr std::uint32_t kInitializing = 1;
constexpr std::uint32_t kReady = 2;
constexpr std::uint32_t kMaxCapacity = 1U << 20;
constexpr std::size_t kCacheLine = 64;

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

struct alignas(16) Header {
  std::uint64_t magic{};
  std::uint32_t version{};
  std::uint32_t header_size{};
  std::uint32_t capacity{};
  std::uint32_t slot_size{};
  std::uint64_t total_size{};
  std::atomic<std::uint32_t> state{0};
  std::uint32_t reserved{};
  Cursor published;
  Cursor acknowledged;
  Generation data_generation;
  Generation space_generation;
};

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

std::size_t mapping_size(std::uint32_t capacity) {
  if (capacity < 2 || capacity > kMaxCapacity || (capacity & (capacity - 1)) != 0) {
    throw Error(ErrorCode::invalid_argument,
                "capacity must be a power of two between 2 and 1048576");
  }
  return sizeof(Header) + static_cast<std::size_t>(capacity) * sizeof(Slot);
}

void lock_role(int fd, Role role) {
#ifndef F_OFD_SETLK
#error "Linux OFD locks are required"
#endif
  struct flock lock {};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = role == Role::publisher ? 0 : 1;
  lock.l_len = 1;
  if (fcntl(fd, F_OFD_SETLK, &lock) != 0) {
    if (errno == EAGAIN || errno == EACCES) {
      throw Error(ErrorCode::role_busy, role == Role::publisher
                                            ? "publisher role is already held"
                                            : "subscriber role is already held");
    }
    throw Error(ErrorCode::system, os_error("acquire role lock"));
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
    return reinterpret_cast<Slot *>(static_cast<std::byte *>(mapping) + sizeof(Header));
  }
};

Bus::Bus() noexcept = default;
Bus::Bus(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Bus::~Bus() = default;
Bus::Bus(Bus &&) noexcept = default;
Bus &Bus::operator=(Bus &&) noexcept = default;

Bus Bus::create(std::string_view name, std::uint32_t capacity) {
  auto bytes = mapping_size(capacity);
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
  h->total_size = bytes;
  h->published.value.store(0, std::memory_order_relaxed);
  h->acknowledged.value.store(0, std::memory_order_relaxed);
  h->data_generation.value.store(0, std::memory_order_relaxed);
  h->space_generation.value.store(0, std::memory_order_relaxed);
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
  if (!h->state.is_lock_free() || !h->published.value.is_lock_free() ||
      !h->acknowledged.value.is_lock_free() || !h->data_generation.value.is_lock_free() ||
      !h->space_generation.value.is_lock_free())
    throw Error(ErrorCode::incompatible_layout, "required shared atomics are not lock-free");
  if (h->state.load(std::memory_order_acquire) != kReady)
    throw Error(ErrorCode::unsafe_initialization,
                "shared memory is not fully initialized; unlink it before retrying");
  if (h->magic != kMagic || h->version != kVersion || h->header_size != sizeof(Header) ||
      h->slot_size != sizeof(Slot) || h->total_size != impl->bytes || h->capacity < 2 ||
      h->capacity > kMaxCapacity || (h->capacity & (h->capacity - 1)) != 0 ||
      sizeof(Header) + static_cast<std::size_t>(h->capacity) * sizeof(Slot) != impl->bytes) {
    throw Error(ErrorCode::incompatible_layout, "shared-memory layout is incompatible or corrupt");
  }
  return Bus(std::move(impl));
}

void Bus::unlink(std::string_view name) {
  auto path = validate_name(name);
  if (shm_unlink(path.c_str()) != 0)
    throw Error(ErrorCode::system, os_error("unlink shared memory"));
}

void Bus::claim(Role role) {
  if (!impl_)
    throw Error(ErrorCode::invalid_argument, "bus is not open");
  if (impl_->role_claimed)
    throw Error(ErrorCode::invalid_argument, "a role is already claimed by this handle");
  lock_role(impl_->fd, role);
  impl_->role_claimed = true;
  impl_->role = role;
  if (role == Role::subscriber) {
    const auto acknowledged = impl_->header->acknowledged.value.load(std::memory_order_acquire);
    impl_->pending =
        acknowledged == std::numeric_limits<std::uint64_t>::max() ? 0 : acknowledged + 1;
  }
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
  return {impl_->header->published.value.load(std::memory_order_acquire),
          impl_->header->acknowledged.value.load(std::memory_order_acquire),
          impl_->header->capacity, impl_->header->slot_size};
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
  auto published = h->published.value.load(std::memory_order_relaxed);
  if (published == std::numeric_limits<std::uint64_t>::max())
    throw Error(ErrorCode::system, "sequence space exhausted");
  const auto next = published + 1;
  while (next - h->acknowledged.value.load(std::memory_order_acquire) > h->capacity) {
    if (std::chrono::steady_clock::now() >= deadline)
      throw Error(ErrorCode::timeout,
                  "publisher timed out waiting for subscriber acknowledgement (ring full)");
    std::this_thread::yield();
  }
  auto &slot = impl_->slots()[(next - 1) & (h->capacity - 1)];
  slot.sequence = next;
  slot.size = static_cast<std::uint32_t>(payload.size());
  slot.reserved = 0;
  if (!payload.empty())
    std::memcpy(slot.payload, payload.data(), payload.size());
  h->published.value.store(next, std::memory_order_release);
  return next;
}

Message Bus::receive(std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->role_claimed || impl_->role != Role::subscriber)
    throw Error(ErrorCode::invalid_argument, "subscriber role must be claimed before receiving");
  if (impl_->pending == 0)
    throw Error(ErrorCode::system, "sequence space exhausted");
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (impl_->header->published.value.load(std::memory_order_acquire) < impl_->pending) {
    if (std::chrono::steady_clock::now() >= deadline)
      throw Error(ErrorCode::timeout, "subscriber timed out waiting for a message");
    std::this_thread::yield();
  }
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
  impl_->header->acknowledged.value.store(sequence, std::memory_order_release);
  impl_->pending = sequence == std::numeric_limits<std::uint64_t>::max() ? 0 : sequence + 1;
  impl_->delivered = false;
}

} // namespace ironbus
