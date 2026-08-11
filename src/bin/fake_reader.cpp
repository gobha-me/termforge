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
    snapshot.aggregate_cpu = {
        "cpu", static_cast<float>(0.18 + 0.62 *
                                  (std::sin(m_step * 0.19) * 0.5 + 0.5))};
    snapshot.uptime_seconds = 3.0 * 24.0 * 60.0 * 60.0 + m_step;
    snapshot.load_average = {1.25 + m_step * 0.01, 1.10, 0.95};

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
      const auto rss =
          static_cast<std::uint64_t>(24 + (i * 37 + m_step * 3) % 900) *
          1024ULL * 1024ULL;
      const char state = i % 17 == 0 ? 'Z' : (i % 5 == 0 ? 'R' : 'S');
      snapshot.processes.push_back(
          {pid, std::format("worker-{:02}", i), cpu, rss,
           std::format("user{}", i % 4), state,
           static_cast<float>(static_cast<long double>(rss) * 100.0L /
                              snapshot.memory.total_bytes),
           12.34 + i * 67.0 + m_step * 0.1,
           std::format("worker-{:02} --slot {} --fake", i, i)});
      ++snapshot.tasks.total;
      if (state == 'R')
        ++snapshot.tasks.running;
      else if (state == 'Z')
        ++snapshot.tasks.zombie;
      else
        ++snapshot.tasks.sleeping;
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
