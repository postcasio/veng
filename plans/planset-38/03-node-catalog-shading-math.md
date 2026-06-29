# Plan 03 — expand the node catalog to real shading math

**Goal:** make the graph genuinely expressive. With the emit walk (Plan 01) and the graph-sourced
shader cook (Plan 02) in place, the catalog still holds only `TextureSample`/`Param`/`MaterialOutput`
— enough to *bind*, not to *shade*. This plan adds the real **emitter nodes** — math, swizzle, and
utility — each a `NodeType` carrying an explicit leaf `TypeId`, an emit-fn, and a JSON round-trip.
These nodes are exactly the ones inert in the old binding model (a `Multiply` meant nothing), so this
is the catalog finally doing what it was half-shaped for. Depends on **Plan 02** (nothing new in the
cook/preview loop — only catalog entries).

## Why it is its own plan

The codegen *machinery* is done after Plan 02; this plan is pure **content over a stable mechanism** —
adding emitter nodes that each reduce to "register a `NodeType`, write its emit-fn, cover its
round-trip." Splitting it out keeps the mechanism plans (00–02) small and reviewable and lets the
catalog grow without re-touching the emit walk or the cook path. It is also the plan whose scope is
the easiest to tune (ship more or fewer nodes) without affecting the architecture.

## What lands

- **Arithmetic.** `Multiply`, `Add`, `Subtract`, `Divide` — binary, component-wise, with the
  coercion-on-link rules already enforced (a scalar splats against a vector). Each emit-fn is a
  one-line operator emit over its two input `EmittedValue`s.

- **Interpolation / clamping.** `Lerp` (a/b/t — ternary), `Saturate`, `Clamp` (value/min/max —
  ternary), `OneMinus` — the everyday shaping operators.

- **Vector algebra.** `Dot`, `Cross`, `Normalize`, `Length` — emitting the corresponding Slang
  intrinsics; output pin types follow the operation (`Dot`/`Length` → `float`). `Cross` is vec3-only
  (its input pins are typed vec3, so the topology validator rejects a non-vec3 connection at connect
  time — no per-node guard needed).

- **Swizzle / channel plumbing.** `Split` (vecN → N scalar output pins, named `.x/.y/.z/.w`) and
  `Combine` (a fixed 4 scalar input pins → vec4, with unconnected trailing inputs defaulting to 0),
  the channel routing a shading graph constantly needs; emitted as swizzles / a constructor.

- **Constants & scalar params.** `Constant` (an authored literal of a chosen leaf type, always
  inlined) and `ScalarParam` (a `Param` specialized to `float`, for the common single-value knob,
  carrying Plan 02's provenance), so authoring a tweakable scalar is one node.

- **Each node carries an explicit leaf `TypeId`.** The property POD records the node's leaf type
  (`float`/`vec2`/`vec3`/`vec4`/`uint`) explicitly — driving both the emitted Slang type and, for
  `Constant`, the literal form — rather than inferring it from a `vec4` value blob's size. So the
  generated struct field type and any inline literal are unambiguous.

A worked example — a `Combine(r, g, b, a)` feeding a `Multiply(_, tint)`:
```
float n3_Combine = ...;                       // each scalar input
float4 n5_Out = float4(n3_r, n3_g, n3_b, n3_a);
float4 n7_Out = n5_Out * p.Tint;              // Tint an exposed float4 Param
```

- **Each node is the standard `NodeType` shape.** Pins (typed in/out over the builtin-leaf `TypeId`
  space), a reflected property POD where one applies (e.g. `Constant`'s value + leaf type,
  `ScalarParam`'s default + provenance), the emit-fn (Plan 01's contract), and the catalog name (the
  stable serialized key) — so the existing JSON (de)serialization, the node-property inspector, and the
  topology validation all cover them with no new machinery.

- **Catalog registration is domain-shared.** These nodes are domain-independent (math is math), so
  the schema-independent `RegisterMaterialNodeTypes` (Plan 01) registers them for both Surface and
  PostProcess; only `MaterialOutput`'s sinks differ by domain (unchanged from Plan 01).

## Decisions

1. **The new nodes are content, not mechanism.** Every one reduces to a `NodeType` + an emit-fn over
   `EmittedValue`s; none touch the emit walk, the cook path, or the catalog's schema-independence. The
   plan adds no architecture.
2. **Coercion is the existing link rule.** Splat/truncate is already enforced at connect time and
   applied by the walk (Plan 01); a `Multiply(float, vec3)` works because the scalar splats — no
   per-node coercion code.
3. **Output pin types follow the operation.** `Dot`/`Length` emit `float` pins, `Split` emits scalar
   pins, `Combine` a vec4 — so downstream connection validity (and further coercion) falls out of the
   pin types, not special cases. Arity constraints (`Lerp`/`Clamp` ternary, `Cross` vec3) are encoded
   in the pin types, enforced by the topology validator.
4. **`Constant` always inlines; `ScalarParam` carries the provenance.** A literal has no reason to
   become a uniform; a `ScalarParam` is the ergonomic single-scalar case of Plan 02's `Param`.
5. **Catalog scope is tunable here, nowhere else.** The architecture does not care how many nodes
   ship; this plan is where the initial set is chosen, and a later node is a one-entry addition.

## Files

| File | Change |
|---|---|
| `MaterialCatalog.h` / `.cpp` (`veng::graph`) | Register the new `NodeType`s (pins + property PODs with explicit leaf `TypeId` + emit-fns + stable names); their `FieldDescriptor`s where they carry properties. |
| `MaterialCompile.cpp` / emit-fns (`veng::graph`) | The per-node emit-fns over `EmittedValue` (operators, intrinsics, swizzles, constructors). |
| `tests/…` (cooker/unit band, device-free) | Emit-walk tests over graphs using the new nodes: a `Multiply`/`Lerp`/`Split`/`Combine` chain → expected generated Slang; a round-trip (graph → JSON → graph) preserving the new nodes + properties + leaf types. |
| `editor/CLAUDE.md` | List the material node catalog (now a shading-expression set, not three binding nodes). |

## Verification

- Clean build; `ctest` green. The new emit-walk tests assert correct generated Slang for math/
  swizzle/constant chains and a clean JSON round-trip of the new node types + properties + leaf types.
- A graph mixing the new nodes cooks (offline) into a valid material whose generated fragment compiles
  — exercised by a test fixture material.
- `smoke_golden` does **not** move — no shipping material is migrated yet (Plan 04).
- `include_hygiene` unaffected; the new nodes are `veng::graph` `NodeType` data.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
