// TermForge #111 startup-transfer evidence.
//
// Run as a direct child of a Kitty-protocol terminal in the same POSIX shared
// memory namespace. Each sample uploads the same deterministic 1 MiB RGBA
// image once, waits for a later ordered graphics fence, and deletes it. The
// two routes alternate so scheduler drift does not all land on one side.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cerrno>
#include <cstring>
#include <unistd.h>

#include "detail/base64.hpp"
#include "termforge/core/image_transport.hpp"
#include "termforge/core/terminal.hpp"

using namespace termforge;

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  int samples{7};
  int warmup{2};
  int timeout_ms{5000};
  std::optional<std::filesystem::path> output;
};

struct Sample {
  double assembly_ms{};
  double write_ms{};
  double reply_ms{};
  double total_ms{};
  std::size_t wire_bytes{};
};

class ScreenGuard {
 public:
  explicit ScreenGuard(Terminal& terminal) : m_terminal(terminal) {
    m_terminal.enter_screen();
  }
  ~ScreenGuard() { m_terminal.leave_screen(); }
  ScreenGuard(const ScreenGuard&) = delete;
  auto operator=(const ScreenGuard&) -> ScreenGuard& = delete;

 private:
  Terminal& m_terminal;
};

[[nodiscard]] auto parse_positive(std::string_view value,
                                  std::string_view name) -> int {
  int out = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), out);
  if (error != std::errc{} || end != value.data() + value.size() || out <= 0)
    throw std::runtime_error{
        std::format("{} must be a positive integer", name)};
  return out;
}

[[nodiscard]] auto parse_options(int argc, char** argv) -> Options {
  Options out;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value = [&](std::string_view name) -> std::string_view {
      if (i + 1 >= argc)
        throw std::runtime_error{std::format("{} requires a value", name)};
      return argv[++i];
    };
    if (arg == "--samples") {
      out.samples = parse_positive(value(arg), arg);
    } else if (arg == "--warmup") {
      out.warmup = parse_positive(value(arg), arg);
    } else if (arg == "--timeout-ms") {
      out.timeout_ms = parse_positive(value(arg), arg);
    } else if (arg == "--output") {
      out.output = std::filesystem::path{value(arg)};
    } else if (arg == "--help") {
      std::cout << "Usage: termforge_shm_bench [--samples N] [--warmup N] "
                   "[--timeout-ms N] [--output PATH]\n";
      std::exit(0);
    } else {
      throw std::runtime_error{std::format("unknown argument: {}", arg)};
    }
  }
  return out;
}

[[nodiscard]] auto payload() -> std::vector<std::byte> {
  constexpr std::size_t kBytes = std::size_t{512} * 512U * 4U;
  std::vector<std::byte> out(kBytes);
  std::uint32_t state = 0x12345678U;
  for (auto& byte : out) {
    state = state * 1664525U + 1013904223U;
    byte = static_cast<std::byte>(state >> 24);
  }
  return out;
}

[[nodiscard]] auto direct_command(std::span<const std::byte> bytes,
                                  std::uint32_t id) -> std::string {
  const auto encoded = termforge::detail::base64_encode(bytes);
  constexpr std::size_t kChunk = 4096;
  std::string out;
  out.reserve(encoded.size() + encoded.size() / kChunk * 24U + 96U);
  std::size_t offset = 0;
  bool first = true;
  while (offset < encoded.size() || first) {
    const auto chunk = std::string_view{encoded}.substr(offset, kChunk);
    const bool more = offset + kChunk < encoded.size();
    if (first) {
      out += std::format("\033_Ga=t,t=d,f=32,i={},s=512,v=512,m={},q=2;{}\033\\",
                         id, more ? 1 : 0, chunk);
      first = false;
    } else {
      out += std::format("\033_Gm={},q=2;{}\033\\", more ? 1 : 0, chunk);
    }
    offset += kChunk;
  }
  return out;
}

[[nodiscard]] auto shared_command(const ImageTransferLease& lease,
                                  std::size_t size, std::uint32_t id)
    -> std::string {
  const auto locator =
      std::as_bytes(std::span<const char>{lease.locator()});
  return std::format("\033_Ga=t,t=s,f=32,i={},s=512,v=512,S={},q=2;{}\033\\",
                     id, size, termforge::detail::base64_encode(locator));
}

auto write_all(int fd, std::string_view bytes) -> double {
  const auto started = Clock::now();
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    throw std::runtime_error{std::string{"write: "} + std::strerror(errno)};
  }
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

[[nodiscard]] auto wait_for_reply(Terminal& terminal, std::uint32_t id,
                                  int timeout_ms) -> double {
  const std::string prefix = std::format("\033_Gi={};", id);
  const auto started = Clock::now();
  const auto deadline = started + std::chrono::milliseconds{timeout_ms};
  std::string reply;
  std::array<char, 1024> buffer{};
  while (Clock::now() < deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now());
    if (!terminal.wait_readable(std::max(1, static_cast<int>(left.count()))))
      continue;
    const int count =
        terminal.read_input(buffer.data(), static_cast<int>(buffer.size()));
    if (count <= 0) continue;
    reply.append(buffer.data(), static_cast<std::size_t>(count));
    const auto begin = reply.find(prefix);
    if (begin == std::string::npos) continue;
    const auto end = reply.find("\033\\", begin + prefix.size());
    if (end == std::string::npos) continue;
    const auto status = std::string_view{reply}.substr(
        begin + prefix.size(), end - begin - prefix.size());
    if (status != "OK")
      throw std::runtime_error{std::format("terminal rejected image: {}", status)};
    return std::chrono::duration<double, std::milli>(Clock::now() - started)
        .count();
  }
  throw std::runtime_error{
      std::format("terminal image acknowledgement timed out after receiving "
                  "{} bytes for image {}: {}",
                  reply.size(), id, reply)};
}

[[nodiscard]] auto wait_for_fence(Terminal& terminal, int timeout_ms)
    -> double {
  write_all(terminal.io().out,
            "\033_Gi=424242,s=1,v=1,a=q,t=d,f=24;AAAA\033\\");
  return wait_for_reply(terminal, 424242, timeout_ms);
}

[[nodiscard]] auto percentile(std::vector<double> values, double p) -> double {
  std::sort(values.begin(), values.end());
  const auto index = std::min(
      values.size() - 1,
      static_cast<std::size_t>(
          std::ceil(static_cast<double>(values.size()) * p)) - 1);
  return values[index];
}

[[nodiscard]] auto median(const std::vector<Sample>& samples,
                          double Sample::*member) -> double {
  std::vector<double> values;
  values.reserve(samples.size());
  for (const auto& sample : samples) values.push_back(sample.*member);
  return percentile(std::move(values), 0.5);
}

auto print_json(std::ostream& out, const std::vector<Sample>& direct,
                const std::vector<Sample>& shared) -> void {
  const auto route = [&](std::string_view name,
                         const std::vector<Sample>& samples) {
    out << std::format(
        "    \"{}\": {{\"samples\": {}, \"wire_bytes\": {}, "
        "\"assembly_median_ms\": {:.6f}, \"write_median_ms\": {:.6f}, "
        "\"reply_median_ms\": {:.6f}, \"total_median_ms\": {:.6f}}}",
        name, samples.size(), samples.front().wire_bytes,
        median(samples, &Sample::assembly_ms), median(samples, &Sample::write_ms),
        median(samples, &Sample::reply_ms), median(samples, &Sample::total_ms));
  };
  out << "{\n  \"schema\": 1,\n  \"payload_bytes\": 1048576,\n  \"routes\": {\n";
  route("direct", direct);
  out << ",\n";
  route("shared_memory", shared);
  out << "\n  }\n}\n";
}

} // namespace

int main(int argc, char** argv) try {
  const Options options = parse_options(argc, argv);
  Terminal terminal;
  if (auto raw = terminal.enter_raw(); !raw)
    throw std::runtime_error{raw.error().message};
  const auto capabilities = terminal.query_capabilities();
  if (!capabilities) throw std::runtime_error{capabilities.error().message};
  if (!capabilities->kitty_graphics)
    throw std::runtime_error{"terminal does not report Kitty graphics"};
  terminal.set_read_timeout(0);
  std::vector<Sample> direct;
  std::vector<Sample> shared;
  {
    ScreenGuard screen{terminal};
    static_cast<void>(wait_for_fence(terminal, options.timeout_ms));

    const auto bytes = payload();
    auto transport = std::make_shared<PosixSharedMemoryTransport>();
    const int rounds = options.warmup + options.samples;
    std::uint32_t id = 1000;
    for (int round = 0; round < rounds; ++round) {
      for (int route = 0; route < 2; ++route) {
        ++id;
        const auto total_started = Clock::now();
        const auto assembly_started = Clock::now();
        std::unique_ptr<ImageTransferLease> lease;
        std::string command;
        if ((round + route) % 2 == 0) {
          auto staged = transport->stage(bytes);
          if (!staged) throw std::runtime_error{staged.error().message};
          lease = std::move(*staged);
          command = shared_command(*lease, bytes.size(), id);
        } else {
          command = direct_command(bytes, id);
        }
        const double assembly_ms =
            std::chrono::duration<double, std::milli>(Clock::now() -
                                                      assembly_started)
                .count();
        const double write_ms = write_all(terminal.io().out, command);
        const double reply_ms = wait_for_fence(terminal, options.timeout_ms);
        const double total_ms =
            std::chrono::duration<double, std::milli>(Clock::now() -
                                                      total_started)
                .count();
        lease.reset();
        write_all(terminal.io().out,
                  std::format("\033_Ga=d,d=I,i={},q=2\033\\", id));
        if (round < options.warmup) continue;
        Sample sample{assembly_ms, write_ms, reply_ms, total_ms,
                      command.size()};
        if (command.find("t=s") != std::string::npos)
          shared.push_back(sample);
        else
          direct.push_back(sample);
      }
    }
  }
  terminal.leave_raw();

  print_json(std::cout, direct, shared);
  if (options.output) {
    std::ofstream file{*options.output};
    if (!file) throw std::runtime_error{"could not open benchmark output"};
    print_json(file, direct, shared);
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << "termforge_shm_bench: " << error.what() << '\n';
  return 1;
}
