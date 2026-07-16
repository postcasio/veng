#include "PointFieldScenePass.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Renderer/Backend/Barrier.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/PointField.h>
#include <Veng/Scene/Camera.h>

#include <algorithm>
#include <cstring>
#include <span>

namespace Veng::Renderer
{
    namespace
    {
        // The point-field shader ids in the engine core pack (auto-mounted by AssetManager).
        constexpr AssetId PointSpriteVertId{0x21CF1A41D3AA8650ULL};
        constexpr AssetId PointSpriteDirectVertId{0x58A90BD4C0DE0376ULL};
        constexpr AssetId PointSpriteFragId{0x5C4D5DCE40951527ULL};
        constexpr AssetId PointSpriteNoFadeFragId{0xD94E97718E481413ULL};
        constexpr AssetId PointAggregateVertId{0x7876DE17593B6A1CULL};
        constexpr AssetId PointAggregateFragId{0xA0E92CDBACECE971ULL};
        constexpr AssetId PointSpriteExpandId{0xF617689FC99BB623ULL};

        // VkDrawIndexedIndirectCommand laid out by hand (20 bytes), the record the sprite indirect
        // draw issues over; the compute pass writes one per field into the field's args buffer.
        struct DrawIndexedIndirectCommand
        {
            u32 IndexCount;
            u32 InstanceCount;
            u32 FirstIndex;
            i32 VertexOffset;
            u32 FirstInstance;
        };

        static_assert(sizeof(DrawIndexedIndirectCommand) == 20,
                      "DrawIndexedIndirectCommand must match VkDrawIndexedIndirectCommand");

        // The expansion compute push block (matches point_sprite_expand.comp PushConstants). All the
        // per-point photometric knobs fold in compute, so the raster path carries none of them.
        struct SpriteExpandPush
        {
            mat4 ViewProj;
            vec4 ClipRight;
            vec4 ClipUp;
            u32 RunCount;
            u32 PointTotal;
            f32 ProjScale;
            f32 MinPixels;
            f32 MaxPixels;
            f32 MaxIntensity;
            f32 Opacity;
        };

        // Four unique vertices per expanded quad — a sprite or an aggregate splat — indexed into
        // two triangles by six indices. The two shared corners hit the post-transform cache.
        constexpr u32 QuadVertexCount = 4;
        constexpr u32 QuadIndexCount = 6;

        // A per-frame ring cap on aggregate splat records (one per aggregated visible cell). One
        // record is 32 bytes, so the ring costs a few MiB across frames-in-flight.
        constexpr u32 MaxAggregateSplats = 1u << 16;

        // Shared push block for both point-field pipelines (matches point_sprite/point_aggregate).
        // The sprite draw reads FirstPoint/PointCount to index one cell's point range and the
        // Min/Max photometric clamps; the aggregate draw ignores them (it reads the splat record
        // buffer). ViewportWidth/Height convert pixel sizes to clip.
        struct PointFieldPush
        {
            u32 DepthTexture;
            u32 Sampler;
            u32 ViewConstantsIndex;
            u32 FirstPoint;
            u32 PointCount;
            f32 MinPixels;
            f32 MaxPixels;
            f32 MaxIntensity;
            f32 ViewportWidth;
            f32 ViewportHeight;
            f32 Opacity;
            f32 Spikes;
        };

        // An occluded point fades to this fraction rather than vanishing (matches the sprite frag).
        constexpr f32 OccludedFade = 0.35f;

        // A conservative superset of any device's minStorageBufferOffsetAlignment (MoltenVK reports
        // 16, desktop up to 256), so a per-frame ring region bound at a multiple of this is always a
        // legal storage-buffer descriptor offset without querying the limit.
        constexpr u64 StorageBufferAlign = 256;

        // Rounds up to the next multiple of a power-of-two alignment.
        u64 AlignUp(const u64 value, const u64 alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        // The aggregate kernel is a separable quadratic B-spline whose support spans this many
        // cull-cell widths. Its integer-offset translates sum to one everywhere (a partition of
        // unity), so the splats of a uniformly dense run of cells sum to a flat field with no
        // imprint of the cull grid — no seams or blobs pulsing at the cell frequency.
        constexpr f32 SplatSupportCells = 3.0f;

        // The sprite kernel's integrated flux as a fraction of its quad area (the (1-r)^2 radial
        // falloff in point_sprite.frag integrates to pi/24 of the quad). The aggregate normalizes
        // its per-pixel color by the same factor so a cell delivers identical integrated light on
        // either LOD path and the sprite<->aggregate transition holds brightness.
        constexpr f32 SpriteKernelFlux = 0.1309f;

        // The point count at which a cell counts as space-filling for splat sizing. The kernel
        // footprint blends from the points' projected bounds (an isolated point stays a sharp
        // dot at its own position) up to the full cell edge as occupancy approaches this. The
        // wide flat-summing kernel is only correct where a cell's centroid has converged on its
        // cell center (the centroid of N uniform points scatters ~cellSize/sqrt(12N), so a
        // low-count cell's tight splat sits at a well-jittered position and cannot read as a
        // lattice row) — so the crossover sits high, keeping the grain of few-star cells and
        // reserving the wide kernels for genuinely crowded cells.
        constexpr f32 SplatFillCount = 32.0f;

        // The sparse end of the footprint blend draws at this fraction of the points' projected
        // bounds. The B-spline kernel's half-peak core spans roughly its whole footprint — half
        // again wider and flatter than the resolved sprite's falloff — so an unscaled sparse
        // splat reads as haze; drawing it tighter than the bounds restores a compact, bright
        // core over the few stars it stands in for.
        constexpr f32 SplatSparseScale = 0.45f;

        // Finalizer of a bit-mixing integer hash (Murmur3's), turning a seed into a well-scrambled
        // u32 so nearby cell coordinates map to unrelated jitters.
        u32 HashU32(u32 x)
        {
            x ^= x >> 16;
            x *= 0x7feb352dU;
            x ^= x >> 15;
            x *= 0x846ca68bU;
            x ^= x >> 16;
            return x;
        }

        // A deterministic offset in [-1, 1]^3 from a cell's integer lattice coordinates. Pure in its
        // input, so an aggregate splat's anchor jitter is stable frame to frame and rebuild to
        // rebuild (no shimmering), and independent per cell so neighbors decorrelate.
        vec3 CellJitter(const i32 cx, const i32 cy, const i32 cz)
        {
            const u32 seed = HashU32(HashU32(static_cast<u32>(cx)) + static_cast<u32>(cy)) +
                             static_cast<u32>(cz);
            const auto unit = [](const u32 h)
            { return static_cast<f32>(h) * (2.0f / 4294967295.0f) - 1.0f; };
            return vec3(unit(HashU32(seed + 1u)), unit(HashU32(seed + 2u)),
                        unit(HashU32(seed + 3u)));
        }

        AssetHandle<Veng::Shader> LoadShader(AssetManager& assets, AssetId id, const char* what)
        {
            const AssetResult<AssetHandle<Veng::Shader>> shader = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(shader.has_value(), "PointFieldScenePass: {} load failed: {}", what,
                      shader.error().Detail);
            return *shader;
        }

        // A cell's projected screen measurements: the point density driving the LOD switch and
        // the point-bounds extent the sparse end of the splat sizing reads.
        struct CellFootprint
        {
            // Visible points divided by the projected screen area in pixels (the area floors at one
            // pixel, so density is at most the cell's point count).
            f32 Density = 0.0f;
            // Largest projected screen dimension in pixels of the cell's bounds.
            f32 Pixels = 0.0f;
        };

        // Estimates a cell's screen footprint from two projections rather than all eight AABB
        // corners: project the bounds center, derive pixels-per-world at that depth from the
        // projection diagonal (projScale/w, the identity the splat sizing already uses), and take
        // screen size as the world extents scaled by that factor. It ignores perspective skew
        // across the cell — an error on the order of cellSize/distance, absorbed by the LOD
        // hysteresis band. A center behind the eye reports zero density (keep resolving — the
        // footprint is unbounded); the projected area floors at one pixel, so a cell collapsed to
        // a point reads its point count as density and aggregates.
        CellFootprint MeasureCellFootprint(const PointField::Cell& cell, const mat4& viewProj,
                                           const f32 projScale)
        {
            const vec4 clipCenter = viewProj * vec4(cell.Bounds.Center(), 1.0f);
            if (clipCenter.w <= 0.0f)
            {
                return CellFootprint{};
            }
            const f32 pixelsPerWorld = projScale / clipCenter.w;
            const vec3 extents = cell.Bounds.Extents();
            const vec2 size =
                vec2(std::max(extents.x, extents.z), extents.y) * (2.0f * pixelsPerWorld);
            const f32 area = std::max(size.x, 1.0f) * std::max(size.y, 1.0f);
            return CellFootprint{
                .Density = static_cast<f32>(cell.PointCount) / area,
                .Pixels = std::max(size.x, size.y),
            };
        }
    }

    PointFieldScenePass::PointFieldScenePass(Context& context, AssetManager& assets,
                                             const vector<const PointField*>* fields,
                                             const Format outputFormat,
                                             const SamplerHandle samplerHandle,
                                             const u32 framesInFlight)
        : m_Context(context), m_Fields(fields), m_FramesInFlight(framesInFlight),
          m_OutputFormat(outputFormat), m_SamplerHandle(samplerHandle)
    {
        const AssetHandle<Veng::Shader> spriteVs =
            LoadShader(assets, PointSpriteVertId, "point-sprite vertex");
        const AssetHandle<Veng::Shader> spriteDirectVs =
            LoadShader(assets, PointSpriteDirectVertId, "point-sprite direct vertex");
        const AssetHandle<Veng::Shader> spriteFs =
            LoadShader(assets, PointSpriteFragId, "point-sprite fragment");
        const AssetHandle<Veng::Shader> spriteNoFadeFs =
            LoadShader(assets, PointSpriteNoFadeFragId, "point-sprite depth-fade-free fragment");
        const AssetHandle<Veng::Shader> aggregateVs =
            LoadShader(assets, PointAggregateVertId, "point-aggregate vertex");
        const AssetHandle<Veng::Shader> aggregateFs =
            LoadShader(assets, PointAggregateFragId, "point-aggregate fragment");

        const PushConstantRange pushRange =
            PushConstantRange::Of<PointFieldPush>(ShaderStage::Vertex | ShaderStage::Fragment);

        // Additive into the linear HDR scene color, ahead of bloom and tonemap: emissive points
        // accumulate as radiance, dense regions roll off through the tone curve instead of
        // clipping, and a bright point picks up bloom like any other HDR emitter.
        const PipelineAttachmentInfo additiveTarget{.Format = m_OutputFormat,
                                                    .Blend = BlendState::Additive()};

        // Set 1 binding 0: the field's resident point SSBO (sprite path) — a closed producer→
        // consumer buffer off bindless, mirroring DebugDraw's record set.
        m_SetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "PointField Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageBuffer,
                                         .Count = 1,
                                         .Stages = ShaderStage::Vertex}},
                       });
        m_Layout = PipelineLayout::Create(m_Context, {
                                                         .Name = "PointField Layout",
                                                         .DescriptorSetLayouts = {m_SetLayout},
                                                         .PushConstantRanges = {pushRange},
                                                     });
        // Each sprite pipeline pairs one vertex stage (compute-path record fetch or direct-path
        // per-point expansion) with one fragment permutation (depth-fade or the trimmed one), all
        // over the shared layout and additive target.
        const auto makeSpritePipeline =
            [&](const char* name, const Ref<ShaderModule>& vs, const Ref<ShaderModule>& fs)
        {
            return GraphicsPipeline::Create(
                m_Context, {
                               .Name = name,
                               .ColorAttachments = {additiveTarget},
                               .PipelineLayout = m_Layout,
                               .ShaderStages =
                                   {
                                       {.Stage = ShaderStage::Vertex, .Module = vs},
                                       {.Stage = ShaderStage::Fragment, .Module = fs},
                                   },
                           });
        };

        // The compute-path sprite pipelines: the vertex stage fetches a compacted record and applies
        // one corner FMA (set 1 binding 0 = the record buffer). The depth-fade-free permutation drops
        // the per-fragment g-buffer depth sample, selected per field by its LOD DepthFade knob.
        m_SpritePipeline = makeSpritePipeline("PointField Sprite Pipeline", spriteVs.Get()->Module,
                                              spriteFs.Get()->Module);
        m_SpriteNoFadePipeline =
            makeSpritePipeline("PointField Sprite No-Fade Pipeline", spriteVs.Get()->Module,
                               spriteNoFadeFs.Get()->Module);
        // The direct-path sprite pipelines: the vertex stage reads the resident points (set 1 binding
        // 0 = the point SSBO) and runs the per-point math per corner. The fallback for a device
        // without the compute path and the A/B verification reference.
        m_SpriteDirectPipeline =
            makeSpritePipeline("PointField Sprite Direct Pipeline", spriteDirectVs.Get()->Module,
                               spriteFs.Get()->Module);
        m_SpriteDirectNoFadePipeline =
            makeSpritePipeline("PointField Sprite Direct No-Fade Pipeline",
                               spriteDirectVs.Get()->Module, spriteNoFadeFs.Get()->Module);
        m_AggregatePipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = "PointField Aggregate Pipeline",
                .ColorAttachments = {additiveTarget},
                .PipelineLayout = m_Layout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = aggregateVs.Get()->Module},
                        {.Stage = ShaderStage::Fragment, .Module = aggregateFs.Get()->Module},
                    },
            });

        // The expansion compute pipeline. It writes a storage buffer (the records) and a storage
        // buffer used as indirect args — both universally available — and reads points + runs, so
        // it needs no gated device feature; a single vkCmdDrawIndexedIndirect with one GPU-written
        // command carries the draw (no drawIndirectCount, no multiDrawIndirect). Its set 1 holds
        // points (0), runs (1), records (2), args (3), and the cursor (4).
        m_ComputeSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "PointField Compute Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 1,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 2,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 3,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 4,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });
        m_ComputeLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "PointField Compute Layout",
                           .DescriptorSetLayouts = {m_ComputeSetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<SpriteExpandPush>(
                               ShaderStage::Compute)},
                       });
        const AssetResult<AssetHandle<Veng::Shader>> expandCs =
            assets.LoadSync<Veng::Shader>(PointSpriteExpandId);
        VE_ASSERT(expandCs.has_value(), "PointFieldScenePass: expansion compute load failed: {}",
                  expandCs.error().Detail);
        m_ComputePipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "PointField Sprite Expand Pipeline",
                .PipelineLayout = m_ComputeLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = expandCs->Get()->Module},
            });
        m_ComputeSupported = m_ComputePipeline != nullptr;
    }

    PointFieldScenePass::~PointFieldScenePass() = default;

    PointFieldScenePass::FieldState& PointFieldScenePass::StateFor(const PointField* const field)
    {
        FieldState& state = m_Fields_State[field];
        if (state.SpriteSets.empty())
        {
            // First time this field is drawn: allocate its own sprite/aggregate sets and the
            // host-mapped aggregate ring (one region per frame-in-flight) so its per-frame writes
            // never collide with a sibling field's draws in the same command buffer.
            state.SpriteSets.reserve(m_FramesInFlight);
            for (u32 frame = 0; frame < m_FramesInFlight; ++frame)
            {
                state.SpriteSets.push_back(
                    DescriptorSet::Create(m_Context, {
                                                         .Name = "PointField Set",
                                                         .Layout = m_SetLayout,
                                                     }));
            }
            state.BoundBuffers.assign(m_FramesInFlight, nullptr);
            state.AggregateRegionStride =
                static_cast<u64>(MaxAggregateSplats) * sizeof(GpuAggregateSplat);
            state.AggregateBuffer = Buffer::Create(
                m_Context, {
                               .Name = "PointField Aggregate Buffer",
                               .Size = state.AggregateRegionStride * m_FramesInFlight,
                               .Usage = BufferUsage::Storage,
                               .HostMapped = true,
                           });

            // One aggregate set per frame-in-flight, each written once to its own ring region; the
            // draw binds the current frame's set rather than rewriting a shared one each frame (a
            // descriptor update on a set a pending command buffer references is a validation error).
            state.AggregateSets.reserve(m_FramesInFlight);
            for (u32 frame = 0; frame < m_FramesInFlight; ++frame)
            {
                Ref<DescriptorSet> set = DescriptorSet::Create(
                    m_Context, {.Name = "PointField Aggregate Set", .Layout = m_SetLayout});
                set->Write(0, state.AggregateBuffer,
                           static_cast<u64>(frame) * state.AggregateRegionStride,
                           state.AggregateRegionStride);
                state.AggregateSets.push_back(std::move(set));
            }
        }
        return state;
    }

    void PointFieldScenePass::EnsureQuadIndexBuffer(const u32 quads)
    {
        if (quads <= m_QuadIndexCapacity)
        {
            return;
        }

        // Build the whole index run on the host (six per quad: two triangles sharing the diagonal,
        // 0,1,2, 1,3,2 offset by 4*q), then hand it to a fresh buffer. Assigning m_QuadIndexBuffer
        // retires the previous one through the per-frame deferred-destruction path, so a draw still
        // referencing the old buffer this frame completes before it is destroyed.
        vector<u32> indices(static_cast<usize>(quads) * QuadIndexCount);
        for (u32 q = 0; q < quads; ++q)
        {
            const u32 base = q * QuadVertexCount;
            u32* out = &indices[static_cast<usize>(q) * QuadIndexCount];
            out[0] = base + 0;
            out[1] = base + 1;
            out[2] = base + 2;
            out[3] = base + 1;
            out[4] = base + 3;
            out[5] = base + 2;
        }

        m_QuadIndexBuffer = Buffer::Create(m_Context, {
                                                          .Name = "PointField Quad Index Buffer",
                                                          .Size = indices.size() * sizeof(u32),
                                                          .Usage = BufferUsage::Index,
                                                      });
        m_QuadIndexBuffer->UploadSync(
            std::span(reinterpret_cast<const u8*>(indices.data()), indices.size() * sizeof(u32)));
        m_QuadIndexCapacity = quads;
    }

    void PointFieldScenePass::EnsureComputeResources(FieldState& state,
                                                     const PointField* const field)
    {
        const u32 pointCount = field->GetPointCount();
        if (state.RecordBuffer != nullptr && state.RecordCapacity >= pointCount)
        {
            return;
        }

        // Capacity is the field's point count: a survivor count never exceeds it, and the run table
        // never exceeds one run per cell. Every ring region is padded to a legal storage-buffer
        // descriptor offset and rings framesInFlight deep like the aggregate buffer, so a frame's
        // writes never collide with a sibling frame's pending dispatch/draw.
        state.RecordRegionStride =
            AlignUp(static_cast<u64>(pointCount) * sizeof(GpuSpriteRecord), StorageBufferAlign);
        state.RecordBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "PointField Record Buffer",
                                          .Size = state.RecordRegionStride * m_FramesInFlight,
                                          .Usage = BufferUsage::Storage,
                                      });

        const u32 cellCount = std::max<u32>(1, static_cast<u32>(field->GetCells().size()));
        state.RunRegionStride =
            AlignUp(static_cast<u64>(cellCount) * sizeof(GpuDrawRun), StorageBufferAlign);
        state.RunBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "PointField Run Buffer",
                                          .Size = state.RunRegionStride * m_FramesInFlight,
                                          .Usage = BufferUsage::Storage,
                                          .HostMapped = true,
                                      });

        state.ArgsRegionStride = AlignUp(sizeof(DrawIndexedIndirectCommand), StorageBufferAlign);
        state.ArgsBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "PointField Sprite Args",
                                          .Size = state.ArgsRegionStride * m_FramesInFlight,
                                          .Usage = BufferUsage::Storage | BufferUsage::Indirect,
                                          .HostMapped = true,
                                      });

        state.CursorRegionStride = AlignUp(sizeof(u32), StorageBufferAlign);
        state.CursorBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "PointField Sprite Cursor",
                                          .Size = state.CursorRegionStride * m_FramesInFlight,
                                          .Usage = BufferUsage::Storage,
                                          .HostMapped = true,
                                      });

        // One compute set and one sprite (record) set per frame-in-flight, each pointing at its own
        // ring region for the buffers' lifetime. The compute set's binding 0 (points) is written per
        // frame in Declare (a rebuilt field re-points it), the rest are region-fixed here.
        state.ComputeSets.clear();
        state.RecordSets.clear();
        state.ComputeSets.reserve(m_FramesInFlight);
        state.RecordSets.reserve(m_FramesInFlight);
        for (u32 frame = 0; frame < m_FramesInFlight; ++frame)
        {
            Ref<DescriptorSet> computeSet = DescriptorSet::Create(
                m_Context, {.Name = "PointField Compute Set", .Layout = m_ComputeSetLayout});
            computeSet->Write(1, state.RunBuffer, static_cast<u64>(frame) * state.RunRegionStride,
                              state.RunRegionStride);
            computeSet->Write(2, state.RecordBuffer,
                              static_cast<u64>(frame) * state.RecordRegionStride,
                              state.RecordRegionStride);
            computeSet->Write(3, state.ArgsBuffer, static_cast<u64>(frame) * state.ArgsRegionStride,
                              state.ArgsRegionStride);
            computeSet->Write(4, state.CursorBuffer,
                              static_cast<u64>(frame) * state.CursorRegionStride,
                              state.CursorRegionStride);
            state.ComputeSets.push_back(std::move(computeSet));

            Ref<DescriptorSet> recordSet = DescriptorSet::Create(
                m_Context, {.Name = "PointField Record Set", .Layout = m_SetLayout});
            recordSet->Write(0, state.RecordBuffer,
                             static_cast<u64>(frame) * state.RecordRegionStride,
                             state.RecordRegionStride);
            state.RecordSets.push_back(std::move(recordSet));
        }

        state.RecordCapacity = pointCount;
    }

    void PointFieldScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const ResourceId outputId = io.Hdr;
        const ResourceId depthId = io.GBufferDepth;
        m_DepthTextureIndex = io.DepthHandle.Index;
        m_SamplerIndex = m_SamplerHandle.Index;

        // The expansion compute pass runs ahead of the draw (declaration order is execution order),
        // so its per-point work + compaction complete and its manual compute-write → vertex/indirect
        // read barrier is in place before the sprites draw. It records outside any render pass.
        graph.AddComputePass("PointField Expand")
            .Execute(
                [this](PassContext& inner)
                {
                    const ScenePassContext ctx = Wrap(inner);
                    WalkFields(ctx.Cmd(), ctx);
                });

        graph.AddPass("PointField")
            .Color({
                .Resource = outputId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            })
            .Sample(depthId)
            .Execute(
                [this](PassContext& inner)
                {
                    const ScenePassContext ctx = Wrap(inner);
                    DrawFields(ctx.Cmd(), ctx);
                });
    }

    void PointFieldScenePass::WalkFields(CommandBuffer& cmd, const ScenePassContext& ctx)
    {
        // Accumulate the walk-side counters into a fresh block; DrawFields adds the submission
        // counters and publishes to m_Stats, so a reader never sees a partial frame and a frame that
        // draws no field reads back zeroed.
        m_PendingStats = PointFieldStats{};

        if (m_Fields == nullptr || m_Fields->empty())
        {
            m_Stats = PointFieldStats{};
            return;
        }

        const SceneView& view = ctx.View();
        const mat4 viewProj = view.Camera.ViewProjection();
        const Frustum frustum = Frustum::FromViewProjection(viewProj);
        const u32 region = m_Context.GetCurrentFrameInFlight();
        const uvec2 renderExtent = view.RenderExtent;
        // Pixels per world unit at depth w is |Proj[1][1]|*H/(2w) — the projection is
        // Y-flipped for Vulkan clip space, so the diagonal is negative.
        const f32 projScale =
            std::abs(view.Camera.Projection()[1][1]) * static_cast<f32>(renderExtent.y) * 0.5f;

        // The clip-space corner basis: the projected camera right/up axes (the View matrix's first
        // two rows). The expansion shader offsets each corner by these, so the CPU derives them once.
        const mat4 viewMatrix = view.Camera.View();
        const vec3 right(viewMatrix[0][0], viewMatrix[0][1], viewMatrix[0][2]);
        const vec3 up(viewMatrix[1][0], viewMatrix[1][1], viewMatrix[1][2]);
        const vec4 clipRight = viewProj * vec4(right, 0.0f);
        const vec4 clipUp = viewProj * vec4(up, 0.0f);

        for (const PointField* field : *m_Fields)
        {
            if (field == nullptr || field->GetPointCount() == 0)
            {
                continue;
            }
            FieldState& state = StateFor(field);
            state.Seen = true;
            state.DrewCompute = false;
            state.SpritePointTotal = 0;

            const PointFieldLod& lod = field->GetLod();
            // A fully faded field contributes nothing (Opacity multiplies every sprite
            // and splat), so skip its whole walk and draw — a consumer fading a layer
            // out pays nothing for keeping it resident.
            if (lod.Opacity <= 0.0f)
            {
                continue;
            }
            ++m_PendingStats.Fields;

            // Re-point this frame's sprite set at the resident buffer only when it
            // changed (a rebuilt field), never per frame for a static field. Only the
            // current frame's set is ever written — its fence has been waited, so no
            // pending command buffer references it.
            if (state.BoundBuffers[region] != field->GetPointBuffer().get())
            {
                state.SpriteSets[region]->Write(0, field->GetPointBuffer(), 0,
                                                field->GetPointBuffer()->GetSize());
                state.BoundBuffers[region] = field->GetPointBuffer().get();
            }

            // Size the per-cell aggregating latch to this field's cells (a rebuilt field
            // with a new cell count starts resolving and settles on its own densities).
            const vector<PointField::Cell>& cells = field->GetCells();
            if (state.Aggregating.size() != cells.size())
            {
                state.Aggregating.assign(cells.size(), false);
            }

            // Partition the frustum-surviving cells into resolved (sprite) and aggregate
            // (splat) sets by their on-screen density, hysteretic around the threshold: a
            // cell flips to aggregate only past the high gate and back only below the low
            // gate, so a cell hovering at the boundary does not pop frame to frame.
            const f32 lowGate = lod.AggregateThreshold * (1.0f - lod.Hysteresis);
            const f32 highGate = lod.AggregateThreshold * (1.0f + lod.Hysteresis);

            // Fixed-outcome fast paths. A cell's on-screen density is at most its point
            // count (the footprint area floors at one pixel), so if the low gate exceeds
            // every cell's point count no cell can ever reach the high gate and none
            // aggregates — the density measure is dead work, skipped entirely. Symmetric
            // at the other end: a zero high gate aggregates every cell (density is never
            // negative), so the measure only decides which cells resolve versus aggregate
            // when the threshold is a real, reachable bound.
            const bool alwaysAggregate = highGate <= 0.0f;
            u32 maxCellPoints = 0;
            for (const PointField::Cell& cell : cells)
            {
                maxCellPoints = std::max(maxCellPoints, cell.PointCount);
            }
            const bool neverAggregate =
                !alwaysAggregate && lowGate > static_cast<f32>(maxCellPoints);

            // The frame scratch reuses its capacity across frames rather than
            // reallocating a function-local vector per field per frame.
            vector<DrawRun>& runs = state.Runs;
            vector<GpuAggregateSplat>& splats = state.Splats;
            runs.clear();
            splats.clear();
            m_PendingStats.CellsTotal += static_cast<u32>(cells.size());
            for (u32 c = 0; c < cells.size(); ++c)
            {
                const PointField::Cell& cell = cells[c];
                if (cell.PointCount == 0 || !Intersects(frustum, cell.Bounds))
                {
                    continue;
                }
                ++m_PendingStats.CellsInFrustum;

                // Resolve the LOD outcome. The never-aggregate path skips the density
                // measure outright; otherwise the cheaper center-plus-extent estimate
                // feeds the hysteresis latch. Always-aggregate still measures because the
                // splat sizing reads the footprint's Pixels term.
                bool aggregate = false;
                CellFootprint footprint;
                if (neverAggregate)
                {
                    state.Aggregating[c] = false;
                }
                else
                {
                    footprint = MeasureCellFootprint(cell, viewProj, projScale);
                    ++m_PendingStats.CellsMeasured;
                    aggregate = state.Aggregating[c];
                    if (footprint.Density >= highGate)
                    {
                        aggregate = true;
                    }
                    else if (footprint.Density < lowGate)
                    {
                        aggregate = false;
                    }
                    state.Aggregating[c] = aggregate;
                }

                if (aggregate && splats.size() < MaxAggregateSplats)
                {
                    // The splat anchors at the cell's point centroid, and its kernel
                    // footprint follows the cell's occupancy: a sparse cell sizes
                    // from the points' projected bounds, so an isolated star reads
                    // as a sharp dot at its own position, while a filled cell grows
                    // to the projected cell edge, where the wide kernel is what makes
                    // a dense run sum flat (a filled cell's centroid converges on the
                    // cell center, so the kernel's partition-of-unity property
                    // holds). The color spreads the cell's summed flux over the
                    // kernel footprint (times the shared sprite-kernel flux
                    // fraction), so the splat delivers the same integrated light as
                    // the cell's resolved sprites.
                    const f32 cellSize = field->GetCellSize();
                    const bool continuous = lod.Style == PointFieldLod::AggregateStyle::Continuous;

                    // A filled Cloud cell's centroid converges on its cell center,
                    // seating the splat on the cull lattice; where the splat is drawn
                    // narrower than the cell, its coverage dips along the shared cell
                    // boundaries and the edge-on boundary planes read as a grid. Offset
                    // the anchor off the lattice by a per-cell hash so those dips
                    // scatter into grain rather than a coherent imprint. Continuous
                    // tiles the full cell, so it is left unjittered.
                    vec3 anchor = cell.Centroid;
                    if (!continuous && lod.AnchorJitter > 0.0f)
                    {
                        const vec3 lattice = glm::floor(cell.Centroid / cellSize);
                        anchor +=
                            CellJitter(static_cast<i32>(lattice.x), static_cast<i32>(lattice.y),
                                       static_cast<i32>(lattice.z)) *
                            (lod.AnchorJitter * cellSize);
                    }

                    const vec4 clipCenter = viewProj * vec4(anchor, 1.0f);
                    if (clipCenter.w <= 0.0f)
                    {
                        continue;
                    }
                    const f32 pixelsPerWorld = projScale / clipCenter.w;
                    const f32 cellPixels = cellSize * pixelsPerWorld;

                    // The kernel footprint. Cloud sizes it from occupancy (a sparse
                    // cell tightens to its points' bounds, a filled cell grows to the
                    // cell edge). Continuous sizes it from the centroid's centering
                    // within its grid cell — reconstructed from the centroid at the
                    // bucketer's world-origin-0 alignment — so a centered (on-lattice)
                    // centroid draws the full cell width its neighbors sum flat against,
                    // and an off-center (sparse) one tightens to its points' bounds.
                    f32 kernelPixels = 0.0f;
                    if (continuous)
                    {
                        const vec3 gridCenter =
                            (glm::floor(cell.Centroid / cellSize) + 0.5f) * cellSize;
                        const vec3 offset = glm::abs(cell.Centroid - gridCenter);
                        const f32 maxOffset = std::max({offset.x, offset.y, offset.z});
                        const f32 centered =
                            1.0f - std::clamp(2.0f * maxOffset / cellSize, 0.0f, 1.0f);
                        kernelPixels = glm::mix(footprint.Pixels, cellPixels, centered);
                    }
                    else
                    {
                        const f32 fill =
                            std::min(static_cast<f32>(cell.PointCount) / SplatFillCount, 1.0f);
                        kernelPixels =
                            glm::mix(footprint.Pixels * SplatSparseScale, cellPixels, fill);
                    }
                    // The clamp keeps a subpixel kernel drawable (the floor puts the
                    // whole quad at MinPixels) and bounds a large near cell's overdraw.
                    const f32 drawnPixels = std::clamp(
                        kernelPixels, lod.MinPixels / SplatSupportCells, lod.AggregateSplatPixels);
                    // Cloud normalizes by the larger of drawn/kernel — flux-conserving
                    // across the clamp, so a receding cell dims like a real emitter.
                    // Continuous normalizes by the kernel footprint itself (never the
                    // pixel floor), holding surface brightness constant as the camera
                    // recedes so an extended field shrinks and tiles rather than fading.
                    const f32 normalizePixels = continuous ? std::max(kernelPixels, 1e-3f)
                                                           : std::max(drawnPixels, kernelPixels);
                    const f32 normalize = SpriteKernelFlux * (pixelsPerWorld * pixelsPerWorld) /
                                          (normalizePixels * normalizePixels);
                    vec3 color = cell.SummedFlux * lod.AggregateIntensity * normalize;
                    // MaxIntensity bounds the splat's peak brightness exactly as it
                    // bounds a clamped sprite's gain, so a dense cell saturates to
                    // the same ceiling on either LOD path instead of blooming into
                    // an unbounded HDR blob.
                    const f32 peak = std::max({color.r, color.g, color.b});
                    if (peak > lod.MaxIntensity && peak > 0.0f)
                    {
                        color *= lod.MaxIntensity / peak;
                    }
                    // Opacity is a display fade applied last, after the flux
                    // normalization and the intensity ceiling.
                    color *= lod.Opacity;
                    splats.push_back(GpuAggregateSplat{
                        .CenterSize = vec4(anchor, drawnPixels * SplatSupportCells),
                        .Color = vec4(color, 0.0f),
                    });
                }
                else if (!aggregate)
                {
                    // Merge into the open run when this cell continues the buffer range;
                    // else close it and open a new one. Adjacent resolved cells tile the
                    // buffer in ascending FirstPoint order (a Bucket postcondition), so a
                    // run breaks only where an intervening cell was culled or aggregated.
                    if (!runs.empty() &&
                        runs.back().FirstPoint + runs.back().PointCount == cell.FirstPoint)
                    {
                        runs.back().PointCount += cell.PointCount;
                    }
                    else
                    {
                        runs.push_back(
                            DrawRun{.FirstPoint = cell.FirstPoint, .PointCount = cell.PointCount});
                    }
                }
            }

            // Sum the resolved run points (the pre-compaction total); this is what the direct path
            // draws and the compute dispatch covers.
            u64 runPointTotal = 0;
            for (const DrawRun& run : runs)
            {
                runPointTotal += run.PointCount;
            }
            state.SpritePointTotal = runPointTotal;

            // Stage this frame's aggregate records into the field's ring region (the draw reads
            // them in DrawFields). The upload rings by frame-in-flight already.
            if (!splats.empty())
            {
                auto* base = static_cast<u8*>(state.AggregateBuffer->GetMappedData()) +
                             static_cast<u64>(region) * state.AggregateRegionStride;
                std::memcpy(base, splats.data(), splats.size() * sizeof(GpuAggregateSplat));
            }

            if (runs.empty())
            {
                continue;
            }

            // Select the resolved-sprite path per field. The compute expansion path needs the
            // pipeline and a record ring sized to the field's point count; the first frame after a
            // rebuild (or a forced-direct A/B run, or an unsupported device) falls back to direct.
            const bool wantCompute = m_ComputeSupported && !m_ForceDirect;
            if (wantCompute)
            {
                EnsureComputeResources(state, field);
            }
            const bool useCompute = wantCompute && state.RecordBuffer != nullptr &&
                                    state.RecordCapacity >= field->GetPointCount();
            if (!useCompute)
            {
                continue; // DrawFields draws the direct path from state.Runs.
            }

            // Upload the run table with an exclusive point-count prefix sum, so a compute thread maps
            // to its point by a prefix search. Points are contiguous per run, so the whole dispatch
            // covers [0, runPointTotal).
            auto* runData =
                reinterpret_cast<GpuDrawRun*>(static_cast<u8*>(state.RunBuffer->GetMappedData()) +
                                              static_cast<u64>(region) * state.RunRegionStride);
            u32 prefix = 0;
            for (usize r = 0; r < runs.size(); ++r)
            {
                runData[r] = GpuDrawRun{.FirstPoint = runs[r].FirstPoint,
                                        .Count = runs[r].PointCount,
                                        .PointPrefix = prefix,
                                        .Pad = 0};
                prefix += runs[r].PointCount;
            }

            // Preset the indirect command's fixed fields (indexCount 0 — the compute finalizes it to
            // 6·survivors) and zero the append cursor before the dispatch.
            auto* args = reinterpret_cast<DrawIndexedIndirectCommand*>(
                static_cast<u8*>(state.ArgsBuffer->GetMappedData()) +
                static_cast<u64>(region) * state.ArgsRegionStride);
            auto* cursor =
                reinterpret_cast<u32*>(static_cast<u8*>(state.CursorBuffer->GetMappedData()) +
                                       static_cast<u64>(region) * state.CursorRegionStride);

            // Read this region's prior survivor count (the GPU wrote it when this region last drew,
            // frames-in-flight ago) before overwriting the command — the one-frame-late CompactedPoints
            // stat, mirroring the mesh cull's survivor readback, never gating this frame's draw.
            state.PriorSurvivors = args->IndexCount / QuadIndexCount;

            *args = DrawIndexedIndirectCommand{.IndexCount = 0,
                                               .InstanceCount = 1,
                                               .FirstIndex = 0,
                                               .VertexOffset = 0,
                                               .FirstInstance = 0};
            *cursor = 0;

            // Re-point the compute set's points binding (binding 0) at the resident buffer, only when
            // it changed — the run/record/args/cursor bindings are region-fixed at allocation.
            state.ComputeSets[region]->Write(0, field->GetPointBuffer(), 0,
                                             field->GetPointBuffer()->GetSize());

            const SpriteExpandPush push{
                .ViewProj = viewProj,
                .ClipRight = clipRight,
                .ClipUp = clipUp,
                .RunCount = static_cast<u32>(runs.size()),
                .PointTotal = static_cast<u32>(runPointTotal),
                .ProjScale = projScale,
                .MinPixels = lod.MinPixels,
                .MaxPixels = lod.MaxPixels,
                .MaxIntensity = lod.MaxIntensity,
                .Opacity = lod.Opacity,
            };

            cmd.BindPipeline(m_ComputePipeline);
            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                .Sets = {state.ComputeSets[region]},
                .FirstSet = 1, // set 0 reserved for the bindless registry, unused by this dispatch
                .PipelineBindPoint = PipelineBindPoint::Compute,
            });
            cmd.PushConstants(push);
            cmd.Dispatch((static_cast<u32>(runPointTotal) + 63) / 64, 1, 1);

            // Manual compute-write → vertex-read + indirect-read barrier: the sprite draw reads the
            // records as vertex-stage storage and the args as indirect input. The graph does not
            // track these buffers (they are bound off-graph, per field), so order them here; both
            // passes record on the one queue in submission order, so this single buffer barrier
            // reaches the later graphics reads.
            Backend::TransitionBuffer(
                cmd, *state.RecordBuffer, vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderWrite, vk::PipelineStageFlagBits::eVertexShader,
                vk::AccessFlagBits::eShaderRead);
            Backend::TransitionBuffer(
                cmd, *state.ArgsBuffer, vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderWrite, vk::PipelineStageFlagBits::eDrawIndirect,
                vk::AccessFlagBits::eIndirectCommandRead);

            state.DrewCompute = true;
        }
    }

    void PointFieldScenePass::DrawFields(CommandBuffer& cmd, const ScenePassContext& ctx)
    {
        PointFieldStats stats = m_PendingStats;

        if (m_Fields == nullptr || m_Fields->empty())
        {
            m_Stats = stats;
            return;
        }

        const SceneView& view = ctx.View();
        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const u32 region = m_Context.GetCurrentFrameInFlight();
        const uvec2 renderExtent = view.RenderExtent;

        for (const PointField* field : *m_Fields)
        {
            if (field == nullptr || field->GetPointCount() == 0)
            {
                continue;
            }
            FieldState& state = StateFor(field);
            const PointFieldLod& lod = field->GetLod();
            if (lod.Opacity <= 0.0f)
            {
                continue;
            }

            const vector<DrawRun>& runs = state.Runs;
            const vector<GpuAggregateSplat>& splats = state.Splats;
            if (runs.empty() && splats.empty())
            {
                continue;
            }

            cmd.SetViewport({0, 0}, renderExtent);
            cmd.SetScissor({0, 0}, renderExtent);

            // On the compute path the photometric knobs already folded in compute, so the raster
            // push zeroes Opacity (the fragment re-multiplies by it) and carries only the bindless
            // slots. The direct path carries the real knobs its vertex stage reads.
            PointFieldPush push{
                .DepthTexture = m_DepthTextureIndex,
                .Sampler = m_SamplerIndex,
                .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                .MinPixels = lod.MinPixels,
                .MaxPixels = lod.MaxPixels,
                .MaxIntensity = lod.MaxIntensity,
                .ViewportWidth = static_cast<f32>(renderExtent.x),
                .ViewportHeight = static_cast<f32>(renderExtent.y),
                .Opacity = lod.Opacity,
                .Spikes = lod.SpriteSpikes,
            };

            if (!runs.empty() && state.DrewCompute)
            {
                // Compute path: one indirect draw over the compacted survivors. The compute wrote
                // indexCount = 6·survivors; the record buffer holds the survivors from slot 0. The
                // sprite vertex stage reads the record set; Opacity folded in compute, so push 1.
                EnsureQuadIndexBuffer(field->GetPointCount());

                PointFieldPush computePush = push;
                computePush.Opacity = 1.0f;

                // Select the fragment permutation by the field's DepthFade knob: the depth-fade
                // pipeline (default) samples the g-buffer depth; the trimmed one drops it.
                cmd.BindPipeline(lod.DepthFade ? m_SpritePipeline : m_SpriteNoFadePipeline);
                registry.Bind(cmd);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {state.RecordSets[region]},
                    .FirstSet = 1,
                    .PipelineBindPoint = PipelineBindPoint::Graphics,
                });
                cmd.BindIndexBuffer(m_QuadIndexBuffer);
                cmd.PushConstants(computePush);
                cmd.DrawIndexedIndirect(state.ArgsBuffer,
                                        static_cast<u64>(region) * state.ArgsRegionStride, 1,
                                        static_cast<u32>(sizeof(DrawIndexedIndirectCommand)));
                ++stats.ResolvedDraws;
                stats.SpritePoints += state.SpritePointTotal;
                stats.DrawSource = SpriteDrawSource::Compute;

                // CompactedPoints is this frame's submitted total minus the survivor count WalkFields
                // read from this region's prior draw (one frame late, like the mesh cull's survivor
                // count) — for a steady view the two frames' totals match.
                if (state.SpritePointTotal >= state.PriorSurvivors)
                {
                    stats.CompactedPoints += state.SpritePointTotal - state.PriorSurvivors;
                }
            }
            else if (!runs.empty())
            {
                // Direct path: one indexed draw per contiguous run, the vertex stage running the
                // per-point math per corner and reading the resident point buffer.
                u32 maxRunPoints = 0;
                for (const DrawRun& run : runs)
                {
                    maxRunPoints = std::max(maxRunPoints, run.PointCount);
                }
                EnsureQuadIndexBuffer(maxRunPoints);

                cmd.BindPipeline(lod.DepthFade ? m_SpriteDirectPipeline
                                               : m_SpriteDirectNoFadePipeline);
                registry.Bind(cmd);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {state.SpriteSets[region]},
                    .FirstSet = 1,
                    .PipelineBindPoint = PipelineBindPoint::Graphics,
                });
                cmd.BindIndexBuffer(m_QuadIndexBuffer);
                for (const DrawRun& run : runs)
                {
                    push.FirstPoint = run.FirstPoint;
                    push.PointCount = run.PointCount;
                    cmd.PushConstants(push);
                    cmd.DrawIndexed(QuadIndexCount * run.PointCount, 1, 0, 0, 0);
                    ++stats.ResolvedDraws;
                    stats.SpritePoints += run.PointCount;
                }
                if (stats.DrawSource != SpriteDrawSource::Compute)
                {
                    stats.DrawSource = SpriteDrawSource::Direct;
                }
            }

            // Aggregate splats: draw this frame's per-cell records (staged in WalkFields) as
            // additive quads in one call.
            if (!splats.empty())
            {
                const auto splatCount = static_cast<u32>(splats.size());
                EnsureQuadIndexBuffer(splatCount);

                cmd.BindPipeline(m_AggregatePipeline);
                registry.Bind(cmd);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {state.AggregateSets[region]},
                    .FirstSet = 1,
                    .PipelineBindPoint = PipelineBindPoint::Graphics,
                });
                cmd.BindIndexBuffer(m_QuadIndexBuffer);
                push.FirstPoint = 0;
                push.PointCount = splatCount;
                cmd.PushConstants(push);
                cmd.DrawIndexed(QuadIndexCount * splatCount, 1, 0, 0, 0);
                stats.Splats += splatCount;
            }
        }

        // Prune cached state for fields no longer resolved (freeing their sets/rings);
        // clear Seen for the survivors ahead of the next Execute.
        for (auto it = m_Fields_State.begin(); it != m_Fields_State.end();)
        {
            if (!it->second.Seen)
            {
                it = m_Fields_State.erase(it);
            }
            else
            {
                it->second.Seen = false;
                ++it;
            }
        }

        m_Stats = stats;
    }
}
