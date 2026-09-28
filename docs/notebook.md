# Notebook

`Notebook` adds persistent tabbed content to the existing strip-only `TabBar`.
It is not a floating/tiling window manager. Pages remain application-owned:

```cpp
TextInput form;
TextBox transcript;
PixelSurface pixels({64, 32});
Notebook notebook;
notebook.add_page("Form", &form);
notebook.add_page("Transcript", &transcript);
notebook.add_page("Pixels", &pixels);
notebook.set_focused(true);
// on_render:
notebook.set_geometry({0, 0, screen.cols(), screen.rows()});
notebook.draw(screen);
render_pixel_regions(notebook);
```

Keep each root alive until removed and through any event dispatch using it.
Null, self and duplicate roots are refused without changing the pages. One
root must not be registered in multiple containers simultaneously, and page
composition must be acyclic. Notebook
cannot be copied/moved; it neither owns nor deletes registered pages, and its
destructor deliberately does not dereference them.

The first row is the tab strip; `content_rect()` is the remaining non-negative
area. Only the selected root is laid out and drawn; a zero-sized content area
neither draws, routes input to, nor collects pixels from its page. A compound
root lays out its children inside that area and obeys the normal Widget drawing
contract. This is not a general-purpose clipping or scrolling container.

On the strip, Left/Right/Home/End select pages. Ctrl+Tab and Ctrl+Shift+Tab
switch pages from either focus location, wrapping. Tab enters the page;
an unconsumed forward Tab from the page leaves the Notebook, while Shift+Tab
returns to the strip. Shift+Tab on the strip is declined so an outer FocusRing
can move to its previous member. A compound
page handles its own internal tab order and declines Tab at its boundary.
Mouse presses select strip/content focus and route only to the active root.
Page dropdown hit areas use `hit_test_tree`, just as existing modal containers
do. The application remains responsible for mouse capture outside the tree.

A compound page forwards `set_focused(false/true)` to its selected child while
retaining its internal focus cursor. Switching pages clears old focus and
transient feedback/popups, never text, values, selection or scroll state.
`on_tick` forwards only to the active page; background work, if desired, is
explicit application policy. Losing outer focus retains the strip/content
location for restoration. Pages need not self-guard events by focus: the
application's FocusRing remains the keyboard gatekeeper.
Non-focusable pages and zero-sized content use strip focus/traversal; restoring
an interactive, visible page restores the remembered content location.

Programmatic `set_active`, registration and removal never call `on_change`.
User selection calls it after the switch is complete, using a copied callback
so it may replace itself or remove/clear pages. Removing a page before the
selection preserves the selected root; removing the selected page chooses
the next surviving page, or the previous one if it was last. Empty means
`active() == -1`, `active_page() == nullptr`, and no focus stop.

`Widget::pixel_children()` is an optional, non-pure traversal hook. A compound
page returns only its visible, already-drawn children. App traverses borrowed
roots, skips nulls/cycles/duplicates within one collection, and retains the
actual producer's `(Widget*, region index)` identity. No payload forwarding,
hidden-page acknowledgement or App-owned scene tree is introduced. Existing
modal suppression, image retirement and accepted-write/refusal rules apply
unchanged. Call `render_pixel_regions` once for the root, not again for each
child. Children and their borrowed buffers must survive the frame write.

See `examples/notebook.cpp` for a runnable form/transcript/persistent-pixel demo.
F1 switches chrome to ASCII; the pixel page has a complete cell fallback.
Hermetic tests observe actual App frames on Baseline, ANSI and Kitty; they do
not substitute for visual review in a real terminal.
