#!/usr/bin/env bash
# Out-of-tree consumption acceptance test (issue #27).
#
#   tools/consume/run.sh subdir  [cxx]  -- add_subdirectory, ZERO extra options
#   tools/consume/run.sh install [cxx]  -- install + find_package(termforge CONFIG)
#
# Both paths must yield the same target spelling (termforge::lib) and must
# build ONLY the library. Everything happens in a mktemp -d, because
# termforge_TESTS/_EXAMPLES are already cached ON in every existing dev build
# dir and option() will not lower a cached value -- a reused tree would prove
# nothing.
set -euo pipefail

MODE=${1:?usage: run.sh <subdir|install> [cxx-compiler]}
CXX_COMPILER=${2:-}

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "${HERE}/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "${WORK}"' EXIT

ARGS=(-DCMAKE_BUILD_TYPE=Release)
if [ -n "${CXX_COMPILER}" ]; then
  ARGS+=(-DCMAKE_CXX_COMPILER="${CXX_COMPILER}")
fi

njobs() { command -v nproc >/dev/null 2>&1 && nproc || echo 2; }

# $1 = consumer build tree. Only meaningful in subdir mode (in install mode the
# consumer tree contains nothing but `consumer`).
assert_lib_only() {
  local tree=$1 stray
  stray=$(find "${tree}" -type f -perm -u+x \
            \( -name 'termforge_example_*' -o -name '*-test' -o -name 'termforge' \) 2>/dev/null || true)
  if [ -n "${stray}" ]; then
    echo "FAIL: consumer build produced non-library termforge targets:" >&2
    printf '%s\n' "${stray}" >&2
    exit 1
  fi
  if [ -d "${tree}/_deps/catch2-src" ]; then
    echo "FAIL: consumer build fetched Catch2, a test-only dependency" >&2
    exit 1
  fi
  if ! find "${tree}" -name 'libtermforge*.a' | grep -q .; then
    echo "FAIL: the termforge library archive was not built" >&2
    exit 1
  fi
}

case "${MODE}" in
  subdir)
    cmake -S "${HERE}" -B "${WORK}/build" "${ARGS[@]}" -DTERMFORGE_SOURCE_DIR="${ROOT}"
    cmake --build "${WORK}/build" -j"$(njobs)"
    assert_lib_only "${WORK}/build"
    "${WORK}/build/consumer"
    ;;

  install)
    # The explicit OFFs double as proof that the new options actually work.
    cmake -S "${ROOT}" -B "${WORK}/tf" "${ARGS[@]}" \
      -DCMAKE_INSTALL_PREFIX="${WORK}/prefix" \
      -Dtermforge_TESTS=OFF -Dtermforge_EXAMPLES=OFF -Dtermforge_BIN=OFF
    cmake --build "${WORK}/tf" -j"$(njobs)"
    cmake --install "${WORK}/tf"

    if ! find "${WORK}/prefix" -name 'termforgeConfig.cmake' | grep -q .; then
      echo "FAIL: termforgeConfig.cmake was not installed" >&2
      find "${WORK}/prefix" -type f | sed 's/^/  /' >&2
      exit 1
    fi
    if [ ! -f "${WORK}/prefix/include/termforge/core/screen.hpp" ]; then
      echo "FAIL: public headers did not land under \${prefix}/include/termforge" >&2
      exit 1
    fi
    if [ -f "${WORK}/prefix/include/version.hpp" ]; then
      echo "FAIL: the generated version.hpp leaked into \${prefix}/include" >&2
      exit 1
    fi

    cmake -S "${HERE}" -B "${WORK}/build" "${ARGS[@]}" \
      -DCMAKE_PREFIX_PATH="${WORK}/prefix"
    cmake --build "${WORK}/build" -j"$(njobs)"
    "${WORK}/build/consumer"
    ;;

  *)
    echo "unknown mode: ${MODE} (expected 'subdir' or 'install')" >&2
    exit 2
    ;;
esac

echo "OK: consumption path '${MODE}'"
