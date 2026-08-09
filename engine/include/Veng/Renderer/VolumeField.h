#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/FormatInfo.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class TaskSystem;

    template <typename T>
    class Task;
}

namespace Veng::Renderer
{
    class Context;
    class Image;
    class ImageView;

    /// @brief The default sampler for a volume field: linear filtering, clamp-to-edge on every axis.
    ///
    /// A medium is a bounded density function over a box, so its texture must not wrap — a sample
    /// past the volume's edge reads the edge texel (zero density at a padded border), never the
    /// far face. Anisotropy is off (a 3D volume has no meaningful anisotropic minification axis).
    /// @return The sampler parameters a VolumeField uses unless the consumer overrides them.
    [[nodiscard]] inline SamplerInfo DefaultVolumeSampler()
    {
        return SamplerInfo{
            .MagFilter = Filter::Linear,
            .MinFilter = Filter::Linear,
            .MipmapMode = MipmapMode::Linear,
            .AddressModeU = AddressMode::ClampToEdge,
            .AddressModeV = AddressMode::ClampToEdge,
            .AddressModeW = AddressMode::ClampToEdge,
            .AnisotropyEnabled = false,
        };
    }

    /// @brief The CPU build input for a VolumeField: a packed voxel volume plus its world bounds.
    ///
    /// Voxels is a tightly packed, x-major, single-mip, full-extent volume in Format — one texel
    /// per (x, y, z) with no row or slice padding, so its byte size is exactly ExpectedByteSize().
    /// The documented default Format is RGBA16F (RGBA16Sfloat): emission radiance density in RGB
    /// and extinction density in A, both per world-unit. Bounds is the world-space box the volume
    /// texture maps onto; the owning entity's Transform is not applied to it.
    struct VolumeFieldData
    {
        /// @brief Debug name for the field's GPU resources.
        string Name = "VolumeField";
        /// @brief Volume texture dimensions in voxels (x, y, z).
        uvec3 Resolution = {1, 1, 1};
        /// @brief Texel format of the voxel data (RGBA16Sfloat — emission RGB + extinction A — by default).
        Format Format = Format::RGBA16Sfloat;
        /// @brief The voxels, tightly packed x-major over the full extent, single mip, in Format.
        std::span<const u8> Voxels;
        /// @brief World-space bounds the volume texture maps onto (the entity Transform is not applied).
        AABB Bounds = AABB::Empty();
        /// @brief Sampler parameters (linear + clamp-to-edge by default; a medium must not wrap).
        SamplerInfo Sampler = DefaultVolumeSampler();

        /// @brief Returns the byte size a tightly-packed full-extent volume in Format must have.
        ///
        /// Resolution.x * Resolution.y * Resolution.z * bytes-per-texel, sized through FormatInfo
        /// so the depth axis is carried honestly. Returns 0 for a zero-extent Resolution or a
        /// Format whose per-texel size FormatInfo does not know (an unsupported format).
        /// @return The expected tightly-packed byte size, or 0 if the extent or format is invalid.
        [[nodiscard]] usize ExpectedByteSize() const
        {
            return BytesForLevel(Format, Resolution.x, Resolution.y) *
                   static_cast<usize>(Resolution.z);
        }

        /// @brief Returns true when Voxels exactly fills the full extent for Resolution and Format.
        ///
        /// A zero extent, an unsupported Format, or a Voxels span that disagrees with
        /// Resolution x FormatInfo per-texel size all read as invalid — the build rejects such
        /// data loudly rather than copying a mis-sized staging buffer.
        /// @return True if the voxel span honestly carries the full volume.
        [[nodiscard]] bool IsValid() const
        {
            const usize expected = ExpectedByteSize();
            return expected > 0 && Voxels.size() == expected;
        }
    };

    /// @brief A bounded emissive volumetric medium held as a 3D texture with world-space bounds.
    ///
    /// The GPU resource for participating media: a Type3D emission+extinction texture (RGB emission
    /// radiance density, A extinction density, per world-unit), its view and sampler, and the
    /// world-space AABB the texture maps onto. Built once from CPU voxel data through the async
    /// Build factory (or the blocking BuildSync) and sampled many frames by a ray-march pass.
    ///
    /// The dedicated per-pass march (VolumeScenePass) binds the view + sampler through its own
    /// descriptor set and needs no registration. For a **material** to sample the volume, call
    /// Finalize() once on the render thread: it registers the view into the typed bindless volume
    /// set (its own homogeneous set, never set 0, so no non-2D descriptor enters set 0's Metal
    /// argument buffer on MoltenVK), takes a shared sampler, and enqueues the first-use bindless
    /// acquire — the Texture::Finalize model, which is what makes an async-Build volume safe to
    /// sample bindlessly. Ref-counted (a pass and a component both hold one); the factories are the
    /// only construction path.
    class VolumeField
    {
    public:
        /// @brief Builds a volume field from CPU voxel data on a worker thread.
        ///
        /// Creates the Type3D image, view, and sampler and uploads the voxels through the async
        /// transfer-queue path — all worker-legal, with no main-thread finalize step, so the
        /// resolved Ref is immediately complete. The voxel bytes are copied into the job, so the
        /// caller's Voxels span need not outlive the call.
        /// @param context The render context the GPU resources are created on.
        /// @param tasks   The task system the build job and its upload run on.
        /// @param data    The voxel volume, bounds, format, and sampler.
        /// @return A task resolving to the built field once its upload has been submitted.
        /// @pre data.IsValid() (asserted).
        [[nodiscard]] static Task<Ref<VolumeField>> Build(Context& context, TaskSystem& tasks,
                                                          VolumeFieldData data);

        /// @brief Builds a volume field from CPU voxel data, blocking until the upload completes.
        ///
        /// The blocking sibling of Build: creates the Type3D image, view, and sampler and uploads
        /// the voxels through the immediate UploadSync path. For the render thread, tests, and
        /// worker-hostile call sites; prefer Build() off the render thread.
        /// @param context The render context the GPU resources are created on.
        /// @param data    The voxel volume, bounds, format, and sampler.
        /// @return The built field, ready to sample.
        /// @pre data.IsValid() (asserted).
        [[nodiscard]] static Ref<VolumeField> BuildSync(Context& context,
                                                        const VolumeFieldData& data);

        /// @brief Destroys the field's GPU resources (deferred until the GPU is done with them).
        ~VolumeField();

        VolumeField(const VolumeField&) = delete;
        VolumeField& operator=(const VolumeField&) = delete;

        /// @brief Registers the volume into the typed bindless volume set for material sampling.
        ///
        /// The VolumeField counterpart of Texture::Finalize: registers the 3D view into the
        /// bindless volume set (yielding a VolumeHandle), acquires a shared sampler for the field's
        /// sampling description (yielding a SamplerHandle), and enqueues a bindless acquire so the
        /// per-frame drain transitions the view to ShaderReadOnly on the graphics queue and folds
        /// in the transfer-timeline wait before any pass samples it — the step that makes an
        /// async-Build volume safe for a bindless-sampled material. Idempotent-guarded (asserts if
        /// already registered). Render-thread only (the bindless cache and acquire queue are the
        /// render thread's). A field sampled only through the dedicated march pass needs no
        /// Finalize.
        void Finalize();

        /// @brief Returns the bindless volume handle, valid only after Finalize().
        [[nodiscard]] VolumeHandle GetHandle() const { return m_Handle; }

        /// @brief Returns the bindless shared-sampler handle, valid only after Finalize().
        [[nodiscard]] SamplerHandle GetSamplerHandle() const { return m_SamplerHandle; }

        /// @brief Returns the field's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the world-space box the volume texture maps onto.
        [[nodiscard]] const AABB& GetBounds() const { return m_Bounds; }

        /// @brief Returns the volume texture dimensions in voxels (x, y, z).
        [[nodiscard]] uvec3 GetResolution() const { return m_Resolution; }

        /// @brief Returns the texel format of the volume texture.
        [[nodiscard]] Format GetFormat() const { return m_Format; }

        /// @brief Returns the underlying Type3D image.
        [[nodiscard]] const Ref<Image>& GetImage() const { return m_Image; }

        /// @brief Returns the 3D image view a sampling pass binds.
        [[nodiscard]] const Ref<ImageView>& GetImageView() const { return m_View; }

        /// @brief Returns the sampler a sampling pass binds.
        [[nodiscard]] const Ref<Sampler>& GetSampler() const { return m_Sampler; }

    private:
        VolumeField() = default;

        /// @brief Creates the image/view/sampler for a field and records its metadata, no upload.
        ///
        /// Worker-legal: it only allocates GPU objects (VMA + view/sampler create), so both the
        /// async Build job and the blocking BuildSync share it and differ only in the upload path.
        /// @param context The render context the GPU resources are created on.
        /// @param data    The voxel volume description (validated by the caller).
        /// @return The field with its resources created but no voxels uploaded yet.
        [[nodiscard]] static Ref<VolumeField> CreateResources(Context& context,
                                                              const VolumeFieldData& data);

        /// @brief The owning render context; used for bindless registration and its deferred release.
        Context* m_Context = nullptr;
        /// @brief Debug name.
        string m_Name;
        /// @brief Volume texture dimensions in voxels.
        uvec3 m_Resolution = {1, 1, 1};
        /// @brief Texel format of the volume texture.
        Format m_Format = Format::RGBA16Sfloat;
        /// @brief World-space box the volume texture maps onto.
        AABB m_Bounds = AABB::Empty();
        /// @brief The Type3D emission+extinction texture.
        Ref<Image> m_Image;
        /// @brief The 3D view a sampling pass binds.
        Ref<ImageView> m_View;
        /// @brief The sampler a sampling pass binds.
        Ref<Sampler> m_Sampler;
        /// @brief The sampling description, kept so Finalize can acquire a shared bindless sampler.
        SamplerInfo m_SamplerInfo;
        /// @brief The bindless volume handle; valid only after Finalize().
        VolumeHandle m_Handle;
        /// @brief The bindless shared-sampler handle; valid only after Finalize().
        SamplerHandle m_SamplerHandle;
        /// @brief True once Finalize has registered the view into the bindless volume set.
        bool m_Registered = false;
    };
}
