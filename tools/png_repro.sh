#!/usr/bin/env bash
# png_repro.sh — the empirical gate for #163's pre-encoded transmit path.
#
# Run inside each real terminal in the acceptance matrix: kitty, ghostty,
# wezterm, konsole, xterm, GNOME Terminal, and a bare TTY. The last three are
# negative controls: no image and no Kitty reply is the expected result.
#
#   ./tools/png_repro.sh          # every stanza, with a pause between each
#   ./tools/png_repro.sh 5        # ONE stanza, alone, with no pauses
#   ./tools/png_repro.sh --dump   # emit the wire bytes, touch no terminal
#
# The single-stanza form exists because asking for a capture of stanza 5 and
# getting stanza 3 back costs a whole round trip on real hardware -- five
# settled stanzas and five Enter presses stand between the operator and any
# new material. It is also unambiguous about WHICH script was run.
#
# --dump writes the exact escape stream to stdout and needs no tty, so the
# script can be checked before a human is asked for anything: verify the
# verifier. Narration goes to stderr under --dump, so stdout is pure wire and
# can be diffed or decoded.
#
# TermForge can now hand the terminal an already-encoded payload and let the
# terminal decode it (f=100, PNG) instead of base64'ing raw RGBA (f=32). The
# library never parses the payload, so nothing in the test suite can tell us
# whether a real terminal ACCEPTS what we emit — only a real terminal can.
# That is what this script is for.
#
# Every PNG command uses q=0 on its final chunk so the terminal REPORTS errors
# instead of swallowing them, and each response is echoed. "OK" means accepted.
# This is the production policy since #165: intermediate chunks remain q=2,
# while the final PNG chunk requests the reply that commits driver state. Raw
# RGBA remains q=2 because its length is validated locally and no terminal
# decoder is involved.
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
#
# Stanza 5 is #169: the same pre-encoded payload placed TWICE AT ONCE, once
# stretched and once at native size. See its own comment for why both arms
# have to be on screen together.

set -u

# Parsed before anything else: --dump must not touch the terminal at all, and
# that includes the `stty -g` below, which fails outright with no tty.
dump=0
stanzas=()
for arg in "$@"; do
  case $arg in
    --dump) dump=1 ;;
    [1-5]) stanzas+=("$arg") ;;
    *)
      echo "usage: $0 [--dump] [1-5]..." >&2
      exit 2
      ;;
  esac
done

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

# Stanza 5 (#169): a paletted checkerboard with 4px modules.
#
#  - LEAD WITH RELATIVE SIZE. "is the left one bigger than the right one" is
#    binary, needs no reference, and is true at EVERY cell geometry. It is the
#    only thing the stanza actually asserts.
#  - 4px modules, not 1px. At native size 1px modules are a grey smudge, and
#    an unanswerable question is how an ambiguous report gets written.
#
# 48, NOT 64, and the arithmetic is the whole reason. Module unevenness on an
# axis requires dst*module/src to be non-integral. The first version of this
# stanza used a 64px source into c=11,r=5, and at kitty's nominal 16px cell
# height dst_h = 16r, so dst_h*4/64 = r -- EXACTLY INTEGER for every possible
# r. The vertical modules were uniform by construction while the comment
# claimed "non-integer on both axes", so an honest observer checking rows
# would have reported a partial NO against a perfectly working terminal.
# Caught by computing the resample rather than by looking at it.
#
# With src 48 and module 4 the condition is dst % 12 != 0, which holds on both
# axes at 8x16, 9x18, 10x20 and 7x15. It still fails at 6x12 and 12x24, which
# is why unevenness is asked as a SECONDARY observation below and never as the
# pass criterion.
#
# Paletted (colour type 3) like the plate, because f=100 + colour type 3 is
# the combination the downstream budget actually depends on.
card = paletted_png(48, 48, QUAD,
                    [[0 if ((x // 4) + (y // 4)) % 2 == 0 else 3
                      for x in range(48)] for y in range(48)])

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
emit("card", card)
PY

read_b64() { cat "$WORK/$1.b64"; }
wire_size() { cut -d' ' -f2 "$WORK/$1.size"; }
raw_size() { cut -d' ' -f1 "$WORK/$1.size"; }

saved_stty=''
if ((! dump)); then
  # Hard-fail rather than degrade. Without a tty every send() would return
  # "response: (none)" and the script would print a complete, plausible,
  # exit-0 report of a run that reached no terminal at all -- the single most
  # dangerous output this file can produce, because it is indistinguishable
  # from a terminal that accepted everything silently. Redirecting stdout to
  # capture the transcript is the natural way to hit it.
  saved_stty=$(stty -g 2>/dev/null) || {
    echo "$0: needs a real terminal on stdin (or --dump to emit the wire" >&2
    echo "    with no tty). Refusing to report on a run that cannot happen." >&2
    exit 3
  }
fi
restore() { [[ -n $saved_stty ]] && stty "$saved_stty"; return 0; }
trap 'restore; rm -rf "$WORK"' EXIT

# Narration. Under --dump it goes to stderr, so stdout stays pure wire and can
# be diffed or fed to a decoder.
say() { if ((dump)); then printf '%s\n' "$*" >&2; else printf '%s\n' "$*"; fi; }

# Send a command, then drain and echo any terminal response (readable form).
#
# SAFE ONLY WHERE NO PLACEMENT HAS BEEN DRAWN AT THE CURSOR. It drops raw mode
# before printing, so the response line lands wherever the terminal left the
# cursor -- and a classic (C=1) placement draws at exactly that cell and
# deliberately does not move it. Use send_quiet around anything placed.
send() {
  local label=$1 payload=$2
  if ((dump)); then
    printf '%s' "$payload"
    return 0
  fi
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

# As send(), but leaves the reply in $reply_out instead of printing it, so the
# caller decides WHERE the response line goes. That decision is part of the
# experiment: anything printed between a placement and the observer's look at
# it is painted onto the evidence (#171).
reply_out=''
send_quiet() {
  local payload=$1
  if ((dump)); then
    printf '%s' "$payload"
    reply_out='(dump)'
    return 0
  fi
  stty raw -echo
  printf '%s' "$payload"
  sleep 0.3
  reply_out=''
  local ch
  while IFS= read -r -t 0.05 -n 1 ch; do reply_out+=$ch; done
  stty "$saved_stty"
  [[ -n $reply_out ]] || reply_out='(none)'
}

# Place classically, leaving the cursor BELOW the image and the response line
# with it. Reserve the rows, come back up, place, drop back down: the sequence
# kitty_repro.sh's stanza 6 established, and the fix for #171.
place_below() {
  local label=$1 payload=$2 rows=$3
  local i r
  for ((i = 0; i < rows + 1; i++)); do printf '\n'; done
  printf '%s[%dA' "$ESC" "$((rows + 1))"
  send_quiet "$payload"
  r=$reply_out
  printf '%s[%dB\r' "$ESC" "$((rows + 1))"
  say "$(printf '%s place    response: %q' "$label" "$r")"
}

# Transmit in one shot (payload known to fit a single 4096-char chunk), then
# place classically -- KittyDriver's default placement mode.
transmit_one() {
  local label=$1 id=$2 w=$3 h=$4 fmt=$5 b64=$6
  send "$label transmit" \
    "${ESC}_Ga=t,t=d,f=${fmt},i=${id},s=${w},v=${h},m=0,q=0;${b64}${ST}"
  place_below "$label" "${ESC}_Ga=p,i=${id},p=1,c=8,r=4,C=1,q=0${ST}" 4
}

run_all=1
pause() { (( run_all && ! dump )) && read -r -p "$1"; return 0; }

stanza_1() {
  say "== Stanza 1: f=100, TRUECOLOUR png (colour type 2), single chunk =="
  say "   16x16 four-quadrant test card. If this fails, f=100 is unsupported."
  transmit_one "rgb-png" 90 16 16 100 "$(read_b64 small_rgb)"
  pause "Did a four-colour block appear? Press Enter for stanza 2..."
}

stanza_2() {
  say ""
  say "== Stanza 2: f=100, PALETTED png (colour type 3), single chunk =="
  say "   The SAME test card as stanza 1, pixel for pixel, via a 4-entry PLTE."
  say "   This is the format the downstream 8 KB plate budget depends on."
  transmit_one "pal-png" 91 16 16 100 "$(read_b64 small_pal)"
  pause "Same block AND the same four colours (red/green/blue/yellow)? Enter for stanza 3..."
}

stanza_3() {
  say ""
  say "== Stanza 3: f=100 CHUNKED — a 240x160 4-colour dithered plate =="
  say "   $(raw_size plate) bytes -> $(wire_size plate) base64 chars, so this"
  say "   crosses the 4096 boundary and arrives as two chunks (m=1 then m=0)."
  local big
  big=$(read_b64 plate)
  send "plate chunk1" \
    "${ESC}_Ga=t,t=d,f=100,i=92,s=240,v=160,m=1,q=0;${big:0:4096}${ST}"
  send "plate chunk2" "${ESC}_Gm=0;${big:4096}${ST}"
  place_below "plate" "${ESC}_Ga=p,i=92,p=1,c=30,r=10,C=1,q=0${ST}" 10
  pause "Did the dithered plate render? Press Enter for stanza 4..."
}

stanza_4() {
  say ""
  say "== Stanza 4 (control): the SAME plate as raw RGBA, f=32 =="
  say "   $(raw_size rgba) bytes -> $(wire_size rgba) base64 chars."
  local rgba off total first piece more
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
  # q=2 above, so there is no response to strand -- but the placement still
  # draws at the cursor, so it still goes through place_below.
  place_below "rgba " "${ESC}_Ga=p,i=93,p=1,c=30,r=10,C=1,q=0${ST}" 10
  say "(q=2 on the RGBA chunks: 51 chunks of responses is not useful output.)"
  pause "Press Enter for stanza 5..."
}

# #169 — a pre-encoded payload placed at native size.
#
# ONE transmit, TWO placements, both on screen AT ONCE with distinct placement
# ids. That is the whole design of this stanza. #137's gate was written twice
# before it was right: the first version deleted the stretched placement before
# drawing the exact one, with a pause between, so the observer was asked for
# two verdicts instead of one comparison. Give the eye two things to compare,
# never one to judge.
#
# The left arm carries c=11,r=5 and the right omits c=/r= entirely, which is
# exactly the byte-level difference PlacementFit::Exact makes on the wire.
stanza_5() {
  say ""
  say "== Stanza 5 (#169): the same PNG stretched (left) vs native (right) =="
  say "   48x48 paletted checkerboard, 4px modules, transmitted ONCE as f=100."
  say "   Left  (p=1): c=11,r=5 -- the terminal resamples it to fill the rect."
  say "   Right (p=2): no c=, no r= -- placed at its own 48x48."
  send "card transmit" \
    "${ESC}_Ga=t,t=d,f=100,i=94,s=48,v=48,m=0,q=0;$(read_b64 card)${ST}"

  # 8 rows reserved, not 5. The left arm is 5 rows by construction (r=5), but
  # the right arm is 48 PIXELS and its height in rows is ceil(48/cell_h),
  # which this script cannot know -- it is the one placement here not sized in
  # cells, which is the entire point of it. 8 covers every cell height down to
  # 6px. Under-reserving would print the response lines over the bottom of the
  # native arm, which is #171 reintroduced on the one arm that matters most.
  local r5a r5b i
  for ((i = 0; i < 8; i++)); do printf '\n'; done
  printf '%s[8A' "$ESC"
  send_quiet "${ESC}_Ga=p,i=94,p=1,c=11,r=5,C=1,q=0${ST}"
  r5a=$reply_out
  printf '%s[14C' "$ESC"
  send_quiet "${ESC}_Ga=p,i=94,p=2,C=1,q=0${ST}"
  r5b=$reply_out
  printf '%s[8B\r' "$ESC"

  say "$(printf 'place(c=11,r=5) response: %q' "$r5a")"
  say "$(printf 'place(no c/r)   response: %q' "$r5b")"
  say ""
  say "   THE QUESTION: (a) are there TWO checkerboards side by side, and"
  say "   (b) is the LEFT one visibly LARGER than the right one?"
  say ""
  say "   (b) is the pass criterion and the only one. If it holds, omitting"
  say "   c=/r= placed the image at its own resolution, which is the whole"
  say "   ticket."
  say ""
  say "   Secondary, worth noting but NOT a pass criterion: the left arm's"
  say "   squares should look slightly uneven and the right arm's uniform."
  say "   That depends on your cell size dividing the resample -- at some"
  say "   cell geometries the left arm is evenly scaled too. Even squares on"
  say "   the left are NOT a failure; a left arm the same size as the right"
  say "   one is."
  pause "Press Enter to finish..."
}

report_numbers() {
  say ""
  say "── the numbers ─────────────────────────────────────────────────────────"
  say "$(printf '  paletted PNG plate : %6s bytes  -> %6s on the wire' \
    "$(raw_size plate)" "$(wire_size plate)")"
  say "$(printf '  same plate as RGBA : %6s bytes  -> %6s on the wire' \
    "$(raw_size rgba)" "$(wire_size rgba)")"
  say "$(python3 -c "
p=int(open('$WORK/plate.size').read().split()[1])
r=int(open('$WORK/rgba.size').read().split()[1])
print('  ratio              : %.1fx cheaper on the wire' % (r/p))
")"
}

if ((${#stanzas[@]})); then
  run_all=0
  for n in "${stanzas[@]}"; do "stanza_$n"; done
else
  for n in 1 2 3 4 5; do "stanza_$n"; done
  report_numbers
  say ""
  say "Please report, for each stanza: the response line and whether the image"
  say "rendered. A response of ';OK' means the command was accepted."
fi
