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
        // The sprite draw reads FirstPoint/PointCount to index one cell's point range; the
        // aggregate draw ignores them (it reads the splat record buffer). ViewportWidth/Height
        // convert the aggregate's pixel size to clip.
        struct PointFieldPush
        {
            u32 DepthTexture;
            u32 Sampler;
            u32 ViewConstantsIndex;
            u32 FirstPoint;
            u32 PointCount;
            f32 AggregateIntensity;
            f32 ViewportWidth;
            f32 ViewportHeight;
        };

        // One GPU aggregate splat record (std430): world center + pixel size, then summed color.
        struct GpuAggregateSplat
        {
            vec4 CenterSize; // xyz world centroid, w splat size in pixels
            vec4 Color;      // scaled summed cell color (rgb), a unused
        };

        // An occluded point fades to this fraction rather than vanishing (matches the sprite frag).
        constexpr f32 OccludedFade = 0.35f;

        AssetHandle<Veng::Shader> LoadShader(AssetManager& assets, AssetId id, const char* what)
        {
            const AssetResult<AssetHandle<Veng::Shader>> shader = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(shader.has_value(), "PointFieldScenePass: {} load failed: {}", what,
                      shader.error().Detail);
            return *shader;
        }

        // Screen density of a cell: its visible points divided by its projected screen footprint
        // in pixels. Projects the cell's world AABB corners to clip, takes the screen-space area of
        // their bounding rect, and returns points/pixel (INFINITY for a degenerate zero-area
        // footprint so a cell collapsed to a point always aggregates).
        f32 CellScreenDensity(const PointField::Cell& cell, const mat4& viewProj,
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
                    // A corner behind the eye: treat the footprint as unbounded (keep resolving).
                    return 0.0f;
                }
                const vec2 ndc = vec2(clip) / clip.w;
                const vec2 screen = (ndc * 0.5f + 0.5f) * vec2(extent);
                minScreen = glm::min(minScreen, screen);
                maxScreen = glm::max(maxScreen, screen);
            }
            const vec2 size = maxScreen - minScreen;
            const f32 area = std::max(size.x, 1.0f) * std::max(size.y, 1.0f);
            return static_cast<f32>(cell.PointCount) / area;
        }
    }

    PointFieldScenePass::PointFieldScenePass(Context& context, AssetManager& assets,
                                             const PointField* const* field,
                                             const Format outputFormat,
                                             const SamplerHandle samplerHandle,
                                             const u32 framesInFlight, const uvec2 extent)
        : m_Context(context), m_Field(field), m_OutputFormat(outputFormat),
          m_SamplerHandle(samplerHandle), m_Extent(extent)
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

        // Additive over the tonemapped scene color: emissive points accumulate, so a dense region
        // reads brighter and the aggregate splat glows.
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
        m_Set = DescriptorSet::Create(m_Context, {
                                                     .Name = "PointField Set",
                                                     .Layout = m_SetLayout,
                                                 });

        // The aggregate draw's per-frame splat records ride their own host-mapped ring, one region
        // per frame-in-flight, bound through a second set of the same layout.
        m_AggregateSet = DescriptorSet::Create(m_Context, {
                                                              .Name = "PointField Aggregate Set",
                                                              .Layout = m_SetLayout,
                                                          });
        m_AggregateRegionStride = static_cast<u64>(MaxAggregateSplats) * sizeof(GpuAggregateSplat);
        m_AggregateBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "PointField Aggregate Buffer",
                                          .Size = m_AggregateRegionStride * framesInFlight,
                                          .Usage = BufferUsage::Storage,
                                          .HostMapped = true,
                                      });
    }

    PointFieldScenePass::~PointFieldScenePass() = default;

    void PointFieldScenePass::Resize(const uvec2 extent)
    {
        m_Extent = extent;
    }

    void PointFieldScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const ResourceId outputId = io.Output;
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

                    const PointField* field = (m_Field != nullptr) ? *m_Field : nullptr;
                    if (field == nullptr || field->GetPointCount() == 0)
                    {
                        return;
                    }

                    const SceneView& view = ctx.View();
                    const mat4 viewProj = view.Camera.ViewProjection();
                    const Frustum frustum = Frustum::FromViewProjection(viewProj);
                    const PointFieldLod& lod = field->GetLod();

                    // Re-point the set at the field's resident buffer only when it changes (a
                    // rebuilt or swapped field), never per frame for a static field.
                    if (m_BoundBuffer != field->GetPointBuffer().get())
                    {
                        m_Set->Write(0, field->GetPointBuffer(), 0,
                                     field->GetPointBuffer()->GetSize());
                        m_BoundBuffer = field->GetPointBuffer().get();
                    }

                    // The per-cell aggregating latch resets when the field changes, so a swapped
                    // field starts resolving and settles into aggregation on its own densities.
                    const vector<PointField::Cell>& cells = field->GetCells();
                    if (m_LatchedField != field)
                    {
                        m_Aggregating.assign(cells.size(), false);
                        m_LatchedField = field;
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
                        const f32 density = CellScreenDensity(cell, viewProj, m_Extent);
                        bool aggregate = m_Aggregating[c];
                        if (density >= highGate)
                        {
                            aggregate = true;
                        }
                        else if (density < lowGate)
                        {
                            aggregate = false;
                        }
                        m_Aggregating[c] = aggregate;

                        if (aggregate && splats.size() < MaxAggregateSplats)
                        {
                            splats.push_back(GpuAggregateSplat{
                                .CenterSize = vec4(cell.Centroid, lod.AggregateSplatPixels),
                                .Color = vec4(cell.SummedColor * lod.AggregateIntensity, 0.0f),
                            });
                        }
                        else if (!aggregate)
                        {
                            resolved.push_back(&cell);
                        }
                    }

                    if (resolved.empty() && splats.empty())
                    {
                        return;
                    }

                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                    cmd.SetViewport({0, 0}, m_Extent);
                    cmd.SetScissor({0, 0}, m_Extent);

                    PointFieldPush push{
                        .DepthTexture = depthHandle.Index,
                        .Sampler = samplerHandle.Index,
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                        .AggregateIntensity = lod.AggregateIntensity,
                        .ViewportWidth = static_cast<f32>(m_Extent.x),
                        .ViewportHeight = static_cast<f32>(m_Extent.y),
                    };

                    // Resolved sprites: one non-instanced draw per surviving cell, the vertex stage
                    // deriving point + corner from SV_VertexID over [FirstPoint, FirstPoint+Count).
                    if (!resolved.empty())
                    {
                        cmd.BindPipeline(m_SpritePipeline);
                        registry.Bind(cmd);
                        cmd.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {m_Set},
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

                    // Aggregate splats: upload this frame's per-cell records into the ring region
                    // and draw them all as additive quads in one call.
                    if (!splats.empty())
                    {
                        const u32 region = m_Context.GetCurrentFrameInFlight();
                        const u64 regionOffset = static_cast<u64>(region) * m_AggregateRegionStride;
                        auto* base =
                            static_cast<u8*>(m_AggregateBuffer->GetMappedData()) + regionOffset;
                        std::memcpy(base, splats.data(), splats.size() * sizeof(GpuAggregateSplat));
                        m_AggregateSet->Write(0, m_AggregateBuffer, regionOffset,
                                              m_AggregateRegionStride);

                        cmd.BindPipeline(m_AggregatePipeline);
                        registry.Bind(cmd);
                        cmd.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {m_AggregateSet},
                            .FirstSet = 1,
                            .PipelineBindPoint = PipelineBindPoint::Graphics,
                        });
                        push.FirstPoint = 0;
                        push.PointCount = static_cast<u32>(splats.size());
                        cmd.PushConstants(push);
                        cmd.Draw(QuadVertexCount * static_cast<u32>(splats.size()), 1, 0, 0);
                    }
                });
    }
}
