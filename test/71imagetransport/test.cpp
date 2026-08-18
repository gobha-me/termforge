// TermForge — explicit indirect image payload transport (#111).
//
// Offline driver tests pin the policy boundaries: no configured strategy is
// byte-identical direct t=d; a configured strategy owns its locator until an
// ordered reply; staging failure and terminal rejection take the direct route
// with Info; sink refusal releases the lease at the accepted-write boundary.
// A small POSIX case separately proves the built-in strategy copies exact
// bytes and unlinks its object when the lease retires.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/core/image_transport.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace termforge;

namespace {

class FakeLease final : public ImageTransferLease {
 public:
  FakeLease(std::string locator, int& retired)
      : m_locator(std::move(locator)), m_retired(retired) {}
  ~FakeLease() override { ++m_retired; }

  [[nodiscard]] auto medium() const noexcept
      -> ImageTransferMedium override {
    return ImageTransferMedium::SharedMemory;
  }
  [[nodiscard]] auto locator() const noexcept -> std::string_view override {
    return m_locator;
  }

 private:
  std::string m_locator;
  int& m_retired;
};

class FakeTransport final : public ImageTransport {
 public:
  explicit FakeTransport(bool fail = false) : m_fail(fail) {}

  [[nodiscard]] auto stage(std::span<const std::byte> payload)
      -> std::expected<std::unique_ptr<ImageTransferLease>, ErrorEvent>
      override {
    ++calls;
    staged.assign(payload.begin(), payload.end());
    if (m_fail) {
      return std::unexpected{ErrorEvent{Severity::Warning, "fake-transport",
                                        "deliberate staging failure"}};
    }
    return std::make_unique<FakeLease>("/termforge-test-image", retired);
  }

  int calls{0};
  int retired{0};
  std::vector<std::byte> staged;

 private:
  bool m_fail{false};
};

class RefusingSink final : public ByteSink {
 public:
  auto write(std::span<const char>)
      -> std::expected<void, ErrorEvent> override {
    return std::unexpected{
        ErrorEvent{Severity::Error, "sink", "deliberate refusal"}};
  }
};

auto sample_image() -> Image {
  return tfsupport::solid(1, 1, Pixel{0x11, 0x22, 0x33, 0x44});
}

auto megabyte_image() -> Image {
  constexpr std::size_t kPixels = std::size_t{512} * 512U;
  std::vector<Pixel> pixels(kPixels);
  std::uint32_t state = 0x12345678U;
  for (auto& pixel : pixels) {
    state = state * 1664525U + 1013904223U;
    pixel = Pixel{static_cast<std::uint8_t>(state >> 24),
                  static_cast<std::uint8_t>(state >> 16),
                  static_cast<std::uint8_t>(state >> 8),
                  static_cast<std::uint8_t>(state)};
  }
  return Image{512, 512, std::move(pixels)};
}

} // namespace

TEST_CASE("image transport: POSIX lease owns exact bytes and cleanup",
          "[image-transport][posix]") {
  PosixSharedMemoryTransport transport;
  const std::array payload{std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
                           std::byte{0x40}};
  auto staged = transport.stage(payload);
  REQUIRE(staged);
  REQUIRE(*staged);
  CHECK((*staged)->medium() == ImageTransferMedium::SharedMemory);
  const std::string name{(*staged)->locator()};

  const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
  REQUIRE(fd >= 0);
  std::array<std::byte, 4> read{};
  REQUIRE(::pread(fd, read.data(), read.size(), 0) ==
          static_cast<ssize_t>(read.size()));
  CHECK(::close(fd) == 0);
  CHECK(read == payload);

  staged->reset();
  errno = 0;
  CHECK(::shm_open(name.c_str(), O_RDONLY, 0) == -1);
  CHECK(errno == ENOENT);
}

TEST_CASE("image transport: successful Kitty upload holds lease through reply",
          "[image-transport][kitty][reply]") {
  auto transport = std::make_shared<FakeTransport>();
  KittyDriver driver;
  std::string out;
  driver.set_output(&out);
  driver.set_image_transport(transport);
  const Image image = megabyte_image();

  REQUIRE(driver.draw_image(Rect{0, 0, 1, 1}, image));
  driver.flush();
  CHECK(transport->calls == 1);
  CHECK(transport->staged.size() == 1'048'576);
  CHECK(out.find("a=t,t=s,f=32,i=1,s=512,v=512,S=1048576,q=0;") !=
        std::string::npos);
  CHECK(out.find("t=d") == std::string::npos);
  CHECK(out.size() < 512);
  CHECK(driver.last_frame_bytes().image_transmit < 200);
  CHECK(transport->retired == 0);

  KittyDriver direct;
  std::string direct_out;
  direct.set_output(&direct_out);
  REQUIRE(direct.draw_image(Rect{0, 0, 1, 1}, image));
  direct.flush();
  CHECK(tfsupport::reassemble(direct_out) == transport->staged);

  driver.consume_reply(TerminalReply{1, std::nullopt, "OK"});
  CHECK(transport->retired == 1);
  CHECK(driver.take_driver_events().empty());

  out.clear();
  const Image changed =
      tfsupport::solid(1, 1, Pixel{0x55, 0x66, 0x77, 0x88});
  REQUIRE(driver.draw_image(Rect{0, 0, 1, 1}, changed));
  driver.flush();
  CHECK(transport->calls == 1);
  CHECK(out.find("a=t,t=d,f=32") != std::string::npos);
}

TEST_CASE("image transport: staging failure is one direct lesser-route event",
          "[image-transport][kitty][fallback]") {
  auto transport = std::make_shared<FakeTransport>(true);
  KittyDriver driver;
  std::string out;
  driver.set_output(&out);
  driver.set_image_transport(transport);

  REQUIRE(driver.draw_image(Rect{0, 0, 1, 1}, sample_image()));
  driver.flush();
  CHECK(out.find("a=t,t=d,f=32") != std::string::npos);
  CHECK(out.find("t=s") == std::string::npos);
  const auto events = driver.take_driver_events();
  REQUIRE(events.size() == 1);
  CHECK(events.front().severity == Severity::Info);
  CHECK(events.front().message.find("deliberate staging failure") !=
        std::string::npos);
}

TEST_CASE("image transport: refused direct fallback does not claim success",
          "[image-transport][kitty][fallback][sink]") {
  auto transport = std::make_shared<FakeTransport>(true);
  RefusingSink sink;
  KittyDriver driver;
  driver.set_output(&sink);
  driver.set_image_transport(transport);

  REQUIRE(driver.pin_image(sample_image()));
  driver.flush();
  REQUIRE(driver.take_output_error());
  CHECK(driver.take_driver_events().empty());
}

TEST_CASE("image transport: rejection retries direct and latches the route",
          "[image-transport][kitty][fallback][reply]") {
  auto transport = std::make_shared<FakeTransport>();
  KittyDriver driver;
  std::string out;
  driver.set_output(&out);
  driver.set_image_transport(transport);

  const auto first = driver.pin_image(sample_image());
  REQUIRE(first);
  driver.flush();
  REQUIRE(driver.pinned_image_status(*first).update_pending);
  out.clear();

  driver.consume_reply(
      TerminalReply{first->id, std::nullopt, "ENOTSUP: t=s"});
  CHECK(transport->retired == 1);
  CHECK(driver.take_driver_events().empty());
  driver.flush();
  REQUIRE(out.find("a=t,t=d,f=32") != std::string::npos);
  CHECK(driver.pinned_image_status(*first).content_ready);
  const auto events = driver.take_driver_events();
  REQUIRE(events.size() == 1);
  CHECK(events.front().severity == Severity::Info);

  out.clear();
  const auto second = driver.pin_image(sample_image());
  REQUIRE(second);
  driver.flush();
  CHECK(transport->calls == 1);
  CHECK(out.find("a=t,t=d,f=32") != std::string::npos);
  CHECK(driver.take_driver_events().empty());
}

TEST_CASE("image transport: rejected region retry re-places after direct data",
          "[image-transport][kitty][fallback][placement]") {
  auto transport = std::make_shared<FakeTransport>();
  KittyDriver driver;
  std::string out;
  driver.set_output(&out);
  driver.set_image_transport(transport);
  const Image image = sample_image();

  REQUIRE(driver.draw_image(Rect{0, 0, 1, 1}, image));
  driver.flush();
  out.clear();
  driver.consume_reply(TerminalReply{1, std::nullopt, "ENOTSUP: t=s"});
  REQUIRE(driver.draw_image(Rect{0, 0, 1, 1}, image));
  driver.flush();

  const auto remove = out.find("a=d,d=i,i=1,p=1");
  const auto direct = out.find("a=t,t=d,f=32");
  const auto place = out.find("a=p,i=1,p=1");
  REQUIRE(remove != std::string::npos);
  REQUIRE(direct != std::string::npos);
  REQUIRE(place != std::string::npos);
  CHECK(remove < direct);
  CHECK(direct < place);
}

TEST_CASE("image transport: refused write releases lease and commits nothing",
          "[image-transport][kitty][sink]") {
  auto transport = std::make_shared<FakeTransport>();
  RefusingSink sink;
  KittyDriver driver;
  driver.set_output(&sink);
  driver.set_image_transport(transport);

  const auto pin = driver.pin_image(sample_image());
  REQUIRE(pin);
  CHECK(transport->retired == 0);
  driver.flush();
  CHECK(transport->retired == 1);
  CHECK_FALSE(driver.pinned_image_status(*pin).valid);
  CHECK(driver.residency() == ImageResidency{});
  REQUIRE(driver.take_output_error());
  CHECK(driver.take_driver_events().empty());
}
