#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

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

    /// @brief One ribbon segment as the ribbon pass's vertex stage reads it (std430, 96 bytes).
    ///
    /// Positions are relative to the camera's eye (the render origin), rebased on the CPU in double
    /// precision, so the vertex stage never pushes a large world coordinate through the view
    /// matrix. Mirrors ribbon.vert.slang's GpuRibbonSegment byte for byte.
    struct GpuRibbonSegment
    {
        /// @brief xyz: the segment's start, relative to the eye; w: the world width there.
        vec4 Start{0.0f};
        /// @brief xyz: the segment's end, relative to the eye; w: the world width there.
        vec4 End{0.0f};
        /// @brief rgb: the linear HDR colour at the start; a: the opacity there.
        vec4 StartColor{1.0f};
        /// @brief rgb: the linear HDR colour at the end; a: the opacity there.
        vec4 EndColor{1.0f};
        /// @brief xyz: the unit direction the ribbon runs at the start; w unused.
        vec4 StartTangent{0.0f};
        /// @brief xyz: the unit direction the ribbon runs at the end; w unused.
        vec4 EndTangent{0.0f};
    };

    static_assert(sizeof(GpuRibbonSegment) == 96,
                  "GpuRibbonSegment must be 96 bytes (matches ribbon.vert.slang)");

    /// @brief The most ribbon segments the pass draws in one frame; a frame gathering more drops the
    ///        rest.
    inline constexpr u32 MaxRibbonSegmentsPerFrame = 8192;

    /// @brief One frame's gathered ribbon segments, split by compositing.
    struct RibbonDrawPlan
    {
        /// @brief Alpha-composited segments, sorted back to front.
        vector<GpuRibbonSegment> Alpha;
        /// @brief Additive segments, in gather order.
        vector<GpuRibbonSegment> Additive;
        /// @brief Segments gathered past MaxRibbonSegmentsPerFrame and not drawn this frame.
        u32 Dropped = 0;

        /// @brief Returns whether the plan draws anything.
        [[nodiscard]] bool IsEmpty() const { return Alpha.empty() && Additive.empty(); }

        /// @brief Returns how many segments the plan draws.
        [[nodiscard]] usize GetSegmentCount() const { return Alpha.size() + Additive.size(); }
    };

    /// @brief Gathers a scene's Ribbons, Trails and RibbonPaths into a frame's plan.
    ///
    /// A Ribbon contributes one segment, From to To, with its Lifetime fade applied; one that is
    /// degenerate, fully faded, or transparent at both ends contributes none. A Trail contributes a
    /// segment between each consecutive pair of its samples, then from the newest to its entity's
    /// drawn position while it is Emitting, so a trail holding N samples contributes at most N
    /// segments. A RibbonPath's strips are placed by its entity's drawn world transform and each
    /// contributes a segment per consecutive pair of its points, plus one from the last back to the
    /// first when Closed. In trails and strips coincident consecutive points merge, and each point's
    /// tangent is taken across its neighbours, so adjacent segments share their joint's edge and a
    /// curve draws seamless. An entity's drawn pose is the one its meshes draw at: interpolated by
    /// @p alpha while the scene carries motion history, the current one otherwise. Positions are
    /// rebased to the camera's eye in double precision. The result is split into the alpha set
    /// (sorted back to front on view depth) and the additive set; gathering stops at
    /// MaxRibbonSegmentsPerFrame, counting the rest as dropped.
    /// @param scene   The scene to gather from.
    /// @param camera  The viewpoint: the render origin, and the back-to-front sort.
    /// @param alpha   The render interpolation fraction, placing trail heads and paths.
    /// @param plan    The plan to fill; cleared first.
    void GatherRibbons(const Scene& scene, const CameraView& camera, f32 alpha,
                       RibbonDrawPlan& plan);

    /// @brief Draws the frame's ribbons, trails and ribbon paths into the lit scene color.
    ///
    /// Wired after the full-resolution translucent pass and immediately ahead of the sprite pass,
    /// so ribbons composite over translucent surfaces, sprite effects standing on a beam or a trail
    /// (a flash at a muzzle, a burst at an impact) composite over it, and everything resolves under
    /// TAA and feeds bloom. Each segment is a quad expanded in the vertex stage from a per-frame
    /// record buffer (no vertex input), turned about its axis to face the camera and floored at
    /// about a pixel wide, depth-tested against the opaque depth with depth writes off. The alpha
    /// set draws first, straight-alpha over; the additive set follows. Both write the bloom mask by
    /// the contributed colour's luminance when the frame wires one.
    class RibbonScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass and its pipelines.
        /// @param context         Renderer context for pipeline and buffer creation.
        /// @param assets          The asset manager the core ribbon shaders load through.
        /// @param plan            Borrowed per-frame ribbon plan.
        /// @param targetId        The lit scene-color target ribbons composite into.
        /// @param depthId         The opaque depth target, bound read-only for depth-testing.
        /// @param maskId          The bloom-mask target, or invalid when the frame wires none.
        /// @param targetFormat    Color format of the scene-color target.
        /// @param maskFormat      Color format of the bloom-mask target.
        /// @param framesInFlight  Frames in flight, the depth of the record ring.
        RibbonScenePass(Context& context, AssetManager& assets, const RibbonDrawPlan* plan,
                        ResourceId targetId, ResourceId depthId, ResourceId maskId,
                        Format targetFormat, Format maskFormat, u32 framesInFlight);

        /// @brief Releases the pipelines and the record ring.
        ~RibbonScenePass() override;

        /// @brief Contributes the ribbon pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Copies this frame's records into its ring region; returns the count copied.
        u32 Upload() const;

        /// @brief Records the ribbon draws.
        void Record(const ScenePassContext& ctx) const;

        /// @brief Renderer context for bindless and frame-slot access.
        Context& m_Context;
        /// @brief Borrowed per-frame ribbon plan.
        const RibbonDrawPlan* m_Plan;
        /// @brief The lit scene-color target.
        ResourceId m_TargetId;
        /// @brief The opaque depth target.
        ResourceId m_DepthId;
        /// @brief The bloom-mask target (invalid when the frame wires none).
        ResourceId m_MaskId;
        /// @brief Frames in flight, the depth of the record ring.
        u32 m_FramesInFlight;

        /// @brief The record set's layout (binding 0, the record SSBO).
        Ref<DescriptorSetLayout> m_SetLayout;
        /// @brief The pipeline layout shared by both pipelines.
        Ref<PipelineLayout> m_Layout;
        /// @brief The straight-alpha over pipeline.
        Ref<GraphicsPipeline> m_AlphaPipeline;
        /// @brief The coverage-weighted additive pipeline.
        Ref<GraphicsPipeline> m_AdditivePipeline;
        /// @brief Host-mapped record ring, one region per frame in flight.
        Ref<Buffer> m_Records;
        /// @brief Byte size of one ring region.
        u64 m_RegionStride = 0;
        /// @brief One set per frame in flight, each bound to its own ring region.
        vector<Ref<DescriptorSet>> m_Sets;
    };
}
