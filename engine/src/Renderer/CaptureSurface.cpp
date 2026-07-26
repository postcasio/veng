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
        /// @brief Clears the material slots the last drive filled, so no released handle stays bound.
        ~CaptureSurfaceRuntime();

        /// @brief The owned capture, self-unregistering from the drive-list on destruction.
        Unique<SceneCapture> Capture;
        /// @brief The sampler the material reads the capture output through.
        Ref<Sampler> Sampler;
        /// @brief Bindless handle of Sampler, bound alongside the capture output.
        SamplerHandle SamplerHandle;
        /// @brief Faces still owed before the current refresh settles; 0 leaves an OnDemand capture idle.
        u32 PendingFaces = SceneCapture::FaceCount;

        /// @brief The material the last drive bound onto, held resident so the unbind can reach it.
        AssetHandle<MaterialInstance> BoundMaterial;
        /// @brief Texture-slot name the last drive filled on BoundMaterial; empty when it filled none.
        string BoundTextureSlot;
        /// @brief Sampler-slot name the last drive filled on BoundMaterial; empty when it filled none.
        string BoundSamplerSlot;
        /// @brief Centre-slot name the last drive filled on BoundMaterial; empty when it filled none.
        string BoundCenterSlot;
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

        /// @brief Writes the unbound state back into the slots a drive filled, and forgets the binding.
        void ClearBoundSlots(CaptureSurfaceRuntime& runtime)
        {
            if (MaterialInstance* const material = runtime.BoundMaterial.Get(); material != nullptr)
            {
                // The handle slots return to the sentinel they carried before the first drive: the
                // capture's output slot is released with the capture, and the next registration reuses
                // it, so leaving the index bound would sample an unrelated texture. The centre's w goes
                // to 0, which is the signal a consuming fragment gates its sample on.
                if (!runtime.BoundTextureSlot.empty())
                {
                    material->SetTextureHandle(runtime.BoundTextureSlot, TextureHandle{});
                }
                if (!runtime.BoundSamplerSlot.empty())
                {
                    material->SetSamplerHandle(runtime.BoundSamplerSlot, SamplerHandle{});
                }
                if (!runtime.BoundCenterSlot.empty())
                {
                    material->SetParam(runtime.BoundCenterSlot, vec4(0.0f));
                }
            }

            runtime.BoundMaterial = {};
            runtime.BoundTextureSlot.clear();
            runtime.BoundSamplerSlot.clear();
            runtime.BoundCenterSlot.clear();
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

    CaptureSurfaceRuntime::~CaptureSurfaceRuntime()
    {
        // Clearing before the members release keeps the capture's output slot live while the material
        // that named it is overwritten, so no frame can be recorded against a freed slot.
        ClearBoundSlots(*this);
    }

    CaptureSurface::CaptureSurface() = default;
    // The runtime's destructor unbinds, so every path that drops it — component destruction, entity or
    // scene teardown, a move-assignment over a live component — clears the slots that drive filled.
    CaptureSurface::~CaptureSurface() = default;
    CaptureSurface::CaptureSurface(CaptureSurface&&) noexcept = default;
    CaptureSurface& CaptureSurface::operator=(CaptureSurface&&) noexcept = default;

    void CaptureSurface::Unbind() const
    {
        if (Runtime)
        {
            ClearBoundSlots(*Runtime);
        }
    }

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
                                        const Entity entity, const vec3& position, const f32 alpha,
                                        const AssetHandle<MaterialInstance>& material) const
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
        // The source excludes the driving entity: a surface is not part of its own environment.
        const bool pushThisFrame =
            Refresh == CaptureRefresh::EveryFrame || runtime.PendingFaces > 0;
        if (pushThisFrame)
        {
            runtime.Capture->SetView(
                {.World = &world, .Position = position, .Exclude = entity, .Alpha = alpha});
            if (runtime.PendingFaces > 0)
            {
                --runtime.PendingFaces;
            }
        }

        // Bind the capture output onto the sibling material's named slots every frame:
        // SetTextureHandle writes the current frame-in-flight region, so the handle must land
        // regardless of the push decision. The slot names default to Texture/Sampler.
        if (MaterialInstance* const target = material.Get(); target != nullptr)
        {
            // Re-record which slots this drive filled, so a renamed slot's old binding is not the one
            // the unbind clears.
            runtime.BoundTextureSlot.clear();
            runtime.BoundSamplerSlot.clear();
            runtime.BoundCenterSlot.clear();

            const TextureHandle output = runtime.Capture->GetOutputHandle();
            if (HasField(*target, TextureSlot, MaterialField::FieldKind::TextureHandle))
            {
                target->SetTextureHandle(TextureSlot, output);
                runtime.BoundTextureSlot = TextureSlot;
            }
            if (HasField(*target, SamplerSlot, MaterialField::FieldKind::SamplerHandle))
            {
                target->SetSamplerHandle(SamplerSlot, runtime.SamplerHandle);
                runtime.BoundSamplerSlot = SamplerSlot;
            }
            // The centre is what a parallax-correcting fragment intersects its proxy volume about, and
            // w is 1 only once an output slot exists — so a fragment can tell an unpopulated slot from
            // a probe at the origin and reach its fallback instead of indexing the array with it.
            if (!CenterSlot.empty() &&
                HasField(*target, CenterSlot, MaterialField::FieldKind::Param))
            {
                target->SetParam(CenterSlot, vec4(position, output.IsValid() ? 1.0f : 0.0f));
                runtime.BoundCenterSlot = CenterSlot;
            }

            // Hold the material resident only when something was actually written onto it.
            if (!runtime.BoundTextureSlot.empty() || !runtime.BoundSamplerSlot.empty() ||
                !runtime.BoundCenterSlot.empty())
            {
                runtime.BoundMaterial = material;
            }
        }

        return runtime.Capture.get();
    }
}
