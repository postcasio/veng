# Reflection & type registration

Components are plain reflected structs. One describe-block next to the struct
gives you serialization, prefab cooking, and the editor inspector — there is no
separate schema file.

## Describing a type

A component declares its fields with a `VE_REFLECT` block. Each `VE_FIELD`
restates only the field *name*; the offset and the field's type id/class are
derived at compile time.

```cpp
struct Health
{
    f32 Current;
    f32 Max;

    VE_REFLECT(Health, 0x1234ABCD5678EF90ULL,
        VE_FIELD(Current),
        VE_FIELD(Max));
};
```

The macro family covers every shape of type:

| Macro | For |
| --- | --- |
| `VE_REFLECT(T, id, fields…)` | A struct/component with fields. |
| `VE_LEAF(T, id, FieldClass::Kind)` | A leaf or enum type (a scalar-like value). |
| `VE_TYPE(T, id)` | A fieldless (tag) component. |
| `VE_VARIANT(...)` | A tagged-union field (see below). |

## `TypeId`

Each type carries a stable `u64` **`TypeId`**, authored exactly like an
[`AssetId`](../assets/loading.md):

- **hardcoded** `0x…ULL` for engine types;
- minted with **`vengc generate-id`** for game types (hex in C++, decimal in
  JSON).

The `TypeId` is what gets saved — a scene stores a component's id, not its name —
so it must be stable and unique. Two types sharing an id is a fatal error. A type's
display name, by contrast, is only for logs and the editor, so you can rename a type
or relabel a field without breaking saved data; the saved key is the field's name in
code.

## Field classes

You can add any new type without touching the engine, but every field falls into
one of a fixed set of **field classes** that serialization and the inspector know
how to handle:

```
Scalar · Vector · Quaternion · Matrix · String ·
AssetHandle · Reference · Struct · Enum · Variant
```

Serialization is keyed by field name and tolerant of drift: a field missing from
the data keeps its default value, and an unknown field is ignored. Omitting a field
is fine; giving it the wrong type is not.

A field can also carry editor metadata — a display name, tooltip, min/max/step,
hidden/read-only flags, a category — which the inspector uses and the serializer
ignores. That's why renaming a field's display label never breaks saved data: the
on-disk key is the field name, not the label.

### Variants

A `Variant<Ts...>` field holds one of several registered struct types. It saves the
active type's id followed by its data, and is authored in JSON as
`{ "type": "<name>", "value": { … } }`. An unrecognized type just leaves the
variant empty, the same way an unknown field is ignored.

## Registering types

Register your component types in your module's `VengModuleRegister`, alongside the
application:

```cpp
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    host->Types.Register<Health>();
    // ... register the app factory ...
}
```

Order doesn't matter — a type you reference registers itself the first time it's
seen. The built-in components are already registered for you.

## Resolving resources at spawn

A component can hold a *recipe* and build the resource it describes when it spawns,
rather than storing the finished resource. It opts in with the `VE_RESOLVE` macro
and a typed resolver function, which the spawn path runs after the entity is
populated. The built-in `Primitive` works this way — see
[Prefabs & spawning](prefabs.md).
