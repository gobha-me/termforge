#!/usr/bin/env bash
# Out-of-tree consumption acceptance test (issue #27).
#
#   tools/consume/run.sh subdir   [cxx] -- add_subdirectory, ZERO extra options
#   tools/consume/run.sh install  [cxx] -- install + find_package(termforge CONFIG)
#   tools/consume/run.sh vendored [cxx] -- add_subdirectory from a plain copy
#                                          inside someone else's tagged repo
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
            \( -name 'termforge_example_*' -o -name '*-test' \
               -o -name 'forge-top' -o -name 'termforge' \) 2>/dev/null || true)
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

  vendored)
    # `git describe` searches *upward* for a .git, so termforge dropped into
    # another project as a plain directory -- external/termforge/, not a
    # submodule -- used to answer with the *consumer's* tag and report it as
    # its own version. Configure only: this is about the number project()
    # records, and nothing downstream of it needs compiling to see the number.
    VENDOR=${WORK}/vendor
    mkdir -p "${VENDOR}/external/termforge"

    # Tracked paths copied out of the WORKING TREE, so an uncommitted edit to
    # cmake/version.cmake is what gets tested -- and so no .git, no build dir
    # and no stray artifact rides along to hand the copy a repo of its own,
    # which would defeat the whole fixture.
    # -C before --null -T -: tar treats options after a non-option argument as
    # positional, so trailing -C silently does nothing.
    (cd "${ROOT}" && git ls-files -z) \
      | tar -cf - -C "${ROOT}" --null -T - \
      | tar -xf - -C "${VENDOR}/external/termforge"

    # The identity goes on every write, tag included: `git tag -a` needs a
    # tagger just like commit needs a committer, and a CI runner has no global
    # git config to fall back on. A dev box does, which is exactly how a
    # missing -c here stays invisible until it fails on GitHub.
    vgit() { git -C "${VENDOR}" -c user.email=ci@termforge -c user.name=ci "$@"; }

    vgit init -q .
    vgit commit -qm vendor --allow-empty
    vgit tag -a v9.9.9 -m v9.9.9

    cat > "${VENDOR}/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.28)
project(vendored_consumer LANGUAGES CXX)
add_subdirectory(external/termforge)
EOF

    # $1 = build dir, $2 = expected version, rest = extra cmake args
    assert_version() {
      local tree=$1 want=$2; shift 2
      local out
      if ! out=$(cmake -S "${VENDOR}" -B "${tree}" "${ARGS[@]}" "$@" 2>&1); then
        echo "${out}" >&2
        echo "FAIL: vendored consumer failed to configure" >&2
        exit 1
      fi
      if ! grep -qx -- "-- termforge:${want}" <<<"${out}"; then
        echo "FAIL: expected 'termforge:${want}', got:" >&2
        grep -- 'termforge:' <<<"${out}" | sed 's/^/  /' >&2
        exit 1
      fi
    }

    # The bug: 9.9.9 is the CONSUMER's tag. Anything but the honest fallback
    # means the enclosing repository is still being trusted.
    assert_version "${VENDOR}/build" "0.0.0.1"

    # ...and the documented escape hatch still outranks the guard.
    assert_version "${VENDOR}/build-pin" "1.2.3" -DTERMFORGE_VERSION=1.2.3
    ;;

  *)
    echo "unknown mode: ${MODE} (expected 'subdir', 'install' or 'vendored')" >&2
    exit 2
    ;;
esac

echo "OK: consumption path '${MODE}'"
