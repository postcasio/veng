#include <Veng/Gui/Surface.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DocumentHost.h>
#include <Veng/Gui/DocumentTexture.h>
#include <Veng/Gui/Driver.h>
#include <Veng/Gui/DriverRegistry.h>
#include <Veng/Gui/UIDocument.h>
#include <Veng/Log.h>
#include <Veng/Renderer/Context.h>

namespace Veng
{
    /// @brief The document core and its render-to-texture presenter a GuiSurface materializes on Drive.
    struct GuiSurfaceRuntime
    {
        /// @brief The live/bound document core; constructed on the first Drive (needs the asset manager).
        Unique<Gui::DocumentHost> Host;
        /// @brief The render-to-texture presenter: the owned HDR target and the pass that fills it.
        Gui::DocumentTexture Texture;
        /// @brief A document injected through SetDocument before the host exists, adopted on first Drive.
        Unique<Gui::Document> Pending;
        /// @brief The instantiated presentation driver, or null when the surface is undriven.
        Unique<GuiDriver> Driver;
        /// @brief The document the driver was last OnInstantiate'd against; detects a re-instantiate.
        Gui::Document* DriverDocument = nullptr;
        /// @brief Whether the emissive material's white default has been applied once.
        bool EmissiveSeeded = false;
    };

    namespace
    {
        /// @brief Whether a material declares a field of the given name and kind.
        bool HasField(const MaterialInstance& material, std::string_view name,
                      MaterialField::FieldKind kind)
        {
            for (const MaterialField& field : material.GetFields())
            {
                if (field.Name == name && field.Kind == kind)
                {
                    return true;
                }
            }
            return false;
        }

        /// @brief Binds the document handle onto the surface material for the chosen domain.
        void BindMaterial(GuiSurfaceRuntime& runtime, GuiSurfaceDomain domain,
                          MaterialInstance& material, Renderer::TextureHandle handle,
                          Renderer::SamplerHandle sampler)
        {
            if (domain == GuiSurfaceDomain::OpaqueEmissive)
            {
                if (HasField(material, "EmissiveTexture", MaterialField::FieldKind::TextureHandle))
                {
                    material.SetTextureHandle("EmissiveTexture", handle);
                }
                if (HasField(material, "EmissiveSampler", MaterialField::FieldKind::SamplerHandle))
                {
                    material.SetSamplerHandle("EmissiveSampler", sampler);
                }
                // The consumer material's fragment writes EmissiveColor * texel into the emissive
                // g-buffer channel; a white default lets the document value pass through
                // unmodulated. Seeded once so a game tint set later wins over the default.
                if (!runtime.EmissiveSeeded &&
                    HasField(material, "EmissiveColor", MaterialField::FieldKind::Param))
                {
                    material.SetParam("EmissiveColor", vec4(1.0f, 1.0f, 1.0f, 0.0f));
                    runtime.EmissiveSeeded = true;
                }
                return;
            }

            // Translucent: the panel material samples a Texture/Sampler pair as its radiance.
            if (HasField(material, "Texture", MaterialField::FieldKind::TextureHandle))
            {
                material.SetTextureHandle("Texture", handle);
            }
            if (HasField(material, "Sampler", MaterialField::FieldKind::SamplerHandle))
            {
                material.SetSamplerHandle("Sampler", sampler);
            }
        }
    }

    GuiSurface::GuiSurface() = default;
    GuiSurface::~GuiSurface() = default;
    GuiSurface::GuiSurface(GuiSurface&&) noexcept = default;
    GuiSurface& GuiSurface::operator=(GuiSurface&&) noexcept = default;

    void GuiSurface::SetDocument(Unique<Gui::Document> document)
    {
        if (!Runtime)
        {
            Runtime = CreateUnique<GuiSurfaceRuntime>();
        }
        // The host is built lazily on the first Drive (it needs the asset manager), so before then an
        // injected document waits in Pending; after, it is adopted straight into the host.
        if (Runtime->Host)
        {
            Runtime->Host->SetDocument(std::move(document));
        }
        else
        {
            Runtime->Pending = std::move(document);
        }
    }

    Gui::Document* GuiSurface::GetDocument() const
    {
        if (!Runtime)
        {
            return nullptr;
        }
        return Runtime->Host ? Runtime->Host->Get() : Runtime->Pending.get();
    }

    Gui::RenderTarget* GuiSurface::GetTarget() const
    {
        return Runtime ? Runtime->Texture.GetTarget() : nullptr;
    }

    bool GuiSurface::WasRenderedLastDrive() const
    {
        return Runtime && Runtime->Texture.WasRenderedLastDrive();
    }

    bool GuiSurface::Drive(Renderer::Context& context, AssetManager& assets,
                           Renderer::CommandBuffer& cmd, Renderer::SamplerHandle sampler,
                           MaterialInstance* material, f32 delta,
                           const GuiSurfaceDriveContext& driver) const
    {
        VE_ASSERT(Resolution.x > 0 && Resolution.y > 0,
                  "GuiSurface::Drive: Resolution must be positive (got {}x{})", Resolution.x,
                  Resolution.y);
        VE_ASSERT(PixelScale > 0.0f, "GuiSurface::Drive: PixelScale must be positive (got {})",
                  PixelScale);

        // Clamp the scale so the derived target extent is allocatable: at least one pixel on the
        // smaller axis, and no larger than the device's 2D image limit on the larger one. An
        // authored scale that overshoots on a 4K surface would otherwise ask for hundreds of MB of
        // RGBA16Sfloat and fail the allocation outright.
        const f32 largestAxis = static_cast<f32>(std::max(Resolution.x, Resolution.y));
        const f32 smallestAxis = static_cast<f32>(std::min(Resolution.x, Resolution.y));
        const f32 scale =
            glm::clamp(PixelScale, 1.0f / smallestAxis,
                       static_cast<f32>(context.GetMaxImageDimension2D()) / largestAxis);

        if (!Runtime)
        {
            Runtime = CreateUnique<GuiSurfaceRuntime>();
        }
        GuiSurfaceRuntime& runtime = *Runtime;

        // Build the document core on first use, adopting any document injected before the host existed.
        if (!runtime.Host)
        {
            runtime.Host = CreateUnique<Gui::DocumentHost>(assets, assets.GetTypeRegistry());
            if (runtime.Pending)
            {
                runtime.Host->SetDocument(std::move(runtime.Pending));
            }
        }

        // Instantiate a cooked recipe on first use; an imperative document is injected through
        // SetDocument. With neither available yet (a recipe still loading), there is nothing to draw.
        if (runtime.Host->Get() == nullptr)
        {
            if (Document.IsLoaded())
            {
                runtime.Host->SetDocument(Gui::Document::Instantiate(*Document.Get(), assets));
            }
            else
            {
                return false;
            }
        }

        // Feed the named driver before the core refreshes its bindings, so what the driver wrote is
        // what this frame's bindings read and what the dirty-gate below sees.
        DriveDriver(runtime, assets, driver, delta);

        // Drive the core (refresh bindings, expose the live tree), then render it into the HDR target
        // dirty-gated. UpdateBindings precedes the texture's dirty check, so a moved binding re-renders.
        Gui::Document* const document = runtime.Host->Drive();
        if (document == nullptr)
        {
            return false;
        }
        const bool rendered = runtime.Texture.RenderToTarget(context, assets, cmd, *document,
                                                             Resolution, scale, delta);

        // Bind the target handle onto the surface material every frame: SetTextureHandle writes the
        // current frame-in-flight region, so the handle must land regardless of the dirty-gate.
        if (material != nullptr)
        {
            BindMaterial(runtime, Domain, *material, runtime.Texture.GetOutputHandle(), sampler);
        }

        return rendered;
    }

    void GuiSurface::DriveDriver(GuiSurfaceRuntime& runtime, AssetManager& assets,
                                 const GuiSurfaceDriveContext& services, const f32 delta) const
    {
        // Undriven is the common case and every one of these is a legitimate way to spell it: no
        // driver named, no scene or catalog (a viewport that does not claim this surface), or a
        // document that has not instantiated yet.
        Gui::Document* const document = runtime.Host->Get();
        if (Driver == GuiDriverId::Null || services.World == nullptr ||
            services.Drivers == nullptr || document == nullptr)
        {
            return;
        }

        if (runtime.Driver == nullptr)
        {
            runtime.Driver = services.Drivers->Instantiate(Driver);
            runtime.DriverDocument = nullptr;
            if (runtime.Driver == nullptr)
            {
                Log::Warn("GuiSurface names GuiDriver {:#018x}, which no registered driver claims; "
                          "leaving the surface undriven.",
                          static_cast<u64>(Driver));
                return;
            }
        }

        // Re-run OnInstantiate whenever the live document changed identity, so cached element
        // pointers stay valid — the same contract SetOnInstantiate carries.
        if (document != runtime.DriverDocument)
        {
            runtime.Driver->OnInstantiate(*document, *services.World, services.Seat);
            runtime.DriverDocument = document;
        }
        runtime.Driver->OnUpdate(GuiDriverFrame{
            .Document = *document,
            .Scene = *services.World,
            .Owner = services.Owner,
            .Seat = services.Seat,
            .Delta = delta,
            .Alpha = services.Alpha,
            .View = services.View,
            .Assets = assets,
            .Audio = services.Audio,
        });
    }
}
