// TermForge example: dashboard
//
// Demonstrates widget composition: TableWidget (system stats), WaveformWidget
// (live plot), and TextBox (event log). Shows the App event loop with live
// updates via set_cell and WaveformWidget::push.
//
// Keyboard: Up/Down/PgUp/PgDn scroll the focused widget, Tab switches focus,
// ESC quits.

#include <cmath>
#include <format>

#include "termforge/core/app.hpp"
#include "termforge/widgets/table_widget.hpp"
#include "termforge/widgets/text_box.hpp"
#include "termforge/widgets/waveform_widget.hpp"

using namespace termforge;

class DashboardApp final : public App {
 public:
  DashboardApp() : m_wave{512} {
    // Set up the stats table.
    m_table.set_columns({
        {"Metric", Align::Left, 16},
        {"Value", Align::Right, 12},
        {"Status", Align::Center, 10},
    });
    m_table.add_row({"CPU Usage", "23%", "OK"});
    m_table.add_row({"Memory", "4.2 GB", "OK"});
    m_table.add_row({"Disk I/O", "1.1 MB/s", "OK"});
    m_table.add_row({"Network RX", "847 KB/s", "OK"});
    m_table.add_row({"Network TX", "123 KB/s", "OK"});
    m_table.add_row({"Load Avg", "0.85", "OK"});
    m_table.add_row({"Processes", "142", "OK"});
    m_table.add_row({"Threads", "891", "OK"});
    m_table.add_row({"Uptime", "3d 7h", ""});
    m_table.add_row({"Temperature", "62°C", "WARN"});
    m_table.add_row({"Fan Speed", "2400 RPM", "OK"});
    m_table.add_row({"Voltage", "1.24V", "OK"});

    // Set up the event log.
    m_log.append("[12:00:01] System monitor started");
    m_log.append("[12:00:02] Connected to sensors");
    m_log.append("[12:00:03] All checks passed");
    m_log.append("[12:01:15] Temperature threshold: 70°C");
    m_log.append("[12:02:33] Disk usage above 80%");
    m_log.append("[12:03:01] Cleanup scheduled");
    m_log.append("[12:05:44] Network latency: 12ms");
    m_log.append("[12:10:02] Backup completed");
    m_log.append("[12:15:30] Update available: v2.4.1");
    m_log.append("[12:20:18] Memory usage normal");
  }

  auto on_event(const Event& ev) -> void override {
    bool consumed = false;
    if (m_focus == 0) {
      consumed = m_table.on_event(ev);
    } else {
      consumed = m_log.on_event(ev);
    }

    if (!consumed) {
      if (const auto* k = std::get_if<KeyEvent>(&ev)) {
        if (k->key == Key::Tab) {
          m_focus = (m_focus + 1) % 2;
          return;
        }
      }
    }

    App::on_event(ev);
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();

    // Simulate live data.
    const float t = static_cast<float>(m_frame) * 0.05f;
    const float sine = std::sin(t) * 0.5f + 0.5f;         // 0..1
    const float noise = std::sin(t * 3.7f) * 0.2f + 0.5f;  // faster wobble
    m_wave.push(sine);

    const int cpu = 20 + static_cast<int>(sine * 60.0f);
    const int mem_gb = 4 + (m_frame % 3);
    m_table.set_cell(0, 1, std::format("{}%", cpu));
    m_table.set_cell(1, 1, std::format("{}.{} GB", mem_gb, m_frame % 10));

    const int W = screen.cols();
    const int H = screen.rows();

    // Title bar.
    screen.write_text(0, 0, " TermForge Dashboard ", Rgb{0xFF, 0xFF, 0xFF},
                      Rgb{0x20, 0x40, 0x80});

    // Layout: table on left, log on right, waveform across the bottom.
    const int wave_h = 6;
    const int content_h = H - 3 - wave_h;  // title + footer + waveform
    const int table_w = W / 2;
    const int log_w = W - table_w;

    // Table (left half, top section).
    m_table.set_geometry({0, 1, table_w, content_h});
    m_table.draw(screen);

    // Log (right half, top section) with separator.
    if (table_w > 0 && log_w > 2) {
      for (int y = 1; y < 1 + content_h; ++y)
        screen.write_text(table_w - 1, y, "│", Rgb{0x40, 0x40, 0x60}, {});

      m_log.set_geometry({table_w, 1, log_w, content_h});
      m_log.draw(screen);
    }

    // Waveform (full width, bottom section).
    screen.write_text(0, 1 + content_h, " Signal ", Rgb{0x80, 0x80, 0x80},
                      Rgb{0x10, 0x10, 0x20});
    m_wave.set_geometry({0, 2 + content_h, W, wave_h - 1});
    m_wave.draw(screen);
    render_pixel_regions(m_wave);  // kitty: native pixel chart

    // Footer.
    const std::string footer =
        std::format(" Tab: switch focus ({}) | ESC: quit ",
                    m_focus == 0 ? "table" : "log");
    screen.write_text(0, H - 1, footer, Rgb{0x80, 0x80, 0x80},
                      Rgb{0x10, 0x10, 0x20});

    // Frame counter.
    screen.write_text(W - 12, 0, std::format("F:{:5}", m_frame++),
                      Rgb{0x60, 0x60, 0x80}, Rgb{0x20, 0x40, 0x80});
  }

 private:
  TableWidget m_table;
  TextBox m_log;
  WaveformWidget m_wave;
  int m_focus{0};
  int m_frame{0};
};

auto main() -> int {
  DashboardApp app;
  return app.run();
}
