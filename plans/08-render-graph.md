# 08 — Render graph (automatic barriers)

**Goal:** a *linear* render graph — declared passes with reads/writes,
engine-derived barriers, no reordering — so manual `ImageBarrier`s and
`vk::ImageLayout` disappear from the public API. This completes insulation
step 3 (plan 07's acknowledged hole).

**Dependencies:** 06 + 07 (engine types and the backend boundary exist),
04 (no manual keep-alives to thread through pass execution). 12 (reflection)
is independent but complementary.

## Current state

- `ImageBarrier` (`include/Veng/Renderer/Backend/ImageBarrier.h`) exposes
  `vk::ImageLayout NewLayout`; consumers call
  `CommandBuffer::PipelineBarrier(barrier)` manually at every transition.
- `Utils::GetAccessMask/GetSourceStageMask/GetDestinationStageMask`
  (`Backend/Utils.h:14-16`) derive access/stage masks *from layouts alone* —
  lossy (can't represent "compute wrote this") and repeated at every call site.
- `Image` tracks per-layer/per-mip layouts (`Image.h:55-72`) — this tracking
  is the seed of the graph's state model; it stays, but becomes internal.

## Design

### Public API sketch

```cpp
// Veng/Renderer/RenderGraph.h — engine types only
struct PassAttachment { Ref<ImageView> View; LoadOp Load = LoadOp::Clear;
                        StoreOp Store = StoreOp::Store; ClearValue Clear{}; };

class RenderGraph {
public:
    struct PassBuilder {
        PassBuilder& Color(PassAttachment);               // write as color attachment
        PassBuilder& Depth(PassAttachment);               // write as depth attachment
        PassBuilder& Sample(const Ref<ImageView>&);       // read in fragment/any shader
        PassBuilder& StorageRead(const Ref<ImageView>&);  // compute read
        PassBuilder& StorageWrite(const Ref<ImageView>&); // compute write
        PassBuilder& TransferSrc(const Ref<ImageView>&);
        PassBuilder& TransferDst(const Ref<ImageView>&);
        PassBuilder& LayerCount(u32);   // layers rendered into (default 1)
        PassBuilder& ViewMask(u32);     // multiview mask (default 0)
        PassBuilder& Execute(function<void(CommandBuffer&)>);
    };
    PassBuilder& AddPass(string_view name);          // graphics (dynamic rendering)
    PassBuilder& AddComputePass(string_view name);
    PassBuilder& AddTransferPass(string_view name);
    void Execute(CommandBuffer& cmd);                // linear, declaration order
};
```

- **Linear**: passes execute in declaration order. No culling, no reordering,
  no aliasing — those are later upgrades that don't change this API.
- **All access declarations take `Ref<ImageView>`**, not `Ref<Image>`. The
  view carries its subresource range (`BaseMipLevel`, `MipLevels`,
  `BaseArrayLayer`, `ArrayLayers`), which is the unit of barrier precision.
  The graph resolves `view->GetImage()` for state-map lookup and emits each
  barrier over exactly the affected subresource range. This is required for
  effects that operate on specific mip levels or array layers (bloom
  downsample chain, hierarchical Z, cube shadow maps, screen-space mip reads)
  — declaring a whole `Image` would force over-broad barriers and prevent
  correct hazard detection between passes touching different mips of the
  same image.
- Each declared use carries `(layout, stage mask, access mask)` internally —
  e.g. `StorageWrite` = `(eGeneral, ComputeShader, ShaderWrite)`. `Execute`
  walks passes, diffs each image's current tracked state **per subresource**
  (using `Image`'s existing per-layer/per-mip layout array, extended with
  last-stage/last-access) against the declared view's range, and emits
  exactly the needed `vkCmdPipelineBarrier` — fixing the "can't know a
  compute write happened" hole in `Utils::GetAccessMask`.
- Graphics passes get `BeginRendering`/`EndRendering` called for them from
  their `Color`/`Depth` declarations (folding the existing `RenderingInfo`
  path in `CommandBuffer.h:34-44` into the graph); the `Execute` lambda only
  binds pipelines and draws. Rendering extent is auto-derived from the first
  color attachment view's image at the correct mip level:
  `image_extent >> base_mip_level` — so rendering into mip N produces the
  right extent without caller involvement. `LayerCount` and `ViewMask` on
  `PassBuilder` forward into `RenderingInfo` for array-layer and multiview
  rendering.
- The graph is rebuilt per frame (cheap: it's a vector of pass structs);
  retained/compiled graphs are a later optimization.

### `ImageView` subresource accessors (prerequisite)

`ImageView` currently discards its subresource range after construction.
Add `GetBaseMipLevel()`, `GetMipLevels()`, `GetBaseArrayLayer()`,
`GetArrayLayers()` — store the four values from `ImageViewInfo` on the class.
The graph reads these when building barriers; callers that create views for
specific mip levels or cube faces get correct transitions for free.

### What gets removed/internalized

- `ImageBarrier` struct and `CommandBuffer::PipelineBarrier` leave the public
  API (move into the backend; the graph uses the internal version).
- `Image::SetLayout`/`GetLayout` become internal (backend/`Native` surface) —
  consumers no longer reason about layouts at all.
- `Utils::GetAccessMask`-style layout-only derivation is replaced by the
  declared-use table.
- `CommandBuffer::BeginRendering`/`EndRendering` *can* stay public for
  graph-less quick hacks, but mark them as the escape hatch; the sample app
  should migrate to passes.

### Out-of-graph operations

`Image::Upload`/`Download`, `GenerateMipmaps`, and `Context::ImmediateCommands`
do their own transitions today — they keep doing so internally, but must
update the same tracked state the graph reads, or the graph's diffing is wrong.
Audit all `PipelineBarrier` call sites inside veng for this.

## Migration

The sample app's frame becomes: build graph (one `AddPass` per existing
logical pass, declaring what it already binds), `graph.Execute(cmd)`. Its
manual `ImageBarrier` calls are deleted, not translated — if a barrier doesn't
fall out of a declared use, the declaration is missing, which is exactly the
bug class this removes.

## Acceptance

- No `ImageBarrier`/layout/stage/access type in any public header.
- Validation layers (sync validation enabled, `VK_LAYER_KHRONOS_validation`
  with `VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION`) clean on a
  frame with: graphics → sampled-read → compute write → graphics read chain.
- Declaring a read without a prior write still produces a correct
  `eUndefined`-source transition (first-use case).
