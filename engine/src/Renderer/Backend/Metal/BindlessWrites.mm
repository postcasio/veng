// The Metal half of the bindless registry: each of the three bindless sets is one argument buffer,
// and a slot write stores the resource's ID or address into its array element in place. The
// frame-slot queries read the context's frame fences.

#include <Veng/Renderer/BindlessRegistry.h>

#include <cstring>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>

#include "../../BindlessBackend.h"
#include "Natives.h"

namespace Veng::Renderer
{
    namespace
    {
        u64 ResourceIdOf(const MTLResourceID resource)
        {
            u64 value = 0;
            std::memcpy(&value, &resource, sizeof(value));
            return value;
        }

        void WriteSlot(const DescriptorSet& set, const DescriptorSetLayout& layout,
                       const u32 binding, const u32 index, const u64 value)
        {
            const Metal::BindingPlacement* const entry = layout.GetNative().Placement.Find(binding);
            VE_ASSERT(entry != nullptr && index < entry->Count,
                      "BindlessRegistry: slot {} of binding {} lies outside its array", index,
                      binding);
            set.GetNative().WriteElement(entry->FirstElement + index, value);
        }
    }

    u32 BindlessBackend::GetReleaseSlot(const Context& context)
    {
        return context.GetNative().GetReleaseSlot();
    }

    bool BindlessBackend::IsSlotWritable(const Context& context, const u32 slot)
    {
        return context.GetNative().IsSlotWritable(slot);
    }

    void BindlessRegistry::WriteTexture(u32 index, const Ref<ImageView>& view) const
    {
        WriteSlot(*m_Set, *m_Layout, TextureBinding, index,
                  ResourceIdOf([view->GetNative().Texture gpuResourceID]));
    }

    void BindlessRegistry::WriteNormalVariance(u32 index, const Ref<ImageView>& view) const
    {
        WriteSlot(*m_Set, *m_Layout, NormalVarianceBinding, index,
                  ResourceIdOf([view->GetNative().Texture gpuResourceID]));
    }

    void BindlessRegistry::WriteVolume(u32 index, const Ref<ImageView>& view) const
    {
        WriteSlot(*m_VolumeSet, *m_VolumeLayout, TypedSetBinding, index,
                  ResourceIdOf([view->GetNative().Texture gpuResourceID]));
    }

    void BindlessRegistry::WriteCube(u32 index, const Ref<ImageView>& view) const
    {
        WriteSlot(*m_CubeSet, *m_CubeLayout, TypedSetBinding, index,
                  ResourceIdOf([view->GetNative().Texture gpuResourceID]));
    }

    void BindlessRegistry::WriteSampler(u32 index, const Ref<Sampler>& sampler) const
    {
        WriteSlot(*m_Set, *m_Layout, SamplerBinding, index,
                  ResourceIdOf([sampler->GetNative().Sampler gpuResourceID]));
    }

    void BindlessRegistry::WriteStorageImage(u32 index, const Ref<ImageView>& view) const
    {
        WriteSlot(*m_Set, *m_Layout, StorageImageBinding, index,
                  ResourceIdOf([view->GetNative().Texture gpuResourceID]));
    }

    void BindlessRegistry::WriteStorageBuffer(u32 index, const Ref<Buffer>& buffer) const
    {
        // The whole buffer, addressed by its handle's slot and read byte-addressed inside.
        WriteSlot(*m_Set, *m_Layout, StorageBufferBinding, index,
                  [buffer->GetNative().Buffer gpuAddress]);
    }
}
