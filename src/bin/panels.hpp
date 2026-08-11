#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "proc_reader.hpp"
#include "termforge/widgets/dialog.hpp"
#include "termforge/widgets/frame.hpp"
#include "termforge/widgets/pixel_surface.hpp"
#include "termforge/widgets/progress_bar.hpp"
#include "termforge/widgets/table_widget.hpp"
#include "termforge/widgets/text_input.hpp"
#include "termforge/widgets/waveform_widget.hpp"

namespace termforge::forge_top {

class CpuPanel final : public Widget {
public:
  CpuPanel();
  auto set_style(BorderStyle style) -> void;
  auto set_samples(std::span<const CpuSample> samples) -> void;
  auto draw(Screen &screen) -> void override;
  auto pixel_regions() -> std::vector<Rect> override;
  auto draw_pixels(Rect region, Extent preferred) -> const Image * override;

private:
  Frame m_frame{"CPU cores"};
  std::vector<std::string> m_names;
  std::vector<float> m_usage;
  std::vector<std::unique_ptr<WaveformWidget>> m_waves;
};

class MemoryPanel final : public Widget {
public:
  MemoryPanel();
  auto set_style(BorderStyle style) -> void;
  auto set_memory(const MemoryInfo &memory) -> void;
  auto draw(Screen &screen) -> void override;

private:
  Frame m_frame{"Memory"};
  ProgressBar m_memory;
  ProgressBar m_swap;
};

enum class ProcessSort { Cpu, Memory, Pid, Name };

class ProcessPanel final : public Widget {
public:
  ProcessPanel();

  auto set_processes(std::vector<ProcessRow> processes) -> void;
  auto set_filter(std::string text) -> void;
  auto set_style(BorderStyle style) -> void;
  auto choose_sort(ProcessSort key) -> void;
  [[nodiscard]] auto sort_key() const noexcept -> ProcessSort { return m_sort; }
  [[nodiscard]] auto descending() const noexcept -> bool {
    return m_descending;
  }

  auto on_activate(std::function<void(const ProcessRow &)> callback) -> void;
  auto activate_selected() -> bool;
  auto handle_header_click(const MouseEvent &mouse) -> bool;

  [[nodiscard]] auto filter() -> TextInput & { return m_filter; }
  [[nodiscard]] auto table() -> TableWidget & { return m_table; }
  [[nodiscard]] auto visible_rows() const noexcept
      -> const std::vector<ProcessRow> & {
    return m_visible;
  }

  auto draw(Screen &screen) -> void override;

private:
  auto rebuild() -> void;
  auto selected_pid() const -> std::optional<int>;

  Frame m_frame{"Processes"};
  TextInput m_filter;
  TableWidget m_table;
  std::vector<ProcessRow> m_processes;
  std::vector<ProcessRow> m_visible;
  ProcessSort m_sort{ProcessSort::Cpu};
  bool m_descending{true};
  BorderStyle m_style{BorderStyle::Rounded};
  std::function<void(const ProcessRow &)> m_on_activate;
};

class DetailPopup final : public Dialog {
public:
  DetailPopup();
  auto set_style(BorderStyle style) -> void { set_border_style(style); }
  auto set_process(const ProcessRow &process, std::span<const float> history)
      -> void;
  [[nodiscard]] auto pid() const noexcept -> int { return m_pid; }

  auto pixel_regions() -> std::vector<Rect> override;
  auto draw_pixels(Rect region, Extent preferred) -> const Image * override;
  [[nodiscard]] auto pixel_region_state(Rect region) const noexcept
      -> PixelRegionState override;
  auto pixel_region_submitted(Rect region) noexcept -> void override;
  [[nodiscard]] auto pixel_fit(Rect region) const noexcept
      -> PlacementFit override;

protected:
  [[nodiscard]] auto content_rows() const -> int override { return 10; }
  [[nodiscard]] auto content_cols() const -> int override { return 48; }
  auto layout_content(Rect area) -> void override;
  auto draw_content(Screen &screen) -> void override;

private:
  auto rasterize(std::span<const float> history) -> void;

  PixelSurface m_graph{{160, 64}};
  int m_pid{-1};
};

} // namespace termforge::forge_top
