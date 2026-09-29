# Widget gallery captures

The **before** source is `examples/widgets.cpp` at
`796d0e85d395000a6f3740d60f0117e9e337aa9f` (v0.57.28). Its cell captures were
recorded before changing the demo. The **after** source is the private gallery
in this change. Captures use zero first-tick time; no tty, network or random
data. Tier 0 is FallbackDriver, 1 is AnsiRgbDriver, 2 is KittyDriver.

The after set now includes real Gallery Theme adoption (parent #373, child
#401), nine Controls specimens, palette identification and reference updates.
The frozen before set is unchanged. Dark is the initial app palette; native
Baseline uses ASCII while native ANSI/Kitty use enhanced glyphs.

`before-WxH.txt` contains the original authored cell grid (the layout does not
vary by selected tier). `after-tierN-WxH.txt` contains the initial Controls
page at each tier, including its actual default presentation. Empty cells are
spaces and wide-glyph continuation cells are omitted from UTF-8 serialization.
Trailing spaces are intentional: each row retains its terminal display width.

`before-wire.txt` measures one original real-App frame at every size/tier,
including its Waveform route and headless shutdown. `after-wire.txt` measures
both the Controls page and the persistent PixelSurface page through actual App
frames. FNV-1a is a deterministic byte fingerprint, **not** a security hash.
These records are not image screenshots, benchmarks, decoder proof or a human
real-emulator review. The after pixel path has different authored content and
extent from the old Waveform; do not compare those byte counts as a speedup.

`theme-evidence.txt` records seven real-App frames (initial, editor focus,
category activation, focused selection, disabled, error, help) for both dark
and high-contrast palettes, all three tiers, and native/explicit-ASCII policy:
84 frames total. Activation is included separately because a borrowed page's
focusability depends on its laid-out body; selection is observed on the next
production frame. Each record includes full cell and lossless RGB/attribute-run
fingerprints, readable header/body/status probes (`fg/bg/attribute-bits`), and
the actual frame's byte fingerprint/count. The help Screen probe describes the
backdrop; its wire fingerprint includes the later modal. Unit tests additionally
read ASCII modal output on both truecolor drivers and assert semantic roles,
markers, bold/reverse cues and data preservation. Fallback drops RGB/Dim but
keeps the explicit disabled marker and its supported Bold/Reverse feedback.
These are deterministic authored-state/emission evidence, not physical screen
or font/capability claims.

Build/regenerate the after set with the opt-in target (tests enabled):

```sh
cmake --build build --target termforge_gallery_capture --parallel 2
capture=build/test/82gallery/termforge_gallery_capture
for size in '24 8' '80 24' '40 16' '120 32'; do
  set -- $size
  for tier in 0 1 2; do
    "$capture" "$1" "$2" "$tier" --cells > "examples/captures/widgets/after-tier$tier-$1x$2.txt"
    "$capture" "$1" "$2" "$tier"
    "$capture" "$1" "$2" "$tier" pixels
  done
done > examples/captures/widgets/after-wire.txt

for palette in dark hc; do
  for tier in 0 1 2; do
    for mode in native ascii; do
      printf '# palette=%s tier=%s mode=%s\n' "$palette" "$tier" "$mode"
      options=()
      if [ "$palette" = hc ]; then options+=(--hc); fi
      if [ "$mode" = ascii ]; then options+=(--ascii); fi
      "$capture" 80 24 "$tier" --theme-evidence "${options[@]}"
    done
  done
done > examples/captures/widgets/theme-evidence.txt

# Inspect the complete lossless style runs, not only their fingerprint:
"$capture" 80 24 2 --hc --ascii --styles
```

Regenerate the frozen before source in a task-owned build directory; for exact
historical library reproduction also build the library at the recorded SHA.
The gallery change itself makes no library changes.

```sh
mkdir -p build/capture-baseline
git show 796d0e85d395000a6f3740d60f0117e9e337aa9f:examples/widgets.cpp > build/capture-baseline/baseline_widgets.cpp
g++ -std=c++23 -I include -I examples -I build/capture-baseline \
  examples/gallery_baseline_capture.cpp build/src/lib/libtermforge.a \
  -o build/capture-baseline/capture
for size in '24 8' '80 24' '40 16' '120 32'; do
  set -- $size
  build/capture-baseline/capture "$1" "$2" > "examples/captures/widgets/before-$1x$2.txt"
  build/capture-baseline/capture "$1" "$2" wire
done > examples/captures/widgets/before-wire.txt
```

Review: the before 24x8 layout spends nearly all rows on frames and status,
leaving the controls crowded out. The after tiny/narrow layout keeps a usable
specimen body and navigation; normal adds instructions/source, wide adds a
reference sidebar. `82gallery-test` pins all after cell captures and complete
input/state/pixel journeys; snapshots alone cannot detect routing regressions.
