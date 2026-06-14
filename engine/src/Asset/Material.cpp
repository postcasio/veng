#include <Veng/Asset/Material.h>

#include <cstring>
#include <string_view>

#include <Veng/Assert.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>

namespace Veng
{
    using namespace Renderer;

    Material::Material(const MaterialInfo& info) :
        m_Context(*info.Context),
        m_Name(info.Name),
        m_Pipeline(info.Pipeline),
        m_VertexShader(info.VertexShader),
        m_FragmentShader(info.FragmentShader),
        m_Textures(info.Textures),
        m_Params(info.Params),
        m_Fields(info.Fields),
        m_SelectorOffset(info.SelectorOffset)
    {
        // Construction is unregistered: the texture indices in m_Params are not
        // yet resolved (the textures register on the main thread) and the SSBO
        // slot is allocated in Finalize().
    }

    Material::~Material()
    {
        if (m_Registered)
            m_Context.GetBindlessRegistry().Release(m_Handle);
    }

    void Material::Finalize(Ref<Renderer::GraphicsPipeline> pipeline)
    {
        VE_ASSERT(!m_Registered, "Material::Finalize: '{}' already registered", m_Name);
        VE_ASSERT(pipeline != nullptr, "Material::Finalize: '{}' given a null pipeline", m_Name);

        m_Pipeline = std::move(pipeline);

        // The textures are Finalize()d by now, so their bindless handles are
        // valid: patch each TextureHandle/SamplerHandle field in the param block
        // with the resolved index of the texture it references.
        for (const MaterialField& field : m_Fields)
        {
            if (field.Kind != MaterialField::FieldKind::TextureHandle
             && field.Kind != MaterialField::FieldKind::SamplerHandle)
                continue;

            const Texture* tex = nullptr;
            for (const AssetHandle<Texture>& handle : m_Textures)
            {
                if (handle.Id().Value == field.TextureId)
                {
                    tex = handle.Get();
                    break;
                }
            }
            VE_ASSERT(tex != nullptr,
                      "Material::Finalize: '{}' field '{}' references texture {} not in its dependency set",
                      m_Name, field.Name, field.TextureId);

            const u32 index = field.Kind == MaterialField::FieldKind::TextureHandle
                ? tex->GetHandle().Index
                : tex->GetSamplerHandle().Index;
            std::memcpy(reinterpret_cast<std::byte*>(&m_Params) + field.Offset, &index, sizeof(u32));
        }

        // Allocate a slot in the registry and upload the patched MaterialData.
        m_Handle = m_Context.GetBindlessRegistry().RegisterMaterial(
            std::as_bytes(std::span(&m_Params, 1)));
        m_Registered = true;
    }

    void Material::Bind(CommandBuffer& cmd) const
    {
        cmd.BindPipeline(m_Pipeline);
        cmd.PushConstants(m_Handle.Index, m_SelectorOffset);
    }

    const MaterialField* Material::FindField(std::string_view name) const
    {
        for (const MaterialField& f : m_Fields)
        {
            if (f.Name == name)
                return &f;
        }
        return nullptr;
    }

    void Material::UploadParams() const
    {
        m_Context.GetBindlessRegistry().UpdateMaterial(
            m_Handle, std::as_bytes(std::span(&m_Params, 1)));
    }

    void Material::SetTexture(std::string_view name, AssetHandle<Texture> texture)
    {
        const MaterialField* field = FindField(name);
        VE_ASSERT(field != nullptr,
                  "Material::SetTexture: field '{}' not found in material '{}'",
                  name, m_Name);
        VE_ASSERT(field->Kind == MaterialField::FieldKind::TextureHandle,
                  "Material::SetTexture: field '{}' in material '{}' is not a TextureHandle (Kind={})",
                  name, m_Name, static_cast<u32>(field->Kind));

        const Texture& tex = *texture.Get();

        // Write the sampled-image handle index into m_Params at the field's offset.
        VE_ASSERT(field->Offset + sizeof(u32) <= sizeof(MaterialData),
                  "Material::SetTexture: field '{}' offset {} + 4 exceeds MaterialData size",
                  name, field->Offset);
        const u32 textureIndex = tex.GetHandle().Index;
        std::memcpy(reinterpret_cast<std::byte*>(&m_Params) + field->Offset,
                    &textureIndex, sizeof(u32));

        // Also patch the paired <name>Sampler field if it exists.
        const string samplerFieldName = string(name) + "Sampler";
        const MaterialField* samplerField = FindField(samplerFieldName);
        if (samplerField != nullptr
         && samplerField->Kind == MaterialField::FieldKind::SamplerHandle)
        {
            VE_ASSERT(samplerField->Offset + sizeof(u32) <= sizeof(MaterialData),
                      "Material::SetTexture: sampler field '{}' offset {} + 4 exceeds MaterialData size",
                      samplerFieldName, samplerField->Offset);
            const u32 samplerIndex = tex.GetSamplerHandle().Index;
            std::memcpy(reinterpret_cast<std::byte*>(&m_Params) + samplerField->Offset,
                        &samplerIndex, sizeof(u32));
        }

        // Keep the texture asset resident (replace by AssetId if already present,
        // otherwise append).
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
            m_Textures.push_back(std::move(texture));

        UploadParams();
    }

    void Material::SetParam(std::string_view name, const vec4& value)
    {
        const MaterialField* field = FindField(name);
        VE_ASSERT(field != nullptr,
                  "Material::SetParam: field '{}' not found in material '{}'",
                  name, m_Name);
        VE_ASSERT(field->Kind == MaterialField::FieldKind::Param,
                  "Material::SetParam: field '{}' in material '{}' is not a Param (Kind={})",
                  name, m_Name, static_cast<u32>(field->Kind));

        const u32 writeBytes = std::min(field->Size, static_cast<u32>(sizeof(vec4)));
        VE_ASSERT(field->Offset + writeBytes <= sizeof(MaterialData),
                  "Material::SetParam: field '{}' offset {} + {} exceeds MaterialData size",
                  name, field->Offset, writeBytes);

        std::memcpy(reinterpret_cast<std::byte*>(&m_Params) + field->Offset,
                    &value, writeBytes);

        UploadParams();
    }
}
