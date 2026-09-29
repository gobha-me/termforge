#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <vector>

#include "termforge/widgets/layout.hpp"

using namespace termforge;

TEST_CASE("row and column layout share exact cell rules", "[layout]") {
  const std::array tracks{LayoutTrack{10, 20, 0}, LayoutTrack{10, 30, 1}};

  const auto row = layout_row({5, 7, 40, 3}, tracks, 2);
  REQUIRE(row);
  const std::vector<Rect> expected_row{{5, 7, 20, 3}, {27, 7, 18, 3}};
  CHECK(*row == expected_row);

  const auto column = layout_column({7, 5, 3, 40}, tracks, 2);
  REQUIRE(column);
  const std::vector<Rect> expected_column{{7, 5, 3, 20}, {7, 27, 3, 18}};
  CHECK(*column == expected_column);

  // The trailing track loses preferred cells first, then the leading track.
  const auto narrow = layout_row({5, 7, 27, 3}, tracks, 2);
  REQUIRE(narrow);
  const std::vector<Rect> expected_narrow{{5, 7, 15, 3}, {22, 7, 10, 3}};
  CHECK(*narrow == expected_narrow);
}

TEST_CASE("weighted growth gives remainder to later flexible tracks",
          "[layout]") {
  const std::array equal{LayoutTrack{}, LayoutTrack{}};
  const auto odd = layout_row({0, 0, 5, 1}, equal);
  REQUIRE(odd);
  const std::vector<Rect> expected_odd{{0, 0, 2, 1}, {2, 0, 3, 1}};
  CHECK(*odd == expected_odd);

  const std::array weighted{LayoutTrack{0, 20, 1}, LayoutTrack{0, 20, 2}};
  const auto result = layout_row({3, 4, 101, 2}, weighted);
  REQUIRE(result);
  const std::vector<Rect> expected{{3, 4, 40, 2}, {43, 4, 61, 2}};
  CHECK(*result == expected);

  const std::array fixed{LayoutTrack{3, 3, 0}, LayoutTrack{4, 4, 0}};
  const auto slack = layout_row({0, 0, 20, 1}, fixed, 1);
  REQUIRE(slack);
  const std::vector<Rect> expected_slack{{0, 0, 3, 1}, {4, 0, 4, 1}};
  CHECK(*slack == expected_slack);
}

TEST_CASE("minimum and gap failures refuse the whole layout", "[layout]") {
  const std::array tracks{LayoutTrack{3, 5, 1}, LayoutTrack{4, 4, 0}};
  const auto exact = layout_row({0, 0, 8, 1}, tracks, 1);
  REQUIRE(exact);
  const std::vector<Rect> expected{{0, 0, 3, 1}, {4, 0, 4, 1}};
  CHECK(*exact == expected);

  for (const auto& refused : {layout_row({0, 0, 7, 1}, tracks, 1),
                              layout_row({0, 0, 8, 1}, tracks, 9),
                              layout_column({0, 0, 1, 8}, tracks, 9)}) {
    REQUIRE_FALSE(refused);
    CHECK(refused.error().severity == Severity::Warning);
    CHECK(refused.error().source == "layout");
  }
}

TEST_CASE("invalid declarations fail even when bounds are empty", "[layout]") {
  for (const auto bad :
       {LayoutTrack{-1, 0, 1}, LayoutTrack{2, 1, 1}, LayoutTrack{0, 0, -1}}) {
    const std::array tracks{bad};
    const auto result = layout_row({0, 0, 0, 0}, tracks);
    REQUIRE_FALSE(result);
    CHECK(result.error().severity == Severity::Warning);
  }
  const std::array tracks{LayoutTrack{}};
  CHECK_FALSE(layout_row({0, 0, 0, 0}, tracks, -1));
  CHECK_FALSE(layout_column({0, 0, 0, 0}, tracks, -1));
}

TEST_CASE("zero and offscreen bounds are safe and do not invent clipping",
          "[layout]") {
  const std::array tracks{LayoutTrack{4, 9, 0}, LayoutTrack{3, 3, 1}};
  const auto empty = layout_row({-100, 80, 0, 20}, tracks, 1);
  REQUIRE(empty);
  const std::vector<Rect> expected_empty{{-100, 80, 0, 0}, {-100, 80, 0, 0}};
  CHECK(*empty == expected_empty);
  const auto cross_empty = layout_column({30, -100, -2, 20}, tracks, 1);
  REQUIRE(cross_empty);
  const std::vector<Rect> expected_cross{{30, -100, 0, 0}, {30, -100, 0, 0}};
  CHECK(*cross_empty == expected_cross);

  const auto offscreen = layout_row({-100, 80, 20, 3}, tracks, 1);
  REQUIRE(offscreen);
  const std::vector<Rect> expected_offscreen{{-100, 80, 9, 3},
                                             {-90, 80, 10, 3}};
  CHECK(*offscreen == expected_offscreen);

  const std::array<LayoutTrack, 0> none{};
  const auto zero_tracks = layout_row({0, 0, 100, 1}, none);
  REQUIRE(zero_tracks);
  CHECK(zero_tracks->empty());
}

TEST_CASE("extreme origins use checked wide arithmetic", "[layout]") {
  constexpr int hi = std::numeric_limits<int>::max();
  constexpr int lo = std::numeric_limits<int>::min();
  const std::array tracks{LayoutTrack{}, LayoutTrack{}};

  const auto low = layout_row({lo, 2, 3, 1}, tracks);
  REQUIRE(low);
  CHECK((*low)[0] == Rect{lo, 2, 1, 1});
  CHECK((*low)[1] == Rect{lo + 1, 2, 2, 1});

  const auto high = layout_row({hi, 2, 3, 1}, tracks);
  REQUIRE_FALSE(high);
  CHECK(high.error().severity == Severity::Warning);
  const auto high_column = layout_column({2, hi, 1, 3}, tracks);
  REQUIRE_FALSE(high_column);
  CHECK(high_column.error().severity == Severity::Warning);

  const std::array huge_weights{LayoutTrack{0, 0, hi}, LayoutTrack{0, 0, hi}};
  const auto huge = layout_row({0, 0, hi, 1}, huge_weights);
  REQUIRE(huge);
  CHECK((*huge)[0] == Rect{0, 0, hi / 2, 1});
  CHECK((*huge)[1] == Rect{hi / 2, 0, hi / 2 + 1, 1});

  const std::array huge_minima{LayoutTrack{hi, hi, 0}, LayoutTrack{hi, hi, 0}};
  const auto no_room = layout_row({0, 0, hi, 1}, huge_minima);
  REQUIRE_FALSE(no_room);
  CHECK(no_room.error().severity == Severity::Warning);
}

TEST_CASE("bounded matrix keeps ordering, minima and transpose symmetry",
          "[layout]") {
  const std::array tracks{LayoutTrack{0, 2, 1}, LayoutTrack{1, 4, 0},
                          LayoutTrack{0, 3, 2}};
  for (int major = 0; major <= 24; ++major) {
    for (int gap = 0; gap <= 2; ++gap) {
      const auto row = layout_row({-3, 7, major, 2}, tracks, gap);
      const auto column = layout_column({7, -3, 2, major}, tracks, gap);
      REQUIRE(static_cast<bool>(row) == static_cast<bool>(column));
      if (!row) {
        CHECK(row.error().severity == Severity::Warning);
        continue;
      }
      REQUIRE(row->size() == tracks.size());
      REQUIRE(column->size() == tracks.size());
      if (major == 0) {
        for (const auto& rect : *row)
          CHECK(rect.empty());
        continue;
      }
      int previous_end = -3;
      for (std::size_t i = 0; i < tracks.size(); ++i) {
        const auto r = (*row)[i];
        const auto c = (*column)[i];
        CHECK(r.x >= previous_end);
        CHECK(r.w >= tracks[i].minimum);
        CHECK(r.y == 7);
        CHECK(r.h == 2);
        CHECK(c.x == 7);
        CHECK(c.y == r.x);
        CHECK(c.w == 2);
        CHECK(c.h == r.w);
        previous_end = r.x + r.w;
      }
      CHECK(previous_end <= -3 + major);
    }
  }
}
