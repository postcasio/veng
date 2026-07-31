# Veng/Reflection — types, fields, and the shared serializers

The reflection layer every subsystem binds through: an **open** `TypeId` space (a game adds its own
leaf/struct/component types with no engine change) over a **closed** `FieldClass` set a generic
walker switches on, with one binary walker and one JSON walker shared by the cooker, the editor,
MCP, and the net layer's replication codec. Project-wide conventions live in
[the root CLAUDE.md](../../../CLAUDE.md), the runtime overview in
[engine/CLAUDE.md](../../CLAUDE.md), and the ECS that pools reflected components in
[../Scene/CLAUDE.md](../Scene/CLAUDE.md); the consumers are
[cooker/CLAUDE.md](../../../cooker/CLAUDE.md), [editor/CLAUDE.md](../../../editor/CLAUDE.md), and
[mcp/CLAUDE.md](../../../mcp/CLAUDE.md).

## TypeRegistry & TypeId

Types register into the **`TypeRegistry`** — **host-owned, borrowed by the engine**: the host
(launcher or cooker) constructs it, pre-registers the builtins, fills it via `VengModuleRegister`,
and threads it in; `Application` borrows a `TypeRegistry&` (`Application::GetTypeRegistry()`) and
threads it into the `AssetManager` and into `Scene::Create(TypeRegistry&)`. It is generic over
*any* reflected type — leaf field types, nested structs, and components share **one `TypeId`
space**.

Each type carries a stable `u64` **`TypeId` authored exactly like an `AssetId`**: hardcoded
zero-padded `0x…ULL` for engine types, `vengc generate-id` for game types — the same zero-padded
hex spelling in C++ and (where an id reaches JSON) as a hex string, never a bare decimal. It is a
compile-time constant (`TypeIdOf<T>()` reads it off a trait, independent of registration order),
persisted directly (a scene stores a component's `TypeId`, never its name), and byte-identical
across the module boundary (so the cooker reflecting a module reads the same ids the runtime does);
two types claiming one id is a **fatal collision assert**.

**Every reflect-macro site must spell its type fully qualified** (a leading `::`) — a hard rule the
macros enforce with a `static_assert` (`Detail::IsFullyQualifiedSpelling`; a fundamental type like
`bool`, which has no namespace and cannot be `::`-prefixed, is the sole exception) — so the
registry captures the namespace for every type. `SplitQualifiedTypeName` splits the authored
spelling into the bare `TypeInfo.Name`, its `TypeInfo.Namespace` (e.g. `{ "vec3", "Veng" }`), and
the joined `TypeInfo.QualifiedName` (`"Veng::vec3"`, or just the bare name when global).
Name/Namespace are logs/editor display only — the editor shows them as `Name (Namespace)` with the
namespace de-emphasized (`Veng::UI::TypeLabel`); `QualifiedName` is the single key **all**
type-name matching is done against (`TypeNameMatches`, strict).

A game registers its own types through the same path as the builtins — a **`VE_REFLECT`**
describe-block next to the struct, read back by the zero-arg `Register<T>()` (a referenced type's
schema auto-registers from its trait on first reference, so there is no registration-ordering
burden). A **leaf or enum** type is authored with **`VE_LEAF(Type, 0x…ULL, FieldClass::Kind)`** and
a **fieldless component** with **`VE_TYPE`** — all three macros specialise the **single
`VengReflect<T>`** identity trait with a uniform member set, so `TypeIdOf<T>()` /
`FieldClassOf<T>()` read it directly and the registry has one `Register<T>()` path (no separate
leaf registration).

Beside the identity trait sit four **separate specialisation points**, each authored by its own
macro next to the describe block and read into `TypeInfo` by `Register<T>()`: `VE_REPLICATED`
(`Replicated`), `VE_ALWAYS_RELEVANT` (`AlwaysRelevant`), `VE_VIEW_OUTPUT` (`ViewOutput`), and
**`VE_REQUIRES(Type, Siblings…)`** (`Requires`, the sibling components an entity carrying the type
must keep — see [../Scene/CLAUDE.md](../Scene/CLAUDE.md) for the removal gate that reads it). Being
separate from `VengReflect<T>` is what lets them compose with every reflection macro without
touching one. `VengRequires<T>::Required()` is a member template on a defaulted parameter for the
same reason `Enumerators()` is: the `vector` it builds must be dependent, so it instantiates only
where `Register<T>()` calls it.

## The field model

The layer pairs the open `TypeId` space with a **closed** `FieldClass`
(`Scalar`/`Vector`/`Quaternion`/`Matrix`/`String`/`AssetHandle`/`Reference`/`Struct`/`Enum`/
`Variant`/`Array`) a generic walker switches on.

**`FieldClass::Array`** is a dynamic list (a `vector<T>` of one registered element type) authored
with **`VE_ARRAY_FIELD`**; the element type and four type-erased container shims (`ArraySize` /
`ArrayElement` / `ArrayElementConst` / `ArrayResize`) ride the `FieldDescriptor`, so the generic
walker, the name-keyed serializer, and the editor inspector each drive the list through the shims
rather than by offset — the same erased-ops shape `Variant` uses.

**`FieldClass::Variant`** is the tagged-union meta-kind: a `Variant<Ts...>` field
(`Veng/Reflection/Variant.h`, authored with `VE_VARIANT`) holds one of several registered struct
alternatives, and reflection reaches its active member only through type-erased thunks on the
variant's `TypeInfo` (`VariantActiveType`/`VariantActivePtr`/`VariantSetActive`/…), never by
offset. It **serializes as a `TypeId` tag** (the active alternative's id, `InvalidTypeId` for
empty) followed by that member's record; an unknown or unregistered tag leaves the variant empty
and skips the record, the same schema-drift tolerance an unknown field name gets. It is **authored
in JSON as `{ "type": <registered name>, "value": {…} }`**, the cooker matching `"type"` against an
alternative's fully-qualified `QualifiedName` (`TypeNameMatches`, strict). The prefab loader's
dependency walk and `Prefab::SpawnInto`'s rehydration both recurse into the active alternative, so
an embedded `AssetHandle` inside a variant (a shape's material) loads as an ordinary dependency.

`FieldDescriptor`s — authored via `VE_REFLECT`/`VE_FIELD`, each deriving its `Offset` (`offsetof`)
and its field type's `TypeId`/`FieldClass` at compile time, restating only the field *name* — drive
a tolerant, name-keyed, recursive generic serialization and the editor inspectors. A
`FieldDescriptor` additionally carries **optional editor metadata** (`DisplayName`, `Tooltip`,
`Min`/`Max`/`Step`, `Hidden`, `ReadOnly`, `Category`) that the serializer **ignores** — it reads
only `Name`/`Type`/`Offset`. The serialization key (`Name`) is kept distinct from the UI label
(`DisplayName`), so relabelling never breaks on-disk compatibility: **on-disk type identity is the
`TypeId`, field identity is the name**.

## The two shared walkers

**`Veng/Reflection/Serialize.h` (`WriteFields`/`ReadFields`) is the shared binary walker** that
produces the cooked-blob encoding; **`Veng/Reflection/JsonSerialize.h`
(`JsonWriteFields`/`JsonReadFields`) is its JSON analogue**, the one walker every JSON asset
surface binds through — the cooker's `PrefabImporter`/`LevelImporter`, the editor's
`PrefabSerialize` and `LevelEditorPanel` config round-trip, and MCP's `ReflectToJson` all call it
rather than each hand-rolling a `FieldClass` switch. It covers every `FieldClass`
(Scalar/Vector/Quaternion/Matrix/String/Enum/AssetHandle/Reference/Struct/Variant/Array) and
reports a malformed field as a **dotted field path** ("`Settings.Bloom.Kernel`: expected an
enumerator name"), which each caller prepends its own located prefix to (file/entity, document,
request).

What differs per consumer is isolated into a small **`JsonFieldHooks`** policy struct —
`ValidateAssetId` (a nonzero `AssetHandle` id against the caller's resolve context; unset accepts
every id) and `ReadReference`/`WriteReference` (an `Entity` from/to JSON — prefab-local index, live
entity, MCP's own addressing; unset makes a `Reference` field a located error). `JsonWriteFields`
has two forms: the fresh-object overload builds a brand-new `json::object()`, and the
**merge-write** overload (`JsonWriteFields(json& into, ...)`) patches an existing document's
reflected keys in place, leaving every other key untouched — the form a save-in-place writer
(`PrefabSerialize`, `LevelEditorPanel`) needs to keep an edit a minimal diff against hand-authored
source. `JsonReadFields` defaults to the cooker's strict posture (an unrecognized key is a located
error); `allowUnknownFields = true` is the editor panels' tolerant read, ignoring a key the walker
doesn't own so the merge-write can preserve it.

## Enums

**Every enum in asset JSON serializes as its C++ enumerator name, never an ordinal** — a hard cut,
not a tolerated pair of forms. `EnumName.h`'s runtime-typed functions back the walker's `Enum`
case: `EnumeratorName(const TypeInfo&, i64)` / `ParseEnumValue(const TypeInfo&, string_view)` look
an enumerator up by a runtime `TypeInfo&` rather than a compile-time `T` (the templated
`ParseEnum<T>`/`EnumeratorName<T>` are thin wrappers over these), and
`LoadEnumBits`/`StoreEnumBits` read/write an enum field's backing bytes at its reflected `Size`.
Matching is exact and case-sensitive; a JSON integer where a name is expected is a located error
naming the field. `EnumeratorName` keeps a documented decimal-string fallback for an out-of-range
value (a corrupt value stays readable); `ParseEnumValue` does not accept that fallback back on
read, which is the correct loud failure.
