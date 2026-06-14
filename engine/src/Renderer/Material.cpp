#include <Veng/Renderer/Material.h>

#include <cstring>
#include <string_view>

#include <Veng/Assert.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Texture.h>

namespace Veng::Renderer
{
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
        // Allocate a slot in the registry and upload the already-patched
        // MaterialData (texture handle indices are pre-filled by MaterialLoader).
        m_Handle = m_Context.GetBindlessRegistry().RegisterMaterial(
            std::as_bytes(std::span(&m_Params, 1)));
    }

    Material::~Material()
    {
        m_Context.GetBindlessRegistry().Release(m_Handle);
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
