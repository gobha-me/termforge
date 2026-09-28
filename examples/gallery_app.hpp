#pragma once

// Private example composition, not an installed TermForge API. All page and
// widget storage belongs to GalleryApp; Notebook only borrows its page roots.
#include <array>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/widgets/button.hpp"
#include "termforge/widgets/checkbox.hpp"
#include "termforge/widgets/choice_dialog.hpp"
#include "termforge/widgets/choice_wizard_dialog.hpp"
#include "termforge/widgets/composer.hpp"
#include "termforge/widgets/dialogs.hpp"
#include "termforge/widgets/file_picker_dialog.hpp"
#include "termforge/widgets/frame.hpp"
#include "termforge/widgets/label.hpp"
#include "termforge/widgets/list_widget.hpp"
#include "termforge/widgets/map_widget.hpp"
#include "termforge/widgets/menu_bar.hpp"
#include "termforge/widgets/notebook.hpp"
#include "termforge/widgets/pixel_surface.hpp"
#include "termforge/widgets/progress_bar.hpp"
#include "termforge/widgets/radio_group.hpp"
#include "termforge/widgets/select.hpp"
#include "termforge/widgets/table_widget.hpp"
#include "termforge/widgets/text_box.hpp"
#include "termforge/widgets/text_input.hpp"
#include "termforge/widgets/waveform_widget.hpp"

namespace termforge::examples {

struct GalleryCard {
  std::string title, help, example;
  Widget* widget{};
  bool interactive{true};
};

class GalleryPage final : public Widget {
 public:
  std::vector<GalleryCard> cards;
  bool ascii{true}, enabled{true};
  std::string notice;
  auto select(int index) -> void;
  [[nodiscard]] auto selected() const -> int { return m_selected; }
  [[nodiscard]] auto card() const -> const GalleryCard&;
  [[nodiscard]] auto body() const -> Rect { return m_body; }
  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& event) -> bool override;
  auto on_tick(std::chrono::duration<double> dt) -> void override;
  auto set_focused(bool focused) -> void override;
  auto reset_transient() -> void override;
  [[nodiscard]] auto focusable() const -> bool override;
  [[nodiscard]] auto hit_test_tree(int x, int y) const -> bool override;
  auto pixel_children() -> std::vector<Widget*> override;

 private:
  Frame m_frame;
  Label m_title, m_help, m_reference;
  Rect m_body{}, m_previous{}, m_next{};
  int m_selected{0};
};

// ASCII projections are example policy. The underlying widgets still own
// their data, tick state and enhanced pixel buffers; no library seam is added.
class GallerySignal final : public Widget {
 public:
  WaveformWidget wave{64};
  bool ascii{true};
  auto push(float sample) -> void;
  auto draw(Screen& screen) -> void override;
  auto pixel_children() -> std::vector<Widget*> override;

 private:
  std::deque<float> m_samples;
};

class GalleryProgress final : public Widget {
 public:
  ProgressBar bar;
  bool ascii{true};
  auto draw(Screen& screen) -> void override;
  auto on_tick(std::chrono::duration<double> dt) -> void override {
    bar.on_tick(dt);
  }
};

class GalleryApp : public App {
 public:
  explicit GalleryApp(std::filesystem::path browse = ".");
  auto on_render(Screen& screen) -> void override;
  auto on_event(const Event& event) -> void override;
  auto on_tick(std::chrono::duration<double> dt) -> void override;
  [[nodiscard]] auto category() const -> int { return m_book.active(); }
  [[nodiscard]] auto specimen() const -> int;
  [[nodiscard]] auto specimen_rect() const -> Rect;
  [[nodiscard]] auto status() const -> const std::string& { return m_result; }
  [[nodiscard]] auto draft() const -> const std::string& {
    return m_input.text();
  }
  [[nodiscard]] auto composer_text() const -> const std::string& {
    return m_composer.text();
  }
  [[nodiscard]] auto counter() const -> int { return m_counter; }
  [[nodiscard]] auto checked() const -> bool { return m_check.checked(); }
  [[nodiscard]] auto ascii() const -> bool { return m_ascii; }
  [[nodiscard]] auto pixel_submissions() const -> std::uint64_t {
    return m_pixels.submission_count();
  }

 private:
  auto page() -> GalleryPage&;
  auto reset_demo() -> void;
  auto apply_state(int state) -> void;
  auto apply_style(bool ascii) -> void;
  auto show(Dialog& dialog) -> void;
  auto return_to_book() -> void;

  TextInput m_input;
  Button m_increment{"[ Increment counter ]"};
  Checkbox m_check{"Enable demo notifications"};
  RadioGroup m_radio;
  Select m_select;
  GalleryProgress m_progress;
  Label m_unicode;
  ListWidget m_list;
  TableWidget m_table;
  GallerySignal m_signal;
  MapWidget m_map;
  TextBox m_transcript;
  Composer m_composer;
  TextBox m_blocks;
  PixelSurface m_pixels{{32, 16}};
  std::array<Button, 6> m_dialog_buttons;
  MessageDialog m_message{"Message", "A real modal with a real result."};
  ConfirmDialog m_confirm{"Clear draft?", "Clear the Composer demo draft?"};
  PromptDialog m_prompt{"Rename demo", "Enter a new TextInput value:"};
  ChoiceDialog m_choice;
  ChoiceWizardDialog m_wizard;
  FilePickerDialog m_picker{"Select path (read-only)"};
  MessageDialog m_help_dialog;
  std::array<GalleryPage, 5> m_pages;
  Notebook m_book;
  MenuBar m_menu;
  Label m_header, m_tier, m_status, m_help;
  Frame m_sidebar_frame{"Explore"};
  TextBox m_sidebar;
  TextEntryHandle m_stream;
  std::string m_result{
      "Ready. Data is simulated; controls and results are real."};
  std::optional<bool> m_ascii_override, m_style_applied;
  bool m_ascii{true}, m_menu_focused{false}, m_usable{true};
  int m_counter{0}, m_state{0}, m_stream_chunks{0};
  double m_elapsed{0}, m_stream_elapsed{0};
};

} // namespace termforge::examples
