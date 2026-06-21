# Assets

veng's assets are cooked offline and loaded at runtime. You author them as JSON,
cook them into a single binary `.vengpack` archive with `vengc`, and at runtime
mount the archive and load assets by id.

The runtime never parses a source asset — it loads the binary archive and nothing
else. That's why all the import tooling (stb, assimp, Slang, JSON) lives in the
cooker and never ends up in the engine.

- **[Cooking asset packs](cooking.md)** — the pack manifest, the per-asset source
  files, and the `vengc cook` and `verify` commands.
- **[Loading at runtime](loading.md)** — `AssetId`, loading async or blocking,
  asset handles, and the dependencies that come with an asset.
