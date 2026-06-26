# Dynamic meshes — mutable runtime geometry (DRAFT / vision)

> Future area 16. Direction, not a plan. Becomes its own planset when a concrete consumer
> (sculpting, voxel terrain, destruction) is taken up.

## What this is — and is not

planset-34 unified **procedural primitives** into the mesh-load path: a mesh reference's source is
`cooked AssetId | inline recipe`, both resolved to a pending `AssetHandle<Mesh>` (the Godot
`PrimitiveMesh : Mesh` model). That covers meshes that are a **pure function of a few parameters** —
`Box(size)`, `Sphere(radius, rings)` — declarative, re-derivable, serialized as those parameters,
stable until edited.

It deliberately does **not** cover meshes whose **vertex buffer is itself the source of truth** —
geometry mutated in place at runtime that does not reduce to a recipe. That is a genuinely different
capability (Unreal models it as a `DynamicMeshComponent` that *owns* a mutable buffer, distinct from a
baked StaticMesh), and it is what this area is.

## The consumers that would need it

- **Runtime / in-editor modeling and sculpting** — the editor edits the mesh's vertices directly.
- **Voxel / marching-cubes terrain** — the surface changes as the world is dug; the buffer is the
  state, not a recipe.
- **CSG / booleans / destruction** — cutting, slicing, fracturing produce new geometry.
- **Gameplay-generated geometry** — trails, growing vines, dynamic spline-extruded roads, soft-body /
  fluid surfaces.

## The real new capability — a mutable `Mesh`

A `Mesh` in veng is **immutable after upload**. A dynamic mesh needs an in-place vertex/index buffer
update path (with retire-on-resize through the existing deferred-destruction window), plus a dirty
channel so the renderer re-reads changed geometry. That mutable-`Mesh` substrate is the substance of
this area, and it is **orthogonal** to the recipe question settled in planset-34. Unreal's parametric
generation sits *on top of* its dynamic mesh (a "generate box" writes into the mutable buffer); veng
could likewise express generators over the substrate, but the substrate is the prerequisite.

## Why it is deferred

veng has no mutable-geometry consumer yet — no sculpting tool, no voxel terrain, no destruction
system. Designing the mutable-`Mesh` plumbing against no concrete requirement would be speculative.
Crucially, the planset-34 model **does not foreclose it**: everything is one `AssetHandle<Mesh>` in
the single `MeshRenderer.Mesh` slot, so a future `DynamicMeshComponent` produces a `Mesh` into that
same slot — one it mutates in place — and nothing about the slot or the renderer query changes. The
substrate is added when a consumer arrives to design it against.
