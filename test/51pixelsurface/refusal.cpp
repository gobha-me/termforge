// App recovery must reuse committed roots, not projected refused handles.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "support/apc.hpp"
#include "termforge/core/app.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace termforge;
namespace {
enum class Payload { Raw, Rgba, Rgb, Png };
enum class Change { Content, Move, Extent, Format, Kind, Cells, None };

class Producer final : public Widget {
 public:
  Payload kind{Payload::Raw};
  Image image{2, 2, std::vector<Pixel>(4, Pixel{80, 90, 100, 255})};
  std::vector<std::byte> bytes;
  EncodedImage encoded{};
  bool dirty{true};
  std::uint64_t revision{1};
  int raw_calls{0}, encoded_calls{0}, submissions{0};

  auto update(Payload next, Extent extent = {2, 2}) -> void {
    kind = next;
    image = Image{extent.w, extent.h,
                  std::vector<Pixel>(static_cast<std::size_t>(extent.w) *
                                         static_cast<std::size_t>(extent.h),
                                     Pixel{140, 150, 160, 255})};
    const auto format = kind == Payload::Rgb   ? ImageFormat::Rgb24
                        : kind == Payload::Png ? ImageFormat::Png
                                               : ImageFormat::Rgba32;
    const auto size =
        kind == Payload::Png
            ? 7U
            : image.pixels().size() * (kind == Payload::Rgb ? 3U : 4U);
    bytes.assign(size, static_cast<std::byte>(revision));
    encoded = {format, bytes, extent};
    dirty = true;
    ++revision;
  }
  auto draw(Screen& screen) -> void override {
    const auto r = rect();
    screen.fill_rect(r.x, r.y, r.w, r.h, {}, {});
    if (!r.empty()) screen.write_text(r.x, r.y, "Q", {}, {});
  }
  auto pixel_regions() -> std::vector<Rect> override { return {rect()}; }
  auto draw_encoded_pixels(Rect) -> const EncodedImage* override {
    ++encoded_calls;
    return kind == Payload::Raw ? nullptr : &encoded;
  }
  auto draw_pixels(Rect, Extent) -> const Image* override {
    ++raw_calls;
    return &image;
  }
  auto pixel_region_state(Rect) const noexcept -> PixelRegionState override {
    return {PixelRegionMode::Persistent, dirty, revision};
  }
  auto pixel_region_submitted(Rect, std::uint64_t accepted_revision) noexcept
      -> void override {
    ++submissions;
    if (accepted_revision == revision) dirty = false;
  }
};

struct RecoverySink final : ByteSink {
  std::vector<int> refused{1};
  std::vector<std::string> frames;
  std::vector<bool> accepted;
  auto write(std::span<const char> data)
      -> std::expected<void, ErrorEvent> override {
    const bool ok =
        !std::ranges::contains(refused, static_cast<int>(frames.size()));
    frames.emplace_back(data.begin(), data.end());
    accepted.push_back(ok);
    if (!ok)
      return std::unexpected{
          ErrorEvent{Severity::Warning, "recovery-sink", "refused"}};
    return {};
  }
};

class RecoveryApp final : public App {
 public:
  std::optional<Producer> changed{std::in_place};
  Producer clean;
  RecoverySink sink;
  Change change{Change::Content};
  int mutate_at{1};
  int omit_at{-1};
  bool repeated_collection{false};
  std::vector<std::pair<int, std::string>> replies;
  std::vector<ImageResidency> residency;
  std::vector<FrameBytes> meters;
  std::vector<int> acknowledgements;
  std::vector<bool> dirty;
  std::vector<ErrorEvent> errors;
  int frame{0};

  auto run(KittyDriver::PlacementMode mode, int frames = 4) -> void {
    auto selected = std::make_unique<KittyDriver>();
    selected->set_placement_mode(mode);
    run_with(std::move(selected), frames);
  }
  auto run_with(std::unique_ptr<TerminalDriver> selected, int frames = 4)
      -> void {
    test_run_frames(frames, 20, 8, nullptr, std::move(selected));
  }

 protected:
  auto on_render(Screen& screen) -> void override {
    driver().set_output(&sink);
    residency.push_back(driver().residency());
    meters.push_back(driver().last_frame_bytes());
    acknowledgements.push_back(changed ? changed->submissions : 0);
    dirty.push_back(changed && changed->dirty);
    screen.clear();
    if (frame == omit_at) changed.reset();
    if (changed) {
      if (frame == mutate_at) {
        if (change == Change::Content) changed->update(changed->kind);
        if (change == Change::Extent) changed->update(changed->kind, {3, 2});
        if (change == Change::Format) changed->update(Payload::Rgb);
        if (change == Change::Kind) changed->update(Payload::Rgba);
      }
      const int x = change == Change::Move && frame >= mutate_at ? 4 : 1;
      changed->set_geometry({x, 1, 4, 2});
      changed->draw(screen);
      render_pixel_regions(*changed);
      if (repeated_collection) render_pixel_regions(*changed);
    }
    clean.set_geometry({10, 1, 4, 2});
    clean.draw(screen);
    render_pixel_regions(clean);
    if (change == Change::Cells && frame >= mutate_at)
      screen.write_text(0, 6, "new cell", {}, {});
    ++frame;
  }
  auto on_event(const Event& event) -> void override {
    if (const auto* error = std::get_if<ErrorEvent>(&event))
      errors.push_back(*error);
  }
  auto now_steady() const -> std::chrono::steady_clock::time_point override {
    return m_now;
  }
  auto wait_readable(int ms) -> bool override {
    m_now += std::chrono::milliseconds(ms);
    return std::ranges::any_of(
        replies, [&](const auto& reply) { return reply.first == frame; });
  }
  auto read_available(char* out, int max) -> int override {
    const auto reply =
        std::ranges::find_if(replies, [&](const auto& candidate) {
          return candidate.first == frame;
        });
    if (reply == replies.end() || max < static_cast<int>(reply->second.size()))
      return 0;
    std::copy(reply->second.begin(), reply->second.end(), out);
    const int count = static_cast<int>(reply->second.size());
    replies.erase(reply);
    return count;
  }

 private:
  std::chrono::steady_clock::time_point m_now{};
};

constexpr auto modes =
    std::array{KittyDriver::PlacementMode::Classic,
               KittyDriver::PlacementMode::UnicodePlaceholders};
auto require_sink_error(const RecoveryApp& app, int count = 1) -> void {
  REQUIRE(app.errors.size() == static_cast<std::size_t>(count));
  for (const auto& error : app.errors) {
    CHECK(error.source == "recovery-sink");
    CHECK(error.severity == Severity::Warning);
  }
  for (std::size_t i = 1; i < app.meters.size(); ++i)
    CHECK(app.meters[i].total() == app.sink.frames[i - 1].size());
}
constexpr std::string_view placeholder{"\xF4\x8E\xBB\xAE"};
} // namespace

TEST_CASE("mixed refused content edits preserve unrelated resident roots",
          "[pixelsurface][app][refusal][issue398]") {
  for (const auto mode : modes) {
    CAPTURE(mode);
    RecoveryApp app;
    const auto source = app.clean.image;
    app.run(mode);
    require_sink_error(app);
    REQUIRE(app.changed);
    REQUIRE(app.sink.frames.size() >= 4);
    CHECK_FALSE(app.sink.accepted[1]);
    CHECK(app.dirty[2]);
    CHECK(app.acknowledgements[2] == 1);
    CHECK(app.changed->submissions == 2);
    CHECK(app.changed->raw_calls == 3);
    CHECK_FALSE(app.changed->dirty);
    CHECK(app.clean.submissions == 1);
    CHECK(app.clean.raw_calls == 1);
    CHECK(std::ranges::equal(source.pixels(), app.clean.image.pixels()));
    CHECK(app.residency[2] == ImageResidency{0, 2, 32});
    CHECK(app.residency[3] == ImageResidency{0, 2, 32});
    CHECK(tfsupport::total_transmits(app.sink.frames[2]) == 0);
    CHECK(tfsupport::frame_updates_of(app.sink.frames[2], 272) == 1);
    CHECK(tfsupport::frame_updates_of(app.sink.frames[2], 271) == 0);
    CHECK(tfsupport::data_deletes_of(app.sink.frames[2], 272) == 0);
    CHECK(tfsupport::data_deletes_of(app.sink.frames[2], 271) == 0);
    // A full root replacement retains its historical transmit bucket; only
    // partial edits are image_edit. Retry is exactly the same root payload.
    CHECK(app.meters[3].image_transmit > 0);
    CHECK(app.meters[3].image_transmit == app.meters[2].image_transmit);
    CHECK(app.sink.frames[3].empty());
    if (mode == KittyDriver::PlacementMode::UnicodePlaceholders)
      CHECK(tfsupport::count_of(app.sink.frames[2], placeholder) == 16);
  }
}

TEST_CASE("cell-only refusal repairs placeholder cells without content upload",
          "[pixelsurface][app][refusal][issue398]") {
  for (const auto mode : modes) {
    RecoveryApp app;
    app.change = Change::Cells;
    app.run(mode);
    require_sink_error(app);
    REQUIRE(app.changed);
    CHECK(app.changed->submissions == 1);
    CHECK(app.changed->raw_calls == 1);
    CHECK(app.clean.submissions == 1);
    CHECK(app.clean.raw_calls == 1);
    CHECK(tfsupport::total_data_transmits(app.sink.frames[2]) == 0);
    CHECK(app.sink.frames[3].empty());
    if (mode == KittyDriver::PlacementMode::UnicodePlaceholders)
      CHECK(tfsupport::count_of(app.sink.frames[2], placeholder) == 16);
  }
}

TEST_CASE("ANSI refusal repairs clean raster placements without content ack",
          "[pixelsurface][app][refusal][ansi][issue398]") {
  for (const auto change : {Change::Cells, Change::Content}) {
    RecoveryApp app;
    app.change = change;
    const auto source = app.clean.image;
    app.run_with(std::make_unique<AnsiRgbDriver>());
    require_sink_error(app);
    REQUIRE(app.changed);
    CHECK(tfsupport::count_of(app.sink.frames[2], "\xE2\x96\x80") == 16);
    CHECK(app.clean.raw_calls == 2); // initial raster + cell-shadow repair
    CHECK(app.clean.submissions == 1);
    CHECK_FALSE(app.clean.dirty);
    CHECK(std::ranges::equal(source.pixels(), app.clean.image.pixels()));
    CHECK(app.changed->submissions == (change == Change::Content ? 2 : 1));
    CHECK_FALSE(app.changed->dirty);
    CHECK(app.sink.frames[3].empty());
  }
}

TEST_CASE("refused movement retries placement without recreating its root",
          "[pixelsurface][app][refusal][issue398]") {
  for (const auto mode : modes) {
    RecoveryApp app;
    app.change = Change::Move;
    app.run(mode);
    require_sink_error(app);
    REQUIRE(app.changed);
    CHECK(app.changed->submissions == 1);
    CHECK(app.changed->raw_calls == 1);
    CHECK(app.clean.submissions == 1);
    CHECK(tfsupport::total_data_transmits(app.sink.frames[2]) == 0);
    CHECK(tfsupport::placements_of(app.sink.frames[2], 272) == 1);
    CHECK(tfsupport::data_deletes_of(app.sink.frames[2], 272) == 0);
    CHECK(app.residency[3] == ImageResidency{0, 2, 32});
    CHECK(app.sink.frames[3].empty());
  }
}

TEST_CASE("refused identity changes retain ownership of the old root",
          "[pixelsurface][app][refusal][identity][issue398]") {
  for (const auto mode : modes)
    for (const auto change : {Change::Extent, Change::Format, Change::Kind}) {
      CAPTURE(mode, change);
      RecoveryApp app;
      app.change = change;
      if (change == Change::Format) app.changed->update(Payload::Rgba);
      app.run(mode);
      require_sink_error(app);
      REQUIRE(app.changed);
      CHECK(app.dirty[2]);
      CHECK(app.residency[2] == ImageResidency{0, 2, 32});
      CHECK(app.residency[3].pinned_images == 2);
      const std::uint64_t changed_bytes = change == Change::Extent   ? 24U
                                          : change == Change::Format ? 12U
                                                                     : 16U;
      CHECK(app.residency[3].source_payload_bytes == changed_bytes + 16U);
      CHECK(tfsupport::total_transmits(app.sink.frames[2]) == 1);
      CHECK(tfsupport::data_deletes_of(app.sink.frames[2], 272) == 1);
      CHECK(tfsupport::data_deletes_of(app.sink.frames[2], 271) == 0);
      CHECK(app.clean.submissions == 1);
      CHECK(app.clean.raw_calls == 1);
      CHECK(app.changed->submissions == 2);
      CHECK_FALSE(app.changed->dirty);
      CHECK(app.sink.frames[3].empty());
    }
}

TEST_CASE("omission after refused identity replacement retires restored root",
          "[pixelsurface][app][refusal][retirement][issue398]") {
  for (const auto mode : modes) {
    RecoveryApp app;
    app.change = Change::Extent;
    app.omit_at = 2; // destroy producer; cleanup must hold no Widget pointer
    app.run(mode);
    require_sink_error(app);
    CHECK_FALSE(app.changed);
    CHECK(app.residency[2].pinned_images == 2);
    CHECK(app.residency[3] == ImageResidency{0, 1, 16});
    CHECK(tfsupport::total_data_transmits(app.sink.frames[2]) == 0);
    CHECK(tfsupport::data_deletes_of(app.sink.frames[2], 272) == 1);
    CHECK(tfsupport::data_deletes_of(app.sink.frames[2], 271) == 0);
    CHECK(app.clean.submissions == 1);
    CHECK(app.clean.raw_calls == 1);
    CHECK(app.sink.frames[3].empty());
  }
}

TEST_CASE("initial refused uploads retry only from widget-owned payload",
          "[pixelsurface][app][refusal][initial][issue398]") {
  for (const auto mode : modes)
    for (bool opaque : {false, true}) {
      RecoveryApp app;
      app.change = Change::None;
      app.sink.refused = {0};
      if (opaque) {
        app.changed->update(Payload::Png);
        app.replies.emplace_back(2, "\033_Gi=272;OK\033\\");
      }
      app.run(mode, 5);
      require_sink_error(app);
      REQUIRE(app.changed);
      CHECK(app.residency[1] == ImageResidency{});
      CHECK(app.dirty[1]);
      CHECK(app.acknowledgements[1] == 0);
      CHECK(app.changed->submissions == 1);
      CHECK(app.clean.submissions == 1);
      CHECK(tfsupport::total_transmits(app.sink.frames[1]) == 2);
      CHECK(app.residency[4].pinned_images == 2);
      CHECK(app.sink.frames[4].empty());
    }
}

TEST_CASE("refused opaque replacement awaits the retry's accepted reply",
          "[pixelsurface][app][refusal][opaque][issue398]") {
  for (const auto mode : modes) {
    RecoveryApp app;
    app.changed->update(Payload::Png);
    app.mutate_at = 2;
    app.sink.refused = {2};
    app.replies.emplace_back(1, "\033_Gi=272;OK\033\\");
    app.replies.emplace_back(4, "\033_Gi=272;OK\033\\");
    app.run(mode, 6);
    require_sink_error(app);
    REQUIRE(app.changed);
    CHECK(app.acknowledgements[3] == 1);
    CHECK(app.dirty[3]);
    CHECK(app.changed->submissions == 2);
    CHECK_FALSE(app.changed->dirty);
    CHECK(app.clean.submissions == 1);
    CHECK(app.clean.raw_calls == 1);
    CHECK(tfsupport::total_transmits(app.sink.frames[3]) == 0);
    CHECK(tfsupport::frame_updates_of(app.sink.frames[3], 272) == 1);
    CHECK(app.residency[5] == ImageResidency{0, 2, 23});
    CHECK(app.sink.frames[5].empty());
  }
}

TEST_CASE("repeated refused edits preserve committed roots and dirty work",
          "[pixelsurface][app][refusal][issue398]") {
  for (const auto mode : modes) {
    RecoveryApp app;
    app.sink.refused = {1, 2};
    app.run(mode, 5);
    require_sink_error(app, 2);
    REQUIRE(app.changed);
    CHECK(app.dirty[2]);
    CHECK(app.dirty[3]);
    CHECK(app.acknowledgements[3] == 1);
    CHECK(app.changed->submissions == 2);
    CHECK(app.clean.submissions == 1);
    CHECK(app.clean.raw_calls == 1);
    for (std::size_t i : {2U, 3U}) {
      CHECK(tfsupport::total_transmits(app.sink.frames[i]) == 0);
      CHECK(tfsupport::frame_updates_of(app.sink.frames[i], 272) == 1);
      CHECK(app.residency[i] == ImageResidency{0, 2, 32});
    }
    CHECK(app.residency[4] == ImageResidency{0, 2, 32});
    CHECK(app.sink.frames[4].empty());
  }
}

TEST_CASE("an older opaque OK cannot acknowledge newer refused source content",
          "[pixelsurface][app][refusal][opaque][revision][issue398]") {
  for (const auto mode : modes) {
    RecoveryApp app;
    app.changed->update(Payload::Png);
    app.replies.emplace_back(1, "\033_Gi=272;OK\033\\");
    app.replies.emplace_back(3, "\033_Gi=272;OK\033\\");
    app.run(mode, 5);
    require_sink_error(app);
    REQUIRE(app.changed);
    // Mutation precedes App's initial-reply reconciliation in collect. The
    // old revision must not acknowledge the newer dirty source.
    CHECK(app.acknowledgements[2] == 0);
    CHECK(app.dirty[2]);
    CHECK(app.changed->submissions == 1);
    CHECK_FALSE(app.changed->dirty);
    CHECK(app.clean.submissions == 1);
    CHECK(tfsupport::total_transmits(app.sink.frames[2]) == 0);
    CHECK(tfsupport::frame_updates_of(app.sink.frames[2], 272) == 1);
    CHECK(app.residency[4] == ImageResidency{0, 2, 23});
    CHECK(app.sink.frames[4].empty());
  }
}

TEST_CASE("repeated collection keeps the frame's original handle checkpoint",
          "[pixelsurface][app][refusal][identity][issue398]") {
  for (const auto mode : modes) {
    RecoveryApp app;
    app.change = Change::Extent;
    app.repeated_collection = true;
    app.run(mode);
    require_sink_error(app);
    REQUIRE(app.changed);
    CHECK(app.changed->submissions == 2);
    CHECK(app.clean.submissions == 1);
    CHECK(app.residency[3] == ImageResidency{0, 2, 40});
    CHECK(tfsupport::data_deletes_of(app.sink.frames[2], 272) == 1);
    CHECK(tfsupport::total_transmits(app.sink.frames[2]) == 1);
    CHECK(app.sink.frames[3].empty());
  }
}

namespace {
// An older resident driver: hashes advance before the sink write and inherited
// retain_pinned delegates to draw_pinned. It deliberately does not know #398.
class LegacyResidentDriver final : public TerminalDriver {
 public:
  int pins{0}, replacements{0}, retirements{0};
  auto init() -> std::expected<void, ErrorEvent> override { return {}; }
  auto draw_text(int, int, std::string_view, Rgb, Rgb, Attr) -> void override {}
  auto draw_image(Rect, const Image&)
      -> std::expected<void, ErrorEvent> override {
    return std::unexpected{
        ErrorEvent{Severity::Warning, "legacy", "no immediate images"}};
  }
  auto preferred_pixel_extent(Rect cells) const noexcept -> Extent override {
    return {cells.w, cells.h};
  }
  auto capabilities() const noexcept -> Capabilities override {
    return {.kitty_graphics = true, .truecolor = true};
  }
  auto max_pinned_images() const noexcept -> std::size_t override { return 8; }
  auto pin_image(const Image& image)
      -> std::expected<PinnedImage, ErrorEvent> override {
    const auto id = ++m_id;
    m_roots.emplace(id, image.at(0, 0));
    ++pins;
    m_buffer += 'T';
    tally_image_transmit(1);
    return PinnedImage{id, instance_token(), id};
  }
  auto replace_pinned(PinnedImage pin, const Image& image)
      -> std::expected<void, ErrorEvent> override {
    auto& pixel = m_roots.at(pin.id);
    if (pixel == image.at(0, 0)) return {};
    pixel = image.at(0, 0); // deliberately not rolled back after refusal
    ++replacements;
    append_edit('F');
    return {};
  }
  auto unpin_image(PinnedImage pin)
      -> std::expected<void, ErrorEvent> override {
    m_roots.erase(pin.id);
    ++retirements;
    append_edit('D');
    return {};
  }
  auto draw_pinned(Rect, PinnedImage, PlacementFit)
      -> std::expected<void, ErrorEvent> override {
    append_edit('P');
    return {};
  }
  auto flush() -> void override {
    (void)emit_frame(m_buffer);
    m_buffer.clear();
  }

 private:
  auto append_edit(char command) -> void {
    m_buffer += command;
    tally_image_edit(1);
  }
  std::uint32_t m_id{0};
  std::unordered_map<std::uint32_t, Pixel> m_roots;
  std::string m_buffer;
};
} // namespace

TEST_CASE("legacy resident drivers retain conservative refused-write recovery",
          "[pixelsurface][app][refusal][legacy][issue398]") {
  RecoveryApp app;
  auto selected = std::make_unique<LegacyResidentDriver>();
  auto* legacy = selected.get();
  CHECK_FALSE(legacy->supports_pinned_image_rollback());
  app.run_with(std::move(selected));
  require_sink_error(app);
  REQUIRE(app.changed);
  CHECK(legacy->pins == 4);
  CHECK(legacy->replacements == 1);
  CHECK(legacy->retirements == 2);
  CHECK(app.changed->submissions == 2);
  CHECK(app.clean.submissions == 2);
  CHECK(app.clean.raw_calls == 2);
  CHECK_FALSE(app.changed->dirty);
  CHECK_FALSE(app.clean.dirty);
}
