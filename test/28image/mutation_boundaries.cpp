#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "termforge/core/types.hpp"

using termforge::Image;
using termforge::Pixel;
using termforge::Rect;

TEST_CASE("Rect: touching edges return the canonical empty rectangle",
          "[image][rect][failure][mutation-boundary]") {
  SECTION("only the horizontal intervals touch") {
    const Rect left{2, 3, 2, 4};
    const Rect right{4, 1, 3, 8};

    CHECK(left.intersect(right) == Rect{});
    CHECK(right.intersect(left) == Rect{});

    const Rect overlapping{2, 3, 3, 4};
    CHECK(overlapping.intersect(right) == Rect{4, 3, 1, 4});
    CHECK(right.intersect(overlapping) == Rect{4, 3, 1, 4});
  }

  SECTION("only the vertical intervals touch") {
    const Rect top{2, 3, 4, 2};
    const Rect bottom{1, 5, 8, 3};

    CHECK(top.intersect(bottom) == Rect{});
    CHECK(bottom.intersect(top) == Rect{});

    const Rect overlapping{2, 3, 4, 3};
    CHECK(overlapping.intersect(bottom) == Rect{2, 5, 4, 1});
    CHECK(bottom.intersect(overlapping) == Rect{2, 5, 4, 1});
  }
}

TEST_CASE("Image: cropped blend preserves the source row stride",
          "[image][failure][mutation-boundary]") {
  const Image source{2, 3,
                     std::vector<Pixel>{{1, 2, 3, 255},
                                        {11, 12, 13, 255},
                                        {21, 22, 23, 255},
                                        {31, 32, 33, 255},
                                        {41, 42, 43, 255},
                                        {51, 52, 53, 255}}};
  Image destination{2, 2, std::vector<Pixel>(4, Pixel{0, 0, 0, 0})};

  destination.blend(source, Rect{0, 1, 2, 2}, 0, 0);

  CHECK(destination.at(0, 0) == Pixel{21, 22, 23, 255});
  CHECK(destination.at(1, 0) == Pixel{31, 32, 33, 255});
  CHECK(destination.at(0, 1) == Pixel{41, 42, 43, 255});
  CHECK(destination.at(1, 1) == Pixel{51, 52, 53, 255});

  CHECK(source.at(0, 0) == Pixel{1, 2, 3, 255});
  CHECK(source.at(1, 0) == Pixel{11, 12, 13, 255});
  CHECK(source.at(0, 1) == Pixel{21, 22, 23, 255});
  CHECK(source.at(1, 1) == Pixel{31, 32, 33, 255});
  CHECK(source.at(0, 2) == Pixel{41, 42, 43, 255});
  CHECK(source.at(1, 2) == Pixel{51, 52, 53, 255});
}
