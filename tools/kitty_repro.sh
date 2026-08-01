#!/usr/bin/env bash
# kitty_repro.sh — minimal standalone repro for the kitty Unicode-placeholder
# path used by KittyDriver. Run inside a real kitty (>= 0.28) terminal:
#
#   ./tools/kitty_repro.sh
#
# Stanza 1 transmits a 2x2 solid-RED RGBA image (id 42), creates a virtual
# placement (U=1), and prints a 2x2 grid of U+10EEEE placeholder cells with
# the image id encoded as the truecolor foreground — exactly what
# KittyDriver::place_unicode emits.
#
# Stanza 2 retransmits the SAME id 42 with solid GREEN pixels and does NOT
# re-place. If the red block turns green, retransmit-with-same-id refreshes
# existing placements (KittyDriver can reuse one id per region for animation).
#
# All commands use q=0 so kitty REPORTS errors; every response the terminal
# sends is captured and echoed in readable form. A response of "_Gi=42;OK"
# means the command was accepted.

set -u

ESC=$'\033'
ST="${ESC}\\"          # string terminator
PH=$'\xf4\x8f\xbb\xae' # U+10EEEE placeholder
D0=$'\xcc\x85'         # diacritic index 0: U+0305
D1=$'\xcc\x8d'         # diacritic index 1: U+030D

red_b64=$(printf '\xff\x00\x00\xff\xff\x00\x00\xff\xff\x00\x00\xff\xff\x00\x00\xff' | base64)
green_b64=$(printf '\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff' | base64)

saved_stty=$(stty -g)
restore() { stty "$saved_stty"; }
trap restore EXIT

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

# As send(), but leaves the reply in $reply_out instead of printing it. For the
# case where the cursor position between two commands is load-bearing (stanza
# 6 places two images side by side) and an echoed response line would both move
# the cursor and paint over the image classic placement draws at it.
reply_out=''
send_quiet() {
  local payload=$1
  stty raw -echo
  printf '%s' "$payload"
  sleep 0.3
  reply_out=''
  local ch
  while IFS= read -r -t 0.05 -n 1 ch; do reply_out+=$ch; done
  stty "$saved_stty"
  [[ -n $reply_out ]] || reply_out='(none)'
}

echo "== Stanza 1: red 2x2 image, virtual placement, placeholder cells =="
send "transmit(red)" "${ESC}_Ga=t,t=d,f=32,i=42,s=2,v=2,m=0,q=0;${red_b64}${ST}"
send "place(U=1)   " "${ESC}_Ga=p,i=42,p=1,U=1,c=2,r=2,q=0${ST}"

# Placeholder grid: fg = image id 42 as 24-bit color (0;0;42), row/col diacritics.
printf '%s[38;2;0;0;42m' "$ESC"
printf '%s%s%s%s%s%s' "$PH" "$D0" "$D0" "$PH" "$D0" "$D1"   # row 0: cols 0,1
printf '\n'
printf '%s%s%s%s%s%s' "$PH" "$D1" "$D0" "$PH" "$D1" "$D1"   # row 1: cols 0,1
printf '%s[0m\n' "$ESC"

echo
echo "You should see a 2x2-cell RED block above."
read -r -p "Press Enter for stanza 2 (retransmit same id as GREEN)..."

echo "== Stanza 2: retransmit id 42 as green (no new placement) =="
send "transmit(grn)" "${ESC}_Ga=t,t=d,f=32,i=42,s=2,v=2,m=0,q=0;${green_b64}${ST}"

echo
echo "Did the red block turn GREEN?"
read -r -p "Press Enter for stanza 3 (classic placement, KittyDriver's default)..."

echo "== Stanza 3: classic placement — transmit id 43, place at cursor with c=2,r=2,C=1 =="
blue_b64=$(printf '\x00\x00\xff\xff\x00\x00\xff\xff\x00\x00\xff\xff\x00\x00\xff\xff' | base64)
send "transmit(blu)" "${ESC}_Ga=t,t=d,f=32,i=43,s=2,v=2,m=0,q=0;${blue_b64}${ST}"
send "place(C=1)   " "${ESC}_Ga=p,i=43,p=1,c=2,r=2,C=1,q=0${ST}"
printf '\n\n\n'  # scroll past the placement so the prompt doesn't overwrite it

echo
read -r -p "Press Enter for stanza 4 (retransmit id 43 as YELLOW — classic refresh)..."

echo "== Stanza 4: retransmit id 43 as yellow + recreate the placement =="
# Finding from earlier runs: kitty replaces the image data on retransmit
# but does NOT refresh an existing classic placement — so KittyDriver now
# deletes and recreates the placement on every content change. This stanza
# mirrors that exact sequence.
yellow_b64=$(printf '\xff\xff\x00\xff\xff\xff\x00\xff\xff\xff\x00\xff\xff\xff\x00\xff' | base64)
send "transmit(yel)" "${ESC}_Ga=t,t=d,f=32,i=43,s=2,v=2,m=0,q=0;${yellow_b64}${ST}"
send "del place    " "${ESC}_Ga=d,d=i,i=43,p=1,q=0${ST}"
send "place(C=1)   " "${ESC}_Ga=p,i=43,p=1,c=2,r=2,C=1,q=0${ST}"
printf '\n\n\n'

echo
echo "A YELLOW block should appear at the cursor (the old blue one is gone"
echo "with its deleted placement)."
read -r -p "Press Enter for stanza 5 (placeholders again, 38;5 id encoding)..."

echo "== Stanza 5: placeholder cells for id 42 with 256-color id encoding =="
# Same virtual placement as stanza 1 (already created); only the cell fg
# encoding differs: 38;5;42 instead of 38;2;0;0;42. kitten icat uses this
# form for ids < 256 — if THIS renders where stanza 1 didn't, the 24-bit
# id encoding is what this kitty rejects.
printf '%s[38;5;42m' "$ESC"
printf '%s%s%s%s%s%s' "$PH" "$D0" "$D0" "$PH" "$D0" "$D1"
printf '\n'
printf '%s%s%s%s%s%s' "$PH" "$D1" "$D0" "$PH" "$D1" "$D1"
printf '%s[0m\n' "$ESC"

echo
read -r -p "Press Enter for stanza 6 (PlacementFit::Exact — #137)..."

echo
echo "== Stanza 6: c=/r= omitted, so the terminal places at TRUE SIZE =="
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
w='\xff\xff\xff\xff'; k='\x00\x00\x00\xff'
row_a=''; row_b=''
for _ in $(seq 4); do row_a+="$w$w$k$k"; row_b+="$k$k$w$w"; done
checker=''
for _ in $(seq 4); do checker+="$row_a$row_a$row_b$row_b"; done
check_b64=$(printf "$checker" | base64 | tr -d '\n')

send "transmit(chk)" \
  "${ESC}_Ga=t,t=d,f=32,i=44,s=16,v=16,m=0,q=0;${check_b64}${ST}"

echo "  LEFT  (p=1) — STRETCHED into 5x3 cells (c=5,r=3). 16px across 5 cells"
echo "                is not an integer ratio, so the 2px squares come out"
echo "                UNEVEN — 5px wide in places, 6px in others, and the rows"
echo "                likewise. That unevenness is the bug #137 fixes."
echo "  RIGHT (p=2) — EXACT: the identical a=p with c= and r= simply removed."
echo "                Expect a much SMALLER block (16x16 device pixels, about"
echo "                two cells) with every square the same size."
echo

# Reserve rows for the images, then step back up to the top of them. Both
# placements use C=1 so the cursor does not move, which is what lets the second
# be positioned relative to the first. The responses are held back and printed
# below the block: send()'s own echo would otherwise land on top of the images.
printf '\n\n\n\n\n'
printf '%s[5A' "$ESC"
send_quiet "${ESC}_Ga=p,i=44,p=1,c=5,r=3,C=1,q=0${ST}"; r6a=$reply_out
printf '%s[7C' "$ESC"
send_quiet "${ESC}_Ga=p,i=44,p=2,C=1,q=0${ST}"; r6b=$reply_out
printf '%s[5B\r' "$ESC"

printf 'place(c=5,r=3) response: %q\n' "$r6a"
printf 'place(no c/r)  response: %q\n' "$r6b"

echo
echo "Report: (a) stanza 1 red block, (b) green after stanza 2, (c) stanza 3"
echo "blue block, (d) YELLOW block after stanza 4, (e) stanza 5 shows a"
echo "green 2x2 block, (f) for stanza 6, COMPARING the two blocks side by"
echo "side: did the RIGHT one render at all — and if so, is it much smaller"
echo "than the left with even, crisp squares, while the left is larger with"
echo "uneven ones? A blank right-hand side is a real answer, not a bug in this"
echo "script; it would mean this kitty needs c=/r=. (g) any response"
echo "containing ';E' (an error)."
