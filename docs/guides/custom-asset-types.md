# Defining your own asset type

veng ships sixteen asset types, but the type space is open: an asset type is a minted `u64`, not
an enum case, so a game defines its own without touching the engine. A complete custom type is
three pieces — an **identity**, an **importer** that cooks a source file into a blob, and a
**loader** that decodes that blob at runtime — registered through two seams. This guide walks the
whole path. The worked example is `examples/template`, which carries a small `MarkerSet` type end
to end; every file named below is real and readable there.

## The shape of it

```
lib<game>          runtime   registers the type id + name, and a loader factory
lib<game>_cook     tool      registers the importer; loaded by vengc and the editor
```

Two images, because the cooker's machinery must not ship in your game and your importer's
dependencies must not ship either. The split is also why the seam is safe: `lib<game>_cook` links
**`veng::cook_interface`** — the importer contract as headers only — and never the static
`libveng_cook`, which would carry a second copy of the cooker's process-wide state (the Slang
session, the graph-shader resolver hook) into the dlopened image beside the tool's own.

**The type's identity registers in exactly one place: the runtime module.** The cook module
registers importers and nothing else. Both images load in the editor, so registering the id from
both seams would deliver it twice — and a duplicate asset-type id is fatal by design.

## 1. Mint the identity

Never invent an id by hand:

```sh
vengc generate-asset-type --module build/libmy_game.dylib
```

It collision-checks against the engine builtins and every type the named module already
registers, and prints the id in both spellings — `0x{:016X}ULL` for the C++ literal, the same
zero-padded hex as a quoted **string** for JSON. Put the constant in a header both images
include, beside the cooked layout they share:

```cpp
// MarkerSet.h — the contract the cook module writes and the runtime loader reads.
inline constexpr Veng::AssetTypeId MarkerSetAssetType{0xCE3FFFC917EF1C7FULL};
inline constexpr const char* MarkerSetTypeName = "MarkerSet";
```

Sharing one header is the point: if the blob layout changes, both halves fail to compile
together rather than drifting into a silent decode bug.

## 2. Define the runtime asset and its loader

The asset is an ordinary struct. Bind it to the minted id with an `AssetTypeTrait`
specialization — that single specialization is what makes `AssetHandle<T>` and
`AssetManager::Load<T>(id)` resolve for a type the engine has never heard of:

```cpp
struct MarkerSet { Veng::vector<Marker> Markers; };

namespace Veng
{
    template <>
    struct AssetTypeTrait<MarkerSet>
    {
        static constexpr AssetTypeId Type = MarkerSetAssetType;
    };
}
```

The loader is an `AssetLoader` subclass. It returns a `Detail::LoadJob` carrying the decoded
resource; a type with no GPU resource needs no `Finalize` and no dependencies, which is the
simplest shape. Treat a malformed blob as **recoverable** — return an `AssetLoadError`, do not
assert. A cooked blob is a build artifact, but a stale one is a normal thing to meet.

## 3. Write the importer

The importer lives in the cook module and turns the pack entry's JSON (plus whatever files it
names) into bytes:

```cpp
class MarkerSetImporter final : public Veng::Cook::AssetImporter
{
public:
    Veng::AssetTypeId Type() const override { return MarkerSetAssetType; }

    Veng::Result<Veng::vector<Veng::u8>> Cook(const Veng::Cook::CookContext& context,
                                              const Veng::Cook::json& entry) const override;
};
```

Two rules that bite:

- **Guard every typed JSON access with an `is_*()` check first.** The cooker builds with
  `JSON_NOEXCEPTION`, so an unchecked access aborts instead of reporting a located error.
- **Call `context.RecordDependency(path)` for every file you read** that the manifest does not
  itself name. That is what makes editing your source re-cook the pack.

Register it and export the cook ABI:

```cpp
extern "C" void VengCookModuleRegister(Veng::Cook::VengCookModuleHost* host)
{
    host->Importers.Register(Veng::CreateUnique<MarkerSetImporter>());
}

VE_EXPORT_COOK_MODULE_ABI()
```

`Veng::Cook` re-exports `CreateUnique`, so qualify it as `Veng::CreateUnique` if your TU also has
a `using namespace Veng;`.

## 4. Register both halves

The runtime module's `VengModuleRegister` registers the identity and the loader **factory** — a
factory, not a loader, because registration runs before any `Context` or `AssetManager` exists.
It then points the app's `ApplicationInfo` at the host-owned registries, which is how the
`AssetManager` the engine later builds finds them:

```cpp
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    host->AssetTypes.Register(AssetTypeInfo{.Id = MarkerSetAssetType,
                                            .Name = MarkerSetTypeName,
                                            .DisplayName = "Marker Set",
                                            .Glyph = "MRK"});
    host->AssetLoaders.Register(MarkerSetAssetType,
                                [] { return Unique<AssetLoader>(new MarkerSetLoader()); });

    host->App.RegisterApplication(
        [assetTypes = &host->AssetTypes, assetLoaders = &host->AssetLoaders](
            TypeRegistry& types, SystemRegistry& systems)
        {
            return Unique<Application>(new MyApp(
                ApplicationInfo{
                    /* ... */
                    .AssetTypes = assetTypes,
                    .AssetLoaders = assetLoaders,
                },
                types, systems));
        });
}
```

`DisplayName` and `Glyph` are editor metadata: your type browses and chips under that name. Badge
*colour* is an editor-local table, so a game-registered type shows the neutral grey fallback.

> **Lifetime.** These registries hold owned polymorphic objects whose vtables live in a
> `dlclose`-able image. The module handle must outlive the registries *and* the `AssetManager`
> built from them. Every veng host enforces this structurally — the handle is declared first in
> the owning struct, so it destructs last. Do the same in yours.

Registering a loader for a type the **engine** already handles is fatal: override semantics for
builtin types stay engine-owned.

## 5. Wire the build

```cmake
veng_add_project(my_game_assets
        PROJECT     project.veng
        OUTPUT_DIR  ${ASSET_DIR}
        MODULE      my_game
        COOK_MODULE my_game_cook)

veng_add_game(my_game
        SOURCES      main.cpp MarkerSet.cpp
        COOK_SOURCES MarkerSetImporter.cpp
        PROJECT      ${my_game_assets_HOST_TARGET})
```

`COOK_SOURCES` emits `libmy_game_cook` beside `libmy_game`, linking `veng::cook_interface`,
`veng::veng`, and `libmy_game` itself — so an importer shares the game's own format headers with
the loader that reads what it writes.

`COOK_MODULE` on `veng_add_project` is the build-order edge and nothing more: `vengc` finds the
module on its own, beside `--module`'s argument. It is named explicitly because the cook is an
`add_custom_command`, whose `DEPENDS` is what actually orders the build.

## 6. Author and load

A pack entry names the type by its registered **name**, never its id:

```json
{ "id": "0x4A433D1EA2E5ACF2", "type": "MarkerSet", "source": "markers/template.markers.json" }
```

Mint that `"id"` with `vengc generate-id --reference <pack.json> --module <lib>`. The `--module`
is required once the pack contains a game-defined type: the reference manifest's type names
resolve only against that module's registrations.

At runtime it is an ordinary typed load — no special path:

```cpp
const auto markers = GetAssetManager().LoadSync<MarkerSet>(MarkerSetId);
```

## What you get, and what you don't

The editor browses game-typed assets under their registered display names and recooks them on
demand (it loads the same cook module per request, so a source edit hot-reloads). Cross-asset
references resolve through `CookContext::Resolve` like any builtin's.

Out of scope for this seam: **editor panels** from game code, **cook-module hot reload**, and
**game-defined loaders for builtin types**.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `unknown type 'X'` at cook | The runtime module was not loaded (neither `--module` nor `--cook-module` was passed), or it never registered the id. The error lists every type that did register. |
| `no importer registered for type 'X'` | The identity registered but the cook module did not load — check it sits beside `--module`'s argument, or pass `--cook-module`. |
| `asset type 'X' already has an importer` (abort) | The cook module registered an importer for a type the engine already cooks. Override semantics for builtin types stay engine-owned. |
| `exports no VengCookModuleAbiVersion` | The library is not a cook module, or `VE_EXPORT_COOK_MODULE_ABI()` is missing. |
| `built against ABI vN, host expects vM` | Stale module — rebuild it against this engine. |
| `no loader registered for asset type X` at runtime | The module registered the id but not a loader factory, or `ApplicationInfo::AssetLoaders` was left null. |
