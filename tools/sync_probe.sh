#!/usr/bin/env bash
# Probe DEC private mode 2026 (synchronized output) on the terminal attached
# to this process. Python owns the raw-mode lifetime so the tty is restored on
# timeout, Ctrl-C, and exceptions.

set -euo pipefail

exec python3 - "$@" <<'PY'
import argparse
import os
import re
import select
import sys
import termios
import time
import tty


QUERY = b"\x1b[?2026$p"
SYNC_BEGIN = b"\x1b[?2026h"
SYNC_END = b"\x1b[?2026l"
REPLY = re.compile(rb"\x1b\[\?2026;([0-4])\$y")
STATE = {
    0: "not recognized",
    1: "set",
    2: "reset",
    3: "permanently set",
    4: "permanently reset",
}


def visible(data: bytes) -> str:
    """Render control bytes without letting them affect the report."""
    return "".join(
        chr(byte) if 0x20 <= byte <= 0x7E else f"\\x{byte:02x}"
        for byte in data
    )


def probe(fd: int, timeout: float) -> bytes:
    saved = termios.tcgetattr(fd)
    reply = bytearray()
    try:
        termios.tcflush(fd, termios.TCIFLUSH)
        tty.setraw(fd, termios.TCSANOW)
        os.write(fd, QUERY)

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            readable, _, _ = select.select([fd], [], [], remaining)
            if not readable:
                break
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            reply.extend(chunk)
            if REPLY.search(reply):
                break
    finally:
        termios.tcsetattr(fd, termios.TCSANOW, saved)
    return bytes(reply)


def visual_check(fd: int) -> None:
    print()
    print("Visual check: watch the next line for about 1.5 seconds.")
    print("Expected: steps 1 and 2 never appear; FINAL appears all at once.")
    time.sleep(1.0)
    os.write(fd, SYNC_BEGIN)
    try:
        for label in (b"step 1", b"step 2", b"FINAL "):
            os.write(fd, b"\r\x1b[2K2026 visual: " + label)
            time.sleep(0.5)
    finally:
        # Never leave the terminal buffering if the check is interrupted.
        os.write(fd, SYNC_END)
        os.write(fd, b"\r\n")
    print("Report whether you saw intermediate steps or only FINAL.")


parser = argparse.ArgumentParser(
    prog="tools/sync_probe.sh",
    description="Probe DEC synchronized-output mode 2026 on /dev/tty."
)
parser.add_argument(
    "--timeout",
    type=float,
    default=1.0,
    help="seconds to wait for the DECRPM response (default: 1.0)",
)
parser.add_argument(
    "--visual",
    action="store_true",
    help="after a supported response, demonstrate atomic presentation",
)
args = parser.parse_args()
if args.timeout <= 0:
    parser.error("--timeout must be greater than zero")

try:
    tty_fd = os.open("/dev/tty", os.O_RDWR | os.O_NOCTTY)
except OSError as error:
    print(f"sync_probe: cannot open /dev/tty: {error}", file=sys.stderr)
    raise SystemExit(1)

try:
    response = probe(tty_fd, args.timeout)
    match = REPLY.search(response)

    print(f"TERM={os.environ.get('TERM', '')}")
    print(f"COLORTERM={os.environ.get('COLORTERM', '')}")
    print(f"query:    {visible(QUERY)}")
    print(f"query hex:{QUERY.hex(' ')}")
    if response:
        print(f"reply:    {visible(response)}")
        print(f"reply hex:{response.hex(' ')}")
    else:
        print(f"reply:    <none within {args.timeout:g}s>")

    if match is None:
        print("result:   no usable mode-2026 DECRPM; TermForge leaves sync off")
    else:
        state = int(match.group(1))
        enabled = state in (1, 2)
        print(f"state:    {state} ({STATE[state]})")
        print(
            "result:   TermForge enables synchronized output"
            if enabled
            else "result:   TermForge conservatively leaves sync off"
        )
        if args.visual and enabled:
            visual_check(tty_fd)
        elif args.visual:
            print("visual:   skipped because the response was not settable")
finally:
    os.close(tty_fd)
PY
