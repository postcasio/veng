# Plan 00 — the `Viewport` type

**Goal:** add `Veng::Renderer::Viewport` — a single-owner type that **owns its rectangle of the
window**, renders a world from a camera into its own texture, and carries a role saying whether the
engine composites it. It owns a `SceneRenderer`, carries a `ViewportRegion` (its window rect, whose
size drives the render extent), takes a per-frame `ViewState` pushed by its owner, and on
`Render(cmd)` does the `Execute` + `PrepareForAccess(Sample)` pair itself. Its product is a
sampleable `Ref<ImageView>` and a bindless `TextureHandle`. **Purely additive** — no caller is
migrated here (Plans 02–03 do that), and the window↔view *mapping* (`WindowToViewport`,
`ScreenToWorldRay`) is deferred to Plan 06.

## Why it is its own plan

It is the foundational seam every later plan builds on, reviewable against the renderer's existing
`SceneView`/`GetOutput` contract on its own — a wrapper with a clear surface and a headless render
test, before any drive-list, compositor change, or editor refactor depends on it. Splitting it from
the `Application` integration (Plan 02) keeps the type's surface reviewable without the frame-loop
churn.

## What lands

- **`Viewport::Role` — `{ Presented, Offscreen }`.** `Presented`: the engine compositor places this
  viewport's texture into its region (at most a handful per app — the game's view, splitscreen
  quadrants). `Offscreen`: a consumer samples its texture (an ImGui panel, a material). The role
  gates **only** whether the engine composites it; the viewport renders identically either way, and
  both roles own a region.

- **`ViewportRegion` — the window rectangle.** `{ ivec2 Offset; uvec2 Extent; }` in window
  framebuffer pixels. The `Extent` is the render resolution; the `Offset` is where a `Presented`
  viewport is placed (and where an `Offscreen` panel viewport's picking maps from). `SetRegion`
  stores it and drives a **debounced** internal `SceneRenderer::Resize` when the extent changes
  (the resize-on-next-`Render` pattern `SceneViewportPanel` already uses).

- **`ViewState` — the per-frame source.** The input subset, distinct from the internal `SceneView`
  (which also carries renderer-written scratch — `LightCount`, the cascade matrices, the
  broadphase): `const Scene* World = nullptr` (null ⇒ renders nothing — a closed document),
  `CameraView Camera`, and the per-frame `Exposure` / `BloomThreshold` / `BloomIntensity` /
  `BloomRadius` knobs. `SetViewState` stores it; `Render` reads it; the viewport **retains** the
  camera for Plan 06's `ScreenToWorldRay`.

- **`ViewportInfo`** — `Context&`, `AssetManager&`, `ViewportRegion Region`,
  `Format ColorFormat = Format::Undefined` (resolved to `Context::GetOutputFormat()` inside `Create`
  when left `Undefined` — a struct member can't default to a value pulled from the `Context&`),
  `SceneRendererSettings Settings`, `Role Role = Offscreen`. Designated-initializer house idiom;
  forwarded into the owned `SceneRenderer::Create` (extent = `Region.Extent`).

- **`Viewport`** — `Unique`, single-owner (like `SceneRenderer`); `Create(const ViewportInfo&)` is
  the factory. `Create` constructs an **unregistered** viewport (driveable on its own — the gpu test
  calls `Render` directly); registration with the engine drive-list is Plan 02, and a registered
  viewport carries a non-owning back-reference to its drive-list and **self-unregisters in its
  destructor** (the back-reference is null until registered, so an unregistered viewport's destructor
  is a no-op). The owner holds the `Unique`; the engine holds only a raw pointer — dropping the
  `Unique` removes the engine's pointer, no explicit unregister call.
  - `void SetRegion(const ViewportRegion&)` — placement + extent; debounced resize on extent change.
    A zero extent is ignored (a collapsed/first-frame panel reports `{0,0}`; mirrors the existing
    `MaterialPreview::Resize` and `SceneViewportPanel` `m_PendingExtent` guards), so it never drives
    `SceneRenderer::Resize(0,0)`.
  - `void SetViewState(const ViewState&)` — per-frame source bind (stores a copy).
  - `void Configure(const SceneRendererSettings&)` — forwarded; invalidates `GetOutput`/
    `GetOutputHandle` exactly as `SceneRenderer` does, so a cached output/handle must be re-fetched
    after this and after a region resize (the renderer's existing rule).
  - `void Render(CommandBuffer&)` — builds the internal `SceneView` from the stored `ViewState`
    (null `World` ⇒ no-op early return **before** constructing `SceneView`, whose `World` is a
    `const Scene&` reference that cannot be built from a null pointer; a freshly-registered viewport
    not yet given a `ViewState` therefore renders nothing rather than dereferencing null), applies
    any pending region resize, calls
    `SceneRenderer::Execute`, then `cmd.PrepareForAccess(GetOutput(), AccessKind::Sample)`. The
    `Execute` + barrier pair copied verbatim in `main.cpp` and `SceneViewportPanel::OnRender` today
    moves here once.
  - `[[nodiscard]] Ref<ImageView> GetOutput() const` / `[[nodiscard]] TextureHandle GetOutputHandle() const`
    — the sampleable result and its bindless slot (for the compositor, `ImGuiLayer::CreateTexture`,
    and `Material::SetTextureHandle`).
  - `[[nodiscard]] const ViewportRegion& GetRegion() const` / `[[nodiscard]] Role GetRole() const`.
  - `[[nodiscard]] SceneRenderer& GetRenderer() const` — escape hatch for the renderer-stats surface
    (`GetLastDrawnCount`, etc.) the sample's Stats window and the editor toolbar read, rather than
    re-exporting every getter.

## Decisions

1. **The region is universal; the role gates compositing.** Both `Presented` and `Offscreen`
   viewports own a region — an `Offscreen` editor panel needs it for resize and picking. The role
   answers one question only: does the *engine* composite this into the window? This keeps RTT the
   single render path and lets one type serve the game, splitscreen, the editor, and material-RTT.

2. **`SetRegion` carries placement and extent together.** A view's window rectangle *is* its
   placement and its resolution; splitting them would let them drift. A material-source viewport that
   has no window placement simply sets `Offset = {0,0}` and uses its extent as a render size — the
   degenerate case, not a separate concept.

3. **Push, not pull, for the source.** `SetViewState` takes the camera explicitly; the editor's
   camera lives outside the scene, the game resolves its own. The viewport retains the camera so the
   Plan 06 mapping is self-contained.

4. **A wrapper, not a rewrite.** `Viewport` owns a `SceneRenderer` and forwards; the renderer's
   lifetime split is unchanged and visible through `GetRenderer()`. The new type adds region, role,
   the per-frame source bind, and the barrier — nothing about the pipeline.

5. **Lifetime is RAII; the engine reference is non-owning.** The owner holds the `Unique<Viewport>`;
   the engine drive-list (Plan 02) holds only a raw `Viewport*`. A registered viewport keeps a
   back-reference to the drive-list and removes its own pointer in the destructor, so dropping the
   `Unique` is the only cleanup a caller writes — no explicit unregister, matching the engine's
   existing "dropping a resource is safe" back-reference idiom. `GetRenderer() const` returns a
   *mutable* `SceneRenderer&` by the same Native-idiom rule the rest of the renderer uses (the
   wrapper's constness is its own identity, not the GPU state behind it).

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/Viewport.h` *(new)* | `Role`, `ViewportRegion`, `ViewState`, `ViewportInfo`, the `Viewport` class (full Doxygen). |
| `engine/src/Renderer/Viewport.cpp` *(new)* | `Create` (owns a `SceneRenderer`; resolves `ColorFormat`), `SetRegion` (debounced resize, zero-extent guard), `SetViewState`, `Configure`, `Render` (`SceneView` build + `Execute` + `PrepareForAccess`), the self-unregistering destructor, getters. (Beside the sibling `engine/src/Renderer/SceneRenderer.cpp` — not under `Backend/`.) |
| `engine/CMakeLists.txt` | Add the new TU. |
| `tests/gpu/…` | Headless render test: create a `Viewport` over a one-mesh scene, set a region + a fixed-camera `ViewState`, `Render`, assert `GetOutput()` is correctly sized and `GetOutputHandle()` valid; a region extent change resizes the output; a null-`World` `Render` is a no-op. |

## Verification

- Clean build; `ctest` green. The new gpu test passes; skips cleanly with no ICD (label `gpu`).
- `include_hygiene` green — `Viewport.h` pulls only public renderer headers (`SceneRenderer.h`,
  `Types.h`, `ImageView.h`, `BindlessRegistry.h` for `TextureHandle`), no backend include.
- `smoke_golden` does **not** move — no caller renders through a `Viewport` yet.
- `validation_gate` green under `build-debug` — a `Viewport` render issues exactly the
  `SceneRenderer` workload, no new descriptor or barrier surface.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
