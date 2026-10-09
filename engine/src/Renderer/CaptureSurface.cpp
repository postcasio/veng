#include <Veng/Renderer/CaptureSurface.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Log.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/SceneCapture.h>
#include <Veng/Renderer/SceneCapturePool.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include "CaptureDrive.h"

#include <utility>

namespace Veng::Renderer
{
    /// @brief A named material slot and the field handle that name last resolved to.
    struct ResolvedSlot
    {
        /// @brief The slot name the handle was resolved from; empty before the first resolve.
        string Name;
        /// @brief The field the name resolved to, invalid when the material declares no such field.
        MaterialFieldHandle Handle;
    };

    /// @brief The capture and its output sampler a CaptureSurface materializes on the first Drive.
    struct CaptureSurfaceRuntime
    {
        /// @brief Clears the material slots the last drive filled, so no released handle stays bound.
        ~CaptureSurfaceRuntime();

        /// @brief The owned capture, self-unregistering from the drive-list on destruction.
        Unique<SceneCapture> Capture;
        /// @brief Where Capture goes when the surface releases it; expired or empty drops it instead.
        std::weak_ptr<SceneCapturePool> Pool;
        /// @brief Bindless slot of the point sampler the material reads the distance map through.
        ///
        /// Declared before SamplerHandle, whose member name shadows the type from that point on.
        SamplerHandle DepthSamplerHandle;
        /// @brief Bindless slot of the shared sampler the material reads the capture output through.
        SamplerHandle SamplerHandle;
        /// @brief Faces still owed before the current refresh settles; 0 leaves an OnDemand capture idle.
        u32 PendingFaces = SceneCapture::FaceCount;
        /// @brief Context frame serial of the last drive; 0 before the first.
        u64 LastDrivenSerial = 0;
        /// @brief Whether the Entity-aligned SceneLighting warning has logged for this surface.
        bool AlignmentWarned = false;

        /// @brief The material the last drive bound onto, held resident so the unbind can reach it.
        AssetHandle<MaterialInstance> BoundMaterial;
        /// @brief The parent the slots below were resolved against; a change re-resolves every slot.
        const Material* ResolvedParent = nullptr;
        /// @brief The texture slot, resolved from TextureSlot.
        ResolvedSlot Texture;
        /// @brief The sampler slot, resolved from SamplerSlot.
        ResolvedSlot Sampler;
        /// @brief The probe-centre slot, resolved from CenterSlot.
        ResolvedSlot Center;
        /// @brief The capture-frame slot, resolved from OrientationSlot.
        ResolvedSlot Orientation;
        /// @brief The distance-map texture slot, resolved from DepthTextureSlot.
        ResolvedSlot DepthTexture;
        /// @brief The distance-map sampler slot, resolved from DepthSamplerSlot.
        ResolvedSlot DepthSampler;

        /// @brief Texture field the last drive filled on BoundMaterial; invalid when it filled none.
        MaterialFieldHandle BoundTexture;
        /// @brief Sampler field the last drive filled on BoundMaterial; invalid when it filled none.
        MaterialFieldHandle BoundSampler;
        /// @brief Centre field the last drive filled on BoundMaterial; invalid when it filled none.
        MaterialFieldHandle BoundCenter;
        /// @brief Orientation field the last drive filled on BoundMaterial; invalid when none.
        MaterialFieldHandle BoundOrientation;
        /// @brief Distance-map texture field the last drive filled; invalid when it filled none.
        MaterialFieldHandle BoundDepthTexture;
        /// @brief Distance-map sampler field the last drive filled; invalid when it filled none.
        MaterialFieldHandle BoundDepthSampler;
    };

    namespace
    {
        /// @brief The rotation an unbound orientation slot carries — world space, in xyzw order.
        constexpr vec4 IdentityOrientation{0.0f, 0.0f, 0.0f, 1.0f};

        /// @brief Resolves a field name to a handle, invalid unless the material declares it at that kind.
        MaterialFieldHandle FieldOfKind(const MaterialInstance& material, std::string_view name,
                                        MaterialField::FieldKind kind)
        {
            const MaterialFieldHandle handle = material.Field(name);
            if (!handle.IsValid() || material.GetFields()[handle.Index].Kind != kind)
            {
                return MaterialFieldHandle{};
            }
            return handle;
        }

        /// @brief Re-resolves a slot when its authored name moved, or when the target material did.
        void ResolveSlot(ResolvedSlot& slot, const MaterialInstance& material,
                         std::string_view name, MaterialField::FieldKind kind, bool reresolve)
        {
            if (!reresolve && slot.Name == name)
            {
                return;
            }
            slot.Name = name;
            slot.Handle = FieldOfKind(material, name, kind);
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
                if (runtime.BoundTexture.IsValid())
                {
                    material->SetTextureHandle(runtime.BoundTexture, TextureHandle{});
                }
                if (runtime.BoundSampler.IsValid())
                {
                    material->SetSamplerHandle(runtime.BoundSampler, SamplerHandle{});
                }
                // The distance map's handle rides back to the same sentinel as the radiance map: the
                // capture releases the slot and the next registration reuses it.
                if (runtime.BoundDepthTexture.IsValid())
                {
                    material->SetTextureHandle(runtime.BoundDepthTexture, TextureHandle{});
                }
                if (runtime.BoundDepthSampler.IsValid())
                {
                    material->SetSamplerHandle(runtime.BoundDepthSampler, SamplerHandle{});
                }
                if (runtime.BoundCenter.IsValid())
                {
                    material->SetParam(runtime.BoundCenter, vec4(0.0f));
                }
                // The frame goes back to the identity rather than to zero: the centre's flag is what
                // gates the sample, so this value is unread once unbound, and a zero quaternion
                // normalizes to a NaN in a consumer that reads it without the gate.
                if (runtime.BoundOrientation.IsValid())
                {
                    material->SetParam(runtime.BoundOrientation, IdentityOrientation);
                }
            }

            runtime.BoundMaterial = {};
            runtime.BoundTexture = {};
            runtime.BoundSampler = {};
            runtime.BoundCenter = {};
            runtime.BoundOrientation = {};
            runtime.BoundDepthTexture = {};
            runtime.BoundDepthSampler = {};
        }

        /// @brief Returns the material a surface on @p entity binds into, installing its per-entity clone.
        ///
        /// The sibling MeshRenderer's first material belongs to the mesh asset and is shared by every
        /// entity drawing that mesh, so binding the capture there would make every sharer sample it.
        /// The first drive clones it into the entity's InstanceMaterials override, which the render
        /// gather honours; later drives find the clone installed and read it const.
        AssetHandle<MaterialInstance> ResolveCaptureMaterial(const AssetManager& assets,
                                                             Scene& world, const Entity entity)
        {
            const auto* renderer = std::as_const(world).TryGet<MeshRenderer>(entity);
            if (renderer == nullptr || !renderer->Mesh.IsLoaded())
            {
                return {};
            }
            if (!renderer->InstanceMaterials.empty())
            {
                return renderer->InstanceMaterials[0];
            }
            const std::span<const AssetHandle<MaterialInstance>> meshMaterials =
                renderer->Mesh.Get()->GetMaterials();
            if (meshMaterials.empty() || !meshMaterials[0].IsLoaded())
            {
                return {};
            }
            // The one mutable access bumps the scene's spatial version once, which the broadphase
            // re-gathers on.
            vector<AssetHandle<MaterialInstance>> overrides(meshMaterials.begin(),
                                                            meshMaterials.end());
            overrides[0] = assets.Adopt<MaterialInstance>(
                meshMaterials[0].Get()->Clone(meshMaterials[0].Get()->GetName() + " (capture)"));
            AssetHandle<MaterialInstance> material = overrides[0];
            world.Get<MeshRenderer>(entity).InstanceMaterials = std::move(overrides);
            return material;
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
            // A probe wants no game screen effect baked into its faces, and dropping them also keeps
            // the probe's six per-frame Executes off the main view's per-viewport MaterialInstance.
            settings.PostProcessEffects = false;
            // A capture samples pre-tonemap HDR, so no post-tonemap AA reaches it; the temporal
            // resolve is dropped as another cost multiplied across the faces.
            settings.AntiAliasing = AntiAliasingMode::None;
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
        // slot still being drawn through. The capture releases the texture slots it took — unless
        // it goes back to the pool it came from, which keeps them for the next owner.
        if (Capture != nullptr)
        {
            if (const Ref<SceneCapturePool> pool = Pool.lock(); pool != nullptr)
            {
                pool->Return(std::move(Capture));
            }
        }
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

    void CaptureSurface::Release() const
    {
        Runtime.reset();
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
        if (!Enabled)
        {
            return false;
        }
        if (Refresh == CaptureRefresh::EveryFrame)
        {
            return true;
        }
        // Before the runtime materializes an OnDemand capture is still owed its first refresh.
        return !Runtime || Runtime->PendingFaces > 0;
    }

    SceneCaptureInfo CaptureSurface::GetCaptureInfo(Context& context, AssetManager& assets) const
    {
        return SceneCaptureInfo{
            .Context = context,
            .Assets = assets,
            .FaceResolution = Resolution,
            .Settings = CaptureSettings(Shadows),
            // The distance map is opt-in: an empty DepthTextureSlot builds none, so the depth
            // atlas, the distance map, and their pipelines and slots do not exist.
            .CaptureDistance = Output == CaptureOutput::Material && !DepthTextureSlot.empty(),
            .DistanceResolution = DepthResolution,
            // Only a lighting capture is convolved, and only the cube is what gets convolved.
            .Cube = Output == CaptureOutput::SceneLighting,
        };
    }

    void CaptureSurface::Materialize(Context& context, Unique<SceneCapture> capture,
                                     std::weak_ptr<SceneCapturePool> pool) const
    {
        VE_ASSERT(capture != nullptr, "CaptureSurface::Materialize: no capture to install");
        if (!Runtime)
        {
            Runtime = CreateUnique<CaptureSurfaceRuntime>();
        }
        CaptureSurfaceRuntime& runtime = *Runtime;
        VE_ASSERT(runtime.Capture == nullptr,
                  "CaptureSurface::Materialize: the surface already holds a capture");
        runtime.Capture = std::move(capture);
        runtime.Pool = std::move(pool);

        // The sampler the output is read through: a clamp sampler over the octahedral map — the
        // same edge-clamp the capture's own resample uses, and the same one every other clamped blit
        // in the engine reads through.
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
        if (Output == CaptureOutput::Material && !DepthTextureSlot.empty())
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

        // A capture its driver skipped for a frame holds the scene as it last saw it, so the drive
        // that finds the gap restarts the refresh rather than resuming mid-refresh.
        const u64 serial = context.GetFrameSerial();
        if (CaptureDriveSkippedFrame(runtime.LastDrivenSerial, serial))
        {
            runtime.PendingFaces = SceneCapture::FaceCount;
        }
        runtime.LastDrivenSerial = serial;

        // Build the capture on first use when no driver installed one ahead of this drive.
        if (!runtime.Capture)
        {
            Materialize(context, SceneCapture::Create(GetCaptureInfo(context, assets)), {});
        }

        // A lighting capture is sampled by world direction, so its faces stay on the world axes.
        const bool lighting = Output == CaptureOutput::SceneLighting;
        const mat3 basis = lighting ? mat3(1.0f) : faceBasis;

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
                                      .FaceBasis = basis,
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
        //
        // The material is shared by every viewport presenting this scene, but each value written here
        // is derived from the capture and its carrier entity, never from a recording view, so the
        // shared block is correct for all of them. This is the view-independent case
        // BindlessRegistry::MaterialArenaBytes describes; a per-view value would need a per-view
        // instance instead.
        MaterialInstance* const target = lighting ? nullptr : material.Get();
        if (target != nullptr)
        {
            // The slot names are authored data and the target material can change under the drive,
            // so each name resolves to a field handle once and is reused until either moves.
            const Material* const parent = target->GetParent().Get();
            const bool reresolve = runtime.ResolvedParent != parent;
            runtime.ResolvedParent = parent;
            ResolveSlot(runtime.Texture, *target, TextureSlot,
                        MaterialField::FieldKind::TextureHandle, reresolve);
            ResolveSlot(runtime.Sampler, *target, SamplerSlot,
                        MaterialField::FieldKind::SamplerHandle, reresolve);
            ResolveSlot(runtime.Center, *target, CenterSlot, MaterialField::FieldKind::Param,
                        reresolve);
            ResolveSlot(runtime.Orientation, *target, OrientationSlot,
                        MaterialField::FieldKind::Param, reresolve);
            ResolveSlot(runtime.DepthTexture, *target, DepthTextureSlot,
                        MaterialField::FieldKind::TextureHandle, reresolve);
            ResolveSlot(runtime.DepthSampler, *target, DepthSamplerSlot,
                        MaterialField::FieldKind::SamplerHandle, reresolve);

            // Re-record which slots this drive filled, so a renamed slot's old binding is not the one
            // the unbind clears.
            runtime.BoundTexture = {};
            runtime.BoundSampler = {};
            runtime.BoundCenter = {};
            runtime.BoundOrientation = {};
            runtime.BoundDepthTexture = {};
            runtime.BoundDepthSampler = {};

            const TextureHandle output = runtime.Capture->GetOutputHandle();
            if (runtime.Texture.Handle.IsValid())
            {
                target->SetTextureHandle(runtime.Texture.Handle, output);
                runtime.BoundTexture = runtime.Texture.Handle;
            }
            if (runtime.Sampler.Handle.IsValid())
            {
                target->SetSamplerHandle(runtime.Sampler.Handle, runtime.SamplerHandle);
                runtime.BoundSampler = runtime.Sampler.Handle;
            }
            // The octahedral distance map and its point sampler, when the component opted into a
            // distance map (DepthTextureSlot names one) and the material declares the fields. Both
            // ride back to the unbound sentinel on teardown, and CenterSlot's flag gates the sample.
            if (runtime.DepthTexture.Handle.IsValid())
            {
                target->SetTextureHandle(runtime.DepthTexture.Handle,
                                         runtime.Capture->GetDistanceOutputHandle());
                runtime.BoundDepthTexture = runtime.DepthTexture.Handle;
            }
            // The point sampler binds only when the distance path is on (DepthTextureSlot names one):
            // DepthSamplerSlot defaults non-empty, but a capture with no distance map holds no sampler.
            if (!DepthTextureSlot.empty() && runtime.DepthSampler.Handle.IsValid())
            {
                target->SetSamplerHandle(runtime.DepthSampler.Handle, runtime.DepthSamplerHandle);
                runtime.BoundDepthSampler = runtime.DepthSampler.Handle;
            }
            // The centre is where a parallax-correcting fragment starts marching the recorded distance,
            // and w is 1 only once an output slot exists — so a fragment can tell an unpopulated slot
            // from a probe at the origin and reach its fallback instead of indexing the array with it.
            if (runtime.Center.Handle.IsValid())
            {
                target->SetParam(runtime.Center.Handle,
                                 vec4(position, output.IsValid() ? 1.0f : 0.0f));
                runtime.BoundCenter = runtime.Center.Handle;
            }
            // The frame the faces were oriented in, so a fragment can express a world direction in
            // the map's own frame. It carries no flag of its own — the centre's w already reports
            // whether a capture is bound, and both slots are written by the same drive.
            if (runtime.Orientation.Handle.IsValid())
            {
                target->SetParam(runtime.Orientation.Handle, PackCaptureOrientation(faceBasis));
                runtime.BoundOrientation = runtime.Orientation.Handle;
            }

            // Hold the material resident only when something was actually written onto it.
            if (runtime.BoundTexture.IsValid() || runtime.BoundSampler.IsValid() ||
                runtime.BoundCenter.IsValid() || runtime.BoundOrientation.IsValid() ||
                runtime.BoundDepthTexture.IsValid() || runtime.BoundDepthSampler.IsValid())
            {
                runtime.BoundMaterial = material;
            }
        }

        return runtime.Capture.get();
    }

    SceneCapture* CaptureSurface::Drive(Context& context, AssetManager& assets, Scene& world,
                                        const Entity entity, const f32 alpha) const
    {
        // The capture renders from the pose the entity is drawn at — the pose the mesh it feeds is
        // drawn at — so the probe, its published centre and everything rigidly attached to its
        // carrier agree at every alpha instead of sitting a fraction of a tick apart.
        const mat4 drawTransform = world.GetInterpolatedWorldTransform(entity, alpha);
        const vec3 position = vec3(drawTransform[3]);

        if (Output == CaptureOutput::SceneLighting)
        {
            if (!Runtime)
            {
                Runtime = CreateUnique<CaptureSurfaceRuntime>();
            }
            if (Alignment == CaptureAlignment::Entity && !Runtime->AlignmentWarned)
            {
                Log::Warn(
                    "CaptureSurface: a SceneLighting capture renders world-aligned; its Entity "
                    "Alignment is ignored.");
                Runtime->AlignmentWarned = true;
            }
            // A lighting capture binds no material, so the sibling's material is never cloned.
            return Drive(context, assets, world, entity, position, alpha, mat3(1.0f), {});
        }

        // An Entity-aligned capture orients its faces in the carrier's frame: the draw rotation with
        // any scale divided out.
        mat3 faceBasis(1.0f);
        if (Alignment == CaptureAlignment::Entity)
        {
            faceBasis = mat3(drawTransform);
            faceBasis[0] = glm::normalize(faceBasis[0]);
            faceBasis[1] = glm::normalize(faceBasis[1]);
            faceBasis[2] = glm::normalize(faceBasis[2]);
        }

        return Drive(context, assets, world, entity, position, alpha, faceBasis,
                     ResolveCaptureMaterial(assets, world, entity));
    }
}
