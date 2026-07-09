#include <Veng/Gui/RenderTarget.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>

namespace Veng::Gui
{
    Unique<RenderTarget> RenderTarget::Create(const RenderTargetInfo& info)
    {
        return Unique<RenderTarget>(new RenderTarget(info));
    }

    RenderTarget::RenderTarget(const RenderTargetInfo& info)
        : m_Context(info.Context), m_Extent(info.Extent), m_Name(info.Name)
    {
        VE_ASSERT(info.Extent.x > 0 && info.Extent.y > 0,
                  "Gui::RenderTarget extent must be positive (got {}x{})", info.Extent.x,
                  info.Extent.y);
        Allocate();
    }

    RenderTarget::~RenderTarget()
    {
        m_Context.GetBindlessRegistry().Release(m_Handle);
    }

    void RenderTarget::Allocate()
    {
        Renderer::BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_Handle);

        m_Image =
            Renderer::Image::Create(m_Context, {
                                                   .Name = m_Name + " Image",
                                                   .Extent = {m_Extent.x, m_Extent.y, 1},
                                                   .Format = ColorFormat,
                                                   .Usage = Renderer::ImageUsage::ColorAttachment |
                                                            Renderer::ImageUsage::Sampled |
                                                            Renderer::ImageUsage::TransferSrc,
                                               });
        m_View =
            Renderer::ImageView::Create(m_Context, {.Name = m_Name + " View", .Image = m_Image});
        m_Handle = bindless.Register(m_View);
    }

    void RenderTarget::Resize(uvec2 extent)
    {
        if (extent == m_Extent || extent.x == 0 || extent.y == 0)
        {
            return;
        }
        m_Extent = extent;
        Allocate();
    }

    const Ref<Renderer::ImageView>& RenderTarget::GetOutput() const
    {
        return m_View;
    }

    Renderer::TextureHandle RenderTarget::GetOutputHandle() const
    {
        return m_Handle;
    }

    uvec2 RenderTarget::GetExtent() const
    {
        return m_Extent;
    }

    Renderer::Format RenderTarget::GetFormat() const
    {
        return ColorFormat;
    }
}
