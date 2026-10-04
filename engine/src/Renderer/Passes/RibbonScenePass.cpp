#include "RibbonScenePass.h"

#include <algorithm>
#include <cstring>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng::Renderer
{
    namespace
    {
        // The ribbon shaders in the engine core pack (auto-mounted by AssetManager). The masked
        // fragment writes the bloom mask as a second output; the plain one writes colour alone.
        constexpr AssetId RibbonVertId{0xF6BFB05FF0406BA1ULL};
        constexpr AssetId RibbonFragId{0x53221CEA8AE17B58ULL};
        constexpr AssetId RibbonMaskedFragId{0x27AC55CDC4A9B5EFULL};

        // Six vertices (two triangles) per segment quad.
        constexpr u32 RibbonVertexCount = 6;

        // Two strip points closer than this, in world units, are one point: a zero-length segment
        // has no direction to face the camera about.
        constexpr f64 CoincidentDistance = 1e-4;

        // Matches ribbon.vert.slang's push block.
        struct RibbonPushConstants
        {
            u32 ViewConstantsIndex;
            u32 Pad0;
            u32 Pad1;
            u32 Pad2;
        };

        AssetHandle<Veng::Shader> LoadShader(AssetManager& assets, const AssetId id,
                                             const char* what)
        {
            const AssetResult<AssetHandle<Veng::Shader>> shader = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(shader.has_value(), "RibbonScenePass: {} load failed: {}", what,
                      shader.error().Detail);
            return *shader;
        }

        // One point along a strip or a trail, before it is joined to its neighbours.
        struct StripPoint
        {
            dvec3 Position{0.0};
            f32 Width = 0.0f;
            vec4 Color{0.0f};
        };

        // Packs segments into the plan, rebased to the eye, against the per-frame budget.
        struct SegmentSink
        {
            RibbonDrawPlan& Plan;
            dvec3 Eye{0.0};
            mat3 Rotation{1.0f};
            vector<std::pair<f32, GpuRibbonSegment>> Sorted;
            u32 Gathered = 0;

            void Add(const StripPoint& start, const StripPoint& end, const vec3& startTangent,
                     const vec3& endTangent, const bool additive)
            {
                if (Gathered >= MaxRibbonSegmentsPerFrame)
                {
                    ++Plan.Dropped;
                    return;
                }
                ++Gathered;

                const vec3 relativeStart(start.Position - Eye);
                const vec3 relativeEnd(end.Position - Eye);
                const GpuRibbonSegment segment{
                    .Start = vec4(relativeStart, start.Width),
                    .End = vec4(relativeEnd, end.Width),
                    .StartColor = start.Color,
                    .EndColor = end.Color,
                    .StartTangent = vec4(startTangent, 0.0f),
                    .EndTangent = vec4(endTangent, 0.0f),
                };
                if (additive)
                {
                    Plan.Additive.push_back(segment);
                }
                else
                {
                    const f32 viewZ = (Rotation * ((relativeStart + relativeEnd) * 0.5f)).z;
                    Sorted.emplace_back(viewZ, segment);
                }
            }
        };

        vec3 Direction(const dvec3& from, const dvec3& to)
        {
            const dvec3 delta = to - from;
            const f64 length = glm::length(delta);
            return length > 0.0 ? vec3(delta / length) : vec3(0.0f);
        }

        // A point coincident with the last one replaces it: for a trail the newer is the brighter.
        void AppendPoint(vector<StripPoint>& points, const StripPoint& point)
        {
            if (!points.empty() &&
                glm::distance(points.back().Position, point.Position) < CoincidentDistance)
            {
                points.back() = point;
                return;
            }
            points.push_back(point);
        }

        // Joins merged points into segments; a closed run of three or more also joins the last
        // point back to the first. Each point's tangent spans its neighbours (wrapping when closed),
        // so both segments meeting at a joint turn about the same edge there.
        void JoinStrip(const vector<StripPoint>& points, const bool closed, const bool additive,
                       SegmentSink& sink)
        {
            const usize count = points.size();
            if (count < 2)
            {
                return;
            }
            const bool wraps = closed && count > 2;
            const auto tangentAt = [&points, count, wraps](const usize i, const vec3& segment)
            {
                const usize previous = wraps ? (i + count - 1) % count : (i == 0 ? 0 : i - 1);
                const usize next = wraps ? (i + 1) % count : std::min(i + 1, count - 1);
                // A hairpin doubles straight back, leaving its neighbours no span to face about.
                if (glm::distance(points[previous].Position, points[next].Position) <
                    CoincidentDistance)
                {
                    return segment;
                }
                return Direction(points[previous].Position, points[next].Position);
            };
            const usize segments = wraps ? count : count - 1;
            for (usize i = 0; i < segments; ++i)
            {
                const usize j = (i + 1) % count;
                const StripPoint& a = points[i];
                const StripPoint& b = points[j];
                if (a.Color.a <= 0.0f && b.Color.a <= 0.0f)
                {
                    continue;
                }
                const vec3 segment = Direction(a.Position, b.Position);
                sink.Add(a, b, tangentAt(i, segment), tangentAt(j, segment), additive);
            }
        }

        void GatherTrail(const Trail& trail, const optional<vec3> head, SegmentSink& sink,
                         vector<StripPoint>& points)
        {
            if (trail.Lifetime <= 0.0f)
            {
                return;
            }

            // The point at an age: full at the head, fading and tapering to the tail.
            const auto pointAt = [&trail](const vec3& position, const f32 age)
            {
                const f32 t = std::clamp(age / trail.Lifetime, 0.0f, 1.0f);
                return StripPoint{
                    .Position = dvec3(position),
                    .Width = trail.Width * (1.0f + (trail.TailWidthScale - 1.0f) * t),
                    .Color = vec4(trail.Color, trail.Opacity * (1.0f - t)),
                };
            };

            points.clear();
            for (const TrailSample& sample : trail.Samples)
            {
                AppendPoint(points, pointAt(sample.Position, sample.Age));
            }
            if (head)
            {
                AppendPoint(points, pointAt(*head, 0.0f));
            }
            JoinStrip(points, false, trail.Additive, sink);
        }

        void GatherPath(const RibbonPath& path, const mat4& world, SegmentSink& sink,
                        vector<StripPoint>& points)
        {
            const glm::dmat4 transform(world);
            const f64 scale = glm::length(dvec3(transform[0]));
            for (const RibbonStrip& strip : path.Strips)
            {
                if (strip.Opacity <= 0.0f || strip.Points.size() < 2)
                {
                    continue;
                }
                const auto width = static_cast<f32>(strip.Width * scale);
                const vec4 color(strip.Color, strip.Opacity);
                points.clear();
                for (const vec3& local : strip.Points)
                {
                    AppendPoint(points,
                                StripPoint{
                                    .Position = dvec3(transform * glm::dvec4(dvec3(local), 1.0)),
                                    .Width = width,
                                    .Color = color,
                                });
                }
                // A closed strip authored with its first point repeated at the end closes once.
                if (strip.Closed && points.size() > 2 &&
                    glm::distance(points.back().Position, points.front().Position) <
                        CoincidentDistance)
                {
                    points.pop_back();
                }
                JoinStrip(points, strip.Closed, path.Additive, sink);
            }
        }

        // The pose an entity's meshes draw at: interpolated while the scene interpolates, the
        // current one at alpha 0, and offset by any prediction smoothing still decaying.
        mat4 DrawnWorld(const Scene& scene, const Entity entity, const f32 alpha)
        {
            mat4 world = alpha != 0.0f && scene.HasTransformInterpolation()
                             ? scene.GetInterpolatedWorldTransform(entity, alpha)
                             : WorldMatrix(scene, entity);
            if (const auto* error = scene.TryGet<PredictionError>(entity))
            {
                world = ApplyPredictionError(world, *error);
            }
            return world;
        }
    }

    void GatherRibbons(const Scene& scene, const CameraView& camera, const f32 alpha,
                       RibbonDrawPlan& plan)
    {
        plan.Alpha.clear();
        plan.Additive.clear();
        plan.Dropped = 0;

        const mat4 view = camera.View();
        SegmentSink sink{
            .Plan = plan,
            .Eye = dvec3(glm::inverse(glm::dmat4(view))[3]),
            .Rotation = mat3(view),
        };

        for (auto [entity, ribbon] : scene.View<Ribbon>())
        {
            const f32 fade = ribbon.Lifetime > 0.0f
                                 ? std::clamp(1.0f - ribbon.Age / ribbon.Lifetime, 0.0f, 1.0f)
                                 : 1.0f;
            const dvec3 from(ribbon.From);
            const dvec3 to(ribbon.To);
            if (fade <= 0.0f || (ribbon.OpacityFrom <= 0.0f && ribbon.OpacityTo <= 0.0f) ||
                glm::distance(from, to) < CoincidentDistance)
            {
                continue;
            }
            const vec3 tangent = Direction(from, to);
            sink.Add({.Position = from,
                      .Width = ribbon.WidthFrom,
                      .Color = vec4(ribbon.ColorFrom, ribbon.OpacityFrom * fade)},
                     {.Position = to,
                      .Width = ribbon.WidthTo,
                      .Color = vec4(ribbon.ColorTo, ribbon.OpacityTo * fade)},
                     tangent, tangent, ribbon.Additive);
        }

        vector<StripPoint> points;
        for (auto [entity, trail] : scene.View<Trail>())
        {
            optional<vec3> head;
            if (trail.Emitting && scene.Has<Transform>(entity))
            {
                head = vec3(DrawnWorld(scene, entity, alpha)[3]);
            }
            GatherTrail(trail, head, sink, points);
        }

        for (auto [entity, path] : scene.View<RibbonPath>())
        {
            if (!path.Strips.empty() && scene.Has<Transform>(entity))
            {
                GatherPath(path, DrawnWorld(scene, entity, alpha), sink, points);
            }
        }

        // The camera looks down -Z, so the most negative view-space z is the farthest segment.
        std::ranges::stable_sort(sink.Sorted, {}, &std::pair<f32, GpuRibbonSegment>::first);
        plan.Alpha.reserve(sink.Sorted.size());
        for (const auto& [depth, segment] : sink.Sorted)
        {
            plan.Alpha.push_back(segment);
        }
    }

    RibbonScenePass::RibbonScenePass(Context& context, AssetManager& assets,
                                     const RibbonDrawPlan* plan, const ResourceId targetId,
                                     const ResourceId depthId, const ResourceId maskId,
                                     const Format targetFormat, const Format maskFormat,
                                     const u32 framesInFlight)
        : m_Context(context), m_Plan(plan), m_TargetId(targetId), m_DepthId(depthId),
          m_MaskId(maskId), m_FramesInFlight(framesInFlight)
    {
        const AssetHandle<Veng::Shader> vs = LoadShader(assets, RibbonVertId, "ribbon vertex");
        const AssetHandle<Veng::Shader> fs = LoadShader(
            assets, maskId.IsValid() ? RibbonMaskedFragId : RibbonFragId, "ribbon fragment");

        // Set 3 carries the per-frame record SSBO, off bindless, as the sprite pass's does.
        m_SetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "Ribbon Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageBuffer,
                                         .Count = 1,
                                         .Stages = ShaderStage::Vertex}},
                       });
        m_Layout = PipelineLayout::Create(
            m_Context, {
                           .Name = "Ribbon Layout",
                           .DescriptorSetLayouts = {m_SetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<RibbonPushConstants>(
                               ShaderStage::Vertex | ShaderStage::Fragment)},
                       });

        const auto makePipeline = [&](const char* name, const BlendState& colorBlend)
        {
            vector<PipelineAttachmentInfo> attachments = {
                {.Format = targetFormat, .Blend = colorBlend}};
            if (m_MaskId.IsValid())
            {
                // The mask accumulates like the translucent pass's: two glows over one pixel sum.
                attachments.push_back({.Format = maskFormat, .Blend = BlendState::Additive()});
            }
            return GraphicsPipeline::Create(
                m_Context,
                {
                    .Name = name,
                    .ColorAttachments = std::move(attachments),
                    .DepthAttachmentFormat = GBuffer::DepthFormat,
                    .PipelineLayout = m_Layout,
                    .ShaderStages =
                        {
                            {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                            {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                        },
                    .Topology = PrimitiveTopology::TriangleList,
                    // The quad turns to face the camera, so it may present either winding.
                    .CullMode = CullMode::None,
                    .DepthTestEnable = true,
                    .DepthWriteEnable = false,
                    // Reverse-Z: a nearer fragment has larger depth.
                    .DepthCompareOp = CompareOp::GreaterOrEqual,
                });
        };
        m_AlphaPipeline = makePipeline("Ribbon Alpha Pipeline", BlendState::AlphaBlend());
        m_AdditivePipeline = makePipeline("Ribbon Additive Pipeline", BlendState::AlphaAdditive());

        m_RegionStride = static_cast<u64>(MaxRibbonSegmentsPerFrame) * sizeof(GpuRibbonSegment);
        m_Records = Buffer::Create(m_Context, {
                                                  .Name = "Ribbon Records",
                                                  .Size = m_RegionStride * m_FramesInFlight,
                                                  .Usage = BufferUsage::Storage,
                                                  .HostMapped = true,
                                              });

        // One set per frame in flight, each bound once to its own ring region: rewriting a shared
        // set still referenced by a pending command buffer is a validation error.
        m_Sets.reserve(m_FramesInFlight);
        for (u32 frame = 0; frame < m_FramesInFlight; ++frame)
        {
            Ref<DescriptorSet> set =
                DescriptorSet::Create(m_Context, {.Name = "Ribbon Set", .Layout = m_SetLayout});
            set->Write(0, m_Records, static_cast<u64>(frame) * m_RegionStride, m_RegionStride);
            m_Sets.push_back(std::move(set));
        }
    }

    RibbonScenePass::~RibbonScenePass() = default;

    void RibbonScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        RenderGraph::PassBuilder builder = graph.AddPass("Scene Ribbons");
        builder.Color({
            .Resource = m_TargetId,
            .Load = LoadOp::Load,
            .Store = StoreOp::Store,
        });
        if (m_MaskId.IsValid())
        {
            // The translucent pass ahead of this one cleared the mask; ribbons add to it.
            builder.Color({
                .Resource = m_MaskId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            });
        }
        builder.Depth({
            .Resource = m_DepthId,
            .Load = LoadOp::Load,
            .Store = StoreOp::Store,
        });
        // An idle wired pass (deactivation hysteresis) skips its frame rather than loading and
        // storing its targets around no draws.
        builder.SkipWhen([this] { return m_Plan == nullptr || m_Plan->IsEmpty(); });
        builder.Execute([this](PassContext& inner) { Record(Wrap(inner)); });
    }

    u32 RibbonScenePass::Upload() const
    {
        const RibbonDrawPlan& plan = *m_Plan;
        const auto count =
            static_cast<u32>(std::min<usize>(plan.GetSegmentCount(), MaxRibbonSegmentsPerFrame));
        const u32 region = m_Context.GetCurrentFrameInFlight();
        auto* base = static_cast<u8*>(m_Records->GetMappedData()) +
                     static_cast<usize>(region) * m_RegionStride;

        // Alpha first, then additive, so each set is one contiguous run of the region.
        const usize alphaCount = std::min<usize>(plan.Alpha.size(), count);
        std::memcpy(base, plan.Alpha.data(), alphaCount * sizeof(GpuRibbonSegment));
        std::memcpy(base + alphaCount * sizeof(GpuRibbonSegment), plan.Additive.data(),
                    (count - alphaCount) * sizeof(GpuRibbonSegment));
        return count;
    }

    void RibbonScenePass::Record(const ScenePassContext& ctx) const
    {
        CommandBuffer& cmd = ctx.Cmd();
        const uvec2 extent = ctx.View().RenderExtent;
        cmd.SetViewport({0, 0}, extent);
        cmd.SetScissor({0, 0}, extent);

        if (m_Plan == nullptr || m_Plan->IsEmpty())
        {
            return;
        }

        const u32 count = Upload();
        const auto alphaCount = static_cast<u32>(std::min<usize>(m_Plan->Alpha.size(), count));
        const u32 additiveCount = count - alphaCount;

        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const RibbonPushConstants push{.ViewConstantsIndex =
                                           registry.GetCurrentViewConstantsIndex()};
        const u32 frame = m_Context.GetCurrentFrameInFlight();

        // The record index is the vertex index over six, so a set's first vertex selects its run.
        const auto draw =
            [&](const Ref<GraphicsPipeline>& pipeline, const u32 first, const u32 segments)
        {
            cmd.BindPipeline(pipeline);
            registry.Bind(cmd);
            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                .Sets = {m_Sets[frame]},
                .FirstSet = BindlessRegistry::FirstUserSet,
                .PipelineBindPoint = PipelineBindPoint::Graphics,
            });
            cmd.PushConstants(push);
            cmd.Draw(segments * RibbonVertexCount, 1, first * RibbonVertexCount, 0);
        };
        if (alphaCount > 0)
        {
            draw(m_AlphaPipeline, 0, alphaCount);
        }
        if (additiveCount > 0)
        {
            draw(m_AdditivePipeline, alphaCount, additiveCount);
        }
    }
}
