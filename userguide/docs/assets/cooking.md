# Cooking asset packs

Assets are cooked **offline** into a binary archive by `vengc`, the asset cooker.
The cooker pulls the heavy import toolchain (stb for images, assimp for meshes,
Slang for shaders, JSON for sources) — none of which is ever linked into the
engine.

## The shape of a pack

An asset pack is a pure **`{ id, type, source }` manifest** — it carries no
per-asset settings. Every asset type has its own per-asset JSON source file that
the manifest entry points at, and *those* files hold the real settings:

| Type | Source file | Carries |
| --- | --- | --- |
| Texture | `*.tex.json` | image path, sampler settings, import options |
| Mesh | `*.mesh.json` | model path, import options, material ids |
| Shader | `*.shader.json` | `.slang` source, entry point, vertex layout |
| Material | `*.vmat.json` | shader ids, domain, typed field values |
| Prefab | `*.prefab.json` | entities, components, field values |

The manifest just lists them:

```json
{
  "assets": [
    { "id": 123456789,  "type": "shader",   "source": "shaders/lit.shader.json" },
    { "id": 987654321,  "type": "material", "source": "materials/brick.vmat.json" },
    { "id": 555555555,  "type": "prefab",   "source": "prefabs/scene.prefab.json" }
  ]
}
```

!!! note "Ids are decimal in JSON, hex in C++"
    Asset packs use decimal ids (JSON has no hex literal). The same id is written
    `0x…ULL` in hand-written C++. `vengc generate-id` prints both forms.

## Cooking & verifying

```sh
vengc cook my_pack.json -o my_pack.vengpack
vengc verify my_pack.vengpack          # check archive integrity
```

`cook` turns the manifest into a `.vengpack` archive. `verify` re-checks the
archive's content hashes and exits non-zero on any mismatch — useful in CI. (The
runtime itself doesn't verify; it trusts its packs.)

## Validation at cook time

The cooker validates as it goes, so authoring mistakes surface as clear errors
rather than runtime surprises. A material is checked against its shader's
parameters — a wrong type or unknown field fails the cook — and a prefab's
components are checked against your module's actual types.

That prefab check is why a pack containing prefabs names its game module:

```sh
vengc cook my_pack.json -o my_pack.vengpack --module libmy_app
```

The cooker loads the module to read its component types, then validates each prefab
against them. The build wires this up for you — `add_asset_pack(... MODULE <lib>)`
makes a prefab pack cook after its module is built, and `veng_add_game` sets it up
for you. See [The vengc cooker](../tools/vengc.md).

## Minting ids

Don't write an id by hand — generate one:

```sh
vengc generate-id        # a fresh AssetId
vengc generate-type-id   # a fresh TypeId, for a component
```

Each command prints the id in both hex (for C++) and decimal (for JSON).
