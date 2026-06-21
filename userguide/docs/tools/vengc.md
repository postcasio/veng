# The vengc cooker

`vengc` is veng's asset cooker — a command-line tool that turns hand-written JSON
into the binary packs the engine loads. You run it ahead of time; the engine itself
never reads source assets.

For the format and authoring side, see [Cooking asset packs](../assets/cooking.md);
this page is the command reference.

## Commands

```sh
vengc cook <pack.json> -o <pack.vengpack>      # cook a manifest into an archive
vengc cook <pack.json> -o <out> --module <lib> # cook a pack containing prefabs
vengc verify <pack.vengpack>                   # re-hash and check integrity
vengc generate-id                              # mint a fresh AssetId
vengc generate-type-id                         # mint a fresh TypeId
```

`generate-id` and `generate-type-id` print **both forms** of the minted id — the
`0x…ULL` hex for C++ literals and the decimal for JSON packs.

## What cooking does

For each asset, the cooker runs the right importer:

- **Textures** — decoded and processed per their `*.tex.json` settings.
- **Meshes** — imported into the engine's vertex layout.
- **Shaders** — compiled from Slang to SPIR-V and reflected. The engine loads only
  the SPIR-V and the reflection, so it never needs Slang.
- **Materials** — checked against their shader's parameters.
- **Prefabs** — checked against your module's component types (see below).

The output is a single `.vengpack` archive with content hashes for `verify`.

## Cooking prefabs

To validate prefabs, the cooker needs to know your component types, so it loads
your game module and reads them — the same way the launcher does. Pass the module
with `--module`:

```sh
vengc cook my_pack.json -o my_pack.vengpack --module libmy_app
```

This is the one time the cooker touches the engine, and only to read types — no
graphics driver is needed, which is what keeps cooking runnable in CI. The build
sets the `--module` flag up for you; see
[Cooking asset packs](../assets/cooking.md).
