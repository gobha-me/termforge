#!/usr/bin/env bash
set -euo pipefail

if (($# != 0)); then
  echo "usage: tools/lint.sh" >&2
  exit 2
fi

tidy=${CLANG_TIDY:-clang-tidy-20}
runner=${RUN_CLANG_TIDY:-run-clang-tidy-20}
compiler=${CLANGXX:-clang++-20}
build_dir=${TERMFORGE_TIDY_BUILD_DIR:-build-tidy}
jobs=${TERMFORGE_TIDY_JOBS:-2}

for tool in "$tidy" "$runner" "$compiler"; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: $tool was not found; install Clang/clang-tidy 20.x" >&2
    exit 1
  fi
done

tidy_version=$($tidy --version)
if [[ ! "$tidy_version" =~ version[[:space:]]20\. ]]; then
  echo "error: TermForge linting requires clang-tidy 20.x; found: $tidy_version" >&2
  exit 1
fi

compiler_version=$($compiler --version)
if [[ ! "$compiler_version" =~ version[[:space:]]20\. ]]; then
  echo "error: TermForge linting requires Clang 20.x; found: $compiler_version" >&2
  exit 1
fi

if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: TERMFORGE_TIDY_JOBS must be a positive integer" >&2
  exit 2
fi

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

cmake -S . -B "$build_dir" \
  -DCMAKE_CXX_COMPILER="$compiler" \
  -DCMAKE_BUILD_TYPE=Release \
  -Dtermforge_TESTS=OFF \
  -Dtermforge_EXAMPLES=OFF \
  -Dtermforge_BIN=OFF \
  -Dtermforge_BENCH=OFF \
  -Dtermforge_INSTALL=OFF

"$runner" \
  -p "$build_dir" \
  -j "$jobs" \
  -clang-tidy-binary "$tidy" \
  -header-filter='^.*/(include/termforge|src/lib)/' \
  -warnings-as-errors='*' \
  -quiet \
  '^.*/src/lib/.*[.]cpp$'
