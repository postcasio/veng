#include <Veng/Asset/Environment.h>

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

    Environment::Environment(Context& context, const EnvironmentData& info)
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

        // Linear filtering with horizontal wrap matches equirectangular sampling — the
        // direction-to-UV mapping in the IBL-generation compute wraps in U at the seam.
        m_Sampler = Sampler::Create(context, {
                                                 .Name = m_Name + " Sampler",
                                                 .MagFilter = Filter::Linear,
                                                 .MinFilter = Filter::Linear,
                                                 .MipmapMode = MipmapMode::Linear,
                                                 .AddressModeU = AddressMode::Repeat,
                                                 .AddressModeV = AddressMode::ClampToEdge,
                                                 .AddressModeW = AddressMode::ClampToEdge,
                                             });
    }

    Ref<Environment> Environment::PrepareSync(Context& context, const EnvironmentData& data)
    {
        Ref<Environment> environment(new Environment(context, data));
        environment->m_Image->UploadSync(data.Pixels);
        return environment;
    }

    Ref<Environment> Environment::PrepareAsync(Context& context, const EnvironmentData& data,
                                               TaskSystem& tasks, Task<void>& outUpload)
    {
        Ref<Environment> environment(new Environment(context, data));
        outUpload = environment->m_Image->Upload(tasks, data.Pixels);
        return environment;
    }

    Environment::~Environment()
    {
        if (!m_Registered)
        {
            return;
        }

        auto& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_TextureHandle);
        bindless.Release(m_SamplerHandle);
    }

    void Environment::Finalize()
    {
        VE_ASSERT(!m_Registered, "Environment::Finalize: '{}' already registered", m_Name);

        auto& bindless = m_Context.GetBindlessRegistry();
        m_TextureHandle = bindless.Register(m_View);
        m_SamplerHandle = bindless.Register(m_Sampler);
        m_Registered = true;

        // The panorama is sampled bindlessly through set 0, so the RenderGraph never sees it
        // and can't transition it. The context acquires it onto the graphics queue at the next
        // frame, before the IBL-generation compute samples it.
        m_Context.EnqueueBindlessAcquire(m_View);
    }
}
