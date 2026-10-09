#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Math/AABB.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CaptureSurface.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class AssetManager;
    class Prefab;
    class Scene;
}

namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;
    class ModelPortraitPool;

    /// @brief A pooled renderer a ModelPortrait draws through; defined in the renderer's sources.
    class PortraitRenderer;

    /// @brief Runtime state a ModelPortrait materializes lazily; defined in ModelPortrait.cpp.
    struct ModelPortraitRuntime;

    /// @brief What a ModelPortrait renders: the shaded model, or its shape alone.
    enum class PortraitOutput : u8
    {
        /// @brief The lit, tonemapped model, with its coverage in alpha (ModelPortraitOutput::Color).
        Shaded,
        /// @brief Depth and world normal only (RenderPath::GeometryDepthNormal), for a stylized or
        ///        outline fill that shades the shape itself.
        GeometryDepthNormal,
    };

    /// @brief How a ModelPortrait places its camera.
    enum class PortraitFramingMode : u8
    {
        /// @brief Frame the model's bounds from Yaw, Pitch, FieldOfView and Padding.
        AutoFit,
        /// @brief Take CameraPosition, CameraRotation and FieldOfView as given.
        Explicit,
    };

    /// @brief The camera a ModelPortrait renders its model through.
    ///
    /// Every value is in the portrait's private scene, whose model root sits at the origin posed by
    /// ModelPortrait::ModelPose. AutoFit aims at the centre of the instantiated model's bounds
    /// (ModelPortraitOutput::ModelBounds — never what a populate attached) from the bearing Yaw and
    /// Pitch describe, backed off until a sphere of Padding times the bounds' half-diagonal fits the
    /// narrower field of view. Explicit is for a caller framing by its own rule — a target shown at
    /// its true attitude to a viewer — and is taken exactly. Either way the clip planes are fitted
    /// to everything rendered, so nothing in the private scene is cut by them.
    struct PortraitFraming
    {
        /// @brief Whether the camera is fitted to the model or placed by the caller.
        PortraitFramingMode Mode = PortraitFramingMode::AutoFit;
        /// @brief AutoFit: the camera's bearing about +Y, in degrees; 0 places it on the +Z axis.
        f32 Yaw = 30.0f;
        /// @brief AutoFit: the camera's elevation above the model's horizon, in degrees, within ±89.
        f32 Pitch = 15.0f;
        /// @brief The vertical field of view, in degrees, for either mode.
        f32 FieldOfView = 30.0f;
        /// @brief AutoFit: the framed sphere's radius as a multiple of the bounds' half-diagonal.
        f32 Padding = 1.1f;
        /// @brief Explicit: the camera's position in the private scene.
        vec3 CameraPosition{0.0f, 0.0f, 3.0f};
        /// @brief Explicit: the camera's rotation in the private scene; it looks down its local -Z.
        quat CameraRotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    /// @brief The light a ModelPortrait's private scene is lit by: one key light and an ambient floor.
    struct PortraitLighting
    {
        /// @brief The key light's travel direction in the private scene (a directional light).
        vec3 KeyDirection{-0.4f, -0.6f, -0.7f};
        /// @brief The key light's linear RGB colour.
        vec3 KeyColor{1.0f, 1.0f, 1.0f};
        /// @brief The key light's illuminance in lux (see Light::Intensity); 100000 is a full sun.
        f32 KeyIntensity = 100000.0f;
        /// @brief The ambient floor every surface receives (SceneView::AmbientFloor).
        vec3 Ambient{0.12f, 0.13f, 0.16f};
    };

    /// @brief What a ModelPortrait's last render produced, read through ModelPortrait::GetOutput.
    ///
    /// Every handle is a runtime bindless slot owned by the portrait's renderer: valid while Ready,
    /// and handed back (or to the next portrait taking the renderer from the pool) once the
    /// portrait is disabled or destroyed, so a reader re-reads the struct each frame rather than
    /// holding a handle.
    struct ModelPortraitOutput
    {
        /// @brief Whether the portrait has rendered since its renderer was materialized.
        ///
        /// False before the first render and once the component is disabled or destroyed; every
        /// other field is meaningful only while it is true.
        bool Ready = false;
        /// @brief Shaded: the tonemapped model, coverage in alpha (opaque where the model is drawn,
        ///        zero elsewhere), so an Image composites it over its own background.
        TextureHandle Color;
        /// @brief GeometryDepthNormal: the reverse-Z depth (0 where nothing was drawn).
        TextureHandle Depth;
        /// @brief GeometryDepthNormal: the world-space normal, signed.
        TextureHandle Normal;
        /// @brief A clamped linear sampler to read Color through (depth and normal want a load).
        SamplerHandle Sampler;
        /// @brief The outputs' size in pixels.
        uvec2 Extent{0, 0};
        /// @brief The camera the render was made through, for a fill reconstructing positions.
        CameraView Camera;
        /// @brief The model root's world matrix in the private scene (ModelPose).
        mat4 ModelTransform{1.0f};
        /// @brief World bounds of everything rendered, a populate's parts included.
        AABB Bounds = AABB::Empty();
        /// @brief World bounds of the instantiated Model alone, excluding what a populate attached.
        AABB ModelBounds = AABB::Empty();
    };

    /// @brief A model rendered offscreen for UI, owned by a scene component — a portrait of a
    ///        selected object, an equipment preview, a character on a loadout screen.
    ///
    /// A reflected scene component, the fourth member of the engine-driven family beside GuiOverlay,
    /// GuiSurface and CaptureSurface. It owns a private scene holding an instance of Model, a camera
    /// framed on it, a key light, and a lean renderer drawing it into a texture a UI samples. It is
    /// driven exactly as a CaptureSurface is: the ViewportCompositor's pre-pass drives the portraits
    /// of each scene a registered viewport will render this frame, once per scene, ahead of every
    /// viewport — so a Gui driver reading GetOutput in that frame's drive paints this frame's render.
    /// A portrait in a scene nothing renders is not rendered, and one whose scene went unrendered
    /// for a frame renders again when it resumes (an OnDemand portrait included).
    ///
    /// Its renderer comes from the compositor's ModelPortraitPool, keyed by Extent and Output, at
    /// most one new build per frame shared with the capture pre-pass, and renders inside the frame's
    /// view budget ahead of the captures; a portrait the budget cannot seat keeps its last render.
    /// The renderer is lean — no bloom, AO, SSR, shadows, post effects or anti-aliasing. Destroying
    /// or disabling the component returns the renderer to the pool, hands its slots back and drops
    /// the private scene.
    ///
    /// The private scene has no systems and no presentation scope: it is static and silent, and the
    /// model is posed by ModelPose and by what SetOnPopulate attaches, never animated by systems.
    /// Model is instantiated on the first drive that finds it resident (and again when it is
    /// re-pointed); the portrait waits, not Ready, until what the spawn streams in is resident.
    ///
    /// Showing one in a document is a driver reading GetOutput each frame and calling
    /// Gui::Document::SetImageTexture (or ClearImageTexture while it is not Ready).
    struct ModelPortrait
    {
        /// @brief Default-constructs an unmaterialized portrait (its runtime is empty until driven).
        ModelPortrait();
        /// @brief Releases the runtime: the renderer back to its pool, the private scene dropped.
        ~ModelPortrait();
        /// @brief Move-constructs, transferring the runtime state.
        ModelPortrait(ModelPortrait&&) noexcept;
        /// @brief Move-assigns, transferring the runtime state.
        ModelPortrait& operator=(ModelPortrait&&) noexcept;

        ModelPortrait(const ModelPortrait&) = delete;
        ModelPortrait& operator=(const ModelPortrait&) = delete;

        /// @brief Whether the engine drives the portrait at all.
        ///
        /// A disabled portrait keeps its settings and its populate callback and holds nothing else:
        /// the drive releases its renderer and private scene, and builds none while it stays off.
        bool Enabled = true;

        /// @brief The model instantiated into the private scene.
        ///
        /// Empty, or a handle still loading, renders nothing: an instance already shown is dropped
        /// and the output reads not Ready until a resident Model is instantiated.
        AssetHandle<Prefab> Model;

        /// @brief The render's size in pixels.
        uvec2 Extent{256, 256};

        /// @brief Whether the render is shaded or depth and normal only (see PortraitOutput).
        PortraitOutput Output = PortraitOutput::Shaded;

        /// @brief How the camera is placed (see PortraitFraming).
        PortraitFraming Framing;

        /// @brief The model root's pose in the private scene, applied every frame the portrait renders.
        ///
        /// How a caller turns the model — an object's attitude relative to a viewer — without
        /// re-instantiating it.
        Transform ModelPose;

        /// @brief The key light and ambient floor (see PortraitLighting).
        PortraitLighting Lighting;

        /// @brief Whether the portrait re-renders every driven frame, or once until MarkDirty.
        CaptureRefresh Refresh = CaptureRefresh::EveryFrame;

        /// @brief Runtime state, materialized on the first drive (or SetOnPopulate); empty until then.
        mutable Unique<ModelPortraitRuntime> Runtime;

        /// @brief Sets the callback run after each instantiation of Model, to attach parts to it.
        ///
        /// Called with the private scene and the model root — the entity ModelPose poses, every
        /// spawned root of Model parented under it — so a caller adds the parts the model wears (an
        /// attachment, a decal) as entities of that scene. Every entity the callback creates is
        /// recorded, which is what Repopulate destroys. Stored on the runtime, like
        /// GuiOverlay::SetOnInstantiate; set after the model is live, it repopulates at once. It
        /// survives the component being disabled. An empty function clears it.
        /// @param callback  The populate callback, or an empty function.
        void SetOnPopulate(function<void(Scene&, Entity)> callback);

        /// @brief Re-runs the populate callback on the live model without re-instantiating it.
        ///
        /// Destroys every entity a previous populate created, then calls the callback again, so
        /// parts that change — an attachment swapped — rebuild while the model stays resident. A
        /// no-op before the model is instantiated (the first populate runs then).
        void Repopulate() const;

        /// @brief Requests a render: an OnDemand portrait renders on its next driven frame.
        ///
        /// A no-op for an EveryFrame portrait, which renders anyway. Const for the reason
        /// CaptureSurface::MarkDirty is: the request lives in the runtime, not the authored config.
        void MarkDirty() const;

        /// @brief Releases the renderer and the private scene, keeping the settings and callback.
        ///
        /// What the drive does to a disabled portrait; the next drive of an enabled one builds again.
        void Release() const;

        /// @brief Returns what the last render produced (see ModelPortraitOutput).
        [[nodiscard]] ModelPortraitOutput GetOutput() const;

        /// @brief Returns the private scene, or null before the model is first instantiated.
        [[nodiscard]] Scene* GetScene() const;

        /// @brief Returns the model root in the private scene, or Entity::Null before instantiation.
        [[nodiscard]] Entity GetModelRoot() const;

        /// @brief Returns how many times Model has been instantiated over the runtime's life.
        [[nodiscard]] u32 GetInstantiationCount() const;

        /// @brief Whether a renderer is installed (Materialize has run and nothing released it).
        [[nodiscard]] bool HasRenderer() const;

        /// @brief Returns the installed renderer, or null — for diagnostics and tests.
        [[nodiscard]] const PortraitRenderer* GetRenderer() const;

        /// @brief Installs @p renderer as this portrait's, ahead of its first render.
        ///
        /// For the compositor's pre-pass, which paces new builds and reuses released renderers.
        /// When @p pool is live as the portrait releases the renderer, it is returned there.
        /// @param renderer  A renderer configured for Extent and Output (new, or from a pool).
        /// @param pool      Where the renderer returns on release; an empty pointer drops it.
        /// @pre HasRenderer() is false.
        void Materialize(Unique<PortraitRenderer> renderer,
                         std::weak_ptr<ModelPortraitPool> pool) const;

        /// @brief Brings the private scene current and decides whether this frame renders.
        ///
        /// Instantiates Model when it is resident and not yet (or no longer) the instance, runs the
        /// populate callback after an instantiation, re-arms an OnDemand render after a frame the
        /// portrait was not driven on, writes ModelPose and Lighting into the private scene, and
        /// frames the camera. A portrait whose renderer no longer matches Extent or Output releases
        /// it first, and the caller materializes a matching one.
        /// @param context  The render context (its frame serial re-arms after a gap).
        /// @param assets   The asset manager the model spawns through.
        /// @return True when the portrait should render this frame.
        /// @pre HasRenderer() is true.
        bool Prepare(Context& context, AssetManager& assets) const;

        /// @brief Records this frame's render into @p cmd, leaving the outputs sampleable.
        /// @param cmd  The frame command buffer.
        /// @pre Prepare returned true this frame.
        void Render(CommandBuffer& cmd) const;
    };

    /// @brief The configuration a PortraitRenderer is built for: its size and what it renders.
    struct PortraitRendererConfig
    {
        /// @brief The render's size in pixels.
        uvec2 Extent{0, 0};
        /// @brief Whether it renders shaded colour or depth and normal.
        PortraitOutput Output = PortraitOutput::Shaded;

        /// @brief Member-wise equality: two configurations build interchangeable renderers.
        bool operator==(const PortraitRendererConfig&) const = default;
    };

    /// @brief Builds a renderer for a portrait of @p config — the build the pool exists to avoid.
    /// @param context  The render context it allocates on.
    /// @param assets   The asset manager its shaders load through.
    /// @param config   Its size and output.
    /// @return The renderer.
    [[nodiscard]] VE_API Unique<PortraitRenderer>
    CreatePortraitRenderer(Context& context, AssetManager& assets,
                           const PortraitRendererConfig& config);

    /// @brief Holds released portrait renderers for reuse by the next portrait of the same configuration.
    ///
    /// The SceneCapturePool of model portraits: a portrait whose component goes hands its renderer
    /// here, detached from its private scene, and the next portrait asking for the same Extent and
    /// Output takes it instead of building one — so a UI opening and closing a portrait builds once.
    /// Holds at most its capacity, dropping the longest-held past it. Its renderers die with the pool,
    /// so it must not outlive the context and asset manager they were built against.
    class VE_API ModelPortraitPool
    {
    public:
        /// @brief How many released renderers a pool holds unless told otherwise.
        static constexpr usize DefaultCapacity = 4;

        /// @brief Constructs an empty pool.
        /// @param capacity  The most renderers the pool holds at once; 0 holds none.
        explicit ModelPortraitPool(usize capacity = DefaultCapacity);

        /// @brief Drops every held renderer.
        ~ModelPortraitPool();

        ModelPortraitPool(const ModelPortraitPool&) = delete;
        ModelPortraitPool& operator=(const ModelPortraitPool&) = delete;

        /// @brief Hands over a held renderer built for @p config, or null when none is.
        /// @param config  The configuration the caller would otherwise build.
        /// @return A held renderer of that configuration, or null.
        [[nodiscard]] Unique<PortraitRenderer> Take(const PortraitRendererConfig& config);

        /// @brief Takes a released renderer into the pool; past capacity the longest-held is dropped.
        /// @param renderer  The renderer its portrait no longer wants; null is a no-op.
        void Return(Unique<PortraitRenderer> renderer);

        /// @brief Returns how many renderers the pool holds.
        [[nodiscard]] usize GetHeldCount() const { return m_Held.size(); }

        /// @brief Drops every held renderer.
        void Clear();

    private:
        /// @brief The most renderers held at once.
        usize m_Capacity = DefaultCapacity;
        /// @brief The held renderers, longest-held first.
        vector<Unique<PortraitRenderer>> m_Held;
    };
}

VE_ENUM(::Veng::Renderer::PortraitOutput, 0x823D9C03CC700094ULL)
VE_ENUMERATOR(Shaded)
VE_ENUMERATOR(GeometryDepthNormal)
VE_ENUM_END();

VE_ENUM(::Veng::Renderer::PortraitFramingMode, 0x286064904CB00241ULL)
VE_ENUMERATOR(AutoFit)
VE_ENUMERATOR(Explicit)
VE_ENUM_END();

VE_REFLECT(::Veng::Renderer::PortraitFraming, 0x9F4FB214520C06E2ULL)
VE_FIELD(Mode, .DisplayName = "Mode")
VE_FIELD(Yaw, .DisplayName = "Yaw")
VE_FIELD(Pitch, .DisplayName = "Pitch", .Display = {.Min = -89.0, .Max = 89.0})
VE_FIELD(FieldOfView, .DisplayName = "Field Of View", .Display = {.Min = 1.0, .Max = 170.0})
VE_FIELD(Padding, .DisplayName = "Padding", .Display = {.Min = 0.01})
VE_FIELD(CameraPosition, .DisplayName = "Camera Position")
VE_FIELD(CameraRotation, .DisplayName = "Camera Rotation")
VE_REFLECT_END();

VE_REFLECT(::Veng::Renderer::PortraitLighting, 0x8D5105F30790E446ULL)
VE_FIELD(KeyDirection, .DisplayName = "Key Direction")
VE_FIELD(KeyColor, .DisplayName = "Key Color")
VE_FIELD(KeyIntensity, .DisplayName = "Key Intensity", .Display = {.Min = 0.0})
VE_FIELD(Ambient, .DisplayName = "Ambient")
VE_REFLECT_END();

VE_REFLECT(::Veng::Renderer::ModelPortrait, 0x61CA1D82D40F53ECULL)
VE_FIELD(Enabled, .DisplayName = "Enabled")
VE_FIELD(Model, .DisplayName = "Model")
VE_FIELD(Extent, .DisplayName = "Extent", .Display = {.Min = 1})
VE_FIELD(Output, .DisplayName = "Output")
VE_FIELD(Framing, .DisplayName = "Framing")
VE_FIELD(ModelPose, .DisplayName = "Model Pose")
VE_FIELD(Lighting, .DisplayName = "Lighting")
VE_FIELD(Refresh, .DisplayName = "Refresh")
VE_REFLECT_END();
