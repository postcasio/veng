#include <Veng/Gui/Overlay.h>

#include <utility>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DocumentHost.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/DocumentLayer.h>
#include <Veng/Gui/Driver.h>
#include <Veng/Gui/DriverRegistry.h>
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
        /// @brief The resident composite material, LoadSync'd once from Material on the first DriveHdr.
        AssetHandle<MaterialInstance> CompositeMaterial;
        /// @brief Whether the composite-material load was attempted (so a failed load is not retried).
        bool CompositeMaterialAttempted = false;
    };

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
                           Audio::AudioEngine* const audio) const
    {
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
        Gui::Document* const document = runtime.Layer->Present(viewport);
        if (document == nullptr)
        {
            return;
        }

        // The ambient frame every driver on this document reads; a component driver gets it rebased
        // onto its boundary, so both the overlay's own driver and its components share one View/seat.
        const GuiDriverFrame frame{
            .Document = *document,
            .Root = &document->Root(),
            .Scene = scene,
            .Owner = owner,
            .Seat = viewport.GetSeat(),
            .Delta = viewport.GetViewDelta(),
            .Alpha = viewport.GetViewAlpha(),
            .View = SystemViewInfo{.Camera = viewport.GetPresentedCamera(),
                                   .Region = viewport.GetRegion(),
                                   .UiScale = viewport.GetUiScale()},
            .Assets = assets,
            .Audio = audio,
        };

        // Instantiate the named driver once, when a registry is available and the id resolves; an
        // unresolved id logs once and leaves the overlay undriven (a recoverable miss).
        if (runtime.Driver == nullptr && Driver != GuiDriverId::Null && drivers != nullptr)
        {
            runtime.Driver = drivers->Instantiate(Driver);
            runtime.DriverDocument = nullptr;
            if (runtime.Driver == nullptr)
            {
                Log::Warn("GuiOverlay names GuiDriver {:#018x}, which no registered driver claims; "
                          "leaving the overlay undriven.",
                          static_cast<u64>(Driver));
            }
        }

        if (runtime.Driver != nullptr)
        {
            // Re-run OnInstantiate whenever the live document changed identity (first instantiate or
            // a re-instantiate), so cached element pointers stay valid — exactly like SetOnInstantiate.
            // A whole-document driver drives the document root.
            if (document != runtime.DriverDocument)
            {
                runtime.Driver->OnInstantiate(*document, document->Root(), scene,
                                              viewport.GetSeat());
                runtime.DriverDocument = document;
            }
            runtime.Driver->OnUpdate(frame);
        }

        // Drive the document's embedded component drivers — those run whether or not the overlay
        // itself is driven, so a plain overlay may still host a self-driving component.
        document->DriveComponents(drivers, frame);
    }

    void GuiOverlay::DriveHdr(Renderer::Viewport& viewport, AssetManager& assets, Scene& scene,
                              const Entity owner, GuiDriverRegistry* const drivers,
                              Audio::AudioEngine* const audio, const vec2 docExtent,
                              const f32 delta, Gui::DrawList& out) const
    {
        out.Clear();
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
        Gui::Document* const document = runtime.Host->Drive();
        if (document == nullptr)
        {
            return;
        }

        const GuiDriverFrame frame{
            .Document = *document,
            .Root = &document->Root(),
            .Scene = scene,
            .Owner = owner,
            .Seat = viewport.GetSeat(),
            .Delta = delta,
            .Alpha = viewport.GetViewAlpha(),
            .View = SystemViewInfo{.Camera = viewport.GetPresentedCamera(),
                                   .Region = viewport.GetRegion(),
                                   .UiScale = viewport.GetUiScale()},
            .Assets = assets,
            .Audio = audio,
        };

        // Instantiate and run the named driver, mirroring Drive: an unresolved id logs once and
        // leaves the overlay undriven, OnInstantiate re-runs on a document re-instantiate, OnUpdate
        // runs each frame.
        if (runtime.Driver == nullptr && Driver != GuiDriverId::Null && drivers != nullptr)
        {
            runtime.Driver = drivers->Instantiate(Driver);
            runtime.DriverDocument = nullptr;
            if (runtime.Driver == nullptr)
            {
                Log::Warn("GuiOverlay names GuiDriver {:#018x}, which no registered driver claims; "
                          "leaving the overlay undriven.",
                          static_cast<u64>(Driver));
            }
        }
        if (runtime.Driver != nullptr)
        {
            if (document != runtime.DriverDocument)
            {
                runtime.Driver->OnInstantiate(*document, document->Root(), scene,
                                              viewport.GetSeat());
                runtime.DriverDocument = document;
            }
            runtime.Driver->OnUpdate(frame);
        }
        document->DriveComponents(drivers, frame);

        // Lay the document out at the authored logical extent and build its geometry into the caller's
        // draw list; the engine projects and records it in the pre-bloom pass.
        document->Drive(docExtent, delta, out);
    }

    void GuiOverlay::Detach(Renderer::Viewport& viewport) const
    {
        // An overlay that never drove holds no document, so there is nothing to detach.
        if (Runtime == nullptr || Runtime->Host == nullptr)
        {
            return;
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
