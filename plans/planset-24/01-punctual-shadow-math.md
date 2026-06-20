# Plan 01 — punctual shadow-view math: spot perspective + point cube faces

**Goal:** the analytic heart of punctual shadows — a **pure, device-free** function set that turns
a spot or point light into the view-projection matrix (or six) a depth pass renders with and the
lighting pass samples against. Spot → one perspective view-proj from cone angle + range; point →
six 90° cube-face view-projs. Fully unit-tested with no GPU, the way `ComputeCascades`
([ShadowCascades.h](../../engine/include/Veng/Renderer/ShadowCascades.h)) is; Plans 03–04 consume
its output. Pure glm work, no device. Independent of the rest of the planset's GPU work — it lands
its value in unit tests.

## What lands

### `PunctualShadows` ([engine/include/Veng/Renderer/PunctualShadows.h](../../engine/include/Veng/Renderer/PunctualShadows.h), new — glm-only, device-free)

A second shadow-math header beside `ShadowCascades.h`, under the renderer surface but glm-only and
device-free, so it compiles under `include_hygiene` and is unit-testable with no ICD. It consumes a
light's world position, travel direction, range, and cone half-angles (a `Light`'s fields) and
produces the world→light-clip matrices a depth pass renders the casters with.

```cpp
namespace Veng::Renderer
{
    // The six cube faces, in the fixed order the point-shadow atlas tiles them and
    // the lighting pass selects them by the major axis of the light→fragment
    // direction: +X, -X, +Y, -Y, +Z, -Z.
    inline constexpr u32 CubeFaceCount = 6;

    // A spot light's single perspective shadow view. ViewProj maps a world point to
    // the light's clip space (Vulkan ZO, Y-flipped to match Camera); a fragment in
    // the cone projects to z ∈ [0,1], xy ∈ [-1,1]. Near/Far are the clip range the
    // depth pass uses; the lighting pass reconstructs the same projective sample.
    struct SpotShadowView
    {
        mat4 ViewProj;
        f32  Near = 0.0f;
        f32  Far  = 0.0f;
    };

    // A point light's six cube-face shadow views, one perspective frustum per axis
    // (90° fovy, aspect 1). Element f is face f's world→clip matrix in CubeFace
    // order; all six share Near/Far. The lighting pass picks the face from the
    // light→fragment direction's major axis and samples projectively, exactly as a
    // spot tile is sampled.
    struct PointShadowView
    {
        std::array<mat4, CubeFaceCount> ViewProj;
        f32 Near = 0.0f;
        f32 Far  = 0.0f;
    };

    // Pure: the spot's perspective shadow view. fovy is derived from the outer cone
    // half-angle (2·OuterCone, clamped below π so the projection never degenerates),
    // aspect 1, far = the light's Range, near a small fixed fraction of Range. A
    // VE_ASSERT pins range > 0 and outerCone in (0, π/2).
    [[nodiscard]] SpotShadowView ComputeSpotShadowView(
        vec3 position, vec3 direction, f32 range, f32 outerCone);

    // Pure: the point's six cube-face views. Each face is lookAt(position, position +
    // faceForward, faceUp) with a 90° perspective; the six forwards/ups are the
    // canonical cube basis (the same set a Vulkan cube map expects). far = Range,
    // near a small fixed fraction. A VE_ASSERT pins range > 0.
    [[nodiscard]] PointShadowView ComputePointShadowView(vec3 position, f32 range);
}
```

The bodies, per the standard punctual-shadow construction:

1. **Spot.** `fovy = clamp(2·outerCone, epsilon, π − epsilon)` — the cone's full angular width, so
   the shadow frustum exactly contains the lit cone (the lighting pass's `smoothstep(cosOuter,
   cosInner, …)` falls to zero at the frustum edge, so nothing lit samples outside the map). `near =
   max(range · NearFraction, MinNear)`, `far = range`. `view = lookAt(position, position +
   normalize(direction), stableUp)` with the **same near-vertical `stableUp` swap**
   `DirectionalLightViewProj`/`ComputeCascades` use (world-up `(0,1,0)`, swapped to `(0,0,1)` when
   `abs(direction.y) > 0.99`) so a straight-down spot never degenerates the `lookAt` basis.
   `proj = perspectiveZO(fovy, 1.0, near, far)` with `proj[1][1] *= −1` (Vulkan Y-flip, matching
   `Camera` and the depth-compare the lighting pass does). `ViewProj = proj · view`.

2. **Point.** Six faces, `fovy = π/2` (90°), aspect 1, so the six frustums tile the full sphere with
   no gap or overlap at the seams. Each face `f` uses the **canonical cube-face forward/up basis**
   (`+X`, `−X`, `+Y`, `−Y`, `+Z`, `−Z` forwards, with the conventional up vectors that keep the six
   maps consistently oriented). `view_f = lookAt(position, position + forward_f, up_f)`;
   `proj = perspectiveZO(π/2, 1.0, near, far)` with `proj[1][1] *= −1`; `ViewProj[f] = proj · view_f`.
   `near`/`far` as for the spot (`Range`-relative). The cube basis is **not** subject to the
   near-vertical swap — the six fixed faces already cover every direction, and the `±Y` faces use a
   `±Z` up by construction.

Both functions are pure glm — no `Context`, no device — so the header/`.cpp` live under the renderer
surface but are callable and testable with no ICD, exactly like `ComputeCascades`.

## Decisions

1. **`PunctualShadows.h` is a public renderer header, not a `Veng/Math/` primitive.** Like
   `ShadowCascades.h` (planset-20 Plan 02 decision 6) and unlike `AABB`/`Frustum`, it produces
   renderer-facing light-clip matrices and consumes light state, so it lives under `Veng/Renderer/`.
   It is still glm-only and device-free, so it is **public**, inside `include_hygiene`, and
   unit-tested with no ICD.

2. **Spot fovy = the full cone width, so the frustum contains the lit cone exactly.** Deriving fovy
   from `2·OuterCone` (not the inner angle, not a fixed margin) means every fragment the lighting
   pass lights — the cone fades to zero by `OuterCone` — projects inside the shadow map, and nothing
   outside the cone wastes map area. The clamp below π keeps a wide cone from degenerating the
   perspective; a cone at or past a hemisphere is not a spot the single-frustum model serves (the
   assert pins `outerCone < π/2`, the practical spot range).

3. **Point = six 90° faces, the cube-map standard.** Six 90° frustums tile the sphere seamlessly;
   the alternative (a dual-paraboloid or a single wide frustum) either distorts depth non-linearly
   (breaking the hardware compare) or cannot cover the full sphere. The fixed canonical basis is
   what a Vulkan cube image and the lighting pass's major-axis face select both expect, so the
   render and the sample agree by construction — the cube analogue of the cascade tile remap.

4. **`Near`/`Far` are `Range`-relative and carried on the result.** The depth pass needs the clip
   range to set its viewport depth and the lighting pass needs it to linearize the compare; carrying
   them on the result (rather than recomputing) keeps the two sides in lockstep. `far = Range` ties
   the shadow extent to the light's own falloff radius — past `Range` the light contributes nothing,
   so nothing past it needs shadowing.

5. **Pure and unit-tested, foundation-first.** The math lands and proves out with no GPU, the way
   `ComputeCascades` did — face-direction coverage, frustum containment of the cone, and range/cone
   fit are all exact glm properties checkable with no ICD. Plans 03–04 consume `SpotShadowView` /
   `PointShadowView` without re-deriving any of it.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/PunctualShadows.h` (new) | `CubeFaceCount`, `SpotShadowView`, `PointShadowView`, `ComputeSpotShadowView`, `ComputePointShadowView`. |
| `engine/src/Renderer/PunctualShadows.cpp` (new) | The spot perspective + point six-face bodies (lookAt + perspectiveZO + Y-flip + stableUp swap). |
| `engine/CMakeLists.txt` | Add `src/Renderer/PunctualShadows.cpp`. |
| `tests/unit/punctual_shadows.cpp` (new) + the unit suite source list | Device-free spot/point shadow-view math tests. |

## Verification

- Clean build; the header is glm-only and public, so it compiles under `include_hygiene`
  (no backend leak — glm/fmt only).
- **`tests/unit/punctual_shadows.cpp`** (device-free, no ICD — the `shadow_cascades.cpp` /
  `frustum.cpp` pattern):
  - **Spot cone containment:** a point on the cone axis at half `Range` projects to `z ∈ [0,1]`,
    `xy ≈ (0,0)`; a point at the outer-cone edge at half `Range` projects to the frustum boundary
    (`|xy| ≈ 1` within epsilon) — the frustum contains the lit cone and no more. A point just
    **outside** the outer cone projects to `|xy| > 1` (outside the map), so nothing lit samples off
    the tile.
  - **Spot range fit:** a point at `Range` along the axis projects to `z ≈ 1`; a point nearer than
    `near` projects to `z < 0` (clipped). `Far == range`, `Near` is the expected `Range`-relative
    fraction.
  - **Spot degeneracy:** a **straight-down** spot (`direction = (0,−1,0)`) yields a finite,
    non-degenerate `ViewProj` (the `stableUp` swap holds), as does a straight-up one.
  - **Point face coverage:** for each of the six canonical axis directions, the point along that
    axis at half `Range` projects to `z ∈ [0,1]`, `xy ≈ (0,0)` of the **matching** face's `ViewProj`
    — the face the major-axis select would pick — and to outside `[−1,1]²` (or `z` outside `[0,1]`)
    of every other face. So the six faces partition the sphere with the seam alignment the lighting
    pass's major-axis select assumes.
  - **Point seam consistency:** a direction on a face boundary (e.g. `normalize(1,1,0)`) projects
    near the shared edge (`|xy| ≈ 1`) of **both** adjacent faces with matching depth, so a sample
    near a seam reads consistent depth from either face (the cross-fade/seam handling Plan 04 leans
    on).
  - **Range assert:** a zero or negative `range`, or a spot `outerCone` outside `(0, π/2)`, trips
    the `VE_ASSERT` (a `death` test, the suite's pattern for fatal-assert coverage).
- `smoke_golden` is **byte-identical** — Plan 01 adds no rendering.
- Full `ctest` green across the unit/death/cooker bands and the `gpu` band where present; validation
  gate clean under `VE_DEBUG`.
