#include <Veng/Asset/Texture.h>

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>

namespace Veng
{
    using namespace Renderer;

    Texture::Texture(Context& context, const TextureInfo& info) :
        m_Context(context),
        m_Name(info.Name),
        m_Extent(info.Extent),
        m_Format(info.Format)
    {
        m_Image = Image::Create(context, {
            .Name = m_Name,
            .Extent = {info.Extent.x, info.Extent.y, 1},
            .Format = info.Format,
            .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
        });

        m_Image->Upload(info.Pixels);

        m_View = ImageView::Create(context, {
            .Name = m_Name + " View",
            .Image = m_Image,
        });

        SamplerInfo samplerInfo = info.Sampler;
        samplerInfo.Name = m_Name + " Sampler";
        m_Sampler = Sampler::Create(context, samplerInfo);

        auto& bindless = context.GetBindlessRegistry();
        m_TextureHandle = bindless.Register(m_View);
        m_SamplerHandle = bindless.Register(m_Sampler);
    }

    Texture::~Texture()
    {
        auto& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_TextureHandle);
        bindless.Release(m_SamplerHandle);
    }
}
