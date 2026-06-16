# ImGuiCompositePass — design overview (future)

> **Vision / design sketch, not scheduled.** Addresses the boilerplate every
> `SceneRenderer`-based app must write today: a fullscreen composite that blits
> the renderer's offscreen output to the swapchain behind the ImGui layer. The
> conclusion from design exploration: keep compositing in the app, but provide a
> reusable **`ImGuiCompositePass`** in the engine so apps don't hand-write that
> code.

## The problem

`SceneRenderer` writes to an owned offscreen target. Getting that image onto the
screen requires:

1. A `RenderGraph` pass that samples the scene output and the ImGui layer's
   rendered output and writes both to the swapchain (the composite). The
   composite shader takes three bindless indices: scene texture, ImGui texture,
   and shared sampler.
2. Bindless registrations for all three — the scene `TextureHandle`, the ImGui
   layer's output `TextureHandle`, and the `SamplerHandle`.
3. An ImGui texture registration (`ImGuiLayer::CreateTexture`) so a viewport
   panel can call `ImGui::Image` to display the scene output.
4. A `PrepareForAccess(Sample)` barrier on the scene output **before**
   `ImGuiLayer::Render` — the ImGui layer's render is an out-of-graph sampled
   read of the scene texture, and the barrier must be issued explicitly. This is
   the non-obvious footgun: it is not covered by the composite pass's own
   `.Sample()` declaration, which runs after.

Items 2–4 must be **re-done** whenever `SceneRenderer::Resize` or `Configure`
recreates the output image. Item 1 must be **re-compiled** on swapchain resize,
which is a separate and independent event. Today every app writes and maintains
all of this itself.

## Two independent recompile triggers

The app has two distinct reasons to rebuild the composite, and they are not the
same event:

- **Scene output recreated** (`SceneRenderer::Resize`/`Configure`): re-register
  bindless slots and ImGui texture. No graph recompile needed — the composite
  reads `SceneTextureHandle.Index` live per frame via push constants.
- **Swapchain resized**: re-`Compile()` the composite graph against the new
  extent. The scene source is unchanged; no re-registration needed.

`ImGuiCompositePass` must keep these two paths distinct.

## What was considered and rejected

**`SceneRenderer` managing its own bindless slot** (`GetOutputHandle()` returning
a stable `TextureHandle` the renderer silently updates): surprising ownership —
a `TextureHandle` looks like a value you own, not a live reference; the split
where bindless is transparent but the ImGui texture still needs a callback is
an inconsistency the caller has to understand.

**Moving the composite pass into the engine's rendering loop** (engine calls
`SceneRenderer::Execute`, owns the swapchain composite): removes too much control
from the app. The app should still execute `SceneRenderer` itself and decide when
and how scene rendering happens.

**Per-frame `SetSceneOutput(imageView)`**: passing the same value every frame
when it only changes on `Resize`/`Configure` is noise; a per-frame setter implies
the value is meaningfully per-frame, which is misleading.

**General-purpose compositor**: a configurable multi-source blending compositor
is not what this is. The use case is specifically "render the scene output behind
the ImGui layer" — an editor/tooling concern, not a general GPU compositing
primitive.

## The conclusion: `ImGuiCompositePass`

A `Unique`, engine-provided helper (factory `ImGuiCompositePass::Create(const
ImGuiCompositePassInfo&)`) that owns everything the composite needs and wires a
`RenderGraph` pass for it. Named deliberately — if you are not using the ImGui
layer, you would not use this. Not available on the headless path
(`GetImGuiLayer()` is null, and there is no swapchain to composite into).

**Owns:**
- The fullscreen pipeline (no vertex buffer — draws a fullscreen triangle),
  blend state, and pipeline layout.
- The composite shaders, loaded from the **engine core pack** by `AssetId` —
  they cannot live in the game pack since `ImGuiCompositePass` is an engine type.
- Bindless registrations: scene `TextureHandle`, ImGui layer output
  `TextureHandle`, and `SamplerHandle`.
- The ImGui texture (`ImGuiLayer::CreateTexture`) for displaying the scene output
  in a viewport panel.

**App setup (once at init):**
```cpp
m_Composite = ImGuiCompositePass::Create(context, {
    .ImGuiLayer = GetImGuiLayer(),
    .SceneSource = m_SceneRenderer->GetOutput(),
});
```

**After `SceneRenderer::Resize` or `Configure`** (source image recreated —
re-register, no recompile):
```cpp
m_Composite->SetSource(m_SceneRenderer->GetOutput());
```

**After swapchain resize** (extent changed — recompile, no re-registration):
```cpp
m_CompositeGraph = m_Composite->Compile(graph, swapchainTarget);
```

**Per frame** (`OnRender`):
```cpp
// ImGui samples the scene texture outside the graph — barrier must precede Render.
m_Composite->PrepareSceneForImGui(cmd);
GetImGuiLayer()->Render(cmd);
m_CompositeGraph->Execute(cmd, imports);
```

**Panel:**
```cpp
ImGui::Image(m_Composite->GetSceneTexture(), size);
```

`SetSource` is the single re-registration call when the scene output changes:
one owner, one call site, no split between bindless and ImGui texture.
`PrepareSceneForImGui` issues the explicit `PrepareForAccess(Sample)` barrier
that must precede the ImGui render — it makes the ordering requirement visible
and impossible to forget.

`GetSceneTexture()` returns the stable ImGui texture handle for the scene output
— the thing a viewport panel passes to `ImGui::Image`. The panel is editor UI;
the engine does not own it.

The app still owns when compositing happens and drives the frame. `ImGuiCompositePass`
is a reusable building block, not a rendering loop takeover.

## Decisions

**Shaders:** the composite VS/FS must live in the engine core pack, addressed by
engine-owned `AssetId`s. They cannot be in the game pack.

**Vertex input:** none — the composite draws a fullscreen triangle with no vertex
buffer. The pipeline has no vertex layout.

**Sampler:** engine-defaulted to `ClampToEdge` (the critical parameter for a
fullscreen blit — wrap addressing would sample garbage at edges). Filter is
linear. Both overridable via `ImGuiCompositePassInfo` for non-standard setups.

**`GetSceneTexture()` vs. `GetImGuiTexture()`:** named `GetSceneTexture()` — it
returns the ImGui texture handle for the *scene* output for use in a viewport
panel, not the composited result or the ImGui layer's own output.

**Headless:** `ImGuiCompositePass` is windowed-only. The headless/smoke path
skips compositing entirely; `GetImGuiLayer()` being null is the guard.
