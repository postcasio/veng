#pragma once

#include <Veng/Veng.h>
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

    /// @brief How a grid axis behaves at its edges.
    enum class FluidWrap : u8
    {
        /// @brief The axis is a ring: a coordinate past one edge folds around to the other.
        Periodic,
        /// @brief The axis is bounded by walls.
        ///
        /// A coordinate past an edge clamps to it, and the projection zeroes the wall-normal
        /// velocity component at the outermost texels (free slip) — which is also why those
        /// texels carry residual divergence the interior does not.
        Clamped,
    };

    /// @brief The Jacobi iteration count a projection runs when a caller names none.
    ///
    /// Too few leaves compressible-looking artifacts; the cost is one dispatch per iteration over
    /// the grid, so raising it is a straight trade of GPU time for incompressibility.
    inline constexpr u32 DefaultFluidJacobiIterations = 20;

    /// @brief The largest number of dye fields one solver instance advects.
    inline constexpr u32 MaxFluidDyes = 4;

    /// @brief One dye field the solver advects through the velocity, and its dissipation.
    struct FluidDyeInfo
    {
        /// @brief The caller-created, caller-seeded dye image.
        ///
        /// R16Sfloat, RG16Sfloat or RGBA16Sfloat, at the velocity field's extent, carrying
        /// ImageUsage::Sampled | ImageUsage::Storage. What its channels mean is the caller's.
        Ref<Veng::Renderer::Image> Field;

        /// @brief Fraction of the dye lost per unit of simulated time; 0 conserves it exactly.
        f32 Dissipation = 0.0f;
    };

    /// @brief Everything a solver instance is built from: its fields, its geometry, its knobs.
    struct FluidSimInfo
    {
        /// @brief Debug name; the solver's owned resources are named after it.
        string Name = "FluidSim";

        /// @brief The caller-created, caller-seeded velocity field.
        ///
        /// RG16Sfloat or RG32Sfloat, carrying ImageUsage::Sampled | ImageUsage::Storage. Its
        /// texels are grid cells per unit of simulated time, and its extent is the grid.
        Ref<Veng::Renderer::Image> Velocity;

        /// @brief The dye fields advected through the velocity; may be empty.
        vector<FluidDyeInfo> Dyes;

        /// @brief An optional velocity field the solver relaxes toward at RelaxationRate.
        ///
        /// Same formats and extent as Velocity. This is what holds a caller's prescribed flow
        /// against the turbulence eroding it; with no target the rate is inert.
        Ref<Veng::Renderer::Image> RelaxationTarget;

        /// @brief An optional mask (R16Sfloat or R32Sfloat) multiplying the velocity each step.
        ///
        /// Same extent as Velocity. A texel of 1 leaves the velocity untouched and 0 stops it
        /// dead; values between damp it geometrically over the steps.
        Ref<Veng::Renderer::Image> DampingMask;

        /// @brief An optional per-row scale on the x axis, one entry per grid row.
        ///
        /// A row of metric m advances m grid cells per unit of x velocity and has its x
        /// derivatives scaled by m — the general form of "rows are shorter here than there". It
        /// is a stretch on the grid and says nothing about what the grid represents. Empty
        /// leaves every row at 1.
        vector<f32> RowMetric;

        /// @brief Boundary behaviour of the x axis.
        FluidWrap WrapX = FluidWrap::Periodic;

        /// @brief Boundary behaviour of the y axis.
        FluidWrap WrapY = FluidWrap::Clamped;

        /// @brief How much simulated time one RecordStep advances.
        ///
        /// The advection is unconditionally stable, so this is a quality knob rather than a
        /// stability constraint: a larger step travels further per dispatch and diffuses more.
        f32 TimeStep = 1.0f;

        /// @brief Jacobi iterations the projection runs per step.
        u32 JacobiIterations = DefaultFluidJacobiIterations;

        /// @brief Vorticity confinement strength; 0 leaves the force out.
        ///
        /// Feeds energy back into the eddies the advection's bilinear taps diffuse away, which
        /// is what keeps small structure crisp over a long run.
        f32 VorticityConfinement = 0.0f;

        /// @brief Rate at which velocity relaxes toward RelaxationTarget, per unit of time.
        f32 RelaxationRate = 0.0f;

        /// @brief Fraction of velocity lost per unit of simulated time.
        f32 VelocityDissipation = 0.0f;
    };

    /// @brief A 2D stable-fluids solver over caller-supplied velocity and dye images.
    ///
    /// The mathematics is Jos Stam's stable-fluids scheme, reimplemented from his published
    /// papers: semi-Lagrangian advection, then forces, then a Jacobi pressure projection. No
    /// code was taken from any implementation of it.
    ///
    /// One step is: advect the velocity through itself; apply vorticity confinement from the
    /// curl field, relaxation toward an optional target, and an optional damping mask; project
    /// the result divergence-free through `JacobiIterations` Jacobi passes and a gradient
    /// subtraction; then advect each dye through the projected velocity with its own
    /// dissipation. Every pass is compute, and every barrier is recorded by hand into the
    /// command buffer the caller supplies — the compiled scene graph never knows the solver
    /// exists, and nothing here rides the set-0 bindless registry.
    ///
    /// **It renders nothing and owns no meaning.** What a dye channel is, what the initial
    /// fields look like, and how many steps are worth running belong entirely to the caller;
    /// the solver never fills initial conditions.
    ///
    /// **Determinism.** The same seeded fields, the same configuration and the same step count
    /// produce the same output on a given device. Cross-device bit-identity is *not* promised.
    class FluidSim
    {
    public:
        /// @brief Creates a solver over the supplied fields, loading its core compute kernels.
        ///
        /// The configuration is validated first and a rejection is fatal — a mismatched extent,
        /// an unsupported format, a missing velocity field or a zero-size grid is API misuse.
        /// @param context The render context the solver's own transients are created on.
        /// @param assets  Asset manager used to load the core fluid compute shaders.
        /// @param info    The fields, the geometry, and the knobs.
        /// @return A new solver, ready to record steps.
        static Unique<FluidSim> Create(Context& context, AssetManager& assets,
                                       const FluidSimInfo& info);

        /// @brief Destroys the solver's transients through the deferred-destruction retire path.
        ~FluidSim();

        FluidSim(const FluidSim&) = delete;
        FluidSim& operator=(const FluidSim&) = delete;

        /// @brief Records one solver step's dispatches and barriers into a command buffer.
        ///
        /// A caller amortizing a long spin-up wraps this in generated-texture service ticks, one
        /// or a few steps per tick; the solver holds no reference to that service. Every field
        /// is left in a sampled layout, so the caller may read the result immediately after.
        /// @param cmd The command buffer the step is recorded into.
        void RecordStep(CommandBuffer& cmd);

        /// @brief Records `steps` consecutive steps into one command buffer.
        /// @param cmd   The command buffer the steps are recorded into.
        /// @param steps How many steps to record; zero records nothing.
        void RecordSteps(CommandBuffer& cmd, u32 steps);

        /// @brief The grid's extent in texels, taken from the velocity field.
        [[nodiscard]] uvec2 GetExtent() const { return m_Extent; }

        /// @brief The number of steps recorded over this solver's lifetime.
        [[nodiscard]] u64 GetStepCount() const { return m_StepCount; }

        /// @brief Sets how much simulated time one step advances.
        void SetTimeStep(f32 timeStep);

        /// @brief How much simulated time one step advances.
        [[nodiscard]] f32 GetTimeStep() const { return m_TimeStep; }

        /// @brief Sets the Jacobi iterations the projection runs per step.
        void SetJacobiIterations(u32 iterations);

        /// @brief The Jacobi iterations the projection runs per step.
        [[nodiscard]] u32 GetJacobiIterations() const { return m_JacobiIterations; }

        /// @brief Sets the vorticity confinement strength.
        void SetVorticityConfinement(f32 strength) { m_Confinement = strength; }

        /// @brief The vorticity confinement strength.
        [[nodiscard]] f32 GetVorticityConfinement() const { return m_Confinement; }

        /// @brief Sets the rate at which velocity relaxes toward the target field.
        void SetRelaxationRate(f32 rate) { m_RelaxationRate = rate; }

        /// @brief The rate at which velocity relaxes toward the target field.
        [[nodiscard]] f32 GetRelaxationRate() const { return m_RelaxationRate; }

        /// @brief Sets the fraction of velocity lost per unit of simulated time.
        void SetVelocityDissipation(f32 dissipation) { m_VelocityDissipation = dissipation; }

        /// @brief The fraction of velocity lost per unit of simulated time.
        [[nodiscard]] f32 GetVelocityDissipation() const { return m_VelocityDissipation; }

        /// @brief Sets one dye's dissipation.
        /// @param index      The dye's index in the order it was supplied.
        /// @param dissipation Fraction of the dye lost per unit of simulated time.
        void SetDyeDissipation(u32 index, f32 dissipation);

        /// @brief The number of dye fields the solver advects.
        [[nodiscard]] u32 GetDyeCount() const { return static_cast<u32>(m_Dyes.size()); }

    private:
        FluidSim(Context& context, AssetManager& assets, const FluidSimInfo& info);

        /// @brief One dye's image, its views, its descriptor sets, and its dissipation.
        struct Dye
        {
            /// @brief The caller's dye image.
            Ref<Image> Field;
            /// @brief The whole-image view, bound both sampled and as a storage image.
            Ref<ImageView> View;
            /// @brief Advects the dye into the shared scratch.
            Ref<DescriptorSet> AdvectSet;
            /// @brief Moves the scratch back into the dye, scaled by the survival factor.
            Ref<DescriptorSet> StoreSet;
            /// @brief The store pipeline matching the dye's format.
            Ref<ComputePipeline> StorePipeline;
            /// @brief Fraction of the dye lost per unit of simulated time.
            f32 Dissipation = 0.0f;
        };

        /// @brief Records the velocity half of a step: advect, forces, and the projection.
        void RecordVelocity(CommandBuffer& cmd);

        /// @brief Records the pressure solve and the gradient subtraction.
        void RecordProjection(CommandBuffer& cmd);

        /// @brief Records one dye's advection and its store back into the caller's image.
        void RecordDye(CommandBuffer& cmd, const Dye& dye);

        /// @brief Binds a compute pipeline with its set-1 descriptor set and pushes the params.
        void Bind(CommandBuffer& cmd, const Ref<ComputePipeline>& pipeline,
                  const Ref<DescriptorSet>& set, f32 scale, u32 flags) const;

        /// @brief Dispatches one workgroup per 8x8 tile of the grid.
        void DispatchGrid(CommandBuffer& cmd) const;

        /// @brief The context the solver's transients are created on.
        Context& m_Context;
        /// @brief Debug name the owned resources are named after.
        string m_Name;
        /// @brief The grid extent, taken from the velocity field.
        uvec2 m_Extent{0, 0};

        /// @brief The caller's velocity field.
        Ref<Image> m_Velocity;
        /// @brief The velocity field's whole-image view.
        Ref<ImageView> m_VelocityView;
        /// @brief The caller's relaxation target, or a 1x1 stand-in when none was supplied.
        Ref<ImageView> m_TargetView;
        /// @brief The caller's damping mask, or a 1x1 stand-in when none was supplied.
        Ref<ImageView> m_DampingView;
        /// @brief The 1x1 image standing in for an absent optional field.
        Ref<Image> m_Placeholder;
        /// @brief The view of the stand-in image.
        Ref<ImageView> m_PlaceholderView;

        /// @brief The rgba32f scratch every advection writes into, shared by velocity and dyes.
        Ref<Image> m_Scratch;
        /// @brief The scratch's view.
        Ref<ImageView> m_ScratchView;
        /// @brief The curl field confinement steers by.
        Ref<Image> m_Curl;
        /// @brief The curl field's view.
        Ref<ImageView> m_CurlView;
        /// @brief The divergence field, the Poisson solve's right-hand side.
        Ref<Image> m_Divergence;
        /// @brief The divergence field's view.
        Ref<ImageView> m_DivergenceView;
        /// @brief The two pressure images the Jacobi iteration ping-pongs between.
        Ref<Image> m_Pressure[2];
        /// @brief The pressure images' views.
        Ref<ImageView> m_PressureView[2];
        /// @brief One metric scale per grid row, all ones when the caller supplied none.
        Ref<Buffer> m_RowMetric;

        /// @brief The set layout every kernel family's own layout is built from.
        Ref<DescriptorSetLayout> m_AdvectSetLayout;
        /// @brief The store kernels' set layout.
        Ref<DescriptorSetLayout> m_StoreSetLayout;
        /// @brief The curl and divergence kernels' shared set layout.
        Ref<DescriptorSetLayout> m_FieldSetLayout;
        /// @brief The Jacobi kernel's set layout.
        Ref<DescriptorSetLayout> m_JacobiSetLayout;
        /// @brief The force kernel's set layout.
        Ref<DescriptorSetLayout> m_ForcesSetLayout;
        /// @brief The gradient kernel's set layout.
        Ref<DescriptorSetLayout> m_GradientSetLayout;

        /// @brief The pipeline layouts, one per set layout, all carrying the same push block.
        Ref<PipelineLayout> m_AdvectLayout;
        /// @brief The store kernels' pipeline layout.
        Ref<PipelineLayout> m_StoreLayout;
        /// @brief The curl and divergence kernels' pipeline layout.
        Ref<PipelineLayout> m_FieldLayout;
        /// @brief The Jacobi kernel's pipeline layout.
        Ref<PipelineLayout> m_JacobiLayout;
        /// @brief The force kernel's pipeline layout.
        Ref<PipelineLayout> m_ForcesLayout;
        /// @brief The gradient kernel's pipeline layout.
        Ref<PipelineLayout> m_GradientLayout;

        /// @brief Advection pipeline (one, since every advection writes the rgba32f scratch).
        Ref<ComputePipeline> m_AdvectPipeline;
        /// @brief Curl pipeline.
        Ref<ComputePipeline> m_CurlPipeline;
        /// @brief Divergence pipeline.
        Ref<ComputePipeline> m_DivergencePipeline;
        /// @brief Jacobi pipeline.
        Ref<ComputePipeline> m_JacobiPipeline;
        /// @brief Force pipeline matching the velocity field's format.
        Ref<ComputePipeline> m_ForcesPipeline;
        /// @brief Gradient pipeline matching the velocity field's format.
        Ref<ComputePipeline> m_GradientPipeline;

        /// @brief Advects the velocity field into the scratch.
        Ref<DescriptorSet> m_AdvectVelocitySet;
        /// @brief Reads the advected velocity, writes the curl field.
        Ref<DescriptorSet> m_CurlSet;
        /// @brief Reads the curl and the advected velocity, writes the caller's velocity.
        Ref<DescriptorSet> m_ForcesSet;
        /// @brief Reads the caller's velocity, writes the divergence field.
        Ref<DescriptorSet> m_DivergenceSet;
        /// @brief The two Jacobi sets, indexed by which pressure image is the source.
        Ref<DescriptorSet> m_JacobiSet[2];
        /// @brief The two gradient sets, indexed by which pressure image holds the solution.
        Ref<DescriptorSet> m_GradientSet[2];

        /// @brief The dye fields, in the order the caller supplied them.
        vector<Dye> m_Dyes;

        /// @brief Boundary behaviour of the x axis.
        FluidWrap m_WrapX = FluidWrap::Periodic;
        /// @brief Boundary behaviour of the y axis.
        FluidWrap m_WrapY = FluidWrap::Clamped;
        /// @brief How much simulated time one step advances.
        f32 m_TimeStep = 1.0f;
        /// @brief Jacobi iterations per projection.
        u32 m_JacobiIterations = DefaultFluidJacobiIterations;
        /// @brief Vorticity confinement strength.
        f32 m_Confinement = 0.0f;
        /// @brief Rate at which velocity relaxes toward the target field.
        f32 m_RelaxationRate = 0.0f;
        /// @brief Fraction of velocity lost per unit of simulated time.
        f32 m_VelocityDissipation = 0.0f;
        /// @brief Whether a relaxation target was supplied.
        bool m_HasTarget = false;
        /// @brief Whether a damping mask was supplied.
        bool m_HasDamping = false;
        /// @brief Steps recorded over this solver's lifetime.
        u64 m_StepCount = 0;
    };

    /// @brief Folds a texel coordinate into [0, extent) under a wrap mode.
    ///
    /// The CPU reference for the fold every kernel applies: a periodic axis wraps (negatives
    /// included), a clamped axis clamps to the outermost texel.
    /// @param coord  The coordinate to fold, possibly outside the grid.
    /// @param extent The axis length in texels; must be non-zero.
    /// @param wrap   The axis's boundary behaviour.
    /// @return The folded coordinate, in [0, extent).
    [[nodiscard]] i32 FoldFluidTexel(i32 coord, u32 extent, FluidWrap wrap);

    /// @brief The grid position a semi-Lagrangian step back-traces a texel's centre to.
    ///
    /// The CPU reference for what the advection kernel computes before it taps: the texel's
    /// centre less one timestep of velocity, with the x component scaled by the row's metric.
    /// @param texel     The texel being advected.
    /// @param velocity  The velocity sampled at that texel, in grid cells per unit time.
    /// @param timeStep  How much simulated time the step advances.
    /// @param rowMetric The row's metric scale.
    /// @return The back-traced position in grid coordinates (texel centres at i + 0.5).
    [[nodiscard]] vec2 FluidBackTrace(uvec2 texel, vec2 velocity, f32 timeStep, f32 rowMetric);

    /// @brief The bilinear tap the advection kernel reads at a back-traced position.
    ///
    /// The CPU reference for the hand-filtered tap: four folded texel loads combined by the
    /// fractional position. A caller supplies the loads, so the same reference serves a field of
    /// any channel count.
    /// @param position The back-traced grid position.
    /// @param extent   The grid extent in texels.
    /// @param wrapX    Boundary behaviour of the x axis.
    /// @param wrapY    Boundary behaviour of the y axis.
    /// @param load     Reads a field texel; only already-folded coordinates are passed to it.
    /// @return The filtered value.
    [[nodiscard]] vec4 SampleFluidBilinear(vec2 position, uvec2 extent, FluidWrap wrapX,
                                           FluidWrap wrapY, const function<vec4(ivec2)>& load);
}
