#include "cli.hpp"

auto main(int argc, char** argv) -> int {
  return termforge::forge_top::run_cli(argc, argv);
}
