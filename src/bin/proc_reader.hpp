#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "termforge/core/types.hpp"

namespace termforge::forge_top {

struct CpuSample {
  std::string name;
  float usage{};
};

struct MemoryInfo {
  std::uint64_t total_bytes{};
  std::uint64_t available_bytes{};
  std::uint64_t swap_total_bytes{};
  std::uint64_t swap_free_bytes{};
};

struct TaskCounts {
  int total{};
  int running{};
  int sleeping{};
  int stopped{};
  int zombie{};
};

struct ProcessIdentity {
  int pid{};
  std::uint64_t start_time_ticks{};

  auto operator==(const ProcessIdentity&) const -> bool = default;
};

struct ProcessIdentityHash {
  [[nodiscard]] auto operator()(const ProcessIdentity& identity) const noexcept
      -> std::size_t {
    const std::size_t pid = std::hash<int>{}(identity.pid);
    const std::size_t start =
        std::hash<std::uint64_t>{}(identity.start_time_ticks);
    return start ^ (pid + 0x9e3779b9U + (start << 6U) + (start >> 2U));
  }
};

struct ProcessRow {
  int pid{};
  std::string name;
  float cpu_percent{};
  std::uint64_t rss_bytes{};
  std::string user{"?"};
  char state{'?'};
  float memory_percent{};
  double cpu_seconds{};
  std::string command;
  std::uint64_t start_time_ticks{};

  [[nodiscard]] auto identity() const noexcept -> ProcessIdentity {
    return {pid, start_time_ticks};
  }

  auto operator==(const ProcessRow&) const -> bool = default;
};

struct SystemSnapshot {
  std::vector<CpuSample> cpus;
  MemoryInfo memory;
  std::vector<ProcessRow> processes;
  CpuSample aggregate_cpu{"cpu", 0.0F};
  double uptime_seconds{};
  std::array<double, 3> load_average{};
  TaskCounts tasks;
};

// Binary-private observation seam: production discovers both values with
// sysconf; fixture tests can force the failure paths no real host exposes.
struct ProcReaderConfig {
  std::optional<long> page_size;
  std::optional<long> clock_ticks;
};

class SystemReader {
 public:
  virtual ~SystemReader() = default;
  virtual auto sample() -> std::expected<SystemSnapshot, ErrorEvent> = 0;
};

auto make_proc_reader(std::filesystem::path root = "/proc",
                      ProcReaderConfig config = {})
    -> std::unique_ptr<SystemReader>;
auto make_fake_reader(int core_count = 20) -> std::unique_ptr<SystemReader>;

} // namespace termforge::forge_top
