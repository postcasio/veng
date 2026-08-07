#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/FlowFieldShape.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Buffer;
    class CommandBuffer;
    class ComputePipeline;
    class Context;
    class DescriptorSet;
    class DescriptorSetLayout;
    class Image;
    class ImageView;
    class PipelineLayout;

    /// @brief The largest number of dye fields one flow instance transports.
    inline constexpr u32 MaxFlowDyes = 4;

    /// @brief Everything a flow instance is built from: its velocity, its dyes, its geometry.
    struct FlowFieldInfo
    {
        /// @brief Debug name; the instance's owned resources are named after it.
        string Name = "FlowField";

        /// @brief The caller-created, caller-painted velocity field, read-only for life.
        ///
        /// RG16Sfloat or RG32Sfloat, carrying ImageUsage::Sampled. Its texels are grid cells per
        /// unit of flow, and its extent is the grid. FlowField never writes it.
        Ref<Veng::Renderer::Image> Velocity;

        /// @brief The dye fields transported through the velocity; may not be empty.
        ///
        /// R16Sfloat, RG16Sfloat or RGBA16Sfloat, at the velocity field's extent, carrying
        /// ImageUsage::Sampled | ImageUsage::Storage. What a dye's channels mean is the caller's;
        /// FlowField advects them in place through an internally owned scratch.
        vector<Ref<Veng::Renderer::Image>> Dyes;

        /// @brief The grid geometry the dyes advect through.
        FlowFieldShape Shape;
    };

    /// @brief A lean advection-only transport of caller-supplied dye images along a static field.
    ///
    /// FlowField is the art-directed cousin of FluidSim with the whole pressure solve removed: a
    /// caller paints a velocity field once, and FlowField carries one or more dye images along it
    /// in a feedback loop. There is no velocity update — the field is static by construction, which
    /// is what makes this a *flow field* rather than a fluid — and no seeding, reinjection, or
    /// colour management: those are the caller's, because the source content is the caller's.
    ///
    /// The primitive allocates only a single advection scratch; the velocity is never written and
    /// the dyes are advected in place. Every pass is compute, and every barrier is recorded by hand
    /// into the command buffer the caller supplies — nothing rides the set-0 bindless registry and
    /// nothing touches the render graph.
    ///
    /// **A long unbroken advect turns any dye to mush.** Each semi-Lagrangian step samples with
    /// interpolation, so a feedback advection homogenises over many steps: the dye's contrast bleeds
    /// away and every texel drifts toward the local mean. RecordSharpen fights the blur, but a
    /// caller holding a specific look must interleave its **own** reinjection of source content
    /// between advects — which the record-into-caller-command-buffer shape makes trivial. The
    /// canonical use is to wrap RecordAdvect in generated-texture service ticks, reinjecting a seed
    /// between ticks. The typical consumer is a stylized transport — ink diffusing through water,
    /// smoke drifting on a painted breeze — settled to a still bake over a bounded step count.
    ///
    /// **Determinism.** The same seeded fields, the same configuration and the same step count
    /// produce the same output on a given device. Cross-device bit-identity is *not* promised.
    class FlowField
    {
    public:
        /// @brief Creates a flow instance over the supplied fields, loading its compute kernels.
        ///
        /// The configuration is validated first and a rejection is fatal — a mismatched extent, an
        /// unsupported format, a missing velocity field, no dye, or a zero-size grid is API misuse.
        /// @param context The render context the instance's scratch is created on.
        /// @param assets  Asset manager used to load the advect, sharpen, and store shaders.
        /// @param info    The fields and the geometry.
        /// @return A new flow instance, ready to record advects.
        static Unique<FlowField> Create(Context& context, AssetManager& assets,
                                        const FlowFieldInfo& info);

        /// @brief Destroys the instance's scratch through the deferred-destruction retire path.
        ~FlowField();

        FlowField(const FlowField&) = delete;
        FlowField& operator=(const FlowField&) = delete;

        /// @brief Records one semi-Lagrangian advection of every dye along the static velocity.
        ///
        /// Each dye moves by its local velocity scaled by the shape's StepScale, honouring the
        /// per-axis wrap and the per-row metric. Every dye is left in a sampled layout, so the
        /// caller may read the result immediately after.
        /// @param cmd The command buffer the advection is recorded into.
        void RecordAdvect(CommandBuffer& cmd);

        /// @brief Records `steps` consecutive advections into one command buffer.
        /// @param cmd   The command buffer the advections are recorded into.
        /// @param steps How many advections to record; zero records nothing.
        void RecordAdvect(CommandBuffer& cmd, u32 steps);

        /// @brief Records one clamped unsharp pass over every dye, holding detail against blur.
        ///
        /// Each dye is replaced by itself plus `strength` times its difference from a blurred copy,
        /// then clamped to its local neighbourhood's range — the clamp is not optional, since an
        /// unclamped sharpen in a feedback loop amplifies artifacts exponentially. `strength` of 0
        /// is a no-op and records nothing.
        /// @param cmd      The command buffer the sharpen is recorded into.
        /// @param strength How hard to sharpen; 0 records nothing.
        void RecordSharpen(CommandBuffer& cmd, f32 strength);

        /// @brief The grid's extent in texels, taken from the velocity field.
        [[nodiscard]] uvec2 GetExtent() const { return m_Extent; }

        /// @brief The number of advections recorded over this instance's lifetime.
        [[nodiscard]] u64 GetAdvectCount() const { return m_AdvectCount; }

        /// @brief The number of dye fields the instance transports.
        [[nodiscard]] u32 GetDyeCount() const { return static_cast<u32>(m_Dyes.size()); }

        /// @brief Sets how far one advect step moves the dye, per unit of velocity.
        void SetStepScale(f32 stepScale);

        /// @brief How far one advect step moves the dye, per unit of velocity.
        [[nodiscard]] f32 GetStepScale() const { return m_StepScale; }

    private:
        FlowField(Context& context, AssetManager& assets, const FlowFieldInfo& info);

        /// @brief One dye's image, its view, its descriptor sets, and its store pipeline.
        struct Dye
        {
            /// @brief The caller's dye image.
            Ref<Image> Field;
            /// @brief The whole-image view, bound both sampled and as a storage image.
            Ref<ImageView> View;
            /// @brief Advects the dye into the shared scratch.
            Ref<DescriptorSet> AdvectSet;
            /// @brief Sharpens the dye into the shared scratch.
            Ref<DescriptorSet> SharpenSet;
            /// @brief Moves the scratch back into the dye.
            Ref<DescriptorSet> StoreSet;
            /// @brief The store pipeline matching the dye's format.
            Ref<ComputePipeline> StorePipeline;
        };

        /// @brief Binds a compute pipeline with its set-1 descriptor set and pushes the params.
        void Bind(CommandBuffer& cmd, const Ref<ComputePipeline>& pipeline,
                  const Ref<DescriptorSet>& set, f32 scale) const;

        /// @brief Records one dye's advection and its store back into the caller's image.
        void RecordDyeAdvect(CommandBuffer& cmd, const Dye& dye);

        /// @brief Records one dye's sharpen and its store back into the caller's image.
        void RecordDyeSharpen(CommandBuffer& cmd, const Dye& dye, f32 strength);

        /// @brief Dispatches one workgroup per 8x8 tile of the grid.
        void DispatchGrid(CommandBuffer& cmd) const;

        /// @brief The context the instance's scratch is created on.
        Context& m_Context;
        /// @brief Debug name the owned resources are named after.
        string m_Name;
        /// @brief The grid extent, taken from the velocity field.
        uvec2 m_Extent{0, 0};

        /// @brief The caller's velocity field, read-only for the instance's life.
        Ref<Image> m_Velocity;
        /// @brief The velocity field's whole-image view.
        Ref<ImageView> m_VelocityView;
        /// @brief The rgba32f scratch every advection and sharpen writes into.
        Ref<Image> m_Scratch;
        /// @brief The scratch's view.
        Ref<ImageView> m_ScratchView;
        /// @brief One metric scale per grid row, all ones when the caller supplied none.
        Ref<Buffer> m_RowMetric;

        /// @brief The advect kernel's set layout (source, velocity, scratch, metric).
        Ref<DescriptorSetLayout> m_AdvectSetLayout;
        /// @brief The sharpen and store kernels' shared set layout (source, dest).
        Ref<DescriptorSetLayout> m_ImageSetLayout;

        /// @brief The advect kernel's pipeline layout.
        Ref<PipelineLayout> m_AdvectLayout;
        /// @brief The sharpen and store kernels' pipeline layout.
        Ref<PipelineLayout> m_ImageLayout;

        /// @brief The advection pipeline (one; every advection writes the rgba32f scratch).
        Ref<ComputePipeline> m_AdvectPipeline;
        /// @brief The sharpen pipeline (one; it too writes the rgba32f scratch).
        Ref<ComputePipeline> m_SharpenPipeline;

        /// @brief The dye fields, in the order the caller supplied them.
        vector<Dye> m_Dyes;

        /// @brief Boundary behaviour of the x axis.
        FlowWrap m_WrapX = FlowWrap::Periodic;
        /// @brief Boundary behaviour of the y axis.
        FlowWrap m_WrapY = FlowWrap::Clamped;
        /// @brief How far one advect step moves the dye, per unit of velocity.
        f32 m_StepScale = 1.0f;
        /// @brief Advections recorded over this instance's lifetime.
        u64 m_AdvectCount = 0;
    };

    /// @brief Folds a texel coordinate into [0, extent) under a wrap mode.
    ///
    /// The CPU reference for the fold every kernel applies: a periodic axis wraps (negatives
    /// included), a clamped axis clamps to the outermost texel.
    /// @param coord  The coordinate to fold, possibly outside the grid.
    /// @param extent The axis length in texels; must be non-zero.
    /// @param wrap   The axis's boundary behaviour.
    /// @return The folded coordinate, in [0, extent).
    [[nodiscard]] i32 FoldFlowTexel(i32 coord, u32 extent, FlowWrap wrap);

    /// @brief The grid position a semi-Lagrangian step back-traces a texel's centre to.
    ///
    /// The CPU reference for what the advection kernel computes before it taps: the texel's centre
    /// less one step of velocity, the x component scaled by the row's metric and the whole scaled
    /// by the step scale.
    /// @param texel     The texel being advected.
    /// @param velocity  The velocity sampled at that texel, in grid cells per unit of flow.
    /// @param stepScale How far one step moves the dye, per unit of velocity.
    /// @param rowMetric The row's metric scale.
    /// @return The back-traced position in grid coordinates (texel centres at i + 0.5).
    [[nodiscard]] vec2 FlowBackTrace(uvec2 texel, vec2 velocity, f32 stepScale, f32 rowMetric);

    /// @brief The bilinear tap the advection kernel reads at a back-traced position.
    ///
    /// The CPU reference for the hand-filtered tap: four folded texel loads combined by the
    /// fractional position. A caller supplies the loads, so the same reference serves a field of any
    /// channel count.
    /// @param position The back-traced grid position.
    /// @param extent   The grid extent in texels.
    /// @param wrapX    Boundary behaviour of the x axis.
    /// @param wrapY    Boundary behaviour of the y axis.
    /// @param load     Reads a field texel; only already-folded coordinates are passed to it.
    /// @return The filtered value.
    [[nodiscard]] vec4 SampleFlowBilinear(vec2 position, uvec2 extent, FlowWrap wrapX,
                                          FlowWrap wrapY, const function<vec4(ivec2)>& load);

    /// @brief The clamped unsharp value the sharpen kernel writes for one texel.
    ///
    /// The CPU reference for the sharpen the feedback loop depends on staying bounded: the centre
    /// plus `strength` times its difference from the blurred neighbourhood, clamped per channel to
    /// the neighbourhood's own range. The clamp is what makes it safe to run in a feedback loop —
    /// fed an extreme centre, the result is the neighbourhood extreme, never an amplified value.
    /// @param centre   The texel's own value.
    /// @param blur      The mean of the texel's neighbourhood.
    /// @param neighbourhoodMin Per-channel minimum over the neighbourhood.
    /// @param neighbourhoodMax Per-channel maximum over the neighbourhood.
    /// @param strength  How hard to sharpen; 0 returns the centre unchanged.
    /// @return The clamped unsharp value.
    [[nodiscard]] vec4 FlowSharpen(vec4 centre, vec4 blur, vec4 neighbourhoodMin,
                                   vec4 neighbourhoodMax, f32 strength);
}
