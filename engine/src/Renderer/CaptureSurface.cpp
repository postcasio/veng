#include <Veng/Renderer/CaptureSurface.h>

#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/SceneCapture.h>

namespace Veng::Renderer
{
    /// @brief The capture and its output sampler a CaptureSurface materializes on the first Drive.
    struct CaptureSurfaceRuntime
    {
        /// @brief The owned capture, self-unregistering from the drive-list on destruction.
        Unique<SceneCapture> Capture;
        /// @brief The sampler the material reads the capture output through.
        Ref<Sampler> Sampler;
        /// @brief Bindless handle of Sampler, bound alongside the capture output.
        SamplerHandle SamplerHandle;
        /// @brief Faces still owed before the current refresh settles; 0 leaves an OnDemand capture idle.
        u32 PendingFaces = SceneCapture::FaceCount;
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

        /// @brief A lean renderer config for a capture: the heavy per-view batteries multiply by six
        ///        faces, and the capture samples pre-tonemap HDR, so bloom/AO/shadows/SSR are dropped.
        SceneRendererSettings CaptureSettings()
        {
            SceneRendererSettings settings;
            settings.Bloom = false;
            settings.Shadows = false;
            settings.PunctualShadows = false;
            settings.AO = false;
            settings.SSR = false;
            settings.TAA = false;
            return settings;
        }
    }

    CaptureSurface::CaptureSurface() = default;
    CaptureSurface::~CaptureSurface() = default;
    CaptureSurface::CaptureSurface(CaptureSurface&&) noexcept = default;
    CaptureSurface& CaptureSurface::operator=(CaptureSurface&&) noexcept = default;

    void CaptureSurface::MarkDirty()
    {
        if (!Runtime)
        {
            Runtime = CreateUnique<CaptureSurfaceRuntime>();
        }
        Runtime->PendingFaces = SceneCapture::FaceCount;
    }

    SceneCapture* CaptureSurface::GetCapture() const
    {
        return Runtime ? Runtime->Capture.get() : nullptr;
    }

    TextureHandle CaptureSurface::GetOutputHandle() const
    {
        return Runtime && Runtime->Capture ? Runtime->Capture->GetOutputHandle() : TextureHandle{};
    }

    bool CaptureSurface::IsRefreshing() const
    {
        if (Refresh == CaptureRefresh::EveryFrame)
        {
            return true;
        }
        // Before the runtime materializes an OnDemand capture is still owed its first refresh.
        return !Runtime || Runtime->PendingFaces > 0;
    }

    SceneCapture* CaptureSurface::Drive(Context& context, AssetManager& assets, const Scene& world,
                                        const vec3& position, MaterialInstance* material) const
    {
        VE_ASSERT(Resolution > 0, "CaptureSurface::Drive: Resolution must be positive (got {})",
                  Resolution);

        if (!Runtime)
        {
            Runtime = CreateUnique<CaptureSurfaceRuntime>();
        }
        CaptureSurfaceRuntime& runtime = *Runtime;

        // Build the capture and its output sampler on first use. The sampler is a clamp sampler over
        // the octahedral map — the same edge-clamp the capture's own resample uses.
        if (!runtime.Capture)
        {
            runtime.Capture = SceneCapture::Create({
                .Context = context,
                .Assets = assets,
                .FaceResolution = Resolution,
                .Settings = CaptureSettings(),
            });
            runtime.Sampler = Sampler::Create(context, {
                                                           .Name = "CaptureSurface Sampler",
                                                           .MagFilter = Filter::Linear,
                                                           .MinFilter = Filter::Linear,
                                                           .AddressModeU = AddressMode::ClampToEdge,
                                                           .AddressModeV = AddressMode::ClampToEdge,
                                                           .AddressModeW = AddressMode::ClampToEdge,
                                                       });
            runtime.SamplerHandle = context.GetBindlessRegistry().Register(runtime.Sampler);
        }

        // Push this frame's capture source when the refresh policy calls for it. EveryFrame always
        // pushes; OnDemand pushes only while faces are still owed, then idles — SceneCapture records
        // nothing on a frame with no fresh SetView, so a settled OnDemand capture costs nothing.
        const bool pushThisFrame =
            Refresh == CaptureRefresh::EveryFrame || runtime.PendingFaces > 0;
        if (pushThisFrame)
        {
            runtime.Capture->SetView({.World = &world, .Position = position});
            if (runtime.PendingFaces > 0)
            {
                --runtime.PendingFaces;
            }
        }

        // Bind the capture output onto the sibling material every frame: SetTextureHandle writes the
        // current frame-in-flight region, so the handle must land regardless of the push decision.
        if (material != nullptr)
        {
            if (HasField(*material, "Texture", MaterialField::FieldKind::TextureHandle))
            {
                material->SetTextureHandle("Texture", runtime.Capture->GetOutputHandle());
            }
            if (HasField(*material, "Sampler", MaterialField::FieldKind::SamplerHandle))
            {
                material->SetSamplerHandle("Sampler", runtime.SamplerHandle);
            }
        }

        return runtime.Capture.get();
    }
}
