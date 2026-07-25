#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class AssetManager;
    class MaterialInstance;
    class Scene;
}

namespace Veng::Renderer
{
    class Context;
    class SceneCapture;

    /// @brief Runtime capture state a CaptureSurface materializes lazily; defined in CaptureSurface.cpp.
    struct CaptureSurfaceRuntime;

    /// @brief The surface's intended sampling model, read by the entity's material.
    ///
    /// The engine renders one environment capture from the entity's world position regardless of this
    /// value — an octahedral map of the scene seen from there. The value records how the surface's
    /// material samples that map: a reflective or refractive object gathers it by reflection direction
    /// (EnvironmentProbe), a flat mirror or monitor by its surface parameterization (PlanarReflection).
    /// It is authored data for the material, not a switch on the engine's capture path.
    enum class CaptureShape : u8
    {
        /// @brief A reflective or refractive object sampling the capture by direction (a probe).
        EnvironmentProbe,
        /// @brief A flat mirror or monitor sampling the capture across its surface (a plane).
        PlanarReflection,
    };

    /// @brief When the engine re-renders the capture.
    ///
    /// Selects whether the capture tracks the live scene every frame or freezes until a system marks
    /// it dirty. The delivered capture renders one cube face per driven frame, so a full refresh spans
    /// SceneCapture::FaceCount driven frames from the moment it is (re)dirtied.
    enum class CaptureRefresh : u8
    {
        /// @brief Re-render every frame — a live mirror or a moving probe.
        EveryFrame,
        /// @brief Render once (a full-face refresh) then idle until MarkDirty is called again.
        OnDemand,
    };

    /// @brief A scene entity's declaration of a render-to-texture capture the engine discovers and drives.
    ///
    /// A reflected scene component, the render-to-texture sibling of GuiSurface: where GuiSurface maps a
    /// document onto a world mesh, a CaptureSurface renders the scene into a texture from the entity's
    /// world position and binds that texture's handle onto the material of the mesh it shares an entity
    /// with (its sibling MeshRenderer). The engine discovers the component in the scene it drives, builds
    /// the owned SceneCapture from the authored config on first sight, feeds it to the capture drive-list
    /// (RegisterCapture) against the component's lifetime, and drops it — self-unregistering — when the
    /// component, its entity, or its scene goes away. So a reflective or refractive surface, a mirror, or
    /// a monitor is authored data on the entity: no app-side RegisterCapture, no per-frame game code.
    ///
    /// The capture renders the scene *around* the entity, never the entity itself: the mesh the capture
    /// feeds is excluded from its own capture (CaptureView::Exclude), in every domain the capture draws.
    /// Drawing it would compound the material's own sampled term into the next capture and — because the
    /// probe sits on or inside the surface it feeds — occlude the environment behind it. The rule is
    /// unconditional and has no authored knob.
    ///
    /// The output is an octahedral map (pre-tonemap linear HDR) the material samples by direction. Its
    /// handle is a runtime bindless slot, not a cooked asset id, so Drive rebinds it onto the sibling
    /// material's named texture slot each frame — the GuiSurface locality rule and per-frame rebind. The
    /// material authors the named slot (Material::SetTextureHandle, the no-resident-asset path) and this
    /// component fills it.
    ///
    /// The runtime resources (the SceneCapture and its sampler) are materialized on the first Drive, which
    /// needs the render context and asset manager the engine supplies; a component that never drives
    /// allocates none.
    struct CaptureSurface
    {
        /// @brief Default-constructs an unmaterialized capture (its runtime is empty until Drive).
        CaptureSurface();
        /// @brief Releases the owned capture, self-unregistering it from the drive-list.
        ~CaptureSurface();
        /// @brief Move-constructs, transferring the runtime state.
        CaptureSurface(CaptureSurface&&) noexcept;
        /// @brief Move-assigns, transferring the runtime state.
        CaptureSurface& operator=(CaptureSurface&&) noexcept;

        CaptureSurface(const CaptureSurface&) = delete;
        CaptureSurface& operator=(const CaptureSurface&) = delete;

        /// @brief The surface's sampling model, read by the entity's material (see CaptureShape).
        CaptureShape Shape = CaptureShape::EnvironmentProbe;

        /// @brief Edge length, in pixels, of each captured cube face.
        u32 Resolution = 256;

        /// @brief When the engine re-renders the capture (see CaptureRefresh).
        CaptureRefresh Refresh = CaptureRefresh::EveryFrame;

        /// @brief Name of the sibling material's texture slot the capture output binds onto.
        ///
        /// Drive binds the capture's octahedral output handle onto the material field of this name
        /// (a TextureHandle-kind field). The default "Texture" preserves the built-in binding; author
        /// a different name to match a descriptively-named material slot.
        string TextureSlot = "Texture";

        /// @brief Name of the sibling material's sampler slot the capture sampler binds onto.
        ///
        /// Drive binds the capture's clamp sampler onto the material field of this name (a
        /// SamplerHandle-kind field). The default "Sampler" preserves the built-in binding.
        string SamplerSlot = "Sampler";

        /// @brief Runtime capture state, materialized on the first Drive; empty until then.
        mutable Unique<CaptureSurfaceRuntime> Runtime;

        /// @brief Requests a refresh: the next SceneCapture::FaceCount driven frames re-render the map.
        ///
        /// An OnDemand capture idles after its refresh completes; a system calls this when the sampled
        /// scene content changed so the frozen map is rebuilt. An EveryFrame capture refreshes anyway,
        /// so this is a no-op for it. Before the runtime materializes, it is remembered and applied on
        /// the first Drive.
        void MarkDirty();

        /// @brief Returns the owned capture, or nullptr before the first Drive materializes it.
        [[nodiscard]] SceneCapture* GetCapture() const;

        /// @brief Returns the capture's octahedral output handle, or an invalid handle before Drive.
        ///
        /// The runtime bindless slot Drive binds onto the sibling material; the material samples the
        /// scene-from-here through it.
        [[nodiscard]] TextureHandle GetOutputHandle() const;

        /// @brief Whether the next Drive will still push a face into the capture (the refresh state).
        ///
        /// True while a refresh is in progress: always for EveryFrame, and for OnDemand until its
        /// FaceCount-frame refresh completes (re-armed by MarkDirty). False for a settled OnDemand
        /// capture — the point at which it renders nothing until dirtied again.
        [[nodiscard]] bool IsRefreshing() const;

        /// @brief Builds and drives the capture, then binds its output onto the sibling material.
        ///
        /// Materializes the runtime on first use (creating the SceneCapture at the authored resolution
        /// and its sampler) and returns the capture so the caller registers it on the drive-list the
        /// first time it appears. Pushes this frame's capture source (SceneCapture::SetView from @p
        /// position) when the refresh policy calls for it — every frame for EveryFrame, only while a
        /// refresh is outstanding for OnDemand — so a settled OnDemand capture records nothing. Binds the
        /// capture's output handle onto @p material's named slot every frame (SetTextureHandle writes the
        /// current frame-in-flight region, so the handle must land regardless of whether a face was
        /// pushed).
        ///
        /// The pushed source excludes @p entity (CaptureView::Exclude), so the capture never draws the
        /// mesh it feeds — the rule has no authoring surface and cannot be misconfigured.
        /// @param context   The render context the capture allocates on.
        /// @param assets    The asset manager the capture's SceneRenderer loads its shaders through.
        /// @param world     The scene being captured, pushed as this frame's source.
        /// @param entity    The entity this component belongs to; excluded from its own capture.
        /// @param position  The entity's world-space position the capture renders from.
        /// @param material  The sibling mesh material to bind the output onto; null skips binding.
        /// @return The owned capture (built on first use), or nullptr when the resolution is invalid.
        SceneCapture* Drive(Context& context, AssetManager& assets, const Scene& world,
                            Entity entity, const vec3& position, MaterialInstance* material) const;
    };
}

VE_ENUM(::Veng::Renderer::CaptureShape, 0x497C9B89E8057D21ULL)
VE_ENUMERATOR(EnvironmentProbe)
VE_ENUMERATOR(PlanarReflection)
VE_ENUM_END();

VE_ENUM(::Veng::Renderer::CaptureRefresh, 0x604C3FD0FE4F9840ULL)
VE_ENUMERATOR(EveryFrame)
VE_ENUMERATOR(OnDemand)
VE_ENUM_END();

VE_REFLECT(::Veng::Renderer::CaptureSurface, 0x59B48CAC6127A406ULL)
VE_FIELD(Shape, .DisplayName = "Shape")
VE_FIELD(Resolution, .DisplayName = "Resolution", .Display = {.Min = 1})
VE_FIELD(Refresh, .DisplayName = "Refresh")
VE_FIELD(TextureSlot, .DisplayName = "Texture Slot")
VE_FIELD(SamplerSlot, .DisplayName = "Sampler Slot")
VE_REFLECT_END();
