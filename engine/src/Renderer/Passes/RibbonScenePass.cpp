#include "RibbonScenePass.h"

#include <algorithm>
#include <array>
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
#include <Veng/Scene/RibbonSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng::Renderer
{
    namespace
    {
        // The ribbon shaders in the engine core pack (auto-mounted by AssetManager). The masked
        // fragments write the bloom mask as a second output; the plain ones write colour alone. The
        // post-resolve pair projects through the pushed unjittered projection and occludes by the
        // sampled scene depth.
        constexpr AssetId RibbonVertId{0xF6BFB05FF0406BA1ULL};
        constexpr AssetId RibbonFragId{0x53221CEA8AE17B58ULL};
        constexpr AssetId RibbonMaskedFragId{0x27AC55CDC4A9B5EFULL};
        constexpr AssetId RibbonPostResolveVertId{0x3E89E5673CF50020ULL};
        constexpr AssetId RibbonPostResolveFragId{0x4D7810339113239AULL};
        constexpr AssetId RibbonPostResolveMaskedFragId{0xF6C14E1276C14FEDULL};

        // Six vertices (two triangles) per record quad.
        constexpr u32 RibbonVertexCount = 6;

        // Two strip points closer than this, in world units, are one point: a zero-length segment
        // has no direction to face the camera about.
        constexpr f64 CoincidentDistance = 1e-4;

        // Matches ribbon_common.slang's push block.
        struct RibbonPushConstants
        {
            mat4 Proj{1.0f};
            u32 ViewConstantsIndex = 0;
            u32 DepthTexture = 0;
            u32 Sampler = 0;
            u32 Occluded = 1;
            vec2 TargetExtent{0.0f};
            vec2 Pad1{0.0f};
        };

        static_assert(sizeof(RibbonPushConstants) == 96,
                      "RibbonPushConstants must match ribbon_common.slang's push block");

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
            // A trail's cross-section semi-axes in world space; zero on both draws a band of Width.
            vec3 SemiX{0.0f};
            vec3 SemiY{0.0f};
        };

        // The width a point is drawn at from the eye: its Width, or its cross-section's silhouette.
        f32 DrawnWidth(const StripPoint& point, const vec3& tangent, const vec3& eyeToPoint)
        {
            if (point.SemiX == vec3(0.0f) && point.SemiY == vec3(0.0f))
            {
                return point.Width;
            }
            return TrailCrossSectionWidth(point.SemiX, point.SemiY, tangent, eyeToPoint);
        }

        // One placement's plan, with its alpha records held for the back-to-front sort.
        struct PlanSink
        {
            RibbonDrawPlan& Plan;
            vector<std::pair<f32, GpuRibbonSegment>> Sorted;
            vector<std::pair<f32, GpuRibbonSegment>> SortedUnoccluded;
        };

        // Packs records into the plans, rebased to the eye, against the per-frame budget the two
        // placements share. Records go to the placement Target names.
        struct SegmentSink
        {
            PlanSink Scene;
            PlanSink PostResolve;
            dvec3 Eye{0.0};
            mat3 Rotation{1.0f};
            u32 Gathered = 0;
            RibbonPlacement Target = RibbonPlacement::Scene;
            bool Occluded = true;

            void Add(const StripPoint& start, const StripPoint& end, const vec3& startTangent,
                     const vec3& endTangent, const bool additive)
            {
                const vec3 startAt(start.Position - Eye);
                const vec3 endAt(end.Position - Eye);
                Push(
                    GpuRibbonSegment{
                        .Start = vec4(startAt, DrawnWidth(start, startTangent, startAt)),
                        .End = vec4(endAt, DrawnWidth(end, endTangent, endAt)),
                        .StartColor = start.Color,
                        .EndColor = end.Color,
                        .StartTangent = vec4(startTangent, 0.0f),
                        .EndTangent = vec4(endTangent, 0.0f),
                    },
                    additive);
            }

            // A dot stands both ends on its centre; the marker in StartTangent.w selects the disc.
            void AddDot(const StripPoint& center, const bool additive)
            {
                const vec4 position(vec3(center.Position - Eye), center.Width);
                Push(
                    GpuRibbonSegment{
                        .Start = position,
                        .End = position,
                        .StartColor = center.Color,
                        .EndColor = center.Color,
                        .StartTangent = vec4(0.0f, 0.0f, 0.0f, 1.0f),
                        .EndTangent = vec4(0.0f),
                    },
                    additive);
            }

            void Push(const GpuRibbonSegment& record, const bool additive)
            {
                PlanSink& sink = Target == RibbonPlacement::PostResolve ? PostResolve : Scene;
                if (Gathered >= MaxRibbonSegmentsPerFrame)
                {
                    ++sink.Plan.Dropped;
                    return;
                }
                ++Gathered;
                if (additive)
                {
                    (Occluded ? sink.Plan.Additive : sink.Plan.UnoccludedAdditive)
                        .push_back(record);
                    return;
                }
                const f32 viewZ = (Rotation * ((vec3(record.Start) + vec3(record.End)) * 0.5f)).z;
                (Occluded ? sink.Sorted : sink.SortedUnoccluded).emplace_back(viewZ, record);
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

        void GatherTrail(const Trail& trail, const optional<mat4>& head, SegmentSink& sink,
                         vector<StripPoint>& points)
        {
            if (trail.Lifetime <= 0.0f)
            {
                return;
            }

            // The point at an age: full at the head, fading and tapering to the tail; a
            // cross-section tapers with the width, about the axes the sample was recorded with.
            const auto pointAt =
                [&trail](const vec3& position, const vec3& axisX, const vec3& axisY, const f32 age)
            {
                const f32 t = std::clamp(age / trail.Lifetime, 0.0f, 1.0f);
                const f32 width = trail.Width * (1.0f + (trail.TailWidthScale - 1.0f) * t);
                return StripPoint{
                    .Position = dvec3(position),
                    .Width = width,
                    .Color = vec4(trail.Color, trail.Opacity * (1.0f - t)),
                    .SemiX = axisX * (0.5f * trail.CrossSection.x * width),
                    .SemiY = axisY * (0.5f * trail.CrossSection.y * width),
                };
            };

            points.clear();
            for (const TrailSample& sample : trail.Samples)
            {
                AppendPoint(points,
                            pointAt(sample.Position, sample.AxisX, sample.AxisY, sample.Age));
            }
            if (head)
            {
                const mat4& pose = *head;
                AppendPoint(points, pointAt(vec3(pose[3]), vec3(pose[0]), vec3(pose[1]), 0.0f));
            }
            JoinStrip(points, false, trail.Additive, sink);
        }

        void GatherPath(const RibbonPath& path, const mat4& world, SegmentSink& sink,
                        vector<StripPoint>& points)
        {
            const glm::dmat4 transform(world);
            const f64 scale = glm::length(dvec3(transform[0]));
            sink.Target = path.Placement;
            sink.Occluded = path.Occluded;
            for (const RibbonStrip& strip : path.Strips)
            {
                if (strip.Opacity <= 0.0f || strip.Points.empty())
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
                if (points.size() == 1)
                {
                    sink.AddDot(points.front(), path.Additive);
                    continue;
                }
                JoinStrip(points, strip.Closed, path.Additive, sink);
            }
            sink.Target = RibbonPlacement::Scene;
            sink.Occluded = true;
        }

        // Moves a placement's sorted alpha records into its plan, farthest first.
        void FinishPlan(PlanSink& sink)
        {
            // The camera looks down -Z, so the most negative view-space z is the farthest record.
            for (auto [sorted, alpha] :
                 {std::pair{&sink.Sorted, &sink.Plan.Alpha},
                  std::pair{&sink.SortedUnoccluded, &sink.Plan.UnoccludedAlpha}})
            {
                std::ranges::stable_sort(*sorted, {}, &std::pair<f32, GpuRibbonSegment>::first);
                alpha->reserve(sorted->size());
                for (const auto& [depth, record] : *sorted)
                {
                    alpha->push_back(record);
                }
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
                       RibbonDrawPlan& scenePlan, RibbonDrawPlan& postResolvePlan,
                       const u32 visibleLayers, const Entity exclude)
    {
        // What a view leaves out of its mesh gather it leaves out here: a layer its mask omits, and
        // the one entity a capture feeds.
        const auto drawn = [&](const Entity entity, const RenderLayer layer)
        { return entity != exclude && RenderLayerInMask(visibleLayers, layer); };

        for (RibbonDrawPlan* plan : {&scenePlan, &postResolvePlan})
        {
            plan->Alpha.clear();
            plan->Additive.clear();
            plan->UnoccludedAlpha.clear();
            plan->UnoccludedAdditive.clear();
            plan->Dropped = 0;
        }

        const mat4 view = camera.View();
        SegmentSink sink{
            .Scene = {.Plan = scenePlan},
            .PostResolve = {.Plan = postResolvePlan},
            .Eye = dvec3(glm::inverse(glm::dmat4(view))[3]),
            .Rotation = mat3(view),
        };

        for (auto [entity, ribbon] : scene.View<Ribbon>())
        {
            if (!drawn(entity, ribbon.Layer))
            {
                continue;
            }
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
            if (!drawn(entity, trail.Layer))
            {
                continue;
            }
            optional<mat4> head;
            if (trail.Emitting && scene.Has<Transform>(entity))
            {
                head = DrawnWorld(scene, entity, alpha);
            }
            GatherTrail(trail, head, sink, points);
        }

        for (auto [entity, path] : scene.View<RibbonPath>())
        {
            if (!drawn(entity, path.Layer))
            {
                continue;
            }
            if (!path.Strips.empty() && scene.Has<Transform>(entity))
            {
                GatherPath(path, DrawnWorld(scene, entity, alpha), sink, points);
            }
        }

        FinishPlan(sink.Scene);
        FinishPlan(sink.PostResolve);
    }

    RibbonScenePass::RibbonScenePass(Context& context, AssetManager& assets,
                                     const RibbonScenePassInfo& info)
        : m_Context(context), m_Plan(info.Plan), m_Placement(info.Placement),
          m_TargetId(info.Target), m_DepthId(info.Depth), m_DepthHandle(info.DepthHandle),
          m_SamplerHandle(info.Sampler), m_MaskId(info.Mask), m_FramesInFlight(info.FramesInFlight)
    {
        const bool postResolve = m_Placement == RibbonPlacement::PostResolve;
        const bool masked = m_MaskId.IsValid();
        const AssetHandle<Veng::Shader> vs = LoadShader(
            assets, postResolve ? RibbonPostResolveVertId : RibbonVertId, "ribbon vertex");
        const AssetId fragmentId =
            postResolve ? (masked ? RibbonPostResolveMaskedFragId : RibbonPostResolveFragId)
                        : (masked ? RibbonMaskedFragId : RibbonFragId);
        const AssetHandle<Veng::Shader> fs = LoadShader(assets, fragmentId, "ribbon fragment");

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

        const auto makePipeline =
            [&](const char* name, const BlendState& colorBlend, const bool occluded)
        {
            vector<PipelineAttachmentInfo> attachments = {
                {.Format = info.TargetFormat, .Blend = colorBlend}};
            if (masked)
            {
                // The mask accumulates like the translucent pass's: two glows over one pixel sum.
                attachments.push_back({.Format = info.MaskFormat, .Blend = BlendState::Additive()});
            }
            return GraphicsPipeline::Create(
                m_Context,
                {
                    .Name = name,
                    .ColorAttachments = std::move(attachments),
                    // The post-resolve allocation has no depth buffer; that placement's fragment
                    // occludes against the sampled scene depth instead.
                    .DepthAttachmentFormat = postResolve ? Format::Undefined : GBuffer::DepthFormat,
                    .PipelineLayout = m_Layout,
                    .ShaderStages =
                        {
                            {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                            {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                        },
                    .Topology = PrimitiveTopology::TriangleList,
                    // The quad turns to face the camera, so it may present either winding.
                    .CullMode = CullMode::None,
                    .DepthTestEnable = !postResolve && occluded,
                    .DepthWriteEnable = false,
                    // Reverse-Z: a nearer fragment has larger depth.
                    .DepthCompareOp = CompareOp::GreaterOrEqual,
                });
        };
        m_AlphaPipeline = makePipeline("Ribbon Alpha Pipeline", BlendState::AlphaBlend(), true);
        m_AdditivePipeline =
            makePipeline("Ribbon Additive Pipeline", BlendState::AlphaAdditive(), true);
        m_UnoccludedAlphaPipeline =
            makePipeline("Ribbon Unoccluded Alpha Pipeline", BlendState::AlphaBlend(), false);
        m_UnoccludedAdditivePipeline =
            makePipeline("Ribbon Unoccluded Additive Pipeline", BlendState::AlphaAdditive(), false);

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
        const bool postResolve = m_Placement == RibbonPlacement::PostResolve;
        RenderGraph::PassBuilder builder =
            graph.AddPass(postResolve ? "Post-Resolve Ribbons" : "Scene Ribbons");
        builder.Color({
            .Resource = m_TargetId,
            .Load = LoadOp::Load,
            .Store = StoreOp::Store,
        });
        if (m_MaskId.IsValid())
        {
            // Every texel of the mask is already written — by the translucent pass's clear at the
            // render allocation, by it or the promotion at the post-resolve one — and ribbons add.
            builder.Color({
                .Resource = m_MaskId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            });
        }
        if (postResolve)
        {
            builder.Sample(m_DepthId);
        }
        else
        {
            builder.Depth({
                .Resource = m_DepthId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            });
        }
        // An idle wired pass (deactivation hysteresis) skips its frame rather than loading and
        // storing its targets around no draws.
        builder.SkipWhen([this] { return m_Plan == nullptr || m_Plan->IsEmpty(); });
        builder.Execute([this](PassContext& inner) { Record(Wrap(inner)); });
    }

    u32 RibbonScenePass::Upload() const
    {
        const RibbonDrawPlan& plan = *m_Plan;
        const auto budget =
            static_cast<u32>(std::min<usize>(plan.GetSegmentCount(), MaxRibbonSegmentsPerFrame));
        const u32 region = m_Context.GetCurrentFrameInFlight();
        auto* base = static_cast<u8*>(m_Records->GetMappedData()) +
                     static_cast<usize>(region) * m_RegionStride;

        // The four sets in draw order, so each is one contiguous run of the region.
        u32 count = 0;
        for (const vector<GpuRibbonSegment>* set :
             {&plan.Alpha, &plan.Additive, &plan.UnoccludedAlpha, &plan.UnoccludedAdditive})
        {
            const auto take = static_cast<u32>(std::min<usize>(set->size(), budget - count));
            std::memcpy(base + static_cast<usize>(count) * sizeof(GpuRibbonSegment), set->data(),
                        static_cast<usize>(take) * sizeof(GpuRibbonSegment));
            count += take;
        }
        return count;
    }

    void RibbonScenePass::Record(const ScenePassContext& ctx) const
    {
        CommandBuffer& cmd = ctx.Cmd();
        const SceneView& view = ctx.View();
        const bool postResolve = m_Placement == RibbonPlacement::PostResolve;
        const uvec2 extent = postResolve ? view.PostResolveExtent : view.RenderExtent;
        cmd.SetViewport({0, 0}, extent);
        cmd.SetScissor({0, 0}, extent);

        if (m_Plan == nullptr || m_Plan->IsEmpty())
        {
            return;
        }

        const u32 count = Upload();

        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        RibbonPushConstants push{.ViewConstantsIndex = registry.GetCurrentViewConstantsIndex()};
        if (postResolve)
        {
            // The view constants carry the jittered projection the scene rasterized through; the
            // tail is past the temporal resolve, so it projects through the camera's own.
            push.Proj = view.Camera.Projection();
            push.DepthTexture = m_DepthHandle.Index;
            push.Sampler = m_SamplerHandle.Index;
            push.TargetExtent = vec2(extent);
        }
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
        // Occluded alpha, occluded additive, then the unoccluded sets over them, matching Upload.
        const std::array<std::pair<const vector<GpuRibbonSegment>*, const Ref<GraphicsPipeline>*>,
                         4>
            sets{{{&m_Plan->Alpha, &m_AlphaPipeline},
                  {&m_Plan->Additive, &m_AdditivePipeline},
                  {&m_Plan->UnoccludedAlpha, &m_UnoccludedAlphaPipeline},
                  {&m_Plan->UnoccludedAdditive, &m_UnoccludedAdditivePipeline}}};
        u32 first = 0;
        for (usize i = 0; i < sets.size() && first < count; ++i)
        {
            const auto segments =
                static_cast<u32>(std::min<usize>(sets[i].first->size(), count - first));
            push.Occluded = i < 2 ? 1u : 0u;
            if (segments > 0)
            {
                draw(*sets[i].second, first, segments);
            }
            first += segments;
        }
    }
}
