# Plan 04 — `Camera` + `MeshRenderer` + game-defined registration + sample

**Goal:** close the loop — add the rendering-facing builtins (`Camera` value type +
`CameraComponent`, `MeshRenderer`), confirm the public **game-defined** component
registration path, and **migrate hello-triangle** to a `Scene`: one entity carrying
`Transform` + `MeshRenderer` + a game-defined `Spinner`, its rotation driven by a
query update, rendered through a `Camera` that replaces the hand-rolled MVP. This is
the planset's **only GPU-touching plan**.

## Why this is its own plan

Plans 01–03 are pure CPU with no consumer; this plan is where the ECS meets the
renderer and where the headline feature — a game registering and using its own
component type — is exercised end-to-end in the smoke path. Keeping it last means
the sample migrates against a finished, tested core in a single pass (the house
"migrate the sample with the breaking change" rule).

## `Camera` — `engine/include/Veng/Scene/Camera.h`

```cpp
class Camera
{
public:
    void SetPerspective(f32 fovYRadians, f32 aspect, f32 near, f32 far);
    void SetView(vec3 eye, vec3 target, vec3 up);    // or set from a world matrix

    mat4 View() const;
    mat4 Projection() const;
    mat4 ViewProjection() const;
};
```

`Camera` is a plain value type — the thing the future `SceneView` will carry and the
sample uses directly. Projection follows the engine's existing clip conventions
(the same column-major / depth setup hello-triangle's hand-rolled MVP uses today —
no clip-space change, so no golden move from the camera math itself).

```cpp
// Camera.h — a camera that lives on an entity; its view derives from the entity's
// world transform.
struct CameraComponent { f32 FovY = radians(60.f); f32 Near = 0.1f; f32 Far = 100.f; };
```

A helper builds a `Camera` from a `CameraComponent` + the entity's `WorldMatrix`
(plan 02). The sample can use either the bare `Camera` or the component form; v1
wires the bare `Camera` to keep the migration small and registers `CameraComponent`
for completeness and the future renderer. Like every component it is reflected through
`VE_REFLECT` (every registered component carries a describe-block — fieldless
components use an empty one):

```cpp
VE_REFLECT(CameraComponent, 0x…ULL)
    VE_FIELD(FovY, .DisplayName = "Field of View", .Min = 0.01)
    VE_FIELD(Near, .DisplayName = "Near", .Min = 0.001)
    VE_FIELD(Far,  .DisplayName = "Far")
VE_REFLECT_END();
```

## `MeshRenderer` — `engine/include/Veng/Scene/Components.h` (extends plan 02)

```cpp
struct MeshRenderer { AssetHandle<Mesh> Mesh; };
```

The mesh owns its materials (planset-7), so the draw queries
`(world Transform, MeshRenderer)` and, per entity, iterates the mesh's submeshes
binding `GetMaterials()[MaterialIndex]` — the existing per-submesh draw, now sourced
from the scene instead of a hand-held `Ref<Mesh>`. The deferred pipeline is the next
planset; v1 keeps hello-triangle's current `RenderGraph` forward draw.

Registered through `VE_REFLECT` like the rest (`Mesh`'s leaf type resolves to
`FieldClass::AssetHandle` via `IdOf<AssetHandle<Mesh>>()`), so it serializes
identically:

```cpp
VE_REFLECT(MeshRenderer, 0x…ULL) VE_FIELD(Mesh, .DisplayName = "Mesh") VE_REFLECT_END();
```

## Game-defined registration — the headline, exercised in the sample

The sample defines and registers its own component, through the **same** public call
the engine uses for builtins:

```cpp
// hello-triangle — a game-defined component, described next to its struct.
struct Spinner { f32 SpeedRadiansPerSec = 1.0f; };

VE_REFLECT(Spinner, 0x…ULL)   // a game-minted TypeId (vengc generate-id)
    VE_FIELD(SpeedRadiansPerSec, .DisplayName = "Speed", .Tooltip = "Radians per second", .Min = 0.0)
VE_REFLECT_END();

// at startup (OnInitialize), via the engine-owned registry:
GetTypeRegistry().Register<Spinner>();   // pulls id + name + fields from VE_REFLECT
```

The describe-block lives in the game's own translation unit; `Register<Spinner>()`
reads it back. In today's single-exe sample this call is direct. Under planset-9's
module model the identical call routes through `VengModuleRegister(host)`, which means
adding a `TypeRegistry&` to `VengModuleHost` — an **additive** seam recorded in the
README, **not built here** (planset-9 need not have landed for this planset to ship
and be tested). The point this plan proves: the engine stores, queries, and (via plan
03) can serialize `Spinner` **without any engine knowledge of the type** — that is
what makes game-defined components work across the eventual boundary.

## Sample migration — `examples/hello-triangle`

- `OnInitialize`: register `Spinner`; `Scene::Create(GetTypeRegistry())`; create one
  entity with `Transform`, `MeshRenderer{ sphere }` (the existing runtime sphere with
  the brick material), and `Spinner`; build a `Camera` (the pose the hand-rolled MVP
  used).
- `OnUpdate(delta)`: `Each<Transform, Spinner>(...)` advances `Transform::Rotation`
  (a quaternion) about a fixed axis by `Spinner::SpeedRadiansPerSec * delta`. The smoke
  mode pins the fixed pose (set the entity's rotation to `angleAxis(SmokeAngle, axis)`
  in smoke), so the capture stays reproducible.
- `OnRender`: query `(Transform, MeshRenderer)`, compute world matrices, push
  `Camera.ViewProjection() * world` per draw — replacing the hand-rolled MVP.
- `OnDispose`: drop the `Scene` and any `AssetHandle`s, as the ownership rule
  requires.

## Verification

- Clean build (`build/` and `build-debug/`), `-j 2`.
- `include_hygiene`: add `Veng/Scene/Camera.h` to the manifest (it stays
  backend-free — `Camera` is pure CPU math).
- `ctest --test-dir build --output-on-failure` green (incl. the new unit/death
  tests from 01–03 and the headless smoke).
- **Validation gate:** `ctest --test-dir build-debug -L validation` green — the draw
  path is unchanged in kind, so **no allowlist widening** (it is empty).
- Smoke binary writes a correct-sized PPM (1280×720 RGB ≈ 2,764,816 bytes).
- **Real `TypeId`s minted:** once the build is green, replace the `0x…ULL`
  placeholders on the new engine components (`MeshRenderer`, `CameraComponent`) and the
  game's `Spinner` with `vengc generate-id` values (hex for the C++ literals), per the
  working norms — never hand-invented.
- **`smoke_golden`:** the rotation representation changes (a scalar angle-about-axis
  MVP term becomes a `Transform` quaternion), so the capture is expected to shift at
  the sub-pixel level. **Regenerate the golden** as part of this plan, per `CLAUDE.md`
  (`HT_SMOKE=/tmp/ht.ppm build/.../hello_triangle-launcher && sips -s format png … `),
  and call it out in the commit body. The pose itself is unchanged in intent — the
  same orientation, expressed as a quaternion.

## Acceptance

All of the above green. Commit: `Plan 04: Camera + MeshRenderer + game-defined
components; hello-triangle renders a scene`.
