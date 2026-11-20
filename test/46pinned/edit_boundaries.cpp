#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace termforge;

TEST_CASE("pinned: either axis independently refuses a partial-edit overrun",
          "[pinned][kitty][edit][failure][mutation-boundary]") {
  struct Boundary {
    Extent block;
    PixelPoint outside;
    PixelPoint inside;
  };
  const Boundary boundaries[] = {{{2, 1}, {3, 0}, {2, 0}},
                                 {{1, 2}, {0, 3}, {0, 2}}};

  for (const auto& boundary : boundaries) {
    DYNAMIC_SECTION("block " << boundary.block.w << "x" << boundary.block.h) {
      KittyDriver driver;
      std::string wire;
      driver.set_output(&wire);
      const auto pin =
          driver.pin_image(tfsupport::solid(4, 4, Pixel{1, 2, 3, 255}));
      REQUIRE(pin);
      driver.flush();
      const auto resident = driver.residency();
      REQUIRE(resident == ImageResidency{0, 1, 64});
      const auto status = driver.pinned_image_status(*pin);
      REQUIRE(status.valid);
      REQUIRE(status.content_ready);
      wire.clear();

      const auto block = tfsupport::solid(boundary.block.w, boundary.block.h,
                                          Pixel{9, 8, 7, 128});
      const auto refused = driver.edit_pinned(*pin, boundary.outside, block,
                                              ImageComposition::Overwrite);
      REQUIRE_FALSE(refused);
      CHECK(refused.error().severity == Severity::Warning);
      CHECK(driver.residency() == resident);
      CHECK(driver.pinned_image_status(*pin) == status);
      driver.flush();
      CHECK(wire.empty());
      CHECK(driver.last_frame_bytes().total() == 0);
      CHECK(driver.residency() == resident);
      CHECK(driver.pinned_image_status(*pin) == status);

      // The immediately adjacent block fits exactly against the same edge.
      REQUIRE(driver.edit_pinned(*pin, boundary.inside, block,
                                 ImageComposition::Overwrite));
      CHECK(driver.residency() == resident);
      driver.flush();
      const auto commands = tfsupport::apcs(wire);
      REQUIRE(commands.size() == 1);
      CHECK(tfsupport::key_value(commands[0], "a") == "f");
      CHECK(tfsupport::key_value(commands[0], "r") == "1");
      CHECK(tfsupport::key_value(commands[0], "i") == std::to_string(pin->id));
      CHECK(tfsupport::key_value(commands[0], "x") ==
            std::to_string(boundary.inside.x));
      CHECK(tfsupport::key_value(commands[0], "y") ==
            std::to_string(boundary.inside.y));
      CHECK(tfsupport::key_value(commands[0], "s") ==
            std::to_string(boundary.block.w));
      CHECK(tfsupport::key_value(commands[0], "v") ==
            std::to_string(boundary.block.h));
      CHECK(tfsupport::key_value(commands[0], "X") == "1");
      CHECK(tfsupport::reassemble(wire) ==
            std::vector<std::byte>{std::byte{9}, std::byte{8}, std::byte{7},
                                   std::byte{128}, std::byte{9}, std::byte{8},
                                   std::byte{7}, std::byte{128}});
      CHECK(driver.residency() == ImageResidency{0, 1, 72});
      CHECK(driver.pinned_image_status(*pin).content_ready);
    }
  }
}

TEST_CASE("pinned: a partial edit meters only its APC after preceding text",
          "[pinned][kitty][edit][bytes][mutation-boundary]") {
  KittyDriver driver;
  std::string wire;
  driver.set_output(&wire);
  const auto pin =
      driver.pin_image(tfsupport::solid(4, 4, Pixel{1, 2, 3, 255}));
  REQUIRE(pin);
  driver.flush();
  wire.clear();

  driver.draw_text(2, 3, "AB", Rgb{1, 2, 3}, Rgb{4, 5, 6}, Attr::None);
  const Image block{1, 1, {Pixel{9, 8, 7, 128}}};
  REQUIRE(driver.edit_pinned(*pin, PixelPoint{1, 1}, block,
                             ImageComposition::Overwrite));
  driver.flush();

  const auto commands = tfsupport::apcs(wire);
  REQUIRE(commands.size() == 1);
  CHECK(tfsupport::key_value(commands[0], "a") == "f");
  CHECK(tfsupport::key_value(commands[0], "r") == "1");
  CHECK(tfsupport::key_value(commands[0], "X") == "1");
  CHECK(tfsupport::key_value(commands[0], "x") == "1");
  CHECK(tfsupport::key_value(commands[0], "y") == "1");
  CHECK(tfsupport::key_value(commands[0], "s") == "1");
  CHECK(tfsupport::key_value(commands[0], "v") == "1");
  CHECK(tfsupport::reassemble(wire) ==
        std::vector<std::byte>{std::byte{9}, std::byte{8}, std::byte{7},
                               std::byte{128}});

  const auto begin = wire.find("\033_G");
  REQUIRE(begin != std::string::npos);
  const auto end = wire.find("\033\\", begin);
  REQUIRE(end != std::string::npos);
  const auto edit_bytes = end + 2 - begin;
  const auto cell_bytes = wire.size() - edit_bytes;
  REQUIRE(cell_bytes > 0);
  CHECK(wire.substr(0, begin).find("AB") != std::string::npos);
  const auto metered = driver.last_frame_bytes();
  CHECK(metered.image_edit == edit_bytes);
  CHECK(metered.image_transmit == 0);
  CHECK(metered.cells == cell_bytes);
  CHECK(metered.total() == wire.size());
}
