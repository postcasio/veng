# Plan 02 — Variant (de)serialization

**Goal:** make the engine reflection serializer read and write a `FieldClass::Variant`
field. A variant serializes as its active alternative's `TypeId` tag followed by that
alternative's own field record; an empty variant is the `InvalidTypeId` tag and nothing
more. Reading is skip-tolerant: an unknown or unregistered tag leaves the variant empty
and consumes only the record it was given. Also adds the `Variant` case to
`Prefab::SpawnInto`'s `Resolve` walk, so an `AssetHandle`/`Reference` field inside a
variant alternative rehydrates at spawn. Builds on plan 01's `TypeInfo` ops.

## Why this is its own plan

It is the first consumer of plan 01's contract and the one that fixes the **byte
format** the cooker (plan 03) must reproduce exactly. Pinning the tag width, the empty
encoding, and the tolerance rule here keeps 03 a mechanical mirror.

## `WriteValue` — `Serialize.cpp`

Add the `FieldClass::Variant` case beside `Struct`:

```cpp
case FieldClass::Variant:
{
    const TypeInfo& info = registry.Info(field.Type);
    const TypeId active = info.VariantActiveType(fieldPtr);
    u64 tag = active;
    AppendBytes(out, &tag, sizeof(tag));            // active TypeId, InvalidTypeId == empty
    if (active != InvalidTypeId)
    {
        const void* memberPtr = info.VariantActivePtrConst(fieldPtr);
        WriteFields(out, memberPtr, registry.Info(active), registry);
    }
    break;
}
```

The active alternative is a registered `Struct`-class type, so `WriteFields` on it is
the existing recursion — the variant adds only the leading tag. The whole value is still
wrapped in the outer length-prefix `WriteFields` already emits, so a reader that does not
know the field can skip it.

## `ReadValue` — `Serialize.cpp`

```cpp
case FieldClass::Variant:
{
    if (cursor + sizeof(u64) > in.size())
    {
        return std::unexpected("ReadFields: truncated variant tag");
    }
    u64 tag = 0;
    std::memcpy(&tag, in.data() + cursor, sizeof(tag));
    cursor += sizeof(tag);

    const TypeInfo& info = registry.Info(field.Type);
    if (tag == InvalidTypeId)
    {
        break;                                       // empty variant; nothing follows
    }
    if (!registry.IsRegistered(tag))
    {
        // Unknown alternative: leave empty and let the enclosing record's length skip
        // the rest. (The value span handed to ReadValue is bounded to this field.)
        break;
    }
    void* memberPtr = info.VariantSetActive(fieldPtr, tag);
    if (memberPtr == nullptr)
    {
        break;                                       // tag is registered but not an alternative of this variant
    }
    const Result<usize> consumed =
        ReadFieldsInner(in.subspan(cursor), memberPtr, registry.Info(tag), registry);
    if (!consumed)
    {
        return std::unexpected(consumed.error());
    }
    cursor += *consumed;
    break;
}
```

Two tolerance paths matter and both must be safe: a tag for a type registered elsewhere
but **not an alternative of this variant** (`SetActive` returns `nullptr`), and a tag
for a wholly **unregistered** type. Both leave the variant empty and rely on the outer
length-prefix to advance past any trailing bytes — the same drift tolerance an unknown
field name already gets. `ReadValue` is handed a span bounded to `cursor + valueLen`
(`ReadFieldsInner` passes `in.subspan(0, cursor + *valueLen)` to `ReadValue`), so an
over-read is a recoverable error, never a buffer overrun.

## Prefab spawn rehydration — `Prefab.cpp`

`ReadFields` decodes a variant's tag + member bytes, but the **`AssetId`-to-cache-entry
rehydration and the prefab-local `Entity` remap happen in a separate post-`ReadFields`
pass** — `Resolve` (`engine/src/Asset/Prefab.cpp`), which switches on `FieldClass` and
recurses into `Struct`. It has no `Variant` case, so an `AssetHandle` or `Reference` field
living *inside* a variant alternative (e.g. each primitive shape's `AssetHandle<Material>`,
plan 07) would keep its raw `AssetId` and never resolve to a live entry — a **silent**
failure (the `default: break` swallows it), and exactly the headline path plan 08 exercises.

Add the fifth `FieldClass` consumer there, symmetric to the `Struct` case but reaching the
active member through the variant ops:

```cpp
case FieldClass::Variant:
{
    const TypeInfo& info = registry.Info(field.Type);
    void* memberPtr = info.VariantActivePtr(fieldPtr);
    if (memberPtr != nullptr)                          // empty variant → nothing to resolve
    {
        const TypeId active = info.VariantActiveType(fieldPtr);
        Resolve(memberPtr, registry.Info(active), registry, spawned, manager);
    }
    break;
}
```

## Tests — extend `tests/unit/reflection_variant.cpp`

Reuse plan 01's `Variant<TestA, TestB>` and a wrapper struct that carries one as a field:

- **Round-trip, populated.** `SetActive<TestB>`, set its value, `WriteFields` →
  `ReadFields` into a fresh object → the active alternative and its value match.
- **Round-trip, empty.** A default (empty) variant writes the `InvalidTypeId` tag and
  reads back empty.
- **Unknown-tag tolerance.** Hand-craft a record with a tag that is (a) an unregistered
  id and (b) a registered-but-not-an-alternative id; `ReadFields` succeeds, leaves the
  variant empty, and consumes exactly the record (the following field, if any, still
  reads correctly).
- **Switch alternative on read.** Write a `TestA` record over an object whose variant
  currently holds a `TestB`; reading re-activates `TestA` (the `SetActive` path
  replaces, it does not merge).
- **Spawn rehydration.** A prefab whose component carries a variant with an
  `AssetHandle`-bearing alternative spawns and resolves: after `SpawnInto` the handle
  inside the active alternative is non-empty (its cache entry resolved), proving the
  `Resolve` Variant case descends into the active member. (Covered end-to-end by plan 07's
  round-trip test; a unit-level check here uses a throwaway handle-bearing alternative.)

## Acceptance

- Clean build; `ctest -L unit` green.
- A variant round-trips populated and empty; the serialized form is `u64` tag
  (`InvalidTypeId` = empty) + the active member's `WriteFields` record.
- Unknown / non-alternative / unregistered tags are skip-tolerant — `ReadFields`
  returns success, the variant is left empty, and surrounding fields are unaffected.
- Truncated tag / truncated member record return a recoverable `Result` error, never a
  fatal or an over-read.
- `Prefab::SpawnInto`'s `Resolve` descends into a variant's active alternative, so an
  `AssetHandle`/`Reference` field inside it is rehydrated/remapped like one in a nested struct.
