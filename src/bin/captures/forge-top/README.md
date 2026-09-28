# ForgeTop capture evidence

Before source: `6223e551abc424dbfa0cc3f546b3700fcd49688d` (v0.57.29),
before any visual/layout changes. The capture setup adds only a read-only
accessor in the private binary class and an optional capture executable; it
does not change the app's render/input/sample behavior.

Inputs: the same deterministic fake reader as `--fake`, 20 cores, 48 workers,
initial sample, one production App frame, then driver shutdown. Tier 0 is
Fallback, tier 1 ANSI RGB, tier 2 Kitty. Sizes: 24x8, 40x16, 80x24, 120x32.

After files are produced by this issue's implementation with a zero-origin
SyntheticClock; before files used the frozen baseline executable. Both use
the initial sample with no input or resampling. After includes the explicit
DEMO marker. Actual initial process row capacities before -> after are 0 -> 2,
0 -> 10, 2 -> 7 and 6 -> 24 respectively. All twelve after cell fixtures are
checked against the production frame by `53forgetop-test`.

Cell files read Screen at the frame observer. These initial-view captures have
no overlay. Pixel collection may have blanked enhanced regions; cells are not
Kitty images, and colors are not represented. App restores the underlying
Screen after presenting an overlay, so modal visibility requires an emitted-
output test rather than this accessor. No physical-terminal review is claimed.

Wire JSONL files describe actual sink bytes including shutdown. FNV-1a-64 is a
deterministic emission fingerprint, not a security hash or quality/performance
claim. Layout changes also change graph extents, so byte totals alone are not
a like-for-like graphics benchmark.

Rebuild the optional tool with:

```sh
cmake --build build --target termforge_forge_top_capture --parallel 2
./build/test/53forgetop/termforge_forge_top_capture 80 24 0
./build/test/53forgetop/termforge_forge_top_capture 80 24 0 --wire
```
