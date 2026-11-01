#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/Atmosphere.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Types.h>

#include <array>
#include <span>

namespace Veng
{
    class AssetManager;
    class MaterialInstance;
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class GraphicsPipeline;
    class ShaderModule;
    class Image;
    class ImageView;
    class Sampler;
    class Buffer;
    class ComputePipeline;
    class DescriptorSet;
    class DescriptorSetLayout;
    class GeneratedTextureService;

    /// @brief A caller-chosen identity for a generated-texture job (mirrors GeneratedTextureService).
    using GeneratedTextureKey = u64;

    /// @brief A caller-ownable baked sky radiance cube several renderers and worlds can share.
    ///
    /// Renders a Sky-domain material's fragment, or the procedural-atmosphere sky fragment, into
    /// the six faces of a radiance cube — the synchronous Bake with one fullscreen draw per face,
    /// the amortized RequestBake one scissor-clipped tile of a face per service tick — driving the
    /// same view-ray + depth-mask contract the direct sky passes use, but with a per-face fixed basis (six
    /// axis-aligned orientations matching the cube image-view layer order, so the twelve shared
    /// edges evaluate one world direction and the cube is seamless) and the background depth mask
    /// forced everywhere (a 1×1 far-plane stand-in depth image bound in the depth slot, so every
    /// pixel passes the background test). The fragment's param block and bound handles bind exactly
    /// as in the direct path; the fragment is unchanged and unaware it is baked.
    ///
    /// It is public and ownable so one bake can be **shared**: a SceneRenderer's SkyResolver owns one
    /// for a MaterialSky/AtmosphereSky in SkyMode::Baked, and a caller (a game service baking one sky
    /// for a system) owns one that every renderer showing that sky samples through a Scene `CubeSky`
    /// source — one bake for a main viewport and its capture probes, and for the several worlds a
    /// system spans, instead of a per-renderer re-bake on every world swap. `GetRevision` advances
    /// each time a fresh bake lands, so every renderer sampling the cube re-derives its IBL/SH from it
    /// without a per-renderer dirty signal.
    ///
    /// The displayed cube is created off the bindless registry the way the IBL radiance image is;
    /// the synchronous Bake records on the caller's dirty signal (the source swapped or its
    /// params/bound buffers changed), so a static sky bakes once. Exposes a Cube image view + a
    /// consumer descriptor set matching the IBL set's radiance binding, so the skybox pass samples
    /// the baked cube exactly as it samples a resident environment's radiance cube.
    ///
    /// A bake is also available **amortized**: RequestBake fills a second, scratch cube one tile of
    /// a face per GeneratedTextureService tick rather than six whole faces in one frame — so the
    /// per-frame GPU cost is bounded by tile area regardless of face size or fragment cost — and
    /// RecordAmortized copies
    /// the finished scratch cube into the displayed one on completion — so a dirty sky never blocks
    /// a frame on six fullscreen sky evaluations, and the previous cube stands until the new lands.
    /// The scratch cube is the reason VRAM is two full-size cubes, not one; only the displayed one
    /// carries the reduction chain down to the SH readback level, which the SH ambient tier reads
    /// back without blocking through RecordReductionMips + AsyncReadback.
    ///
    /// **An amortized bake may begin with a field.** A baked sky's cost is usually one smooth,
    /// expensive evaluation (a volume march, a multiple-scattering integral) while its look depends
    /// on full-resolution detail. A first layer with the Field role evaluates that expensive part at
    /// a reduced face resolution into up to four field cubes of physical quantities, not radiance,
    /// and every later layer reads them (Veng/sky_bake.slang) while it renders the picture at full
    /// resolution. A re-bake naming the field that already stands (RequestBake's `fieldKey`) skips
    /// the field and re-runs only the picture layers.
    class VE_API BakedSkyCube
    {
    public:
        /// @brief Creates the cube target, the 1×1 stand-in depth, and the per-face bake pipelines.
        /// @param context       The render context the resources are created on.
        /// @param consumerLayout The IBL consumer set layout the skybox path binds; the bake's
        ///                       consumer set is created against it, so the skybox pass can sample
        ///                       the baked cube through the same layout.
        /// @param sceneColorFormat The scene-color format the skybox pipeline is built against; the
        ///                       cube target renders at this format so the baked radiance round-trips
        ///                       the skybox sampler with no conversion.
        /// @param faceSize      Cube face edge length in texels.
        /// @param assets        The asset manager the field's solid-angle reduction shader loads
        ///                      from during this call, or null for a cube that never bakes a field
        ///                      layer. A field bake on a cube created without one is fatal.
        /// @return A new BakedSkyCube.
        static Unique<BakedSkyCube> Create(Context& context,
                                           const Ref<DescriptorSetLayout>& consumerLayout,
                                           Format sceneColorFormat, u32 faceSize,
                                           AssetManager* assets = nullptr);

        /// @brief Creates the descriptor-set layout a baked cube's consumer set (and the skybox
        ///        pipeline that binds it) is built against — the single definition of that shape.
        ///
        /// The skybox samples the baked radiance cube through the radiance/sampler bindings of the
        /// image-based-lighting consumer set, so a baked cube's consumer set and the IBL lighting set
        /// share one layout shape; this is where that shape lives. A caller that owns a BakedSkyCube
        /// without a SceneRenderer — a service that bakes one sky for several renderers to sample —
        /// creates a compatible layout here to pass to Create; two layouts with identical bindings
        /// are interchangeable, so the cube it fills binds into any renderer's skybox pipeline.
        /// @param context The render context the layout is created on.
        /// @return The consumer set layout.
        static Ref<DescriptorSetLayout> CreateConsumerSetLayout(Context& context);

        /// @brief Destroys all owned resources through the deferred-destruction retire path.
        ~BakedSkyCube();

        BakedSkyCube(const BakedSkyCube&) = delete;
        BakedSkyCube& operator=(const BakedSkyCube&) = delete;

        /// @brief Faces in the radiance cube.
        ///
        /// A synchronous Bake renders one fullscreen draw per face, each claiming its own view slot,
        /// so a caller checks the frame's remaining view budget against this before recording one. An
        /// amortized tick claims one slot per tile — one slot at a time, spread across frames.
        static constexpr u32 CubeFaces = 6;

        /// @brief Face edge length the SH readback reduces the cube to before reading it back.
        ///
        /// A spherical-harmonic ambient tier projects nine order-2 coefficients, which resolve
        /// angular features no finer than roughly 45°. A cube at 64 texels per face samples the
        /// sphere at ~1.4° — far finer than the basis can represent — so reading back the full
        /// display face (which a sky material sizes for point features like a star, thousands of
        /// texels per face) to produce those nine numbers is wasted bandwidth and CPU. The bake
        /// reduces the cube to this size through a box-averaging blit chain and reads that level.
        /// The projection's own per-texel solid-angle weight makes it a proper spherical integral
        /// at any face resolution, so the reduction is a partial evaluation of the same integral.
        static constexpr u32 ShReadbackFaceSize = 64;

        /// @brief The most field targets a field layer may write.
        static constexpr u32 MaxFieldTargets = 4;

        /// @brief The format every field target is stored in.
        static constexpr Format FieldFormat = Format::RGBA16Sfloat;

        /// @brief What a layer of a composited bake produces.
        enum class SkyBakeRole : u8
        {
            /// @brief Radiance composited into the cube by the layer's blend.
            Picture,
            /// @brief Physical quantities the later layers read, written into the field cubes
            ///        rather than the radiance cube. Only the first layer may be a field.
            Field,
        };

        /// @brief One layer of a composited bake: a material's fragment and how it blends over the
        ///        layers already in the cube.
        ///
        /// A layered bake renders its layers in order into each face — the first clearing the face,
        /// each later one Loading and blending over the accumulated result — so the finished cube is
        /// their composite. Splitting a heavy sky into layers (a dust backdrop, an over-attenuating
        /// nebula overlay, an additive star overlay) keeps each fragment's shader small, and the
        /// framebuffer blend does the compositing with no read of the in-progress cube: an emissive
        /// overlay uses `(ONE, SRC_ALPHA)` (add its light, attenuate the backdrop behind it by its
        /// alpha) and a pure light overlay uses Additive.
        ///
        /// A first layer with the Field role writes its material's targets into the field cubes
        /// instead (see SkyBakeRole::Field); the first Picture layer after it clears the face, and
        /// every later layer reads the field through Veng/sky_bake.slang.
        struct SkyBakeLayer
        {
            /// @brief The resident Sky-domain material whose fragment fills this layer.
            const MaterialInstance* Material = nullptr;
            /// @brief Whether the layer renders radiance (the default) or the bake's field.
            ///
            /// A Field layer's material declares "fieldTargets" at cook time and writes that many
            /// float4 targets; a Picture layer's declares none. Only the first layer may be a field,
            /// it bakes only through the amortized RequestBake, and at least one Picture layer
            /// follows it.
            SkyBakeRole Role = SkyBakeRole::Picture;
            /// @brief How many field targets a Field layer writes, from one to MaxFieldTargets.
            ///
            /// Must equal the count its material declares. Each is a FieldFormat cube at the face
            /// size over FaceSizeDivisor, with a full box-reduced mip chain. Ignored by a Picture
            /// layer.
            u32 FieldTargets = 1;
            /// @brief How this layer blends onto the layers beneath it. The first layer clears the
            ///        face, so its blend is typically Opaque; later layers Load and blend.
            BlendState Blend = BlendState::Opaque();
            /// @brief Bakes this layer at a reduced face resolution, then bilinear-upsamples it into
            ///        the full-resolution cube as the base the finer layers composite over.
            ///
            /// A power-of-two divisor of the face size (1 = full resolution, the default). A smooth,
            /// low-frequency base layer — one whose per-pixel fragment is expensive but whose result
            /// varies slowly across the sky — need not be evaluated at full face resolution: baking it
            /// into a coarser scratch cube and upsampling cuts its fragment count by the divisor
            /// squared while the detail layers stay full resolution. Only the **first** layer may set a
            /// divisor above 1: a Picture base is promoted into the cube by an upsampling blit that
            /// replaces (rather than blends) the region, and a Field base is the field cubes' face size,
            /// so a later layer — which Loads and blends over what is already there — must be full
            /// resolution.
            u32 FaceSizeDivisor = 1;

            /// @brief Edge length in texels of one amortization tile for this layer, or 0 for the
            ///        engine default.
            ///
            /// An amortized bake renders one scissor-clipped tile of a face per tick, so the tile size
            /// bounds one tick's fragment work — the granularity half of the caller's control, the
            /// companion to the per-tick cost that bounds how many ticks a frame runs. A layer whose
            /// per-pixel fragment is expensive (a volumetric backdrop) names a smaller tile so no
            /// single tick runs a whole heavy face; a cheap point/splat layer leaves it at the default.
            /// The tile need not divide the face — the last row and column are clamped to real texels.
            /// Zero (the default) takes the engine's default tile size.
            u32 TilePixels = 0;

            /// @brief The per-tick cost this layer declares to the generated-texture budget, or the
            ///        default weight of one.
            ///
            /// The pump spends a total cost budget per frame (see GeneratedTextureRequest::Cost), so a
            /// higher cost runs fewer of this bake's ticks per frame — the amortization half of the
            /// caller's control, the companion to TilePixels: TilePixels bounds one tick's work, Cost
            /// bounds how many run a frame. Each layer's tiles charge its own Cost, so a cheap overlay
            /// tick is not priced like an expensive backdrop's (GeneratedTextureRequest::CostPhases).
            /// The single steps between layers — a coarse base's promotion, a field's mips and sums —
            /// charge the default weight of one. A layer that names none takes the default weight.
            u32 Cost = 1;
        };

        /// @brief Records the six face renders of `material` into the radiance cube.
        ///
        /// The caller gates this on its sky dirty signal; each call overwrites the cube in place.
        /// Records into `cmd` before the graph that samples the cube. Uses the shared bindless
        /// registry for set 0 (the material's param block + the stand-in depth handle); writes each
        /// face's view constants into the registry's view-constants ring, so the material fragment's
        /// per-pixel view-ray reconstruction lands on the face's fixed basis.
        /// @param cmd      The command buffer the face renders are recorded into.
        /// @param material The resident Sky-domain material to bake.
        /// @pre material's domain is MaterialDomain::Sky and its param block is uploaded this frame.
        /// @pre The frame's remaining view budget covers CubeFaces slots — the caller reserves them.
        void Bake(CommandBuffer& cmd, const MaterialInstance& material);

        /// @brief Records the six face renders of a composited layer stack into the radiance cube.
        ///
        /// The layered counterpart to the single-material Bake: renders `layers` in order into each
        /// face, the first clearing it and each later one blending over the result. Records into
        /// `cmd` before the graph that samples the cube.
        /// @param cmd    The command buffer the face renders are recorded into.
        /// @param layers The ordered layer stack; each layer's material must be resident with its
        ///               param block uploaded this frame.
        /// @pre Every layer's material is a MaterialDomain::Sky material.
        /// @pre No layer is a Field or sets FaceSizeDivisor above 1; both need RequestBake.
        /// @pre The frame's remaining view budget covers CubeFaces slots — the caller reserves them.
        void Bake(CommandBuffer& cmd, std::span<const SkyBakeLayer> layers);

        /// @brief Records the six face renders of the procedural atmosphere into the radiance cube.
        ///
        /// The atmosphere sibling of Bake: runs the atmosphere sky fragment (via `pipeline`, built
        /// against the cube-face format) into each face with the face's fixed basis, binding the
        /// renderer's atmosphere LUT set at set 1 and pushing the sun direction, intensity, and
        /// atmosphere parameters — the same push the direct atmosphere pass performs, so the
        /// fragment is unchanged and unaware it is baked. The far-plane stand-in depth satisfies its
        /// background mask. The caller gates this on the sun/params dirty signal; each call
        /// overwrites the cube in place. Records into `cmd` before the graph that samples the cube.
        /// @param cmd           The command buffer the face renders are recorded into.
        /// @param pipeline      The atmosphere sky pipeline, built against the cube-face format.
        /// @param atmosphereSet The renderer's atmosphere LUT set (scattering/transmittance/sampler).
        /// @param atmosphere    The atmosphere parameters the LUTs were generated for.
        /// @param sunDirection  The normalized toward-sun direction.
        /// @param intensity     Scales the baked sky radiance + sun disc.
        /// @pre `atmosphereSet`'s LUTs were generated for `atmosphere` this frame.
        /// @pre The frame's remaining view budget covers CubeFaces slots — the caller reserves them.
        void BakeAtmosphere(CommandBuffer& cmd, const Ref<GraphicsPipeline>& pipeline,
                            const Ref<DescriptorSet>& atmosphereSet, const Atmosphere& atmosphere,
                            const vec3& sunDirection, f32 intensity);

        /// @brief Reduces the baked cube to the SH readback level through a box-averaging blit chain.
        ///
        /// Records a halving blit per mip step from mip 0 down to `GetShReadbackMipLevel()`, each
        /// step a linear 2× downsample (a proper 2×2 box average) across all six layers, so the
        /// readback level holds a spherical-integral-preserving reduction of the display face. Leaves
        /// mip 0 in a sampled layout for the skybox pass and the readback level in a transfer layout
        /// for the copy that follows. A no-op when the face already sits at or below
        /// `ShReadbackFaceSize`. Records into the frame command buffer after the display bake, so the
        /// deferred SH readback reads the cube that bake produced rather than baking a second one.
        /// @param cmd The command buffer the reduction blits are recorded into.
        void RecordReductionMips(CommandBuffer& cmd);

        /// @brief Requests an amortized bake of `material` into the scratch cube through the service.
        ///
        /// The non-blocking counterpart to Bake: instead of recording all six face draws into one
        /// frame, it submits a GeneratedTextureService job that fills the scratch cube one tile of a
        /// face per tick, spread across the frame budget, so the per-frame GPU cost is bounded by
        /// tile area rather than a whole heavy face. The displayed cube keeps being sampled untouched
        /// until the job completes, at which point RecordAmortized() copies the finished scratch cube
        /// into the displayed cube in one step. A request supersedes any bake still in flight, so a
        /// re-bake on a fresh dirty signal replaces the pending one rather than queuing behind it.
        /// @param service  The service the job runs through.
        /// @param material The resident Sky-domain material to bake.
        /// @pre material's domain is MaterialDomain::Sky and its param block is uploaded this frame.
        void RequestBake(GeneratedTextureService& service, const MaterialInstance& material);

        /// @brief Requests an amortized composited bake of a layer stack into the scratch cube.
        ///
        /// The layered counterpart to the single-material RequestBake: the job fills the scratch cube
        /// one tile of one layer's face per tick — every layer of the cube marched at bake time, the
        /// layers composited by the framebuffer blend rather than by a re-read of the cube — spread
        /// across the frame budget, so a heavy multi-layer sky never blocks a frame. RecordAmortized
        /// copies the finished composite into the displayed cube on completion. Supersedes any bake
        /// still in flight.
        ///
        /// A stack beginning with a Field layer runs in two phases, each tile charging its own
        /// layer's Cost: every tile of the field, one step that builds each field target's mip chain
        /// and its solid-angle sums and makes the field readable, then the Picture layers' tiles.
        /// @param service  The service the job runs through.
        /// @param layers   The ordered layer stack; each layer's material must stay resident with its
        ///                 param block uploaded until the bake completes.
        /// @param cacheKey The key the finished composite is stored under in the service's attached
        ///                 cache, or empty for an uncached bake. The caller composes it from
        ///                 everything the composite depends on that the cache's generation does not;
        ///                 a hit restores the stored texels and the layer march never runs. A hit
        ///                 leaves no field standing.
        /// @param fieldKey The identity of the field a Field layer bakes, or empty. When it names
        ///                 the field standing in the field cubes — one whose every tile, mips and
        ///                 sums have landed, at the same face size and target count — the field
        ///                 phase is skipped and only the Picture layers run. Starting a field phase
        ///                 clears the standing field, so a bake superseded mid-field leaves none; an
        ///                 empty key always runs the field. Ignored by a stack with no field.
        /// @pre Every layer's material is a MaterialDomain::Sky material.
        /// @pre A Field layer is the first layer, is followed by a Picture layer, and the cube was
        ///      created with an AssetManager.
        void RequestBake(GeneratedTextureService& service, std::span<const SkyBakeLayer> layers,
                         string cacheKey = {}, string fieldKey = {});

        /// @brief Requests an amortized bake of the procedural atmosphere into the scratch cube.
        ///
        /// The atmosphere sibling of RequestBake: fills the scratch cube one tile of an atmosphere
        /// face per service tick, binding the LUT set at set 1 and pushing the sun/params exactly as
        /// BakeAtmosphere does. Supersedes any bake still in flight.
        /// @param service       The service the job runs through.
        /// @param pipeline      The atmosphere sky pipeline, built against the cube-face format.
        /// @param atmosphereSet The renderer's atmosphere LUT set (scattering/transmittance/sampler).
        /// @param atmosphere    The atmosphere parameters the LUTs were generated for.
        /// @param sunDirection  The normalized toward-sun direction.
        /// @param intensity     Scales the baked sky radiance + sun disc.
        /// @pre `atmosphereSet`'s LUTs were generated for `atmosphere` and stay resident until done.
        void RequestBakeAtmosphere(GeneratedTextureService& service,
                                   const Ref<GraphicsPipeline>& pipeline,
                                   const Ref<DescriptorSet>& atmosphereSet,
                                   const Atmosphere& atmosphere, const vec3& sunDirection,
                                   f32 intensity);

        /// @brief Tears down any amortized bake in flight, e.g. when the source stops being baked.
        void AbandonBake();

        /// @brief Whether an amortized bake is still filling the scratch cube.
        ///
        /// True from RequestBake/RequestBakeAtmosphere until the job's last tick has run. A caller
        /// deferring the lighting-tier readback until the bake lands polls this.
        [[nodiscard]] bool IsBakePending() const { return m_BakeState == BakeState::Pending; }

        /// @brief Copies a just-landed amortized bake into the displayed cube, once.
        ///
        /// Records nothing until an amortized bake has completed; on the frame after completion it
        /// copies the finished scratch cube into the displayed cube (leaving it in a sampled layout)
        /// and returns true, so the caller knows to refresh whatever it derives from the cube (the
        /// SH readback, the IBL convolution). Records into `cmd` before the graph that samples the
        /// cube. A no-op returning false on every other frame.
        /// @param cmd The command buffer the copy is recorded into.
        /// @return True on the one frame the displayed cube was refreshed from a completed bake.
        bool RecordAmortized(CommandBuffer& cmd);

        /// @brief A revision that advances each time a completed bake lands in the displayed cube.
        ///
        /// RecordAmortized bumps it on the frame it copies a finished bake across. A renderer that
        /// samples this cube — its owner, or another renderer borrowing a shared one — re-derives
        /// whatever it convolves from the cube (the IBL split-sum maps, the SH ambient) when this
        /// moves, so a re-bake refreshes every consumer's lighting without a per-consumer dirty
        /// signal. Zero until the first bake lands.
        [[nodiscard]] u64 GetRevision() const { return m_Revision; }

        /// @brief Whether any bake has landed (else the displayed cube is the initial black clear).
        [[nodiscard]] bool IsBaked() const { return m_Revision != 0; }

        /// @brief The consumer descriptor set the skybox pass binds to sample the baked cube.
        ///
        /// Matches the IBL consumer set's radiance binding (binding 0 the cube, binding 4 the
        /// sampler), so the skybox pipeline samples it exactly as it samples the IBL radiance cube.
        [[nodiscard]] const Ref<DescriptorSet>& GetSet() const { return m_ConsumerSet; }

        /// @brief The cube image view of the baked radiance, exposed for tests.
        [[nodiscard]] const Ref<ImageView>& GetCubeView() const { return m_CubeView; }

        /// @brief The owned radiance cube image, exposed for tests reading back the baked faces.
        [[nodiscard]] const Ref<Image>& GetCubeImage() const { return m_CubeImage; }

        /// @brief The cube face edge length in texels.
        [[nodiscard]] u32 GetFaceSize() const { return m_FaceSize; }

        /// @brief The mip level RecordReductionMips reduces to, and the SH readback reads back.
        [[nodiscard]] u32 GetShReadbackMipLevel() const { return m_ShReadbackMip; }

        /// @brief The face edge length of the reduced readback level (`GetFaceSize()` >> this level).
        [[nodiscard]] u32 GetShReadbackFaceSize() const { return m_FaceSize >> m_ShReadbackMip; }

        /// @brief Tiles a face is divided into for an amortized bake (one per tick).
        ///
        /// `TilesPerAxis²`, where `TilesPerAxis = ceil(GetFaceSize() / tile)`. A face at or below the
        /// tile size is one tile, so an amortized bake then fills one whole face per tick. Exposed for
        /// tests.
        [[nodiscard]] u32 GetTilesPerFace() const { return m_TilesPerFace; }

        /// @brief Total region fullscreen draws this bake has recorded over its lifetime.
        ///
        /// A synchronous Bake records one draw per face (CubeFaces); an amortized tick records one
        /// per tile (`CubeFaces * GetTilesPerFace()` over a full fill). A caller reads the delta
        /// across a dirty signal to see how many draws a re-bake cost. Exposed for tests.
        [[nodiscard]] u64 GetFaceRendersRecorded() const { return m_FaceRendersRecorded; }

        /// @brief Field-layer tile draws this bake has recorded over its lifetime.
        ///
        /// Counted within GetFaceRendersRecorded too. A re-composite over a standing field adds
        /// none. Exposed for tests.
        [[nodiscard]] u64 GetFieldTilesRecorded() const { return m_FieldTilesRecorded; }

        /// @brief Whether a field stands in the field cubes, readable by a re-composite.
        [[nodiscard]] bool IsFieldStanding() const { return m_FieldStanding; }

        /// @brief The face edge length of the field cubes in texels, or 0 before any field bake.
        [[nodiscard]] u32 GetFieldFaceSize() const { return m_FieldFaceSize; }

    private:
        BakedSkyCube(Context& context, const Ref<DescriptorSetLayout>& consumerLayout,
                     Format sceneColorFormat, u32 faceSize, AssetManager* assets);

        /// @brief Builds the per-face graphics pipeline from the bound material's shaders, once per identity.
        void EnsurePipeline(const MaterialInstance& material);

        /// @brief Builds one bake pipeline from a material's shaders against the cube-face format.
        ///
        /// The shared body of EnsurePipeline and EnsureLayerPipelines: the material's own fragment +
        /// the fullscreen vertex, its pipeline layout, and the given blend state on the one cube-face
        /// color attachment, or on each of a field's attachments.
        /// @param material     The Sky-domain material whose shaders + layout the pipeline is built
        ///                     from.
        /// @param blend        The blend state for every color attachment.
        /// @param fieldTargets 0 for the one cube-face attachment, else that many FieldFormat field
        ///                     attachments.
        /// @return The bake pipeline.
        [[nodiscard]] Ref<GraphicsPipeline> CreateBakePipeline(const MaterialInstance& material,
                                                               const BlendState& blend,
                                                               u32 fieldTargets = 0);

        /// @brief Builds/refreshes the per-layer bake pipelines to match a layer stack, once per change.
        ///
        /// Sizes m_LayerPipelines to the stack and rebuilds a layer's pipeline when its material's
        /// fragment module or its blend state differs from what the slot last held, so a re-request of
        /// the same layers reuses the pipelines instead of recompiling the sky shaders.
        /// @param layers The ordered layer stack.
        void EnsureLayerPipelines(std::span<const SkyBakeLayer> layers);

        /// @brief Ensures the coarse scratch cube exists at the given face size, allocating on change.
        ///
        /// The reduced-resolution base layer renders into this cube before it is upsampled into the
        /// full-resolution scratch. Reallocates only when the requested face size differs from the last,
        /// so a caller baking at a fixed divisor pays one allocation.
        /// @param coarseFaceSize The coarse cube's face edge length in texels (m_FaceSize / divisor).
        void EnsureCoarseScratch(u32 coarseFaceSize);

        /// @brief Bilinear-upsamples the coarse scratch cube into the full scratch cube.
        ///
        /// The promotion of a reduced-resolution base layer: one blit upsamples all six coarse faces
        /// into the full-resolution scratch faces with a linear filter, replacing their contents — the
        /// base the finer layers then Load and blend over. Whole-image transitions (not per-face), so
        /// it composes with the service's per-tick whole-image producer-access transition.
        /// @param cmd The command buffer the blit is recorded into.
        void RecordCoarsePromote(CommandBuffer& cmd);

        /// @brief Ensures the first `count` field targets exist at the given face size.
        ///
        /// A face-size change recreates every target (and leaves no field standing); a larger count
        /// creates the missing targets and keeps the rest.
        /// @param faceSize The field cubes' face edge length in texels.
        /// @param count    The targets the field layer writes.
        void EnsureFieldTargets(u32 faceSize, u32 count);

        /// @brief Makes a just-baked field readable: builds each target's box-reduced mip chain,
        ///        writes its per-face solid-angle sums, and leaves every target sampled.
        /// @param cmd   The command buffer the reductions are recorded into.
        /// @param count The targets the field layer wrote.
        void RecordFieldFinish(CommandBuffer& cmd, u32 count);

        /// @brief Claims (or reuses) the view slot carrying a face's basis, for a tile about to draw.
        ///
        /// Every tile of a face writes the identical face basis, so one view slot serves them all: a
        /// fresh slot is claimed and written at the face's first tile (and whenever the registry's
        /// view slots have reset since the last claim, so an amortized face split across frames
        /// re-claims each frame), and reused for the face's remaining tiles within that recording.
        /// This bounds a whole-cube bake to CubeFaces view slots per frame regardless of tile count or
        /// tick budget.
        /// @param face      The cube face index, selecting its fixed view-ray basis.
        /// @param faceFirst Whether this is the first tile of its face (forces a fresh claim).
        /// @param fieldTargets The standing field's target count the slot carries for a layer that
        ///                  reads it, or 0 for a slot that reads none.
        /// @return The view-constants index the tile pushes.
        u32 AcquireFaceViewSlot(u32 face, bool faceFirst, u32 fieldTargets = 0);

        /// @brief Records one material tile render into a target face view.
        ///
        /// The shared body of every material bake — the display cube (Bake, one whole-face tile) and
        /// the scratch cube (the amortized tick, one tile of a face). Claims one view slot for the
        /// face basis and draws the material fragment through it with the viewport spanning the whole
        /// face (so the SV_Position → direction mapping is the full-face one) but the render-area +
        /// scissor clipped to the tile, so only the tile's texels shade. Leaves the face in a
        /// color-attachment layout; the caller owns any whole-cube transition after the loop.
        /// @param cmd        The command buffer the tile render is recorded into.
        /// @param pipeline   The bake pipeline to bind (its blend state carries the layer's compositing).
        /// @param material   The resident Sky-domain material to bake; supplies the selector it pushes.
        /// @param faceViews  The single-layer color targets for this face, one per attachment.
        /// @param faceSize   The face edge length in texels (the viewport extent).
        /// @param face       The cube face index, selecting its fixed view-ray basis.
        /// @param tileOffset The tile's top-left pixel offset within the face.
        /// @param tileExtent The tile's pixel extent (clamped to the face on the last row/column).
        /// @param clear      Clear the tile's render-area (the first layer's first tile) or Load it.
        /// @param fieldTargets The standing field's target count the layer reads, or 0.
        void RecordMaterialRegion(CommandBuffer& cmd, const Ref<GraphicsPipeline>& pipeline,
                                  const MaterialInstance& material,
                                  std::span<const Ref<ImageView>> faceViews, u32 faceSize, u32 face,
                                  uvec2 tileOffset, uvec2 tileExtent, bool clear,
                                  u32 fieldTargets = 0);

        /// @brief Records one atmosphere tile render into a target face view.
        ///
        /// The atmosphere sibling of RecordMaterialRegion, shared by the display cube and the scratch
        /// cube.
        /// @param cmd           The command buffer the tile render is recorded into.
        /// @param pipeline      The atmosphere sky pipeline, built against the cube-face format.
        /// @param atmosphereSet The renderer's atmosphere LUT set.
        /// @param atmosphere    The atmosphere parameters the LUTs were generated for.
        /// @param sunDirection  The normalized toward-sun direction.
        /// @param intensity     Scales the baked sky radiance + sun disc.
        /// @param faceView      The single-layer color target for this face.
        /// @param faceSize      The face edge length in texels (the viewport extent).
        /// @param face          The cube face index, selecting its fixed view-ray basis.
        /// @param tileOffset    The tile's top-left pixel offset within the face.
        /// @param tileExtent    The tile's pixel extent (clamped to the face on the last row/column).
        /// @param clear         Clear the tile's render-area (the face's first tile) or Load it.
        void RecordAtmosphereRegion(CommandBuffer& cmd, const Ref<GraphicsPipeline>& pipeline,
                                    const Ref<DescriptorSet>& atmosphereSet,
                                    const Atmosphere& atmosphere, const vec3& sunDirection,
                                    f32 intensity, const Ref<ImageView>& faceView, u32 faceSize,
                                    u32 face, uvec2 tileOffset, uvec2 tileExtent, bool clear);

        /// @brief Supersedes any in-flight/landed bake and resets the amortization state to idle.
        void CancelBake();

        /// @brief The lifecycle of an amortized bake filling the scratch cube.
        enum class BakeState
        {
            Idle,    ///< No amortized bake outstanding.
            Pending, ///< A job is filling the scratch cube, face by face.
            Landed,  ///< The job completed; RecordAmortized() will copy the result across.
        };

        Context& m_Context;
        Format m_SceneColorFormat;
        u32 m_FaceSize;
        u32 m_ShReadbackMip; // halving steps from the face down to ShReadbackFaceSize
        u32 m_TilesPerAxis;  // tiles along one face axis, ceil(m_FaceSize / tile size)
        u32 m_TilesPerFace;  // m_TilesPerAxis², the amortized tick count per face
        u64 m_FaceRendersRecorded = 0;
        u64 m_FieldTilesRecorded = 0;

        Ref<Image> m_CubeImage;
        Ref<ImageView> m_CubeView; // Cube view, mip 0 only — sampled by the skybox pass
        std::array<Ref<ImageView>, CubeFaces>
            m_FaceViews; // one single-layer mip-0 view per face — rendered
        // One all-six-layers view per mip from 0 to m_ShReadbackMip — for the reduction/readback
        // layout transitions (the raw blit itself addresses subresources directly).
        vector<Ref<ImageView>> m_MipViews;

        // The scratch cube an amortized bake fills a face at a time, held off the displayed cube so
        // the skybox samples the last completed bake untouched while the next fills. Mip-0 only: it
        // is copied into the displayed cube (which carries the reduction chain) on completion.
        Ref<Image> m_ScratchImage;
        Ref<ImageView> m_ScratchView; // all six layers, mip 0 — for the copy's layout transitions
        std::array<Ref<ImageView>, CubeFaces> m_ScratchFaceViews; // one single-layer view per face

        // The coarse scratch cube a reduced-resolution base layer (SkyBakeLayer::FaceSizeDivisor > 1)
        // renders into, upsampled into m_ScratchImage by a blit before the finer layers composite over
        // it. Allocated lazily at the divisor a bake first asks for, and reallocated only if a later
        // bake asks for a different one — so a caller baking at a fixed divisor allocates it once.
        Ref<Image> m_CoarseScratchImage;
        Ref<ImageView>
            m_CoarseScratchView; // all six layers, mip 0 — for the promote blit's transition
        std::array<Ref<ImageView>, CubeFaces> m_CoarseScratchFaceViews;
        u32 m_CoarseFaceSize =
            0; // the coarse cube's face edge, 0 until a divisor > 1 bake allocates it

        /// @brief One field target: a FieldFormat cube with a full mip chain.
        struct FieldTarget
        {
            /// @brief The cube image.
            Ref<Image> Cube;
            /// @brief Every mip as a cube: registered bindless for the later layers to sample.
            Ref<ImageView> CubeView;
            /// @brief Mip 0 as a six-layer array: what the solid-angle sum reads.
            Ref<ImageView> SumView;
            /// @brief Mip 0 of each face as a 2D view: what the field layer renders.
            std::array<Ref<ImageView>, CubeFaces> FaceViews;
            /// @brief Each mip as a six-layer array, for the mip chain's transitions.
            vector<Ref<ImageView>> MipViews;
            /// @brief The cube's bindless slot.
            CubeHandle Handle;
            /// @brief The solid-angle sum's set: SumView and the sum buffer.
            Ref<DescriptorSet> SumSet;
        };

        /// @brief The field targets created so far; the first m_FieldTargetsCreated are live.
        std::array<FieldTarget, MaxFieldTargets> m_FieldTargets;
        /// @brief How many field targets exist.
        u32 m_FieldTargetsCreated = 0;
        /// @brief The field cubes' face edge in texels; 0 until a field bake allocates them.
        u32 m_FieldFaceSize = 0;
        /// @brief The field cubes' mip count.
        u32 m_FieldMipCount = 0;
        /// @brief The per-target, per-face solid-angle sums (MaxFieldTargets × CubeFaces float4).
        Ref<Buffer> m_FieldSums;
        /// @brief The sum buffer's storage-buffer slot.
        StorageBufferHandle m_FieldSumsHandle;
        /// @brief The trilinear clamp sampler the later layers read the field through.
        SamplerHandle m_FieldSamplerHandle;
        /// @brief The solid-angle sum's descriptor-set layout.
        Ref<DescriptorSetLayout> m_FieldSumSetLayout;
        /// @brief The solid-angle sum's compute pipeline.
        Ref<ComputePipeline> m_FieldSumPipeline;
        /// @brief Whether a complete field stands in the field cubes.
        bool m_FieldStanding = false;
        /// @brief The standing field's key; empty when none stands or it was baked unkeyed.
        string m_FieldKey;
        /// @brief The standing field's target count.
        u32 m_FieldStandingTargets = 0;

        // The amortized bake's service job key (unique per instance) and its lifecycle state. The
        // material path also records the resolved instance's identity + revision so a re-request for
        // the same content is idempotent while a genuine change supersedes.
        GeneratedTextureKey m_JobKey;
        BakeState m_BakeState = BakeState::Idle;

        // Advances each time RecordAmortized copies a finished bake into the displayed cube; a
        // consumer re-derives its IBL/SH from the cube when it moves. See GetRevision.
        u64 m_Revision = 0;

        Ref<Image> m_DepthImage;     // 1×1 stand-in, holds the far-plane value (1.0)
        Ref<ImageView> m_DepthView;  // sampled as a color texture by the fragment's depth read
        TextureHandle m_DepthHandle; // bindless slot for the stand-in depth
        // the shared bindless slot the fragment reads the stand-in depth through
        SamplerHandle m_DepthSamplerHandle;

        Ref<DescriptorSet> m_ConsumerSet; // the skybox pass binds this to sample the baked cube

        Ref<GraphicsPipeline> m_Pipeline; // the material's fragment against the cube-face format
        // The fragment module the pipeline was built from — the pipeline's one material-dependent
        // input, shared by every instance of a Sky material. Keyed on it (not the material's instance
        // index) so a fresh instance of the same material reuses the pipeline instead of recompiling
        // the sky shader. Non-owning: valid to compare only while m_Pipeline (which holds the module)
        // is alive.
        const ShaderModule* m_PipelineFragment = nullptr;

        // One cached bake pipeline per layer of a layered bake. Keyed on the layer's fragment module
        // and blend, so a re-request of the same stack reuses the pipelines rather than recompiling
        // the sky shaders. Non-owning Fragment pointer, compared only while its Pipeline is alive.
        struct LayerPipeline
        {
            Ref<GraphicsPipeline> Pipeline;
            const ShaderModule* Fragment = nullptr;
            BlendState Blend;
            u32 FieldTargets = 0;
        };
        vector<LayerPipeline> m_LayerPipelines;

        // The six per-face InvViewProj matrices: each maps a fullscreen [0,1]² UV to the face's
        // world direction, matching the cube image-view layer order so shared edges agree.
        std::array<mat4, CubeFaces> m_FaceInvViewProj;

        /// @brief The registry view epoch the shared face slot was claimed under.
        ///
        /// A tile reuses the slot while the epoch (one frame or one immediate recording) and the face
        /// match, so a bake spends one view slot per face per frame rather than one per tile.
        u64 m_LastTileViewEpoch = ~0ULL;
        /// @brief The face the shared slot carries the basis of.
        u32 m_LastTileFace = ~0u;
        /// @brief The field target count the shared slot carries.
        u32 m_LastTileFieldTargets = ~0u;
        /// @brief The view-constants index of the shared face slot.
        u32 m_LastTileViewIndex = 0;
    };
}
