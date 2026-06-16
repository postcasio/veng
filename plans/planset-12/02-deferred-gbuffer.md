# Plan 02 — the deferred g-buffer pass + the material g-buffer contract

**Goal:** replace plan 01's single forward pass with a **deferred g-buffer geometry
pass** — material shaders write albedo + world-space normal into a multi-render-target
(MRT) g-buffer the `SceneRenderer` owns — and establish the **deferred opaque material
fragment-shader contract** (decision 8). hello-triangle's brick material shader is
updated to honor it. To keep the renderer renderable in isolation (one plan per
session, each verifiable), this plan ends with an **`AlbedoBlitScenePass`**: output =
g-buffer albedo, unlit. The real directional-light pass is plan 03.

## Why this is its own plan, and on the main thread

The reviewable surface is the **g-buffer layout contract** — what channels exist,
their formats, which attachment is which, and what a material fragment shader must
write. Every opaque material in the engine eventually agrees on it, so it is fixed here
once, documented as a present-tense fact beside the geometry pass and the material
pipeline, and proven by the brick shader honoring it. The MRT pipeline plumbing (N
color blend states, N attachment formats) rides that contract.

## The g-buffer layout (the contract)

A fixed set of **renderer-owned images** (decision 6 — not graph transients: they are
sampled downstream by the lighting pass, which needs a `Ref<ImageView>` to register
into bindless, so the renderer owns them and `Import`s them into the internal graph),
sized to the renderer's extent, recreated on `Resize` through the retire path, and
registered into bindless once at `Create`:

| Target | Format | `ImageUsage` | Contents |
|---|---|---|---|
| G0 — Albedo | `RGBA8Srgb` | `ColorAttachment \| Sampled` | base color (rgb, sRGB-encoded); a = free/material flag |
| G1 — Normal | `RGBA16Sfloat` | `ColorAttachment \| Sampled` | world-space normal (xyz), encoded |
| Depth | `D32Sfloat` | `DepthAttachment \| Sampled` | depth attachment, **also** the lighting pass's depth source |
| HDR (plan 03) | `RGBA16Sfloat` | `ColorAttachment \| Sampled` | the lighting pass's output (allocated in plan 03) |

These are the **chosen, final** v1 formats (the contract is fixed, not a menu); all
four exist in `Renderer::Format`. The usage flags are load-bearing: `RenderGraph::
Compile` asserts on a sampled target missing `Sampled`, so G0/G1/HDR are
`ColorAttachment | Sampled` and **Depth is `DepthAttachment | Sampled`** — depth is the
first image read as a texture in the engine (see the depth-as-texture note in plan 03
and the README).

- **Albedo is stored sRGB-encoded and sampled as linear.** `RGBA8Srgb` means the
  sampler decodes to linear on read, so the lighting pass (plan 03) does its `N·L`
  math in linear space correctly. The brick shader writes its sampled albedo straight
  to `Albedo` (the target's sRGB encode happens on store); the lighting pass's sample
  returns linear. This round-trip is the documented color-space contract.
- **The v1 g-buffer is the minimum set, designed to grow (decision 8).** PBR shading
  later needs roughness/metallic/AO — a future **G2** target. The contract is
  documented as "the v1 channels," and the material write goes through one
  `GBufferOutput` struct so adding G2 is a one-place engine change, not a per-material
  edit.

The geometry pass declares G0 + G1 as `.Color` (Clear → Store) and Depth as `.Depth`
(Clear → Store — the lighting pass reads it). Material graphics pipelines are built
with **two color attachments** matching G0/G1's formats (attachment formats flow from
the render target, per planset-2); each gets its own `BlendState::Opaque()` (no blend).

## The deferred opaque material contract (decision 8)

An **opaque** material's **fragment shader** stops outputting final swapchain color and
instead writes the g-buffer through one engine-provided struct:

```hlsl
// The v1 deferred g-buffer. The minimum channel set; a future G2 (roughness/
// metallic/AO) extends this struct in one place. This is the OPAQUE material
// contract — a future transparent/forward material outputs final color instead,
// a separate entry, not a change to this one.
struct GBufferOutput
{
    float4 Albedo : SV_Target0;   // base color (sRGB target; sampler decodes to linear)
    float4 Normal : SV_Target1;   // encoded world-space normal
};
```

- The set-0 **bindless** path, `MaterialData` SSBO, and texture handles are
  **unchanged** — the shader still samples its textures and reads its `MaterialData`;
  only its **outputs** move from one color target to the g-buffer MRT.
- The contract is stated as a fact in a header/comment beside the geometry pass and in
  the material/shader docs — channels, formats, semantics, the sRGB/linear convention,
  and "this is the opaque contract; the minimum set" — so a material author knows
  exactly what to write. (No plan citation in the comment — CLAUDE.md.)
- hello-triangle's `brick` material fragment shader (`*.slang`) is updated to emit
  `GBufferOutput`: sample the albedo texture → `Albedo`; transform the interpolated
  normal to world space and encode → `Normal`. The smoke pose is unchanged.

## The pass units

- **`GBufferScenePass`** (replaces `ForwardScenePass`): same `Each<Transform,
  MeshRenderer>` draw, but its `Declare` targets G0+G1+Depth (the renderer's imported
  g-buffer images, threaded in via `PassIO`) and its material pipelines draw into the
  MRT. Geometry/draw logic is otherwise plan 01's.
- **`AlbedoBlitScenePass` (this plan):** a fullscreen pass sampling **G0 (albedo)**
  through its bindless `TextureHandle` (the renderer registered G0 once and threads the
  handle in via `PassIO`) and writing the renderer's **imported output** — an unlit
  blit. It declares `.Sample(g0)` so the graph derives the color → shader-read barrier,
  and `.Color(output)`. It exists so the chain produces a coherent image now; plan 03
  swaps it for the lighting pass (g-buffer + light → HDR). Wiring it as a distinct pass
  here means plan 03 is a pass-set swap in the same slot, not a deeper topology change.

`SceneRenderer` wires `[GBufferScenePass, AlbedoBlitScenePass]` and threads the
g-buffer ids + the G0 bindless `TextureHandle` from the geometry pass's outputs into
the blit pass's `PassIO` — the renderer owns this wiring (decision 4/5).

## Sample migration

hello-triangle gains nothing structural — it already renders through
`SceneRenderer::Execute` (plan 01). The only sample-side change is the **brick
material shader** honoring the g-buffer contract (and its `*.shader.json`/material if
the fragment entry or outputs are declared there). The composite + ImGui are
untouched.

## Tests

- **GPU (`veng_gpu`):** assert the g-buffer images allocate at the renderer's extent
  with the contracted formats/usage; `Execute` one frame and assert the output is the
  albedo blit via **several spread sample points** (a known cube-albedo texel, a
  background texel) **plus a whole-frame invariant** (mean luminance in the expected
  unlit-albedo range) — the automated oracle (decision 10). Resize still updates all
  g-buffer images + output and re-registers their bindless handles.
- **`smoke_golden`:** **regenerated** — the capture is now unlit deferred albedo
  (decision 10). Regenerate `hello_triangle_scene.png` per the CLAUDE.md procedure; for
  plans 02–04 the golden re-asserts reproducibility while the GPU assertions above are
  the real correctness gate.
- **Validation gate:** MRT + the new fullscreen blit must pass `-L validation` with the
  allowlist still **empty** (no new validation errors from the multi-attachment
  pipelines or the blit's sampled reads). Confirm the depth → shader-read transition is
  derived clean here even though depth is sampled only in plan 03 (it is `Store`d here).
- **`include_hygiene`:** any new public header (if the g-buffer layout is exposed)
  stays backend-free and is added.

## Acceptance

Clean build; `ctest` green; the brick material renders into the g-buffer and the
albedo blit reaches the output; the GPU oracle (multi-point + luminance) passes;
`smoke_golden` green against the **regenerated** golden; smoke PPM correct size + exit
0; validation gate green, allowlist empty. Commit: `Plan 02: deferred g-buffer pass +
opaque material g-buffer contract; brick shader writes albedo+normal (albedo blit)`.
