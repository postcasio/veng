#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Gui/Driver.h>
#include <Veng/Gui/UIDocument.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class AssetManager;
    class GuiDriverRegistry;
    class Scene;

    namespace Gui
    {
        class Document;
        class DocumentHost;
        class BindingContext;
    }

    namespace Renderer
    {
        class Viewport;
    }

    /// @brief Runtime document state a GuiOverlay materializes lazily; defined in Overlay.cpp.
    struct GuiOverlayRuntime;

    /// @brief A Gui::Document presented on a viewport's screen-space layer stack.
    ///
    /// A reflected scene component authored on an entity, the screen-space peer of GuiSurface: where
    /// GuiSurface maps a document onto a world mesh (HDR, in-scene, glowing through bloom), a
    /// GuiOverlay attaches its document to the presenting viewport's layer stack (LDR, composited
    /// after tonemap, un-bloomed). The Viewport discovers every GuiOverlay in the scene it renders
    /// (its View<GuiOverlay>() loop) and drives the ones it claims (by seat, see TargetSeat), so the
    /// engine owns the load, instantiate, and attach the consumer otherwise hand-rolls; the game
    /// owns only the data binding, through the DocumentHost/BindingContext surface.
    ///
    /// The document lifecycle (a Gui::DocumentHost) and its screen presenter (a Gui::DocumentLayer)
    /// are materialized on the first Drive, which needs the asset manager the driving viewport
    /// supplies. A binding is deferrable before that first Drive: SetContext and SetOnInstantiate are
    /// stored and applied to the host when it is created, so a binding system that runs ahead of the
    /// first render has no ordering hole. The document data-binds like any other (`{obj.field}`);
    /// Interactive gates whether it takes input, and a system may flip it at runtime.
    ///
    /// The runtime is materialized on the first Drive; a component that never drives allocates none.
    struct GuiOverlay
    {
        /// @brief Default-constructs an undriven overlay (its runtime is empty until Drive).
        GuiOverlay();
        /// @brief Releases the runtime host and detaches the presented document from its viewport.
        ~GuiOverlay();
        /// @brief Move-constructs, transferring the runtime state.
        GuiOverlay(GuiOverlay&&) noexcept;
        /// @brief Move-assigns, transferring the runtime state.
        GuiOverlay& operator=(GuiOverlay&&) noexcept;

        GuiOverlay(const GuiOverlay&) = delete;
        GuiOverlay& operator=(const GuiOverlay&) = delete;

        /// @brief The cooked UI document this entity presents on the viewport layer stack.
        AssetHandle<Gui::UIDocument> Document;

        /// @brief The document's z-order in the viewport layer stack; higher composites over lower.
        i32 Layer = 0;

        /// @brief The presentation driver instantiated with this overlay's document; Null = undriven.
        ///
        /// Names a driver in the host-owned GuiDriverRegistry (see Veng/Gui/Driver.h). When set and
        /// the registry resolves it, Drive instantiates the driver on the first drive, owns it for the
        /// runtime's lifetime, re-runs OnInstantiate on any document re-instantiate, and calls
        /// OnUpdate each drive — the engine's per-instance path for a HUD's data binding. Null (the
        /// default) leaves the overlay undriven; a consumer binds it through SetContext instead.
        GuiDriverId Driver = GuiDriverId::Null;

        /// @brief Whether the document takes input, or is display-only.
        ///
        /// False (the default) leaves the overlay display-only: it data-binds and draws but hit-tests
        /// nothing and takes no focus. True routes the claiming viewport's seat input into the
        /// document. A system may flip it at runtime; the next Drive reapplies the change.
        bool Interactive = false;

        /// @brief The seat whose viewport presents this overlay under multi-viewport presentation.
        ///
        /// A scene presented by more than one viewport (split-screen) resolves which viewport claims
        /// this overlay by seat: a viewport claims the overlays whose target seat is its own bound
        /// seat (Viewport::GetSeat). The target seat is the entity's own seat when the GuiOverlay
        /// sits on a Viewer entity; else this TargetSeat; else unbound, in which case the sole (or
        /// primary) presenting viewport claims it. Entity::Null — the default — is the unbound case,
        /// which is every single-viewport HUD. The reference remaps on prefab spawn like any Entity
        /// reference.
        Entity TargetSeat = Entity::Null;

        /// @brief Runtime document state (host, presenter, deferred binding); empty until first use.
        ///
        /// Materialized on the first SetContext/SetOnInstantiate (to hold the deferred binding) or the
        /// first Drive (which additionally builds the GPU-free document host, needing the driving
        /// viewport's asset manager). Public so the component stays standard-layout for reflection.
        mutable Unique<GuiOverlayRuntime> Runtime;

        /// @brief Binds the context the document's `{obj.field}` bindings resolve against.
        ///
        /// The game's view-model, given as a Gui::BindingContext (a reflected data object plus a
        /// handler table). Callable before the first Drive: the context is stored and applied to the
        /// host on instantiate — a deferred bind with no ordering hole — and forwarded immediately
        /// when the host is already live. The context is borrowed; it must outlive the binding (or be
        /// cleared with nullptr first).
        /// @param context  The game-owned binding context, or nullptr to clear.
        void SetContext(Gui::BindingContext* context);

        /// @brief Sets the callback run after the document is instantiated and bound.
        ///
        /// Where a consumer resolves element pointers and does one-time setup; it runs once on the
        /// lazy instantiate and re-runs on any re-instantiate, so cached pointers stay correct.
        /// Callable before the first Drive (stored and applied on instantiate) and forwarded
        /// immediately when the document is already live. An empty function clears the callback.
        /// @param callback  The callback receiving the live document, or an empty function to clear.
        void SetOnInstantiate(function<void(Gui::Document&)> callback);

        /// @brief Returns the live document, or nullptr before the first Drive (or a failed load).
        [[nodiscard]] Gui::Document* GetDocument() const;

        /// @brief Returns the runtime document host, or nullptr before the first Drive materializes it.
        ///
        /// The host is created on the first Drive (it needs the driving viewport's asset manager), so
        /// this is null until then; bind through SetContext/SetOnInstantiate, which are callable
        /// before the host exists.
        [[nodiscard]] Gui::DocumentHost* GetHost() const;

        /// @brief Drives the overlay onto the viewport's layer stack (the Viewport's per-frame call).
        ///
        /// Materializes the host + layer on first use (instantiating the Document recipe and applying
        /// any deferred binding), reapplies Interactive when it changed, and presents the live
        /// document on @p viewport — attaching it to the layer stack at Layer, re-attaching across a
        /// document recreation, and reapplying the interactive flag on a fresh attach. Only the
        /// viewport that claims this overlay calls Drive, so the document never thrashes between
        /// viewports. A failed document load is logged once and leaves the overlay silent — a
        /// recoverable miss, never an abort.
        ///
        /// When Driver is set and @p drivers resolves it, the driver is instantiated on the first
        /// drive (owned in the runtime, destroyed with it), its OnInstantiate re-run whenever the
        /// document (re)instantiates, and its OnUpdate called each drive with the claiming viewport's
        /// real view. An unresolved or Null Driver leaves the overlay undriven.
        /// @param viewport  The claiming viewport to present the document on.
        /// @param assets    The asset manager the document recipe and its fonts load through.
        /// @param scene     The presented scene the overlay lives in, handed to the driver.
        /// @param drivers   The driver catalog the Driver id resolves against, or nullptr (undriven).
        void Drive(Renderer::Viewport& viewport, AssetManager& assets, Scene& scene,
                   GuiDriverRegistry* drivers) const;

        /// @brief Detaches the presented document from a viewport's layer stack — the inverse of Drive.
        ///
        /// Removes the live document from @p viewport's layer stack when it is hosted there, leaving the
        /// runtime host and its document intact so the next Drive re-attaches. Idempotent: an undriven
        /// overlay, a document hosted on another viewport, or a document already detached is a no-op.
        /// Used to release an overlay a viewport stopped presenting while its world stays alive (a world
        /// rebind), where ~GuiOverlay's destroy-time detach is the wrong lifetime. Only the document the
        /// engine attached through Drive is touched.
        /// @param viewport  The viewport to detach the document from, if it is hosted there.
        void Detach(Renderer::Viewport& viewport) const;

    private:
        /// @brief Ensures the runtime record exists, holding the deferred binding before first Drive.
        GuiOverlayRuntime& EnsureRuntime() const;

        /// @brief Materializes the document host + presenter on first Drive and applies the binding.
        /// @param assets  The asset manager the host loads its recipe through.
        void EnsureHost(AssetManager& assets) const;
    };
}

VE_REFLECT(::Veng::GuiOverlay, 0xC703A9C84AC4BA09ULL)
VE_FIELD(Document, .DisplayName = "Document")
VE_FIELD(Layer, .DisplayName = "Layer")
VE_FIELD(Driver, .DisplayName = "Driver")
VE_FIELD(Interactive, .DisplayName = "Interactive")
VE_FIELD(TargetSeat, .DisplayName = "Target Seat")
VE_REFLECT_END();
