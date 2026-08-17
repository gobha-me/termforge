#!/usr/bin/env bash
# zlib_repro.sh — empirical gate for ImageFormat::Rgba32Zlib (#166).
#
# Run inside a real Kitty-protocol terminal:
#
#   ./tools/zlib_repro.sh
#   ./tools/zlib_repro.sh --dump   # emit wire only; no tty required
#
# The script generates one 16x16 RGBA test card with Python's stdlib zlib,
# transmits the exact pixels twice (raw f=32 and compressed f=32,o=z), and
# places both at the same scaled size. q=0 keeps decoder rejection observable.

set -u

dump=0
if (($# > 1)); then
  echo "usage: $0 [--dump]" >&2
  exit 2
fi
if (($# == 1)); then
  [[ $1 == --dump ]] || {
    echo "usage: $0 [--dump]" >&2
    exit 2
  }
  dump=1
fi

command -v python3 >/dev/null || {
  echo "$0: python3 is required (stdlib zlib only)" >&2
  exit 3
}

mapfile -t payloads < <(python3 - <<'PY'
import base64
import zlib

colors = (
    bytes((255, 32, 32, 255)),
    bytes((32, 255, 32, 255)),
    bytes((32, 32, 255, 255)),
    bytes((255, 224, 32, 255)),
)
raw = bytearray()
for y in range(16):
    for x in range(16):
        raw += colors[(x >= 8) + 2 * (y >= 8)]
print(base64.b64encode(raw).decode())
print(base64.b64encode(zlib.compress(raw, 1)).decode())
print(len(raw))
print(len(zlib.compress(raw, 1)))
PY
)
[[ ${#payloads[@]} == 4 ]] || {
  echo "$0: payload generation failed" >&2
  exit 4
}

raw_b64=${payloads[0]}
zlib_b64=${payloads[1]}
raw_size=${payloads[2]}
zlib_size=${payloads[3]}
ESC=$'\033'
ST="${ESC}\\"

saved_stty=''
if ((! dump)); then
  saved_stty=$(stty -g 2>/dev/null) || {
    echo "$0: needs a real terminal on stdin (or use --dump)" >&2
    exit 5
  }
fi
restore() { [[ -n $saved_stty ]] && stty "$saved_stty"; return 0; }
trap restore EXIT

say() { if ((dump)); then printf '%s\n' "$*" >&2; else printf '%s\n' "$*"; fi; }

reply_out=''
send_quiet() {
  local payload=$1 ch
  if ((dump)); then
    printf '%s' "$payload"
    reply_out='(dump)'
    return 0
  fi
  stty raw -echo
  printf '%s' "$payload"
  sleep 0.3
  reply_out=''
  while IFS= read -r -t 0.05 -n 1 ch; do reply_out+=$ch; done
  stty "$saved_stty"
  [[ -n $reply_out ]] || reply_out='(none)'
}

say "== #166: raw RGBA versus application-supplied zlib RGBA =="
say "   source ${raw_size} bytes; compressed ${zlib_size} bytes"

send_quiet "${ESC}_Ga=t,t=d,f=32,i=60,s=16,v=16,m=0,q=0;${raw_b64}${ST}"
raw_reply=$reply_out
send_quiet "${ESC}_Ga=t,t=d,f=32,o=z,i=61,s=16,v=16,m=0,q=0;${zlib_b64}${ST}"
zlib_reply=$reply_out

if ((dump)); then
  printf '%s[10C' "$ESC"
else
  printf '\n\n\n\n\n%s[5A' "$ESC"
fi
send_quiet "${ESC}_Ga=p,i=60,p=1,c=8,r=4,C=1,q=0${ST}"
raw_place_reply=$reply_out
printf '%s[10C' "$ESC"
send_quiet "${ESC}_Ga=p,i=61,p=1,c=8,r=4,C=1,q=0${ST}"
zlib_place_reply=$reply_out

if ((! dump)); then
  printf '%s[5B\r' "$ESC"
  say "raw transmit response:        $(printf '%q' "$raw_reply")"
  say "zlib transmit response:       $(printf '%q' "$zlib_reply")"
  say "raw placement response:       $(printf '%q' "$raw_place_reply")"
  say "zlib placement response:      $(printf '%q' "$zlib_place_reply")"
  say ""
  say "PASS when both 8x4-cell cards are visible and pixel-identical, and the"
  say "zlib transmit reports OK. The raw card is left; f=32,o=z is right."
fi

send_quiet "${ESC}_Ga=d,d=I,i=60,q=2${ST}${ESC}_Ga=d,d=I,i=61,q=2${ST}"
