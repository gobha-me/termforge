#include "proc_reader.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace termforge::forge_top {
namespace {

class FakeReader final : public SystemReader {
public:
  explicit FakeReader(int core_count) : m_core_count(std::max(1, core_count)) {}

  auto sample() -> std::expected<SystemSnapshot, ErrorEvent> override {
    SystemSnapshot snapshot;
    snapshot.cpus.reserve(static_cast<std::size_t>(m_core_count));
    for (int core = 0; core < m_core_count; ++core) {
      const double phase = m_step * 0.31 + core * 0.47;
      snapshot.cpus.push_back(
          {std::format("cpu{}", core),
           static_cast<float>(0.12 + 0.78 * (std::sin(phase) * 0.5 + 0.5))});
    }

    constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
    snapshot.memory.total_bytes = 32 * gib;
    snapshot.memory.available_bytes =
        static_cast<std::uint64_t>((13.0 + std::sin(m_step * 0.2) * 2.0) * gib);
    snapshot.memory.swap_total_bytes = 8 * gib;
    snapshot.memory.swap_free_bytes =
        static_cast<std::uint64_t>((6.0 + std::cos(m_step * 0.17)) * gib);

    for (int i = 0; i < 48; ++i) {
      const int pid = 1000 + i;
      const float cpu = static_cast<float>(
          (std::sin(m_step * 0.23 + i * 0.61) * 0.5 + 0.5) * 96.0);
      snapshot.processes.push_back(
          {pid, std::format("worker-{:02}", i), cpu,
           static_cast<std::uint64_t>(24 + (i * 37 + m_step * 3) % 900) *
               1024ULL * 1024ULL});
    }
    ++m_step;
    return snapshot;
  }

private:
  int m_core_count;
  int m_step{0};
};

} // namespace

auto make_fake_reader(int core_count) -> std::unique_ptr<SystemReader> {
  return std::make_unique<FakeReader>(core_count);
}

} // namespace termforge::forge_top
