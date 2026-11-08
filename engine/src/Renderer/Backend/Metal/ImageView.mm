#include <Veng/Renderer/ImageView.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>

#include "Natives.h"
#include "TypeMapping.h"

namespace Veng::Renderer
{
    ImageView::Native& ImageView::GetNative() const
    {
        return *m_Native;
    }

    ImageView::ImageView(Context& context, const ImageViewInfo& info)
        : m_Context(context), m_Name(info.Name), m_Format(info.Image->GetFormat()),
          m_ViewType(info.ViewType), m_BaseMipLevel(info.BaseMipLevel), m_MipLevels(info.MipLevels),
          m_BaseArrayLayer(info.BaseArrayLayer), m_ArrayLayers(info.ArrayLayers),
          m_Native(CreateUnique<Native>()), m_Image(info.Image)
    {
        const id<MTLTexture> texture = info.Image->GetNative().Texture;
        if ([texture storageMode] == MTLStorageModeMemoryless)
        {
            // A memoryless texture takes no view; its one use is the whole attachment.
            VE_ASSERT(info.BaseMipLevel == 0 && info.MipLevels == 1 && info.BaseArrayLayer == 0 &&
                          info.ArrayLayers == [texture arrayLength],
                      "ImageView '{}': a view of a memoryless image spans the whole image", m_Name);
            m_Native->Texture = texture;
            return;
        }
        m_Native->Texture = [texture
            newTextureViewWithPixelFormat:[texture pixelFormat]
                              textureType:Metal::ToMetal(info.ViewType)
                                   levels:NSMakeRange(info.BaseMipLevel, info.MipLevels)
                                   slices:NSMakeRange(info.BaseArrayLayer, info.ArrayLayers)];
        VE_ASSERT(m_Native->Texture != nil,
                  "ImageView '{}': Metal refused the view (mips {}+{}, layers {}+{})", m_Name,
                  info.BaseMipLevel, info.MipLevels, info.BaseArrayLayer, info.ArrayLayers);
        [m_Native->Texture setLabel:Metal::ToNSString(m_Name)];
    }

    ImageView::~ImageView()
    {
        m_Context.GetNative().Retire(m_Native->Texture);
        m_Native->Texture = nil;
    }
}
