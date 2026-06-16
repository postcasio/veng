# Plan 03 — the directional `Light` component + the deferred lighting pass

**Goal:** make the deferred chain actually *light*. Add a directional **`Light`**
builtin component to the scene model, select it into the per-frame `SceneView`, and
replace plan 02's `AlbedoBlitScenePass` with a real **deferred lighting pass**
(g-buffer + light → HDR), followed by an explicit `HdrBlitScenePass` (HDR → output) as
the stand-in tail plan 04 swaps for tonemap. After this plan the renderer shades a
scene with a directional light through the deferred path.

## Why this is its own plan, and on the main thread

Two contracts: the **`Light` builtin** (a new reflected component registered through
the same `RegisterBuiltinTypes` path as every builtin, so it serializes, cooks into
prefabs, and is editor-inspectable for free) and the **lighting model + HDR target**
(what the lighting pass computes and where it writes). The `Light`-into-`SceneView`
selection rule (which light, what default when none) is the per-frame contract pass 04
and future multi-light work build on.

## The `Light` builtin — `engine/include/Veng/Scene/Components.h` + `BuiltinTypes`

```cpp
// A directional light. Direction is the direction the light travels (world space);
// Color is linear RGB; Intensity scales it. v1 supports a single directional light.
struct Light
{
    vec3 Direction{0.0f, -1.0f, 0.0f};
    vec3 Color{1.0f, 1.0f, 1.0f};
    f32  Intensity{1.0f};
};
```

```cpp
VE_REFLECT(::Veng::Light, 0x________________ULL)   // minted with `vengc generate-type-id` (planset-11)
    VE_FIELD(Direction, .DisplayName = "Direction")
    VE_FIELD(Color,     .DisplayName = "Color")
    VE_FIELD(Intensity, .DisplayName = "Intensity", .Min = 0.0)
VE_REFLECT_END();
```

- **Registered by `RegisterBuiltinTypes`** alongside
  `Name`/`Transform`/`Parent`/`CameraComponent`/`MeshRenderer` — GPU-free contract
  unchanged, so the cooker reflects it and the headless no-device test still passes.
  Same `VE_REFLECT`/`VE_FIELD`/`VE_REFLECT_END` form as the existing builtins.
- The `TypeId` is a **placeholder while implementing**, minted real with `vengc
  generate-type-id` (the `TypeId` minter planset-11 shipped) once the build is green
  (per the working-norms rule), written uppercase-hex in C++.

## `SceneView` light selection — decision 9

`SceneView` gains the directional light **by value** (the renderer computes it per
frame; it is not borrowed like `World`/`Camera`):

```cpp
struct SceneView
{
    const Scene&  World;
    const Camera& Camera;
    Light         Light;        // ← the selected directional light (a per-frame VALUE)
    f32           Delta = 0.0f;
};
```

`SceneRenderer::Execute` selects it: the **first** `Light` entity (the first iterate of
`World.View<Light>()`, then `break`), copied by value. When the scene has **none**, the
documented default is a **zero-intensity directional light** — so the lighting pass's
`N·L` term contributes nothing and the scene renders **flat-ambient** (the small
ambient term below), never pure black, never asserting. Selection is per-frame data →
rides the `SceneView` → never recompiles (decision 2).

## The lighting pass — `DeferredLightingScenePass`

Replaces plan 02's `AlbedoBlitScenePass` in the same wiring slot (a pass-set swap, not
a deeper topology change). ("Lighting pass," not "resolve" — `Resolved`/"resolve" is
the `RenderGraph` view-resolution verb; see the README terminology note.)

- **Inputs (via `PassIO`/`ScenePassContext`):** G0 (albedo), G1 (normal), Depth —
  sampled through their bindless `TextureHandle`s (the renderer registered each owned
  g-buffer image once and threads the handles in); plus the camera + light from
  `ctx.View()`. The pass declares `.Sample(g0)`, `.Sample(g1)`, `.Sample(depth)` so the
  graph derives the barriers.
- **Output:** a renderer-owned **imported HDR image** (`RGBA16Sfloat`,
  `ColorAttachment | Sampled`, allocated in `SceneRenderer` like the g-buffer,
  registered into bindless once) — not the final output, because tonemap (plan 04) maps
  HDR → output. Confirm `RGBA16Sfloat` is supported as **both** a color attachment and
  a sampled image on MoltenVK before depending on it (it is the same format G1 already
  uses as a sampled color target, so this is established here).
- **The temporary tail — `HdrBlitScenePass`:** this plan adds an explicit fullscreen
  pass that samples the HDR image and blits it (clamped) to the imported output, so the
  chain is renderable now. Wiring it as its own pass means plan 04 is a clean
  **single-pass swap** (`HdrBlitScenePass` → `TonemapScenePass`) mirroring 02→03 — the
  lighting pass's output target (HDR) stays stable across 03 and 04.
- **Math:** a fullscreen pass reconstructing world position from depth + the inverse
  view-projection (or sampling the world-normal directly from G1), applying a single
  **directional** term (Lambert `N·L` × color × intensity) plus a **small documented
  ambient** so unlit/back faces — and the no-light default — aren't pure black. The
  model is deliberately minimal — Blinn-Phong/specular, multiple lights, and shadows
  are later batteries.

**Depth-as-texture (verify):** the g-buffer depth is a `.Depth` attachment in the
geometry pass and a `.Sample` input here — the first depth read-as-texture in the
engine. This plan confirms the graph derives the depth-attachment → shader-read
transition (validation gate green); if it does not, that derivation in the backend
barrier path is a prerequisite to land first.

## Sample migration

- hello-triangle adds a `Light` to its scene: one entity (or the existing entity)
  carrying a directional `Light` at a fixed direction so the smoke pose is lit
  reproducibly. The `Each`/draw path is unchanged — the light flows through
  `SceneView`, not the draw.
- No composite/ImGui change.

## Tests

- **Unit (`-L unit`):** a `Light` serializer round-trip (`WriteFields`/`ReadFields`),
  proving the new builtin rides the reflection layer like every other component.
- **Headless reflection (`-L cooker`/no-device):** `RegisterBuiltinTypes` now exposes
  `Light` with its fields/`TypeId` — the existing no-device contract test covers it
  (extend its assertions).
- **GPU (`veng_gpu`):** `Execute` a scene with a known albedo + a known directional
  light and assert **a lit face matches the expected `N·L` shading** and **a back/
  shadowed face matches the ambient term** (both within tolerance), plus the whole-frame
  luminance invariant — the automated oracle (decision 10). Separately, `Execute` a
  scene with **no** `Light` and assert it renders the **flat-ambient default** (a
  non-black ambient texel), never pure black, never asserting.
- **`smoke_golden`:** **regenerated** — the capture is now lit. Regenerate per
  CLAUDE.md; the GPU assertions above are the real gate.
- **Validation gate:** green, allowlist empty (incl. the depth-as-texture transition).

## Acceptance

Clean build; `ctest` green incl. the `Light` round-trip and the no-device reflection
assertions; the scene shades under a directional light through the deferred lighting
pass; the no-light default renders flat-ambient; `smoke_golden` green against the
**regenerated** lit golden; smoke PPM correct size + exit 0; validation gate green,
allowlist empty; the real `Light` `TypeId` minted with `generate-type-id`. Commit:
`Plan 03: directional Light builtin + deferred lighting pass (g-buffer + light → HDR)`.
