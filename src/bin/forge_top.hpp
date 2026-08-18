#pragma once

#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "panels.hpp"
#include "proc_reader.hpp"
#include "termforge/core/app.hpp"
#include "termforge/widgets/dialogs.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/menu_bar.hpp"

namespace termforge::forge_top {

using DriverChoice = BuiltinDriver;

class HelpPopup final : public Dialog {
 public:
  HelpPopup();
  auto on_event(const Event& event) -> bool override;
};

class ForgeTopApp final : public App {
 public:
  explicit ForgeTopApp(std::unique_ptr<SystemReader> reader);

  auto force_driver(DriverChoice choice) -> std::expected<void, ErrorEvent>;
  auto run_headless(int frames, int cols, int rows, std::string* sink,
                    DriverChoice choice) -> void;
  auto show_first_process_for_test() -> bool;
  [[nodiscard]] auto process_panel_for_test() const -> const ProcessPanel& {
    return m_processes;
  }
  [[nodiscard]] auto process_panel_for_test() -> ProcessPanel& {
    return m_processes;
  }
  [[nodiscard]] auto cpu_per_cpu_for_test() const noexcept -> bool {
    return m_cpu.per_cpu();
  }
  [[nodiscard]] auto sample_delay_for_test() const noexcept
      -> std::chrono::duration<double> {
    return m_sample_delay;
  }
  [[nodiscard]] auto section_state_for_test() const noexcept
      -> std::array<bool, 4> {
    return {m_show_overview, m_show_cpu, m_show_memory, m_show_processes};
  }

  auto on_event(const Event& event) -> void override;
  auto on_tick(std::chrono::duration<double> dt) -> void override;
  auto on_render(Screen& screen) -> void override;
  auto on_start() -> void override;

 private:
  enum class Preset { All, Processes, Summary };

  auto refresh() -> void;
  auto rebuild_focus() -> void;
  auto set_preset(Preset preset) -> void;
  auto show_delay_prompt() -> void;
  auto apply_delay(std::string value) -> void;
  auto handle_global_key(const KeyEvent& key) -> bool;
  auto open_detail(const ProcessRow& process) -> void;
  auto update_detail() -> void;
  auto show_help() -> void;
  auto set_status(std::string status) -> void;
  auto apply_style(bool ascii) -> void;

  std::unique_ptr<SystemReader> m_reader;
  OverviewPanel m_overview;
  CpuPanel m_cpu;
  MemoryPanel m_memory;
  ProcessPanel m_processes;
  MenuBar m_menu;
  FocusRing m_focus;
  DetailPopup m_detail;
  HelpPopup m_help;
  PromptDialog m_delay_prompt{"Sampling delay",
                              "Seconds between samples (0 = every frame):"};
  SystemSnapshot m_snapshot;
  std::unordered_map<int, std::vector<float>> m_history;
  std::chrono::duration<double> m_sample_elapsed{};
  std::chrono::duration<double> m_sample_delay{1.0};
  bool m_show_overview{true};
  bool m_show_cpu{true};
  bool m_show_memory{true};
  bool m_show_processes{true};
  std::string m_status{"Starting…"};
};

} // namespace termforge::forge_top
