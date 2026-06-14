# Plan 03 — `cooker` lib + `vengc` CLI + JSON pack parsing

**Goal:** stand up the cooking tool. `libveng_cook` parses a JSON **asset pack**,
dispatches each entry to a per-type **importer** registered in a table, and writes
a `.vengpack` archive via `assetformat`'s `ArchiveWriter`. The `vengc` CLI exposes
`vengc cook <pack.json> -o <out.vengpack>`. This plan ships the **skeleton**: the
JSON schema, the importer-registry mechanism, and an end-to-end path that produces
a valid archive from a pack whose entries use a trivial `raw` importer — concrete
texture/mesh/shader/material importers arrive in 05–08.

## Why this is its own plan

The cooker's *shell* — argument handling, JSON parsing, the importer dispatch
table, error reporting, archive emission — is orthogonal to any one asset type.
Building it once, proven against a `raw` passthrough importer, means 05/06/07/08
each add *only* a `Cook()` body and a registry line, not tool plumbing.

## The pack JSON schema

```jsonc
{
  "version": 1,
  "assets": [
    { "id": 1001, "type": "texture",  "source": "textures/brick.png" },
    { "id": 42,   "type": "raw",      "source": "data/blob.bin" }
    // material/mesh/shader entries gain type-specific fields in their plans
  ]
}
```

- **`id`** is the author-owned `u64` `AssetId` (random or hand-assigned; 0 is
  rejected). Duplicate ids in one pack → a hard cook error.
- **`type`** maps to `AssetType`; an unknown type or one with no registered
  importer → a cook error naming the entry.
- **`source`** is a path **relative to the pack file's directory** (so packs are
  relocatable). Material/inline-shader entries may instead carry inline data
  (plan 08) — `source` vs inline is per-importer.
- A `vengc mint-id` helper (or just documented "any non-zero u64") covers id
  minting for hand-written packs; the editor mints random ids later.

## The importer interface (cooker-side)

```cpp
namespace Veng::Cook
{
    struct CookContext { path PackDir; /* resolve sources; later: dep id lookups */ };

    class AssetImporter
    {
    public:
        virtual ~AssetImporter() = default;
        [[nodiscard]] virtual AssetType Type() const = 0;
        // source-relative JSON entry → cooked blob bytes (Vulkan-free, offline).
        virtual Result<vector<u8>> Cook(const CookContext&, const json& entry) const = 0;
    };

    class Cooker   // owns the importer table + drives a pack → archive
    {
    public:
        void Register(Unique<AssetImporter>);
        Result<void> CookPack(const path& packJson, const path& outArchive) const;
    };
}
```

`CookPack`: parse JSON → validate (version, unique non-zero ids, known types) →
for each entry call the registered importer's `Cook` → `ArchiveWriter.Add(id,
type, blob)` → `Write(out)`. All `Result<>`, no exceptions.

## Work

1. **CMake** — fill `cooker/CMakeLists.txt`: `libveng_cook` (links
   `assetformat`, `fmt`, nlohmann/json) + a `vengc` executable in `cooker/tool/`.
   `FetchContent` nlohmann/json with a pinned tag (cooker-only — never linked by
   the engine). Built only when `PROJECT_IS_TOP_LEVEL` (a new
   `VENG_BUILD_TOOLS` toggle, default ON top-level).
2. **JSON layer** — schema parsing + validation with clear, located errors
   ("pack `sample.json`: asset id 1001 duplicated", "unknown type 'txture'").
3. **Importer registry + `CookPack`** as above.
4. **A `raw` importer** — reads `source` bytes verbatim → blob (`AssetType::Raw`).
   The end-to-end proof with zero type-specific logic.
5. **`vengc` CLI** — `vengc cook <pack.json> [-o out.vengpack]`; nonzero exit +
   stderr message on any cook error; `-o` defaults to the pack name with
   `.vengpack`.
6. **Test** — a `tests/cooker` doctest: cook a fixture pack with a couple of `raw`
   entries to a temp archive, then open it with `assetformat`'s `ArchiveReader` and
   assert the ids/types/bytes match. Round-trips the *whole* tool.

## Dependencies

Plans 01 (the subdir) and 02 (`ArchiveWriter`, `AssetType`, `AssetId`). Blocks the
cook side of 05/06/07/08.

## Acceptance

- Clean build; `vengc` builds and `vengc cook tests/.../raw_pack.json -o /tmp/x.vengpack`
  exits 0 and produces an archive the `assetformat` reader accepts.
- `ctest` green incl. the cooker round-trip test.
- The engine and its consumers link **none** of nlohmann/json or `libveng_cook`
  (grep the engine target's link deps).

## Notes

- Keep `CookContext` minimal now; it grows a dependency-id resolver when materials
  reference textures by id (plan 08) — but cross-asset *resolution* is by raw id,
  so the cooker doesn't need the referenced asset present, only its id.
- The CLI stays thin — all logic in `libveng_cook` so it's unit-testable without
  spawning a process.
- The cook side of each concrete type is well-scoped subagent work once this shell
  exists (`model: sonnet`): each is "implement one `AssetImporter::Cook` + register
  it + a fixture".
