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

- **The `EntityId` g-buffer-adjacent target — `R32Uint`, off by default.** A
  `SceneRendererSettings::Picking` topology flag (default `false`) adds an `EntityId` render
  target sized to the allocation extent, written by the geometry pass as an extra MRT channel
  alongside the g-buffer. **It is not part of the `GBufferOutput` opaque contract** — the
  shipping deferred path never allocates or writes it; the surface fragment writes the id only
  when the picking pipeline variant is bound. Cleared to **0** (the no-hit sentinel) each frame.
  Allocated and registered only while `Picking` is set; a `Configure` toggles it like any other
  battery.

- **Entity-index encoding.** The pass writes the drawn entity's **index + 1** (`u32`), so **0**
  reads back as "background / no entity." The generation is *not* encoded — a pick resolves
  within a couple of frames, and the readback maps the index back to the currently-live `Entity`
  and validates liveness (`Scene::IsAlive`) at resolve time, so a recycled slot resolves to the
  live occupant or to none. The entity is already visited by the candidate gather
  (`GatherMeshes` over `(Transform, MeshRenderer)`); thread its packed index through
  `VisibleMesh` → the per-instance `GpuDrawData` the buffer-indexed surface shader already reads
  (`firstInstance`-carried candidate id is the existing channel), so the id write costs no new
  draw and rides both the CPU-direct and GPU-indirect submission paths.

- **`Viewport::Pick(ivec2 windowPoint, function<void(optional<Entity>)> onResolved)`.** Hit-tests
  the window point through the existing `WindowToViewport` (nullopt outside the region → the
  callback fires with `nullopt`); on a hit it records a **pick request** for the texel (the
  normalized point × allocation extent). The request is serviced in `Render`: after the geometry
  pass writes the id target, a **transfer-timeline** copy stages the requested texel (plus a
  small neighborhood — see the search radius below) into a host-visible buffer, and the result
  resolves on the **continuation pump** (the `TaskSystem` main-thread landing the engine already
  uses for async uploads), firing `onResolved` with the mapped `optional<Entity>`. So a pick is
  click-driven and latency-tolerant (a frame or two), never a `WaitIdle` stall.

- **A small screen-space search radius.** The readback samples the exact texel first; if it is 0,
  it expands a few pixels and takes the nearest non-zero id to the cursor (front-most by the
  depth already resolved into the id target). This is general "click-near" forgiveness — it also
  helps thin mesh silhouettes — and is the seam Plan 01's billboard proxy footprint composes
  with. The radius is a small fixed constant (a handful of pixels).

- **The picking pipeline variant.** The surface vertex/fragment pipeline gains an id-writing
  variant built only when `Picking` is set (the surface shader writes the per-instance entity id
  to the `EntityId` target under a compile/runtime gate); the static and skinned draw paths both
  route through it. The shadow/post/debug passes are untouched.

## Decisions

1. **The id pass is editor-only and off by default.** Picking is an authoring concern, not a
   runtime one — entity ids have no place in the shipping g-buffer. The target is allocated only
   when `SceneRendererSettings::Picking` is set, so the shipping path is byte-identical and
   `smoke_golden` does not move. It re-renders geometry only on a frame a pick is requested (the
   pass can early-out when no request is pending), so its amortized cost is near zero.
2. **Encode entity index + 1, resolve liveness late.** The id buffer is one `u32`; the index + 1
   fits with 0 as the sentinel, and the generation is recovered by mapping index → live `Entity`
   at resolve. Picking is same-frame-ish, so generation drift is not a concern; a stale slot
   resolves to the live occupant or `nullopt`, never to a dangling handle.
3. **Async readback over the transfer timeline + continuation pump.** This is the veng-idiomatic
   GPU→CPU path (the same machinery as async asset uploads), so a pick never stalls the render
   thread. The callback shape (`function<void(optional<Entity>)>`) mirrors
   `EditorHost::RequestCook`'s request-then-callback idiom rather than returning a polled future.
4. **The id write rides the existing per-instance draw data.** The candidate index is already
   carried per instance (`firstInstance` / `GpuDrawData`); the entity index threads through the
   same channel, so the id pass adds no second geometry pass and works identically under
   `CullMode::CPU` and `CullMode::GPU`.
5. **`Pick` lives on `Viewport`, not `SceneRenderer` directly.** The `Viewport` already owns the
   region → ray mapping (`WindowToViewport`/`ScreenToWorldRay`) and the per-frame `Render`, so it
   is the natural home for "a window click → an entity"; it forwards the texel request to its
   owned `SceneRenderer`. Gameplay-agnostic — the viewport imports no editor type, only `Entity`.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/SceneRenderer.h` | Add `SceneRendererSettings::Picking`; declare the id-target accessor + the texel-pick request entry the `Viewport` forwards to. |
| `engine/src/Renderer/SceneRenderer.cpp` | Allocate/register the `R32Uint` id target under `Picking`; build the id-writing surface pipeline variant; wire the geometry pass's extra MRT write; service a pending pick (transfer-timeline texel copy). |
| `engine/src/Renderer/GBuffer.h` (or a new `Picking.h`) | The id-target format/usage constant, kept out of the `GBufferOutput` opaque contract. |
| `engine/include/Veng/Renderer/Viewport.h` | Declare `Pick(ivec2 windowPoint, function<void(optional<Entity>)>)`. |
| `engine/src/Renderer/Viewport.cpp` | Hit-test via `WindowToViewport`, record the request, resolve on the continuation pump → `optional<Entity>` with the search-radius + liveness logic. |
| `engine/src/Scene/Visibility.h` / gather + `GpuDrawData` | Thread the packed entity index through `VisibleMesh` → per-instance draw data. |
| `engine/assets/core/shaders/surface*.{slang,shader.json}` | The id-writing variant of the surface fragment (gated), for static + skinned. |
| `tests/…` | A `gpu`-band test: render a two-entity fixture with `Picking` on, `Pick` the texel over each, assert the resolved `Entity` (and `nullopt` over the cleared background). Skips with no ICD. |

## Verification

- Clean build; `ctest` green. The new `gpu` picking test resolves the correct entity per texel
  and `nullopt` over background; it skips cleanly with no Vulkan ICD.
- `smoke_golden` does **not** move — `Picking` defaults off, so the shipping deferred path
  allocates and writes nothing new.
- `include_hygiene` unaffected — the `Viewport::Pick` signature names only `Entity`/`function`/
  glm/`optional` (all PUBLIC-clean); no backend type leaks.
- Validation gate clean under `VE_DEBUG` (the id target's transitions and the transfer-timeline
  readback copy raise no unallowlisted error).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
