# Plan 07 — docs and roadmap

**Goal:** record the delivered viewport architecture in the module guides and the roadmap, and run
the full verification gate. The roadmap-only close-out: `engine/CLAUDE.md` gains a `Viewport`
section, `editor/CLAUDE.md` is corrected to "panels own viewports, the engine drives," and the
`future/README.md` areas 4 / 6 / 8 status lines are updated (area 4 noting the pointer-routing seam
this delivers).

**Depends on Plans 00–06 (the delivered architecture).**

## Why it is its own plan

The guides are the project's authoritative architecture record, written in the present tense against
what the code *is* — they must be brought current in one deliberate pass once the architecture is
settled, not drifted across the implementation plans. Keeping it last means the docs describe the
final shape (region-owning viewports, the gather pass + composite, the drive-list, the mapping seam)
rather than an intermediate one.

## What lands

- **`engine/CLAUDE.md` — a `Viewport` section.** Beside `SceneRenderer`: `Viewport` owns a region +
  a `SceneRenderer` + a role (`Presented` = engine-composited into its region; `Offscreen` =
  sampled by ImGui or a material), takes a pushed `ViewState`, does the `Execute` + `Sample` barrier
  on `Render`, and produces a sampleable output + a bindless handle; the RAII drive-list
  (`Viewport::Create` + `RegisterViewport(Viewport&)`, `~Viewport` self-unregisters) and the render-phase
  order (render all viewports → `OnRender`/ImGui → gather + composite); the managed primary viewport
  as the game's plug-and-play path; splitscreen as N `Presented` viewports gathered into one assembly
  target; the region-derived `WindowToViewport`/`ScreenToWorldRay` mapping; the registration-order
  RTT contract and the single-copy/no-ring note. Cross-reference the unchanged `SceneRenderer`
  lifetime split and the unchanged composite encode.

- **`engine/CLAUDE.md` — the `Application` section** updated: `RegisterViewport(Viewport&)` (with
  `Viewport::Create` the factory), `GetPrimaryViewport`, `ApplicationInfo::ManagedViewport`, the engine render
  phase (the viewport list driven between `BeginFrame` and `EndFrame`, the narrowed `OnRender`, then
  the gather + composite incl. the `SetSwapChainTarget` re-target).

- **`editor/CLAUDE.md`** corrected: a render-owning panel **holds the `Unique<Viewport>` from
  `RegisterViewport`** (`Offscreen`), feeding its region from the ImGui content rect; the engine
  renders it; `EditorPanel` no longer carries the "record your scene render in `OnRender`" contract;
  `EditorHost`'s hand-rolled present path is gone (it runs the engine tail); the editor registers no
  `Presented` viewport, so the composite runs with zero placements (ImGui only). Update the
  `SceneViewportPanel` / `MaterialPreview` descriptions; note the `ScreenToWorldRay` picking seam now
  available to panels.

- **`future/README.md`** — area 8 gains a "Delivered — planset-31 (viewports)" paragraph (the
  region-owning `Viewport` over `SceneRenderer`, the gather pass + assembled composite, the engine
  drive-list, splitscreen, material-RTT); area 6 notes the editor's viewports moved onto the central
  list; **area
  4** records that *pointer* routing now has its seam (`WindowToViewport`/`ScreenToWorldRay`) and is
  the remaining multi-seat work, with *device* (gamepad) routing the independent half. The named
  follow-ons (declared inter-viewport dependencies, output ringing, a playable splitscreen /
  monitor sample) go under "still future."

- **This README** status column → `done`.

## Files

| File | Change |
|---|---|
| `engine/CLAUDE.md` | The `Viewport` section; the `Application` drive-list + frame-loop update. |
| `editor/CLAUDE.md` | Panels own/register `Offscreen` viewports; engine drives; zero-placement composite; `EditorPanel` contract change; the picking seam. |
| `plans/future/README.md` | Area 8 "Delivered — planset-31" paragraph; area 6 note; area 4 pointer-routing seam + multi-seat framing; the follow-ons. |
| `plans/planset-31/README.md` | Status column → `done`. |

## Verification

- **Full gate green:** clean `build` and `build-debug`; `ctest --test-dir build
  --output-on-failure` green across the bands; `smoke_golden` green (and unmoved);
  `hello_triangle_launcher_smoke` exits 0; `validation_gate` green under `build-debug`.
- The docs build (`cmake --build build --target docs`) stays clean where Doxygen is present — the
  new `Viewport.h` / `Ray.h` doc comments render into the API reference.
- A read-through confirms no plan/planset citations or future-work narrative leaked into the code
  comments added across 00–06 (the house comment rules).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
