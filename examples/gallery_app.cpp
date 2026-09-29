#include "gallery_app.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

#include "termforge/widgets/theme.hpp"

namespace termforge::examples {
namespace {
constexpr Rgb kAccent{112, 190, 230};
constexpr Rgb kChrome{20, 26, 40};
constexpr std::array<const char*, 5> kStates{"populated", "empty", "loading",
                                             "error", "disabled"};

auto inherit(Widget& owned, const Widget& parent) -> void {
  if (parent.theme_snapshot())
    owned.set_theme(*parent.theme_snapshot());
  else
    owned.clear_theme();
}

auto diagnostic(const Theme& value, Severity severity) -> Rgb {
  switch (severity) {
    case Severity::Info: return value.info;
    case Severity::Warning: return value.warning;
    case Severity::Error: return value.error;
  }
  return value.error;
}

auto line(Screen& screen, Rect area, std::string text, Rgb color,
          Rgb background) -> void {
  Label label{std::move(text)};
  label.set_colors(color, background);
  label.set_geometry(area);
  label.draw(screen);
}
} // namespace

auto gallery_theme(GalleryPalette palette, bool ascii) -> Theme {
  Theme value;
  value.accent = kAccent;
  value.surface_bg = kChrome;
  if (palette == GalleryPalette::HighContrast) {
    value.content_fg = value.surface_fg = {255, 255, 255};
    value.content_bg = value.surface_bg = {0, 0, 0};
    value.focus_fg = value.selection_fg = {0, 0, 0};
    value.focus_bg = {255, 255, 0};
    value.selection_bg = {0, 255, 255};
    value.muted = {192, 192, 192};
    value.accent = {255, 255, 0};
    value.info = {0, 255, 255};
    value.warning = {255, 192, 0};
    value.error = {255, 96, 128};
  }
  value.glyphs = ascii ? BorderStyle::Ascii : BorderStyle::Rounded;
  return value;
}

auto GalleryHelp::on_theme_changed() -> void {
  Dialog::on_theme_changed();
  inherit_theme(m_document);
}

auto GalleryHelp::on_border_style_changed() -> void {
  if (border_style_overridden()) m_document.set_style(border_style());
}

GalleryHelp::GalleryHelp() {
  add_child(&m_document);
}

auto GalleryHelp::set_document(std::string_view text) -> void {
  m_document.clear();
  while (!text.empty()) {
    const auto newline = text.find('\n');
    m_document.append(std::string{text.substr(0, newline)});
    if (newline == std::string_view::npos) break;
    text.remove_prefix(newline + 1);
  }
}

auto GalleryHelp::layout_content(Rect area) -> void {
  m_document.set_geometry(area);
}

auto GalleryPage::on_theme_changed() -> void {
  for (Widget* owned : std::array<Widget*, 3>{&m_frame, &m_help, &m_reference})
    inherit(*owned, *this);
  // cards are borrowed: GalleryApp explicitly applies their snapshots.
}

auto GallerySignal::on_theme_changed() -> void {
  inherit(wave, *this);
}

auto GalleryProgress::on_theme_changed() -> void {
  inherit(bar, *this);
}

auto GalleryHelp::on_show() -> void {
  m_document.scroll(-std::numeric_limits<int>::max());
}

auto GalleryHelp::on_escape() -> void {
  if (begin_result()) close();
}

auto GalleryHelp::on_event(const Event& event) -> bool {
  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    if (key->action == KeyAction::Release) return true;
    switch (key->key) {
      case Key::F6: on_escape(); return true;
      case Key::Home: on_show(); return true;
      case Key::End: m_document.scroll_to_bottom(); return true;
      case Key::Up: m_document.scroll(-1); return true;
      case Key::Down: m_document.scroll(1); return true;
      default: break;
    }
  }
  return Dialog::on_event(event);
}

auto GalleryPage::card() const -> const GalleryCard& {
  return cards[static_cast<std::size_t>(m_selected)];
}

auto GalleryPage::select(int index) -> void {
  if (cards.empty()) return;
  const int count = static_cast<int>(cards.size());
  index = (index % count + count) % count;
  if (index == m_selected) return;
  card().widget->set_focused(false);
  card().widget->reset_transient();
  card().widget->set_geometry({});
  m_selected = index;
  card().widget->set_geometry(m_body);
  set_focused(focused());
}

auto GalleryPage::focusable() const -> bool {
  return !cards.empty() && enabled && card().interactive &&
         card().widget->focusable() && !m_body.empty();
}

auto GalleryPage::set_focused(bool focus) -> void {
  Widget::set_focused(focus);
  for (const auto& specimen : cards)
    specimen.widget->set_focused(focus && enabled && specimen.interactive &&
                                 specimen.widget == card().widget);
}

auto GalleryPage::reset_transient() -> void {
  for (const auto& specimen : cards)
    specimen.widget->reset_transient();
}

auto GalleryPage::draw(Screen& screen) -> void {
  const auto r = rect();
  const auto fg = theme_color(&Theme::content_fg, theme::kFg);
  const auto bg = theme_color(&Theme::content_bg, theme::kBg);
  const auto accent = theme_color(&Theme::accent, kAccent);
  const auto muted = theme_color(&Theme::muted, theme::kDim);
  screen.fill_rect(r.x, r.y, r.w, r.h, fg, bg);
  m_body = m_previous = m_next = {};
  for (const auto& specimen : cards)
    if (specimen.widget != card().widget) specimen.widget->set_geometry({});
  if (cards.empty()) return;
  if (r.empty()) {
    card().widget->set_geometry({});
    return;
  }
  Rect inner = r;
  // Short terminals spend their rows on the specimen, not decorative chrome.
  if (r.h >= 7 && r.w >= 30) {
    m_frame.set_title("Specimen");
    m_frame.set_geometry(r);
    m_frame.draw(screen);
    inner = m_frame.content_rect();
  }
  if (inner.empty()) return;
  if (inner.h >= 2) {
    m_previous = {inner.x, inner.y, 1, 1};
    m_next = {inner.x + inner.w - 1, inner.y, 1, 1};
    line(screen, {inner.x, inner.y, 1, 1}, "<", accent, bg);
    const auto title = enabled
                           ? std::format(" {}/{} {}", m_selected + 1,
                                         cards.size(), card().title)
                           : std::format("[disabled] {}/{} {}", m_selected + 1,
                                         cards.size(), card().title);
    line(screen, {inner.x + 1, inner.y, std::max(0, inner.w - 2), 1}, title,
         accent, bg);
    line(screen, m_next, ">", accent, bg);
    ++inner.y;
    --inner.h;
  }
  if (inner.h >= 6) {
    m_help.set_text(card().help);
    m_help.set_geometry({inner.x, inner.y, inner.w, 1});
    m_help.set_colors(muted, bg);
    m_help.draw(screen);
    inner.y += 2;
    inner.h -= 2;
    m_reference.set_text("Source: examples/" + card().example + ".cpp");
    m_reference.set_geometry({inner.x, inner.y + inner.h - 1, inner.w, 1});
    m_reference.set_colors(muted, bg);
    m_reference.draw(screen);
    --inner.h;
  }
  m_body = inner;
  // One-row controls get one row; viewport widgets use the available height.
  if (card().title == "TextInput" || card().title == "Button" ||
      card().title == "Checkbox" || card().title == "Select" ||
      card().title == "ProgressBar" || card().title == "Label" ||
      card().title == "Slider" || card().title == "NumericInput" ||
      card().example == "dialogs")
    m_body.h = std::min(1, m_body.h);
  card().widget->set_geometry(m_body);
  set_focused(focused());
  card().widget->draw(screen);
  if (!notice.empty() &&
      (card().title == "ListWidget" || card().title == "TableWidget")) {
    line(screen, m_body, notice,
         theme_snapshot() ? diagnostic(*theme_snapshot(), notice_severity)
                          : accent,
         bg);
    if (notice_severity != Severity::Info)
      for (int x = m_body.x; x < m_body.x + m_body.w; ++x)
        screen.at(x, m_body.y).attrs |= Attr::Bold;
  }
  if (!enabled)
    for (int y = m_body.y; y < m_body.y + m_body.h; ++y)
      for (int x = m_body.x; x < m_body.x + m_body.w; ++x)
        screen.at(x, y).attrs = screen.at(x, y).attrs | Attr::Dim;
}

auto GalleryPage::on_event(const Event& event) -> bool {
  if (cards.empty()) return false;
  if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
    if (mouse->pressed && mouse->button == 0) {
      if (m_previous.contains(mouse->x, mouse->y)) {
        select(m_selected - 1);
        return true;
      }
      if (m_next.contains(mouse->x, mouse->y)) {
        select(m_selected + 1);
        return true;
      }
    }
    if (!enabled || !card().interactive ||
        !card().widget->hit_test_tree(mouse->x, mouse->y)) {
      if (mouse->pressed) card().widget->reset_transient();
      return false;
    }
    return card().widget->on_event(event);
  }
  return enabled && card().interactive && !m_body.empty() &&
         card().widget->on_event(event);
}

auto GalleryPage::on_tick(std::chrono::duration<double> dt) -> void {
  if (!cards.empty()) card().widget->on_tick(dt);
}

auto GalleryPage::hit_test_tree(int x, int y) const -> bool {
  return hit_test(x, y) ||
         (!cards.empty() && enabled && card().widget->hit_test_tree(x, y));
}

auto GalleryPage::pixel_children() -> std::vector<Widget*> {
  if (ascii || !enabled || cards.empty() || m_body.empty()) return {};
  return {card().widget};
}

auto GallerySignal::push(float sample) -> void {
  (void)wave.push(sample);
  m_samples.push_back(sample);
  if (m_samples.size() > 64) m_samples.pop_front();
}

auto GallerySignal::draw(Screen& screen) -> void {
  wave.set_geometry(rect());
  if (!ascii) {
    wave.draw(screen);
    return;
  }
  const auto r = rect();
  if (r.empty()) return;
  const auto bg = theme_color(&Theme::content_bg, theme::kBg);
  screen.fill_rect(r.x, r.y, r.w, r.h,
                   theme_color(&Theme::content_fg, theme::kFg), bg);
  const auto count = std::min(m_samples.size(), static_cast<std::size_t>(r.w));
  for (std::size_t i = 0; i < count; ++i) {
    const float value = m_samples[m_samples.size() - count + i];
    const int height = std::clamp(static_cast<int>(value * r.h), 0, r.h);
    for (int y = r.y + r.h - height; y < r.y + r.h; ++y)
      screen.write_text(r.x + static_cast<int>(i), y, "#",
                        theme_color(&Theme::accent, kAccent), bg);
  }
}

auto GallerySignal::pixel_children() -> std::vector<Widget*> {
  return ascii ? std::vector<Widget*>{} : std::vector<Widget*>{&wave};
}

auto GalleryProgress::draw(Screen& screen) -> void {
  bar.set_geometry(rect());
  if (!ascii) {
    bar.draw(screen);
    return;
  }
  line(screen, rect(),
       bar.indeterminate() ? "[....] Loading (simulated)"
                           : std::format("[{}%] Demo task",
                                         static_cast<int>(bar.value() * 100)),
       theme_color(&Theme::accent, kAccent),
       theme_color(&Theme::content_bg, theme::kBg));
}

GalleryApp::GalleryApp(std::filesystem::path browse) {
  set_mouse_mode(MouseMode::Drag);
  m_pages[0].cards = {
      {"TextInput", "Type; Enter commits a result, not a file.", "forms",
       &m_input},
      {"Button", "Enter / Space / click increments the counter.",
       "widgets_reference", &m_increment},
      {"Checkbox", "Space / click toggles demo notifications.", "forms",
       &m_check},
      {"RadioGroup", "Arrows select one option; values survive tab switches.",
       "forms", &m_radio},
      {"Select", "Enter opens; arrows choose; Esc closes; Tab leaves.", "forms",
       &m_select},
      {"ProgressBar", "F4: populated / empty / loading / error / disabled.",
       "widgets_reference", &m_progress, false},
      {"Label", "Unicode data; F1 selects authored ASCII presentation.",
       "hello", &m_unicode, false},
      {"Slider", "Arrows / Home / End adjust; drag and Esc cancel capture.",
       "slider", &m_slider},
      {"NumericInput",
       "Type an integer; Enter commits; Up/Down step; Esc restores.",
       "numeric_settings", &m_numeric}};
  m_pages[1].cards = {
      {"ListWidget", "Arrows / wheel scroll; Enter reports the selection.",
       "widgets_reference", &m_list},
      {"TableWidget", "Arrows / wheel navigate rows; Enter reports the row.",
       "dashboard", &m_table},
      {"WaveformWidget", "Simulated sine samples. ASCII bars remain complete.",
       "dashboard", &m_signal, false},
      {"MapWidget", "ASCII tiles are the complete map; sprites are optional.",
       "game", &m_map, false}};
  m_pages[2].cards = {
      {"TextBox stream",
       "F5 restarts a bounded simulated stream; PgUp inspects history.", "chat",
       &m_transcript},
      {"Composer",
       "Enter submits to TextBox; Alt+Enter newline; Up recalls history.",
       "chat", &m_composer},
      {"TextBox blocks", "A retained bounded block has authored text fallback.",
       "chat", &m_blocks}};
  m_pages[3].cards = {
      {"PixelSurface",
       "Owned 32x16 framebuffer; clean frames retain Kitty content.",
       "pixel_surface", &m_pixels, false}};
  constexpr std::array<const char*, 6> titles{
      "Message", "Confirm", "Prompt", "Choice", "Wizard", "FilePicker"};
  std::array<Dialog*, 6> dialogs{&m_message, &m_confirm, &m_prompt,
                                 &m_choice,  &m_wizard,  &m_picker};
  for (std::size_t i = 0; i < titles.size(); ++i) {
    m_dialog_buttons[i].set_label(std::string{"[ Show "} + titles[i] + " ]");
    m_dialog_buttons[i].on_activate(
        [this, dialog = dialogs[i]] { show(*dialog); });
    m_pages[4].cards.push_back(
        {titles[i],
         i == 5 ? "Choose a path only; no file is opened or written."
                : "Enter / click opens a real modal; Esc cancels.",
         "dialogs", &m_dialog_buttons[i]});
  }
  constexpr std::array<const char*, 5> categories{"Controls", "Data", "Text",
                                                  "Pixels", "Dialogs"};
  for (std::size_t i = 0; i < categories.size(); ++i)
    (void)m_book.add_page(categories[i], &m_pages[i]);
  m_book.set_focused(true);
  m_book.on_change([this](int) {
    m_result = "Category changed. F2/F3 browse; Tab enters the specimen.";
  });
  m_input.set_placeholder("Name this demo...");
  m_input.on_change([this](const std::string&) {
    m_result = "Draft changed; Enter commits.";
  });
  m_increment.on_activate([this] {
    if (m_counter == std::numeric_limits<int>::max()) {
      m_result = "Counter limit reached; Demo > Reset values starts again.";
      return;
    }
    ++m_counter;
    m_result = std::format("Counter: {}", m_counter);
  });
  m_check.on_change([this](bool value) {
    m_result = value ? "Demo notifications: on" : "Demo notifications: off";
  });
  m_radio.set_options(
      {"Compact", "Balanced", "Detailed with a deliberately long label"});
  m_radio.on_change(
      [this](int) { m_result = "Radio: " + m_radio.selected_text(); });
  m_select.set_options({"Local demo", "Simulated remote",
                        "Unicode: Grüße / 日本語",
                        "Long option kept reachable with arrows"});
  m_select.on_change(
      [this](int, const std::string& text) { m_result = "Select: " + text; });
  m_slider.set_label("Demo level");
  (void)m_slider.configure({0, 100, 40, 5});
  m_slider.on_change(
      [this](double value) { m_result = std::format("Slider: {}", value); });
  m_numeric.set_label("Demo count");
  (void)m_numeric.configure(IntegerInputConfig{0, 100, 12, 1});
  m_numeric.on_change([this](NumericValue value) {
    m_result = std::format("NumericInput: {}", std::get<std::int64_t>(value));
  });
  m_list.on_select(
      [this](int, const std::string& text) { m_result = "List: " + text; });
  m_table.on_select([this](int row, const std::vector<std::string>&) {
    m_result = std::format("Table row: {}", row + 1);
  });
  m_composer.set_max_height(6);
  m_composer.on_change([this](const std::string&) {
    m_result = "Composer draft changed; Enter sends to TextBox.";
  });
  m_blocks.append(
      "A block reserves document rows; it does not own child widgets.");
  const auto block = m_blocks.append_block(
      3, StyledText{{"+----------------------+\n| embedded block: demo "
                     "|\n+----------------------+",
                     {theme::kFg, theme::kBg}}});
  if (!block) m_result = block.error().message;
  m_blocks.append(
      "This following line survives scrolling and category switches.");
  m_map.set_map_size(16, 8);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 16; ++x)
      m_map.set_tile(0, x, y, (x == y || x + y == 15) ? 2 : 1);
  auto pixels = m_pixels.pixels();
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 32; ++x)
      pixels[static_cast<std::size_t>(y) * 32U + static_cast<std::size_t>(x)] =
          Pixel{static_cast<std::uint8_t>(x * 8),
                static_cast<std::uint8_t>(y * 16), 96, 255};
  for (auto* dialog : dialogs)
    dialog->on_close([this] { pop_overlay(); });
  m_message.on_ok([this] { m_result = "Message acknowledged."; });
  m_confirm.on_result([this](bool yes) {
    if (yes) m_composer.clear();
    m_result = yes ? "Composer draft cleared." : "Composer draft kept.";
  });
  m_prompt.on_submit([this](std::string value) {
    m_input.set_text(std::move(value));
    m_result = "TextInput renamed: " + m_input.text();
  });
  m_prompt.on_cancel([this] { m_result = "Prompt cancelled; draft kept."; });
  m_choice.set_title("Choose demo features");
  m_choice.set_choices({{"Mouse", "Pointer navigation"},
                        {"Motion", "Simulated live data"},
                        {"Detail", "More context"}});
  m_choice.set_mode(ChoiceMode::Multiple);
  m_choice.set_selected_indices({});
  m_choice.on_result([this](std::optional<ChoiceResult> result) {
    m_result = result ? std::format("Choice submitted: {} selections",
                                    result->selected_indices.size())
                      : "Choice cancelled.";
  });
  ChoiceWizardPage first, second;
  first.title = "Density";
  first.choices = {{"Compact", "Less chrome"}, {"Detailed", "More context"}};
  second.title = "Feedback";
  second.choices = {{"Quiet", "Minimal status"},
                    {"Verbose", "Detailed status"}};
  (void)m_wizard.set_pages({std::move(first), std::move(second)});
  m_wizard.set_title("Demo preferences");
  m_wizard.on_result([this](std::optional<ChoiceWizardResult> result) {
    m_result =
        result ? std::format("Wizard submitted: {} pages", result->pages.size())
               : "Wizard cancelled.";
  });
  m_picker.set_start_dir(std::move(browse));
  m_picker.on_result([this](std::optional<std::filesystem::path> path) {
    m_result = path ? "Path selected (not opened): " + path->string()
                    : "Path selection cancelled.";
  });
  m_picker.on_error_overlay([this](Dialog& error) {
    error.on_close([this] { pop_overlay(); });
    push_overlay(error);
  });
  m_picker.error_overlay_up([this] { return top_overlay() != &m_picker; });
  m_help_dialog.set_title("Widget lab / controls");
  m_help_dialog.set_document(
      "Esc/F6: back\nPgUp/PgDn, arrows, wheel: scroll\nHome/End: top/bottom\n\n"
      "Ctrl+Tab / Ctrl+Shift+Tab: category\nF2/F3: previous/next "
      "specimen\nTab: strip, specimen, menu; Shift+Tab: back\nF1: "
      "ASCII/enhanced presentation\nF4: cycle simulated data states\nF5: "
      "restart simulated stream\nF6: this help; F10: menu; Esc: quit\n\nMouse: "
      "category, specimen < >, controls and menu. Dropdown Escape closes "
      "before quitting.\n\nFocused sources: forms, widgets_reference, chat, "
      "dialogs, pixel_surface, dashboard, game, notebook. FilePicker selects a "
      "path only. No system clipboard or file writes.\n\n"
      "Choice/Wizard preferences are demos, not terminal capabilities.\n"
      "Compact wizard: < back, > next, OK submits, Esc cancels.\n\n"
      "View menu: Dark / High contrast palettes. F1 glyph choice is "
      "independent.\n\n"
      "End of help. Esc returns.");
  m_help_dialog.on_close([this] { pop_overlay(); });
  m_menu.add_menu(
      {"Demo",
       {{"Reset values", [this] { reset_demo(); }},
        {"Next simulated state", [this] { apply_state((m_state + 1) % 5); }},
        {"Restart simulated stream", [this] { apply_state(0); }},
        {"Quit", [this] { quit(); }}}});
  m_menu.add_menu(
      {"View",
       {{"Previous specimen", [this] { page().select(specimen() - 1); }},
        {"Next specimen", [this] { page().select(specimen() + 1); }},
        {"ASCII / enhanced", [this] { m_ascii_override = !m_ascii; }},
        {"Dark palette", [this] { set_palette(GalleryPalette::Dark); }},
        {"High contrast palette",
         [this] { set_palette(GalleryPalette::HighContrast); }}}});
  m_menu.add_menu(
      {"Help", {{"Controls and sources", [this] { show(m_help_dialog); }}}});
  for (const char* text :
       {"WIDGET LAB", "", "Ctrl+Tab: category", "F2 / F3: specimen",
        "Tab: edit / navigate", "F4: simulated state", "F6: full help",
        "View: palette", "", "FOCUSED EXAMPLES", "forms: form routing",
        "chat: retained text", "dialogs: modal stack", "pixel_surface: pixels",
        "notebook: page lifetime", "widgets_reference:",
        "  tabs, borders, ticks", "", "No clipboard / file writes."})
    m_sidebar.append(text);
  m_sidebar.set_retention({64, 8192});
  reset_demo();
}

auto GalleryApp::page() -> GalleryPage& {
  return m_pages[static_cast<std::size_t>(category())];
}
auto GalleryApp::specimen() const -> int {
  return m_pages[static_cast<std::size_t>(category())].selected();
}
auto GalleryApp::specimen_rect() const -> Rect {
  return m_usable ? m_pages[static_cast<std::size_t>(category())].body()
                  : Rect{};
}

auto GalleryApp::reset_demo() -> void {
  m_counter = 0;
  m_input.set_text("TermForge demo");
  m_check.set_checked(false);
  m_radio.set_selected(0);
  m_select.set_selected(0);
  (void)m_slider.set_value(40);
  (void)m_numeric.set_value(std::int64_t{12});
  m_composer.set_text("Edit this draft; Enter sends it to the transcript.");
  m_composer.clear_history();
  apply_state(0);
  m_result = "Demo reset. Data is simulated; control results are real.";
}

auto GalleryApp::apply_state(int state) -> void {
  m_state = state;
  if (state == 4) m_slider.stop_drag();
  m_result_severity = state == 3 ? Severity::Error : Severity::Info;
  m_pages[1].notice_severity = m_result_severity;
  m_pages[1].notice = state == 1   ? "No demo rows. F4 changes state."
                      : state == 2 ? "Loading demo rows... (simulated)"
                      : state == 3 ? "Simulated error. F4 changes state."
                                   : "";
  for (auto& category_page : m_pages)
    category_page.enabled = state != 4;
  m_list.clear();
  m_table.clear_rows();
  if (state == 0 || state == 4) {
    for (int i = 0; i < 24; ++i) {
      m_list.add_item(std::format(
          "Demo {:02}: retained selectable entry with a long label", i + 1));
      m_table.add_row({std::format("demo-{:02}", i + 1),
                       i % 3 ? "ready" : "waiting", std::to_string(i * 7)});
    }
    m_table.set_selected(0);
  }
  m_progress.bar.set_indeterminate(state == 2);
  if (state != 2) (void)m_progress.bar.set_value(state == 0 ? 0.65f : 0.0f);
  m_progress.bar.set_label(state == 2 ? "Loading (simulated)" : "Demo task");
  m_transcript.clear();
  m_transcript.append(std::string{"Demo data: "} +
                      kStates[static_cast<std::size_t>(state)]);
  m_stream = {};
  m_stream_elapsed = 0;
  m_stream_chunks = 0;
  if (state == 0) {
    m_transcript.append(
        "Retained history. PgUp inspects it without losing the draft.");
    m_stream = m_transcript.begin_entry("Simulated stream: ");
  } else if (state == 3) {
    m_transcript.append(
        "Simulated error: no network request was made. F4 retries the demo.");
  }
  m_result = std::string{"Demo data: "} +
             kStates[static_cast<std::size_t>(state)] + " (simulated).";
}

auto GalleryApp::apply_style(bool ascii) -> void {
  const auto value = gallery_theme(m_palette, ascii);
  if (m_theme_applied == value) return;
  const bool glyph_changed = !m_theme_applied || m_ascii != ascii;
  m_theme_applied = m_theme = value;
  m_ascii = ascii;
  for (auto& category_page : m_pages) {
    category_page.ascii = ascii;
    category_page.set_theme(value);
    for (const auto& card : category_page.cards)
      card.widget->set_theme(value);
  }
  for (Widget* widget :
       std::array<Widget*, 8>{&m_book, &m_menu, &m_sidebar, &m_sidebar_frame,
                              &m_header, &m_tier, &m_status, &m_help})
    widget->set_theme(value);
  // These descriptors are authored *by this app* as semantic roles. The
  // library never guesses whether arbitrary columns/tiles should recolor.
  m_table.set_columns(
      {{"Name", Align::Left, 0, value.accent, value.surface_bg},
       {"State", Align::Left, 0, value.accent, value.surface_bg},
       {"Count", Align::Right, 6, value.accent, value.surface_bg}});
  TileSet tiles;
  tiles.define(1, {".", value.muted, value.content_bg});
  tiles.define(2, {"#", value.accent, value.content_bg});
  m_map.set_tileset(std::move(tiles));
  m_signal.ascii = m_progress.ascii = ascii;
  m_unicode.set_text(ascii ? "ASCII: Gruesse / Nihongo / e + accent"
                           : "Unicode: Grüße / 日本語 / é");
  // Demo-owned option text has an authored ASCII version. User-entered text
  // is never transliterated or destroyed when changing presentation.
  if (glyph_changed && m_select.option_count() == 4) {
    const int selected = m_select.selected();
    m_select.set_options(
        {"Local demo", "Simulated remote",
         ascii ? "ASCII: Gruesse / Nihongo" : "Unicode: Grüße / 日本語",
         "Long option kept reachable with arrows"});
    m_select.set_selected(selected);
  }
  for (Dialog* dialog :
       std::array<Dialog*, 7>{&m_message, &m_confirm, &m_prompt, &m_choice,
                              &m_wizard, &m_picker, &m_help_dialog})
    dialog->set_theme(value);
}

auto GalleryApp::show(Dialog& dialog) -> void {
  if (&dialog != &m_help_dialog &&
      (screen().cols() < 24 || screen().rows() < 8)) {
    m_result = "Resize >=24x8 for dialogs; F6 help.";
    return;
  }
  // Modal input can swallow the release. End example-owned pointer capture
  // now, keeping the last chosen value rather than leaving a latent drag.
  m_slider.stop_drag();
  push_overlay(dialog);
  m_result = "Modal open; Esc cancels without quitting the gallery.";
}

auto GalleryApp::cancel_small_forms(int cols, int rows) -> void {
  if ((cols >= 24 && rows >= 8) || !top_overlay() ||
      top_overlay() == &m_help_dialog)
    return;
  // Shrinking cannot leave an invisible form active. Cancel before this
  // frame's input pump, so Enter cannot commit a now-hidden draft. A child's
  // transient state may consume Escape; dropping that overlay is still safe
  // for these example-owned dialogs and never commits a result.
  while (auto* dialog = top_overlay()) {
    (void)dialog->on_event(KeyEvent{.key = Key::Escape});
    if (top_overlay() == dialog) pop_overlay();
  }
  m_result = "Resize >=24x8 for dialogs; F6 help.";
}

auto GalleryApp::return_to_book() -> void {
  m_menu_focused = false;
  m_menu.set_focused(false);
  m_menu.close_dropdown();
  m_book.set_focused(true);
}

auto GalleryApp::on_render(Screen& screen) -> void {
  const int w = screen.cols(), h = screen.rows();
  const auto caps = driver().capabilities();
  apply_style(
      m_ascii_override.value_or(!caps.truecolor && !caps.kitty_graphics));
  screen.clear(m_theme.content_fg, m_theme.content_bg);
  // Three full labels on the wizard's final page need 32 columns including
  // borders. Its existing label API keeps every button visible at 24 columns.
  if (w < 32)
    m_wizard.set_labels("<", ">", "OK", "Esc");
  else
    m_wizard.set_labels("Back", "Next", "Submit", "Cancel");
  m_usable = w >= 12 && h >= 6;
  cancel_small_forms(w, h);
  if (!m_usable) {
    m_book.reset_transient();
    m_menu.close_dropdown();
    m_book.set_geometry({});
    m_book.draw(screen);
    m_menu.set_geometry({});
    line(screen, {0, 0, w, std::min(1, h)}, "Resize >=12x6", m_theme.warning,
         m_theme.content_bg);
    if (h > 1)
      line(screen, {0, 1, w, 1}, "F6 help; Esc quit", m_theme.content_fg,
           m_theme.content_bg);
    return;
  }
  const int sidebar = w >= 110 && h >= 20 ? 27 : 0;
  m_header.set_text("TERMFORGE / WIDGET LAB");
  m_header.set_colors(m_theme.accent, m_theme.surface_bg);
  const int tier_width = w >= 60 ? 29 : 0;
  m_header.set_geometry({0, 0, w - tier_width, 1});
  m_header.draw(screen);
  m_tier.set_text(
      std::format("{} | {} | {}",
                  caps.kitty_graphics ? "Kitty"
                  : caps.truecolor    ? "ANSI RGB"
                                      : "Baseline",
                  m_ascii ? "ASCII" : "enhanced",
                  m_palette == GalleryPalette::Dark ? "dark" : "HC"));
  m_tier.set_align(Label::Align::Right);
  m_tier.set_colors(m_theme.muted, m_theme.surface_bg);
  m_tier.set_geometry({w - tier_width, 0, tier_width, 1});
  m_tier.draw(screen);
  m_book.set_geometry({0, 2, w - sidebar, h - 4});
  m_book.draw(screen);
  if (!m_ascii && !m_menu.dropdown_open()) render_pixel_regions(m_book);
  if (sidebar) {
    m_sidebar_frame.set_geometry({w - sidebar, 2, sidebar, h - 4});
    m_sidebar_frame.draw(screen);
    m_sidebar.set_geometry(m_sidebar_frame.content_rect());
    m_sidebar.draw(screen);
  }
  const auto severity = m_state == 3                      ? Severity::Error
                        : m_result == m_diagnostic_result ? m_result_severity
                                                          : Severity::Info;
  const bool compact = w < 32 || h < 8;
  if (compact && m_state == 4)
    m_status.set_text("[disabled] " + m_result);
  else if (compact && severity != Severity::Info)
    m_status.set_text(
        std::string{severity == Severity::Error ? "[error] " : "[warning] "} +
        m_result);
  else
    m_status.set_text(w < 32 && m_result.starts_with("Resize >=24x8")
                          ? "Resize>=24x8"
                          : "Result: " + m_result);
  m_status.set_colors(diagnostic(m_theme, severity), m_theme.surface_bg);
  m_status.set_geometry({0, h - 2, w, 1});
  m_status.draw(screen);
  if (severity != Severity::Info)
    for (int x = 0; x < w; ++x)
      screen.at(x, h - 2).attrs |= Attr::Bold;
  const std::string focus = m_menu_focused     ? "menu"
                            : page().focused() ? page().card().title
                                               : "tabs";
  m_help.set_text(
      w < 60 ? "^Tab tabs F2/3 cards F6 ?"
             : "Tab edit | ^Tab tabs | F2/3 cards | F6 ? | Esc quit | Focus: " +
                   focus);
  m_help.set_colors(m_theme.muted, m_theme.surface_bg);
  m_help.set_geometry({0, h - 1, w, 1});
  m_help.draw(screen);
  m_menu.set_geometry({0, 1, w, 1});
  m_menu.draw(screen);
  // Dropdowns deliberately exceed their control rectangle and paint last.
  if (page().card().widget == &m_select && m_select.dropdown_open())
    m_select.draw(screen);
}

auto GalleryApp::on_event(const Event& event) -> void {
  if (!m_usable && !std::holds_alternative<ResizeEvent>(event) &&
      !std::holds_alternative<ErrorEvent>(event)) {
    if (const auto* key = std::get_if<KeyEvent>(&event);
        key && key->action != KeyAction::Release) {
      if (key->key == Key::F6)
        show(m_help_dialog);
      else
        App::on_event(event);
    }
    return;
  }
  if (std::holds_alternative<PasteEvent>(event) && m_input.focused() &&
      page().card().widget == &m_input) {
    m_result = "TextInput demo: type to edit; Composer supports paste.";
    return;
  }
  if (const auto* resized = std::get_if<ResizeEvent>(&event)) {
    m_slider.stop_drag();
    m_usable = resized->cols >= 12 && resized->rows >= 6;
    cancel_small_forms(resized->cols, resized->rows);
    return;
  }
  if (const auto* error = std::get_if<ErrorEvent>(&event)) {
    m_result = error->source + ": " + error->message;
    m_diagnostic_result = m_result;
    m_result_severity = error->severity;
    return;
  }
  if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
    // A dragging slider gets release/movement outside its current hit area.
    // This is example-owned routing, never a global framework capture.
    if (m_slider.dragging()) {
      (void)m_slider.on_event(event);
      return;
    }
    if (m_menu.dropdown_open() && m_menu.hit_test_tree(mouse->x, mouse->y)) {
      (void)m_menu.on_event(event);
      return;
    }
    if (mouse->pressed && m_menu.hit_test(mouse->x, mouse->y)) {
      m_menu_focused = true;
      m_book.set_focused(false);
      m_menu.set_focused(true);
      (void)m_menu.on_event(event);
      return;
    }
    if (mouse->pressed) return_to_book();
    (void)m_book.on_event(event);
    return;
  }
  const auto* key = std::get_if<KeyEvent>(&event);
  if (key && key->action == KeyAction::Release) return;
  if (m_menu.dropdown_open() && m_menu.on_event(event)) return;
  if (key) {
    switch (key->key) {
      case Key::F1: m_ascii_override = !m_ascii; return;
      case Key::F2: page().select(specimen() - 1); return;
      case Key::F3: page().select(specimen() + 1); return;
      case Key::F4: apply_state((m_state + 1) % 5); return;
      case Key::F5: apply_state(0); return;
      case Key::F6: show(m_help_dialog); return;
      case Key::F10:
        m_menu_focused = !m_menu_focused;
        m_book.set_focused(!m_menu_focused);
        m_menu.set_focused(m_menu_focused);
        return;
      default: break;
    }
  }
  if (m_menu_focused) {
    if (key && (key->key == Key::Tab || key->key == Key::Escape)) {
      return_to_book();
      return;
    }
    if (m_menu.on_event(event)) return;
  } else {
    if (m_book.on_event(event)) return;
    if (key && key->key == Key::Enter && page().enabled) {
      if (page().card().widget == &m_input && m_input.focused()) {
        m_result = "Committed demo name: " + m_input.text();
        return;
      }
      if (page().card().widget == &m_composer && m_composer.focused()) {
        const auto text = m_composer.text();
        m_composer.push_history(text);
        m_transcript.append(text);
        m_stream = {};
        m_composer.clear();
        m_result = "Composer sent to the retained TextBox (in memory).";
        return;
      }
    }
    if (key && key->key == Key::Tab && !key->ctrl) {
      m_book.set_focused(false);
      m_menu_focused = true;
      m_menu.set_focused(true);
      return;
    }
  }
  App::on_event(event);
}

auto GalleryApp::on_tick(std::chrono::duration<double> dt) -> void {
  m_elapsed = std::fmod(m_elapsed + dt.count(), 1000.0);
  m_signal.push(static_cast<float>(std::sin(m_elapsed * 1.5) * 0.45 + 0.5));
  m_book.on_tick(dt);
  // This bounded producer is app policy, independent of the active page.
  // A hidden transcript can finish streaming; hidden widgets are not ticked.
  if (m_state == 0 && m_stream && m_stream_chunks < 12) {
    m_stream_elapsed += dt.count();
    while (m_stream_elapsed >= 0.2 && m_stream_chunks < 12) {
      m_stream_elapsed -= 0.2;
      if (!m_transcript.append_to_entry(m_stream, "chunk ")) break;
      ++m_stream_chunks;
      if (m_stream_chunks == 12) {
        (void)m_transcript.finalize_entry(m_stream);
        m_stream = {};
      }
    }
  }
}

} // namespace termforge::examples
