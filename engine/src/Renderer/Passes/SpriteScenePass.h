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

    /// @brief One sprite as the sprite pass's vertex stage reads it (std430, 80 bytes).
    ///
    /// Mirrors sprite.vert.slang's GpuSprite byte for byte.
    struct GpuSprite
    {
        /// @brief xyz: the world position the sprite's anchor sits on; w: the quad's world width.
        vec4 PositionWidth{0.0f};
        /// @brief rgb: the linear HDR tint; a: the opacity.
        vec4 Color{1.0f};
        /// @brief The frame's atlas rectangle, (u0, v0, u1, v1) with (u0, v0) its top-left.
        vec4 UvRect{0.0f, 0.0f, 1.0f, 1.0f};
        /// @brief Where the anchor sits within the quad, as a fraction (x right, y up).
        vec2 Anchor{0.5f};
        /// @brief The quad's world height.
        f32 Height = 1.0f;
        /// @brief Screen-plane roll, radians counter-clockwise.
        f32 Rotation = 0.0f;
        /// @brief The atlas texture's bindless index.
        u32 Texture = 0;
        /// @brief The atlas sampler's bindless index.
        u32 Sampler = 0;
        /// @brief The underlying FlipbookAlpha integer: how the atlas's alpha relates to colour.
        u32 AlphaMode = 0;
        /// @brief Padding to the std430 stride.
        u32 Pad = 0;
    };

    static_assert(sizeof(GpuSprite) == 80,
                  "GpuSprite must be 80 bytes (matches sprite.vert.slang)");

    /// @brief The most sprites the pass draws in one frame; a frame gathering more drops the rest.
    inline constexpr u32 MaxSpritesPerFrame = 4096;

    /// @brief One frame's gathered sprites, split by compositing.
    struct SpriteDrawPlan
    {
        /// @brief Alpha-composited sprites, sorted back to front.
        vector<GpuSprite> Alpha;
        /// @brief Additive sprites, in gather order.
        vector<GpuSprite> Additive;
        /// @brief Sprites gathered past MaxSpritesPerFrame and not drawn this frame.
        u32 Dropped = 0;

        /// @brief Returns whether the plan draws anything.
        [[nodiscard]] bool IsEmpty() const { return Alpha.empty() && Additive.empty(); }
    };

    /// @brief Gathers a scene's drawable FlipbookSprites into a frame's plan.
    ///
    /// Walks every entity carrying a FlipbookSprite and a Transform whose flipbook and atlas are
    /// resident, whose opacity is positive, and which has not finished; resolves its world pose at
    /// the render alpha, its size, frame, and compositing; and splits the result into the alpha set
    /// (sorted back to front along the camera's view axis) and the additive set. Gathering stops at
    /// MaxSpritesPerFrame, counting the rest as dropped.
    /// @param scene   The scene to gather from.
    /// @param camera  The viewpoint, for the back-to-front sort.
    /// @param alpha   The render interpolation fraction.
    /// @param plan    The plan to fill; cleared first.
    void GatherSprites(const Scene& scene, const CameraView& camera, f32 alpha,
                       SpriteDrawPlan& plan);

    /// @brief Draws the frame's flipbook sprites into the lit scene color.
    ///
    /// Wired after the full-resolution translucent pass and the ribbon pass, so sprites composite
    /// over translucent surfaces and ribbons, and ahead of TAA, bloom, and tonemap. Each sprite is a camera-facing quad
    /// expanded in the vertex stage from a per-frame record buffer (no vertex input), depth-tested
    /// against the opaque depth with depth writes off. The alpha set draws first, premultiplied-over;
    /// the additive set follows. Both write the bloom mask by the sprite's luminance when the frame
    /// wires one, so a bright sprite glows however the bloom threshold is set.
    class SpriteScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass and its pipelines.
        /// @param context         Renderer context for pipeline and buffer creation.
        /// @param assets          The asset manager the core sprite shaders load through.
        /// @param plan            Borrowed per-frame sprite plan.
        /// @param targetId        The lit scene-color target sprites composite into.
        /// @param depthId         The opaque depth target, bound read-only for depth-testing.
        /// @param maskId          The bloom-mask target, or invalid when the frame wires none.
        /// @param targetFormat    Color format of the scene-color target.
        /// @param maskFormat      Color format of the bloom-mask target.
        /// @param framesInFlight  Frames in flight, the depth of the record ring.
        SpriteScenePass(Context& context, AssetManager& assets, const SpriteDrawPlan* plan,
                        ResourceId targetId, ResourceId depthId, ResourceId maskId,
                        Format targetFormat, Format maskFormat, u32 framesInFlight);

        /// @brief Releases the pipelines and the record ring.
        ~SpriteScenePass() override;

        /// @brief Contributes the sprite pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Copies this frame's records into its ring region; returns the count copied.
        u32 Upload() const;

        /// @brief Records the sprite draws.
        void Record(const ScenePassContext& ctx) const;

        /// @brief Renderer context for bindless and frame-slot access.
        Context& m_Context;
        /// @brief Borrowed per-frame sprite plan.
        const SpriteDrawPlan* m_Plan;
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
        /// @brief The premultiplied-over pipeline.
        Ref<GraphicsPipeline> m_AlphaPipeline;
        /// @brief The additive pipeline.
        Ref<GraphicsPipeline> m_AdditivePipeline;
        /// @brief Host-mapped record ring, one region per frame in flight.
        Ref<Buffer> m_Records;
        /// @brief Byte size of one ring region.
        u64 m_RegionStride = 0;
        /// @brief One set per frame in flight, each bound to its own ring region.
        vector<Ref<DescriptorSet>> m_Sets;
    };
}
