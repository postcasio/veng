#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Asset/Flipbook.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

#include "SceneLayersScenePass.h"
#include "TranslucentScenePass.h"

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

    /// @brief GpuSprite::Flags bits, mirrored by sprite_record.slang.
    namespace SpriteFlag
    {
        /// @brief The flipbook is six-way: Texture and LightB are lightmaps, shaded by the scene.
        inline constexpr u32 SixWay = 1u << 0;
        /// @brief The shift of the two-bit frame blend (the FlipbookFrameBlend integer).
        inline constexpr u32 BlendShift = 1;
        /// @brief The stored temperature is under the sqrt transfer.
        inline constexpr u32 SqrtTemperature = 1u << 3;
        /// @brief The temperature is read from red of its own texture rather than LightB's alpha.
        inline constexpr u32 TemperatureRed = 1u << 4;
        /// @brief The sprite's emission ramp has keys; Ramp indexes it.
        inline constexpr u32 Emits = 1u << 5;
    }

    /// @brief One sprite as the sprite pass reads it (std430, 160 bytes).
    ///
    /// Mirrors sprite_record.slang's GpuSprite byte for byte.
    struct GpuSprite
    {
        /// @brief xyz: the world position the sprite's anchor sits on; w: the full frame's quad
        ///        width.
        vec4 PositionWidth{0.0f};
        /// @brief rgb: the linear HDR tint (of the lit term, for a six-way flipbook); a: the
        ///        opacity.
        vec4 Color{1.0f};
        /// @brief Frame N's atlas rectangle, (u0, v0, u1, v1) with (u0, v0) its top-left.
        vec4 UvRect{0.0f, 0.0f, 1.0f, 1.0f};
        /// @brief Frame N+1's atlas rectangle.
        vec4 UvRectNext{0.0f, 0.0f, 1.0f, 1.0f};
        /// @brief Where the anchor sits within the full frame, as a fraction (x right, y up).
        vec2 Anchor{0.5f};
        /// @brief The full frame's quad height.
        f32 Height = 1.0f;
        /// @brief Screen-plane roll, radians counter-clockwise.
        f32 Rotation = 0.0f;
        /// @brief One tile's extent in atlas UV.
        vec2 TileSize{1.0f};
        /// @brief Frame N's extent as a fraction of the full frame.
        f32 FrameScale = 1.0f;
        /// @brief Frame N+1's extent as a fraction of the full frame.
        f32 FrameScaleNext = 1.0f;
        /// @brief The colour sheet's (or six-way lightA's) bindless index.
        u32 Texture = 0;
        /// @brief The bindless index of the sampler every texture of the sprite is read through.
        u32 Sampler = 0;
        /// @brief The underlying FlipbookAlpha integer: how the colour sheet's alpha relates to
        ///        its colour.
        u32 AlphaMode = 0;
        /// @brief SpriteFlag bits.
        u32 Flags = 0;
        /// @brief The six-way -X/-Y/+Z lightmap's bindless index.
        u32 LightB = 0;
        /// @brief The motion sheet's bindless index, read when blending by motion.
        u32 Motion = 0;
        /// @brief The bindless index of the texture holding the six-way temperature.
        u32 Temperature = 0;
        /// @brief The index of the sprite's entry in the frame's emission-ramp table.
        u32 Ramp = 0;
        /// @brief How far playback is from frame N toward frame N+1, in [0, 1).
        f32 FrameFraction = 0.0f;
        /// @brief The motion sheet's decode scale.
        f32 MotionScale = 0.0f;
        /// @brief The world depth the sprite fades over into opaque geometry; 0 is no fade.
        f32 SoftDistance = 0.0f;
        /// @brief Multiplies the six-way emission.
        f32 EmissionIntensity = 1.0f;
        /// @brief Added to the decoded six-way temperature.
        f32 TemperatureOffset = 0.0f;
        /// @brief Padding to the std430 stride.
        u32 Pad0 = 0;
        /// @brief Padding to the std430 stride.
        u32 Pad1 = 0;
        /// @brief Padding to the std430 stride.
        u32 Pad2 = 0;
    };

    static_assert(sizeof(GpuSprite) == 160,
                  "GpuSprite must be 160 bytes (matches sprite_record.slang)");

    /// @brief One emission ramp as the sprite pass's fragment stage reads it (std430, 144 bytes).
    ///
    /// Mirrors Veng/sprite_shading_core.slang's SpriteEmissionRamp.
    struct GpuEmissionRamp
    {
        /// @brief (r, g, b, t): linear HDR colour at normalised temperature t, ascending in t.
        std::array<vec4, MaxFlipbookEmissionKeys> Keys{};
        /// @brief x: the ignition threshold; y: the key count; zw unused.
        vec4 Params{0.0f};
    };

    static_assert(sizeof(GpuEmissionRamp) == 144,
                  "GpuEmissionRamp must be 144 bytes (matches sprite_shading_core.slang)");

    /// @brief The most sprites the pass draws in one frame; a frame gathering more drops the rest.
    inline constexpr u32 MaxSpritesPerFrame = 4096;

    /// @brief One frame's gathered sprites, split by compositing.
    struct SpriteDrawPlan
    {
        /// @brief Alpha-composited sprites, sorted back to front.
        vector<GpuSprite> Alpha;
        /// @brief Additive sprites, in gather order.
        vector<GpuSprite> Additive;
        /// @brief The emission ramps six-way sprites index: one per distinct flipbook default and
        ///        one per sprite carrying an override.
        vector<GpuEmissionRamp> Ramps;
        /// @brief Sprites gathered past MaxSpritesPerFrame and not drawn this frame.
        u32 Dropped = 0;
        /// @brief Sprites whose override ramp held more than MaxFlipbookEmissionKeys keys and was
        ///        cut to its first ones.
        u32 RampsClamped = 0;
        /// @brief Whether a gathered sprite fades softly, and so reads the opaque-depth copy.
        bool ReadsSceneDepth = false;

        /// @brief Returns whether the plan draws anything.
        [[nodiscard]] bool IsEmpty() const { return Alpha.empty() && Additive.empty(); }
    };

    /// @brief Gathers a scene's drawable FlipbookSprites into a frame's plan.
    ///
    /// Walks every entity carrying a FlipbookSprite and a Transform whose flipbook and the textures
    /// it draws with are resident, whose opacity is positive, and which has not finished; resolves
    /// its world pose at the render alpha, its size, frame pair and fraction, shading
    /// (ResolveSpriteShading) and compositing, and its emission ramp; and splits the result into the
    /// alpha set (sorted back to front along the camera's view axis) and the additive set. Gathering
    /// stops at MaxSpritesPerFrame, counting the rest as dropped. The ramp table is bounded by the
    /// same budget, one entry at most per gathered sprite.
    /// @param scene   The scene to gather from.
    /// @param camera  The viewpoint, for the back-to-front sort.
    /// @param alpha   The render interpolation fraction.
    /// @param plan    The plan to fill; cleared first.
    void GatherSprites(const Scene& scene, const CameraView& camera, f32 alpha,
                       SpriteDrawPlan& plan);

    /// @brief Construction parameters for a SpriteScenePass.
    struct SpriteScenePassInfo
    {
        /// @brief The borrowed per-frame plan the pass draws.
        const SpriteDrawPlan* Plan = nullptr;
        /// @brief The lit scene-color target sprites composite into.
        ResourceId Target;
        /// @brief The opaque depth target, bound read-only for depth-testing.
        ResourceId Depth;
        /// @brief The bloom-mask target, or invalid when the frame wires none.
        ResourceId Mask;
        /// @brief The temporal resolve's reactive mask, or invalid when no temporal resolve is
        ///        wired.
        ResourceId Reactive;
        /// @brief The opaque-depth copy a soft sprite reads, or invalid when the frame has none.
        ResourceId SceneDepthCopy;
        /// @brief The renderer's IBL and shadow sets, bound at 4 and 5 for the six-way light
        ///        gather.
        ForwardLightingSets Forward;
        /// @brief Color format of the scene-color target.
        Format TargetFormat = Format::Undefined;
        /// @brief Color format of the bloom-mask target.
        Format MaskFormat = Format::Undefined;
        /// @brief Color format of the reactive mask.
        Format ReactiveFormat = Format::Undefined;
        /// @brief Frames in flight, the depth of the record ring.
        u32 FramesInFlight = 1;
    };

    /// @brief Draws the frame's flipbook sprites into the lit scene color.
    ///
    /// Wired after the full-resolution translucent pass and the ribbon pass, so sprites composite
    /// over translucent surfaces and ribbons, and ahead of TAA, bloom, and tonemap. Each sprite is
    /// a camera-facing quad expanded in the vertex stage from a per-frame record buffer (no vertex
    /// input), depth-tested against the opaque depth with depth writes off. The alpha set draws
    /// first, premultiplied-over; the additive set follows. A six-way sprite gathers the scene's
    /// light in the vertex stage through the renderer's IBL and shadow sets, bound at 4 and 5 as
    /// the forward-lit translucent pass binds them. When the frame wires a bloom mask, an unlit
    /// sprite writes it by its luminance and a six-way one by its emission's.
    class SpriteScenePass final : public SceneLayer
    {
    public:
        /// @brief Constructs the pass and its pipelines.
        /// @param context  Renderer context for pipeline and buffer creation.
        /// @param assets   The asset manager the core sprite shaders load through.
        /// @param info     The plan, targets, forward-lighting sets and formats.
        SpriteScenePass(Context& context, AssetManager& assets, const SpriteScenePassInfo& info);

        /// @brief Releases the pipelines and the record ring.
        ~SpriteScenePass() override;

        /// @brief Contributes the sprite pass, as a pass of its own, into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

        /// @brief Declares the opaque-depth copy and the shadow atlases read.
        void DeclareInputs(RenderGraph::PassBuilder& builder, const PassIO& io) const override;

        /// @brief Returns whether the plan holds a sprite this frame.
        [[nodiscard]] bool IsDrawing() const override;

        /// @brief Records the sprite draws.
        void Record(const ScenePassContext& ctx) const override;

    private:
        /// @brief Copies this frame's records and ramps into their ring regions; returns the record
        ///        count copied.
        u32 Upload() const;

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
        /// @brief The reactive mask (invalid when no temporal resolve is wired).
        ResourceId m_ReactiveId;
        /// @brief The opaque-depth copy (invalid when the frame has none).
        ResourceId m_SceneDepthCopyId;
        /// @brief The renderer's IBL and shadow sets.
        ForwardLightingSets m_Forward;
        /// @brief Frames in flight, the depth of the record ring.
        u32 m_FramesInFlight;

        /// @brief The sprite set's layout (binding 0 the records, binding 1 the ramps).
        Ref<DescriptorSetLayout> m_SetLayout;
        /// @brief The pipeline layout shared by both pipelines.
        Ref<PipelineLayout> m_Layout;
        /// @brief The premultiplied-over pipeline.
        Ref<GraphicsPipeline> m_AlphaPipeline;
        /// @brief The additive pipeline.
        Ref<GraphicsPipeline> m_AdditivePipeline;
        /// @brief Host-mapped record ring, one region per frame in flight.
        Ref<Buffer> m_Records;
        /// @brief Byte size of one record-ring region.
        u64 m_RegionStride = 0;
        /// @brief Host-mapped emission-ramp ring, one region per frame in flight.
        Ref<Buffer> m_Ramps;
        /// @brief Byte size of one ramp-ring region.
        u64 m_RampRegionStride = 0;
        /// @brief One set per frame in flight, each bound to its own ring region.
        vector<Ref<DescriptorSet>> m_Sets;
    };
}
