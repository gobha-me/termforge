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

enum class DriverChoice { Automatic, Kitty, Ansi, Fallback };

class ForgeTopApp final : public App {
public:
  explicit ForgeTopApp(std::unique_ptr<SystemReader> reader);

  auto force_driver(DriverChoice choice) -> std::expected<void, ErrorEvent>;
  auto run_headless(int frames, int cols, int rows, std::string *sink,
                    DriverChoice choice) -> void;
  auto show_first_process_for_test() -> bool;

  auto on_event(const Event &event) -> void override;
  auto on_tick(std::chrono::duration<double> dt) -> void override;
  auto on_render(Screen &screen) -> void override;
  auto on_start() -> void override;

private:
  enum class View { All, Processes, Graphs };

  auto refresh() -> void;
  auto rebuild_focus() -> void;
  auto set_view(View view) -> void;
  auto open_detail(const ProcessRow &process) -> void;
  auto update_detail() -> void;
  auto show_help() -> void;
  auto set_status(std::string status) -> void;
  auto apply_style(bool ascii) -> void;

  std::unique_ptr<SystemReader> m_reader;
  CpuPanel m_cpu;
  MemoryPanel m_memory;
  ProcessPanel m_processes;
  MenuBar m_menu;
  FocusRing m_focus;
  DetailPopup m_detail;
  MessageDialog m_help;
  SystemSnapshot m_snapshot;
  std::unordered_map<int, std::vector<float>> m_history;
  std::chrono::duration<double> m_sample_elapsed{};
  View m_view{View::All};
  std::string m_status{"Starting…"};
};

} // namespace termforge::forge_top
