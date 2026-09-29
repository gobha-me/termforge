#include "termforge/widgets/layout.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace termforge {
namespace {

auto refuse(std::string message) -> std::unexpected<ErrorEvent> {
  return std::unexpected{
      ErrorEvent{Severity::Warning, "layout", std::move(message)}};
}

auto add_checked(std::uint64_t& total, int value) -> bool {
  const auto part = static_cast<std::uint64_t>(value);
  if (part > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += part;
  return true;
}

auto compose(Rect bounds, std::span<const LayoutTrack> tracks, int spacing,
             bool row) -> std::expected<std::vector<Rect>, ErrorEvent> {
  if (spacing < 0) return refuse("Spacing must be nonnegative");

  std::uint64_t minimum_sum = 0;
  std::uint64_t preferred_sum = 0;
  std::uint64_t weight_sum = 0;
  for (const auto& track : tracks) {
    if (track.minimum < 0 || track.preferred < track.minimum ||
        track.grow_weight < 0)
      return refuse(
          "Tracks require nonnegative minimum/weight and preferred >= minimum");
    if (!add_checked(minimum_sum, track.minimum) ||
        !add_checked(preferred_sum, track.preferred) ||
        !add_checked(weight_sum, track.grow_weight))
      return refuse("Track totals are not representable");
  }

  if (tracks.empty()) return std::vector<Rect>{};

  const int major = row ? bounds.w : bounds.h;
  const int cross = row ? bounds.h : bounds.w;
  if (major <= 0 || cross <= 0) {
    return std::vector<Rect>(tracks.size(), Rect{bounds.x, bounds.y, 0, 0});
  }

  const auto available = static_cast<std::uint64_t>(major);
  const auto gaps = static_cast<std::uint64_t>(tracks.size() - 1);
  const auto gap = static_cast<std::uint64_t>(spacing);
  if (gap != 0 && gaps > available / gap)
    return refuse("Spacing does not fit the bounds");
  const auto cell_budget = available - gaps * gap;
  if (minimum_sum > cell_budget)
    return refuse("Track minima do not fit the bounds");

  std::vector<Rect> result;
  result.reserve(tracks.size());
  std::vector<int> sizes;
  sizes.reserve(tracks.size());
  for (const auto& track : tracks)
    sizes.push_back(track.preferred);

  if (preferred_sum > cell_budget) {
    auto deficit = preferred_sum - cell_budget;
    for (std::size_t i = tracks.size(); i > 0 && deficit > 0; --i) {
      const auto capacity =
          static_cast<std::uint64_t>(sizes[i - 1] - tracks[i - 1].minimum);
      const auto shrink = std::min(capacity, deficit);
      sizes[i - 1] -= static_cast<int>(shrink);
      deficit -= shrink;
    }
  } else if (weight_sum > 0) {
    const auto surplus = cell_budget - preferred_sum;
    std::uint64_t assigned = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      const auto share = surplus *
                         static_cast<std::uint64_t>(tracks[i].grow_weight) /
                         weight_sum;
      sizes[i] += static_cast<int>(share);
      assigned += share;
    }
    auto remainder = surplus - assigned;
    for (std::size_t i = tracks.size(); i > 0 && remainder > 0; --i) {
      if (tracks[i - 1].grow_weight == 0) continue;
      ++sizes[i - 1];
      --remainder;
    }
  }

  std::int64_t position = row ? bounds.x : bounds.y;
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    if (position > std::numeric_limits<int>::max())
      return refuse("Track origin is not representable");
    if (row) {
      result.push_back(
          Rect{static_cast<int>(position), bounds.y, sizes[i], bounds.h});
    } else {
      result.push_back(
          Rect{bounds.x, static_cast<int>(position), bounds.w, sizes[i]});
    }
    position += sizes[i];
    if (i + 1 < tracks.size()) position += spacing;
  }
  return result;
}

} // namespace

auto layout_row(Rect bounds, std::span<const LayoutTrack> tracks, int spacing)
    -> std::expected<std::vector<Rect>, ErrorEvent> {
  return compose(bounds, tracks, spacing, true);
}

auto layout_column(Rect bounds, std::span<const LayoutTrack> tracks,
                   int spacing)
    -> std::expected<std::vector<Rect>, ErrorEvent> {
  return compose(bounds, tracks, spacing, false);
}

} // namespace termforge
