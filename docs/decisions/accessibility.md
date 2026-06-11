# ADR: Accessibility bridge — L2 a11y tree → L1 → macOS NSAccessibility

Status: **Accepted — design + minimal sound subset.** The engine-side intake API
(flat-array a11y tree, headless-testable) + a `Window#a11y_available?` probe +
`Window#set_a11y_title` (the smallest real NSAccessibility touch) land now; the
NSAccessibility exposure of CHILD elements (the element-array dance) is staged with
a precise reason below.
Date: 2026-06-11
Relates to: `docs/decisions/frame-pacing.md` (the objc-runtime-via-dlopen pattern),
the file-dialog path in `runtime/sdl_window.c` (the established NSPanel/objc
pattern this mirrors), `docs/ROADMAP.md` (Phase 3 — Part B).

## Context

L3 apps built on quiver (L2) need to be usable with VoiceOver / assistive tech.
The platform a11y model on macOS is **NSAccessibility**: the OS walks a tree of
accessibility elements (each with a role, label, frame, and state) rooted at the
NSWindow, querying them on demand. L2 owns the widget tree and the semantics
(this is a button, that is a heading); L1 (canvas) owns the only `unsafe`/FFI
seam, so L1 must be where the NSAccessibility objects live. This ADR fixes the
contract between the two, and what lands now vs is staged.

The constraints that bound the design are the SAME ones every Phase-2 desktop
service lived under:

- **The objc runtime is reached by dlopen, no link dependency** — exactly the
  file-dialog (`rx_appkit_init` / `ak_msg`) and Metal-device patterns already in
  `runtime/sdl_window.c`. No new framework link.
- **Cocoa/AppKit is unsafe after `fork()`-without-`exec()`**, and the test harness
  forks per case. So any LIVE NSAccessibility touch is gated by the shared
  `rx_window_allowed()` fork gate (the file-dialog discipline) and returns a clean
  "unavailable" headless — a11y is a **live-window-only** capability. The
  ENGINE-SIDE intake (storing the tree) is pure C with no Cocoa, so it IS
  headless-testable in the harness.
- **L2 is 100% safe Ruxen** — it cannot build objc objects. It hands L1 a
  description of the tree as flat data; L1 turns that into NSAccessibility objects.

## Decisions

### 1. The L2 → L1 contract: a flat a11y tree pushed as primitive arrays

L2 describes its a11y tree to L1 as a flat list of NODES, each a fixed tuple of
primitives keyed by a node id — the SAME arena/flat-array idiom quiver already
uses for its retained layout/paint data (no pointers, no objc, no Ruxen objects
crossing the FFI). The intake API (engine-side, headless):

- `push_a11y_node(id, role, label, x, y, w, h)` — append/replace one node. `id` is
  L2's stable node id (an Int); `role` is a small stable int enum (mirrors the
  blend-mode / cursor-kind enums: 0 group / 1 button / 2 static-text / 3 image /
  4 heading / 5 link — grown as L2 needs); `label` is the accessibility label
  (a String, copied into engine-owned storage); `x,y,w,h` is the element's frame
  in window points. A `parent` linkage is deferred (see §4) — the first cut is a
  FLAT list of elements under the window, which is enough for VoiceOver to
  enumerate controls.
- `clear_a11y_tree` — drop all nodes. **Tree updates are wholesale re-push per
  change (Tier-1):** when L2's tree changes, it clears and re-pushes. This is
  simple and correct; an incremental diff is a later optimization, not a
  correctness requirement (the tree is small — visible controls).
- `a11y_node_count` — read back the stored node count (the headless pin reads
  this to prove the intake round-trips without any live window).

The store is a process-wide fixed-capacity table (the never-freed-singleton model
the font/window tables use), each slot owning its label String copy. No heap
churn per frame beyond the label copies on a re-push.

### 2. L1 → macOS: NSAccessibility objects via the objc runtime (staged)

On macOS, L1 exposes the stored tree to the OS by implementing the
NSAccessibility protocol: the NSWindow's content view returns
`accessibilityChildren` — an array of accessibility element objects, each
answering `accessibilityRole` / `accessibilityLabel` / `accessibilityFrame` from
a stored node. These are reached through the objc runtime by dlopen (the
file-dialog pattern). Building a CUSTOM NSAccessibilityElement subclass at runtime
(registering a class with `objc_allocateClassPair`, adding method imps) is the
deep part — see §4.

### 3. What lands NOW (the minimal sound subset)

- **`Window#a11y_available?`** — a capability probe: true only when NOT under the
  forked harness AND the objc runtime + AppKit are reachable (reusing
  `rx_appkit_init` + `rx_window_allowed`). Headless / forked → false, so callers
  degrade cleanly.
- **`Window#set_a11y_title(s)`** — the SMALLEST real NSAccessibility touch that
  proves the objc path: set the window's accessibility title. This exercises
  `objc_msgSend` to an NSAccessibility setter on a real Cocoa object, gated by
  `rx_window_allowed` (clean Err headless). It is the window-level role/label
  exposure the ADR calls for — the window itself is an a11y element before any
  child is.
- **The engine-side intake** (`push_a11y_node` / `clear_a11y_tree` /
  `a11y_node_count`) — fully headless, pinned in the harness (the tree round-trips
  as flat data with no live window). This is the contract L2 codes against TODAY;
  the macOS exposure of those stored nodes as child elements is §4.

### 4. What is STAGED, and the precise reason

**The NSAccessibility CHILD-element array** — exposing each stored node as an
NSAccessibilityElement the OS can query — is staged. The reason is concrete: it
requires building accessibility element objects that answer the NSAccessibility
protocol selectors (`accessibilityRole`, `accessibilityLabel`,
`accessibilityFrame`, `accessibilityParent`) from our stored nodes, which means
either (a) registering a custom objc class at runtime
(`objc_allocateClassPair` + `class_addMethod` with C trampolines for each
selector) or (b) using `NSAccessibilityElement` instances and setting their
attributes via the accessibility setters. Both are a non-trivial objc-runtime
dance whose CORRECTNESS can only be verified with a live window + a real assistive
client (VoiceOver) — it cannot be pinned in the forked, headless harness (Cocoa
after fork is blocked, exactly like the file dialogs and on-screen Metal). So,
per the established discipline (file dialogs, on-screen Metal), the child-element
exposure is filed as the precise remainder and will be proven by a MANUAL
`examples/a11y_verify.c` (a human runs VoiceOver against a live window), NOT wired
into the automated suite. The automated bar this cycle is: the intake contract
pinned headless + the `a11y_available?` / `set_a11y_title` probes' Err/contract
behavior.

This mirrors the file-dialog landing exactly: the testable subset (intake +
probes) lands and is pinned; the human-verified live subset (child elements via
VoiceOver) is filed with a named verify example.

### 5. Focus

Focus is part of the contract but its sound landing is coupled to §4 (the OS
queries `accessibilityFocusedUIElement` on the window): once child elements
exist, a `set_a11y_focus(id)` setter points the window's focused element at a
stored node. Until then `a11y_available?` reporting false off-host means L2's
focus calls degrade cleanly. Focus is filed with §4 as part of the live remainder.

## Consequences

- New `ruxen_canvas_*`: `window_a11y_available`, `window_set_a11y_title`,
  `push_a11y_node`, `clear_a11y_tree`, `a11y_node_count`. The intake trio is in
  `skia_shim.c` (pure C, headless); the live window touches are in
  `sdl_window.c` (objc, gated by `rx_window_allowed`).
- No new dependency: the objc runtime + AppKit are already dlopen'd for the file
  dialogs.
- Pins: `tests/accessibility.rx` — the intake round-trips headless (push N nodes →
  count == N → clear → count == 0; role/label/frame stored), and the
  `a11y_available?` / `set_a11y_title` Err/contract under the forked harness. The
  live child-element + VoiceOver path is the filed `examples/a11y_verify.c`
  remainder.
