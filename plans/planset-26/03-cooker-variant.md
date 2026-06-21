# Plan 03 — Cooker variant binding + validation

**Goal:** teach the cooker's `PrefabImporter` to bind and validate a `FieldClass::Variant`
field from JSON, emitting bytes byte-identical to plan 02's engine reader. A variant is
authored as `{ "type": <registered name>, "value": { …fields… } }`; the cooker validates
that `"type"` names one of the variant's alternatives and recurses into `"value"`.
Depends on plans 01 + 02 (the ops and the byte format). Independent of plan 04.

## Why this is its own plan

The cooker binds JSON into a type-erased component instance and then serializes it with
the **engine's own `WriteFields`** — it does not carry a parallel encoder
(`PrefabImporter.cpp`: default-construct the instance, `BindField` each field into it,
`WriteFields(records, instance, typeInfo, registry)`, `Destruct`). So byte-identity with
plan 02 is **automatic** once plan 02's `WriteFields` gains the Variant case — this plan
adds no encoding, only the JSON **binding** + **validation** of the active alternative,
plus the one author-facing surface decision (the `{ "type", "value" }` shape). That is
why it earns its own plan and its own validation tests.

## The JSON shape

```json
"Shape": { "type": "IcosphereShape", "value": { "Radius": 0.8, "Subdivisions": 4 } }
```

- `"type"` is matched against the **registered `TypeInfo.Name`** of each alternative in
  `registry.Info(field.Type).VariantAlternatives`. A name not among them is a located
  error (`"'<name>' is not an alternative of variant '<variant>'"`). The author writes the
  human name; the cooked bytes carry the `TypeId` — the same name/identity split the rest
  of the format uses.
- `"value"` is an object bound by recursing `BindField` per nested field against the
  alternative's `TypeInfo.Fields`, exactly as the existing `FieldClass::Struct` case does.
- The **primary** empty form is **omission**: a variant field absent from the JSON is
  skipped, the default-constructed (empty) component field stays empty, and `WriteFields`
  on the instance emits the `InvalidTypeId` tag — the existing field-omission tolerance,
  unchanged.
- The **explicit** empty form is `{ "type": "" }`: bind it as the empty variant (leave the
  default-constructed member untouched), which `WriteFields` again emits as the
  `InvalidTypeId` tag. (`"none"` is **not** accepted — it would collide with a future
  alternative literally named "none".) Both empty forms therefore produce the identical
  on-disk bytes and round-trip to the same empty state.

## `BindField` — `PrefabImporter.cpp`

`BindField` writes directly into the default-constructed component instance; the shared
engine `WriteFields` (plan 02's Variant case) encodes it afterward. So the variant case is
symmetric to `Struct` — `VariantSetActive` the chosen alternative into the live instance
and recurse `BindField` into `"value"`; no byte-emitting code lives here:

```cpp
case FieldClass::Variant:
{
    if (!value.is_object() || !value.contains("type"))
    {
        return err("expected an object with a 'type' key");
    }
    const TypeInfo& info = registry.Info(field.Type);
    const string typeName = value["type"].get<string>();

    if (typeName.empty())                          // explicit empty selection
    {
        return {};                                 // leave the default-constructed (empty) variant
    }

    const TypeId chosen = MatchAlternativeByName(info, typeName, registry);
    if (chosen == InvalidTypeId)
    {
        return err(fmt::format("'{}' is not an alternative of variant '{}'", typeName, info.Name));
    }
    void* memberPtr = info.VariantSetActive(fieldPtr, chosen);
    // memberPtr is non-null: chosen came from this variant's alternative list.

    if (value.contains("value"))
    {
        const json& inner = value["value"];
        if (!inner.is_object()) { return err("variant 'value' must be an object"); }
        const TypeInfo& alt = registry.Info(chosen);
        for (auto it = inner.begin(); it != inner.end(); ++it)
        {
            const FieldDescriptor* match = FindField(alt, it.key());
            if (match == nullptr)
            {
                return err(fmt::format("field '{}' is not in '{}'", it.key(), alt.Name));
            }
            const VoidResult bound = BindField(memberPtr, *match, it.value(), registry, …);
            if (!bound) { return bound; }
        }
    }
    return {};
}
```

`MatchAlternativeByName` scans `info.VariantAlternatives`, comparing each
`registry.Info(altId).Name` to `typeName`. Because the bound instance is serialized by the
shared `WriteFields`, the cooked blob and the engine reader agree to the byte with no
separate encoder to keep in sync — the byte format is owned entirely by plan 02.

## Tests — cooker suite

Register a tiny variant-bearing component in the cooker test's type registry (or reuse
plan 07's `PrimitiveComponent` if sequencing allows) and cook a `.prefab.json`:

- **Valid.** A component with `{ "type": "SphereShape", "value": { "Radius": 0.5 } }`
  cooks, and the blob round-trips through the engine `ReadFields` to the right active
  alternative + value (cross-check against plan 02).
- **Bad tag.** `"type": "NotAShape"` is a located error naming the field and the variant.
- **Omitted.** A component with the variant field absent cooks and loads empty.
- **Empty selection.** `{ "type": "" }` cooks to the `InvalidTypeId` tag, and the blob is
  byte-identical to the omitted case (both empty forms encode the same bytes).

## Acceptance

- Clean build of `vengc` + the cooker suite; cooker tests green.
- A variant authored as `{ "type", "value" }` cooks to bytes the engine serializer reads
  back to the same active alternative and field values.
- An unknown `"type"`, or a nested field not on the chosen alternative, is a **located**
  cook error (file + entity + component + field), not a silent drop.
- Omission and explicit-empty both yield an empty variant at load.
