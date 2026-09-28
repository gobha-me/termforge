# ForgeTop

`forge-top` is a read-only system monitor, not a process-control tool. Run
`forge-top --fake --driver=fallback` to explore deterministic, explicitly
marked DEMO data without reading the host. Other forced tiers are `ansi` and
`kitty`; normal invocation selects the terminal's supported tier.

## Layout and navigation

Compact grids (under 60 columns or 20 rows) give the body to a frameless
filter/table and put aggregate CPU/RAM facts in the heading. Normal grids pair
system and memory summaries above bounded per-core graphs and the process
table. Wide grids (at least 110x24) use a summary sidebar and full-height
process table. Below 24x8 the app shows a resize prompt, gates hidden input,
and cancels ordinary forms before new input can submit them. Help remains
scrollable. Restoring size preserves process selection and filtering.

F2 cycles All, Processes and Summary; F3 cycles three summary balances on
normal/wide grids. In compact All, use Summary to see the full summaries and
graphs. `/` focuses filtering by name, PID or user; Tab/Shift+Tab move focus.
Enter/Esc leave the filter for the table. A focused filter receives literal
top-command letters, including q; paste is refused with explicit feedback.
Table arrows/PgUp/PgDn/Home/End navigate, Enter opens process detail, and
Esc/q close detail without quitting. Mouse header clicks sort, and the menus
mirror keyboard actions. The footer identifies focus, sort, delay and results.

F1/h/? open context help, which snapshots current view, focus, filter, sort,
sampling and facts at open. Sampling continues while it is open. Use arrows,
PgUp/PgDn, wheel and Home/End to reach the whole document even on short grids;
Esc/q/h/?/F1 close it. Ctrl+C exits even when a menu is open. q closes an open
menu first; q outside the filter/modal/menu quits.

## Top-compatible actions

P sorts CPU, M memory, N PID, T accumulated TIME+, R reverses sort; 1 switches
aggregate/per-core CPU, l/t/m toggle overview/CPU/memory, c switches program
name/full command line. d/s set a finite non-negative sampling delay using a
decimal dot; zero samples once per frame. Invalid values reopen the draft,
without clamping. Space refreshes immediately. No kill/renice, filesystem
writes, or process-tree claims are provided.

Action/result feedback is separate from sample number, age and configured
delay. A failed read keeps the last good rows and history with STALE feedback;
an initial failure shows no invented zero-valued facts. Space retries. Recovery
is labelled and does not erase the last action. On compact grids the short
footer prioritizes action/failure; full sampling context remains in help.

## Rendering and evidence

Fallback uses authored ASCII labels, borders, bars and graphs (`#` full bar,
`:` half bar). Narrow CPU labels are core IDs, optionally with a percentage
when it fits; graph height represents usage. Enhanced tiers retain persistent
images across clean frames. Dialogs suspend underlying graphs, preserving
residency; open menus omit/retire graphs so images cannot cover menu text and
restore them when closed. Hiding a view retires its graph producers.

The [capture bundle](../src/bin/captures/forge-top/README.md) contains actual
headless before/after cells for four sizes across all three tiers, plus sink
byte metadata. Initial All exposes 2/10/7/24 process rows at 24x8/40x16/80x24/
120x32, versus 0/0/2/6 before. Golden tests and emitted-ASCII modal tests prove
headless layout/input behavior. They are not physical-terminal screenshots,
pixel decoder validation, or a like-for-like bandwidth benchmark.
