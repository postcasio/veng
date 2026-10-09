#include <Veng/Gui/Overlay.h>

#include <utility>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DocumentHost.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/DocumentLayer.h>
#include <Veng/Gui/Driver.h>
#include <Veng/Gui/DriverRegistry.h>
#include <Veng/Localization/Localization.h>
#include <Veng/Log.h>
#include <Veng/Renderer/Viewport.h>

namespace Veng
{
    /// @brief The document core, its screen presenter, and the deferred binding a GuiOverlay drives.
    struct GuiOverlayRuntime
    {
        /// @brief The bound binding context, applied to the host on materialize; borrowed, or null.
        Gui::BindingContext* Context = nullptr;
        /// @brief The on-instantiate callback, applied to the host on materialize; empty until set.
        function<void(Gui::Document&)> OnInstantiate;
        /// @brief The live/bound document core; constructed on the first Drive (needs the asset manager).
        Unique<Gui::DocumentHost> Host;
        /// @brief The screen-space presenter over Host; constructed alongside it on the first Drive.
        Unique<Gui::DocumentLayer> Layer;
        /// @brief The Interactive value last applied to the layer, to reapply only on a change.
        bool AppliedInteractive = false;
        /// @brief The instantiated presentation driver, or null when the overlay is undriven.
        Unique<GuiDriver> Driver;
        /// @brief The document the driver was last OnInstantiate'd against; detects a re-instantiate.
        Gui::Document* DriverDocument = nullptr;
        /// @brief Whether the drivers are attached: a drive reached the live document, no detach since.
        bool Attached = false;
        /// @brief The viewport whose drive attached the drivers; compared, never dereferenced.
        Renderer::ViewportId AttachedBy;
        /// @brief The seat the attached drivers answer to, handed back to them at detach.
        Entity Seat = Entity::Null;
        /// @brief The localization service the attached drivers were handed; never null once attached.
        const Localization::Localization* Localization = nullptr;
        /// @brief The recipe the host was built for; a Document re-pointed elsewhere rebuilds the host.
        AssetId HostDocument;
        /// @brief The resident composite material, LoadSync'd once from Material on the first DriveHdr.
        AssetHandle<MaterialInstance> CompositeMaterial;
        /// @brief Whether the composite-material load was attempted (so a failed load is not retried).
        bool CompositeMaterialAttempted = false;
    };

    namespace
    {
        /// @brief Detaches the attached drivers: the overlay's own OnDetach, then its components'.
        void DetachDrivers(GuiOverlayRuntime& runtime, Scene& scene, const Entity owner)
        {
            if (!runtime.Attached)
            {
                return;
            }
            runtime.Attached = false;
            Gui::Document* const document = runtime.Host != nullptr ? runtime.Host->Get() : nullptr;
            if (document == nullptr)
            {
                runtime.DriverDocument = nullptr;
                return;
            }
            const GuiDriverContext context{.Document = *document,
                                           .Root = document->Root(),
                                           .Scene = scene,
                                           .Owner = owner,
                                           .Seat = runtime.Seat,
                                           .Localization = *runtime.Localization};
            if (runtime.Driver != nullptr && runtime.DriverDocument != nullptr)
            {
                runtime.Driver->OnDetach(context);
            }
            runtime.DriverDocument = nullptr;
            document->DetachComponents(scene, owner, runtime.Seat, *runtime.Localization);
        }

        /// @brief Instantiates the named driver once, attaches it, and runs this frame's updates.
        ///
        /// Shared by Drive and DriveHdr. A live document whose identity changed since the driver
        /// attached is a re-instantiate: the driver detaches from it first, so the two hooks pair.
        void RunDrivers(GuiOverlayRuntime& runtime, const GuiDriverId id,
                        GuiDriverRegistry* const drivers, const GuiDriverFrame& frame,
                        const Renderer::ViewportId viewport)
        {
            Gui::Document& document = frame.Document;

            // Instantiate the named driver once, when a registry is available and the id resolves;
            // an unresolved id logs once and leaves the overlay undriven (a recoverable miss).
            if (runtime.Driver == nullptr && id != GuiDriverId::Null && drivers != nullptr)
            {
                runtime.Driver = drivers->Instantiate(id);
                runtime.DriverDocument = nullptr;
                if (runtime.Driver == nullptr)
                {
                    Log::Warn("GuiOverlay names GuiDriver {:#018x}, which no registered driver "
                              "claims; leaving the overlay undriven.",
                              static_cast<u64>(id));
                }
            }

            const GuiDriverContext context{.Document = document,
                                           .Root = document.Root(),
                                           .Scene = frame.Scene,
                                           .Owner = frame.Owner,
                                           .Seat = frame.Seat,
                                           .Localization = frame.Localization};
            if (runtime.Driver != nullptr && runtime.DriverDocument != nullptr &&
                runtime.DriverDocument != &document)
            {
                runtime.Driver->OnDetach(context);
                runtime.DriverDocument = nullptr;
            }

            runtime.Attached = true;
            runtime.AttachedBy = viewport;
            runtime.Seat = frame.Seat;
            runtime.Localization = &frame.Localization;

            if (runtime.Driver != nullptr)
            {
                // Re-run OnInstantiate whenever the driver is not attached to the live document (a
                // first drive, a re-instantiate, or a drive after a detach), so cached element
                // pointers stay valid — exactly like SetOnInstantiate. A whole-document driver
                // drives the document root.
                if (runtime.DriverDocument != &document)
                {
                    runtime.Driver->OnInstantiate(context);
                    runtime.DriverDocument = &document;
                }
                VE_PROFILE_SCOPE("Gui/DriverUpdate");
                runtime.Driver->OnUpdate(frame);
            }

            // Drive the document's embedded component drivers — those run whether or not the
            // overlay itself is driven, so a plain overlay may still host a self-driving component.
            VE_PROFILE_SCOPE("Gui/DriveComponents");
            document.DriveComponents(drivers, frame);
        }
    }

    GuiOverlay::GuiOverlay() = default;
    GuiOverlay::~GuiOverlay() = default;
    GuiOverlay::GuiOverlay(GuiOverlay&&) noexcept = default;
    GuiOverlay& GuiOverlay::operator=(GuiOverlay&&) noexcept = default;

    GuiOverlayRuntime& GuiOverlay::EnsureRuntime() const
    {
        if (Runtime == nullptr)
        {
            Runtime = std::make_unique<GuiOverlayRuntime>();
        }
        return *Runtime;
    }

    void GuiOverlay::SetContext(Gui::BindingContext* context)
    {
        GuiOverlayRuntime& runtime = EnsureRuntime();
        runtime.Context = context;
        if (runtime.Host != nullptr)
        {
            runtime.Host->SetContext(context);
        }
    }

    void GuiOverlay::SetOnInstantiate(function<void(Gui::Document&)> callback)
    {
        GuiOverlayRuntime& runtime = EnsureRuntime();
        runtime.OnInstantiate = std::move(callback);
        if (runtime.Host != nullptr)
        {
            runtime.Host->SetOnInstantiate(runtime.OnInstantiate);
        }
    }

    Gui::Document* GuiOverlay::GetDocument() const
    {
        return Runtime != nullptr && Runtime->Host != nullptr ? Runtime->Host->Get() : nullptr;
    }

    Gui::DocumentHost* GuiOverlay::GetHost() const
    {
        return Runtime != nullptr ? Runtime->Host.get() : nullptr;
    }

    MaterialInstance* GuiOverlay::GetCompositeMaterial() const
    {
        return Runtime != nullptr ? Runtime->CompositeMaterial.Get() : nullptr;
    }

    void GuiOverlay::EnsureHost(AssetManager& assets) const
    {
        GuiOverlayRuntime& runtime = EnsureRuntime();
        if (runtime.Host != nullptr)
        {
            return;
        }
        runtime.HostDocument = Document.Id();

        // The host is id-driven from the authored recipe; it does its own LoadSync on the first Drive
        // (a cache hit on the resident prefab dependency) and logs a failed load once. The deferred
        // binding stored before the host existed is applied here, so a binding system that ran ahead
        // of the first render takes effect on instantiate.
        runtime.Host =
            std::make_unique<Gui::DocumentHost>(assets, assets.GetTypeRegistry(), Document.Id());
        if (runtime.Context != nullptr)
        {
            runtime.Host->SetContext(runtime.Context);
        }
        if (runtime.OnInstantiate)
        {
            runtime.Host->SetOnInstantiate(runtime.OnInstantiate);
        }

        runtime.Layer = std::make_unique<Gui::DocumentLayer>(*runtime.Host, Layer);
        runtime.Layer->SetInteractive(Interactive);
        runtime.AppliedInteractive = Interactive;
    }

    void GuiOverlay::Drive(Renderer::Viewport& viewport, AssetManager& assets, Scene& scene,
                           const Entity owner, GuiDriverRegistry* const drivers,
                           const Audio::ScopedAudio& audio, const Haptics::ScopedHaptics& haptics,
                           const Gui::GuiTranslator* const translator,
                           const Localization::Localization* const localization) const
    {
        ReleaseRepointedHost(scene, owner);
        EnsureHost(assets);
        GuiOverlayRuntime& runtime = *Runtime;

        // Interactive is a reflected field a system may flip at runtime; reapply only on a change.
        if (Interactive != runtime.AppliedInteractive)
        {
            runtime.Layer->SetInteractive(Interactive);
            runtime.AppliedInteractive = Interactive;
        }

        // Present first: it instantiates the document (or re-instantiates it) and returns the live
        // tree, which the driver's OnInstantiate/OnUpdate then read.
        Gui::Document* const document = [&]
        {
            VE_PROFILE_SCOPE("Gui/HostDrive");
            return runtime.Layer->Present(viewport);
        }();
        if (document == nullptr)
        {
            return;
        }

        // Auto-wire the host's translator so engine-managed markup is localized without game code; a
        // no-op once installed, and re-applied for free after a document re-instantiate.
        document->SetTranslator(translator);

        // Restamped every drive rather than on a change: a re-instantiated tree comes back declaring
        // no cursor, and the store is cheaper than the comparison that would avoid it.
        document->SetDrawsCursor(DrawsCursor);

        // A host that wired no service hands the driver the inert null-object, so the frame's
        // reference always has a referent and a driver formats text with no null-guard.
        const Localization::Localization& strings =
            localization != nullptr ? *localization : Localization::NullService();

        // The ambient frame every driver on this document reads; a component driver gets it rebased
        // onto its boundary, so both the overlay's own driver and its components share one View/seat.
        const GuiDriverFrame frame{
            .Document = *document,
            .Root = &document->Root(),
            .Scene = scene,
            .Owner = owner,
            .Seat = viewport.GetSeat().Viewer,
            .Delta = viewport.GetViewDelta(),
            .Alpha = viewport.GetViewAlpha(),
            .View = SystemViewInfo{.Camera = viewport.GetPresentedCamera(),
                                   .Region = viewport.GetRegion(),
                                   .UiScale = viewport.GetUiScale()},
            .Assets = assets,
            .Audio = audio,
            .Haptics = haptics,
            .Localization = strings,
        };

        RunDrivers(runtime, Driver, drivers, frame, viewport.GetId());
    }

    void GuiOverlay::DriveHdr(Renderer::Viewport& viewport, AssetManager& assets, Scene& scene,
                              const Entity owner, GuiDriverRegistry* const drivers,
                              const Audio::ScopedAudio& audio,
                              const Haptics::ScopedHaptics& haptics, const vec2 docExtent,
                              const f32 delta, Gui::DrawList& out,
                              const Gui::GuiTranslator* const translator,
                              const Localization::Localization* const localization) const
    {
        out.Clear();
        ReleaseRepointedHost(scene, owner);
        EnsureHost(assets);
        GuiOverlayRuntime& runtime = *Runtime;

        // Resolve the optional composite material once: a SceneHdrPreBloom overlay naming a material is
        // composited through it (the glow-split path), an absent or failed one takes the direct blend.
        // LoadSync is a cache hit on the resident pack dependency after the first call; a null id or a
        // failed load leaves CompositeMaterial empty, and GetCompositeMaterial() then returns nullptr.
        if (!runtime.CompositeMaterialAttempted && Material.Id().IsValid())
        {
            runtime.CompositeMaterialAttempted = true;
            const AssetResult<AssetHandle<MaterialInstance>> loaded =
                assets.LoadSync<MaterialInstance>(Material.Id());
            if (loaded)
            {
                runtime.CompositeMaterial = *loaded;
            }
            else
            {
                Log::Error("GuiOverlay composite material {:#018x} load failed: {}",
                           Material.Id().Value, loaded.error().Detail);
            }
        }

        // Drive the host directly (load, instantiate, bind refresh) rather than through the
        // DocumentLayer: an HDR overlay is composited by the engine's pre-bloom pass, not attached to
        // the viewport's post-tonemap layer stack, so its document never joins that stack.
        Gui::Document* const document = [&]
        {
            VE_PROFILE_SCOPE("Gui/HostDrive");
            return runtime.Host->Drive();
        }();
        if (document == nullptr)
        {
            return;
        }

        // Auto-wire the host's translator, as Drive does — see there.
        document->SetTranslator(translator);

        // Restamped every drive, as Drive does — see there.
        document->SetDrawsCursor(DrawsCursor);

        // Interactive says this overlay takes input whatever it composites into, so the flag reaches
        // the document here the way the layer applies it on the stack. The comparand is the live
        // document rather than the last applied value: there is no attach to reapply the flag on, and
        // a re-instantiated tree comes back display-only.
        if (document->IsInteractive() != Interactive)
        {
            document->SetInteractive(Interactive);
            runtime.AppliedInteractive = Interactive;
        }

        // The null-object stands in for an unwired host, as Drive does — see there.
        const Localization::Localization& strings =
            localization != nullptr ? *localization : Localization::NullService();

        const GuiDriverFrame frame{
            .Document = *document,
            .Root = &document->Root(),
            .Scene = scene,
            .Owner = owner,
            .Seat = viewport.GetSeat().Viewer,
            .Delta = delta,
            .Alpha = viewport.GetViewAlpha(),
            .View = SystemViewInfo{.Camera = viewport.GetPresentedCamera(),
                                   .Region = viewport.GetRegion(),
                                   .UiScale = viewport.GetUiScale()},
            .Assets = assets,
            .Audio = audio,
            .Haptics = haptics,
            .Localization = strings,
        };

        RunDrivers(runtime, Driver, drivers, frame, viewport.GetId());

        // Lay the document out at the authored logical extent and build its geometry into the caller's
        // draw list; the engine projects and records it in the pre-bloom pass.
        document->Drive(docExtent, delta, out);
    }

    bool GuiOverlay::Prepare(AssetManager& assets) const
    {
        if (!Document.Id().IsValid())
        {
            return true;
        }
        EnsureHost(assets);
        return Runtime->Host->Prepare();
    }

    void GuiOverlay::DetachDriver(Scene& scene, const Entity owner) const
    {
        if (Runtime != nullptr)
        {
            DetachDrivers(*Runtime, scene, owner);
        }
    }

    bool GuiOverlay::IsDriverAttachedBy(const Renderer::Viewport& viewport) const
    {
        return Runtime != nullptr && Runtime->Attached && Runtime->AttachedBy == viewport.GetId();
    }

    void GuiOverlay::ReleaseRepointedHost(Scene& scene, const Entity owner) const
    {
        if (Runtime == nullptr || Runtime->Host == nullptr ||
            Runtime->HostDocument == Document.Id())
        {
            return;
        }
        // The drivers leave the old document while it is still live; the host and its presenter go
        // with it, and the next EnsureHost builds both for the recipe now named.
        DetachDrivers(*Runtime, scene, owner);
        Runtime->Layer.reset();
        Runtime->Host.reset();
    }

    void GuiOverlay::Detach(Renderer::Viewport& viewport, Scene& scene, const Entity owner) const
    {
        // An overlay that never drove holds no document, so there is nothing to detach.
        if (Runtime == nullptr || Runtime->Host == nullptr)
        {
            return;
        }

        // The drivers detach only from the viewport that attached them: another viewport letting go
        // of a document it never drove leaves them driving.
        if (IsDriverAttachedBy(viewport))
        {
            DetachDrivers(*Runtime, scene, owner);
        }

        // Detach only when the live document is hosted on this exact viewport: a document attached
        // elsewhere or already detached is left alone, so the call is idempotent and touches only what
        // Drive attached here. The host and its document survive for the next Drive to re-attach.
        Gui::Document* const document = Runtime->Host->Get();
        if (document != nullptr && document->GetHostViewport() == &viewport)
        {
            viewport.DetachDocument(*document);
        }
    }
}
