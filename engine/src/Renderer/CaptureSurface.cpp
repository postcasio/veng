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
        /// @brief Bindless slot of the point sampler the material reads the distance map through.
        ///
        /// Declared before SamplerHandle, whose member name shadows the type from that point on.
        SamplerHandle DepthSamplerHandle;
        /// @brief Bindless slot of the shared sampler the material reads the capture output through.
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
        /// @brief Orientation-slot name the last drive filled on BoundMaterial; empty when none.
        string BoundOrientationSlot;
        /// @brief Distance-map texture-slot name the last drive filled; empty when it filled none.
        string BoundDepthTextureSlot;
        /// @brief Distance-map sampler-slot name the last drive filled; empty when it filled none.
        string BoundDepthSamplerSlot;
    };

    namespace
    {
        /// @brief The rotation an unbound orientation slot carries — world space, in xyzw order.
        constexpr vec4 IdentityOrientation{0.0f, 0.0f, 0.0f, 1.0f};

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
                // The distance map's handle rides back to the same sentinel as the radiance map: the
                // capture releases the slot and the next registration reuses it.
                if (!runtime.BoundDepthTextureSlot.empty())
                {
                    material->SetTextureHandle(runtime.BoundDepthTextureSlot, TextureHandle{});
                }
                if (!runtime.BoundDepthSamplerSlot.empty())
                {
                    material->SetSamplerHandle(runtime.BoundDepthSamplerSlot, SamplerHandle{});
                }
                if (!runtime.BoundCenterSlot.empty())
                {
                    material->SetParam(runtime.BoundCenterSlot, vec4(0.0f));
                }
                // The frame goes back to the identity rather than to zero: the centre's flag is what
                // gates the sample, so this value is unread once unbound, and a zero quaternion
                // normalizes to a NaN in a consumer that reads it without the gate.
                if (!runtime.BoundOrientationSlot.empty())
                {
                    material->SetParam(runtime.BoundOrientationSlot, IdentityOrientation);
                }
            }

            runtime.BoundMaterial = {};
            runtime.BoundTextureSlot.clear();
            runtime.BoundSamplerSlot.clear();
            runtime.BoundCenterSlot.clear();
            runtime.BoundOrientationSlot.clear();
            runtime.BoundDepthTextureSlot.clear();
            runtime.BoundDepthSamplerSlot.clear();
        }

        /// @brief A lean renderer config for a capture: the heavy per-view batteries multiply by six
        ///        faces, and the capture samples pre-tonemap HDR, so bloom/AO/SSR are dropped.
        ///
        /// Shadows are the one battery the authoring surface can ask back, because an enclosed
        /// interior renders flooded without them (see CaptureSurface::Shadows). Both shadow flags
        /// move together: an interior wants the enclosure's own occlusion, whichever kind of light
        /// is casting it.
        SceneRendererSettings CaptureSettings(bool shadows)
        {
            SceneRendererSettings settings;
            settings.Bloom = false;
            settings.Shadows = shadows;
            settings.PunctualShadows = shadows;
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

        // The sampler slot is not released: it is the registry's shared clamp sampler, named by
        // every other surface and pass wanting the same settings, so returning it here would free a
        // slot still being drawn through. The capture releases the texture slots it took.
    }

    vec4 PackCaptureOrientation(const mat3& faceBasis)
    {
        const quat rotation = glm::normalize(glm::quat_cast(faceBasis));
        return vec4(rotation.x, rotation.y, rotation.z, rotation.w);
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

    void CaptureSurface::MarkDirty() const
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
                                        const mat3& faceBasis,
                                        const AssetHandle<MaterialInstance>& material) const
    {
        VE_ASSERT(Resolution > 0, "CaptureSurface::Drive: Resolution must be positive (got {})",
                  Resolution);

        if (!Runtime)
        {
            Runtime = CreateUnique<CaptureSurfaceRuntime>();
        }
        CaptureSurfaceRuntime& runtime = *Runtime;

        // Build the capture on first use and take the sampler its output is read through. That is a
        // clamp sampler over the octahedral map — the same edge-clamp the capture's own resample
        // uses, and the same one every other clamped blit in the engine reads through.
        if (!runtime.Capture)
        {
            // The distance map is opt-in: an empty DepthTextureSlot builds none, so the depth atlas,
            // the distance map, and their pipelines and slots do not exist.
            const bool captureDistance = !DepthTextureSlot.empty();
            runtime.Capture = SceneCapture::Create({
                .Context = context,
                .Assets = assets,
                .FaceResolution = Resolution,
                .Settings = CaptureSettings(Shadows),
                .CaptureDistance = captureDistance,
                .DistanceResolution = DepthResolution,
            });
            runtime.SamplerHandle = context.GetBindlessRegistry()
                                        .AcquireSampler({
                                            .Name = "CaptureSurface Sampler",
                                            .MagFilter = Filter::Linear,
                                            .MinFilter = Filter::Linear,
                                            .AddressModeU = AddressMode::ClampToEdge,
                                            .AddressModeV = AddressMode::ClampToEdge,
                                            .AddressModeW = AddressMode::ClampToEdge,
                                        })
                                        .Handle;
            if (captureDistance)
            {
                // A point sampler for the distance map — a bilinear tap across a depth discontinuity
                // yields a distance at which nothing is.
                runtime.DepthSamplerHandle = context.GetBindlessRegistry()
                                                 .AcquireSampler({
                                                     .Name = "CaptureSurface Depth Sampler",
                                                     .MagFilter = Filter::Nearest,
                                                     .MinFilter = Filter::Nearest,
                                                     .MipmapMode = MipmapMode::Nearest,
                                                     .AddressModeU = AddressMode::ClampToEdge,
                                                     .AddressModeV = AddressMode::ClampToEdge,
                                                     .AddressModeW = AddressMode::ClampToEdge,
                                                 })
                                                 .Handle;
            }
        }

        // Push this frame's capture source when the refresh policy calls for it. EveryFrame always
        // pushes; OnDemand pushes only while faces are still owed, then idles — SceneCapture records
        // nothing on a frame with no fresh SetView, so a settled OnDemand capture costs nothing.
        // The source excludes the driving entity: a surface is not part of its own environment.
        const bool pushThisFrame =
            Refresh == CaptureRefresh::EveryFrame || runtime.PendingFaces > 0;
        if (pushThisFrame)
        {
            runtime.Capture->SetView({.World = &world,
                                      .Position = position,
                                      .FaceBasis = faceBasis,
                                      .Exclude = entity,
                                      .Alpha = alpha});
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
            runtime.BoundOrientationSlot.clear();
            runtime.BoundDepthTextureSlot.clear();
            runtime.BoundDepthSamplerSlot.clear();

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
            // The octahedral distance map and its point sampler, when the component opted into a
            // distance map (DepthTextureSlot names one) and the material declares the fields. Both
            // ride back to the unbound sentinel on teardown, and CenterSlot's flag gates the sample.
            if (!DepthTextureSlot.empty() &&
                HasField(*target, DepthTextureSlot, MaterialField::FieldKind::TextureHandle))
            {
                target->SetTextureHandle(DepthTextureSlot,
                                         runtime.Capture->GetDistanceOutputHandle());
                runtime.BoundDepthTextureSlot = DepthTextureSlot;
            }
            // The point sampler binds only when the distance path is on (DepthTextureSlot names one):
            // DepthSamplerSlot defaults non-empty, but a capture with no distance map holds no sampler.
            if (!DepthTextureSlot.empty() && !DepthSamplerSlot.empty() &&
                HasField(*target, DepthSamplerSlot, MaterialField::FieldKind::SamplerHandle))
            {
                target->SetSamplerHandle(DepthSamplerSlot, runtime.DepthSamplerHandle);
                runtime.BoundDepthSamplerSlot = DepthSamplerSlot;
            }
            // The centre is where a parallax-correcting fragment starts marching the recorded distance,
            // and w is 1 only once an output slot exists — so a fragment can tell an unpopulated slot
            // from a probe at the origin and reach its fallback instead of indexing the array with it.
            if (!CenterSlot.empty() &&
                HasField(*target, CenterSlot, MaterialField::FieldKind::Param))
            {
                target->SetParam(CenterSlot, vec4(position, output.IsValid() ? 1.0f : 0.0f));
                runtime.BoundCenterSlot = CenterSlot;
            }
            // The frame the faces were oriented in, so a fragment can express a world direction in
            // the map's own frame. It carries no flag of its own — the centre's w already reports
            // whether a capture is bound, and both slots are written by the same drive.
            if (!OrientationSlot.empty() &&
                HasField(*target, OrientationSlot, MaterialField::FieldKind::Param))
            {
                target->SetParam(OrientationSlot, PackCaptureOrientation(faceBasis));
                runtime.BoundOrientationSlot = OrientationSlot;
            }

            // Hold the material resident only when something was actually written onto it.
            if (!runtime.BoundTextureSlot.empty() || !runtime.BoundSamplerSlot.empty() ||
                !runtime.BoundCenterSlot.empty() || !runtime.BoundOrientationSlot.empty() ||
                !runtime.BoundDepthTextureSlot.empty() || !runtime.BoundDepthSamplerSlot.empty())
            {
                runtime.BoundMaterial = material;
            }
        }

        return runtime.Capture.get();
    }
}
