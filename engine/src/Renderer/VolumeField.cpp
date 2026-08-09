#include <Veng/Renderer/VolumeField.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng::Renderer
{
    Ref<VolumeField> VolumeField::CreateResources(Context& context, const VolumeFieldData& data)
    {
        auto field = Ref<VolumeField>(new VolumeField());
        field->m_Context = &context;
        field->m_Name = data.Name;
        field->m_Resolution = data.Resolution;
        field->m_Format = data.Format;
        field->m_Bounds = data.Bounds;

        // Sampled is the render use, TransferDst the voxel-upload target; TransferSrc lets the
        // built volume be read back to the host for inspection or verification (free on an
        // optimally-tiled image).
        field->m_Image =
            Image::Create(context, {
                                       .Name = data.Name,
                                       .Extent = data.Resolution,
                                       .Format = data.Format,
                                       .Type = ImageType::Type3D,
                                       .Usage = ImageUsage::Sampled | ImageUsage::TransferDst |
                                                ImageUsage::TransferSrc,
                                   });

        field->m_View = ImageView::Create(context, {
                                                       .Name = data.Name + " View",
                                                       .Image = field->m_Image,
                                                       .ViewType = ImageViewType::Type3D,
                                                   });

        SamplerInfo samplerInfo = data.Sampler;
        samplerInfo.Name = data.Name + " Sampler";
        field->m_SamplerInfo = samplerInfo;
        field->m_Sampler = Sampler::Create(context, samplerInfo);

        return field;
    }

    Task<Ref<VolumeField>> VolumeField::Build(Context& context, TaskSystem& tasks,
                                              VolumeFieldData data)
    {
        VE_ASSERT(data.IsValid(),
                  "VolumeField::Build: '{}' voxel span is {} bytes but Resolution {}x{}x{} in the "
                  "given format needs {} — the volume must tightly pack the full extent",
                  data.Name, data.Voxels.size(), data.Resolution.x, data.Resolution.y,
                  data.Resolution.z, data.ExpectedByteSize());

        // The caller's Voxels span is non-owning; copy the bytes into the job so they outlive the
        // caller's frame (the async Image::Upload makes its own copy, but data.Voxels must be valid
        // when it is called on the worker).
        vector<u8> voxels(data.Voxels.begin(), data.Voxels.end());

        return tasks.Submit(
            [&context, &tasks, data = std::move(data),
             voxels = std::move(voxels)]() mutable -> Ref<VolumeField>
            {
                data.Voxels = voxels;

                const Ref<VolumeField> field = CreateResources(context, data);

                // Block on the transfer-queue submit here on the worker; the staging buffer retires
                // on the transfer timeline, so the frame that first samples this view folds in the
                // timeline wait. There is no bindless registration to defer to the main thread.
                Task<void> upload = field->m_Image->Upload(tasks, data.Voxels);
                (void)upload.Get();

                return field;
            });
    }

    Ref<VolumeField> VolumeField::BuildSync(Context& context, const VolumeFieldData& data)
    {
        VE_ASSERT(data.IsValid(),
                  "VolumeField::BuildSync: '{}' voxel span is {} bytes but Resolution {}x{}x{} in "
                  "the given format needs {} — the volume must tightly pack the full extent",
                  data.Name, data.Voxels.size(), data.Resolution.x, data.Resolution.y,
                  data.Resolution.z, data.ExpectedByteSize());

        const Ref<VolumeField> field = CreateResources(context, data);
        field->m_Image->UploadSync(data.Voxels);
        return field;
    }

    void VolumeField::Finalize()
    {
        VE_ASSERT(!m_Registered, "VolumeField::Finalize: '{}' already registered", m_Name);
        VE_ASSERT(m_Context != nullptr, "VolumeField::Finalize: '{}' has no context", m_Name);

        BindlessRegistry& bindless = m_Context->GetBindlessRegistry();
        m_Handle = bindless.RegisterVolume(m_View);

        // Take a shared set-0 sampler for the field's description: a bindless volume samples
        // through g_Samplers, so the material needs a SamplerHandle even though the dedicated
        // march pass binds its own m_Sampler.
        const SharedSampler shared = bindless.AcquireSampler(m_SamplerInfo);
        m_SamplerHandle = shared.Handle;
        m_Registered = true;

        // The 3D view is sampled bindlessly, so the RenderGraph never sees it and cannot
        // transition it. The context acquires it onto the graphics queue at the next frame —
        // folding in the async upload's transfer-timeline wait — before any pass samples it.
        m_Context->EnqueueBindlessAcquire(m_View);
    }

    VolumeField::~VolumeField()
    {
        if (m_Registered)
        {
            // The volume slot is this field's own and comes back; the sampler slot is shared with
            // every caller asking for the same settings and stays with the registry.
            m_Context->GetBindlessRegistry().Release(m_Handle);
        }
    }
}
