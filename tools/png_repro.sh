#!/usr/bin/env bash
# png_repro.sh — the empirical gate for #163's pre-encoded transmit path.
#
# Run inside a real kitty-graphics terminal (kitty, ghostty, wezterm, konsole):
#
#   ./tools/png_repro.sh
#
# TermForge can now hand the terminal an already-encoded payload and let the
# terminal decode it (f=100, PNG) instead of base64'ing raw RGBA (f=32). The
# library never parses the payload, so nothing in the test suite can tell us
# whether a real terminal ACCEPTS what we emit — only a real terminal can.
# That is what this script is for.
#
# Every command uses q=0 so the terminal REPORTS errors instead of swallowing
# them, and each response is echoed. "OK" means accepted. Note that TermForge
# itself emits q=2 in normal operation, which means a rejected payload is
# SILENT in production (see #163's follow-up) — all the more reason to settle
# acceptance here.
#
# ── the three uncertainties this answers ────────────────────────────────────
#
#  1. Does the terminal accept f=100 at all? The kitty spec has required PNG
#     support for years, but "the spec says so" is not evidence.
#
#  2. Does it accept a PALETTED PNG (colour type 3)? This is the interesting
#     one. The format that gets a 240x160 plate under a downstream 8 KB budget
#     is 4 colours + ordered dither, which is colour type 3 — not the RGB/RGBA
#     PNG an implementation is most likely to have tested. A terminal that
#     only handles truecolour PNG would pass stanza 1 and fail stanza 2.
#
#  3. Does a CHUNKED PNG behave like chunked RGBA? Stanza 3's payload crosses
#     the 4096-byte boundary, so the terminal must reassemble two chunks
#     before decoding rather than after — a different code path from the raw
#     formats, where each chunk is independently meaningful.
#
# Stanza 4 is the control: the same visual plate as raw RGBA, so the byte
# counts printed at the end are a like-for-like comparison rather than an
# estimate.

set -u

command -v python3 >/dev/null || {
  echo "png_repro.sh needs python3 to bake the test images (stdlib zlib only)." >&2
  exit 1
}

ESC=$'\033'
ST="${ESC}\\" # string terminator

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ── bake the payloads ───────────────────────────────────────────────────────
#
# Generated rather than checked in: these are regenerable from 40 lines of
# stdlib, and the repo has already had one accident with committed binary art.
# zlib and struct are both stdlib, so this needs nothing installed.
python3 - "$WORK" <<'PY'
import base64, math, os, struct, sys, zlib

out = sys.argv[1]

def chunk(t, d):
    c = t + d
    return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

def paletted_png(w, h, pal, rows):
    """Colour type 3: an 8-bit palette index per pixel, 4-entry PLTE."""
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0)
    plte = bytes(b for c in pal for b in c)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)   # filter 0 per scanline
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"PLTE", plte)
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

def truecolour_png(w, h, rgb_rows):
    """Colour type 2, for the 'does it need truecolour?' comparison."""
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    raw = b"".join(b"\x00" + bytes(b for px in r for b in px) for r in rgb_rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

PAL = [(20, 24, 34), (90, 70, 110), (200, 120, 90), (245, 230, 200)]
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]

# Stanzas 1 and 2 are ONE test card in two encodings, and both are built from
# the single `qidx` below so they cannot drift apart. Same geometry AND same
# colours is the whole point: it makes stanza 2 a DIFFERENTIAL test against
# stanza 1, where the observer compares two images rather than judging one.
# "Did four blocks appear?" is satisfied by a terminal that ignored the PLTE
# entirely; "are these the same four colours as a moment ago?" is not.
#
# The first version of this script gave stanza 2 the plate's dither palette
# (PAL) instead, so a correct decode rendered in different colours, the
# "same block again?" prompt was truthfully answered NO, and a passing
# terminal read as a failed merge gate. A verification tool that asks a
# misleading question manufactures the failure it was written to detect.
QUAD = [(220, 60, 60), (60, 200, 120), (60, 110, 220), (240, 220, 80)]
qidx = [[0 if (x < 8 and y < 8) else 1 if (x >= 8 and y < 8) else
         2 if (x < 8) else 3 for x in range(16)] for y in range(16)]

# Stanza 1: 16x16 truecolour, four coloured quadrants. Deliberately the
# simplest possible PNG -- if this fails, f=100 is unsupported outright.
small_rgb = truecolour_png(16, 16, [[QUAD[i] for i in row] for row in qidx])

# Stanza 2: that exact card as a PALETTED png -- QUAD becomes the 4-entry
# PLTE and the quadrants become 8-bit palette indices into it.
small_pal = paletted_png(16, 16, QUAD, qidx)

# Stanza 3: 240x160, 4 colours, ordered dither -- the downstream plate spec.
# Radial with texture rather than a smooth ramp, because a smooth ramp zlibs
# down to ~300 bytes and would never cross the 4096-byte chunk boundary this
# stanza exists to exercise.
rows = []
for y in range(160):
    row = []
    for x in range(240):
        dx, dy = (x - 120) / 120.0, (y - 80) / 80.0
        v = max(0.0, min(1.0, 1.15 - math.sqrt(dx * dx + dy * dy)))
        v += 0.10 * math.sin(x * 0.7) * math.cos(y * 0.9)
        v += 0.10 * ((((x * 7919) ^ (y * 104729)) % 97) / 97.0 - 0.5)
        t = (BAYER[y % 4][x % 4] + 0.5) / 16.0
        row.append(max(0, min(3, int(v * 3.999 + (t - 0.5) * 0.9))))
    rows.append(row)
plate = paletted_png(240, 160, PAL, rows)

# Stanza 4 control: the identical plate as raw RGBA.
rgba = bytearray()
for r in rows:
    for i in r:
        rgba += bytes(PAL[i]) + b"\xff"

def emit(name, data):
    b64 = base64.b64encode(data).decode()
    with open(os.path.join(out, name + ".b64"), "w") as f:
        f.write(b64)
    with open(os.path.join(out, name + ".size"), "w") as f:
        f.write("%d %d" % (len(data), len(b64)))

emit("small_rgb", small_rgb)
emit("small_pal", small_pal)
emit("plate", plate)
emit("rgba", bytes(rgba))
PY

read_b64() { cat "$WORK/$1.b64"; }
wire_size() { cut -d' ' -f2 "$WORK/$1.size"; }
raw_size() { cut -d' ' -f1 "$WORK/$1.size"; }

saved_stty=$(stty -g)
restore() { stty "$saved_stty"; }
trap 'restore; rm -rf "$WORK"' EXIT

# Send a command, then drain and echo any terminal response (readable form).
send() {
  local label=$1 payload=$2
  stty raw -echo
  printf '%s' "$payload"
  sleep 0.3
  local reply='' ch
  while IFS= read -r -t 0.05 -n 1 ch; do reply+=$ch; done
  stty "$saved_stty"
  if [[ -n $reply ]]; then
    printf '%s response: %q\n' "$label" "$reply"
  else
    printf '%s response: (none)\n' "$label"
  fi
}

# Transmit in one shot (payload known to fit a single 4096-char chunk), then
# place classically at the cursor -- KittyDriver's default placement mode.
transmit_one() {
  local label=$1 id=$2 w=$3 h=$4 fmt=$5 b64=$6
  send "$label transmit" \
    "${ESC}_Ga=t,t=d,f=${fmt},i=${id},s=${w},v=${h},m=0,q=0;${b64}${ST}"
  send "$label place   " "${ESC}_Ga=p,i=${id},p=1,c=8,r=4,C=1,q=0${ST}"
  printf '\n\n\n\n\n'
}

echo "== Stanza 1: f=100, TRUECOLOUR png (colour type 2), single chunk =="
echo "   16x16 four-quadrant test card. If this fails, f=100 is unsupported."
transmit_one "rgb-png" 90 16 16 100 "$(read_b64 small_rgb)"
read -r -p "Did a four-colour block appear? Press Enter for stanza 2..."

echo
echo "== Stanza 2: f=100, PALETTED png (colour type 3), single chunk =="
echo "   The SAME test card as stanza 1, pixel for pixel, via a 4-entry PLTE."
echo "   This is the format the downstream 8 KB plate budget depends on."
transmit_one "pal-png" 91 16 16 100 "$(read_b64 small_pal)"
read -r -p "Same block AND the same four colours (red/green/blue/yellow)? Enter for stanza 3..."

echo
echo "== Stanza 3: f=100 CHUNKED — a 240x160 4-colour dithered plate =="
echo "   $(raw_size plate) bytes -> $(wire_size plate) base64 chars, so this"
echo "   crosses the 4096 boundary and arrives as two chunks (m=1 then m=0)."
big=$(read_b64 plate)
send "plate chunk1" \
  "${ESC}_Ga=t,t=d,f=100,i=92,s=240,v=160,m=1,q=0;${big:0:4096}${ST}"
send "plate chunk2" "${ESC}_Gm=0;${big:4096}${ST}"
send "plate place " "${ESC}_Ga=p,i=92,p=1,c=30,r=10,C=1,q=0${ST}"
printf '\n\n\n\n\n\n\n\n\n\n\n'
read -r -p "Did the dithered plate render? Press Enter for stanza 4..."

echo
echo "== Stanza 4 (control): the SAME plate as raw RGBA, f=32 =="
echo "   $(raw_size rgba) bytes -> $(wire_size rgba) base64 chars."
rgba=$(read_b64 rgba)
off=0
total=${#rgba}
first=1
while ((off < total)); do
  piece=${rgba:off:4096}
  if ((off + 4096 < total)); then more=1; else more=0; fi
  if ((first)); then
    printf '%s' \
      "${ESC}_Ga=t,t=d,f=32,i=93,s=240,v=160,m=${more},q=2;${piece}${ST}"
    first=0
  else
    printf '%s' "${ESC}_Gm=${more};${piece}${ST}"
  fi
  ((off += 4096))
done
printf '%s' "${ESC}_Ga=p,i=93,p=1,c=30,r=10,C=1,q=0${ST}"
printf '\n\n\n\n\n\n\n\n\n\n\n'
echo "(q=2 on the RGBA chunks: 51 chunks of responses is not useful output.)"

echo
echo "── the numbers ─────────────────────────────────────────────────────────"
printf '  paletted PNG plate : %6s bytes  -> %6s on the wire\n' \
  "$(raw_size plate)" "$(wire_size plate)"
printf '  same plate as RGBA : %6s bytes  -> %6s on the wire\n' \
  "$(raw_size rgba)" "$(wire_size rgba)"
python3 -c "
import sys
p=int(open('$WORK/plate.size').read().split()[1])
r=int(open('$WORK/rgba.size').read().split()[1])
print('  ratio              : %.1fx cheaper on the wire' % (r/p))
"
echo
echo "Please report, for each stanza: the response line and whether the image"
echo "rendered. A response of ';OK' means the command was accepted."
