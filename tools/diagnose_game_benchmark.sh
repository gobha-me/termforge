#!/usr/bin/env bash

# Rebuild and diagnose issue #252 on the machine where it was observed.
# The build and all captured output live in one fresh /tmp directory whose
# path is printed at the end; this script does not alter an existing build.

set -u

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
run_dir=$(mktemp -d /tmp/termforge-game-diagnose.XXXXXX)
build_dir="$run_dir/build"
report="$run_dir/game.json"
stdout_log="$run_dir/stdout.txt"
stderr_log="$run_dir/stderr.txt"
backtrace_log="$run_dir/backtrace.txt"

echo "TermForge game benchmark diagnostic"
echo "source:    $source_dir"
echo "artifacts: $run_dir"

if ! command -v g++-13 >/dev/null 2>&1; then
  echo "error: g++-13 is required but was not found" >&2
  exit 2
fi

if ! cmake -S "$source_dir" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-13 \
  -Dtermforge_BENCH=ON \
  -Dtermforge_TESTS=OFF \
  -Dtermforge_BIN=OFF; then
  echo "error: configure failed; artifacts: $run_dir" >&2
  exit 1
fi
if ! cmake --build "$build_dir" --target termforge_example_game -j4; then
  echo "error: build failed; artifacts: $run_dir" >&2
  exit 1
fi

game="$build_dir/examples/termforge_example_game"
echo "Starting: $game --benchmark 1 --report $report"
"$game" --benchmark 1 --report "$report" \
  >"$stdout_log" 2>"$stderr_log" &
game_pid=$!

# A healthy one-frame Release run takes milliseconds. Five seconds leaves
# ample room for a heavily loaded host while still making the hang bounded.
for _ in $(seq 1 50); do
  if ! kill -0 "$game_pid" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

if kill -0 "$game_pid" 2>/dev/null; then
  echo "HANG REPRODUCED: PID $game_pid is still running after 5 seconds"
  ps -o pid,ppid,state,etime,wchan:32,cmd -p "$game_pid" | tee "$run_dir/process.txt"
  if [ -r "/proc/$game_pid/wchan" ]; then
    echo "wchan: $(<"/proc/$game_pid/wchan")" | tee -a "$run_dir/process.txt"
  fi

  if command -v gdb >/dev/null 2>&1; then
    echo "Capturing all thread backtraces (sudo may request your password)..."
    sudo gdb -q -batch \
      -ex 'set pagination off' \
      -ex 'thread apply all bt' \
      -p "$game_pid" 2>&1 | tee "$backtrace_log"
  else
    echo "gdb is not installed; process state was captured without a backtrace" \
      | tee "$backtrace_log"
  fi

  # The script owns this diagnostic child. Stop it after collecting evidence
  # so the user's terminal is not left with the reported hung process.
  kill "$game_pid" 2>/dev/null || true
  sleep 0.2
  if kill -0 "$game_pid" 2>/dev/null; then
    kill -KILL "$game_pid" 2>/dev/null || true
  fi
  echo "Diagnostic artifacts: $run_dir"
  exit 124
fi

wait "$game_pid"
game_status=$?
echo "Benchmark exited with status $game_status"
cat "$stdout_log"
cat "$stderr_log" >&2
if [ -f "$report" ]; then
  echo "Report: $report"
else
  echo "No report was written"
fi
echo "Diagnostic artifacts: $run_dir"
exit "$game_status"
