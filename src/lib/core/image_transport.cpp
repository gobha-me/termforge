#include "termforge/core/image_transport.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace termforge {
namespace {

class PosixSharedMemoryLease final : public ImageTransferLease {
 public:
  PosixSharedMemoryLease(std::string name, int fd, void* mapping,
                         std::size_t size) noexcept
      : m_name(std::move(name)), m_fd(fd), m_mapping(mapping), m_size(size) {}

  ~PosixSharedMemoryLease() override {
    if (m_mapping != MAP_FAILED) (void)::munmap(m_mapping, m_size);
    if (m_fd >= 0) (void)::close(m_fd);
    if (!m_name.empty()) (void)::shm_unlink(m_name.c_str());
  }

  [[nodiscard]] auto medium() const noexcept -> ImageTransferMedium override {
    return ImageTransferMedium::SharedMemory;
  }
  [[nodiscard]] auto locator() const noexcept -> std::string_view override {
    return m_name;
  }

 private:
  std::string m_name;
  int m_fd{-1};
  void* m_mapping{MAP_FAILED};
  std::size_t m_size{0};
};

std::atomic<std::uint64_t> g_next_name{1};

[[nodiscard]] auto transport_error(std::string_view operation, int error)
    -> std::unexpected<ErrorEvent> {
  return std::unexpected{
      ErrorEvent{Severity::Warning, "image-transport",
                 std::format("POSIX shared memory {} failed: {}", operation,
                             std::strerror(error))}};
}

} // namespace

auto PosixSharedMemoryTransport::stage(std::span<const std::byte> payload)
    -> std::expected<std::unique_ptr<ImageTransferLease>, ErrorEvent> {
  if (payload.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "image-transport",
                                      "cannot stage an empty image payload"}};
  }
  if (payload.size() >
      static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max())) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "image-transport",
        "image payload is too large for a POSIX shared-memory object"}};
  }

  int fd = -1;
  std::string name;
  constexpr int kNameAttempts = 64;
  for (int attempt = 0; attempt < kNameAttempts; ++attempt) {
    const auto sequence = g_next_name.fetch_add(1, std::memory_order_relaxed);
    name = std::format("/tty-graphics-protocol-termforge-{}-{}",
                       static_cast<long long>(::getpid()), sequence);
    fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd >= 0) break;
    if (errno != EEXIST) return transport_error("creation", errno);
  }
  if (fd < 0) {
    return std::unexpected{
        ErrorEvent{Severity::Warning, "image-transport",
                   "could not allocate a unique POSIX shared-memory name"}};
  }

  const auto fail = [&](std::string_view operation, int error)
      -> std::expected<std::unique_ptr<ImageTransferLease>, ErrorEvent> {
    (void)::close(fd);
    (void)::shm_unlink(name.c_str());
    return transport_error(operation, error);
  };

  while (::ftruncate(fd, static_cast<off_t>(payload.size())) != 0) {
    if (errno == EINTR) continue;
    return fail("resize", errno);
  }

  void* mapping = ::mmap(nullptr, payload.size(), PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) return fail("mapping", errno);
  std::memcpy(mapping, payload.data(), payload.size());

  try {
    return std::make_unique<PosixSharedMemoryLease>(std::move(name), fd,
                                                    mapping, payload.size());
  } catch (...) {
    // Ownership has not crossed into a lease yet. Preserve the allocation
    // exception while keeping its external side effects out of /dev/shm.
    (void)::munmap(mapping, payload.size());
    (void)::close(fd);
    (void)::shm_unlink(name.c_str());
    throw;
  }
}

} // namespace termforge
