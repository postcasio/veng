#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Gui/Driver.h>
#include <Veng/Gui/UIDocument.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class AssetManager;
    class GuiDriverRegistry;
    class MaterialInstance;
    class Scene;

    namespace Gui
    {
        class Document;
        class RenderTarget;
    }

    namespace Renderer
    {
        class Context;
        class CommandBuffer;
        class GuiScenePass;
    }

    /// @brief How a GuiSurface places its document's HDR texture into the scene as light.
    ///
    /// The document renders one HDR texture regardless; the domain selects the material path that
    /// turns it into scene light. Both composite into the lit HDR scene color before bloom/tonemap,
    /// so bright document pixels bloom through the scene's own bloom.
    enum class GuiSurfaceDomain : u8
    {
        /// @brief A self-radiant, see-through surface — the panel material returns the document
        ///        texel as its radiance, transparent regions show the scene behind, writes no depth.
        Translucent,
        /// @brief A solid, occluding surface — the material's own fragment samples the bound
        ///        document handle and writes it into the emissive g-buffer channel over an opaque
        ///        lit surface, so the panel occludes what is behind it (and carries motion vectors).
        OpaqueEmissive,
    };

    /// @brief What a GuiSurface's Drive needs to run its named GuiDriver, beyond the render arguments.
    ///
    /// Assembled by the driving viewport for the surface it claims, and empty (World or Drivers null)
    /// for every other drive — a viewport that does not claim the surface still renders its document,
    /// so the two jobs are separated here rather than by skipping the drive. A surface whose Driver is
    /// Null ignores this entirely.
    struct GuiSurfaceDriveContext
    {
        /// @brief The presented scene the surface lives in; null leaves the surface undriven.
        ///
        /// Mutable because a driver stamps request/command and ViewOutput components (the driver
        /// boundary in Veng/Gui/Driver.h), while the render path borrows the scene const.
        Scene* World = nullptr;
        /// @brief The catalog the Driver id resolves against; null leaves the surface undriven.
        GuiDriverRegistry* Drivers = nullptr;
        /// @brief The entity carrying this surface — the driver's own instance, handed to it verbatim.
        Entity Owner = Entity::Null;
        /// @brief The seat the driven document answers to, for the driver's frame.
        Entity Seat = Entity::Null;
        /// @brief Interpolation fraction into the next Sim tick, in [0, 1) — the render gather's own.
        f32 Alpha = 0.0f;
        /// @brief The presenting viewport's resolved camera, region, and UI scale this frame.
        SystemViewInfo View;
    };

    /// @brief Runtime GPU state a GuiSurface materializes lazily; defined in Surface.cpp.
    struct GuiSurfaceRuntime;

    /// @brief A Gui::Document mapped onto a world mesh, glowing through the scene's bloom.
    ///
    /// A reflected scene component that owns a live Gui::Document, renders it into a persistent HDR
    /// (RGBA16Sfloat) target each frame, and binds that target's bindless handle onto the material of
    /// the mesh it shares an entity with (its sibling MeshRenderer). The material domain (see
    /// GuiSurfaceDomain) selects how the document's HDR pixels become scene light — a translucent
    /// self-radiant surface (the default) or an opaque surface's emissive term. Because both material
    /// paths composite pre-bloom, bright document content blooms through the scene's existing bloom
    /// with no dedicated GUI bloom pass.
    ///
    /// The document is driven dirty-gated: it re-renders only when its layout changed, a transition
    /// stepped, a binding moved, or the target resolution changed — so a static panel costs one
    /// allocation and no per-frame repaint. Display-only: the panel data-binds and draws, but does not
    /// hit-test or take focus.
    ///
    /// The sibling MeshRenderer is where the document lands, so the surface declares it required
    /// (VE_REQUIRES): Scene::RemoveComponent refuses to strip the renderer while a surface sits
    /// beside it, naming this type as the reason.
    ///
    /// The runtime resources (the live document, the HDR target) are materialized on the first Drive.
    /// A cooked Document recipe is instantiated automatically; an imperatively-built document is
    /// injected through SetDocument before the first Drive.
    struct GuiSurface
    {
        /// @brief Default-constructs an unmaterialized surface (its runtime is empty until Drive).
        GuiSurface();
        /// @brief Releases the runtime document and HDR target.
        ~GuiSurface();
        /// @brief Move-constructs, transferring the runtime state.
        GuiSurface(GuiSurface&&) noexcept;
        /// @brief Move-assigns, transferring the runtime state.
        GuiSurface& operator=(GuiSurface&&) noexcept;

        GuiSurface(const GuiSurface&) = delete;
        GuiSurface& operator=(const GuiSurface&) = delete;

        /// @brief The cooked document recipe instantiated on the first Drive.
        ///
        /// Empty when the document is supplied imperatively through SetDocument. When set and loaded,
        /// the first Drive instantiates an independent live tree from it.
        AssetHandle<Gui::UIDocument> Document;

        /// @brief The extent, in logical points, the document lays out against.
        ///
        /// The HDR target is this times PixelScale pixels; at the default scale of 1 the two are the
        /// same number and a point is a pixel.
        uvec2 Resolution{512, 512};

        /// @brief Physical target pixels per logical layout point (1 = one pixel per point).
        ///
        /// Splits the two jobs Resolution would otherwise do: the document still lays out at
        /// Resolution logical points, while the HDR target allocates round(Resolution * PixelScale)
        /// pixels and the draw is magnified into it — so authored styles are unchanged and the panel's
        /// text and edges resolve at the target's density. A hidpi consumer sets 2 rather than
        /// doubling every authored size. Named PixelScale, not RenderScale, because that name already
        /// means dynamic scene-resolution scaling on Viewport.
        ///
        /// Clamped on drive so the derived extent is allocatable: at least one pixel per axis and
        /// within the device's maxImageDimension2D — an unclamped scale on a large surface would ask
        /// for an allocation that fails rather than one that merely looks wrong.
        f32 PixelScale = 1.0f;

        /// @brief Which material path turns the document's HDR texture into scene light.
        GuiSurfaceDomain Domain = GuiSurfaceDomain::Translucent;

        /// @brief The presentation driver instantiated with this surface's document; Null = undriven.
        ///
        /// Names a driver in the host-owned GuiDriverRegistry (see Veng/Gui/Driver.h), exactly as
        /// GuiOverlay does. When set and the claiming viewport resolves it, Drive instantiates the
        /// driver once, owns it for the runtime's lifetime, re-runs OnInstantiate on any document
        /// re-instantiate, and calls OnUpdate each drive — ahead of the document's own render, so what
        /// the driver writes is what this frame's panel shows. Null (the default) leaves the surface
        /// undriven; a consumer drives the document it borrows through GetDocument instead.
        GuiDriverId Driver = GuiDriverId::Null;

        /// @brief The Viewer entity whose devices drive this panel when it is interactive.
        ///
        /// A world panel has no host viewport to inherit a seat from, so the game names one
        /// explicitly. Entity::Null (the default) leaves the surface display-only: the world-space
        /// input adapter is never consulted for it, so a scene of non-interactive holograms and
        /// monitors costs no input work. A non-null seat lets the game route that seat's pointer into
        /// the document (under a SeatFocusScope) through the world-space input adapter. The reference
        /// remaps on prefab spawn like any intra-prefab Entity reference.
        Entity Seat = Entity::Null;

        /// @brief Runtime GPU state, materialized on the first Drive; empty until then.
        mutable Unique<GuiSurfaceRuntime> Runtime;

        /// @brief Injects an imperatively-built document, taking ownership.
        ///
        /// The alternative to a cooked Document recipe: a document built and mutated in C++. Call
        /// before the first Drive; it materializes the runtime if needed and adopts the document.
        /// @param document  The live document to drive, or null to clear the injected instance.
        void SetDocument(Unique<Gui::Document> document);

        /// @brief Returns the live document instance, or null before it is materialized.
        [[nodiscard]] Gui::Document* GetDocument() const;

        /// @brief Returns the persistent HDR target the document renders into, or null before Drive.
        [[nodiscard]] Gui::RenderTarget* GetTarget() const;

        /// @brief Whether the most recent Drive re-recorded the document (the dirty-gate outcome).
        [[nodiscard]] bool WasRenderedLastDrive() const;

        /// @brief Drives the document into its HDR target and binds the handle onto @p material.
        ///
        /// Materializes the runtime on first use (instantiating the Document recipe, allocating the
        /// HDR target, and its own GuiScenePass), refreshes bindings, and — when the document is
        /// dirty, animating, or the resolution or pixel scale changed — lays it out at Resolution
        /// logical points, records it magnified into the round(Resolution * PixelScale)-pixel HDR
        /// target (leaving the target shader-readable), and binds the target's
        /// GetOutputHandle onto @p material for the surface's domain. Records into @p cmd ahead of the
        /// scene pass that samples the panel, so the producer-before-consumer handoff needs no extra
        /// barrier. Each surface owns its GuiScenePass, so its per-frame geometry ring is never shared.
        /// When @p driver names a scene and a driver catalog and this surface's Driver resolves, the
        /// driver runs between the document's instantiate and its render: OnInstantiate on the first
        /// drive and on any re-instantiate, then OnUpdate. It therefore runs *before* the scene's
        /// render gather — a surface is sampled by the scene it sits in, so its document has to be
        /// current before the scene is gathered, and what a driver stamps is read by that same gather.
        /// @param context   The render context the target and runtime allocate on.
        /// @param assets    The asset manager the GuiScenePass and a cooked Document recipe load through.
        /// @param cmd       The command buffer the document render records into.
        /// @param sampler   The bindless sampler bound alongside the document handle.
        /// @param material  The sibling mesh material to bind the handle onto; null skips binding.
        /// @param delta     The frame time step, in seconds, forwarded to the document drive.
        /// @param driver    The driver services for the viewport claiming this surface; empty = undriven.
        /// @return True when this Drive re-recorded the document, false when the dirty-gate skipped it.
        bool Drive(Renderer::Context& context, AssetManager& assets, Renderer::CommandBuffer& cmd,
                   Renderer::SamplerHandle sampler, MaterialInstance* material, f32 delta,
                   const GuiSurfaceDriveContext& driver = {}) const;

    private:
        /// @brief Instantiates the named driver on first use and runs its OnInstantiate/OnUpdate.
        ///
        /// A no-op for an undriven surface, for a drive whose services name no scene or catalog, and
        /// before the document instantiates. An id no registered driver claims is logged once and
        /// leaves the surface undriven — a recoverable miss, never an abort.
        /// @param runtime   This surface's materialized runtime.
        /// @param services  The driver services for this drive.
        /// @param delta     The frame time step, in seconds, handed to the driver's frame.
        void DriveDriver(GuiSurfaceRuntime& runtime, const GuiSurfaceDriveContext& services,
                         f32 delta) const;
    };
}

VE_ENUM(::Veng::GuiSurfaceDomain, 0xF83AA418CECFCB46ULL)
VE_ENUMERATOR(Translucent)
VE_ENUMERATOR(OpaqueEmissive)
VE_ENUM_END();

VE_REFLECT(::Veng::GuiSurface, 0x8D6C050074173888ULL)
VE_FIELD(Document, .DisplayName = "Document")
VE_FIELD(Resolution, .DisplayName = "Resolution", .Display = {.Min = 1})
VE_FIELD(PixelScale, .DisplayName = "Pixel Scale", .Display = {.Min = 0.25, .Max = 4.0})
VE_FIELD(Domain, .DisplayName = "Domain")
VE_FIELD(Driver, .DisplayName = "Driver")
VE_FIELD(Seat, .DisplayName = "Seat")
VE_REFLECT_END();

// The document is drawn into the sibling MeshRenderer's material and has nowhere else to land, so
// the renderer cannot be removed out from under a live surface: Scene::RemoveComponent refuses it.
VE_REQUIRES(::Veng::GuiSurface, ::Veng::MeshRenderer);
