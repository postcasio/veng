# Plan 03 — `ImGuiCompositePass`

**Goal:** add an engine-provided **`ImGuiCompositePass`** that owns the scene-output → ImGui
plumbing every `SceneRenderer` app hand-writes today — the ImGui scene texture, the pre-ImGui
sampleability barrier, and (for the full-screen case) the scene-behind-ImGui composite — and
migrate its three consumers onto it. Realizes [future area 11](../future/README.md#11-imguicompositepass)
([imgui-composite-pass.md](../future/imgui-composite-pass.md)).

## Why

`SceneRenderer` writes to an owned offscreen target. Surfacing that target through ImGui has a
**universal** part and a **full-screen-composite** part, and today every consumer re-implements
both by hand.

**The universal part — show the scene output through ImGui (every consumer):** an
`ImGuiLayer::CreateTexture(sampler, GetOutput())` for `ImGui::Image`, **plus** a
`cmd.PrepareForAccess(GetOutput(), Sample)` barrier issued **before** `ImGuiLayer::Render` records
that sampled read (ImGui samples outside the render graph, so no `.Sample()` declaration covers
it). Both must be **redone after `SceneRenderer::Resize`/`Configure`**, which retire and recreate
the output image — the `GetOutput()`-invalidation contract — so the texture re-fetch + barrier are
a recurring, easy-to-desync footgun. The editor's [`SceneViewportPanel`](../../editor/src/panels/SceneViewportPanel.cpp)
(`RegisterOutput()` + the `PrepareForAccess(Sample)` at the top of `Render`) and
[`MaterialPreview`](../../editor/src/material/MaterialPreview.cpp) (same pattern) each hand-write
exactly this — the scene goes **inside** an ImGui panel via `ImGui::Image`; there is no
scene→swapchain composite in the editor.

**The full-screen-composite part (hello-triangle only):**
[hello-triangle's main.cpp](../../examples/hello-triangle/main.cpp) additionally composites the
scene **to the swapchain behind the ImGui overlay** — a fullscreen `RenderGraph` pass
(`BuildCompositeGraph`) whose fragment shader samples the scene output and the ImGui-layer output
via **three bindless registrations** (scene, ImGui output, sampler) and blends them
(`CreateCompositePipeline`/`RegisterSceneOutput`). That pass is re-`Compile()`d on **swapchain
resize** — a trigger independent of the source-swap above.

**Note: the editor's `EditorHost::BuildPresentGraph()` is *not* this.** It blits only the
ImGui-layer output to the swapchain (one import, the engine's existing `hdr_blit.frag`, no scene
sample, no `PrepareForAccess`). It is a plain present blit, a different pattern, and is **out of
scope** — `ImGuiCompositePass` does not replace it.

## Decisions (from the design doc)

1. **A `Unique`, engine-provided helper.** `ImGuiCompositePass::Create(const
   ImGuiCompositePassInfo&)` (factory, private ctor, `Unique` single-owner). It always owns the
   ImGui scene texture (+ its sampler); in **composite mode** it additionally owns the fullscreen
   pipeline (no vertex layout — a fullscreen triangle), blend state, pipeline layout, the composite
   fragment shader, and the three bindless registrations.

   ```cpp
   struct ImGuiCompositePassInfo
   {
       Renderer::Context&        Context;
       ImGuiLayer&               ImGui;
       Ref<Renderer::ImageView>  SceneSource;                 // initial SceneRenderer::GetOutput()
       optional<Renderer::Format> SwapChainFormat = nullopt;  // set → composite mode; nullopt → panel-only
       Renderer::SamplerFilter   Filter = Renderer::SamplerFilter::Linear;
       Renderer::SamplerWrap     Wrap   = Renderer::SamplerWrap::ClampToEdge;
   };
   ```

2. **Two modes, selected by `SwapChainFormat`.** A consumer either composites the scene to the
   swapchain or shows it inside an ImGui panel — never both shapes from one consumer.
   - **Composite mode** (`SwapChainFormat` set → hello-triangle): builds the fullscreen pipeline
     for that color format and the three bindless slots; `Compile` is valid.
   - **Panel-only mode** (`SwapChainFormat == nullopt` → editor `SceneViewportPanel`,
     `MaterialPreview`): owns only the ImGui scene texture + the barrier; no pipeline, no bindless
     slots. `Compile` asserts (misuse — there is no swapchain composite in this mode).
3. **`SetSource(Ref<ImageView>)`** is the **single** re-binding call when the scene output changes
   (after `SceneRenderer::Resize`/`Configure`). It re-creates the ImGui scene texture in both modes
   and, in composite mode, also re-registers the scene bindless slot — one owner, one call site, no
   split. **No recompile:** the composite reads `SceneTextureHandle.Index` live per frame via push
   constants, so a source swap takes effect on the next replay.
4. **`Compile(RenderGraph&, swapchainTarget) → Unique<CompiledGraph>`** (composite mode only) is
   the **recompile** seam on swapchain resize — adds the fullscreen composite pass to the app's
   graph. Independent of `SetSource`.
5. **`PrepareSceneForImGui(cmd)`** issues the explicit `PrepareForAccess(Sample)` barrier that must
   precede `ImGuiLayer::Render`, in **both** modes — making the ordering requirement a named call
   impossible to forget, instead of a comment in every app/panel.
6. **`GetSceneTexture()`** returns the stable ImGui texture handle for the *scene* output (what a
   viewport panel passes to `ImGui::Image`), in both modes — not the composited result, not the
   ImGui layer's own output. The panel stays app/editor UI; the engine does not own it.
7. **Shaders: only the composite fragment shader is new.** The composite **vertex** shader is the
   fullscreen triangle — byte-identical to the engine core pack's existing `fullscreen.vert`
   (`AssetId{0xF46DD3C6F2AE0628ULL}`, already loaded by `SceneRenderer` and `EditorHost`), so the
   pass **reuses that id**; no new VS file or pack entry. Only `composite.frag.slang` is added to
   `engine/assets/core/shaders/` under a **new engine-owned `AssetId`** (minted with `vengc
   generate-id`, mirrored as a `0x…ULL` constant) and a core-pack manifest entry. It cannot live in
   a game pack — `ImGuiCompositePass` is an engine type. hello-triangle's `composite.vert/.frag.slang`
   (+ both `.shader.json`) are **deleted** from `examples/hello-triangle/assets/shaders/` and its
   two pack manifest entries removed (the VS was a duplicate of the engine's).
8. **Sampler defaults to `ClampToEdge` + linear filter** (wrap would sample garbage at the blit
   edges); both overridable via `ImGuiCompositePassInfo`.
9. **Windowed-only.** `ImGuiCompositePass` is not built on the headless/smoke path — `GetImGuiLayer()`
   null and no swapchain are the guards. The smoke render downloads `SceneRenderer::GetOutput()`
   directly (unchanged); only the windowed composite/panel path is replaced.

## Considered, not chosen (recorded from the doc)

`SceneRenderer::GetOutputHandle()` returning a silently-updated `TextureHandle` (surprising
ownership — a handle looks like a value, not a live reference); moving the composite into the
engine's render loop (takes too much frame control from the app); a per-frame
`SetSceneOutput(view)` (implies per-frame meaning for a value that only changes on
`Resize`/`Configure`); a general-purpose multi-source compositor (out of scope — this is
specifically scene-behind-ImGui, an editor/tooling concern).

## Scope

| In scope | Out of scope |
|---|---|
| `ImGuiCompositePass` + `ImGuiCompositePassInfo` (public header + backend impl, Native idiom), composite + panel-only modes | A general-purpose / multi-source compositor |
| New `composite.frag` in the core pack under an engine-owned `AssetId`; VS reuses the existing `fullscreen.vert` id | Any `SceneRenderer` API change (it already hands back `GetOutput()`) |
| `Create` / `SetSource` / `Compile` / `PrepareSceneForImGui` / `GetSceneTexture` surface | Headless/smoke path (windowed-only; unchanged) |
| Migrate **hello-triangle** (composite mode): delete its composite pipeline/graph/registration + barrier code, its composite shaders, and both pack entries | `EditorHost::BuildPresentGraph()` (ImGui-only swapchain blit — a different pattern, untouched) |
| Migrate the editor's **`SceneViewportPanel`** and **`MaterialPreview`** (panel-only mode): replace their hand-written `CreateTexture` + `PrepareForAccess(Sample)` + resize re-fetch with the helper | Multi-viewport OS windows (still future); new `SceneRenderer` batteries / lights |
| `include_hygiene`: the new public header stays backend-free | — |
| Verify: clean build, `ctest` green, smoke PPM correct size + exit 0, windowed app + editor render correctly | — |

## Verification

`cmake -B build -S . && cmake --build build -j 2` clean; `ctest --test-dir build
--output-on-failure` green; the smoke launcher writes a correct-sized PPM and exits 0 (the headless
path never builds the composite — the golden capture is unaffected, since it downloads
`GetOutput()` directly). Run the windowed `hello_triangle-launcher` (composite mode) and confirm
the scene composites behind ImGui and resizes cleanly (both the swapchain-resize recompile and the
`Resize`/`Configure` source-swap path). Run the windowed `hello_triangle-editor` and confirm the
scene viewport panel **and** the material-editor preview both display correctly and survive a panel
resize / a `Configure` (the `DebugView` combo) — the panel-only path. Under `build-debug`, the
`validation` gate stays clean — the explicit `PrepareSceneForImGui` barrier must satisfy the same
layout contract the hand-written barriers did in all three consumers.

## Acceptance

`ImGuiCompositePass` ships in `libveng` with composite + panel-only modes; hello-triangle (composite
mode) and the editor's `SceneViewportPanel` + `MaterialPreview` (panel-only mode) all consume it and
**no longer hand-write** the ImGui scene texture, the pre-ImGui `PrepareForAccess(Sample)` barrier,
the resize re-fetch, or (hello-triangle) the composite pipeline/graph/bindless registration; the new
`composite.frag` lives in the core pack under an engine-owned id and the composite VS reuses the
existing `fullscreen.vert`; `EditorHost::BuildPresentGraph()` is untouched; `include_hygiene` builds;
`ctest` green; the windowed app and editor render correctly. Commit:
`Plan 03: ImGuiCompositePass — engine scene→ImGui helper; hello-triangle + editor panels migrated`.

## Note on planset fit

planset-16 is a **grab bag** of independent small wins; this is win 3. It shares no files with the
reflection refactor (win 1, plans 00/01) or the asset-library rename (win 2, plan 02), touching the
renderer (`engine/include/Veng/Renderer/`), the core pack, and its three consumers (hello-triangle
plus the editor's `SceneViewportPanel` and `MaterialPreview`). It can land in any order.
