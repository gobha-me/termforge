# Structured event sources

`EventSource` lets an application feed already-structured input into `App`.
The terminal decoder remains the default route; a source is an explicit,
owned extension point for adapters such as evdev, a remote session protocol,
or an embedding host that already has an input loop.

The core does not discover devices, open seats, choose a keyboard layout, or
interpret platform key codes. An adapter owns those policies and exposes the
small portable boundary in `termforge/core/event_source.hpp`:

```cpp
class MySource final : public termforge::EventSource {
 public:
  auto start() -> std::expected<void, termforge::ErrorEvent> override;
  auto stop() noexcept -> void override;
  auto poll_fd() const noexcept -> int override;
  auto capabilities() const noexcept
      -> termforge::InputCapabilities override;
  auto poll()
      -> std::expected<std::vector<termforge::Event>,
                       termforge::ErrorEvent> override;
};

app.set_event_source(std::make_unique<MySource>(),
                     termforge::EventSourceMode::ReplaceTerminal);
```

`App` owns the source object. It starts and stops the source once per run, and
destroys it when replaced, cleared, or when the App is destroyed. The source
owns its descriptor and every resource behind it. `poll_fd()` must remain
stable between a successful `start()` and `stop()`, and `poll()` must be
nonblocking. Readability means that an event batch, a failure, or a
capability-only change is ready.

## Replacement and composition

The mode is mandatory because guessing creates duplicate input:

- `ReplaceTerminal` means the source describes the same physical input as the
  terminal stream. `App` drains terminal bytes without decoding or recording
  them, and only the source events are delivered. A failed replacement source
  does not silently fall back to terminal decoding during that run.
- `ComposeTerminal` is for sources known to be disjoint. At each input
  boundary, decoded terminal events are delivered first, followed by the
  source batch in its original order.

Both modes share the ordinary `App::dispatch_event` funnel. Releases bypass
overlays and widgets exactly as terminal-decoded releases do; errors remain
observable even with a modal open; renderer sanitization remains the boundary
for text carried by either route.

## Capabilities and key state

`InputCapabilities` describes semantic guarantees, not a device name:

- `key_press` — key-down events are available;
- `key_repeat` — held keys can produce `KeyAction::Repeat`;
- `key_release` — key-up events are available; and
- `modifier_transitions` — standalone left/right Shift, Ctrl, and Alt
  transitions are available.

Repeat, release, and modifier transitions imply press; modifier transitions
also imply release. `App` refuses inconsistent declarations. In composition,
the effective capabilities are the union of the terminal and source routes;
in replacement they are the source declaration alone. `input_capabilities()`
exposes that value, and `AppRequirements::key_*` evaluates against it. A
structured source can therefore satisfy a repeat/release floor on a terminal
without the kitty keyboard protocol.

Capabilities may change while a source is active. The descriptor must become
readable, `poll()` returns the batch produced under the old declaration, and
the source exposes the new declaration from `capabilities()` afterwards.
`App` emits a `Warning` for loss or an `Info` for restoration and immediately
re-evaluates requirements. Losing release support synthesizes releases for
all keys still held by that source before the warning, so application state
cannot remain stuck.

## Batch validation and failure

A batch is atomic. `App` validates the complete batch before delivering any of
it. It rejects malformed key or mouse values, transitions exceeding the
declared capabilities, duplicate presses, repeats/releases without a matching
press, `ResizeEvent` (a remote adapter must use `App::set_size`), and
`ImageInvalidatedEvent` (embedding code must use `App::invalidate_images` or
the thread-safe `App::post` boundary). Paste payload bytes remain opaque;
display sanitization still belongs to the renderer.

The matching identity available in today's `KeyEvent` is `(key, ch)`. An
adapter must preserve that pair from press through repeat and release, even if
the modifier snapshot changes while the key is held.

On a malformed batch, returned failure, or exception, `App`:

1. synthesizes releases for held source keys;
2. emits an `ErrorEvent` with `Severity::Warning`;
3. stops the source for that run; and
4. marks its capabilities unavailable and re-evaluates requirements.

An empty batch is valid and can carry only a capability transition.

## Demand mode and traces

The source descriptor is polled beside the terminal and `App::post` wake pipe,
so it wakes an idle `RenderMode::Demand` loop without a polling timer. Events
absorbed during a frame wait are delivered at the next ordinary input boundary.

Trace schema 2 added structured-source events and source-reported effective
input-capability changes in addition to raw terminal chunks. Schema 3 adds
resident-image invalidation boundaries; this does not widen what an
`EventSource` may emit. Schema 4 records normalized terminal control replies
while replacement mode suppresses terminal keystrokes; output acknowledgements
therefore still reach the selected driver and replay in their original phase.
Playback does not start or poll a configured live
source: it replays source records through the same event boundary and uses the
recorded capabilities for `AppRequirements`. Schema-1 and schema-2 traces
remain readable, as does schema 3; schema 1 implies the historical press-only
terminal route.

## Raw evdev is an adapter policy, not a core feature

A future Linux evdev adapter must be opt-in and receive an already-authorized
device descriptor from its caller. It must not scan or open
`/dev/input/event*`, require privilege escalation, change group membership,
claim a logind seat, or issue `EVIOCGRAB` on the application's behalf.

That boundary is deliberate:

- raw devices are permission- and seat-controlled and may expose keystrokes
  intended for other focused applications;
- evdev supplies physical key codes, not the focused terminal's keyboard
  layout, dead-key, compose, or IME-produced text;
- focus loss, device hotplug, multiple keyboards, and `SYN_DROPPED`
  resynchronization require policy that only the embedding application has;
- SSH, containers, browser terminals, Guacamole, and other remote/PTY proxies
  normally have no access to the client's physical keyboard; and
- consuming evdev and terminal bytes together duplicates actions unless the
  caller can prove the routes disjoint and deliberately selects composition.

Terminal CSI-u remains the portable path for terminal-composed text and rich
key transitions. A raw-device adapter is suitable only when the caller owns
the security, focus, layout, lifetime, and resynchronization decisions.
