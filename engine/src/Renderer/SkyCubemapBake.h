#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/Atmosphere.h>
#include <Veng/Renderer/BindlessRegistry.h>

#include <array>

namespace Veng
{
    class MaterialInstance;
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class GraphicsPipeline;
    class Image;
    class ImageView;
    class Sampler;
    class DescriptorSet;
    class DescriptorSetLayout;
    class GeneratedTextureService;

    /// @brief A caller-chosen identity for a generated-texture job (mirrors GeneratedTextureService).
    using GeneratedTextureKey = u64;

    /// @brief Bakes a fullscreen sky evaluation into an owned radiance cubemap the skybox path samples.
    ///
    /// Renders a Sky-domain material's fragment, or the procedural-atmosphere sky fragment, into
    /// the six faces of a radiance cube — one fullscreen draw per face — driving the same view-ray
    /// + depth-mask contract the direct sky passes use, but with a per-face fixed basis (six
    /// axis-aligned orientations matching the cube image-view layer order, so the twelve shared
    /// edges evaluate one world direction and the cube is seamless) and the background depth mask
    /// forced everywhere (a 1×1 far-plane stand-in depth image bound in the depth slot, so every
    /// pixel passes the background test). The fragment's param block and bound handles bind exactly
    /// as in the direct path; the fragment is unchanged and unaware it is baked.
    ///
    /// The displayed cube is created off the bindless registry the way the IBL radiance image is;
    /// the synchronous Bake records on the caller's dirty signal (the source swapped or its
    /// params/bound buffers changed), so a static sky bakes once. Exposes a Cube image view + a
    /// consumer descriptor set matching the IBL set's radiance binding, so the skybox pass samples
    /// the baked cube exactly as it samples a resident environment's radiance cube.
    ///
    /// A bake is also available **amortized**: RequestBake fills a second, scratch cube one face per
    /// GeneratedTextureService tick rather than six draws in one frame, and RecordAmortized copies
    /// the finished scratch cube into the displayed one on completion — so a dirty sky never blocks
    /// a frame on six fullscreen sky evaluations, and the previous cube stands until the new lands.
    /// The scratch cube is the reason VRAM is two full-size cubes, not one; only the displayed one
    /// carries the reduction chain down to the SH readback level, which the SH ambient tier reads
    /// back without blocking through RecordReductionMips + AsyncReadback.
    class SkyCubemapBake
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
        /// @return A new SkyCubemapBake.
        static Unique<SkyCubemapBake> Create(Context& context,
                                             const Ref<DescriptorSetLayout>& consumerLayout,
                                             Format sceneColorFormat, u32 faceSize);

        /// @brief Destroys all owned resources through the deferred-destruction retire path.
        ~SkyCubemapBake();

        SkyCubemapBake(const SkyCubemapBake&) = delete;
        SkyCubemapBake& operator=(const SkyCubemapBake&) = delete;

        /// @brief Faces in the radiance cube, and so the view slots one bake claims.
        ///
        /// A bake renders one fullscreen draw per face and each face claims its own view slot, so a
        /// caller checks the frame's remaining view budget against this before recording one.
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
        /// frame, it submits a GeneratedTextureService job that fills the scratch cube one face per
        /// tick, spread across the frame budget. The displayed cube keeps being sampled untouched
        /// until the job completes, at which point RecordAmortized() copies the finished scratch cube
        /// into the displayed cube in one step. A request supersedes any bake still in flight, so a
        /// re-bake on a fresh dirty signal replaces the pending one rather than queuing behind it.
        /// @param service  The service the job runs through.
        /// @param material The resident Sky-domain material to bake.
        /// @pre material's domain is MaterialDomain::Sky and its param block is uploaded this frame.
        void RequestBake(GeneratedTextureService& service, const MaterialInstance& material);

        /// @brief Requests an amortized bake of the procedural atmosphere into the scratch cube.
        ///
        /// The atmosphere sibling of RequestBake: fills the scratch cube one atmosphere face per
        /// service tick, binding the LUT set at set 1 and pushing the sun/params exactly as
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

        /// @brief Total per-face fullscreen draws this bake has recorded over its lifetime.
        ///
        /// One bake records CubeFaces of them; a caller reads the delta across a dirty signal to
        /// see whether a re-bake cost one bake (six) or two (twelve). Exposed for tests.
        [[nodiscard]] u64 GetFaceRendersRecorded() const { return m_FaceRendersRecorded; }

    private:
        SkyCubemapBake(Context& context, const Ref<DescriptorSetLayout>& consumerLayout,
                       Format sceneColorFormat, u32 faceSize);

        /// @brief Builds the per-face graphics pipeline from the bound material's shaders, once per identity.
        void EnsurePipeline(const MaterialInstance& material);

        /// @brief Records one material face render into a target face view at a given size.
        ///
        /// The shared body of every material bake — the display cube (Bake) and the scratch cube (the
        /// amortized tick) — differing only in which face view is the target and at what edge length.
        /// Claims one view slot for the face basis, draws the material fragment through it, and leaves
        /// the face in a color-attachment layout; the caller owns any whole-cube transition after the
        /// loop.
        /// @param cmd      The command buffer the face render is recorded into.
        /// @param material The resident Sky-domain material to bake; its pipeline must be ensured.
        /// @param faceView The single-layer color target for this face.
        /// @param faceSize The face edge length in texels (the target's extent).
        /// @param face     The cube face index, selecting its fixed view-ray basis.
        void RecordMaterialFace(CommandBuffer& cmd, const MaterialInstance& material,
                                const Ref<ImageView>& faceView, u32 faceSize, u32 face);

        /// @brief Records one atmosphere face render into a target face view at a given size.
        ///
        /// The atmosphere sibling of RecordMaterialFace, shared by the display cube and the scratch
        /// cube.
        /// @param cmd           The command buffer the face render is recorded into.
        /// @param pipeline      The atmosphere sky pipeline, built against the cube-face format.
        /// @param atmosphereSet The renderer's atmosphere LUT set.
        /// @param atmosphere    The atmosphere parameters the LUTs were generated for.
        /// @param sunDirection  The normalized toward-sun direction.
        /// @param intensity     Scales the baked sky radiance + sun disc.
        /// @param faceView      The single-layer color target for this face.
        /// @param faceSize      The face edge length in texels (the target's extent).
        /// @param face          The cube face index, selecting its fixed view-ray basis.
        void RecordAtmosphereFace(CommandBuffer& cmd, const Ref<GraphicsPipeline>& pipeline,
                                  const Ref<DescriptorSet>& atmosphereSet,
                                  const Atmosphere& atmosphere, const vec3& sunDirection,
                                  f32 intensity, const Ref<ImageView>& faceView, u32 faceSize,
                                  u32 face);

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
        u64 m_FaceRendersRecorded = 0;

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

        // The amortized bake's service job key (unique per instance) and its lifecycle state. The
        // material path also records the resolved instance's identity + revision so a re-request for
        // the same content is idempotent while a genuine change supersedes.
        GeneratedTextureKey m_JobKey;
        BakeState m_BakeState = BakeState::Idle;

        Ref<Image> m_DepthImage;     // 1×1 stand-in, holds the far-plane value (1.0)
        Ref<ImageView> m_DepthView;  // sampled as a color texture by the fragment's depth read
        TextureHandle m_DepthHandle; // bindless slot for the stand-in depth
        // the shared bindless slot the fragment reads the stand-in depth through
        SamplerHandle m_DepthSamplerHandle;

        Ref<DescriptorSet> m_ConsumerSet; // the skybox pass binds this to sample the baked cube

        Ref<GraphicsPipeline> m_Pipeline; // the material's fragment against the cube-face format
        u32 m_PipelineMaterialIndex = ~0u;

        // The six per-face InvViewProj matrices: each maps a fullscreen [0,1]² UV to the face's
        // world direction, matching the cube image-view layer order so shared edges agree.
        std::array<mat4, CubeFaces> m_FaceInvViewProj;
    };
}
