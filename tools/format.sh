#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: tools/format.sh [--check|--fix]" >&2
}

if (($# > 1)); then
  usage
  exit 2
fi

mode=${1:---check}
formatter=${CLANG_FORMAT:-clang-format-20}

case "$mode" in
  --check | --fix) ;;
  *)
    usage
    exit 2
    ;;
esac

if ! command -v "$formatter" >/dev/null 2>&1; then
  echo "error: $formatter was not found; install clang-format 20.x or set CLANG_FORMAT" >&2
  exit 1
fi

version=$($formatter --version)
if [[ ! "$version" =~ version[[:space:]]20\. ]]; then
  echo "error: TermForge formatting requires clang-format 20.x; found: $version" >&2
  exit 1
fi

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

mapfile -d '' files < <(git ls-files -z -- '*.cpp' '*.hpp')
if ((${#files[@]} == 0)); then
  echo "error: no tracked C++ files found" >&2
  exit 1
fi

if [[ "$mode" == "--fix" ]]; then
  "$formatter" -i "${files[@]}"
else
  "$formatter" --dry-run --Werror "${files[@]}"
fi
