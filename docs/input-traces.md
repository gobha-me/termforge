# Deterministic input traces

`App` can turn an interactive failure into a replayable artifact:

```cpp
std::ofstream trace{"failure.tftrace", std::ios::binary};
app.start_recording(trace);
const int status = app.run();  // a clean exit finalizes the trace
```

Replay uses a fresh instance of the same application:

```cpp
std::ifstream trace{"failure.tftrace", std::ios::binary};
if (auto played = app.play(trace); !played) {
  // played.error() is a Warning when the trace was refused.
}
```

The streams are borrowed. Start recording while the loop is stopped and keep
the output stream alive through `run()` or an earlier `stop_recording()`.
Stopping during a frame produces a playable prefix ending after that frame.
Destruction never writes to a borrowed stream.

Playback is isolated from live terminal and structured-source input. It
temporarily installs the recorded terminal size and capabilities, drives the
production loop with an internal `SyntheticClock`, and restores the caller's
prior pushed size, capabilities, clock and pending decoder state afterwards. A
caller that has explicitly pushed different capabilities gets a `Warning`
before setup or output; silently replaying against another rendering tier would
not be the same run.

## What is recorded

For the terminal route, the source of truth is the exact byte chunks returned
by the nonblocking input stream—not decoded `Event` objects. Each chunk carries
its frame, frame phase, and monotonic nanosecond offset. Playback feeds those
bytes through `Input` again, preserving split and malformed escape sequences
and the boundary where a lone Escape is committed.

A structured `EventSource` has no terminal byte representation, so schema 2
added its validated events in batch order plus source-reported effective
input-capability transitions. Schema 3 adds the applied
`ImageInvalidatedEvent` frame boundary, including its suspend/resume, reattach,
or terminal-reset reason. Playback clears the same driver/Persistent beliefs
before delivering the event, so a recording cannot silently keep resident
image state that the live run discarded. Playback never starts or polls the
configured live source. Schema 4 adds normalized Kitty graphics replies for a
replacement-source session: terminal keystrokes are still suppressed, while
the acknowledgements needed by the output driver remain deterministic. In a
terminal or composed session the raw byte record already contains those APCs,
so they are not duplicated. Schema 5 preserves the SGR motion bit on posted
and structured-source `MouseEvent`s, which keeps a drag distinct from its
matching release; raw terminal records already preserve that bit in their
original bytes. Schema 6 adds the action-probed Kitty animation capability so
playback cannot silently select the same broad graphics tier with a different
frame-registration contract. Schema-1 through schema-5 traces remain readable;
schema 1 implies the historical press-only route, and schemas before 6 imply no
animation-action support.

The trace also records:

- every observed frame-start time, so fixed ticks retain real overruns and
  wait rounding rather than merely the configured frame budget;
- the effective terminal size at each production resize boundary;
- each applied resident-image invalidation at its production frame boundary;
- normalized terminal replies when a structured source replaces terminal input;
- events consumed from `App::post`, at the posted-event snapshot boundary;
- the terminal capabilities, effective input capabilities, and initial size
  resolved during setup;
- the producing TermForge version for provenance; and
- a clean-run or deliberately stopped-prefix end record.

Internally the file is a bounded, versioned little-endian binary format with a
magic header, schema number, explicit field widths and length-prefixed
payloads. It never dumps C++ object layout or enum storage. The schema version
is the compatibility gate; the producing library version is not. A regression
artifact must remain playable by the release containing its fix.

See [event-sources.md](event-sources.md) for replacement/composition semantics
and source failure handling.

Malformed, truncated, oversized, non-monotonic, unknown-schema, invalid-size,
and incompatible-capability traces return
`ErrorEvent{Severity::Warning, "trace", ...}` without starting the App.
