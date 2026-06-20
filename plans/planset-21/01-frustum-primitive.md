# Plan 01 — Frustum primitive + AABB intersection

**Goal:** add the engine's frustum bounds primitive — a six-plane `Frustum` extracted from a
view-projection matrix and a conservative `Intersects(Frustum, AABB)` test. This is the pure,
device-free heart of culling; Plans 02–03 consume it. Mirrors the planset-20 foundation-first
shape (a glm-only primitive, fully unit-tested before any GPU work).

## What lands

### The `Frustum` type ([engine/include/Veng/Math/Frustum.h](../../engine/include/Veng/Math/Frustum.h), new)

A glm-only frustum in the `Veng/Math/` home `AABB` already seeds. Public, pulling in nothing
but `Veng.h` and `AABB.h`.

```cpp
namespace Veng
{
    struct Frustum
    {
        // Six bounding half-spaces — left, right, bottom, top, near, far. Each is
        // a plane vec4(nx, ny, nz, d) with an inward-pointing normal, so a point p
        // is inside the frustum when dot(plane.xyz, p) + plane.w >= 0 for all six.
        // The normals are NOT normalized: Intersects is a sign-only test, so unit
        // length is unnecessary (normalizing for a true signed distance is a
        // one-line add if a distance-based consumer ever needs it).
        std::array<vec4, 6> Planes;

        // Gribb-Hartmann: each plane is one clip-volume inequality of the combined
        // view-projection matrix, read straight off its rows. left/right/bottom/top
        // are row4 -/+ {row1,row2}; the Vulkan ZO near plane is the third clip row
        // alone (clip.z >= 0) and far is row4 - row3 (NOT the OpenGL row4 +/- row3
        // pair). Because each plane IS the inequality the inside-frustum region
        // satisfies by construction, the inward orientation is automatic for any ZO
        // projection the matrix carries — including the engine's Y-flipped one (the
        // flip swaps which plane is geometrically "top" vs "bottom", which a cull
        // that tests all six never names). No sign-correction or per-plane
        // reorientation step exists; the one ZO-specific row is the near plane.
        [[nodiscard]] static Frustum FromViewProjection(const mat4& viewProj);
    };

    // Conservative AABB-vs-frustum test. Returns false only when `box` lies wholly
    // outside one plane (the standard positive-vertex test: the box corner farthest
    // along each plane's normal is still behind it). A box straddling a plane, or
    // just outside a frustum corner, returns true — a false positive that draws,
    // never a false negative that culls a visible box.
    [[nodiscard]] bool Intersects(const Frustum& frustum, const AABB& box);
}
```

`FromViewProjection` takes the **combined** view-projection (the same `view.Camera.ViewProjection()`
the g-buffer pass already forms, and the per-cascade `view.CascadeViewProj[k]` the shadow pass
forms) and yields planes in the **world space** that matrix maps from — so an entity's
world-space `AABB` tests directly, no per-plane space change. The one extraction subtlety is the
**near plane**: the GL form (`row4 + row3`) is wrong for `z ∈ [0,1]`, so the ZO form uses the third
clip row alone (`clip.z >= 0`). No sign-correction or per-plane reorientation is needed — each plane
is a clip-volume inequality of the supplied matrix, so its inward normal falls out regardless of the
Y-flip or handedness; the inequality is satisfied by exactly the points the matrix maps inside the
clip volume, which is the definition of "inside the frustum."

`Intersects` is the positive-vertex (p-vertex) test: for each plane, pick the box corner
farthest in the plane's normal direction (`Min`/`Max` selected component-wise by the normal's
sign) and reject the box only if **that** corner is outside. It is the conservative standard —
correct and branch-light. Non-trivial bodies live in `engine/src/Math/Frustum.cpp`; one-liners
stay inline.

## Decisions

1. **Gribb-Hartmann extraction from the combined matrix, in world space.** The six planes come
   straight off the view-projection rows — no separate frustum-corner reconstruction. The planes
   land in the space the matrix maps **from** (world, for a `viewProj`), so a world-space mesh
   `AABB` tests without a space change. This is distinct from planset-20's `ComputeCascades`,
   which reconstructs frustum **corners** (inverse-matrix over the NDC cube) because a cascade
   fit needs the corner positions; culling needs only the planes, and the direct extraction is
   cheaper.

2. **Vulkan ZO clip, not OpenGL — and inward orientation is automatic.** The near/far rows differ
   between `z ∈ [0,1]` (Vulkan, what veng uses) and `z ∈ [-1,1]` (GL); the extraction uses the ZO
   form (near = third clip row alone). Every plane is a clip-volume inequality of the supplied
   matrix, so its inward normal is correct for **any** ZO projection — including the engine's
   **Y-flipped** one — with no sign-correction logic. The flip relabels "top"/"bottom" geometrically,
   which a six-plane cull never depends on. The unit test pins the ZO near plane against a known
   matrix (a GL-form regression is caught loudly) **and** confirms inward orientation against the
   engine's real Y-flipped `Camera::ViewProjection()`, not a bare `glm::perspective` — the one place
   a transcription slip would surface.

3. **Conservative p-vertex test — correctness over tightness.** A false cull (dropping a visible
   mesh) is a visible artifact; a false keep (an extra draw) is only wasted work. The p-vertex
   test never false-culls. The corner-exact refinement (also testing the frustum's own corners
   against the box, killing the large-box-near-corner false positive) is a measured optimization,
   not built.

4. **`Frustum` is value-type math, not a `Create`-factory resource** — copied freely like
   `AABB`/`mat4`, no ownership rule, not reflected this planset (matches the `AABB` decision).

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Math/Frustum.h` (new) | The `Frustum` struct + `FromViewProjection` + `Intersects`. |
| `engine/src/Math/Frustum.cpp` (new) | `FromViewProjection` (clip-row extraction, no normalize) + `Intersects` (p-vertex sign test) bodies. |
| `engine/CMakeLists.txt` | Add `src/Math/Frustum.cpp`. |
| `tests/unit/frustum.cpp` (new) + the unit suite source list | Device-free `Frustum` extraction + intersection tests. |

## Verification

- Clean build; `include_hygiene` compiles `Veng/Math/Frustum.h` (glm-only — no backend leak).
- **`tests/unit/frustum.cpp`** (device-free, no ICD):
  - **Extraction / inward orientation**: from the engine's real **Y-flipped**
    `Camera::ViewProjection()` (Vulkan ZO, not a bare `glm::perspective`), the six planes have
    **inward** normals — the camera position and a point dead-center in the view satisfy every
    `dot(n, c) + d >= 0`, and a point just above/below screen-center-but-in-FOV is inside (the
    centered-box case a centroid-only check can miss if the Y-flipped top/bottom pair were
    mis-oriented). Planes are not asserted unit-length (the test is sign-only).
  - **ZO near plane (the discriminator)**: a point just in front of the near plane — closer than the
    near plane but not against the camera, so `clip.z < 0` (outside the Vulkan `z = 0` near) yet
    `clip.z + clip.w > 0` — is culled by the ZO extraction and would be **kept** by the GL form. That
    divergence slab is the discriminator; a point hard against the camera fails *both* forms and does
    not pin the ZO fix.
  - **Box wholly inside** the frustum → `Intersects` true; **box wholly outside** each of the six
    planes in turn (translated far left/right/up/down/near/far) → false.
  - **Straddling** a plane (a box centered on the near plane) → true (no false cull).
  - **Behind-camera** box → false (the near/far pair rejects it).
  - **Conservative property**: a box that intersects the true frustum volume is never culled
    (sample boxes around the frustum boundary; assert no `Intersects == false` for any box with a
    point inside).
  - **Ortho extraction**: from a `glm::ortho` (a cascade's shape) the planes are axis-aligned and
    bound the ortho box — a box at the box edge intersects, one just past it does not.
- `smoke_golden` is **byte-identical** — Plan 01 adds no rendering.
- Full `ctest` green across the unit/death/cooker bands and the `gpu` band where present.
</content>
