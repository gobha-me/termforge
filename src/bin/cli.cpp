#include "cli.hpp"

#include <cstdio>
#include <exception>
#include <string_view>

namespace termforge::forge_top {

auto usage() -> const char * {
  return "Usage: termforge [--fake] [--driver=kitty|ansi|fallback]\n"
         "\n"
         "A live /proc system monitor and all-tier TermForge demo.\n"
         "  --fake       deterministic data (screenshots and smoke tests)\n"
         "  --driver=…   force one rendering tier instead of probing\n"
         "  -h, --help   show this help\n";
}

auto parse_options(int argc, char **argv)
    -> std::expected<Options, std::string> {
  Options options;
  bool driver_seen = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument{argv[i]};
    if (argument == "-h" || argument == "--help") {
      options.help = true;
      continue;
    }
    if (argument == "--fake") {
      options.fake = true;
      continue;
    }
    if (argument.starts_with("--driver=")) {
      const std::string_view name = argument.substr(9);
      DriverChoice choice;
      if (name == "kitty")
        choice = DriverChoice::Kitty;
      else if (name == "ansi")
        choice = DriverChoice::Ansi;
      else if (name == "fallback")
        choice = DriverChoice::Fallback;
      else
        return std::unexpected{"unknown driver '" + std::string{name} + "'"};
      if (driver_seen && options.driver != choice)
        return std::unexpected{"conflicting --driver options"};
      options.driver = choice;
      driver_seen = true;
      continue;
    }
    return std::unexpected{"unknown option '" + std::string{argument} + "'"};
  }
  return options;
}

auto run_cli(int argc, char **argv) -> int {
  const auto options = parse_options(argc, argv);
  if (!options) {
    std::fprintf(stderr, "termforge: %s\n%s", options.error().c_str(), usage());
    return 2;
  }
  if (options->help) {
    std::fputs(usage(), stdout);
    return 0;
  }

  try {
    ForgeTopApp app{options->fake ? make_fake_reader() : make_proc_reader()};
    if (auto forced = app.force_driver(options->driver); !forced) {
      std::fprintf(stderr, "termforge: %s\n", forced.error().message.c_str());
      return 1;
    }
    return app.run();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "termforge: %s\n", error.what());
    return 1;
  }
}

} // namespace termforge::forge_top
