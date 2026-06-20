# Plan 02 — cascade math: frustum split + per-cascade fit

**Goal:** the analytic heart of CSM — a **pure, device-free** function that turns a camera,
a light direction, and the scene bound into per-cascade light-space view-projection matrices
and split distances. Fully unit-tested with no GPU; Plans 03–04 consume its output.

## What lands

### `Camera` exposes its clip range ([engine/include/Veng/Scene/Camera.h](../../engine/include/Veng/Scene/Camera.h))

`Camera` gains `m_Near` / `m_Far` members (it stores only `m_View`/`m_Projection` today) with
**safe default initializers** (`m_Near = 0.1f`, `m_Far = 100.0f`, matching `CameraComponent`'s
defaults), set in `SetPerspective`, and exposes `[[nodiscard]] f32 GetNear() const` /
`GetFar() const`. The cascade split needs the view's near/far in view space; recovering them from
the projection matrix is fiddly under the Y-flip, so the camera carries them. `ComputeCascades`
opens with `VE_ASSERT(far > near && near > 0.0f, ...)` so a degenerate range fails loudly. **The
assert cannot catch a *plausible-but-wrong* range**, though: a camera whose perspective was set
by some path other than `SetPerspective` keeps the default 0.1/100, which `ComputeCascades` then
trusts — the splits fit to the wrong range with no error. The engine's render cameras are
`SetPerspective`-built (the `CameraComponent` path sets near/far), so this holds in practice; a
caller constructing a perspective camera by other means must set the range explicitly.

### `ComputeCascades` ([engine/include/Veng/Renderer/ShadowCascades.h](../../engine/include/Veng/Renderer/ShadowCascades.h), new — glm-only, device-free)

```cpp
namespace Veng::Renderer
{
    inline constexpr u32 MaxCascades = 4;

    struct CascadeData
    {
        // Per-cascade world → light-clip matrices (only [0, Count) are valid).
        std::array<mat4, MaxCascades> ViewProj;
        // Each cascade's far distance in *view space* (the lighting pass selects a
        // cascade by comparing the fragment's view depth against these). Element i is
        // cascade i's far split; [0, Count) valid. With MaxCascades == 4 this array
        // packs directly into the ShadowConstants `CascadeSplits` vec4 (Plan 03).
        std::array<f32, MaxCascades> SplitFar;
        u32 Count = 0;  // cascades produced = clamp(settings.Count, 1, MaxCascades)
    };

    struct CascadeSettings
    {
        u32 Count      = 4;     // clamped to [1, MaxCascades]
        f32 Lambda     = 0.85f; // 0 = uniform splits, 1 = logarithmic (PSSM)
        u32 Resolution = 1024;  // per-cascade tile edge, drives texel snapping
    };

    // Pure: per-cascade fit-to-frustum light matrices. lightDir is the light's
    // travel direction; sceneBounds (world space, possibly empty) extends each
    // cascade's near plane toward the light so off-screen casters are included.
    [[nodiscard]] CascadeData ComputeCascades(
        const Camera& camera, vec3 lightDir, const AABB& sceneBounds,
        const CascadeSettings& settings);
}
```

The body, per the standard CSM construction:

1. **Split the depth range.** With `n = GetNear()`, `f = GetFar()`, for each cascade
   boundary `i/Count` blend logarithmic and uniform splits (PSSM):
   `d_i = Lambda · n·(f/n)^(i/Count) + (1−Lambda) · (n + (f−n)·(i/Count))`.
   Cascade `k` spans `[d_k, d_{k+1}]`; `SplitFar[k] = d_{k+1}`.

2. **Extract the slice's world corners.** From `inverse(camera.ViewProjection())` applied to
   the NDC cube (`x,y ∈ [−1,1]`, `z ∈ [0,1]` — Vulkan ZO) get the full frustum's 8 world
   corners, then interpolate the near→far corner pairs by the slice's split fractions
   (`(d − n)/(f − n)`, linear in view depth, so corner interpolation is exact).

3. **Fit a stable box.** Compute the **bounding sphere** of the slice's 8 corners (center +
   radius) — a sphere is rotation-invariant, so the cascade's extent does not change as the
   camera turns (no size shimmer). The ortho box is `[center ± radius]`.

4. **Build the light view + texel-snap.** `lightView = lookAt(center − lightDir·pullback,
   center, stableUp)`, where **`stableUp` is world-up `(0,1,0)`, swapped to `(0,0,1)` when the
   light is near-vertical** (`abs(lightDir.y) > 0.99`) so the `lookAt` basis never degenerates
   for a straight-down sun — the same guard `DirectionalLightViewProj` already uses. Project the
   box into light space and **snap the light-space min** to texel increments:
   `worldUnitsPerTexel = 2·radius / Resolution` (guarded `radius > epsilon`, so a degenerate
   near-zero slice does not divide by zero); round the min to a multiple of it (no shimmer as the
   camera translates). Expand the snapped box by a **one-texel guard band** on each side so a
   caster exactly on the slice edge does not sample off its tile — the atlas analogue of a fit
   padding (the in-tile clamp on the sampling side is Plan 04).

5. **Extend the near plane via the scene bound.** The slice-fit gives the box's near/far along
   the light axis, but casters between the light and the slice must still cast into it. After
   building the cascade's `lightView`, transform `sceneBounds.Transformed(lightView)` and take
   its **minimum along the light axis** (the +Z toward the light in view space); set the ortho
   **near** plane to that value (clamped no nearer than the slice's own near). **Only the
   light-axis near plane moves — the left/right/bottom/top from the texel-snapped sphere fit
   (steps 3–4) are untouched**, so the near extension never disturbs XY texel density. When
   `sceneBounds` is empty, use a fixed pullback instead.

6. **Assemble** `ViewProj[k] = orthoZO(l, r, b, t, near, far) · lightView`, with
   `proj[1][1] *= −1` (Vulkan Y-flip), matching `Camera`'s convention and the depth-compare
   the lighting pass does. `Count` is `clamp(settings.Count, 1, MaxCascades)`.

`ComputeCascades` is pure glm — no `Context`, no device — so it lives in a header/`.cpp`
under the renderer surface but is callable and testable with no ICD.

## Decisions

1. **Fit-to-cascade (frustum slice), not fit-to-scene.** The XY extent comes from the camera
   frustum slice (O(1) in scene size), so resolution follows what the camera sees. The scene
   bound enters only at step 5 (near-plane extension). This is the Unreal/Unity approach;
   fit-to-scene is the simpler mode this supersedes.

2. **Bounding-sphere fit + texel snap — the two standard stabilizations.** A sphere kills
   rotation shimmer (constant extent); texel snapping kills translation shimmer (the box
   moves in texel steps). Both are pure and unit-testable (a rotating camera yields a
   constant cascade extent; a sub-texel camera move yields an identical snapped box).

3. **PSSM split with a `lambda` knob.** The log/uniform blend is the practical scheme;
   `lambda = 0.85` default. Exposed as a `CascadeSettings` field (wired to a renderer setting
   in Plan 05).

4. **Splits carried as view-space far distances.** The lighting pass selects a cascade from
   the fragment's view depth, so `SplitFar` is in view space (not normalized) — a direct
   compare, no reconstruction.

5. **`MaxCascades = 4`, fixed.** Four is the standard ceiling and sizes the `ShadowConstants`
   block (Plan 03). More cascades is a `ShadowConstants` size bump, not done here.

6. **`ShadowCascades.h` is a public renderer header, not a `Veng/Math/` primitive.** Unlike
   `AABB` (a pure glm primitive in `Veng/Math/`, Plan 01), `ComputeCascades` consumes a `Camera`
   (a Scene/Renderer type) and produces renderer-facing matrices, so it lives under
   `Veng/Renderer/`. It is still glm-only and device-free, so it is a **public** header inside
   `include_hygiene` — callable and unit-testable with no ICD.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Scene/Camera.h` | `m_Near`/`m_Far` + `GetNear()`/`GetFar()`. |
| `engine/include/Veng/Renderer/ShadowCascades.h` (new) | `MaxCascades`, `CascadeData`, `CascadeSettings`, `ComputeCascades`. |
| `engine/src/Renderer/ShadowCascades.cpp` (new) | The split + frustum-corner + sphere-fit + texel-snap + near-extend body. |
| `engine/CMakeLists.txt` | Add `src/Renderer/ShadowCascades.cpp`. |
| `tests/unit/shadow_cascades.cpp` (new) + the unit suite source list | Device-free cascade-math tests. |

## Verification

- Clean build; the cascade header is glm-only and public, so it compiles under `include_hygiene`.
- **`tests/unit/shadow_cascades.cpp`** (device-free, no ICD):
  - **Split monotonicity & bounds**: `SplitFar` is strictly increasing, first split > near,
    `SplitFar[Count−1] == far` (exact equality); `Count` clamps to `[1, MaxCascades]`.
  - **`Lambda` endpoints**: `Lambda = 0` reproduces uniform splits, `Lambda = 1` reproduces
    the logarithmic series (within tolerance).
  - **Slice coverage**: an **interior** point of cascade 0's slice (≈50% depth) projects to
    `z ∈ [0,1]`, `xy ∈ [−1,1]` of `ViewProj[0]`, and an **interior** point of the last cascade
    (≈95% depth — not the exact far-plane corner, which guard-band/snap rounding can push past
    `z = 1`) projects within the last `ViewProj`, with an epsilon.
  - **`lookAt` degeneracy**: a **straight-down** light (`lightDir = (0,−1,0)`) yields a
    finite, non-degenerate `ViewProj` for every cascade (the `stableUp` swap holds).
  - **Rotation stability**: rotating the camera in place leaves each cascade's box **extent**
    (sphere diameter) unchanged.
  - **Texel-snap stability**: translating the camera by a sub-texel amount yields an
    identical **snapped light-space min** (the quantity actually snapped). Do **not** assert
    byte-identity of the full `ViewProj` — the bounding-sphere `center` derives from a float
    matrix inverse of the moved camera, so the product matrix varies sub-texel by design; assert
    the snapped min, or `ViewProj` equality within a tight epsilon.
  - **Near-plane extension**: a caster behind the slice (toward the light), inside
    `sceneBounds`, projects within `z ∈ [0,1]` of its cascade; with an empty bound the fixed
    pullback still includes it. **And the near extension leaves XY untouched** — the snapped
    left/right/bottom/top are identical with and without a non-empty `sceneBounds`, so the
    extension is provably near-plane-only.
- `smoke_golden` is **byte-identical** — Plan 02 adds no rendering.
- Full `ctest` green across the bands.
