#pragma once

#include <expected>
#include <string>

#include "forge_top.hpp"

namespace termforge::forge_top {

struct Options {
  bool fake{false};
  bool help{false};
  DriverChoice driver{DriverChoice::Automatic};
};

auto parse_options(int argc, char** argv)
    -> std::expected<Options, std::string>;
auto usage() -> const char*;
auto run_cli(int argc, char** argv) -> int;

} // namespace termforge::forge_top
