#!/usr/bin/env bash
# kitty_repro.sh — minimal standalone repros for the kitty graphics paths that
# KittyDriver emits. Run inside a real kitty (>= 0.28) terminal:
#
#   ./tools/kitty_repro.sh          # all six stanzas, with a pause between each
#   ./tools/kitty_repro.sh 6        # ONLY stanza 6
#   ./tools/kitty_repro.sh 3 4      # a subset, in the order given
#   ./tools/kitty_repro.sh --dump   # emit the wire bytes, touch no terminal
#
# --dump needs no tty, so what this script EMITS can be checked before a human
# is asked to look at anything. That is not a luxury: this file has been the
# fault of three consecutive release gates, and in every case the terminal was
# working. Narration goes to stderr under --dump, so stdout is pure wire.
#
# Take the argument form when only one stanza is in question. Most of this file
# is settled history re-run for context, and walking an observer through five
# known-good stanzas to reach the one that is actually being asked about is how
# the wrong thing gets reported.
#
#   1  RGBA transmit + VIRTUAL placement (U=1) with U+10EEEE placeholder cells,
#      the id encoded as a 24-bit foreground — what place_unicode emits.
#   2  retransmit the same id with new pixels and do NOT re-place: does an
#      existing placement refresh? (needs 1)
#   3  CLASSIC placement (C=1) at the cursor — KittyDriver's default path.
#   4  retransmit + delete + re-place, the sequence KittyDriver uses on a
#      content change, because 2's answer was no. (needs 3)
#   5  stanza 1's placeholders with the 38;5 id encoding kitten icat uses for
#      ids < 256, to tell an id-encoding rejection from a placement one. (needs 1)
#   6  #137 — c=/r= OMITTED so the terminal places at true size, against the
#      same image stretched, side by side. Self-contained.
#
# All commands use q=0 so kitty REPORTS errors; every response the terminal
# sends is captured and echoed in readable form. A response of "_Gi=42;OK"
# means the command was accepted.

set -u

# Parsed first: --dump must touch nothing, and `stty -g` below fails outright
# with no tty.
dump=0
stanzas=()
for arg in "$@"; do
  case $arg in
    --dump) dump=1 ;;
    [1-6]) stanzas+=("$arg") ;;
    *)
      echo "usage: $0 [--dump] [1-6]..." >&2
      exit 2
      ;;
  esac
done

ESC=$'\033'
ST="${ESC}\\"          # string terminator
PH=$'\xf4\x8f\xbb\xae' # U+10EEEE placeholder
D0=$'\xcc\x85'         # diacritic index 0: U+0305
D1=$'\xcc\x8d'         # diacritic index 1: U+030D

red_b64=$(printf '\xff\x00\x00\xff\xff\x00\x00\xff\xff\x00\x00\xff\xff\x00\x00\xff' | base64)
green_b64=$(printf '\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff' | base64)
blue_b64=$(printf '\x00\x00\xff\xff\x00\x00\xff\xff\x00\x00\xff\xff\x00\x00\xff\xff' | base64)
yellow_b64=$(printf '\xff\xff\x00\xff\xff\xff\x00\xff\xff\xff\x00\xff\xff\xff\x00\xff' | base64)

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
trap restore EXIT

# Narration. Under --dump it goes to stderr so stdout stays pure wire.
say() { if ((dump)); then printf '%s\n' "$*" >&2; else printf '%s\n' "$*"; fi; }

# Send a command, then drain and echo any terminal response (readable form).
#
# SAFE ONLY WHERE NO PLACEMENT HAS BEEN DRAWN AT THE CURSOR -- see place_below.
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

# As send(), but leaves the reply in $reply_out instead of printing it. For the
# case where the cursor position between two commands is load-bearing (stanza
# 6 places two images side by side) and an echoed response line would both move
# the cursor and paint over the image classic placement draws at it.
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

# Place classically, leaving the cursor BELOW the image and printing the
# response line there rather than on top of it (#171).
#
# The bug this replaces was subtle enough to survive review twice: send()
# drops raw mode BEFORE printing, so its response line starts wherever the
# terminal left the cursor -- and a C=1 placement draws its top-left corner at
# exactly that cell and deliberately does not move the cursor. The image was
# therefore always stamped over by the report of its own success. Reserve the
# rows, come back up, place, drop below, and only then speak.
place_below() {
  local label=$1 payload=$2 rows=$3
  local i r
  for ((i = 0; i < rows + 1; i++)); do printf '\n'; done
  printf '%s[%dA' "$ESC" "$((rows + 1))"
  send_quiet "$payload"
  r=$reply_out
  printf '%s[%dB\r' "$ESC" "$((rows + 1))"
  say "$(printf '%s response: %q' "$label" "$r")"
}

# Pauses only exist to separate stanzas in a full run. Selecting stanzas
# explicitly means the observer already knows what they are looking at.
run_all=1
pause() { (( run_all && ! dump )) && read -r -p "$1"; return 0; }

# The 2x2 placeholder grid for image id 42, given an SGR foreground that
# encodes the id. Stanzas 1 and 5 differ ONLY in that argument, so they share
# this rather than each spelling the diacritics out.
placeholder_grid() {
  printf '%s%s' "$ESC" "$1"
  printf '%s%s%s%s%s%s' "$PH" "$D0" "$D0" "$PH" "$D0" "$D1"   # row 0: cols 0,1
  printf '\n'
  printf '%s%s%s%s%s%s' "$PH" "$D1" "$D0" "$PH" "$D1" "$D1"   # row 1: cols 0,1
  printf '%s[0m\n' "$ESC"
}

stanza_1() {
  say "== Stanza 1: red 2x2 image, virtual placement, placeholder cells =="
  send "transmit(red)" "${ESC}_Ga=t,t=d,f=32,i=42,s=2,v=2,m=0,q=0;${red_b64}${ST}"
  send "place(U=1)   " "${ESC}_Ga=p,i=42,p=1,U=1,c=2,r=2,q=0${ST}"
  placeholder_grid '[38;2;0;0;42m'
  say ""
  say "You should see a 2x2-cell RED block above."
  pause "Press Enter for stanza 2 (retransmit same id as GREEN)..."
}

stanza_2() {
  say "== Stanza 2: retransmit id 42 as green (no new placement) =="
  send "transmit(grn)" "${ESC}_Ga=t,t=d,f=32,i=42,s=2,v=2,m=0,q=0;${green_b64}${ST}"
  say ""
  say "Did the red block turn GREEN?"
  pause "Press Enter for stanza 3 (classic placement, KittyDriver's default)..."
}

stanza_3() {
  say "== Stanza 3: classic placement — transmit id 43, place at cursor with c=2,r=2,C=1 =="
  send "transmit(blu)" "${ESC}_Ga=t,t=d,f=32,i=43,s=2,v=2,m=0,q=0;${blue_b64}${ST}"
  place_below "place(C=1)   " "${ESC}_Ga=p,i=43,p=1,c=2,r=2,C=1,q=0${ST}" 2
  say ""
  pause "Press Enter for stanza 4 (retransmit id 43 as YELLOW — classic refresh)..."
}

stanza_4() {
  say "== Stanza 4: retransmit id 43 as yellow + recreate the placement =="
  # Finding from earlier runs: kitty replaces the image data on retransmit
  # but does NOT refresh an existing classic placement — so KittyDriver now
  # deletes and recreates the placement on every content change. This stanza
  # mirrors that exact sequence.
  send "transmit(yel)" "${ESC}_Ga=t,t=d,f=32,i=43,s=2,v=2,m=0,q=0;${yellow_b64}${ST}"
  send "del place    " "${ESC}_Ga=d,d=i,i=43,p=1,q=0${ST}"
  place_below "place(C=1)   " "${ESC}_Ga=p,i=43,p=1,c=2,r=2,C=1,q=0${ST}" 2
  say ""
  say "A YELLOW block should appear at the cursor (the old blue one is gone"
  say "with its deleted placement)."
  pause "Press Enter for stanza 5 (placeholders again, 38;5 id encoding)..."
}

stanza_5() {
  say "== Stanza 5: placeholder cells for id 42 with 256-color id encoding =="
  # Same virtual placement as stanza 1 (already created); only the cell fg
  # encoding differs: 38;5;42 instead of 38;2;0;0;42. kitten icat uses this
  # form for ids < 256 — if THIS renders where stanza 1 didn't, the 24-bit
  # id encoding is what this kitty rejects.
  placeholder_grid '[38;5;42m'
  say ""
  pause "Press Enter for stanza 6 (PlacementFit::Exact — #137)..."
}

stanza_6() {
  say ""
  say "== Stanza 6: c=/r= omitted, so the terminal places at TRUE SIZE =="
  # #137. KittyDriver emitted c=/r= on every placement, which made the terminal
  # resample — correct for a widget that generates its image, wrong for one the
  # app ships pre-rendered. Omitting the two keys is the kitty protocol's own
  # spelling of "place at native resolution"; this stanza is the empirical check
  # that a real terminal honours it.
  #
  # A checkerboard is the test pattern on purpose: it is the cheapest thing whose
  # STRUCTURE a non-integer resample visibly destroys. A solid block would look
  # identical either way and prove nothing. The modules are 2px rather than 1px
  # so that "are the squares the same size" is answerable by eye at native size.
  #
  # BOTH placements are on screen at once, side by side, under distinct placement
  # ids (p=1 and p=2) — never one and then the other. #163's capture reported a
  # failure against a terminal that was working perfectly, because it asked the
  # observer to JUDGE one image instead of COMPARE two. Same file, same trap.
  local w='\xff\xff\xff\xff' k='\x00\x00\x00\xff'
  local row_a='' row_b='' checker='' _
  for _ in $(seq 4); do row_a+="$w$w$k$k"; row_b+="$k$k$w$w"; done
  for _ in $(seq 4); do checker+="$row_a$row_a$row_b$row_b"; done
  local check_b64
  check_b64=$(printf "$checker" | base64 | tr -d '\n')

  send "transmit(chk)" \
    "${ESC}_Ga=t,t=d,f=32,i=44,s=16,v=16,m=0,q=0;${check_b64}${ST}"

  say "  LEFT  (p=1) — STRETCHED into 5x3 cells (c=5,r=3). 16px across 5 cells"
  say "                is not an integer ratio, so the 2px squares come out"
  say "                UNEVEN — 5px wide in places, 6px in others, and the rows"
  say "                likewise. That unevenness is the bug #137 fixes."
  say "  RIGHT (p=2) — EXACT: the identical a=p with c= and r= simply removed."
  say "                Expect a much SMALLER block (16x16 device pixels, about"
  say "                two cells) with every square the same size."
  say ""

  # Reserve rows for the images, then step back up to the top of them. Both
  # placements use C=1 so the cursor does not move, which is what lets the second
  # be positioned relative to the first. The responses are held back and printed
  # below the block: send()'s own echo would otherwise land on top of the images.
  local r6a r6b
  printf '\n\n\n\n\n'
  printf '%s[5A' "$ESC"
  send_quiet "${ESC}_Ga=p,i=44,p=1,c=5,r=3,C=1,q=0${ST}"; r6a=$reply_out
  printf '%s[7C' "$ESC"
  send_quiet "${ESC}_Ga=p,i=44,p=2,C=1,q=0${ST}"; r6b=$reply_out
  printf '%s[5B\r' "$ESC"

  say "$(printf 'place(c=5,r=3) response: %q' "$r6a")"
  say "$(printf 'place(no c/r)  response: %q' "$r6b")"
  say ""
  say "Report, COMPARING the two blocks side by side: did the RIGHT one render"
  say "at all — and if so, is it much smaller than the left with even, crisp"
  say "squares, while the left is larger with uneven ones? A blank right-hand"
  say "side is a real answer, not a bug in this script; it would mean this"
  say "kitty needs c=/r=. Also report any response containing ';E' (an error)."
}

if (( ${#stanzas[@]} )); then
  run_all=0
  for n in "${stanzas[@]}"; do "stanza_$n"; done
else
  for n in 1 2 3 4 5 6; do "stanza_$n"; done
  say ""
  say "Report: (a) stanza 1 red block, (b) green after stanza 2, (c) stanza 3"
  say "blue block, (d) YELLOW block after stanza 4, (e) stanza 5 shows a"
  say "green 2x2 block, (f) stanza 6 as described above, (g) any response"
  say "containing ';E' (an error)."
fi
