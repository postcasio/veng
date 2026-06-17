# future — work beyond the current plansets (DRAFT / vision)

> Not a planset and not scheduled — a **holding area** for the larger phases
> ahead. Each area below becomes its **own planset** when taken up and detailed
> planset-1 style. Direction, not plans.

Captured now so the earlier phases stay coherent with where veng is going.

## Areas

Numbered for stable cross-references. **Delivered** areas are documented in their
plansets (see [plans/README.md](../README.md)) and are *not* re-narrated here —
only their **still-future remainder** is kept, plus any delivered capability a
pending area builds on directly. The substance of this document is the
**remaining** work: the editor's scene editor (area 6, sub-area D), event/input
(area 4), `ImGuiCompositePass` (area 11), and the named still-future increments of
the areas done in part (areas 1, 2, 7, 8, 9, 10).

### 1. Asset system — remaining: hot-reload

Delivered by planset-5 + planset-6. **Still future — hot-reload (`Reload`).** Its
re-upload half is exactly what the async path delivers, but its re-cook half
conflicts with offline-only cooking, so it needs a dev-only watcher design.
**Design overview:** [asset-system.md](asset-system.md).

### 2. Threading / task system — remaining: task graph, staging pool, cancellation

Delivered by planset-6. **Still future:** a task *graph* (inter-job dependencies),
staging-buffer pooling, and cancellation. **Design overview:**
[threading-task-system.md](threading-task-system.md).

### 3. De-globalize the rendering context — DONE (planset-4)

Delivered; nothing pending builds on it. `Context::Instance()` / `s_Instance` are
gone — every resource `Create` takes an explicit `Context&`.

### 4. Event & input systems

Thin and partially stubbed: `EventType` declares focus/move events with no
classes; input lives ad-hoc on `Window` (`MouseButton` unused; no actions/
bindings/devices). Off the critical path — revisit when gameplay drives the
requirements.

### 5. Unit testing / test infrastructure — DONE (planset-3 + planset-4)

Delivered; nothing pending builds on it. doctest + CTest wiring, the
separate-process death harness, the in-process `veng_gpu` integration suite, and a
local `ctest -L validation` gate. CI with a hosted software ICD was explicitly
descoped.

### 6. Editor application

The authoring environment, spanning several plansets. **Design overview:**
[editor.md](editor.md), with the build-model prerequisite in
[game-module.md](game-module.md). Delivered sub-areas — the scene editor builds on
all three:

- **Sub-area A — game = shared lib + launcher (planset-9).** `libgame` + a thin
  launcher that `dlopen`s one C-ABI `VengModuleRegister`; the ABI carries an
  `EditorRegistry*` slot the launcher leaves null and the editor host makes
  non-null.
- **Sub-area B — editor shell + framework (planset-14).** `libveng_editor`
  (`EditorPanel`, `EditorRegistry`, the docking `EditorHost`), the `veng_add_editor`
  macro, the reflection-driven inspector (`Scene::ForEachComponent` → a widget per
  `FieldClass`, overrides via `RegisterFieldWidget`), cook-on-demand (`libveng_cook`
  in the editor exe only → injected `CookBackend` → `MountMemory` hot-reload), and
  the texture editor.
- **Sub-area C — material node editor (planset-15).** The generic, device-free
  **`VengEditor/NodeGraph/`** surface (topology core + data-driven `NodeType`/
  `NodeCatalog` + JSON (de)serialization) — **reused by sub-area D** — its material
  specialization, the `MaterialEditorPanel`, and the reusable `MaterialPreview`.

**Sub-area D — scene editor — STILL FUTURE (the only remaining editor sub-area).**
Viewport panel (reuses the `SceneRenderer`, area 8), hierarchy panel, gizmos
(ImGuizmo or hand-rolled), save round-trip to `.prefab.json`. **All its gates are
met:** the runtime scene model (area 7, planset-10), the cooked prefab + module
reflection (area 10, planset-11), the inspector + cook-on-demand foundation
(sub-area B, planset-14), and the reusable `VengEditor/NodeGraph/` surface
(sub-area C, planset-15). Its native-type inspectors reuse area 10's module
reflection rather than re-introducing it; its viewport renders one `Scene` through
N `SceneRenderer` instances with no API change. A planset of its own.

Also still future: **installing `veng_add_game` for downstream
`find_package(veng)` consumers** (see [game-module.md](game-module.md)).

### 7. Scene / entity model — remaining: systems, perf, reflection follow-ons

Runtime half delivered by planset-10; the cooked prefab asset + module-ABI seam by
planset-11 (area 10). **The scene editor (area 6, sub-D) consumes the runtime scene
model directly.**

**Still future:**

- A **systems** framework (planset-10 ships storage + queries; the app writes its
  own update loops over `Each`/`View`).
- **Archetype storage** and **dirty-flag** transform propagation (perf
  optimizations behind the same API).
- Migrating `VE_REFLECT` to inline `[[=…]]` **annotation reflection** once
  AppleClang gains P2996/P3394.

**Named follow-on — unify `ShaderInterface`/`MaterialField` onto the reflection
layer.** A later planset re-expresses the GPU-data field tables (the cooker's reflected
shader/material interface) on planset-10's reflection layer, so the editor inspects
material parameters through the **same generic field-walker** it uses for components,
rather than a parallel mechanism. The two are different mechanisms — `VE_REFLECT`
describes **CPU struct layout** (`offsetof`-based, hand-authored, in `libveng`),
whereas `ShaderInterface`/`MaterialField` describe **GPU data layout** (std140/430
offsets, descriptor slots) reflected **offline by the cooker from Slang**. Folding it
into the CPU-only ECS stream would drag the cooker and the material runtime into it. The
reflection layer is built generic enough to host it (one open `TypeId` space, closed
`FieldClass`, a walker proven *non-component-bound* by planset-10's round-trip tests), so
this is additive when taken up. **Design caveat to settle when it is:**
`FieldDescriptor.Offset` is single-purpose today — a C++ `offsetof`. A GPU field's offset
is a *different* number (a std140/430 buffer offset), so the unification needs
`FieldDescriptor` to carry both (or a layout/`FieldClass` distinction) for one walker to
drive a **CPU memcpy** in the component case and a **GPU buffer write** in the material
case. Decide that representation up front; it is painful to retrofit onto a populated
descriptor table.

**Named follow-on — container/array fields in the reflection layer.** `FieldClass` is closed
and has **no list/array class** (`Vector` is a glm math vector, not `std::vector<T>`), so a
`vector<T>` or `T[N]` field cannot be reflected, serialized, or inspected today —
`FieldClassOf<T>()` silently falls through to `FieldClass::Struct` for an unspecialised type.
A later planset adds container support: a fixed-size `T[N]`/`std::array` is the easy subset
(walkable by element offset + stride — element `TypeId` + count on the `FieldDescriptor`, no
erased ops), while a dynamic `vector<T>` needs the `FieldDescriptor` to carry **type-erased
container ops** (size / element-at / resize) plus the element's `TypeId`/`FieldClass`, with the
generic walker, the name-keyed serializer, and the editor inspectors each gaining an `Array`
arm. That erased-ops payload is exactly the complexity the closed `FieldClass` set was built to
keep out, so it is a deliberate reflection-layer decision — it pairs with the
`ShaderInterface`/`MaterialField` unification above (both grow `FieldDescriptor`) and is decided
at the reflection layer, not in a consumer. (The node-based material editor needs none of this:
its node properties are flat leaves, the graph's node/link collections use a bespoke graph
serializer, and a variable-arity node is modelled as **dynamic pins on the node instance**, never
as an array-typed property.)

### 8. Scene renderer / render-pipeline architecture — remaining: the über-pipeline batteries

The `SceneRenderer` shell + minimal deferred spine (g-buffer → deferred lighting →
tonemap) delivered by planset-12; frames-in-flight > 1 correctness by planset-13.
**The editor's scene viewport (area 6, sub-D) consumes this delivered
`SceneRenderer`.** **Design overview:** [scene-renderer.md](scene-renderer.md).

**Still future:** the rest of the über-pipeline **batteries** — shadows, SSAO,
bloom, MSAA, a transparent/forward pass (a second material contract), a post stack,
and a **G2 PBR g-buffer target** that extends the `GBufferOutput` struct — each its
own increment behind the same `ScenePass` + `Configure`-recompile mechanism;
**multiple & typed lights** (point/spot, light culling); **history-buffer ringing**
for temporal effects (TAA/motion-blur reading an older frame); **cross-queue
synchronization** (an explicit semaphore once a handoff side moves off the single
graphics queue); and **parallel pass recording** into secondary command buffers
(area 2's seam — the user-pointer channel is shaped for it, not built).

### 9. Compiled RenderGraph — remaining: culling, multi-queue, parallel recording

Delivered by planset-8. **Still future:** dead-pass culling, multi-queue /
async-compute scheduling, and parallel pass recording into secondary command
buffers (area 2's seam). **Design overview:**
[compiled-rendergraph.md](compiled-rendergraph.md).

### 10. Cooker-side module reflection — remaining: cross-compiled cooking

Delivered by planset-11 (`vengc cook --module` `dlopen`s `libgame` and reflects its
component types; the cooked prefab asset rides on it). **The editor's native-type
inspectors (area 6, sub-D) reuse this same module-reflection seam.** **Still future
— cross-compiled cooking:** `dlopen` reflection loads a **host == target** lib, so
cooking a cross-compiled target lib on the build host is a latent constraint for the
anticipated Windows port — recorded, not solved.

### 11. ImGuiCompositePass

Every `SceneRenderer`-based app today hand-writes an identical fullscreen composite
pass (scene offscreen output → swapchain) and manually re-registers the bindless
slot and ImGui texture whenever `Resize`/`Configure` recreates the output image.
An engine-provided **`ImGuiCompositePass`** eliminates that boilerplate: the app
wires it into its own `RenderGraph` (compositing stays in the app), calls
`SetSource(imageView)` once after `Resize`/`Configure`, and the pass owns both
registrations internally. Named deliberately — this is not a general-purpose
compositor; it is scoped to the ImGui workflow. **Design overview:**
[imgui-composite-pass.md](imgui-composite-pass.md).

## Ordering & dependencies

The order to *take the remaining areas up* (each becomes its own planset), not a
schedule:

1. **Editor — scene editor (area 6, sub-area D)** is the **next editor planset**;
   all its gates are met (area 7, area 10, area 8's `SceneRenderer`, and editor
   sub-areas B/C).
2. **Event & input (area 4)**, **`ImGuiCompositePass` (area 11)**, and the named
   still-future increments of the areas done in part (1, 2, 7, 8, 9, 10) are each
   independent and off the critical path — slot in whenever wanted.

## Cross-cutting concerns

Considerations that span the work above and are cheaper to decide early than to
retrofit.

- **Process discipline.** Keep planset-1's cadence — small, sample-verified,
  per-plan increments. Every planset to date has followed it; the same discipline
  applies to the scene editor (area 6, sub-D) ahead.

## Status

Vision only beyond what is delivered in the plansets
([plans/README.md](../README.md)).

**Next:** the **scene editor** (area 6, sub-area D) — all its gates met (areas 7,
10, 8, and editor sub-areas B/C).

**Undetailed / unscheduled:** area 4 (events/input), area 11 (`ImGuiCompositePass`),
and the named still-future increments of the areas done in part — area 1's
**hot-reload**, area 2's task graph / staging pool / cancellation, area 7's
**systems framework** + perf follow-ons + the `ShaderInterface`/`MaterialField`
unification + container/array fields, area 8's **batteries** + multiple/typed lights
+ history-buffer ringing / cross-queue sync, area 9's culling / multi-queue /
parallel recording, and area 10's **cross-compiled cooking**. Each becomes its own
planset when taken up.
