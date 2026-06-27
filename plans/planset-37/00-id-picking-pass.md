# Plan 00 — entity-id picking pass + `Viewport::Pick` seam

**Goal:** stand up the engine-side **selection-picking** foundation — an optional, off-by-default
**id pass** in `SceneRenderer` that writes each drawn entity's id into an `R32Uint` target
(depth-tested, so the nearest visible surface wins), plus an **async readback** behind a
`Viewport::Pick(windowPoint, callback)` seam that resolves the texel under a click to an
`optional<Entity>`. This plan covers **meshes only**; billboards ride in on Plan 01. It changes
**no shipping render path** — the pass is allocated only when a picking flag is set, so a normal
viewport never pays for it and `smoke_golden` never moves. **Foundational; nothing precedes it.**

## Why it is its own plan

Selection-by-click is the gate for everything else in the planset (a gizmo needs a selection; a
command needs something to act on), and the id pass + readback is the one piece that lives in
`libveng` rather than the editor. Settling the GPU target, the entity-id encoding, and the
async-readback seam in isolation — engine-only, off by default, mesh-only — keeps Plans 01–04
purely editor-side. It is the same foundation-first move the renderer plansets use: land the
mechanism with a narrow first consumer, then widen.

## What lands

- **The `EntityId` target — a new `Format::R32Uint`, off by default.** A
  `SceneRendererSettings::Picking` topology flag (default `false`) adds an `EntityId` render
  target sized to the allocation extent. `R32Uint` is **not** in the `Renderer::Format` vocabulary
  enum today — it is appended (not inserted) as a new enumerator and mapped in `TypeMapping.h`, the
  same way every prior format addition lands; the device's support for it as a color attachment +
  transfer source + host-readable is confirmed through the existing format-feature query path, not
  assumed. The target **is not part of the `GBufferOutput` opaque contract**: the picking pipeline
  variant binds it as an additional color attachment in its **own** `RenderingInfo`, so the
  shipping geometry pass's 4-target g-buffer `RenderingInfo` — and the shipping surface pipeline,
  shader permutation, and attachment layout — are byte-for-byte unchanged. Cleared to **0** (the
  no-hit sentinel) each frame. Allocated and registered only while `Picking` is set, and enabled
  once for the editor viewport's lifetime (not toggled per-pick), so no mid-session `Configure`
  churns the target.

- **Pick-id encoding.** The selectable value is the **pick id** = the entity's **packed slot
  index + 1** (`u32`), so **0** reads back as "background / no entity." The generation is *not*
  encoded — a pick resolves within a couple of frames, and the readback subtracts 1 and maps the
  index back to the currently-live `Entity`, validating liveness (`Scene::IsAlive`) at resolve
  time, so a recycled slot resolves to the live occupant or to none. The existing per-instance
  channel (`firstInstance` / `GpuDrawData`) carries the **candidate slot** (`CandidateId`), *not*
  an entity index — so the entity index is **added** to the draw data: the `GpuDrawData` spare
  `u32 Pad2` becomes the entity index (keeping the 192-byte `static_assert`ed layout in lockstep
  with the shader-side `DrawData`), sourced from `VisibleMesh::Owner` at draw-data fill time, and
  read in the surface vertex stage and passed to the fragment as a flat varying. The id write
  therefore costs no new draw and rides both the CPU-direct and GPU-indirect submission paths
  (both read `DrawData[slot].Pad2`, never `firstInstance` itself, for the entity index).

- **`Viewport::Pick(ivec2 windowPoint, function<void(optional<Entity>)> onResolved)`.** Hit-tests
  the window point through the existing `WindowToViewport` (nullopt outside the region → the
  callback fires with `nullopt`); on a hit it records a **pick request** for the texel (the
  normalized point × allocation extent). The request is serviced in `Render`: after the geometry
  pass writes the id target, a **transfer-timeline** copy stages the requested texel's
  neighborhood (one `CopyImageToBuffer` of the `(2r+1)²` search window — a single region copy, not
  N point reads) into a host-visible staging buffer, and the result resolves on the **continuation
  pump** (the `TaskSystem` main-thread landing the engine already uses for async uploads), firing
  `onResolved` with the mapped `optional<Entity>`. So a pick is click-driven and latency-tolerant
  (a frame or two), never a `WaitIdle` stall.

- **A scene-epoch + caller-liveness cancellation contract.** Because the callback fires a frame or
  two after the click, the scene it resolves against may no longer be the scene the pick was issued
  against — `PrefabEditorPanel::Play()` repoints `PrefabEditContext::Scene` to a `Clone()` (fresh
  handles) and `Stop()` destroys it, and a document can close mid-flight. So `Pick` captures the
  `Scene*` it was issued against plus a monotonically-bumped **scene epoch**, and the resolve
  bails (fires `nullopt`, or simply drops) if the live scene/epoch no longer matches; the caller
  (the editor panel) additionally guards the callback against its own teardown (a panel-owned
  cancellation flag the continuation checks, stronger than `RequestCook`'s bare `[this]` capture
  which assumes the host outlives every request). The "one pick in flight" guard (Plan 01) is
  separate — it prevents a held button queuing a burst, not identity drift.

- **A small screen-space search radius.** The readback samples the exact texel first; if it is 0,
  it expands within the window and takes the nearest non-zero id to the cursor. When several
  non-zero ids fall in the window, the precedence is: the **exact texel always wins**; otherwise
  the front-most by the depth already resolved into the id target, ties broken by nearest to the
  cursor. This is general "click-near" forgiveness — it also helps thin mesh silhouettes — and is
  the seam Plan 01's billboard proxy footprint composes with. The radius is a small fixed constant
  (a handful of pixels).

- **The picking pipeline variant.** The surface pipeline gains an id-writing variant built only
  when `Picking` is set — and there are **two** such variants, for the static and the (separately
  submitted, CPU-direct) skinned surface shaders, each writing the per-instance entity index to
  the `EntityId` target. Both are distinct pipelines with their own `RenderingInfo` (the
  shipping variants are untouched). The shadow/post/debug passes are untouched.

## Decisions

1. **The id pass is editor-only and off by default.** Picking is an authoring concern, not a
   runtime one — entity ids have no place in the shipping g-buffer. The target is allocated only
   when `SceneRendererSettings::Picking` is set, so the shipping path is byte-identical and
   `smoke_golden` does not move. It re-renders geometry only on a frame a pick is requested (the
   pass can early-out when no request is pending), so its amortized cost is near zero.
2. **Encode the pick id (index + 1), resolve liveness late.** The id buffer is one `u32`; the
   packed slot index + 1 fits with 0 as the sentinel, and the readback subtracts 1 and maps the
   index → live `Entity` at resolve. Picking is same-frame-ish, so generation drift is not a
   concern; a stale slot resolves to the live occupant or `nullopt`, never to a dangling handle.
3. **Async readback over the transfer timeline + continuation pump.** This is the veng-idiomatic
   GPU→CPU path (the same machinery as async asset uploads), so a pick never stalls the render
   thread. The callback shape (`function<void(optional<Entity>)>`) mirrors
   `EditorHost::RequestCook`'s request-then-callback idiom rather than returning a polled future.
   Because the resolve lands a frame or two later, it carries a scene-epoch + caller-liveness
   guard (above) so it never applies an id against a scene that was swapped (Play `Clone()`) or
   torn down (document closed) in the interim.
4. **The entity index is added to the existing per-instance draw data.** The `firstInstance` /
   `GpuDrawData` channel already carries the candidate *slot*, not the entity — so the entity
   index occupies the spare `GpuDrawData::Pad2` (filled from `VisibleMesh::Owner`), read per draw
   from `DrawData[slot]`. This adds no second geometry pass and populates identically under
   `CullMode::CPU` and `CullMode::GPU` (both index `DrawData[slot].Pad2`, not `firstInstance`).
5. **`Pick` lives on `Viewport`, not `SceneRenderer` directly.** The `Viewport` already owns the
   region → ray mapping (`WindowToViewport`/`ScreenToWorldRay`) and the per-frame `Render`, so it
   is the natural home for "a window click → an entity"; it forwards the texel request to its
   owned `SceneRenderer`. Gameplay-agnostic — the viewport imports no editor type, only `Entity`.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/Types.h` + `engine/src/Renderer/Backend/TypeMapping.h` | Append `Format::R32Uint` (not inserted) and map it in the exhaustive backend switch. |
| `engine/include/Veng/Renderer/SceneRenderer.h` | Add `SceneRendererSettings::Picking`; declare the id-target accessor + the texel-pick request entry the `Viewport` forwards to. |
| `engine/src/Renderer/SceneRenderer.cpp` | Allocate/register the `R32Uint` id target under `Picking`; build the two id-writing surface pipeline variants (static + skinned) with their own `RenderingInfo` binding the `EntityId` attachment (shipping geometry-pass `RenderingInfo` untouched); service a pending pick (single transfer-timeline region copy). |
| `engine/src/Renderer/Picking.h` (new) | The `EntityIdFormat` / `EntityIdUsage` constants, kept out of the `GBufferOutput` opaque contract. |
| `engine/include/Veng/Renderer/Viewport.h` | Declare `Pick(ivec2 windowPoint, function<void(optional<Entity>)>)`. |
| `engine/src/Renderer/Viewport.cpp` | Hit-test via `WindowToViewport`, record the request, resolve on the continuation pump → `optional<Entity>` with the search-radius + liveness logic + the scene-epoch guard. |
| `engine/src/Scene/Visibility.h` / gather + `GpuDrawData` | Carry `VisibleMesh::Owner`'s packed index into `GpuDrawData::Pad2` (renamed) → the shader-side `DrawData`. |
| `engine/assets/core/shaders/surface*.{slang,shader.json}` | The id-writing variant of the surface fragment (gated), reading the entity index from `DrawData` and writing it to the `EntityId` target — for static + skinned. |
| `tests/…` | A `gpu`-band test: render a two-entity fixture with `Picking` on, `Pick` the texel over each, assert the resolved `Entity` (and `nullopt` over the cleared background). Skips with no ICD. |

## Verification

- Clean build; `ctest` green. The new `gpu` picking test resolves the correct entity per texel
  and `nullopt` over background; it skips cleanly with no Vulkan ICD.
- `smoke_golden` does **not** move — `Picking` defaults off, so the shipping deferred path
  allocates and writes nothing new.
- `include_hygiene` unaffected — the `Viewport::Pick` signature names only `Entity`/`function`/
  glm/`optional` (all PUBLIC-clean); no backend type leaks.
- Validation gate clean under `VE_DEBUG` (the id target's transitions and the transfer-timeline
  readback copy raise no unallowlisted error); the new `Format::R32Uint` passes the format-feature
  query for color-attachment + transfer-src on the test device (MoltenVK included).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
