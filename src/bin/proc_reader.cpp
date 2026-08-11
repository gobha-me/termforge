#include "proc_reader.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include <unistd.h>

namespace termforge::forge_top {
namespace {

struct CpuTicks {
  std::uint64_t total{};
  std::uint64_t idle{};
};

struct ProcessTicks {
  std::uint64_t ticks{};
};

auto warning(std::string message) -> std::unexpected<ErrorEvent> {
  return std::unexpected{
      ErrorEvent{Severity::Warning, "forge-top", std::move(message)}};
}

template <typename T>
auto parse_integer(std::string_view text, T &value) -> bool {
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

auto numeric_name(std::string_view name) -> bool {
  return !name.empty() && std::ranges::all_of(name, [](unsigned char ch) {
    return std::isdigit(ch) != 0;
  });
}

auto parse_cpu_line(std::string_view line, std::string &name, CpuTicks &out)
    -> bool {
  std::istringstream in{std::string{line}};
  std::uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
  std::uint64_t irq = 0, softirq = 0, steal = 0;
  if (!(in >> name >> user >> nice >> system >> idle))
    return false;
  (void)(in >> iowait >> irq >> softirq >> steal);
  out.idle = idle + iowait;
  out.total = user + nice + system + idle + iowait + irq + softirq + steal;
  return name == "cpu" || name.starts_with("cpu");
}

auto usage_between(const CpuTicks &previous, const CpuTicks &current) -> float {
  if (current.total <= previous.total)
    return 0.0F;
  const auto total = current.total - previous.total;
  const auto idle = current.idle >= previous.idle ? current.idle - previous.idle
                                                  : std::uint64_t{0};
  return std::clamp(static_cast<float>(total - std::min(total, idle)) /
                        static_cast<float>(total),
                    0.0F, 1.0F);
}

auto read_cpu_ticks(const std::filesystem::path &path)
    -> std::expected<std::vector<std::pair<std::string, CpuTicks>>,
                     ErrorEvent> {
  std::ifstream input{path};
  if (!input)
    return warning("cannot read " + path.string());

  std::vector<std::pair<std::string, CpuTicks>> result;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.starts_with("cpu"))
      break;
    std::string name;
    CpuTicks ticks;
    if (!parse_cpu_line(line, name, ticks))
      return warning("malformed CPU counters in " + path.string());
    result.emplace_back(std::move(name), ticks);
  }
  if (result.size() < 2 || result.front().first != "cpu")
    return warning("no per-core CPU counters in " + path.string());
  return result;
}

auto read_memory(const std::filesystem::path &path)
    -> std::expected<MemoryInfo, ErrorEvent> {
  std::ifstream input{path};
  if (!input)
    return warning("cannot read " + path.string());

  std::unordered_map<std::string, std::uint64_t> values;
  std::string key;
  std::uint64_t kib = 0;
  std::string unit;
  while (input >> key >> kib) {
    if (!key.empty() && key.back() == ':')
      key.pop_back();
    std::getline(input, unit);
    values[key] = kib;
  }
  if (!values.contains("MemTotal"))
    return warning("missing MemTotal in " + path.string());

  const auto get = [&](std::string_view name) {
    const auto it = values.find(std::string{name});
    return it == values.end() ? std::uint64_t{0} : it->second;
  };
  const std::uint64_t available =
      values.contains("MemAvailable")
          ? get("MemAvailable")
          : get("MemFree") + get("Buffers") + get("Cached");
  constexpr std::uint64_t kKiB = 1024;
  return MemoryInfo{get("MemTotal") * kKiB,
                    std::min(get("MemTotal"), available) * kKiB,
                    get("SwapTotal") * kKiB,
                    std::min(get("SwapTotal"), get("SwapFree")) * kKiB};
}

struct ParsedProcess {
  int pid{};
  std::string name;
  std::uint64_t ticks{};
  std::uint64_t rss_pages{};
};

auto read_process(const std::filesystem::path &path, int pid)
    -> std::optional<ParsedProcess> {
  std::ifstream input{path};
  std::string line;
  if (!input || !std::getline(input, line))
    return std::nullopt;

  const auto open = line.find('(');
  const auto close = line.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close <= open)
    return std::nullopt;

  std::vector<std::string> fields;
  std::istringstream tail{line.substr(close + 1)};
  for (std::string field; tail >> field;)
    fields.push_back(std::move(field));
  // fields[0] is stat field 3 (state); utime/stime are 14/15 and RSS is 24.
  if (fields.size() <= 21)
    return std::nullopt;

  std::uint64_t utime = 0, stime = 0, rss_pages = 0;
  if (!parse_integer(fields[11], utime) || !parse_integer(fields[12], stime) ||
      !parse_integer(fields[21], rss_pages))
    return std::nullopt;
  return ParsedProcess{pid, line.substr(open + 1, close - open - 1),
                       utime + stime, rss_pages};
}

class ProcReader final : public SystemReader {
public:
  explicit ProcReader(std::filesystem::path root) : m_root(std::move(root)) {}

  auto sample() -> std::expected<SystemSnapshot, ErrorEvent> override {
    auto ticks = read_cpu_ticks(m_root / "stat");
    if (!ticks)
      return std::unexpected{ticks.error()};
    auto memory = read_memory(m_root / "meminfo");
    if (!memory)
      return std::unexpected{memory.error()};

    const CpuTicks aggregate = ticks->front().second;
    SystemSnapshot snapshot;
    snapshot.memory = *memory;
    snapshot.cpus.reserve(ticks->size() - 1);
    for (std::size_t i = 1; i < ticks->size(); ++i) {
      const auto &[name, current] = (*ticks)[i];
      const auto previous = m_cpu_ticks.find(name);
      const float usage = previous == m_cpu_ticks.end()
                              ? 0.0F
                              : usage_between(previous->second, current);
      snapshot.cpus.push_back({name, usage});
    }

    std::unordered_map<int, ProcessTicks> next_process_ticks;
    std::error_code error;
    std::filesystem::directory_iterator entries{m_root, error};
    if (error)
      return warning("cannot enumerate " + m_root.string());
    const long page_size = ::sysconf(_SC_PAGESIZE);
    const std::uint64_t page_bytes =
        page_size > 0 ? static_cast<std::uint64_t>(page_size) : 4096;
    const std::uint64_t total_delta =
        aggregate.total > m_total_ticks.total
            ? aggregate.total - m_total_ticks.total
            : std::uint64_t{0};
    const float cores = static_cast<float>(snapshot.cpus.size());

    for (auto it = entries; it != std::filesystem::directory_iterator{};) {
      const auto entry = *it;
      it.increment(error);
      if (error)
        return warning("cannot enumerate " + m_root.string());
      const std::string filename = entry.path().filename().string();
      if (!numeric_name(filename))
        continue;
      int pid = 0;
      if (!parse_integer(filename, pid) || pid <= 0)
        continue;
      const auto parsed = read_process(entry.path() / "stat", pid);
      if (!parsed)
        continue; // exited or unreadable between enumerate/read

      float cpu = 0.0F;
      const auto previous = m_process_ticks.find(pid);
      if (previous != m_process_ticks.end() && total_delta > 0 &&
          parsed->ticks >= previous->second.ticks) {
        cpu = static_cast<float>(parsed->ticks - previous->second.ticks) /
              static_cast<float>(total_delta) * cores * 100.0F;
      }
      snapshot.processes.push_back(
          {pid, parsed->name, std::max(0.0F, cpu),
           parsed->rss_pages >
                   std::numeric_limits<std::uint64_t>::max() / page_bytes
               ? std::numeric_limits<std::uint64_t>::max()
               : parsed->rss_pages * page_bytes});
      next_process_ticks.emplace(pid, ProcessTicks{parsed->ticks});
    }

    m_cpu_ticks.clear();
    for (const auto &[name, current] : *ticks)
      m_cpu_ticks.emplace(name, current);
    m_total_ticks = aggregate;
    m_process_ticks = std::move(next_process_ticks);
    return snapshot;
  }

private:
  std::filesystem::path m_root;
  std::unordered_map<std::string, CpuTicks> m_cpu_ticks;
  std::unordered_map<int, ProcessTicks> m_process_ticks;
  CpuTicks m_total_ticks;
};

} // namespace

auto make_proc_reader(std::filesystem::path root)
    -> std::unique_ptr<SystemReader> {
  return std::make_unique<ProcReader>(std::move(root));
}

} // namespace termforge::forge_top
