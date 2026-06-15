# Plan 03 — The cooked prefab asset: format + importer + validation

**Goal:** define the cooked **prefab** asset and the cooker side of it. Append
`AssetType::Prefab`, add a `CookedPrefabHeader` to `assetformat`, fix the
`*.prefab.json` source schema, and build a **`PrefabImporter`** that — using plan
02's reflected `TypeRegistry` — **validates** each entity's components against the
descriptors and emits the prefab blob by **reusing planset-10's `WriteFields`**
record encoding (decision 5). Cooker tests for the happy path and each validation
failure. No runtime loader yet (plan 04); no GPU.

## Why this is its own plan, and on the main thread

It sets the on-disk prefab contract — the `*.prefab.json` schema, the
`CookedPrefabHeader`, and the validation rules — which the runtime loader (plan 04)
and any future editor save path must match. The format + validation decisions are
the reviewable surface; the per-`FieldClass` JSON binding and the error cases are
delegable once they're fixed.

## `AssetType::Prefab` + `CookedPrefabHeader` — `assetformat`

```cpp
enum class AssetType : u32 { Raw = 0, Texture, Mesh, Shader, Material, VertexLayout, Prefab };
```

Appended, never renumbered — old archives keep decoding (the enum's existing rule).
`assetformat` gains a `CookedPrefabHeader` describing the entity/component table.
Per the cycle-avoidance discipline, `assetformat` stores **opaque** component bytes
and gains **no** reflection/engine dependency — the `TypeRegistry`-driven meaning
lives entirely in the engine's `PrefabLoader` (plan 04). The layout (concrete; a
change to it is a `Version` bump, never a silent reinterpretation — the `CookedMesh`
precedent):

```cpp
struct CookedPrefabHeader   // blob head
{
    u32 Version;            // prefab-format version; loader rejects an unknown one
    u32 EntityCount;
    u32 ComponentCount;     // total across all entities
    u32 RecordBytes;        // size of the trailing record blob
};

struct CookedPrefabEntity   // CookedPrefabEntity[EntityCount]
{
    u32 FirstComponent;     // index into the component table
    u32 ComponentCount;     // this entity's components are a contiguous run
};

struct CookedPrefabComponent // CookedPrefabComponent[ComponentCount]
{
    u64 TypeId;             // the component's stable type id
    u32 RecordOffset;       // into the record blob
    u32 RecordSize;
};
// <record blob>            // the WriteFields name-keyed records, concatenated
```

Notes that make it sufficient: an **entity carries no name field** — a display name
is just a `Name` component (a builtin), serialized like any other; **hierarchy is no
header field** either — a `Parent` component holds a `Reference` to another entity,
remapped on load. A `Reference` (`Entity`) inside a record stores the **prefab-local
entity index** (its position in `CookedPrefabEntity[]`) so the loader remaps it to
the spawned handle. So the header is purely structural: `TypeId` keys each
component, field values are planset-10's record encoding, and every reference is a
prefab-local index the loader resolves.

## The `*.prefab.json` source schema

A pure data manifest — entities, each carrying components keyed by something that
resolves to a `TypeId`, each component a `{ field-name: value }` map:

```json
{
  "entities": [
    {
      "name": "Sphere",
      "components": {
        "Transform":    { "Position": [0,0,0], "Rotation": [1,0,0,0], "Scale": [1,1,1] },
        "MeshRenderer": { "Mesh": 12345678901234567890 },
        "Spinner":      { "Speed": 1.5 }
      }
    }
  ]
}
```

- **Component key → `TypeId`.** A component is named by its registered **type name**
  (resolved against the reflected registry) or directly by its `TypeId` (decimal,
  the JSON id convention). Decide one primary spelling in implementation; name-keyed
  is friendlier to author, the registry maps it to the stable `TypeId` that goes
  on disk (consistent with cooked data storing ids, not names).
- **Field values** mirror the material `*.vmat.json` value conventions: scalars as
  numbers/bools, `vecN`/`quat`/`mat4` as arrays, `string` as a string,
  `AssetHandle<…>` as an unsigned `AssetId`, `Entity` references as an index into
  this file's `entities` array (the cooker rewrites it to a `{Index,Generation}` the
  loader remaps), enums as their integer.
- **Omission is allowed** (planset-10 schema tolerance): a field absent from the
  source keeps its default-constructed value. An **unknown component** or a
  **type-mismatched field** is a cook error.

## The `PrefabImporter` — `cooker/src/Importers/PrefabImporter.{h,cpp}`

`Type()` returns `AssetType::Prefab`. `Cook(context, entry)` requires the reflected
`TypeRegistry` (threaded via `CookContext` from plan 02; absent → the located
"requires --module" error from plan 02). It:

1. Reads + parses the external `*.prefab.json` (the `source` field, same pattern as
   `MaterialImporter`).
2. For each entity, for each component: resolve the key to a `TypeId` against the
   registry (**unknown component** → located error), look up its `TypeInfo`.
3. **Build the component bytes by reusing `WriteFields`** (decision 5): default-
   construct a type-erased instance through the registry's lifecycle thunk, **bind**
   each JSON field into it through the descriptor (the validation step below), then
   `WriteFields(out, instance, typeInfo, registry)`; destruct the instance via its
   thunk. One encoder, shared with the runtime — no hand-rolled record bytes.
4. Assemble `CookedPrefabHeader` + the entity/component tables + the concatenated
   record tail into the blob.

**The JSON→field binder** (the validation core) walks the descriptor's `Fields` and,
per `FieldClass`, coerces the JSON value into the field bytes at its `Offset`:

- **`Scalar`/`Vector`/`Quaternion`/`Matrix`** — arity/number checks (a `vec3` field
  needs a 3-number array; a `float` needs a number) → write the bytes; mismatch is a
  located error mirroring the material importer's `IsFloat`/`ComponentCount` checks.
- **`String`** — JSON string → the field's string.
- **`AssetHandle`** — unsigned `AssetId`; optionally `Resolve`-checked against the
  pack for type (`MeshRenderer.Mesh` must resolve to `AssetType::Mesh`), the way the
  material importer checks texture references. Residency is the runtime's job.
- **`Enum`** — integer in range → underlying int.
- **`Reference`** (`Entity`) — an index into the file's `entities`; rewritten to the
  `{Index,Generation}` the loader remaps. Out-of-range index → located error.
- **`Struct`** — recurse into the nested type's `Fields`.
- A field **present in JSON but absent from the descriptor** → located error
  (stricter than the runtime's tolerance: at cook time an unknown field is almost
  always a typo worth catching). A field **absent from JSON** → leave the default.

All errors are located: `"prefab importer: '<file>': entity[<n>] '<EntityName>'
component '<TypeName>': field '<f>': <reason>"`.

## Importer registration — `cooker/src/BuiltinImporters.cpp`

`RegisterBuiltinImporters` registers a `PrefabImporter`. Unlike the others it needs
the reflected registry, which is per-cook (depends on `--module`), so it is wired to
read the registry from `CookContext` at `Cook` time rather than at construction —
keeping `AssetImporter`'s zero-arg-construction shape intact.

## Tests — `cooker` suite (`-L cooker`)

Against a fixture `*.prefab.json` + the reflected hello-triangle registry (plan 02):

- **Happy path:** a prefab with one entity (`Transform` + `MeshRenderer` + `Spinner`)
  cooks; assert the blob's `CookedPrefabHeader` counts, that each component's `TypeId`
  matches the registry, and that the record tail round-trips (decode with
  `ReadFields` in-test back to the authored values — proving the cooker emits exactly
  what the runtime will consume, *without* needing plan 04's loader).
- **Unknown component:** a component key with no registered type → located error.
- **Wrong field type:** a `vec3` field given a scalar, a string field given a number
  → located errors naming the field.
- **Unknown field:** a field name absent from the descriptor → located error.
- **Omitted field keeps default:** a component with a field omitted decodes to that
  field's default value.
- **Reference rewrite:** an `Entity` reference to `entities[k]` cooks to a
  `{Index,Generation}`; an out-of-range index is a located error.
- **`AssetHandle` field:** a `MeshRenderer.Mesh` id survives to the blob; a
  type-mismatched reference (id resolves to a non-Mesh) is a located error.
- **No `--module`:** cooking a pack with a prefab entry and no registry is the
  plan-02 "requires --module" error.

## Acceptance

Clean build; `ctest -L cooker` green; `assetformat` gains `CookedPrefabHeader` and
`AssetType::Prefab` with no new dependency; existing packs unaffected. Commit:
`Plan 03: cooked prefab asset — AssetType::Prefab, CookedPrefabHeader, PrefabImporter
validating against reflected descriptors, WriteFields blob`.
