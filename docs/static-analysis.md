# C++ static analysis

TermForge runs clang-tidy **20.x** over the shipped library. The checked-in
[`.clang-tidy`](../.clang-tidy) starts from an empty check set and enables a
small reviewed allowlist; upgrading LLVM does not silently opt the project into
new checks.

Run the same gate as CI from the repository root:

```sh
tools/lint.sh
```

The script creates a Clang Release compilation database in `build-tidy/` and
analyzes the `src/lib` translation units plus diagnostics originating in
`include/termforge` and private library headers. Tests, examples, binaries,
benchmarks, generated sources, dependencies and system headers are outside the
gate. This establishes a clean baseline for the artifact consumers ship;
developer-only targets can be added later after their own review.

`clang-tidy-20`, `run-clang-tidy-20` and `clang++-20` are the defaults. A
versioned installation with different executable paths can be selected with
`CLANG_TIDY`, `RUN_CLANG_TIDY` and `CLANGXX`. `TERMFORGE_TIDY_BUILD_DIR`
selects another build directory, and `TERMFORGE_TIDY_JOBS` sets concurrency.
The compiler and analyzer must both report major version 20.

The script is deliberately check-only. Automated fixes can change terminal
wire assembly, signed cell/pixel arithmetic and virtual interfaces in ways that
look mechanical but are not. Apply a diagnostic by hand, review it as a normal
code change, and run the full GCC/Clang tests and consumer checks.

## Policy decisions

The accepted baseline catches analyzer defects; suspicious ownership, lifetime,
string, optional and integer-width operations; ineffective moves and container
work; two portability hazards; and a small set of C++23 migrations that cannot
alter the driver interface. Representative accepted findings included a cast
performed after signed waveform-index arithmetic, byte-budget multiplication
performed before widening, and moves from trivial or const values.

The broad presets were rejected after sampling core, driver, widget and test
translation units. The following checks stay disabled deliberately:

- `performance-enum-size` changes public enum layout and therefore ABI.
- `performance-unnecessary-value-param` pushes public ownership-taking setters
  toward reference APIs and diagnoses cheap iterators.
- `bugprone-easily-swappable-parameters` treats named cell and pixel coordinate
  pairs as mistakes even though their ordering is the API.
- `bugprone-suspicious-memory-comparison` rejects `Cell`'s tested unique object
  representation, which is intentionally compared with `memcmp`.
- `bugprone-unchecked-optional-access` cannot follow validation performed by
  the pixel-region preflight before the render loop.
- `bugprone-suspicious-stringview-data-usage` diagnoses `data()` even when the
  explicit byte count is supplied to `std::string::append`.
- `bugprone-branch-clone` asks protocol state machines to merge distinct
  transitions merely because their recovery bodies match.
- `modernize-deprecated-headers` recommends `<csignal>` where TermForge needs
  POSIX `sigaction` declarations from `<signal.h>`.
- `modernize-use-designated-initializers` and the remaining broad `modernize`
  checks create style or API churn unrelated to defect prevention.
- `portability-simd-intrinsics` conflicts with the intentional, runtime-gated
  AVX2 implementation and its scalar oracle.
- `clang-analyzer-unix.BlockInCriticalSection` does not model the pipe's
  established `O_NONBLOCK` state and reports its bounded drain under a mutex.

The existing strict GCC/Clang conversion-warning builds remain the authority
for ordinary signed and narrowing conversions; duplicating them with the much
noisier `bugprone-narrowing-conversions` adds no coverage.
