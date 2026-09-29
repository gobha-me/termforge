// Out-of-tree consumer smoke test (issue #27).
//
// Includes public headers by the <termforge/...> spelling the library
// advertises, and calls into two different library TUs (core/screen.cpp and
// widgets/label.cpp) so that a broken include path fails to compile and a
// broken export fails to link. It also compiles a consumer-owned App subclass
// against the three protected loop-source seams (#118) and the concrete
// synthetic-clock API layered over them (#119). Exits non-zero if the drawn
// cell is blank, which would mean we linked something that does not actually
// work.

#include <array>
#include <chrono>
#include <sstream>
#include <utility>

#include <termforge/core/app.hpp>
#include <termforge/core/screen.hpp>
#include <termforge/core/styled_text.hpp>
#include <termforge/drivers/fallback_driver.hpp>
#include <termforge/drivers/kitty_driver.hpp>
#include <termforge/widgets/dialogs.hpp>
#include <termforge/widgets/label.hpp>
#include <termforge/widgets/layout.hpp>
#include <termforge/widgets/list_widget.hpp>
#include <termforge/widgets/notebook.hpp>
#include <termforge/widgets/numeric_input.hpp>
#include <termforge/widgets/pixel_surface.hpp>
#include <termforge/widgets/slider.hpp>
#include <termforge/widgets/text_box.hpp>
#include <termforge/widgets/theme.hpp>

namespace {

class ScriptedApp final : public termforge::App {
 public:
  // Compile-only access proof. Merely overriding a private virtual is legal
  // C++, so the overrides below cannot distinguish private from protected.
  // These qualified base calls can: moving the seams back to private makes
  // both add_subdirectory and installed-package consumer builds fail.
  auto compile_base_defaults(char* out) -> void {
    (void)termforge::App::now_steady();
    (void)termforge::App::wait_readable(0);
    (void)termforge::App::read_available(out, 1);
  }

 protected:
  auto on_render(termforge::Screen&) -> void override {}

  [[nodiscard]] auto now_steady() const
      -> std::chrono::steady_clock::time_point override {
    return m_now;
  }

  auto wait_readable(int timeout_ms) -> bool override {
    m_now += std::chrono::milliseconds{timeout_ms};
    return false;
  }

  auto read_available(char*, int) -> int override { return 0; }

 private:
  std::chrono::steady_clock::time_point m_now{};
};

} // namespace

auto main() -> int {
  constexpr std::array tracks{termforge::LayoutTrack{},
                              termforge::LayoutTrack{}};
  const auto panes = termforge::layout_row({0, 0, 5, 1}, tracks);
  if (!panes || (*panes)[0] != termforge::Rect{0, 0, 2, 1} ||
      (*panes)[1] != termforge::Rect{2, 0, 3, 1})
    return 1;
  termforge::KittyDriver resident;
  termforge::TerminalDriver& resident_base = resident;
  if (!resident_base.supports_pinned_image_rollback() ||
      termforge::FallbackDriver{}.supports_pinned_image_rollback())
    return 1;
  termforge::Theme presentation;
  presentation.content_fg = {1, 2, 3};
  termforge::Label themed{"snapshot"};
  themed.set_geometry({0, 0, 8, 1});
  themed.set_theme(presentation);
  presentation.content_fg = {4, 5, 6};
  termforge::Screen themed_screen{8, 1};
  themed.draw(themed_screen);
  if (themed_screen.at(0, 0).fg != termforge::Rgb{1, 2, 3}) return 1;
  themed.set_colors({7, 8, 9}, {});
  themed.set_theme(presentation);
  themed.draw(themed_screen);
  if (themed_screen.at(0, 0).fg != termforge::Rgb{7, 8, 9}) return 1;
  themed.clear_theme();
  if (themed.theme_snapshot()) return 1;
  termforge::PromptDialog prompt{"Prompt", "Body"};
  prompt.set_value("Draft");
  presentation.glyphs = termforge::BorderStyle::Ascii;
  presentation.surface_bg = {30, 31, 32};
  prompt.set_theme(presentation);
  termforge::Screen modal_screen{40, 12};
  prompt.draw(modal_screen);
  const auto modal = prompt.rect();
  if (modal_screen.text_at(modal.x, modal.y) != "+" ||
      modal_screen.at(modal.x, modal.y).bg != presentation.surface_bg ||
      prompt.value() != "Draft")
    return 1;
  termforge::Dialog& base = prompt;
  base.set_border_style(termforge::BorderStyle::Single);
  prompt.clear_theme();
  prompt.draw(modal_screen);
  if (modal_screen.text_at(modal.x, modal.y) != "┌" ||
      prompt.value() != "Draft")
    return 1;
  termforge::PixelSurface surface{{1, 1}, {80, 90, 100, 255}};
  surface.set_geometry({0, 0, 2, 1});
  surface.set_fit(termforge::PlacementFit::Exact);
  surface.pixel_region_submitted(surface.rect());
  surface.set_theme(presentation);
  termforge::Screen surface_screen{2, 1};
  surface.draw(surface_screen);
  if (surface.content_dirty() ||
      std::as_const(surface).image().at(0, 0) !=
          termforge::Pixel{80, 90, 100, 255} ||
      surface_screen.at(1, 0).bg != presentation.content_bg)
    return 1;
  termforge::NumericInput numeric;
  if (!numeric.configure(termforge::IntegerInputConfig{-10, 10, 0, 1}) ||
      !numeric.set_draft("+0007") || !numeric.commit())
    return 1;
  numeric.set_geometry({0, 0, 20, 3});
  termforge::Screen numeric_screen{20, 3};
  numeric.draw(numeric_screen);
  if (std::get<std::int64_t>(numeric.value()) != 7 ||
      numeric_screen.text_at(1, 1) != "7")
    return 1;
  if (!numeric.set_draft("bad") || numeric.commit() ||
      std::get<std::int64_t>(numeric.value()) != 7)
    return 1;
  termforge::Slider slider;
  if (!slider.configure({-10, 10, 0, 0.5}) || !slider.set_value(1.5)) return 1;
  slider.set_geometry({0, 0, 20, 2});
  termforge::Screen slider_screen(20, 2);
  slider.set_style(termforge::BorderStyle::Ascii);
  slider.draw(slider_screen);
  if (slider.value() != 1.5 || slider_screen.text_at(11, 1) != "#") return 1;
  termforge::Label notebook_page;
  termforge::Notebook notebook;
  if (!notebook.add_page("Page", &notebook_page) ||
      notebook.active_page() != &notebook_page)
    return 1;
  presentation.glyphs = termforge::BorderStyle::Ascii;
  notebook.set_theme(presentation);
  notebook.set_geometry({0, 0, 12, 3});
  termforge::Screen notebook_screen{12, 3};
  notebook.draw(notebook_screen);
  if (notebook_screen.text_at(0, 0) != "*" || notebook_page.theme_snapshot())
    return 1;
  termforge::ListWidget choices;
  choices.set_items({"First", "Second"});
  choices.set_geometry({0, 0, 12, 2});
  choices.set_theme(presentation);
  choices.draw(notebook_screen);
  if (notebook_screen.at(0, 0).bg != presentation.selection_bg ||
      notebook_screen.text_at(0, 0) != "*")
    return 1;
  termforge::SyntheticClock clock;
  clock.advance(std::chrono::duration<double>{0.25});
  ScriptedApp scripted;
  scripted.require(termforge::AppRequirements{
      .truecolor = true, .min_cols = 80, .min_rows = 24});
  if (!scripted.requirements().truecolor || !scripted.requirements_met())
    return 1;
  scripted.set_render_mode(termforge::RenderMode::Demand);
  if (scripted.render_mode() != termforge::RenderMode::Demand) return 1;
  scripted.request_render();
  scripted.set_clock(&clock);
  scripted.set_tick_hz(10);
  scripted.set_max_tick_dt(std::chrono::duration<double>::zero());
  scripted.set_clock(nullptr);
  std::ostringstream recorded;
  scripted.start_recording(recorded);
  scripted.stop_recording();
  std::istringstream invalid_trace;
  (void)scripted.play(invalid_trace); // compile/link the installed API

  termforge::Screen screen(20, 3); // core/screen.cpp

  const termforge::TextStyle style{
      termforge::Rgb{0x11, 0x22, 0x33}, {}, termforge::Attr::Underline};
  termforge::StyledText styled{{"styled", style}};
  if (screen.write_styled(0, 1, styled) != 6) return 1;

  termforge::TextBox text_box;
  text_box.set_geometry(termforge::Rect{0, 2, 20, 1});
  text_box.append(styled);
  text_box.draw(screen); // widgets/text_box.cpp
  text_box.set_theme(presentation);
  text_box.draw(screen);
  if (screen.at(0, 2).fg != style.fg || screen.at(0, 2).attrs != style.attrs)
    return 1;
  text_box.clear();
  text_box.append("Plain");
  text_box.draw(screen);
  if (screen.at(0, 2).fg != presentation.content_fg) return 1;
  text_box.clear_theme();
  text_box.draw(screen);
  if (screen.at(0, 2).fg != termforge::theme::kFg) return 1;
  // Restore the original styled fixture used by the downstream smoke checks.
  text_box.clear();
  text_box.append(styled);
  text_box.draw(screen);

  termforge::Label label{"consumed"};
  label.set_geometry(termforge::Rect{0, 0, 20, 1});
  label.draw(screen); // widgets/label.cpp

  if (screen.at(0, 0).blank() ||
      screen.at(0, 1).attrs != termforge::Attr::Underline ||
      screen.text_at(0, 2) != "s")
    return 1;

  // #354 must be available through installed/public headers and linked TUs,
  // not just an in-tree test that can see private layout internals.
  termforge::TextBox blocks;
  if (!blocks.set_block_limits({2, 8, 16})) return 1;
  const auto block = blocks.append_block(2, styled);
  if (!block) return 1;
  blocks.set_geometry({0, 0, 20, 3});
  blocks.draw(screen);
  const auto geometry = blocks.block_geometry(*block);
  if (!geometry ||
      geometry->state != termforge::TextBlockLayoutState::Visible ||
      geometry->visible != termforge::Rect{0, 0, 20, 2})
    return 1;
  if (!blocks.update_block(*block, 1, styled) ||
      !blocks.set_block_rows(*block, 0) || !blocks.remove_block(*block) ||
      blocks.block_geometry(*block))
    return 1;
  return 0;
}
