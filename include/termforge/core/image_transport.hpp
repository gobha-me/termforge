#pragma once

// TermForge — explicit non-direct image payload transports (#111).
//
// A terminal process can open a file or shared-memory object only when it
// shares that namespace with the application. TermForge cannot infer that
// fact from TERM, SSH variables, a tty, or an emulator name, so the universal
// default remains the Kitty protocol's direct t=d medium. An embedding that
// knows the namespace is shared may install an ImageTransport explicitly.
//
// Staging and cleanup belong to the strategy. The driver owns the returned
// lease until the terminal acknowledges the command (or the operation is
// otherwise retired); destroying the lease releases the external resource.

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string_view>

#include "termforge/core/types.hpp"

namespace termforge {

enum class ImageTransferMedium {
  File,
  TemporaryFile,
  SharedMemory,
};

class ImageTransferLease {
 public:
  ImageTransferLease() = default;
  virtual ~ImageTransferLease() = default;
  ImageTransferLease(const ImageTransferLease&) = delete;
  auto operator=(const ImageTransferLease&) -> ImageTransferLease& = delete;
  ImageTransferLease(ImageTransferLease&&) = delete;
  auto operator=(ImageTransferLease&&) -> ImageTransferLease& = delete;

  [[nodiscard]] virtual auto medium() const noexcept -> ImageTransferMedium = 0;
  [[nodiscard]] virtual auto locator() const noexcept -> std::string_view = 0;
};

class ImageTransport {
 public:
  ImageTransport() = default;
  virtual ~ImageTransport() = default;
  ImageTransport(const ImageTransport&) = delete;
  auto operator=(const ImageTransport&) -> ImageTransport& = delete;
  ImageTransport(ImageTransport&&) = delete;
  auto operator=(ImageTransport&&) -> ImageTransport& = delete;

  [[nodiscard]] virtual auto stage(std::span<const std::byte> payload)
      -> std::expected<std::unique_ptr<ImageTransferLease>, ErrorEvent> = 0;
};

// POSIX shared memory, deliberately opt-in. Constructing this object does not
// claim that the terminal can see the process's shm namespace; supplying it to
// a driver is the embedding's assertion that it can. Each lease uses an
// owner-only, collision-checked object and unlinks it best-effort on release.
class PosixSharedMemoryTransport final : public ImageTransport {
 public:
  [[nodiscard]] auto stage(std::span<const std::byte> payload)
      -> std::expected<std::unique_ptr<ImageTransferLease>,
                       ErrorEvent> override;
};

} // namespace termforge
