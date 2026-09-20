#include <Veng/Asset/MaterialInstance.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetBuild.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng
{
    using namespace Renderer;

    MaterialInstance::MaterialInstance(const MaterialInstanceInfo& info)
        : m_Context(*info.Context), m_Name(info.Name), m_Parent(info.Parent),
          m_Overrides(info.Overrides)
    {
        // The parent handle may still be pending here: construction only stores it. The block is
        // seeded from the parent's default block in Finalize(), where the parent is resident — its
        // block is only patched (handle slots resolved) once the parent itself is finalized, which
        // the dependency ordering guarantees runs before this instance's Finalize.

        // Override textures are kept resident on the instance.
        for (const MaterialOverride& ov : m_Overrides)
        {
            if (ov.Texture.Id().IsValid() || ov.Texture.Get() != nullptr)
            {
                m_Textures.push_back(ov.Texture);
            }
        }
    }

    Task<Detail::BuiltAsset<MaterialInstance>>
    Detail::SubmitAssetBuild(Renderer::Context&, TaskSystem& tasks, MaterialInstanceInfo info)
    {
        return tasks.Submit(
            [info = std::move(info)]() mutable
            {
                const Ref<MaterialInstance> instance = MaterialInstance::Prepare(info);

                // The bindless RegisterMaterial + override patch is render-thread-only, so it is
                // deferred to the main-thread continuation.
                return Detail::BuiltAsset<MaterialInstance>{
                    .Resource = instance,
                    .Finalize = [instance]() mutable -> VoidResult
                    {
                        instance->Finalize();
                        return {};
                    },
                };
            });
    }

    Ref<MaterialInstance> Detail::BuildAssetSync(Renderer::Context&,
                                                 const MaterialInstanceInfo& data)
    {
        const Ref<MaterialInstance> instance = MaterialInstance::Prepare(data);
        instance->Finalize();
        return instance;
    }

    MaterialInstance::~MaterialInstance()
    {
        if (m_Registered)
        {
            m_Context.GetBindlessRegistry().Release(m_Handle);
        }
    }

    const MaterialField& MaterialInstance::ResolveField(const MaterialFieldHandle field,
                                                        const std::string_view caller) const
    {
        const Material* const parent = m_Parent.Get();
        VE_ASSERT(field.IsValid(), "MaterialInstance::{}: invalid field handle on instance '{}'",
                  caller, m_Name);
        VE_ASSERT(field.Parent == parent,
                  "MaterialInstance::{}: field handle was resolved against another material, not "
                  "instance '{}'s parent",
                  caller, m_Name);
        const std::span<const MaterialField> fields = parent->GetFields();
        VE_ASSERT(field.Index < fields.size(),
                  "MaterialInstance::{}: field index {} is past instance '{}'s schema of {} fields",
                  caller, field.Index, m_Name, fields.size());
        return fields[field.Index];
    }

    MaterialFieldHandle MaterialInstance::RequireField(const std::string_view name,
                                                       const std::string_view caller) const
    {
        const MaterialFieldHandle field = Field(name);
        VE_ASSERT(field.IsValid(), "MaterialInstance::{}: field '{}' not found in instance '{}'",
                  caller, name, m_Name);
        return field;
    }

    void MaterialInstance::Finalize()
    {
        VE_ASSERT(!m_Registered, "MaterialInstance::Finalize: '{}' already registered", m_Name);
        VE_ASSERT(m_Parent.Get() != nullptr,
                  "MaterialInstance::Finalize: '{}' parent material is not resident", m_Name);

        // Seed the block from the parent's finalized default block (its handle slots are patched).
        const std::span<const std::byte> defaultBlock = m_Parent.Get()->GetDefaultBlock();
        m_Block.assign(defaultBlock.begin(), defaultBlock.end());

        // Apply each override over the seeded default block.
        const std::span<const MaterialField> parentFields = m_Parent.Get()->GetFields();
        for (const MaterialOverride& ov : m_Overrides)
        {
            const MaterialFieldHandle handle = Field(ov.Name);
            VE_ASSERT(handle.IsValid(),
                      "MaterialInstance::Finalize: '{}' overrides field '{}' not in parent schema",
                      m_Name, ov.Name);
            const MaterialField* field = &parentFields[handle.Index];

            if (!ov.Value.empty())
            {
                // A param override: copy its bytes at the parent field's reflected offset.
                const usize writeBytes = std::min<usize>(ov.Value.size(), field->Size);
                VE_ASSERT(field->Offset + writeBytes <= m_Block.size(),
                          "MaterialInstance::Finalize: '{}' override '{}' offset {} + {} exceeds "
                          "block size {}",
                          m_Name, ov.Name, field->Offset, writeBytes, m_Block.size());
                std::memcpy(m_Block.data() + field->Offset, ov.Value.data(), writeBytes);
                continue;
            }

            // A texture override: patch the field's handle slot with the resolved bindless index.
            VE_ASSERT(field->Kind == MaterialField::FieldKind::TextureHandle ||
                          field->Kind == MaterialField::FieldKind::SamplerHandle,
                      "MaterialInstance::Finalize: '{}' texture override '{}' targets a non-handle "
                      "field",
                      m_Name, ov.Name);
            const Texture* tex = ov.Texture.Get();
            VE_ASSERT(tex != nullptr,
                      "MaterialInstance::Finalize: '{}' texture override '{}' is not resident",
                      m_Name, ov.Name);
            VE_ASSERT(field->Offset + sizeof(u32) <= m_Block.size(),
                      "MaterialInstance::Finalize: '{}' override '{}' offset {} + 4 exceeds block "
                      "size {}",
                      m_Name, ov.Name, field->Offset, m_Block.size());
            const u32 index = field->Kind == MaterialField::FieldKind::TextureHandle
                                  ? tex->GetHandle().Index
                                  : tex->GetSamplerHandle().Index;
            std::memcpy(m_Block.data() + field->Offset, &index, sizeof(u32));

            // Patch the paired <name>Sampler slot when overriding a TextureHandle.
            if (field->PairedSampler != MaterialFieldHandle::Invalid)
            {
                const MaterialField& samplerField = parentFields[field->PairedSampler];
                if (samplerField.Offset + sizeof(u32) <= m_Block.size())
                {
                    const u32 samplerIndex = tex->GetSamplerHandle().Index;
                    std::memcpy(m_Block.data() + samplerField.Offset, &samplerIndex, sizeof(u32));
                }
            }
        }

        m_Handle =
            m_Context.GetBindlessRegistry().RegisterMaterial(std::span<const std::byte>(m_Block));
        m_Registered = true;
    }

    Ref<MaterialInstance> MaterialInstance::Clone(std::string_view name) const
    {
        VE_ASSERT(m_Registered, "MaterialInstance::Clone: '{}' is not finalized", m_Name);

        // Reuse the private constructor (parent + overrides) for the resident-handle bookkeeping,
        // then overwrite the block with this instance's current one rather than re-running
        // Finalize's parent-default reseed — so the copy captures the live appearance, including
        // any Set* writes since — and register the copy's own per-material slot.
        Ref<MaterialInstance> copy =
            Ref<MaterialInstance>(new MaterialInstance(MaterialInstanceInfo{
                .Name = string(name),
                .Context = &m_Context,
                .Parent = m_Parent,
                .Overrides = m_Overrides,
            }));
        copy->m_Textures = m_Textures;
        copy->m_Block = m_Block;
        copy->m_Handle = m_Context.GetBindlessRegistry().RegisterMaterial(
            std::span<const std::byte>(copy->m_Block));
        copy->m_Registered = true;
        copy->m_Revision = m_Revision;
        return copy;
    }

    void MaterialInstance::CopyParamsFrom(const MaterialInstance& source)
    {
        VE_ASSERT(m_Registered, "MaterialInstance::CopyParamsFrom: '{}' is not finalized", m_Name);
        VE_ASSERT(source.m_Registered,
                  "MaterialInstance::CopyParamsFrom: source '{}' is not finalized", source.m_Name);
        VE_ASSERT(source.m_Parent.Get() == m_Parent.Get(),
                  "MaterialInstance::CopyParamsFrom: '{}' and source '{}' have different parents, "
                  "so their blocks describe different schemas",
                  m_Name, source.m_Name);

        m_Textures = source.m_Textures;
        m_Block = source.m_Block;
        UploadParams();
    }

    void MaterialInstance::Bind(CommandBuffer& cmd) const
    {
        const Material& parent = *m_Parent.Get();

        // A PostProcess parent owns no pipeline — the PostProcessScenePass binds the fullscreen
        // pipeline it built from the parent's shaders, so Bind only pushes the selector. A Surface
        // parent binds its own.
        if (parent.GetPipeline() != nullptr)
        {
            cmd.BindPipeline(parent.GetPipeline());
        }

        // A Surface material reads its index from the per-draw DrawData SSBO (the geometry pass
        // writes GetMaterialSelector() into each record), so it pushes nothing. A PostProcess
        // material pushes the frame-folded selector at offset 0.
        const u32 selectorOffset = parent.GetSelectorOffset();
        if (selectorOffset == Material::NoSelectorPush)
        {
            return;
        }

        cmd.PushConstants(GetMaterialSelector(), selectorOffset);
    }

    void MaterialInstance::BindSkinned(CommandBuffer& cmd) const
    {
        const Material& parent = *m_Parent.Get();
        const Ref<GraphicsPipeline>& skinned = parent.GetSkinnedPipeline();
        VE_ASSERT(skinned != nullptr,
                  "MaterialInstance::BindSkinned: '{}' has no skinned pipeline — a skinned draw "
                  "needs a cooked Surface material",
                  m_Name);
        cmd.BindPipeline(skinned);
        // A Surface material reads its selector from the per-draw DrawData SSBO, so it pushes
        // nothing — the skinned surface path is Surface-only, so there is never a selector to push.
    }

    void MaterialInstance::EnsureSkinnedPipeline(AssetManager& assets) const
    {
        m_Parent.Get()->EnsureSkinnedPipeline(assets);
    }

    u32 MaterialInstance::GetMaterialSelector() const
    {
        // Fold the current frame's region base into the selector so the shader's load lands in
        // this frame's copy of the ring-buffered material buffer.
        return m_Context.GetBindlessRegistry().GetCurrentFrameBase() + m_Handle.Offset;
    }

    void MaterialInstance::UploadParams() const
    {
        ++m_Revision;
        m_Context.GetBindlessRegistry().UpdateMaterial(m_Handle,
                                                       std::span<const std::byte>(m_Block));
    }

    void MaterialInstance::SetTexture(std::string_view name, AssetHandle<Texture> texture)
    {
        SetTexture(RequireField(name, "SetTexture"), std::move(texture));
    }

    void MaterialInstance::SetTexture(const MaterialFieldHandle field, AssetHandle<Texture> texture)
    {
        const MaterialField& entry = ResolveField(field, "SetTexture");
        VE_ASSERT(
            entry.Kind == MaterialField::FieldKind::TextureHandle,
            "MaterialInstance::SetTexture: field '{}' in instance '{}' is not a TextureHandle "
            "(Kind={})",
            entry.Name, m_Name, static_cast<u32>(entry.Kind));

        const Texture& tex = *texture.Get();

        VE_ASSERT(entry.Offset + sizeof(u32) <= m_Block.size(),
                  "MaterialInstance::SetTexture: field '{}' offset {} + 4 exceeds block size {}",
                  entry.Name, entry.Offset, m_Block.size());
        const u32 textureIndex = tex.GetHandle().Index;
        std::memcpy(m_Block.data() + entry.Offset, &textureIndex, sizeof(u32));

        // Also patch the paired <name>Sampler field if the schema resolved one.
        if (entry.PairedSampler != MaterialFieldHandle::Invalid)
        {
            const MaterialField& samplerField = m_Parent.Get()->GetFields()[entry.PairedSampler];
            VE_ASSERT(
                samplerField.Offset + sizeof(u32) <= m_Block.size(),
                "MaterialInstance::SetTexture: sampler field '{}' offset {} + 4 exceeds block "
                "size {}",
                samplerField.Name, samplerField.Offset, m_Block.size());
            const u32 samplerIndex = tex.GetSamplerHandle().Index;
            std::memcpy(m_Block.data() + samplerField.Offset, &samplerIndex, sizeof(u32));
        }

        const u64 texId = texture.Id().Value;
        bool found = false;
        for (AssetHandle<Texture>& existing : m_Textures)
        {
            if (existing.Id().Value == texId)
            {
                existing = std::move(texture);
                found = true;
                break;
            }
        }
        if (!found)
        {
            m_Textures.push_back(std::move(texture));
        }

        UploadParams();
    }

    void MaterialInstance::SetParam(std::string_view name, const vec4& value)
    {
        SetParam(RequireField(name, "SetParam"), value);
    }

    void MaterialInstance::SetParam(const MaterialFieldHandle field, const vec4& value)
    {
        const MaterialField& entry = ResolveField(field, "SetParam");
        VE_ASSERT(
            entry.Kind == MaterialField::FieldKind::Param,
            "MaterialInstance::SetParam: field '{}' in instance '{}' is not a Param (Kind={})",
            entry.Name, m_Name, static_cast<u32>(entry.Kind));

        const u32 writeBytes = std::min(entry.Size, static_cast<u32>(sizeof(vec4)));
        VE_ASSERT(entry.Offset + writeBytes <= m_Block.size(),
                  "MaterialInstance::SetParam: field '{}' offset {} + {} exceeds block size {}",
                  entry.Name, entry.Offset, writeBytes, m_Block.size());

        std::memcpy(m_Block.data() + entry.Offset, &value, writeBytes);

        UploadParams();
    }

    void MaterialInstance::SetParam(std::string_view name, f32 value)
    {
        SetParam(RequireField(name, "SetParam"), value);
    }

    void MaterialInstance::SetParam(const MaterialFieldHandle field, const f32 value)
    {
        const MaterialField& entry = ResolveField(field, "SetParam");
        VE_ASSERT(
            entry.Kind == MaterialField::FieldKind::Param,
            "MaterialInstance::SetParam: field '{}' in instance '{}' is not a Param (Kind={})",
            entry.Name, m_Name, static_cast<u32>(entry.Kind));

        // Write only the field's reflected size — for a scalar param that is 4
        // bytes, never spilling into the following bytes of the block.
        const u32 writeBytes = std::min(entry.Size, static_cast<u32>(sizeof(f32)));
        VE_ASSERT(entry.Offset + writeBytes <= m_Block.size(),
                  "MaterialInstance::SetParam: field '{}' offset {} + {} exceeds block size {}",
                  entry.Name, entry.Offset, writeBytes, m_Block.size());

        std::memcpy(m_Block.data() + entry.Offset, &value, writeBytes);

        UploadParams();
    }

    void MaterialInstance::SetTextureHandle(std::string_view name, Renderer::TextureHandle handle)
    {
        SetTextureHandle(RequireField(name, "SetTextureHandle"), handle);
    }

    void MaterialInstance::SetTextureHandle(const MaterialFieldHandle field,
                                            Renderer::TextureHandle handle)
    {
        const MaterialField& entry = ResolveField(field, "SetTextureHandle");
        VE_ASSERT(entry.Kind == MaterialField::FieldKind::TextureHandle,
                  "MaterialInstance::SetTextureHandle: field '{}' in instance '{}' is not a "
                  "TextureHandle (Kind={})",
                  entry.Name, m_Name, static_cast<u32>(entry.Kind));
        VE_ASSERT(entry.Offset + sizeof(u32) <= m_Block.size(),
                  "MaterialInstance::SetTextureHandle: field '{}' offset {} + 4 exceeds block "
                  "size {}",
                  entry.Name, entry.Offset, m_Block.size());

        const u32 index = handle.Index;
        std::memcpy(m_Block.data() + entry.Offset, &index, sizeof(u32));

        UploadParams();
    }

    void MaterialInstance::SetSamplerHandle(std::string_view name, Renderer::SamplerHandle handle)
    {
        SetSamplerHandle(RequireField(name, "SetSamplerHandle"), handle);
    }

    void MaterialInstance::SetSamplerHandle(const MaterialFieldHandle field,
                                            Renderer::SamplerHandle handle)
    {
        const MaterialField& entry = ResolveField(field, "SetSamplerHandle");
        VE_ASSERT(entry.Kind == MaterialField::FieldKind::SamplerHandle,
                  "MaterialInstance::SetSamplerHandle: field '{}' in instance '{}' is not a "
                  "SamplerHandle (Kind={})",
                  entry.Name, m_Name, static_cast<u32>(entry.Kind));
        VE_ASSERT(entry.Offset + sizeof(u32) <= m_Block.size(),
                  "MaterialInstance::SetSamplerHandle: field '{}' offset {} + 4 exceeds block "
                  "size {}",
                  entry.Name, entry.Offset, m_Block.size());

        const u32 index = handle.Index;
        std::memcpy(m_Block.data() + entry.Offset, &index, sizeof(u32));

        UploadParams();
    }

    void MaterialInstance::SetStorageBufferHandle(std::string_view name,
                                                  Renderer::StorageBufferHandle handle)
    {
        SetStorageBufferHandle(RequireField(name, "SetStorageBufferHandle"), handle);
    }

    void MaterialInstance::SetStorageBufferHandle(const MaterialFieldHandle field,
                                                  Renderer::StorageBufferHandle handle)
    {
        const MaterialField& entry = ResolveField(field, "SetStorageBufferHandle");
        VE_ASSERT(entry.Kind == MaterialField::FieldKind::StorageBufferHandle,
                  "MaterialInstance::SetStorageBufferHandle: field '{}' in instance '{}' is not a "
                  "StorageBufferHandle (Kind={})",
                  entry.Name, m_Name, static_cast<u32>(entry.Kind));
        VE_ASSERT(
            entry.Offset + sizeof(u32) <= m_Block.size(),
            "MaterialInstance::SetStorageBufferHandle: field '{}' offset {} + 4 exceeds block "
            "size {}",
            entry.Name, entry.Offset, m_Block.size());

        const u32 index = handle.Index;
        std::memcpy(m_Block.data() + entry.Offset, &index, sizeof(u32));

        UploadParams();
    }

    void MaterialInstance::SetVolumeHandle(std::string_view name, Renderer::VolumeHandle handle)
    {
        SetVolumeHandle(RequireField(name, "SetVolumeHandle"), handle);
    }

    void MaterialInstance::SetVolumeHandle(const MaterialFieldHandle field,
                                           Renderer::VolumeHandle handle)
    {
        const MaterialField& entry = ResolveField(field, "SetVolumeHandle");
        VE_ASSERT(entry.Kind == MaterialField::FieldKind::VolumeHandle,
                  "MaterialInstance::SetVolumeHandle: field '{}' in instance '{}' is not a "
                  "VolumeHandle (Kind={})",
                  entry.Name, m_Name, static_cast<u32>(entry.Kind));
        VE_ASSERT(entry.Offset + sizeof(u32) <= m_Block.size(),
                  "MaterialInstance::SetVolumeHandle: field '{}' offset {} + 4 exceeds block "
                  "size {}",
                  entry.Name, entry.Offset, m_Block.size());

        const u32 index = handle.Index;
        std::memcpy(m_Block.data() + entry.Offset, &index, sizeof(u32));

        UploadParams();
    }
}
