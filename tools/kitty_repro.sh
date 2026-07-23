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
echo "Report: (a) stanza 1 red block, (b) green after stanza 2, (c) stanza 3"
echo "blue block, (d) YELLOW block after stanza 4, (e) stanza 5 shows a"
echo "green 2x2 block, (f) any response containing ';E' (an error)."
