# Plan 04 — material preview surface (Layer P) — PARALLEL

**Goal:** a reusable surface that renders one material on a sphere into a preview render
target shown via `ImGui::Image`. It depends **only on the shipped engine** (`Primitives`,
`SceneRenderer`, `Material`, `ImGuiLayer`) — **not** on the node graph (plans 01–03) — so it
is dispatched in **Wave 1 alongside plan 01** and hides under the 01→02→03 critical path.

## `MaterialPreview`

`editor/src/material/MaterialPreview.h` — a self-contained helper the material editor panel
embeds:

```cpp
namespace VengEditor
{
    class MaterialPreview
    {
    public:
        MaterialPreview(Veng::Renderer::Context&, Veng::AssetManager&, Veng::ImGuiLayer&,
                        Veng::uvec2 extent);
        ~MaterialPreview();

        // Swap the previewed material (after a recook hands back a fresh handle).
        void SetMaterial(Veng::AssetHandle<Veng::Material>);

        // Record the scene render for this frame; call before the ImGui frame.
        // No delta param — like SceneViewportPanel::Render, it reads
        // Time::GetDeltaTime() internally (and the SceneView carries Delta).
        void Render(Veng::Renderer::CommandBuffer& cmd);

        // The preview image for ImGui::Image in the panel.
        [[nodiscard]] ImTextureID GetTextureId() const;   // off the owned Ref<ImGuiTexture>

        void Resize(Veng::uvec2 extent);     // debounced by the caller
    };
}
```

The class lives in `namespace VengEditor`; `Veng::ImGuiLayer` (from `Veng/ImGui/ImGuiLayer.h`)
returns a `Ref<ImGuiTexture>` from `CreateTexture(const Sampler&, const ImageView&)`, so
`MaterialPreview` **owns a `Ref<ImGuiTexture>`** and `GetTextureId()` returns its
`GetTextureId()` — mirroring `TextureEditorPanel`.

It owns:

- A `Scene`: a `Primitives::Sphere` mesh (built once via `Mesh::Create`) under a `Transform`
  + `Camera`, and a **separate** directional-`Light` entity — a small lit stage. The sphere's
  `MeshRenderer` material is swapped by `SetMaterial`.
- A `SceneRenderer` (`Create` at a **small fixed preview extent — 256×256**; the deferred
  pipeline runs per frame, so the preview is sized to bound that second pass's cost), `Execute`d
  each frame against the scene/camera, its `GetOutput()` registered into bindless + wrapped as a
  `Ref<ImGuiTexture>` via `ImGuiLayer::CreateTexture`.
- The cross-graph handoff the renderer documents: `PrepareForAccess(Sample)` before the ImGui
  read; the renderer re-arms `ColorAttachment` before the next `Execute`.

This is the `SceneViewportPanel` pattern, scoped to a single material on a sphere — the
"one preview generalized" the editor design calls for.

### Two live SceneRenderers

The editor already runs the scene viewport's `SceneRenderer`; the preview adds a second. Both
record into the host's single `OnRender` command buffer **in submission order on the single
graphics queue**, and each brackets **its own** output (`Sample` ↔ `ColorAttachment`) over
**disjoint** targets — so the documented frames-in-flight handoff composes without a semaphore
(the contract is per-renderer, and the two renderers never touch each other's images). The
preview's 256² extent keeps the second deferred pipeline cheap. The `validation` gate test
(below) renders **both** renderers live so a two-renderer barrier interaction can't slip past
an isolated-preview test.

## Material swap on hot-reload

`SetMaterial` replaces the sphere's material and is called by the panel whenever a recook's
`Load<Material>` completes behind the stable handle. `Resize`/`Configure` on the
`SceneRenderer` invalidates `GetOutput()`, so the preview **re-fetches and re-registers** the
bindless `TextureHandle` + `ImGuiTexture` after either — the documented `GetOutput`
contract. The old `ImGuiTexture` retires through the per-frame deferred-destruction path.

## Slot in the host frame

The panel drives `MaterialPreview::Render` from the host's `OnRender` (before the ImGui frame
is built), exactly as the scene viewport's `SceneRenderer` is driven today. Plan 04 wires a
temporary harness (preview the cooked `brick.vmat`) to verify in isolation; plan 05 connects
it to the live graph compile.

## Tests

- **GPU (`gpu`-labelled, skips with no ICD):** construct a `MaterialPreview`, `SetMaterial`
  the cooked brick material, `Render` a frame, assert `GetTextureId()` is non-null and the
  output image is the expected 256² extent. A `Resize` re-fetches a fresh, valid texture id.
- Verify no validation errors under the `validation` gate **with a second `SceneRenderer`
  active** (a viewport-style renderer alongside the preview), so the two-renderer handoff is
  exercised, not just an isolated preview pass.
- `ctest` green; smoke PPM unchanged; `include_hygiene` green.

## Acceptance

`MaterialPreview` builds and renders the cooked brick material on a sphere into a
`Ref<ImGuiTexture>`; `SetMaterial` swaps cleanly; `Resize` re-fetches the output; the GPU test
passes and skips with no ICD; the validation gate is clean with two live renderers; smoke PPM
unchanged. Commit:
`Plan 04: MaterialPreview — sphere + SceneRenderer preview RT, material swap on reload`.
</content>
