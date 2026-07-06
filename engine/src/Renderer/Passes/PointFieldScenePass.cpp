#include "PointFieldScenePass.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/PointField.h>
#include <Veng/Scene/Camera.h>

#include <algorithm>
#include <cstring>

namespace Veng::Renderer
{
    namespace
    {
        // The point-field shader ids in the engine core pack (auto-mounted by AssetManager).
        constexpr AssetId PointSpriteVertId{0x21CF1A41D3AA8650ULL};
        constexpr AssetId PointSpriteFragId{0x5C4D5DCE40951527ULL};
        constexpr AssetId PointAggregateVertId{0x7876DE17593B6A1CULL};
        constexpr AssetId PointAggregateFragId{0xA0E92CDBACECE971ULL};

        // Six vertices (two triangles) per expanded quad — a sprite or an aggregate splat.
        constexpr u32 QuadVertexCount = 6;

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
        };

        // One GPU aggregate splat record (std430): world center + pixel size, then the cell's
        // flux-normalized per-pixel color (the summed flux spread over the splat's pixel area).
        struct GpuAggregateSplat
        {
            vec4 CenterSize; // xyz world point centroid, w quad size in pixels (kernel support)
            vec4 Color;      // flux-normalized per-pixel color (rgb), HDR; a unused
        };

        // An occluded point fades to this fraction rather than vanishing (matches the sprite frag).
        constexpr f32 OccludedFade = 0.35f;

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
            // Visible points divided by the projected screen area in pixels (INFINITY for a
            // degenerate zero-area footprint so a cell collapsed to a point always aggregates).
            f32 Density = 0.0f;
            // Largest projected screen dimension in pixels of the points' bounds.
            f32 Pixels = 0.0f;
        };

        // Projects the cell's world AABB corners to clip and measures the bounding rect. A corner
        // behind the eye reports zero density (keep resolving — the footprint is unbounded).
        CellFootprint MeasureCellFootprint(const PointField::Cell& cell, const mat4& viewProj,
                                           const uvec2 extent)
        {
            const std::array<vec3, 8> corners = cell.Bounds.Corners();
            vec2 minScreen(std::numeric_limits<f32>::infinity());
            vec2 maxScreen(-std::numeric_limits<f32>::infinity());
            for (const vec3 corner : corners)
            {
                const vec4 clip = viewProj * vec4(corner, 1.0f);
                if (clip.w <= 0.0f)
                {
                    return CellFootprint{};
                }
                const vec2 ndc = vec2(clip) / clip.w;
                const vec2 screen = (ndc * 0.5f + 0.5f) * vec2(extent);
                minScreen = glm::min(minScreen, screen);
                maxScreen = glm::max(maxScreen, screen);
            }
            const vec2 size = maxScreen - minScreen;
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
        const AssetHandle<Veng::Shader> spriteFs =
            LoadShader(assets, PointSpriteFragId, "point-sprite fragment");
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
        m_SpritePipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = "PointField Sprite Pipeline",
                .ColorAttachments = {additiveTarget},
                .PipelineLayout = m_Layout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = spriteVs.Get()->Module},
                        {.Stage = ShaderStage::Fragment, .Module = spriteFs.Get()->Module},
                    },
            });
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

    void PointFieldScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const ResourceId outputId = io.Hdr;
        const ResourceId depthId = io.GBufferDepth;
        const TextureHandle depthHandle = io.DepthHandle;
        const SamplerHandle samplerHandle = m_SamplerHandle;

        graph.AddPass("PointField")
            .Color({
                .Resource = outputId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            })
            .Sample(depthId)
            .Execute(
                [this, depthHandle, samplerHandle](PassContext& inner)
                {
                    const ScenePassContext ctx = Wrap(inner);
                    CommandBuffer& cmd = ctx.Cmd();

                    if (m_Fields == nullptr || m_Fields->empty())
                    {
                        return;
                    }

                    const SceneView& view = ctx.View();
                    const mat4 viewProj = view.Camera.ViewProjection();
                    const Frustum frustum = Frustum::FromViewProjection(viewProj);
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                    const u32 region = m_Context.GetCurrentFrameInFlight();
                    const uvec2 renderExtent = view.RenderExtent;
                    // Pixels per world unit at depth w is |Proj[1][1]|*H/(2w) — the projection is
                    // Y-flipped for Vulkan clip space, so the diagonal is negative.
                    const f32 projScale = std::abs(view.Camera.Projection()[1][1]) *
                                          static_cast<f32>(renderExtent.y) * 0.5f;

                    // Draw each resolved field through its own descriptor sets and aggregate ring.
                    for (const PointField* field : *m_Fields)
                    {
                        if (field == nullptr || field->GetPointCount() == 0)
                        {
                            continue;
                        }
                        FieldState& state = StateFor(field);
                        state.Seen = true;

                        const PointFieldLod& lod = field->GetLod();
                        // A fully faded field contributes nothing (Opacity multiplies every sprite
                        // and splat), so skip its whole walk and draw — a consumer fading a layer
                        // out pays nothing for keeping it resident.
                        if (lod.Opacity <= 0.0f)
                        {
                            continue;
                        }

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

                        vector<const PointField::Cell*> resolved;
                        vector<GpuAggregateSplat> splats;
                        for (u32 c = 0; c < cells.size(); ++c)
                        {
                            const PointField::Cell& cell = cells[c];
                            if (cell.PointCount == 0 || !Intersects(frustum, cell.Bounds))
                            {
                                continue;
                            }
                            const CellFootprint footprint =
                                MeasureCellFootprint(cell, viewProj, renderExtent);
                            bool aggregate = state.Aggregating[c];
                            if (footprint.Density >= highGate)
                            {
                                aggregate = true;
                            }
                            else if (footprint.Density < lowGate)
                            {
                                aggregate = false;
                            }
                            state.Aggregating[c] = aggregate;

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
                                const bool continuous =
                                    lod.Style == PointFieldLod::AggregateStyle::Continuous;

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
                                    anchor += CellJitter(static_cast<i32>(lattice.x),
                                                         static_cast<i32>(lattice.y),
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
                                    const f32 fill = std::min(
                                        static_cast<f32>(cell.PointCount) / SplatFillCount, 1.0f);
                                    kernelPixels = glm::mix(footprint.Pixels * SplatSparseScale,
                                                            cellPixels, fill);
                                }
                                // The clamp keeps a subpixel kernel drawable (the floor puts the
                                // whole quad at MinPixels) and bounds a large near cell's overdraw.
                                const f32 drawnPixels =
                                    std::clamp(kernelPixels, lod.MinPixels / SplatSupportCells,
                                               lod.AggregateSplatPixels);
                                // Cloud normalizes by the larger of drawn/kernel — flux-conserving
                                // across the clamp, so a receding cell dims like a real emitter.
                                // Continuous normalizes by the kernel footprint itself (never the
                                // pixel floor), holding surface brightness constant as the camera
                                // recedes so an extended field shrinks and tiles rather than fading.
                                const f32 normalizePixels =
                                    continuous ? std::max(kernelPixels, 1e-3f)
                                               : std::max(drawnPixels, kernelPixels);
                                const f32 normalize = SpriteKernelFlux *
                                                      (pixelsPerWorld * pixelsPerWorld) /
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
                                resolved.push_back(&cell);
                            }
                        }

                        if (resolved.empty() && splats.empty())
                        {
                            continue;
                        }

                        cmd.SetViewport({0, 0}, renderExtent);
                        cmd.SetScissor({0, 0}, renderExtent);

                        PointFieldPush push{
                            .DepthTexture = depthHandle.Index,
                            .Sampler = samplerHandle.Index,
                            .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                            .MinPixels = lod.MinPixels,
                            .MaxPixels = lod.MaxPixels,
                            .MaxIntensity = lod.MaxIntensity,
                            .ViewportWidth = static_cast<f32>(renderExtent.x),
                            .ViewportHeight = static_cast<f32>(renderExtent.y),
                            .Opacity = lod.Opacity,
                        };

                        // Resolved sprites: one non-instanced draw per surviving cell, the vertex
                        // stage deriving point + corner from SV_VertexID over the cell's range.
                        if (!resolved.empty())
                        {
                            cmd.BindPipeline(m_SpritePipeline);
                            registry.Bind(cmd);
                            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                                .Sets = {state.SpriteSets[region]},
                                .FirstSet = 1,
                                .PipelineBindPoint = PipelineBindPoint::Graphics,
                            });
                            for (const PointField::Cell* cell : resolved)
                            {
                                push.FirstPoint = cell->FirstPoint;
                                push.PointCount = cell->PointCount;
                                cmd.PushConstants(push);
                                cmd.Draw(QuadVertexCount * cell->PointCount, 1, 0, 0);
                            }
                        }

                        // Aggregate splats: upload this frame's per-cell records into the field's
                        // ring region and draw them all as additive quads in one call.
                        if (!splats.empty())
                        {
                            const u64 regionOffset =
                                static_cast<u64>(region) * state.AggregateRegionStride;
                            auto* base = static_cast<u8*>(state.AggregateBuffer->GetMappedData()) +
                                         regionOffset;
                            std::memcpy(base, splats.data(),
                                        splats.size() * sizeof(GpuAggregateSplat));

                            cmd.BindPipeline(m_AggregatePipeline);
                            registry.Bind(cmd);
                            // Bind this frame's aggregate set, already pointing at the region just
                            // written — no per-frame descriptor update.
                            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                                .Sets = {state.AggregateSets[region]},
                                .FirstSet = 1,
                                .PipelineBindPoint = PipelineBindPoint::Graphics,
                            });
                            push.FirstPoint = 0;
                            push.PointCount = static_cast<u32>(splats.size());
                            cmd.PushConstants(push);
                            cmd.Draw(QuadVertexCount * static_cast<u32>(splats.size()), 1, 0, 0);
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
                });
    }
}
