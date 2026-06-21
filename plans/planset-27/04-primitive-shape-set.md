# Plan 04 — Expand the primitive shape set (cylinder, cone, torus, capsule)

**Goal:** add the four shapes [planset-26](../planset-26/README.md) flagged as easy
follow-ons — `Cylinder`, `Cone`, `Torus`, `Capsule` — to the `Primitives::`
generators and the `PrimitiveShapeVariant`, so a prefab can author them and the
spawn-resolve path streams them in exactly as the existing four. Depends on plan 02
(it extends `BuildShapeMeshData` and the variant after the resolution rewrite has
landed); independent of plan 03.

This plan uses the **pre-rename** component name `PrimitiveComponent`; plan 05 renames
it to `Primitive` last.

## Why this is its own plan

It is a self-contained content addition that rides plan 02's open primitive path —
no new mechanism, only new geometry on the existing one. Keeping it separate from the
seam work (01–03) keeps the resolution-model change reviewable on its own and lets the
four generators land as one focused, mostly device-free commit. It is a clean
`model: sonnet` delegation once plan 02 fixes `CreatePrimitiveMesh` and the variant
shape.

## Generators — `engine/include/Veng/Asset/Primitives.h` / `.cpp`

Four new `MeshData` generators beside `Cube`/`Plane`/`Sphere`/`Icosphere`, each with
analytic normals/tangents/UVs and an optional material, matching the existing
signature idiom:

```cpp
/// @brief A capped cylinder about the Y axis.
[[nodiscard]] MeshData Cylinder(f32 radius = 0.5f, f32 height = 1.0f, u32 segments = 32,
                                AssetHandle<Material> material = {});

/// @brief A cone about the Y axis: a circular base and an apex.
[[nodiscard]] MeshData Cone(f32 radius = 0.5f, f32 height = 1.0f, u32 segments = 32,
                            AssetHandle<Material> material = {});

/// @brief A torus in the XZ plane: a tube of minorRadius swept around majorRadius.
[[nodiscard]] MeshData Torus(f32 majorRadius = 0.5f, f32 minorRadius = 0.2f,
                             u32 majorSegments = 32, u32 minorSegments = 16,
                             AssetHandle<Material> material = {});

/// @brief A capsule about the Y axis: a cylinder of `height` capped by two hemispheres.
[[nodiscard]] MeshData Capsule(f32 radius = 0.5f, f32 height = 1.0f, u32 segments = 32,
                               u32 rings = 8, AssetHandle<Material> material = {});
```

`Cylinder`/`Cone` are the easy pair (a radial band + cap fans). `Torus` is a
two-parameter sweep; `Capsule` is a cylinder band plus two hemispheres sharing the
band's radial segment count — the involved pair, but bounded, device-free geometry
like the existing generators. Each returns canonical-layout vertices + `u32` indices +
a single submesh carrying the material, exactly as the current four do.

## Shape recipes — `engine/include/Veng/Scene/Components.h`

One reflected param struct per generator, each carrying only that shape's parameters
plus its material, with `VE_REFLECT` metadata (`.Min`/`.DisplayName`) matching the
generators' valid ranges (`segments`/`rings` min 3, radii positive):

```cpp
struct CylinderShape { f32 Radius = 0.5f; f32 Height = 1.0f; u32 Segments = 32; AssetHandle<Material> Material; };
struct ConeShape     { f32 Radius = 0.5f; f32 Height = 1.0f; u32 Segments = 32; AssetHandle<Material> Material; };
struct TorusShape    { f32 MajorRadius = 0.5f; f32 MinorRadius = 0.2f; u32 MajorSegments = 32; u32 MinorSegments = 16; AssetHandle<Material> Material; };
struct CapsuleShape  { f32 Radius = 0.5f; f32 Height = 1.0f; u32 Segments = 32; u32 Rings = 8; AssetHandle<Material> Material; };
```

Extend the variant with the four new alternatives:

```cpp
using PrimitiveShapeVariant =
    Variant<CubeShape, PlaneShape, SphereShape, IcosphereShape,
            CylinderShape, ConeShape, TorusShape, CapsuleShape>;
```

`Register<PrimitiveComponent>()` transitively registers the new alternatives (the
variant's dependency recursion), so `RegisterBuiltinTypes` is unchanged.

## Generation dispatch — `BuildShapeMeshData`

Four new arms in the `BuildShapeMeshData` dispatch, each reading its alternative and
calling the matching generator — the identical pattern as the existing four:

```cpp
if (kind == TypeIdOf<CylinderShape>()) { … return Primitives::Cylinder(c.Radius, c.Height, c.Segments, c.Material); }
// … Cone, Torus, Capsule …
```

`CreatePrimitiveMesh` and the resolver (plan 02) are **unchanged** — they read the
variant generically, so the new shapes flow through with no edit.

## Editor + cooker — free

- **Editor.** planset-26's variant widget renders any registered alternative
  generically (a combo over the alternatives' display names + the active member's
  fields), so the four new shapes appear in the `PrimitiveComponent` shape picker with
  **no editor change**.
- **Cooker.** The `PrefabImporter` validates a variant's `"type"` against the
  alternatives' registered names, so a `{ "type": "TorusShape", "value": { … } }`
  validates with **no cooker change**.

## TypeIds

The four new shape structs need stable `TypeId`s. Use placeholder `0x…ULL` while
implementing; once the build is green, mint them with `vengc generate-type-id` and
replace the placeholders (hex in C++, decimal in any JSON). The existing four shapes,
the variant, and `PrimitiveComponent` keep their planset-26 ids unchanged. Adding
alternatives is schema-tolerant — an existing cooked prefab with one of the original
shapes still loads (the tag matches an unchanged id); only the new shapes carry new
tags.

## Tests

- **Unit (CPU).** `BuildShapeMeshData` over each new alternative returns the same
  geometry the corresponding `Primitives::` call does (vertex/index counts);
  each generator's output is sane (non-empty, finite positions, bounds match the
  authored radii/height).
- **GPU.** Extend the primitive spawn test (plan 02) with one new shape (a cylinder)
  spawned and pumped to residency, asserting its `MeshRenderer.Mesh` loads with the
  expected index count — proving a new shape rides the unchanged resolve path.

## Acceptance

- Clean build; `ctest` green (the new CPU geometry cases + the extended GPU spawn
  case; GPU skips with no device).
- A prefab can author a cylinder/cone/torus/capsule `PrimitiveComponent`, the editor
  selects and edits it through the existing variant widget, the cooker validates it,
  and the spawn-resolve path streams its mesh in — all through the unchanged plan-02
  resolver and `CreatePrimitiveMesh`.
- `VE_DEBUG` validation gate clean; `include_hygiene` green.
