#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class AssetManager;
    class Scene;
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class GraphicsPipeline;
    class PipelineLayout;
    class Image;
    class ImageView;
    class Sampler;

    /// @brief Construction parameters for SceneCapture.
    struct SceneCaptureInfo
    {
        /// @brief The Vulkan context for resource creation.
        Context& Context;
        /// @brief Asset manager the owned SceneRenderer loads its engine shaders through.
        ///
        /// Must outlive the capture.
        AssetManager& Assets;
        /// @brief Edge length of each captured cube face, in pixels.
        u32 FaceResolution = 256;
        /// @brief Renderer batteries for the face renders.
        ///
        /// A capture wants a lean pipeline: the heavy per-view batteries (shadows, AO, TAA,
        /// SSR) multiply by six faces, and the capture samples the pre-tonemap HDR, so bloom
        /// and tonemap tuning never reach its output.
        SceneRendererSettings Settings;
    };

    /// @brief The per-frame capture source, pushed by the owner (see SceneCapture::SetView).
    struct CaptureView
    {
        /// @brief The scene to capture; null renders nothing.
        const Scene* World = nullptr;
        /// @brief World-space position the six faces render from.
        vec3 Position{0.0f};
        /// @brief One entity the face renders omit — the surface this capture feeds; Null omits none.
        ///
        /// A surface is not part of its own environment. A capture whose output is sampled by a
        /// mesh's own material sits on or inside that mesh, so drawing it would both compound the
        /// material's sampled term into the next capture and occlude the environment behind it
        /// across whatever share of the sphere the mesh subtends — Near clips only what is within
        /// 5 cm and cannot be relied on to hide a surface the probe sits on. Naming the entity here
        /// (never a mesh or a material, which are shared by many entities) drops it from the
        /// capture's visibility gather, so it is absent from every domain the capture draws.
        Entity Exclude = Entity::Null;
        /// @brief Interpolation fraction the face renders draw the scene at, in [0, 1).
        ///
        /// Forwarded to the face renderer as SceneView::Alpha, so the captured content sits at the
        /// pose the frame draws rather than at the last Sim tick's. It must be the same alpha
        /// @ref Position was resolved at: the two together place the camera and the geometry on one
        /// pose, and a mismatch offsets everything rigidly attached to the capture's own carrier by
        /// that fraction of a tick's motion — which changes every frame as the alpha sweeps.
        f32 Alpha = 0.0f;
        /// @brief Near plane of the face cameras, in world units.
        f32 Near = 0.05f;
        /// @brief Far plane of the face cameras, in world units.
        f32 Far = 10000.0f;
    };

    /// @brief A scene environment capture: six face renders from a world position, resampled
    /// into one octahedral 2D map a material samples by direction.
    ///
    /// The probe primitive: the capture owns one small SceneRenderer and, each frame a fresh
    /// CaptureView is pushed, renders the scene through one of six 90° face cameras
    /// (round-robin, so a full refresh spans six pushed frames), tiles the HDR result into a
    /// persistent face atlas, and resamples the atlas into an octahedral map (see
    /// Veng/octahedral.slang — a material computes OctahedralUV(direction) and samples the
    /// map there). The output is pre-tonemap linear HDR, and it is a plain 2D bindless
    /// texture — bindable to a material through SetTextureHandle — because a cube view
    /// cannot ride the set-0 bindless array.
    ///
    /// A capture is a sample of an *environment*, and a surface is not part of its own
    /// environment: CaptureView::Exclude names one entity the face renders skip, so a capture
    /// feeding a mesh's material never draws that mesh — in any domain, colour or depth. A view
    /// that excludes nothing (the default) captures the whole scene.
    ///
    /// Driven by the engine: Application::RegisterCapture puts the capture on the capture
    /// drive-list, rendered ahead of every viewport each frame so a material consuming the
    /// output reads this frame's result. Push-to-render: a frame with no fresh SetView
    /// records nothing, so an idle capture costs nothing. Single-owner (Unique); Create is
    /// the factory, and destroying the capture self-unregisters it from the drive-list.
    class SceneCapture
    {
    public:
        /// @brief Number of cube faces a full refresh spans — one face renders per pushed frame.
        ///
        /// An owner wanting a one-shot static capture pushes SetView for this many frames and
        /// then stops; the map freezes fully refreshed.
        static constexpr u32 FaceCount = 6;

        /// @brief Creates the capture: the face renderer, atlas, octahedral map, and pipelines.
        /// @param info  Construction parameters.
        /// @return The owning Unique.
        static Unique<SceneCapture> Create(const SceneCaptureInfo& info);

        /// @brief Releases owned resources and self-unregisters from the capture drive-list.
        ~SceneCapture();

        SceneCapture(const SceneCapture&) = delete;
        SceneCapture& operator=(const SceneCapture&) = delete;

        /// @brief Pushes the source the next Render captures.
        ///
        /// Marks the capture fresh: the next engine-driven Render records one face and the
        /// resample, then clears the freshness — so the owner re-pushes each frame it wants a
        /// live capture, and stops pushing to freeze the last result.
        /// @param view  The scene, position, and camera planes to capture.
        void SetView(const CaptureView& view);

        /// @brief Records the capture (one round-robin face + the octahedral resample) into @p cmd.
        ///
        /// Called by the engine's frame drive ahead of the viewports; a frame with no fresh
        /// SetView (or a null World) records nothing. One face renders per push — the face
        /// renderer's per-frame rings support one Execute per frame, and one small render per
        /// frame is the capture's cost ceiling — so a full refresh spans six pushed frames and
        /// a moved capture converges over them. The output is left in a sampled layout.
        /// @param cmd  The frame command buffer.
        void Render(CommandBuffer& cmd);

        /// @brief Returns the octahedral map view (pre-tonemap linear HDR).
        [[nodiscard]] const Ref<ImageView>& GetOutput() const { return m_OctahedralView; }

        /// @brief Returns the octahedral map's bindless handle, for Material::SetTextureHandle.
        [[nodiscard]] TextureHandle GetOutputHandle() const { return m_OctahedralHandle; }

        /// @brief Attaches this capture to the Application capture drive-list.
        ///
        /// Called by Application::RegisterCapture; the capture erases its own pointer on
        /// destruction, so the owner's Unique is the whole of cleanup.
        /// @param driveList  The Application drive-list this capture now belongs to.
        /// @pre This capture is not already attached to a drive-list.
        void AttachToDriveList(vector<SceneCapture*>& driveList);

    private:
        explicit SceneCapture(const SceneCaptureInfo& info);

        Context& m_Context;
        /// @brief Face render resolution (square).
        u32 m_FaceResolution = 0;

        /// @brief The face renderer, executed once per face per capture.
        Unique<SceneRenderer> m_Renderer;
        /// @brief Bindless handle over the renderer's HDR target, sampled by the atlas copy.
        TextureHandle m_HdrHandle;

        /// @brief The 3×2 face atlas the six HDR results tile into.
        Ref<Image> m_AtlasImage;
        /// @brief View over m_AtlasImage.
        Ref<ImageView> m_AtlasView;
        /// @brief Bindless handle of m_AtlasView, sampled by the octahedral resample.
        TextureHandle m_AtlasHandle;

        /// @brief The octahedral output map.
        Ref<Image> m_OctahedralImage;
        /// @brief View over m_OctahedralImage.
        Ref<ImageView> m_OctahedralView;
        /// @brief Bindless handle of m_OctahedralView — the consumer-facing output.
        TextureHandle m_OctahedralHandle;

        /// @brief Clamp sampler shared by the atlas copy and the resample.
        Ref<Sampler> m_Sampler;
        /// @brief Bindless handle of m_Sampler.
        SamplerHandle m_SamplerHandle;

        /// @brief Fullscreen copy pipeline (HDR → atlas cell) + layout.
        Ref<GraphicsPipeline> m_CopyPipeline;
        Ref<PipelineLayout> m_CopyLayout;
        /// @brief Octahedral resample pipeline (atlas → octahedral map) + layout.
        Ref<GraphicsPipeline> m_OctahedralPipeline;
        Ref<PipelineLayout> m_OctahedralLayout;

        /// @brief The source pushed by SetView, captured by the next Render.
        CaptureView m_View;
        /// @brief Whether a fresh view was pushed since the last Render (push-to-render).
        bool m_ViewFresh = false;
        /// @brief The next round-robin face Render refreshes.
        u32 m_NextFace = 0;
        /// @brief Whether the atlas has been cleared once; later faces load the persisted cells.
        bool m_AtlasCleared = false;

        /// @brief The drive-list this capture is registered into; null when unregistered.
        ///
        /// Set by AttachToDriveList; ~SceneCapture erases this capture's pointer from it.
        vector<SceneCapture*>* m_DriveList = nullptr;
    };
}
