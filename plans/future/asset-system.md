# Asset system — design overview (future)

> **Vision / design sketch, not scheduled.** Detail for [area 1](README.md) —
> the headline future phase. Direction, API surfaces, and decisions, not a firm
> plan; it becomes its own planset (likely several) when taken up. Builds on
> [de-globalized context](README.md#3-de-globalize-the-rendering-context--done-planset-4)
> (done, planset-4), the [threading/task system](threading-task-system.md)
> (area 2, for async loads) and the [bindless rework](bindless-descriptors.md)
> (the material backing). Absorbs **all deferred shader work** (planset-1/12 and
> the shader parts of planset-2).

## The shape of the phase

The roadmap is explicit on ordering: **define the asset API first, concrete asset
types after.** This document follows that — a general layer, then materials /
textures / meshes / shaders on top.

```
                 ┌─────────────────────────────────────────────┐
   consumer ───► │  AssetManager : Load / LoadSync / Get        │  ← the API (first)
                 └───────────────┬─────────────────────────────┘
                                 │ AssetHandle<T>  (typed, refcounted, weak-able)
        ┌────────────────────────┼─────────────────────────────┐
   Texture                    Mesh                          Material        ← types
   (importer + cooked form)   (importer + cooked form)      (shader binary + params)
        │                        │                               │
        └──────────── built on Buffer / Image / Shader / BindlessRegistry ─┘
                                 (today's Renderer resources)
```

Two phases, in order:

1. **Asset API + a thin synchronous slice** — identification, handles, a registry,
   import→cook→load, *synchronous* load. Largely independent; deliberately pull a
   thin slice forward as the "real client" that shapes the
   [threading API](threading-task-system.md) (the cross-cutting concern).
2. **Asset types + async** — texture/mesh/material/shader importers and cooked
   runtime forms, made non-blocking on the task system. Materials are the payoff
   and the most coupled to bindless.

## Part 1 — the asset API (the foundation)

### Identification: `AssetId`

An asset is named by a stable id, not a live path. Recommend a **content/path
hash** (`u64`) computed at cook time, with the source path kept for diagnostics
and hot-reload. Stable ids survive moves and let the cooked cache be content-
addressed.

```cpp
namespace Veng
{
    struct AssetId
    {
        u64 Value = 0;
        [[nodiscard]] bool IsValid() const { return Value != 0; }
        // hashes/compares; constructed from a source path or content hash at cook time
    };
}
```

### Reference: `AssetHandle<T>`

The consumer-facing reference — typed, cheap to copy, refcounted. Loading hands
back a handle; the actual asset lives in the `AssetManager`'s cache. This keeps
the *identity* of an asset separate from its *residency* (loaded / unloaded /
loading), which is what enables streaming and hot-reload later.

```cpp
template <typename T>           // T = Texture, Mesh, Material, Shader, ...
class AssetHandle
{
public:
    [[nodiscard]] bool      IsLoaded() const;   // residency check
    [[nodiscard]] AssetId   Id() const;
    [[nodiscard]] const T*  Get() const;        // nullptr until loaded
    const T* operator->() const;                // asserts loaded (engine contract)
    explicit operator bool() const { return IsLoaded(); }
};
```

- **Refcounted, manager-owned storage.** The handle refers into the manager's
  cache; the *engine resources* it ultimately holds (`Ref<Image>`, `Ref<Buffer>`)
  follow today's [ownership rule](../../docs/ownership.md) unchanged. A handle is
  a high-level reference *to* an asset, not a replacement for `Ref<T>` *inside*
  one. When the last handle drops, the asset becomes evictable (deferred, not
  immediate — see lifetime).
- **Weak variant** for caches/scene graphs that want to reference without pinning
  residency: `WeakAssetHandle<T>` that `Get()`s to `nullptr` if evicted.

### The registry: `AssetManager`

Owned by `Application` and threaded explicitly (the planset-4 lesson — **no
global singleton**). Holds the cache, the importer table, and (for async) a
reference to the [`TaskSystem`](threading-task-system.md).

```cpp
struct AssetManagerInfo
{
    path     AssetRoot;      // source assets
    path     CacheRoot;      // cooked/.veng output
    bool     CookOnDemand = true;   // import a stale/missing cooked form on first load
};

class AssetManager
{
public:
    AssetManager(Context& context, TaskSystem& tasks, const AssetManagerInfo& info);
    ~AssetManager();

    // Default: async. Returns immediately; the handle becomes IsLoaded() later.
    // Decode + GPU upload run on the task system (transfer queue, no frame stall).
    // The unmarked name is the non-stalling one — the obvious call is the right one.
    template <typename T>
    AssetHandle<T> Load(const path& source);

    // Marked blocking variant: cooks if needed, loads, blocks. The thin-slice /
    // test / tool path — deliberately the more verbose spelling.
    template <typename T>
    Result<AssetHandle<T>, AssetLoadError> LoadSync(const path& source);

    // Already-cached lookup by id (no load). nullopt if never loaded.
    template <typename T>
    optional<AssetHandle<T>> Get(AssetId id) const;

    void Reload(AssetId id);          // hot-reload: re-cook + re-upload, swap in place
    void CollectGarbage();            // evict zero-ref assets (deferred past frames-in-flight)

    template <typename T>
    void RegisterImporter(Unique<AssetImporter<T>> importer);
};
```

- **`Load` is async, `LoadSync` blocks** — async is the default so the obvious
  call doesn't stall the frame (mirrors `Buffer/Image::Upload` vs. `UploadSync`,
  see [threading](threading-task-system.md#how-the-upload-path-changes)). `Load`
  returns a not-yet-loaded handle and fills it via a
  [`Task`](threading-task-system.md); the main-thread continuation swaps the
  cooked resource into the cache entry. `LoadSync` is `Load(...).Get()` under the
  hood once threading lands. **Sequencing wrinkle:** the thin real-client slice
  ships *before* threading, so it is synchronous-only — land it as `LoadSync`
  first (the honest name), then add the async `Load` as the default when the task
  system arrives. The marked-verbose sync spelling means that early slice never
  has to be renamed out of the default later.
- **Error kind, not a string.** Asset loading is exactly where callers branch on
  *why* a load failed. The README cross-cutting note and `Result.h` both flag
  promoting the error to a structured type. Introduce it here:

```cpp
enum class AssetError { NotFound, Corrupt, VersionMismatch, MissingDependency, ImportFailed };
struct AssetLoadError { AssetError Kind; string Detail; AssetId Id; };
// Result<AssetHandle<T>, AssetLoadError> for asset paths (Result.h's structured-error promotion).
```

### Import → cook → load

The pipeline every asset type plugs into. **Importing** (parse a source format)
and **cooking** (transform to the engine's runtime-ready form) happen offline /
on-demand; **loading** reads the cooked form and creates GPU resources.

```cpp
template <typename T>
class AssetImporter
{
public:
    virtual ~AssetImporter() = default;

    // Source extensions this importer claims (".png", ".gltf", ".vmat", ...).
    [[nodiscard]] virtual std::span<const string_view> Extensions() const = 0;

    // Offline: source bytes → cooked, serializable blob (no GPU, thread-safe).
    virtual Result<vector<u8>, AssetLoadError> Cook(const path& source) const = 0;

    // Runtime: cooked blob → live asset (creates GPU resources via Context).
    virtual Result<Ref<T>, AssetLoadError> Load(Context& context,
                                                std::span<const u8> cooked) const = 0;
};
```

- **Cook is pure + GPU-free** → it runs on a worker thread safely. **Load** is the
  only part that touches the `Context` (resource `Create`/`Upload`), and on the
  async path its GPU work goes through the transfer queue.
- **Cooked cache** is content-addressed under `CacheRoot` keyed by `AssetId` +
  importer version. A `VersionMismatch` cooked blob triggers a re-cook. This is
  also where **pipeline caching** ([`VkPipelineCache` to disk](README.md)) lives
  once materials multiply.

## Part 2 — the asset types

### `Texture`

The simplest type — exercises the whole pipeline end to end and is the natural
thin-slice. Importer decodes (stb for PNG/JPG now, KTX2/Basis later for cooked GPU
formats + mips); cooked form is "format + extent + mip data"; `Load` is
`Image::Create` + `Upload` (async → transfer queue). Registers into the
[`BindlessRegistry`](bindless-descriptors.md) and the `Texture` carries the
returned `TextureHandle` — a texture *is* a `u32` to the renderer.

```cpp
class Texture
{
public:
    [[nodiscard]] Ref<Image>     GetImage() const;
    [[nodiscard]] Ref<ImageView> GetView() const;
    [[nodiscard]] TextureHandle  GetHandle() const;   // bindless slot
    [[nodiscard]] Format         GetFormat() const;
    [[nodiscard]] uvec2          GetExtent() const;
};
```

### `Mesh`

Importer parses glTF/OBJ; cooked form is interleaved vertex + index buffers in the
engine's vertex layout (validated against `VertexBufferLayout`); `Load` is two
`Buffer::Create`s + uploads. A mesh references its material(s) by `AssetId` —
which is where `MissingDependency` errors come from (the dependency graph).

```cpp
struct SubMesh { u32 IndexOffset, IndexCount; AssetHandle<Material> Material; };

class Mesh
{
public:
    [[nodiscard]] Ref<Buffer>            GetVertexBuffer() const;
    [[nodiscard]] Ref<Buffer>            GetIndexBuffer() const;
    [[nodiscard]] const VertexBufferLayout& GetLayout() const;
    [[nodiscard]] std::span<const SubMesh>  GetSubMeshes() const;
};
```

### `Shader` + offline reflection (the absorbed deferred work)

This is where **planset-1/12 (shader reflection) and the shader parts of
planset-2 land.** Today `Shader::Create` loads SPIR-V at runtime with no knowledge
of its interface. Here, the **importer reflects the shader at cook time** and
emits a serializable `ShaderInterface` beside the binary — reflection moves
**offline**, out of the runtime hot path.

```cpp
struct ShaderInterface                 // serialized at cook time, not derived at runtime
{
    vector<DescriptorBinding> Bindings;       // set, binding, type, count, stage
    vector<PushConstantBlock> PushConstants;  // offset, size, stage  (validated ≤128B, plan 01)
    VertexBufferLayout        VertexInputs;    // derived from the shader's vertex stage
    // set 0 is recognized as engine-provided (bindless) — author never declares it
};

struct ShaderAsset { Ref<Shader> Module; ShaderInterface Interface; };
```

Consequences (all previously deferred, now enabled):
- **Descriptor/pipeline layouts derived from reflection** — `PipelineLayout` /
  `DescriptorSetLayout` built from `ShaderInterface` instead of hand-declared.
- **Name-based binding** — bind "albedo" by name; the interface resolves it to a
  set/binding (or a bindless slot).
- **Vertex layout derived from / validated against the shader** — the missing half
  of planset-2; a mesh's `VertexBufferLayout` is checked against the shader's
  `VertexInputs` at load, a loud mismatch instead of silent UB.

### `Material` — the headline

The roadmap's central claim: **the material becomes the primary rendering
interface, not the shader.** Bind and draw a material; don't juggle pipelines,
descriptor sets, push constants and layouts by hand. A material bundles a shader
(binary) with its uniform/texture data. Two construction paths:

```cpp
class Material
{
public:
    // CONSTRUCTED: reference a shader + explicitly supply params, validated
    // against the shader's ShaderInterface (wrong/missing param = loud error).
    static Result<Ref<Material>, AssetLoadError> Create(
        AssetManager& assets, AssetHandle<ShaderAsset> shader, const MaterialParams& params);

    // LOADED: a cooked .vmat asset from the node-based material editor, carrying
    // shader binary + parameter data already baked.
    //   → AssetManager::Load<Material>("materials/brick.vmat")

    void SetTexture(string_view name, AssetHandle<Texture> tex);   // name-based (reflection)
    void SetParam(string_view name, const vec4& value);

    void Bind(CommandBuffer& cmd) const;     // the new primary draw interface
};
```

With [bindless](bindless-descriptors.md) underneath, a `Material` is **thin** — it
is *handles + a parameter-buffer entry*, not a bundle of descriptor sets:

```
Material = ShaderAsset handle
         + { TextureHandle Albedo, Normal, Orm, ... }   // u32 bindless indices
         + a MaterialData entry in the per-material SSBO array
```

`Bind` therefore does **not** swap descriptor sets per draw (the thing bindless
removes). Set 0 (the global registry) is bound once per frame; `Bind` writes the
material's index for the per-draw push constant (`DrawData{ objectIndex }`) — see
the bindless [per-draw layout](bindless-descriptors.md#per-draw-data-layout-push-constants-vs-buffers--for-a-deferred-renderer).

## Lifetime, hot-reload, GC

- **Eviction is deferred.** When the last `AssetHandle` drops, the asset's GPU
  resources retire through the **existing per-frame deferred-destruction queue**
  (planset-1/04) — the same mechanism, not a second one — so an in-flight frame
  never samples a freed image. `CollectGarbage()` only marks; the retire queue
  reclaims past the frames-in-flight window. The bindless slot release is likewise
  deferred ([bindless lifetime](bindless-descriptors.md#lifetime--sync-the-hard-part)).
- **Hot-reload swaps in place.** `Reload(id)` re-cooks, re-uploads on the async
  path, and swaps the resource behind the handle on the main-thread continuation —
  every holder of the handle sees the new asset with no dangling, because handles
  are indirection into the cache, not direct `Ref`s.
- **Dependency graph.** A mesh→materials→textures/shaders. Loading a mesh loads
  its dependencies; `MissingDependency` is a first-class `AssetError`. The graph
  also orders eviction (don't evict a texture a live material references).

## Usage sketch (the consumer's view)

```cpp
void OnInitialize() override
{
    // Async — no frame stall while brick.gltf + its textures decode/upload.
    m_Brick = m_Assets->Load<Mesh>("meshes/brick-wall.gltf");   // async by default
}

void OnRender() override
{
    if (!m_Brick) return;                       // not yet resident
    m_Bindless->Bind(cmd);                       // global set 0, once per frame
    for (const auto& sub : m_Brick->GetSubMeshes())
    {
        sub.Material->Bind(cmd);                  // material is the draw interface
        cmd.BindVertexBuffer(m_Brick->GetVertexBuffer());
        cmd.BindIndexBuffer(m_Brick->GetIndexBuffer());
        cmd.DrawIndexed(sub.IndexCount, sub.IndexOffset);
    }
}
```

## Touch points (what this phase adds / modifies)

- **New:** `AssetId`, `AssetHandle<T>` / `WeakAssetHandle<T>`, `AssetManager`,
  `AssetImporter<T>`, `AssetLoadError`; `Texture`, `Mesh`, `Material`,
  `ShaderAsset` + `ShaderInterface`.
- **`Result.h`:** structured `AssetLoadError` (the long-flagged promotion).
- **`Shader` / `PipelineLayout` / `DescriptorSetLayout`:** built from reflected
  `ShaderInterface` instead of hand-declared; name-based binding.
- **`VertexBufferLayout`:** derived from / validated against the shader.
- **`Application`:** owns the `AssetManager`; releases all handles in `OnDispose`
  before the context tears down (the ownership contract).
- **Depends on** [`TaskSystem`](threading-task-system.md) (async) and
  [`BindlessRegistry`](bindless-descriptors.md) (material backing) — both future.
- **The editor is the demanding second consumer** (README cross-cutting): build the
  node-based material editor alongside this so it exercises the multi-material/
  mesh/scene surface hello-triangle never will.

## Open decisions

- **`AssetId` source** — content hash vs. path hash vs. a GUID baked into a
  `.meta` sidecar (Unity-style). Content hash is simplest; sidecar GUIDs survive
  content edits but add a file per asset.
- **Cooked format** — a bespoke `.veng` binary vs. an existing container
  (glTF-binary, KTX2 as-is). Bespoke gives one loader and version control; reusing
  formats saves importer work. Likely a thin bespoke wrapper around
  format-native payloads.
- **How much to pull forward** (the README open question) — the API + synchronous
  `LoadSync` as the threading "real client", vs. keeping the whole phase after
  threading. Recommend: land the API + sync `Texture` slice early.
- **Sync slice without bindless** — can the thin slice use today's per-set
  `DescriptorSet` and migrate to bindless later, or does `Material` need bindless
  from day one? Lean: textures/meshes ship pre-bindless; `Material` waits for the
  registry (it is defined *in terms of* handles).
- **Material editor format** (`.vmat`) — schema for the node graph's cooked output;
  designed with the editor, not before it.
- **Reflection toolchain** — SPIRV-Reflect vs. SPIRV-Cross for offline reflection.
  Decide when the shader-asset importer is built.
