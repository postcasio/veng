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
wires the bare `Camera` to keep the migration small and adds `CameraComponent` to
the registry for completeness and the future renderer.

## `MeshRenderer` — `engine/include/Veng/Scene/Components.h` (extends plan 02)

```cpp
struct MeshRenderer { AssetHandle<Mesh> Mesh; };
```

The mesh owns its materials (planset-7), so the draw queries
`(world Transform, MeshRenderer)` and, per entity, iterates the mesh's submeshes
binding `GetMaterials()[MaterialIndex]` — the existing per-submesh draw, now sourced
from the scene instead of a hand-held `Ref<Mesh>`. The deferred pipeline is the next
planset; v1 keeps hello-triangle's current `RenderGraph` forward draw.

Registered with field descriptors (`Mesh` is `FieldClass::AssetHandle`,
`TypeIdOf<AssetHandle<Mesh>>()`), so it serializes like the rest.

## Game-defined registration — the headline, exercised in the sample

The sample defines and registers its own component, through the **same** public call
the engine uses for builtins:

```cpp
// hello-triangle — a game-defined component and its hand-written descriptor.
struct Spinner { f32 SpeedRadiansPerSec = 1.0f; };

// at startup (OnInitialize), via the engine-owned registry:
GetTypeRegistry().Register<Spinner>("Spinner", {
    { "SpeedRadiansPerSec", TypeIdOf<f32>(), offsetof(Spinner, SpeedRadiansPerSec) },
});
```

In today's single-exe sample this call is direct. Under planset-9's module model the
identical call routes through `VengModuleRegister(host)`, which means adding a
`TypeRegistry&` to `VengModuleHost` — an **additive** seam recorded in the README,
**not built here** (planset-9 need not have landed for this planset to ship and be
tested). The point this plan proves: the engine stores, queries, and (via plan 03)
can serialize `Spinner` **without any engine knowledge of the type** — that is what
makes game-defined components work across the eventual boundary.

## Sample migration — `examples/hello-triangle`

- `OnInitialize`: register `Spinner`; `Scene::Create(GetTypeRegistry())`; create one
  entity with `Transform`, `MeshRenderer{ sphere }` (the existing runtime sphere with
  the brick material), and `Spinner`; build a `Camera` (the pose the hand-rolled MVP
  used).
- `OnUpdate(delta)`: `Each<Transform, Spinner>(...)` advances `Transform::Rotation`
  by `Spinner::SpeedRadiansPerSec * delta`. The smoke mode still pins the fixed
  `SmokeAngle` (set the entity's rotation to it in smoke), so the capture stays
  reproducible.
- `OnRender`: query `(Transform, MeshRenderer)`, compute world matrices, push
  `Camera.ViewProjection() * world` per draw — replacing the hand-rolled MVP.
- `OnDispose`: drop the `Scene` and any `AssetHandle`s, as the ownership rule
  requires.

## Verification

- Clean build (`build/` and `build-debug/`), `-j 2`.
- `ctest --test-dir build --output-on-failure` green (incl. the new unit/death
  tests from 01–03 and the headless smoke).
- **Validation gate:** `ctest --test-dir build-debug -L validation` green — the draw
  path is unchanged in kind, so **no allowlist widening** (it is empty).
- Smoke binary writes a correct-sized PPM (1280×720 RGB ≈ 2,764,816 bytes).
- **`smoke_golden`:** the fixed smoke pose is preserved (rotation pinned to
  `SmokeAngle`), so the golden should hold. If sourcing the transform through the
  `Scene`/`Camera` moves a pixel, regenerate per `CLAUDE.md`
  (`HT_SMOKE=/tmp/ht.ppm … && sips …`) and note it in the commit.

## Acceptance

All of the above green. Commit: `Plan 04: Camera + MeshRenderer + game-defined
components; hello-triangle renders a scene`.
