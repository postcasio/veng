#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Scene/Components.h>

namespace Veng
{
    class AssetManager;
    class Scene;
    class CameraView;
}

namespace Veng::Renderer
{
    class Buffer;
    class Context;
    class DescriptorSet;
    class DescriptorSetLayout;
    class GraphicsPipeline;
    class PipelineLayout;

    /// @brief One ribbon record as the ribbon pass's vertex stage reads it (std430, 96 bytes): a
    ///        segment, or a dot.
    ///
    /// Positions are relative to the camera's eye (the render origin), rebased on the CPU in double
    /// precision, so the vertex stage never pushes a large world coordinate through the view
    /// matrix. A dot record (StartTangent.w of 1) stands Start and End on the dot's centre with
    /// their widths its diameter and zero tangents, and the vertex stage expands it into a disc
    /// parallel to the image plane rather than a band. Mirrors ribbon.vert.slang's GpuRibbonSegment
    /// byte for byte.
    struct GpuRibbonSegment
    {
        /// @brief xyz: the segment's start, relative to the eye; w: the world width there (a dot's
        ///        diameter).
        vec4 Start{0.0f};
        /// @brief xyz: the segment's end, relative to the eye; w: the world width there.
        vec4 End{0.0f};
        /// @brief rgb: the linear HDR colour at the start; a: the opacity there.
        vec4 StartColor{1.0f};
        /// @brief rgb: the linear HDR colour at the end; a: the opacity there.
        vec4 EndColor{1.0f};
        /// @brief xyz: the unit direction the ribbon runs at the start; w: 1 for a dot record, 0
        ///        for a segment.
        vec4 StartTangent{0.0f};
        /// @brief xyz: the unit direction the ribbon runs at the end; w unused.
        vec4 EndTangent{0.0f};
    };

    static_assert(sizeof(GpuRibbonSegment) == 96,
                  "GpuRibbonSegment must be 96 bytes (matches ribbon.vert.slang)");

    /// @brief Returns whether a record is a dot rather than a segment.
    [[nodiscard]] inline bool IsDot(const GpuRibbonSegment& record)
    {
        return record.StartTangent.w > 0.5f;
    }

    /// @brief One segment of a tube trail, between two rings of its cross-section outline.
    ///
    /// A ring is the outline's points placed about its end's centre along that end's two axes,
    /// which carry the trail's taper. The outline's points live in the plan's outline table.
    struct GpuTrailTube
    {
        /// @brief xyz: the start ring's centre, relative to the eye; w unused.
        vec4 Start{0.0f};
        /// @brief xyz: the end ring's centre, relative to the eye; w unused.
        vec4 End{0.0f};
        /// @brief rgb: the linear HDR colour at the start; a: the opacity there.
        vec4 StartColor{1.0f};
        /// @brief rgb: the linear HDR colour at the end; a: the opacity there.
        vec4 EndColor{1.0f};
        /// @brief xyz: the world axis an outline point's x runs along at the start; w: Softness.
        vec4 StartAxisX{0.0f};
        /// @brief xyz: the world axis an outline point's y runs along at the start; w unused.
        vec4 StartAxisY{0.0f};
        /// @brief xyz: the world axis an outline point's x runs along at the end; w unused.
        vec4 EndAxisX{0.0f};
        /// @brief xyz: the world axis an outline point's y runs along at the end; w unused.
        vec4 EndAxisY{0.0f};
        /// @brief x: the outline's first point in the plan's table; y: its point count.
        uvec4 Outline{0u};
    };

    static_assert(sizeof(GpuTrailTube) == 144,
                  "GpuTrailTube must be 144 bytes (matches ribbon_tube.vert.slang)");

    /// @brief The most ribbon records the frame draws across both placements; a frame gathering
    ///        more drops the rest. A dot is one record, and so is one segment of a tube.
    inline constexpr u32 MaxRibbonSegmentsPerFrame = 8192;

    /// @brief The most outline points a frame's tube trails hold between them; a tube trail whose
    ///        outline would not fit is dropped.
    inline constexpr u32 MaxTrailOutlinePointsPerFrame = 8192;

    /// @brief One placement's gathered ribbon records for a frame, split by compositing.
    struct RibbonDrawPlan
    {
        /// @brief Alpha-composited records, sorted back to front.
        vector<GpuRibbonSegment> Alpha;
        /// @brief Additive records, in gather order.
        vector<GpuRibbonSegment> Additive;
        /// @brief Alpha-composited records of unoccluded paths, sorted back to front.
        vector<GpuRibbonSegment> UnoccludedAlpha;
        /// @brief Additive records of unoccluded paths, in gather order.
        vector<GpuRibbonSegment> UnoccludedAdditive;
        /// @brief Alpha-composited tube segments, sorted back to front.
        vector<GpuTrailTube> AlphaTubes;
        /// @brief Additive tube segments, in gather order.
        vector<GpuTrailTube> AdditiveTubes;
        /// @brief The cross-section outlines the tube segments index, every trail's in turn.
        vector<vec2> Outlines;
        /// @brief Records of this placement gathered past MaxRibbonSegmentsPerFrame and not drawn.
        u32 Dropped = 0;

        /// @brief Returns whether the plan draws anything.
        [[nodiscard]] bool IsEmpty() const { return GetSegmentCount() == 0; }

        /// @brief Returns how many records the plan draws.
        [[nodiscard]] usize GetSegmentCount() const
        {
            return Alpha.size() + Additive.size() + UnoccludedAlpha.size() +
                   UnoccludedAdditive.size() + AlphaTubes.size() + AdditiveTubes.size();
        }
    };

    /// @brief Gathers a scene's Ribbons, Trails and RibbonPaths into a frame's two plans, one per
    ///        RibbonPlacement.
    ///
    /// A Ribbon contributes one segment, From to To, with its Lifetime fade applied; one that is
    /// degenerate, fully faded, or transparent at both ends contributes none. A Trail contributes a
    /// segment between each consecutive pair of its samples, then from the newest to its entity's
    /// drawn position while it is Emitting, so a trail holding N samples contributes at most N
    /// segments. Ribbons and trails always go to the scene plan. A RibbonPath goes to the plan its
    /// Placement names; its strips are placed by its entity's drawn world transform and each
    /// contributes a segment per consecutive pair of its points, plus one from the last back to the
    /// first when Closed, or a single dot record when it holds one distinct point. In trails and
    /// strips coincident consecutive points merge, and each point's tangent is taken across its
    /// neighbours, so adjacent segments share their joint's edge and a curve draws seamless. An
    /// entity's drawn pose is the one its meshes draw at: interpolated by @p alpha while the scene
    /// carries motion history, the current one otherwise. Positions are rebased to the camera's eye
    /// in double precision. Each plan is split into the alpha set (sorted back to front on view
    /// depth) and the additive set, each again by whether its path is Occluded — the unoccluded
    /// sets draw after the occluded ones, through the depth; gathering stops at MaxRibbonSegmentsPerFrame records across both
    /// plans, counting the rest as dropped in the plan each would have joined.
    /// @param scene            The scene to gather from.
    /// @param camera           The viewpoint: the render origin, and the back-to-front sort.
    /// @param alpha            The render interpolation fraction, placing trail heads and paths.
    /// @param scenePlan        The RibbonPlacement::Scene plan to fill; cleared first.
    /// @param postResolvePlan  The RibbonPlacement::PostResolve plan to fill; cleared first.
    /// @param visibleLayers    The view's render-layer mask: a ribbon, trail or path whose Layer it
    ///                         omits is not gathered, as a mesh on that layer is not.
    /// @param exclude          The view's excluded entity (a capture's own surface), not gathered.
    void GatherRibbons(const Scene& scene, const CameraView& camera, f32 alpha,
                       RibbonDrawPlan& scenePlan, RibbonDrawPlan& postResolvePlan,
                       u32 visibleLayers = AllRenderLayers, Entity exclude = Entity::Null);

    /// @brief How a RibbonScenePass is wired into the frame.
    struct RibbonScenePassInfo
    {
        /// @brief The borrowed per-frame plan the pass draws.
        const RibbonDrawPlan* Plan = nullptr;
        /// @brief Where in the frame the pass sits, and so what it draws into and how it occludes.
        RibbonPlacement Placement = RibbonPlacement::Scene;
        /// @brief The scene-color target the ribbons composite into: the lit target for Scene, the
        ///        finished post-resolve scene colour for PostResolve.
        ResourceId Target;
        /// @brief The opaque depth: attached read-only for Scene's depth test, sampled for
        ///        PostResolve's per-fragment occlusion.
        ResourceId Depth;
        /// @brief The depth's bindless slot, which PostResolve samples.
        TextureHandle DepthHandle;
        /// @brief The sampler PostResolve reads the depth through.
        SamplerHandle Sampler;
        /// @brief The bloom-mask target at the pass's allocation, or invalid when the frame wires
        ///        none.
        ResourceId Mask;
        /// @brief Color format of the scene-color target.
        Format TargetFormat = Format::Undefined;
        /// @brief Color format of the bloom-mask target.
        Format MaskFormat = Format::Undefined;
        /// @brief Frames in flight, the depth of the record ring.
        u32 FramesInFlight = 1;
    };

    /// @brief Draws one placement's ribbons, trails and ribbon paths into a scene colour.
    ///
    /// The Scene placement is wired after the full-resolution translucent pass and immediately
    /// ahead of the sprite pass, so ribbons composite over translucent surfaces, sprite effects
    /// standing on a beam or a trail (a flash at a muzzle, a burst at an impact) composite over it,
    /// and everything resolves under TAA and feeds bloom; it is depth-tested against the opaque
    /// depth with depth writes off. The PostResolve placement is declared at the HDR tail anchor,
    /// after the post-process effects and immediately before the pre-bloom GUI overlay composite,
    /// into the finished scene colour at the post-resolve allocation through the unjittered
    /// projection; with no depth attachment there, each fragment samples the render-allocation
    /// depth through the sub-rect remap and is discarded when behind it. Each record is a quad
    /// expanded in the vertex stage from a per-frame record buffer (no vertex input) — a segment
    /// turned about its axis to face the camera, a dot a disc parallel to the image plane — and
    /// floored at about a pixel wide in the target's pixels. The alpha set draws first,
    /// straight-alpha over; the additive set follows. Both write the bloom mask by the contributed
    /// colour's luminance when the frame wires one.
    class RibbonScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass and its pipelines.
        /// @param context  Renderer context for pipeline and buffer creation.
        /// @param assets   The asset manager the core ribbon shaders load through.
        /// @param info     The placement, the plan, and the targets the pass is wired to.
        RibbonScenePass(Context& context, AssetManager& assets, const RibbonScenePassInfo& info);

        /// @brief Releases the pipelines and the record ring.
        ~RibbonScenePass() override;

        /// @brief Contributes the ribbon pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Copies this frame's band records into its ring region; returns the count copied.
        u32 Upload() const;

        /// @brief Copies this frame's tube segments and outlines into their ring regions; returns
        ///        the segment count copied.
        /// @param budget  The records the band upload left of the frame's budget.
        u32 UploadTubes(u32 budget) const;

        /// @brief Records the ribbon draws.
        void Record(const ScenePassContext& ctx) const;

        /// @brief Renderer context for bindless and frame-slot access.
        Context& m_Context;
        /// @brief Borrowed per-frame ribbon plan.
        const RibbonDrawPlan* m_Plan;
        /// @brief Where in the frame the pass sits.
        RibbonPlacement m_Placement;
        /// @brief The scene-color target.
        ResourceId m_TargetId;
        /// @brief The opaque depth target.
        ResourceId m_DepthId;
        /// @brief The depth's bindless slot (sampled by the PostResolve placement).
        TextureHandle m_DepthHandle;
        /// @brief The sampler the PostResolve placement reads the depth through.
        SamplerHandle m_SamplerHandle;
        /// @brief The bloom-mask target (invalid when the frame wires none).
        ResourceId m_MaskId;
        /// @brief Frames in flight, the depth of the record ring.
        u32 m_FramesInFlight;

        /// @brief The record set's layout: binding 0 the band records, 1 the tube segments, 2 the
        ///        outline points.
        Ref<DescriptorSetLayout> m_SetLayout;
        /// @brief The pipeline layout shared by both pipelines.
        Ref<PipelineLayout> m_Layout;
        /// @brief The straight-alpha over pipeline.
        Ref<GraphicsPipeline> m_AlphaPipeline;
        /// @brief The coverage-weighted additive pipeline.
        Ref<GraphicsPipeline> m_AdditivePipeline;
        /// @brief The straight-alpha pipeline for unoccluded paths: the scene placement's without
        ///        its depth test (the post-resolve placement's occlusion is a push-constant switch).
        Ref<GraphicsPipeline> m_UnoccludedAlphaPipeline;
        /// @brief The additive pipeline for unoccluded paths.
        Ref<GraphicsPipeline> m_UnoccludedAdditivePipeline;
        /// @brief The straight-alpha tube pipeline; the scene placement's alone.
        Ref<GraphicsPipeline> m_TubeAlphaPipeline;
        /// @brief The additive tube pipeline; the scene placement's alone.
        Ref<GraphicsPipeline> m_TubeAdditivePipeline;
        /// @brief Host-mapped record ring, one region per frame in flight.
        Ref<Buffer> m_Records;
        /// @brief Byte size of one ring region.
        u64 m_RegionStride = 0;
        /// @brief Host-mapped tube-segment ring, one region per frame in flight.
        Ref<Buffer> m_TubeRecords;
        /// @brief Byte size of one tube ring region.
        u64 m_TubeRegionStride = 0;
        /// @brief Host-mapped outline-point ring, one region per frame in flight.
        Ref<Buffer> m_Outlines;
        /// @brief Byte size of one outline ring region.
        u64 m_OutlineRegionStride = 0;
        /// @brief One set per frame in flight, each bound to its own ring region.
        vector<Ref<DescriptorSet>> m_Sets;
    };
}
