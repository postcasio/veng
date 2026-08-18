#include <Veng/Gui/Overlay.h>

#include <utility>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DocumentHost.h>
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
                           const Entity owner, GuiDriverRegistry* const drivers) const
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

        if (runtime.Driver != nullptr && document != nullptr)
        {
            // Re-run OnInstantiate whenever the live document changed identity (first instantiate or
            // a re-instantiate), so cached element pointers stay valid — exactly like SetOnInstantiate.
            if (document != runtime.DriverDocument)
            {
                runtime.Driver->OnInstantiate(*document, scene, viewport.GetSeat());
                runtime.DriverDocument = document;
            }
            runtime.Driver->OnUpdate(GuiDriverFrame{
                .Document = *document,
                .Scene = scene,
                .Owner = owner,
                .Seat = viewport.GetSeat(),
                .Delta = viewport.GetViewDelta(),
                .Alpha = viewport.GetViewAlpha(),
                .View = SystemViewInfo{.Camera = viewport.GetPresentedCamera(),
                                       .Region = viewport.GetRegion(),
                                       .UiScale = viewport.GetUiScale()},
            });
        }
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
