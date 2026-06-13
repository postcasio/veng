# 09 — ImGui as an optional module

**Goal:** extract all ImGui ownership from `Context` into an `ImGuiLayer` the
application opts into. Headless/non-UI consumers stop paying for ImGui, and
`Veng/Vendor/ImGui.h` stops force-feeding `imgui_internal.h`/
`imnodes_internal.h` to everyone.

**Dependencies:** 02 (teardown hooks exist). Plan 10 (headless) requires this.
Can run before or after 06/07/08 — coordinate the `ImGuiTexture` raw
`VkDescriptorSet` question with plan 07's `Native.h`.

## Current state

`Context` unconditionally owns: `m_ImGuiDescriptorPool`, `m_ImGuiRenderPass`,
`m_ImGuiFramebuffers`, `m_ImGuiImage`, `m_ImGuiImageView`
(`Context.h:124-128`), `m_ImGuiRenderedThisFrame` (`Context.h:101`),
`CreateImGuiResources`/`DisposeImGuiResources` (`Context.h:143-144`),
`RenderImGui`, `GetImGuiImage`, `CreateImGUITexture`/`DestroyImGUITexture`
(`Context.h:83,88,90-91`). Fonts ride in `ContextInfo`
(`DefaultFontPath`/`IconFontPath`, `Context.h:34-35`). `Context.h` includes
`Veng/Vendor/ImGui.h` at the top (`Context.h:3`), pushing imgui headers into
every consumer TU. ImGui/imnodes sources compile into veng via
`src/Vendor/ImGui.cpp`, and internal headers install for all consumers
(`CMakeLists.txt:208-218`).

## Design

### `ImGuiLayer`

```cpp
// Veng/ImGui/ImGuiLayer.h
struct ImGuiLayerInfo {
    optional<path> DefaultFontPath;
    optional<path> IconFontPath;
    // theme/style hook later
};

class ImGuiLayer {
public:
    static Unique<ImGuiLayer> Create(const ImGuiLayerInfo&, Context&, Window&);
    ~ImGuiLayer();                       // shutdown: backend, context, pools
    void BeginFrame();                   // NewFrame trio
    void Render(CommandBuffer& cmd);     // render pass + draw data
    Ref<Image> GetOutputImage() const;   // was Context::GetImGuiImage
    Ref<ImGuiTexture> CreateTexture(const Sampler&, const ImageView&);
};
```

- All `m_ImGui*` members, `CreateImGuiResources`/`DisposeImGuiResources`,
  `RenderImGui`, `ImGuiTexture` create/destroy move from `Context`/
  `Context.cpp` into `src/ImGui/ImGuiLayer.cpp`. `ImGuiTexture`
  (post-plan-01 name) moves to `Veng/ImGui/` too.
- `m_ImGuiRenderedThisFrame` and the swapchain-recreation interaction: the
  layer subscribes via `SwapChain::AddInvalidationCallback` (plan 01) instead
  of `Context` special-casing it.
- Font paths leave `ContextInfo` (finishing what plan 02 started). They live in
  `ImGuiLayerInfo`.

### Application opt-in

`ApplicationInfo` gains `optional<ImGuiLayerInfo> ImGui` (engaged = enabled;
the sample app sets it, default stays engaged-with-defaults for now to avoid
surprising existing consumers — flip the default to disengaged once the
sample app has migrated). `Application` owns `Unique<ImGuiLayer> m_ImGuiLayer`,
calls `BeginFrame` in `Frame()` and exposes `GetImGuiLayer()`.
`Context::BeginFrame` loses its ImGui half.

### Header hygiene

- `Context.h` drops the `Veng/Vendor/ImGui.h` include.
- `Veng/Vendor/ImGui.h` splits: it includes `<imgui.h>`/`<imnodes.h>` only;
  a new `Veng/Vendor/ImGuiInternal.h` carries `imgui_internal.h`/
  `imnodes_internal.h` for the consumers that genuinely want internals.
- CMake: ImGui sources/includes stay in veng (they're exported symbols per
  `CMakeLists.txt:9-13`), but consider a `VE_IMGUI` option later if a truly
  ImGui-free build matters; not required for this plan — the *runtime* opt-out
  plus header hygiene is the deliverable.

## Acceptance

- `Context.h`/`Context.cpp` contain no ImGui code or includes; grep `ImGui`
  in `src/Renderer/` → nothing (or only the layer's own backend file if it
  lives there).
- An app with `ImGui = nullopt` runs and renders without any ImGui
  pool/pass/image being created.
- A consumer TU including `Veng/Vendor/ImGui.h` no longer sees
  `imgui_internal.h` symbols unless it includes `ImGuiInternal.h`.
