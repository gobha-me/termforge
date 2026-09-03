// TermForge example: reported cell-pixel geometry and geometry-only resizes.
//
// A fixed 22x9-cell plate is generated at the terminal's measured native
// pixel extent and placed with PlacementFit::Exact. Changing the Kitty font
// size keeps the cell grid fixed in many windows, but still delivers a
// ResizeEvent with a new cell_pixels value; the plate is then regenerated and
// continues to cover the same cell rect without an 8x16-style magic constant.

#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <variant>

#include "termforge/core/app.hpp"
#include "termforge/widgets/pixel_surface.hpp"

using namespace termforge;

namespace {

constexpr Rect kPlateRect{2, 4, 22, 9};
constexpr std::size_t kMaxExamplePixels{16U * 1024U * 1024U};

class CellGeometryDemo final : public App {
 public:
  CellGeometryDemo() {
    m_plate.set_geometry(kPlateRect);
    m_plate.set_fit(PlacementFit::Exact);
  }

 protected:
  auto on_start() -> void override {
    const auto reported = driver().reported_cell_pixel_size();
    rebuild(reported ? std::optional<Extent>{*reported} : std::nullopt);
  }

  auto on_event(const Event& event) -> void override {
    if (const auto* resize = std::get_if<ResizeEvent>(&event)) {
      rebuild(resize->cell_pixels);
      return;
    }
    App::on_event(event);
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();
    screen.write_text(0, 0, " Cell-pixel geometry | ESC quits ",
                      Rgb{235, 240, 250}, Rgb{20, 45, 85});
    screen.write_text(0, 1, m_status, Rgb{190, 205, 225}, Rgb{});

    if (!m_cell_pixels) {
      screen.write_text(0, 3,
                        "The terminal did not report cell-pixel geometry; "
                        "exact art is withheld.",
                        Rgb{255, 190, 80}, Rgb{});
      return;
    }
    if (screen.cols() < kPlateRect.x + kPlateRect.w ||
        screen.rows() < kPlateRect.y + kPlateRect.h + 2) {
      screen.write_text(0, 3, "Resize to at least 24x15 cells.",
                        Rgb{255, 190, 80}, Rgb{});
      return;
    }

    m_plate.draw(screen);          // Cell baseline on non-graphics tiers.
    render_pixel_regions(m_plate); // Exact native-pixel plate on Kitty.
    screen.write_text(kPlateRect.x, kPlateRect.y + kPlateRect.h + 1,
                      "Change the font size: the plate remains 22x9 cells.",
                      Rgb{190, 205, 225}, Rgb{});
  }

 private:
  auto rebuild(std::optional<Extent> cell_pixels) -> void {
    m_cell_pixels = cell_pixels;
    if (!cell_pixels) {
      m_plate.reset({});
      m_status = "reported cell pixels: unknown";
      return;
    }

    const auto width = static_cast<std::int64_t>(kPlateRect.w) *
                       static_cast<std::int64_t>(cell_pixels->w);
    const auto height = static_cast<std::int64_t>(kPlateRect.h) *
                        static_cast<std::int64_t>(cell_pixels->h);
    const auto max = static_cast<std::int64_t>(std::numeric_limits<int>::max());
    if (width <= 0 || height <= 0 || width > max || height > max ||
        static_cast<std::uint64_t>(width) >
            kMaxExamplePixels / static_cast<std::uint64_t>(height)) {
      m_cell_pixels.reset();
      m_plate.reset({});
      m_status = "reported geometry is too large for this example";
      return;
    }

    const Extent plate_pixels{static_cast<int>(width),
                              static_cast<int>(height)};
    m_plate.reset(plate_pixels);
    auto pixels = m_plate.pixels();
    for (int y = 0; y < plate_pixels.h; ++y) {
      for (int x = 0; x < plate_pixels.w; ++x) {
        const bool grid_line =
            x % cell_pixels->w == 0 || y % cell_pixels->h == 0;
        const bool alternate =
            ((x / cell_pixels->w) + (y / cell_pixels->h)) % 2 != 0;
        pixels[static_cast<std::size_t>(y) *
                   static_cast<std::size_t>(plate_pixels.w) +
               static_cast<std::size_t>(x)] =
            grid_line   ? Pixel{245, 225, 120, 255}
            : alternate ? Pixel{38, 75, 125, 255}
                        : Pixel{27, 48, 84, 255};
      }
    }
    m_status = std::format("reported cell pixels: {}x{}; exact plate: {}x{}",
                           cell_pixels->w, cell_pixels->h, plate_pixels.w,
                           plate_pixels.h);
  }

  PixelSurface m_plate{Extent{}};
  std::optional<Extent> m_cell_pixels;
  std::string m_status{"reported cell pixels: unknown"};
};

} // namespace

auto main() -> int {
  CellGeometryDemo app;
  return app.run();
}
