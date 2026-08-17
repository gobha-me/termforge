// test/69frameobserver — App's production frame cadence for #258.
//
// These cases enter the same frame_step() as run().  That matters here twice:
// the observation belongs after the caller's one flush, and a demand-idle
// iteration has no flush and therefore no observation.  Replaying driver calls
// in a different order would prove neither contract.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"

using namespace termforge;
using namespace std::chrono_literals;

namespace {

auto burn_for(std::chrono::steady_clock::duration duration) -> void {
  const auto until = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < until) {
  }
}

class ProbeSink final : public ByteSink {
 public:
  std::chrono::steady_clock::duration delay{};
  bool fail_first{false};
  std::vector<std::size_t> writes;

  [[nodiscard]] auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    if (delay > std::chrono::steady_clock::duration::zero()) burn_for(delay);
    writes.push_back(bytes.size());
    if (fail_first && writes.size() == 1) {
      return std::unexpected{ErrorEvent{Severity::Warning, "observer-test",
                                        "first frame refused"}};
    }
    return {};
  }
};

class ObservationProbe : public App {
 public:
  explicit ObservationProbe(ProbeSink* sink = nullptr, bool observe = true)
      : m_sink{sink} {
    set_frame_ms(0);
    if (observe) {
      set_frame_observer([this](const FrameObservation& frame) {
        observations.push_back(frame);
      });
    }
  }

  std::chrono::steady_clock::duration tick_work{};
  std::chrono::steady_clock::duration render_work{};
  bool mutate_each_frame{true};
  std::vector<FrameObservation> observations;
  std::vector<ErrorEvent> errors;

  auto run_frames(int count) -> void {
    test_run_frames(count, 12, 4, static_cast<std::string *>(nullptr));
  }

  auto on_event(const Event& event) -> void override {
    if (const auto* error = std::get_if<ErrorEvent>(&event))
      errors.push_back(*error);
  }

  auto on_tick(std::chrono::duration<double>) -> void override {
    if (tick_work > std::chrono::steady_clock::duration::zero())
      burn_for(tick_work);
  }

  auto on_render(Screen& screen) -> void override {
    if (m_sink != nullptr && !m_bound) {
      driver().set_output(m_sink);
      m_bound = true;
    }
    if (render_work > std::chrono::steady_clock::duration::zero())
      burn_for(render_work);
    screen.clear();
    screen.write_text(0, 0, mutate_each_frame ? std::to_string(m_frame) : "x",
                      {}, {}, Attr::None);
    ++m_frame;
  }

 protected:
  auto wait_readable(int) -> bool override { return false; }
  auto read_available(char*, int) -> int override { return 0; }

 private:
  ProbeSink* m_sink{};
  int m_frame{};
  bool m_bound{false};
};

class ThrowingObserverProbe final : public App {
 public:
  ThrowingObserverProbe() {
    set_frame_ms(0);
    set_frame_observer([](const FrameObservation&) {
      throw std::runtime_error{"observer failed"};
    });
  }

  int stops{};

  auto run_guarded() -> int { return test_run_guarded(8, 3, &m_wire); }
  auto on_render(Screen& screen) -> void override {
    screen.write_text(0, 0, "frame", {}, {}, Attr::None);
  }
  auto on_stop() noexcept -> void override { ++stops; }

 protected:
  auto wait_readable(int) -> bool override { return false; }
  auto read_available(char*, int) -> int override { return 0; }

 private:
  std::string m_wire;
};

}  // namespace

TEST_CASE("frame observer: every rendered write reports its exact meter",
          "[frameobserver]") {
  ProbeSink sink;
  ObservationProbe app{&sink};
  app.mutate_each_frame = false;

  REQUIRE(app.has_frame_observer());
  app.run_frames(2);

  REQUIRE(app.observations.size() == 2);
  REQUIRE(sink.writes.size() == 2);
  CHECK(app.observations[0].bytes.total() == sink.writes[0]);
  CHECK(app.observations[0].bytes.total() > 0);
  // flush() still hands the sink one empty span: it is a real rendered frame
  // boundary even though the diff has no bytes.
  CHECK(app.observations[1].bytes.total() == 0);
  CHECK(sink.writes[1] == 0);
  CHECK(app.observations[0].output_accepted);
  CHECK(app.observations[1].output_accepted);
}

TEST_CASE("frame observer: tick, render and sink work stay in their partitions",
          "[frameobserver][timing]") {
  ProbeSink sink;
  sink.delay = 500us;
  ObservationProbe app{&sink};
  app.tick_work = 500us;
  app.render_work = 500us;

  app.run_frames(1);

  REQUIRE(app.observations.size() == 1);
  const auto& frame = app.observations.front();
  // These are deliberately broad lower bounds, not performance gates.  Each
  // phase burns until the same steady clock has advanced twice this amount;
  // removing or moving a phase stamp makes the corresponding assertion fail.
  CHECK(frame.tick >= 250us);
  CHECK(frame.application_render >= 250us);
  CHECK(frame.sink_write >= 250us);
  CHECK(frame.framework_submission >= 0ns);
}

TEST_CASE("frame observer: timing is independent of synthetic application time",
          "[frameobserver][timing][clock]") {
  SyntheticClock clock;
  ProbeSink sink;
  ObservationProbe app{&sink};
  app.set_clock(&clock);
  app.render_work = 500us;

  app.run_frames(1);

  REQUIRE(clock.now() == std::chrono::steady_clock::time_point{});
  REQUIRE(app.observations.size() == 1);
  CHECK(app.observations.front().application_render >= 250us);
}

TEST_CASE(
    "frame observer: a refused write is metered and reported before retry",
    "[frameobserver][failure]") {
  ProbeSink sink;
  sink.fail_first = true;
  ObservationProbe app{&sink};

  app.run_frames(2);

  REQUIRE(app.observations.size() == 2);
  CHECK_FALSE(app.observations[0].output_accepted);
  CHECK(app.observations[0].bytes.total() == sink.writes[0]);
  CHECK(app.observations[1].output_accepted);
  REQUIRE(app.errors.size() == 1);
  CHECK(app.errors.front().source == "observer-test");
  CHECK(app.errors.front().message == "first frame refused");
}

TEST_CASE("frame observer: demand-idle iterations have no stale observation",
          "[frameobserver][demand]") {
  ProbeSink sink;
  ObservationProbe app{&sink};
  app.set_render_mode(RenderMode::Demand);

  app.run_frames(3);

  REQUIRE(app.observations.size() == 1);
  REQUIRE(sink.writes.size() == 1);
}

TEST_CASE("frame observer: live clearing cannot destroy the running callback",
          "[frameobserver][lifetime]") {
  ProbeSink sink;
  ObservationProbe app{&sink, false};
  app.set_frame_observer([&](const FrameObservation& frame) {
    app.observations.push_back(frame);
    app.clear_frame_observer();
  });

  app.run_frames(2);

  CHECK(app.has_frame_observer());
  CHECK(app.observations.size() == 2);
  app.clear_frame_observer();
  CHECK_FALSE(app.has_frame_observer());
}

TEST_CASE("frame observer: callback exceptions take the guarded teardown path",
          "[frameobserver][failure]") {
  ThrowingObserverProbe app;

  REQUIRE_THROWS_AS(app.run_guarded(), std::runtime_error);
  CHECK(app.stops == 1);
}
