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
        void Render(Veng::Renderer::CommandBuffer& cmd, Veng::f32 delta);

        // The preview image as an ImGui texture id, for ImGui::Image in the panel.
        [[nodiscard]] ImTextureID TextureId() const;

        void Resize(Veng::uvec2 extent);     // debounced by the caller
    };
}
```

It owns:

- A one-entity `Scene`: a `Primitives::Sphere` mesh (built once via `Mesh::Create`) under a
  `Transform`, a `Camera`, and a `Light` — a small lit stage. The sphere's `MeshRenderer`
  material is swapped by `SetMaterial`.
- A `SceneRenderer` (`Create` at the panel's preview extent), `Execute`d each frame against
  the scene/camera, its `GetOutput()` registered into bindless + wrapped as an `ImGuiTexture`
  via `ImGuiLayer::CreateTexture`.
- The cross-graph handoff the renderer documents: `PrepareForAccess(Sample)` before the ImGui
  read; the renderer re-arms `ColorAttachment` before the next `Execute`.

This is the `SceneViewportPanel` pattern, scoped to a single material on a sphere — the
"one preview generalized" the editor design calls for.

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
  the cooked brick material, `Render` a frame, assert `TextureId()` is non-null and the
  output image is the expected extent. A `Resize` re-fetches a fresh, valid texture id.
- Verify no validation errors under the `validation` gate (it renders a real `SceneRenderer`
  pass).
- `ctest` green; smoke PPM unchanged; `include_hygiene` green.

## Acceptance

`MaterialPreview` builds and renders the cooked brick material on a sphere into an ImGui
texture; `SetMaterial` swaps cleanly; `Resize` re-fetches the output; the GPU test passes and
skips with no ICD; validation gate clean; smoke PPM unchanged. Commit:
`Plan 04: MaterialPreview — sphere + SceneRenderer preview RT, material swap on reload`.
</content>
