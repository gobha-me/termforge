#!/usr/bin/env bash
# kitty_repro.sh — minimal standalone repros for the kitty graphics paths that
# KittyDriver emits. Run inside a real kitty (>= 0.28) terminal:
#
#   ./tools/kitty_repro.sh          # all twelve stanzas, with pauses
#   ./tools/kitty_repro.sh 12       # ONLY stanza 12
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
#   7  #109 — a=d,d=i retires ONE placement of three and the image DATA
#      survives, proven by placing a fourth with no retransmit. The assumption
#      the whole resident-image feature rests on. Self-contained.
#   8  #199 — compare U+10FEEE with the spec's U+10EEEE under BOTH 38;5 and
#      38;2 id encoding, including an id above 255. Self-contained; run before
#      changing KittyDriver.
#   9  #201 — clear a retired placeholder grid BEFORE same-frame replacement
#      text, then reuse its id at a new rect. Self-contained.
#  10  #261 — edit a pinned image's root frame over MULTIPLE chunks with
#      a=f,r=1,X=1 and verify that its existing classic placement refreshes
#      without re-placement. Self-contained and crosses the game's protocol seam.
#  11  #140 — overwrite and alpha-compose two pixel-offset blocks into one
#      resident root without retransmitting or replacing its placement.
#  12  #114 — three classic placements prove above-text, below-text and
#      below-non-default-background named layers. Self-contained.
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
    [1-9]|10|11|12) stanzas+=("$arg") ;;
    *)
      echo "usage: $0 [--dump] [1-9|10|11|12]..." >&2
      exit 2
      ;;
  esac
done

ESC=$'\033'
ST="${ESC}\\"          # string terminator
PH_LEGACY=$'\xf4\x8f\xbb\xae' # U+10FEEE: pre-#199 KittyDriver bytes
PH_SPEC=$'\xf4\x8e\xbb\xae'   # U+10EEEE: kitty protocol placeholder
D0=$'\xcc\x85'         # diacritic index 0: U+0305
D1=$'\xcc\x8d'         # diacritic index 1: U+030D

red_b64=$(printf '\xff\x00\x00\xff\xff\x00\x00\xff\xff\x00\x00\xff\xff\x00\x00\xff' | base64)
green_b64=$(printf '\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff' | base64)
blue_b64=$(printf '\x00\x00\xff\xff\x00\x00\xff\xff\x00\x00\xff\xff\x00\x00\xff\xff' | base64)
yellow_b64=$(printf '\xff\xff\x00\xff\xff\xff\x00\xff\xff\xff\x00\xff\xff\xff\x00\xff' | base64)
magenta_b64=$(printf '\xff\x00\xff\xff\xff\x00\xff\xff\xff\x00\xff\xff\xff\x00\xff\xff' | base64)

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
  local sgr=$1 placeholder=${2:-$PH_SPEC}
  printf '%s%s' "$ESC" "$sgr"
  printf '%s%s%s%s%s%s' "$placeholder" "$D0" "$D0" "$placeholder" "$D0" "$D1"   # row 0: cols 0,1
  printf '\n'
  printf '%s%s%s%s%s%s' "$placeholder" "$D1" "$D0" "$placeholder" "$D1" "$D1"   # row 1: cols 0,1
  printf '%s[0m\n' "$ESC"
}

# A grid whose top-left cursor is already selected, with an explicit 1-based
# column for the second row. Unlike placeholder_grid(), this does not use LF:
# the tty's ONLCR output processing would return that row to column 1 and turn
# an intentionally offset grid into what looks exactly like a stale-cell ghost.
placeholder_grid_at() {
  local sgr=$1 column=$2 placeholder=${3:-$PH_SPEC}
  printf '%s%s' "$ESC" "$sgr"
  printf '%s%s%s%s%s%s' "$placeholder" "$D0" "$D0" "$placeholder" "$D0" "$D1"
  printf '%s[1B%s[%dG' "$ESC" "$ESC" "$column"
  printf '%s%s%s%s%s%s' "$placeholder" "$D1" "$D0" "$placeholder" "$D1" "$D1"
  printf '%s[0m' "$ESC"
}

# Two copies of the same image side by side. Only the placeholder codepoint
# differs, so a visible left/right difference answers #199 without sharing a
# constant with the driver or asking the observer to compare across time.
placeholder_pair() {
  local sgr=$1 row placeholder
  for row in 0 1; do
    for placeholder in "$PH_LEGACY" "$PH_SPEC"; do
      printf '%s%s' "$ESC" "$sgr"
      if ((row == 0)); then
        printf '%s%s%s%s%s%s' "$placeholder" "$D0" "$D0" \
          "$placeholder" "$D0" "$D1"
      else
        printf '%s%s%s%s%s%s' "$placeholder" "$D1" "$D0" \
          "$placeholder" "$D1" "$D1"
      fi
      printf '%s[0m' "$ESC"
      [[ $placeholder == "$PH_LEGACY" ]] && printf '    '
    done
    printf '\n'
  done
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

stanza_7() {
  say ""
  say "== Stanza 7: d=i retires ONE placement and the image DATA survives =="
  # #109. This is the assumption the whole resident-image feature rests on, and
  # it is one letter wide: a=d,d=I frees the image data and its placements,
  # a=d,d=i retires the placements matching i=/p= and leaves the data. If this
  # terminal treats the two the same, a pinned image silently disappears the
  # first time one of its placements is collected — and q=2 in production means
  # nobody hears about it.
  #
  # Self-contained: transmits its own id (45) so it can be run alone.
  #
  # THREE placements of ONE image, then the middle one deleted, then a FOURTH
  # created from the surviving data with no retransmit. Deleting one of three
  # rather than one of one is deliberate: with a single placement, "the data is
  # gone" and "the placement is gone" look identical on screen, which is exactly
  # the confusion this stanza exists to resolve.
  send "transmit(mag)" \
    "${ESC}_Ga=t,t=d,f=32,i=45,s=2,v=2,m=0,q=0;${magenta_b64}${ST}"

  local r1 r2 r3 rd r4
  printf '\n\n\n'
  printf '%s[3A' "$ESC"
  send_quiet "${ESC}_Ga=p,i=45,p=1,c=2,r=2,C=1,q=0${ST}"; r1=$reply_out
  printf '%s[4C' "$ESC"
  send_quiet "${ESC}_Ga=p,i=45,p=2,c=2,r=2,C=1,q=0${ST}"; r2=$reply_out
  printf '%s[4C' "$ESC"
  send_quiet "${ESC}_Ga=p,i=45,p=3,c=2,r=2,C=1,q=0${ST}"; r3=$reply_out
  say ""
  say "  Three magenta blocks should be on the row above. Deleting the MIDDLE"
  say "  one (p=2) next — with d=i, the lowercase form."
  pause "  Press Enter to delete placement p=2..."

  send_quiet "${ESC}_Ga=d,d=i,i=45,p=2,q=0${ST}"; rd=$reply_out
  say ""
  say "  The middle block should be gone and the OUTER TWO still there."
  say "  Now placing a FOURTH from the same id, with NO retransmit — if the"
  say "  data survived, it renders; if d=i freed it, nothing appears."
  pause "  Press Enter to place p=4 from the surviving data..."

  printf '\n\n\n'
  printf '%s[3A' "$ESC"
  send_quiet "${ESC}_Ga=p,i=45,p=4,c=2,r=2,C=1,q=0${ST}"; r4=$reply_out
  printf '%s[3B\r' "$ESC"

  say "$(printf 'place p=1 response: %q' "$r1")"
  say "$(printf 'place p=2 response: %q' "$r2")"
  say "$(printf 'place p=3 response: %q' "$r3")"
  say "$(printf 'delete  p=2 response: %q' "$rd")"
  say "$(printf 'place p=4 response: %q' "$r4")"
  say ""
  say "Report three things separately, because they fail independently:"
  say "  (a) did the MIDDLE block disappear and the outer two stay?"
  say "  (b) did a FOURTH block render afterwards with no retransmit?"
  say "  (c) any response containing ';E'."
  say "A 'no' to (b) means d=i freed the data on this terminal, and #109's"
  say "placement-only delete is unsafe here — that is a real answer, not a"
  say "bug in this script."
}

stanza_8() {
  say ""
  say "== Stanza 8: #199 placeholder codepoint x id-encoding matrix =="
  say "Each left/right pair names the SAME red image and virtual placement."
  say "Columns differ only by codepoint; pairs differ by SGR encoding/id:"
  say "                 LEFT U+10FEEE      RIGHT U+10EEEE (spec)"
  say "  TOP row pair:  38;5;42             38;5;42"
  say "  BOTTOM pair:   38;2;0;0;42         38;2;0;0;42"
  say "  FINAL pair:    38;2;0;1;44 (id 300, above the old one-byte budget)"
  send "transmit(red)" \
    "${ESC}_Ga=t,t=d,f=32,i=42,s=2,v=2,m=0,q=0;${red_b64}${ST}"
  send "place(U=1)   " "${ESC}_Ga=p,i=42,p=1,U=1,c=2,r=2,q=0${ST}"
  send "transmit(300)" \
    "${ESC}_Ga=t,t=d,f=32,i=300,s=2,v=2,m=0,q=0;${red_b64}${ST}"
  send "place(300)   " "${ESC}_Ga=p,i=300,p=1,U=1,c=2,r=2,q=0${ST}"
  say ""
  say "  38;5 pair (left legacy, right spec):"
  placeholder_pair '[38;5;42m'
  say "  38;2 pair (left legacy, right spec):"
  placeholder_pair '[38;2;0;0;42m'
  say "  38;2 id=300 pair (left legacy, right spec):"
  placeholder_pair '[38;2;0;1;44m'
  say ""
  say "Report which of the six labelled blocks renders a RED 2x2 block."
  say "Also report any response containing ';E'. A blank block is an answer:"
  say "do not infer it from another block or from an OK response."
}

stanza_9() {
  say ""
  say "== Stanza 9: #201 retired placeholder cells are cleared before reuse =="
  say "A RED block appears on the left. After the pause, that same 2x2 cell"
  say "rect becomes the text OK while the SAME image id renders GREEN on the"
  say "right. A green ghost behind/around OK means the old grid survived."

  local rt rp rd rg gp
  printf '\n\n\n\n'
  printf '%s[4A%s7' "$ESC" "$ESC"  # old rect origin, then save it
  send_quiet "${ESC}_Ga=t,t=d,f=32,i=46,s=2,v=2,m=0,q=0;${red_b64}${ST}"; rt=$reply_out
  send_quiet "${ESC}_Ga=p,i=46,p=1,U=1,c=2,r=2,q=0${ST}"; rp=$reply_out
  placeholder_grid_at '[38;5;46m' 1
  printf '%s8%s[4B\r' "$ESC" "$ESC"
  pause "Press Enter to clear the old grid, write OK, and reuse id 46..."

  # Restore the old rect. These are the implementation's logical phases in
  # their required order: clear grid, replacement text, delete/reuse, new grid.
  printf '%s8' "$ESC"
  printf '%s[0m%s[38;2;224;224;240m%s[48;2;10;10;20m  ' "$ESC" "$ESC" "$ESC"
  printf '%s8%s[1B  ' "$ESC" "$ESC"
  printf '%s8%s[0m%s[38;2;255;255;255mOK' "$ESC" "$ESC" "$ESC"
  send_quiet "${ESC}_Ga=d,d=I,i=46,q=0${ST}"; rd=$reply_out
  send_quiet "${ESC}_Ga=t,t=d,f=32,i=46,s=2,v=2,m=0,q=0;${green_b64}${ST}"; rg=$reply_out
  send_quiet "${ESC}_Ga=p,i=46,p=2,U=1,c=2,r=2,q=0${ST}"; gp=$reply_out
  printf '%s8%s[6C' "$ESC" "$ESC"
  placeholder_grid_at '[38;5;46m' 7
  printf '%s8%s[4B\r' "$ESC" "$ESC"

  say "$(printf 'transmit(red) response: %q' "$rt")"
  say "$(printf 'place(red)    response: %q' "$rp")"
  say "$(printf 'delete(red)   response: %q' "$rd")"
  say "$(printf 'transmit(grn) response: %q' "$rg")"
  say "$(printf 'place(grn)    response: %q' "$gp")"
  say "Report: (a) left reads OK with no green ghost, (b) right is a green"
  say "2x2-cell block, and (c) whether any response contains ';E'."
}

stanza_10() {
  say ""
  say "== Stanza 10: #261 chunked root refreshes a classic placement =="
  say "A RED block appears. Both payloads exceed kitty's 4096-byte encoded"
  say "chunk limit. The corrected production sequence keeps every edit chunk"
  say "on root frame 1. There is NO delete and NO second placement."

  # 32x32 RGBA is 4096 raw / 5464 encoded bytes: exactly two APC chunks. The
  # old 2x2 version stayed below the boundary and therefore could not reproduce
  # the game freezing on frame 1. Generate the solids here instead of checking
  # in opaque blobs; the observer can still answer the result by color alone.
  local red32_b64 green32_b64 pixel
  red32_b64=$(
    for ((pixel=0; pixel<1024; pixel++)); do
      printf '\xff\x00\x00\xff'
    done |
      base64 | tr -d '\n'
  )
  green32_b64=$(
    for ((pixel=0; pixel<1024; pixel++)); do
      printf '\x00\xff\x00\xff'
    done |
      base64 | tr -d '\n'
  )

  local red_wire green_wire
  red_wire="${ESC}_Ga=t,t=d,f=32,i=47,s=32,v=32,m=1,q=0;${red32_b64:0:4096}${ST}"
  red_wire+="${ESC}_Gm=0;${red32_b64:4096}${ST}"
  green_wire="${ESC}_Ga=f,t=d,f=32,i=47,s=32,v=32,r=1,X=1,m=1,q=0;${green32_b64:0:4096}${ST}"
  # r=1 on the continuation is load-bearing. Kitty decides whether this is a
  # new or existing frame before restoring the opener's saved control block.
  green_wire+="${ESC}_Ga=f,r=1,m=0;${green32_b64:4096}${ST}"

  send "transmit(red)" "$red_wire"
  place_below "place(red)   " \
    "${ESC}_Ga=p,i=47,p=1,c=4,r=3,C=1,q=0${ST}" 3
  pause "Press Enter to replace the root frame with GREEN..."
  send "frame(green) " "$green_wire"
  say "Report: (a) the existing block turned GREEN without disappearing or"
  say "flickering, and (b) whether any response contains ';E'. A block that"
  say "stays red is the exact #261 failure, even if the command reports OK."
  say "This stanza now exercises the same multi-chunk path as the game."
}

stanza_11() {
  say ""
  say "== Stanza 11: #140 pixel-offset resident block edits =="
  say "A RED 4x4-pixel image is placed once. Its top-left 2x2 pixels are"
  say "overwritten GREEN, then its bottom-right 2x2 pixels receive a 50%"
  say "BLUE source-over edit. No a=t retransmit or second placement follows."

  local red4_b64 blue_half_b64 pixel
  red4_b64=$(
    for ((pixel=0; pixel<16; pixel++)); do printf '\xff\x00\x00\xff'; done |
      base64 | tr -d '\n'
  )
  blue_half_b64=$(
    for ((pixel=0; pixel<4; pixel++)); do printf '\x00\x00\xff\x80'; done |
      base64 | tr -d '\n'
  )

  send "transmit(red)" \
    "${ESC}_Ga=t,t=d,f=32,i=48,s=4,v=4,m=0,q=0;${red4_b64}${ST}"
  place_below "place(red)   " \
    "${ESC}_Ga=p,i=48,p=1,c=8,r=4,C=1,q=0${ST}" 4
  pause "Press Enter to apply the GREEN overwrite block..."
  send "edit(green) " \
    "${ESC}_Ga=f,t=d,f=32,i=48,s=2,v=2,r=1,x=0,y=0,X=1,m=0,q=0;${green_b64}${ST}"
  pause "Press Enter to apply the translucent BLUE source-over block..."
  send "edit(alpha) " \
    "${ESC}_Ga=f,t=d,f=32,i=48,s=2,v=2,r=1,x=2,y=2,m=0,q=0;${blue_half_b64}${ST}"
  say "Report: (a) top-left is GREEN, (b) bottom-right is the expected"
  say "red/blue blend rather than opaque blue, (c) the other pixels remain"
  say "RED, and (d) whether any response contains ';E'."
}

stanza_12() {
  say ""
  say "== Stanza 12: #114 named image layers around text/background =="
  say "Two 8x3 text plates and one 8x1 background plate. Expected:"
  say "RED at z=1 hides its text; BLUE at z=-1 keeps white TEXT visible;"
  say "GREEN below the background boundary is hidden by MAGENTA cells."

  local rt bt gt rp bp gp row
  send_quiet "${ESC}_Ga=t,t=d,f=32,i=49,s=2,v=2,m=0,q=0;${red_b64}${ST}"; rt=$reply_out
  send_quiet "${ESC}_Ga=t,t=d,f=32,i=50,s=2,v=2,m=0,q=0;${blue_b64}${ST}"; bt=$reply_out
  send_quiet "${ESC}_Ga=t,t=d,f=32,i=51,s=2,v=2,m=0,q=0;${green_b64}${ST}"; gt=$reply_out

  # Reserve four rows, save the origin, and place three images without moving
  # the terminal cursor. The subsequent cells deliberately arrive later: z,
  # not command order, decides which content remains visible.
  printf '\n\n\n\n%s[4A%s7' "$ESC" "$ESC"
  send_quiet "${ESC}_Ga=p,i=49,p=1,c=8,r=3,C=1,z=1,q=0${ST}"; rp=$reply_out
  printf '%s8%s[10C' "$ESC" "$ESC"
  send_quiet "${ESC}_Ga=p,i=50,p=1,c=8,r=3,C=1,z=-1,q=0${ST}"; bp=$reply_out
  printf '%s8%s[20C' "$ESC" "$ESC"
  send_quiet "${ESC}_Ga=p,i=51,p=1,c=8,r=1,C=1,z=-1073741825,q=0${ST}"; gp=$reply_out

  for row in 0 1 2; do
    printf '%s8%s[%dB%s[0m%s[38;2;255;255;255m' \
      "$ESC" "$ESC" "$row" "$ESC" "$ESC"
    if ((row == 1)); then printf '  TEXT  '; else printf '        '; fi

    printf '%s8%s[%dB%s[10C%s[0m%s[38;2;255;255;255m' \
      "$ESC" "$ESC" "$row" "$ESC" "$ESC" "$ESC"
    if ((row == 1)); then printf '  TEXT  '; else printf '        '; fi
  done

  # ECH paints eight complete cells with the current non-default background
  # without advancing the cursor. Keeping this case to one row removes image
  # scaling/row-alignment ambiguity from the z<-2^30 protocol check.
  printf '%s8%s[20C%s[48;2;255;0;255m%s[8X' \
    "$ESC" "$ESC" "$ESC" "$ESC"
  printf '%s[0m%s8%s[4B\r' "$ESC" "$ESC" "$ESC"

  say "$(printf 'transmit(red/blue/green) responses: %q / %q / %q' "$rt" "$bt" "$gt")"
  say "$(printf 'place(above/below/background) responses: %q / %q / %q' "$rp" "$bp" "$gp")"
  say "Report: (a) red hides TEXT, (b) TEXT is visible over blue, (c) the"
  say "magenta plate hides green, and (d) whether any response contains ';E'."
}

if (( ${#stanzas[@]} )); then
  run_all=0
  for n in "${stanzas[@]}"; do "stanza_$n"; done
else
  for n in 1 2 3 4 5 6 7 8 9 10 11 12; do "stanza_$n"; done
  say ""
  say "Report: (a) stanza 1 red block, (b) green after stanza 2, (c) stanza 3"
  say "blue block, (d) YELLOW block after stanza 4, (e) stanza 5 shows a"
  say "green 2x2 block, (f) stanza 6 as described above, (g) stanza 7's three"
  say "questions, (h) stanza 8's six labelled blocks, (i) any response"
  say "containing ';E' (an error), (j) stanza 9's three requested results,"
  say "(k) stanza 10's existing block turns green without re-placement, and"
  say "(l) stanza 11's offset overwrite/alpha quadrants match, and (m)"
  say "stanza 12's three named-layer visibility results match its description."
fi
