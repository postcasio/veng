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
    /// One cube at a caller-chosen face resolution, created off the bindless registry the way the
    /// IBL radiance image is; re-bakes overwrite in place, so VRAM is one cube regardless of how
    /// often the sky changes. Bake records on the caller's dirty signal (the source swapped or its
    /// params/bound buffers changed), so a static sky bakes once. Exposes a Cube image view + a
    /// consumer descriptor set matching the IBL set's radiance binding, so the skybox pass samples
    /// the baked cube exactly as it samples a resident environment's radiance cube.
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
        void Bake(CommandBuffer& cmd, const MaterialInstance& material);

        /// @brief Bakes `material` and returns a host-side snapshot of the six radiance faces.
        ///
        /// The device-free path the cheap SH ambient arm reads from: bakes into the owned cube
        /// through a self-contained immediate submit, copies all six layers into one tightly-packed
        /// RGBA16F buffer (layer-major), and blocks until the download completes — so the returned
        /// bytes are the freshly-baked radiance regardless of any frame command buffer's ordering.
        /// Overwrites the cube in place, so a later Bake into the frame command buffer still points
        /// the skybox at the current radiance. Called once on the sky dirty signal (a static sky
        /// pays one readback), so the stall is bounded.
        /// @param material The resident Sky-domain material to bake.
        /// @return The six cube faces, layer-major, RGBA16F (`GetFaceSize()`² texels each).
        /// @pre material's domain is MaterialDomain::Sky and its param block is uploaded this frame.
        [[nodiscard]] vector<u8> BakeAndDownload(const MaterialInstance& material);

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
        void BakeAtmosphere(CommandBuffer& cmd, const Ref<GraphicsPipeline>& pipeline,
                            const Ref<DescriptorSet>& atmosphereSet, const Atmosphere& atmosphere,
                            const vec3& sunDirection, f32 intensity);

        /// @brief Bakes the atmosphere and returns a host-side snapshot of the six radiance faces.
        ///
        /// The device-free path the cheap SH ambient arm reads from: bakes the atmosphere into the
        /// owned cube through a self-contained immediate submit, copies all six layers into one
        /// tightly-packed RGBA16F buffer (layer-major), and blocks until the download completes — so
        /// the returned bytes are the freshly-baked radiance regardless of any frame command
        /// buffer's ordering. Overwrites the cube in place, so a later BakeAtmosphere into the frame
        /// command buffer still points the skybox at the current radiance.
        /// @param pipeline      The atmosphere sky pipeline, built against the cube-face format.
        /// @param atmosphereSet The renderer's atmosphere LUT set (scattering/transmittance/sampler).
        /// @param atmosphere    The atmosphere parameters the LUTs were generated for.
        /// @param sunDirection  The normalized toward-sun direction.
        /// @param intensity     Scales the baked sky radiance + sun disc.
        /// @return The six cube faces, layer-major, RGBA16F (`GetFaceSize()`² texels each).
        /// @pre `atmosphereSet`'s LUTs were generated for `atmosphere`.
        [[nodiscard]] vector<u8> BakeAtmosphereAndDownload(const Ref<GraphicsPipeline>& pipeline,
                                                           const Ref<DescriptorSet>& atmosphereSet,
                                                           const Atmosphere& atmosphere,
                                                           const vec3& sunDirection, f32 intensity);

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

    private:
        SkyCubemapBake(Context& context, const Ref<DescriptorSetLayout>& consumerLayout,
                       Format sceneColorFormat, u32 faceSize);

        /// @brief Builds the per-face graphics pipeline from the bound material's shaders, once per identity.
        void EnsurePipeline(const MaterialInstance& material);

        Context& m_Context;
        Format m_SceneColorFormat;
        u32 m_FaceSize;

        Ref<Image> m_CubeImage;
        Ref<ImageView> m_CubeView;                 // Cube view — sampled by the skybox pass
        std::array<Ref<ImageView>, 6> m_FaceViews; // one single-layer view per face — rendered

        Ref<Image> m_DepthImage;     // 1×1 stand-in, holds the far-plane value (1.0)
        Ref<ImageView> m_DepthView;  // sampled as a color texture by the fragment's depth read
        TextureHandle m_DepthHandle; // bindless slot for the stand-in depth
        Ref<Sampler> m_DepthSampler; // the sampler the fragment reads the stand-in depth through
        SamplerHandle m_DepthSamplerHandle; // its bindless slot

        Ref<DescriptorSet> m_ConsumerSet; // the skybox pass binds this to sample the baked cube

        Ref<GraphicsPipeline> m_Pipeline; // the material's fragment against the cube-face format
        u32 m_PipelineMaterialIndex = ~0u;

        // The six per-face InvViewProj matrices: each maps a fullscreen [0,1]² UV to the face's
        // world direction, matching the cube image-view layer order so shared edges agree.
        std::array<mat4, 6> m_FaceInvViewProj;
    };
}
