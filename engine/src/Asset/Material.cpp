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

    Material::Material(const MaterialInfo& info)
        : m_Context(*info.Context), m_Name(info.Name), m_Domain(info.Domain),
          m_Pipeline(info.Pipeline), m_VertexShader(info.VertexShader),
          m_FragmentShader(info.FragmentShader), m_Textures(info.Textures), m_Block(info.Block),
          m_Fields(info.Fields), m_SelectorOffset(info.SelectorOffset)
    {
        // Unregistered at construction: handle indices and the SSBO slot are assigned in Finalize().
    }

    Material::~Material()
    {
        if (m_Registered)
            m_Context.GetBindlessRegistry().Release(m_Handle);
    }

    void Material::Finalize(Ref<Renderer::PipelineLayout> layout,
                            Ref<Renderer::GraphicsPipeline> pipeline)
    {
        VE_ASSERT(!m_Registered, "Material::Finalize: '{}' already registered", m_Name);
        VE_ASSERT(layout != nullptr, "Material::Finalize: '{}' given a null pipeline layout",
                  m_Name);
        // A Surface material binds its own pipeline; a PostProcess material is
        // finalized with a null pipeline (the PostProcessScenePass builds the
        // GraphicsPipeline against its color format and binds it).
        VE_ASSERT(pipeline != nullptr || m_Domain == MaterialDomain::PostProcess,
                  "Material::Finalize: '{}' given a null pipeline", m_Name);

        m_PipelineLayout = std::move(layout);
        m_Pipeline = std::move(pipeline);

        // The textures are Finalize()d by now, so their bindless handles are
        // valid: patch each TextureHandle/SamplerHandle field in the block with the
        // resolved index of the texture it references. A handle field with no
        // cooked asset (TextureId == 0) is runtime-bound — its slot stays zero
        // here and is written each frame via SetTextureHandle/SetSamplerHandle.
        for (const MaterialField& field : m_Fields)
        {
            if (field.Kind != MaterialField::FieldKind::TextureHandle &&
                field.Kind != MaterialField::FieldKind::SamplerHandle)
                continue;

            if (field.TextureId == 0)
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
                      "Material::Finalize: '{}' field '{}' references texture {} not in its "
                      "dependency set",
                      m_Name, field.Name, field.TextureId);

            VE_ASSERT(field.Offset + sizeof(u32) <= m_Block.size(),
                      "Material::Finalize: '{}' field '{}' offset {} + 4 exceeds block size {}",
                      m_Name, field.Name, field.Offset, m_Block.size());
            const u32 index = field.Kind == MaterialField::FieldKind::TextureHandle
                                  ? tex->GetHandle().Index
                                  : tex->GetSamplerHandle().Index;
            std::memcpy(m_Block.data() + field.Offset, &index, sizeof(u32));
        }

        m_Handle =
            m_Context.GetBindlessRegistry().RegisterMaterial(std::span<const std::byte>(m_Block));
        m_Registered = true;
    }

    void Material::Bind(CommandBuffer& cmd) const
    {
        // A PostProcess material owns no pipeline — the PostProcessScenePass
        // binds the fullscreen pipeline it built from this material's shaders, so
        // Bind only pushes the selector. A Surface material binds its own.
        if (m_Pipeline != nullptr)
            cmd.BindPipeline(m_Pipeline);

        // Fold the current frame's region base into the pushed selector so the
        // shader's index * MaterialParamStride load lands in this frame's copy of
        // the ring-buffered material buffer.
        const u32 selector = m_Context.GetBindlessRegistry().GetCurrentFrameBase() + m_Handle.Index;
        cmd.PushConstants(selector, m_SelectorOffset);
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
        m_Context.GetBindlessRegistry().UpdateMaterial(m_Handle,
                                                       std::span<const std::byte>(m_Block));
    }

    void Material::SetTexture(std::string_view name, AssetHandle<Texture> texture)
    {
        const MaterialField* field = FindField(name);
        VE_ASSERT(field != nullptr, "Material::SetTexture: field '{}' not found in material '{}'",
                  name, m_Name);
        VE_ASSERT(
            field->Kind == MaterialField::FieldKind::TextureHandle,
            "Material::SetTexture: field '{}' in material '{}' is not a TextureHandle (Kind={})",
            name, m_Name, static_cast<u32>(field->Kind));

        const Texture& tex = *texture.Get();

        VE_ASSERT(field->Offset + sizeof(u32) <= m_Block.size(),
                  "Material::SetTexture: field '{}' offset {} + 4 exceeds block size {}", name,
                  field->Offset, m_Block.size());
        const u32 textureIndex = tex.GetHandle().Index;
        std::memcpy(m_Block.data() + field->Offset, &textureIndex, sizeof(u32));

        // Also patch the paired <name>Sampler field if it exists.
        const string samplerFieldName = string(name) + "Sampler";
        const MaterialField* samplerField = FindField(samplerFieldName);
        if (samplerField != nullptr &&
            samplerField->Kind == MaterialField::FieldKind::SamplerHandle)
        {
            VE_ASSERT(
                samplerField->Offset + sizeof(u32) <= m_Block.size(),
                "Material::SetTexture: sampler field '{}' offset {} + 4 exceeds block size {}",
                samplerFieldName, samplerField->Offset, m_Block.size());
            const u32 samplerIndex = tex.GetSamplerHandle().Index;
            std::memcpy(m_Block.data() + samplerField->Offset, &samplerIndex, sizeof(u32));
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
            m_Textures.push_back(std::move(texture));

        UploadParams();
    }

    void Material::SetParam(std::string_view name, const vec4& value)
    {
        const MaterialField* field = FindField(name);
        VE_ASSERT(field != nullptr, "Material::SetParam: field '{}' not found in material '{}'",
                  name, m_Name);
        VE_ASSERT(field->Kind == MaterialField::FieldKind::Param,
                  "Material::SetParam: field '{}' in material '{}' is not a Param (Kind={})", name,
                  m_Name, static_cast<u32>(field->Kind));

        const u32 writeBytes = std::min(field->Size, static_cast<u32>(sizeof(vec4)));
        VE_ASSERT(field->Offset + writeBytes <= m_Block.size(),
                  "Material::SetParam: field '{}' offset {} + {} exceeds block size {}", name,
                  field->Offset, writeBytes, m_Block.size());

        std::memcpy(m_Block.data() + field->Offset, &value, writeBytes);

        UploadParams();
    }

    void Material::SetParam(std::string_view name, f32 value)
    {
        const MaterialField* field = FindField(name);
        VE_ASSERT(field != nullptr, "Material::SetParam: field '{}' not found in material '{}'",
                  name, m_Name);
        VE_ASSERT(field->Kind == MaterialField::FieldKind::Param,
                  "Material::SetParam: field '{}' in material '{}' is not a Param (Kind={})", name,
                  m_Name, static_cast<u32>(field->Kind));

        // Write only the field's reflected size — for a scalar param that is 4
        // bytes, never spilling into the following bytes of the block.
        const u32 writeBytes = std::min(field->Size, static_cast<u32>(sizeof(f32)));
        VE_ASSERT(field->Offset + writeBytes <= m_Block.size(),
                  "Material::SetParam: field '{}' offset {} + {} exceeds block size {}", name,
                  field->Offset, writeBytes, m_Block.size());

        std::memcpy(m_Block.data() + field->Offset, &value, writeBytes);

        UploadParams();
    }

    void Material::SetTextureHandle(std::string_view name, Renderer::TextureHandle handle)
    {
        const MaterialField* field = FindField(name);
        VE_ASSERT(field != nullptr,
                  "Material::SetTextureHandle: field '{}' not found in material '{}'", name,
                  m_Name);
        VE_ASSERT(field->Kind == MaterialField::FieldKind::TextureHandle,
                  "Material::SetTextureHandle: field '{}' in material '{}' is not a TextureHandle "
                  "(Kind={})",
                  name, m_Name, static_cast<u32>(field->Kind));
        VE_ASSERT(field->Offset + sizeof(u32) <= m_Block.size(),
                  "Material::SetTextureHandle: field '{}' offset {} + 4 exceeds block size {}",
                  name, field->Offset, m_Block.size());

        const u32 index = handle.Index;
        std::memcpy(m_Block.data() + field->Offset, &index, sizeof(u32));

        UploadParams();
    }

    void Material::SetSamplerHandle(std::string_view name, Renderer::SamplerHandle handle)
    {
        const MaterialField* field = FindField(name);
        VE_ASSERT(field != nullptr,
                  "Material::SetSamplerHandle: field '{}' not found in material '{}'", name,
                  m_Name);
        VE_ASSERT(field->Kind == MaterialField::FieldKind::SamplerHandle,
                  "Material::SetSamplerHandle: field '{}' in material '{}' is not a SamplerHandle "
                  "(Kind={})",
                  name, m_Name, static_cast<u32>(field->Kind));
        VE_ASSERT(field->Offset + sizeof(u32) <= m_Block.size(),
                  "Material::SetSamplerHandle: field '{}' offset {} + 4 exceeds block size {}",
                  name, field->Offset, m_Block.size());

        const u32 index = handle.Index;
        std::memcpy(m_Block.data() + field->Offset, &index, sizeof(u32));

        UploadParams();
    }
}
