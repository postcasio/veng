#include <Veng/Gui/Overlay.h>

#include <utility>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DocumentHost.h>
#include <Veng/Gui/DocumentLayer.h>

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

    void GuiOverlay::Drive(Renderer::Viewport& viewport, AssetManager& assets) const
    {
        EnsureHost(assets);
        GuiOverlayRuntime& runtime = *Runtime;

        // Interactive is a reflected field a system may flip at runtime; reapply only on a change.
        if (Interactive != runtime.AppliedInteractive)
        {
            runtime.Layer->SetInteractive(Interactive);
            runtime.AppliedInteractive = Interactive;
        }

        runtime.Layer->Present(viewport);
    }
}
