#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
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

struct ProcessRow {
  int pid{};
  std::string name;
  float cpu_percent{};
  std::uint64_t rss_bytes{};

  auto operator==(const ProcessRow &) const -> bool = default;
};

struct SystemSnapshot {
  std::vector<CpuSample> cpus;
  MemoryInfo memory;
  std::vector<ProcessRow> processes;
};

class SystemReader {
public:
  virtual ~SystemReader() = default;
  virtual auto sample() -> std::expected<SystemSnapshot, ErrorEvent> = 0;
};

auto make_proc_reader(std::filesystem::path root = "/proc")
    -> std::unique_ptr<SystemReader>;
auto make_fake_reader(int core_count = 20) -> std::unique_ptr<SystemReader>;

} // namespace termforge::forge_top
