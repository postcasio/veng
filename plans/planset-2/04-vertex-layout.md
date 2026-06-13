# 04 — Vertex layout from the vertex type

**Goal:** stop hand-writing a `VertexBufferLayout` that restates a C++ vertex
struct's fields. Derive offsets/stride from the type, locations/formats from the
shader's reflected vertex inputs, and validate the two agree.

**Dependencies:** 01 (reflected vertex inputs). Complements planset-1/11
(`VertexBuffer<V>` already owns the element type).

## Current state

```cpp
struct Vertex { vec2 Position; vec3 Color; };
// restated by hand, by declaration order and name:
VertexBufferLayout({ {Float2, "a_Position"}, {Float3, "a_Color"} });
```

`VertexBufferLayout` is also pre-vocabulary: `const char*`/`std::string`,
`VertexElementDataType` limited to `Float/Float2/Float3`, no `Format`.

## Design

- **Modernize `VertexBufferLayout`:** express elements in engine `Format`,
  `Veng::string`, expand beyond float vectors. (Mechanical; aligns with
  planset-1/06.)
- **Derive from the type:** a description that yields offsets/stride from the C++
  vertex struct. Options to weigh in impl:
  - explicit field list with `offsetof` (`{Format::RG32Sfloat, offsetof(Vertex,
    Position)}, …`) — no macros, still local to the struct; or
  - a small `VE_VERTEX(Vertex, Position, Color)` reflection macro.
  Recommendation: start with the `offsetof` list (no macro magic), revisit.
- **Validate against the shader:** match the layout to the vertex stage's
  reflected `VertexInputs` (by location, then name) — a missing/extra/wrong-format
  attribute is a fatal assert. Locations/formats can even be *taken* from the
  interface so only offsets/stride come from the C++ side.
- Tie-in: `VertexBuffer<V>` (planset-1/11) can expose the layout for `V` so a
  pipeline created with `VertexBuffer<Vertex>` needs no separate layout arg.

## Acceptance

- Creating a pipeline for `VertexBuffer<Vertex>` needs no hand-written attribute
  list; offsets/stride come from `Vertex`, locations/formats from the shader.
- A C++/shader vertex mismatch (missing field, wrong format) is a fatal assert
  naming the attribute.
- Sample's triangle layout is derived; output unchanged.
