#include <Veng/Asset/Texture.h>

#include <algorithm>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetBuild.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng
{
    using namespace Renderer;

    namespace
    {
        // Builds one tightly-packed copy region per mip level of a 2D RGBA8 image: each level's
        // pixels begin where the previous level ended, with dimensions halved (floored at 1).
        vector<BufferImageCopyRegion> BuildMipCopyRegions(uvec2 extent, u32 mipLevels)
        {
            vector<BufferImageCopyRegion> regions;
            regions.reserve(mipLevels);

            u64 offset = 0;
            for (u32 level = 0; level < mipLevels; level++)
            {
                const u32 levelWidth = std::max(1u, extent.x >> level);
                const u32 levelHeight = std::max(1u, extent.y >> level);

                regions.emplace_back(BufferImageCopyRegion{
                    .BufferOffset = offset,
                    .MipLevel = level,
                    .Extent = {levelWidth, levelHeight, 1},
                });

                offset += static_cast<u64>(levelWidth) * levelHeight * 4;
            }

            return regions;
        }
    }

    Texture::Texture(Context& context, const TextureData& info)
        : m_Context(context), m_Name(info.Name), m_Extent(info.Extent), m_Format(info.Format)
    {
        const u32 mipLevels = std::max(1u, info.MipLevels);

        m_Image = Image::Create(context, {
                                             .Name = m_Name,
                                             .Extent = {info.Extent.x, info.Extent.y, 1},
                                             .MipLevels = mipLevels,
                                             .Format = info.Format,
                                             .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                         });

        m_View = ImageView::Create(context, {
                                                .Name = m_Name + " View",
                                                .Image = m_Image,
                                                .MipLevels = mipLevels,
                                            });

        SamplerInfo samplerInfo = info.Sampler;
        samplerInfo.Name = m_Name + " Sampler";
        m_Sampler = Sampler::Create(context, samplerInfo);
    }

    Ref<Texture> Texture::PrepareSync(Context& context, const TextureData& data)
    {
        Ref<Texture> texture(new Texture(context, data));

        // A precooked chain (MipLevels > 1) uploads one copy region per level with no GPU mipgen;
        // a single-mip texture takes the plain copy path (which a runtime-built texture may extend
        // with GenerateMipmaps when its image was allocated with more levels).
        if (data.MipLevels > 1)
        {
            const vector<BufferImageCopyRegion> regions =
                BuildMipCopyRegions(data.Extent, data.MipLevels);
            texture->m_Image->UploadSync(data.Pixels, regions);
        }
        else
        {
            texture->m_Image->UploadSync(data.Pixels);
        }

        return texture;
    }

    Ref<Texture> Texture::PrepareAsync(Context& context, const TextureData& data, TaskSystem& tasks,
                                       Task<void>& outUpload)
    {
        Ref<Texture> texture(new Texture(context, data));

        if (data.MipLevels > 1)
        {
            const vector<BufferImageCopyRegion> regions =
                BuildMipCopyRegions(data.Extent, data.MipLevels);
            outUpload = texture->m_Image->Upload(tasks, data.Pixels, regions);
        }
        else
        {
            outUpload = texture->m_Image->Upload(tasks, data.Pixels);
        }

        return texture;
    }

    Task<Detail::BuiltAsset<Texture>> Detail::SubmitAssetBuild(Context& context, TaskSystem& tasks,
                                                               TextureData data)
    {
        // The caller's TextureData::Pixels is a non-owning span; copy the source bytes into the
        // worker job so they outlive the caller's frame.
        vector<u8> pixels(data.Pixels.begin(), data.Pixels.end());

        return tasks.Submit(
            [&context, &tasks, data = std::move(data), pixels = std::move(pixels)]() mutable
            {
                data.Pixels = pixels;

                Task<void> upload;
                const Ref<Texture> texture = Texture::PrepareAsync(context, data, tasks, upload);

                // Block on the transfer-queue submit here on the worker; the staging buffer retires
                // on the transfer timeline, so the frame that first samples this view folds in the
                // timeline wait. Finalize (bindless registration) is deferred to the main thread.
                (void)upload.Get();

                return Detail::BuiltAsset<Texture>{
                    .Resource = texture,
                    .Finalize = [texture]() -> VoidResult
                    {
                        texture->Finalize();
                        return {};
                    },
                };
            });
    }

    Ref<Texture> Detail::BuildAssetSync(Context& context, const TextureData& data)
    {
        const Ref<Texture> texture = Texture::PrepareSync(context, data);
        texture->Finalize();
        return texture;
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
