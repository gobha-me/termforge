#!/usr/bin/env bash
# Run one W5 capture from inside the terminal being measured. This deliberately
# does not add an inner `script`/tmux layer: the terminal's own child pty is the
# boundary W5 measures, and an extra relay would turn a direct result into a
# proxy result.
set -euo pipefail

usage() {
  echo "usage: $0 BENCHMARK TERMINAL VERSION ROUTE OUTPUT [extra benchmark args...]" >&2
  echo "  ROUTE is kitty or ansi; OUTPUT must end in .json" >&2
  exit 2
}

if [ "$#" -lt 5 ]; then
  usage
fi

benchmark=$1
terminal_name=$2
terminal_version=$3
route=$4
output=$5
shift 5

if [ ! -x "$benchmark" ]; then
  echo "w5_capture: benchmark is not executable: $benchmark" >&2
  exit 2
fi
if [ "$route" != kitty ] && [ "$route" != ansi ]; then
  echo "w5_capture: route must be kitty or ansi" >&2
  exit 2
fi
if [[ "$output" != *.json ]]; then
  echo "w5_capture: output must end in .json: $output" >&2
  exit 2
fi
if [ ! -t 0 ] || [ ! -t 1 ]; then
  echo "w5_capture: stdin and stdout must be the terminal under test" >&2
  exit 2
fi
if [ -n "${TMUX:-}" ] || [ -n "${SSH_CONNECTION:-}" ] || [ -n "${SSH_TTY:-}" ]; then
  echo "w5_capture: refusing a proxied result (tmux/SSH detected)" >&2
  exit 2
fi

exec "$benchmark" \
  --route "$route" \
  --terminal "$terminal_name" \
  --terminal-version "$terminal_version" \
  --format json \
  --output "$output" \
  "$@"
