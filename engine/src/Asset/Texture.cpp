#include <Veng/Asset/Texture.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng
{
    using namespace Renderer;

    Texture::Texture(Context& context, const TextureInfo& info)
        : m_Context(context), m_Name(info.Name), m_Extent(info.Extent), m_Format(info.Format)
    {
        m_Image = Image::Create(context, {
                                             .Name = m_Name,
                                             .Extent = {info.Extent.x, info.Extent.y, 1},
                                             .Format = info.Format,
                                             .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                         });

        m_View = ImageView::Create(context, {
                                                .Name = m_Name + " View",
                                                .Image = m_Image,
                                            });

        SamplerInfo samplerInfo = info.Sampler;
        samplerInfo.Name = m_Name + " Sampler";
        m_Sampler = Sampler::Create(context, samplerInfo);
    }

    Ref<Texture> Texture::BuildSync(Context& context, const TextureInfo& info)
    {
        Ref<Texture> texture(new Texture(context, info));
        texture->m_Image->UploadSync(info.Pixels);
        return texture;
    }

    Ref<Texture> Texture::CreateAsync(Context& context, const TextureInfo& info, TaskSystem& tasks,
                                      Task<void>& outUpload)
    {
        Ref<Texture> texture(new Texture(context, info));
        outUpload = texture->m_Image->Upload(tasks, info.Pixels);
        return texture;
    }

    Task<Ref<Texture>> Texture::Build(Context& context, TaskSystem& tasks, TextureInfo info)
    {
        // The caller's TextureInfo::Pixels is a non-owning span; copy the source bytes into the
        // worker job so they outlive the caller's frame.
        vector<u8> pixels(info.Pixels.begin(), info.Pixels.end());

        return tasks.Submit(
            [&context, &tasks, info = std::move(info), pixels = std::move(pixels)]
            {
                TextureInfo workerInfo = info;
                workerInfo.Pixels = pixels;

                Ref<Texture> texture(new Texture(context, workerInfo));

                // Record the transfer-queue copy and block on its submit; the staging buffer
                // retires on the transfer timeline, so the frame that first samples this view
                // folds in the timeline wait.
                Task<void> upload = texture->m_Image->Upload(tasks, workerInfo.Pixels);
                (void)upload.Get();

                texture->Finalize();
                return texture;
            });
    }

    Texture::~Texture()
    {
        if (!m_Registered)
        {
            return;
        }

        auto& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_TextureHandle);
        bindless.Release(m_SamplerHandle);
    }

    void Texture::Finalize()
    {
        VE_ASSERT(!m_Registered, "Texture::Finalize: '{}' already registered", m_Name);

        auto& bindless = m_Context.GetBindlessRegistry();
        m_TextureHandle = bindless.Register(m_View);
        m_SamplerHandle = bindless.Register(m_Sampler);
        m_Registered = true;

        // The view is sampled bindlessly through set 0, so the RenderGraph never
        // sees it and can't transition it. The context acquires it onto the
        // graphics queue at the next frame, before any pass samples it.
        m_Context.EnqueueBindlessAcquire(m_View);
    }
}
