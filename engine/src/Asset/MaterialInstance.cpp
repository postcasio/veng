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
        VE_ASSERT(m_Parent.Get() != nullptr,
                  "MaterialInstance '{}': parent material is not resident at construction", m_Name);

        // The block is seeded from the parent's default block in Finalize() — the parent's block is
        // only patched (handle slots resolved) once the parent itself is finalized, which the
        // dependency ordering guarantees runs before this instance's Finalize.

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

    const MaterialField* MaterialInstance::FindField(std::string_view name) const
    {
        for (const MaterialField& f : m_Parent.Get()->GetFields())
        {
            if (f.Name == name)
            {
                return &f;
            }
        }
        return nullptr;
    }

    MaterialInstance::EmissiveParams MaterialInstance::GetEmissive() const
    {
        EmissiveParams emissive;

        const MaterialField* colorField = FindField("EmissiveColor");
        if (colorField != nullptr && colorField->Offset + sizeof(vec3) <= m_Block.size())
        {
            vec3 color{0.0f};
            std::memcpy(&color, m_Block.data() + colorField->Offset, sizeof(vec3));
            emissive.Color = color;
        }

        const MaterialField* textureField = FindField("EmissiveTexture");
        if (textureField != nullptr && textureField->Offset + sizeof(u32) <= m_Block.size())
        {
            std::memcpy(&emissive.Texture, m_Block.data() + textureField->Offset, sizeof(u32));

            const MaterialField* samplerField = FindField("EmissiveSampler");
            if (samplerField != nullptr && samplerField->Offset + sizeof(u32) <= m_Block.size())
            {
                std::memcpy(&emissive.Sampler, m_Block.data() + samplerField->Offset, sizeof(u32));
            }
        }

        return emissive;
    }

    void MaterialInstance::Finalize()
    {
        VE_ASSERT(!m_Registered, "MaterialInstance::Finalize: '{}' already registered", m_Name);

        // Seed the block from the parent's finalized default block (its handle slots are patched).
        const std::span<const std::byte> defaultBlock = m_Parent.Get()->GetDefaultBlock();
        m_Block.assign(defaultBlock.begin(), defaultBlock.end());

        // Apply each override over the seeded default block.
        for (const MaterialOverride& ov : m_Overrides)
        {
            const MaterialField* field = FindField(ov.Name);
            VE_ASSERT(field != nullptr,
                      "MaterialInstance::Finalize: '{}' overrides field '{}' not in parent schema",
                      m_Name, ov.Name);

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
            if (field->Kind == MaterialField::FieldKind::TextureHandle)
            {
                const string samplerName = ov.Name + "Sampler";
                const MaterialField* samplerField = FindField(samplerName);
                if (samplerField != nullptr &&
                    samplerField->Kind == MaterialField::FieldKind::SamplerHandle &&
                    samplerField->Offset + sizeof(u32) <= m_Block.size())
                {
                    const u32 samplerIndex = tex->GetSamplerHandle().Index;
                    std::memcpy(m_Block.data() + samplerField->Offset, &samplerIndex, sizeof(u32));
                }
            }
        }

        m_Handle =
            m_Context.GetBindlessRegistry().RegisterMaterial(std::span<const std::byte>(m_Block));
        m_Registered = true;
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

    u32 MaterialInstance::GetMaterialSelector() const
    {
        // Fold the current frame's region base into the selector so the shader's
        // index * MaterialParamStride load lands in this frame's copy of the
        // ring-buffered material buffer.
        return m_Context.GetBindlessRegistry().GetCurrentFrameBase() + m_Handle.Index;
    }

    void MaterialInstance::UploadParams() const
    {
        m_Context.GetBindlessRegistry().UpdateMaterial(m_Handle,
                                                       std::span<const std::byte>(m_Block));
    }

    void MaterialInstance::SetTexture(std::string_view name, AssetHandle<Texture> texture)
    {
        const MaterialField* field = FindField(name);
        VE_ASSERT(field != nullptr,
                  "MaterialInstance::SetTexture: field '{}' not found in instance '{}'", name,
                  m_Name);
        VE_ASSERT(
            field->Kind == MaterialField::FieldKind::TextureHandle,
            "MaterialInstance::SetTexture: field '{}' in instance '{}' is not a TextureHandle "
            "(Kind={})",
            name, m_Name, static_cast<u32>(field->Kind));

        const Texture& tex = *texture.Get();

        VE_ASSERT(field->Offset + sizeof(u32) <= m_Block.size(),
                  "MaterialInstance::SetTexture: field '{}' offset {} + 4 exceeds block size {}",
                  name, field->Offset, m_Block.size());
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
                "MaterialInstance::SetTexture: sampler field '{}' offset {} + 4 exceeds block "
                "size {}",
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
        {
            m_Textures.push_back(std::move(texture));
        }

        UploadParams();
    }

    void MaterialInstance::SetParam(std::string_view name, const vec4& value)
    {
        const MaterialField* field = FindField(name);
        VE_ASSERT(field != nullptr,
                  "MaterialInstance::SetParam: field '{}' not found in instance '{}'", name,
                  m_Name);
        VE_ASSERT(
            field->Kind == MaterialField::FieldKind::Param,
            "MaterialInstance::SetParam: field '{}' in instance '{}' is not a Param (Kind={})",
            name, m_Name, static_cast<u32>(field->Kind));

        const u32 writeBytes = std::min(field->Size, static_cast<u32>(sizeof(vec4)));
        VE_ASSERT(field->Offset + writeBytes <= m_Block.size(),
                  "MaterialInstance::SetParam: field '{}' offset {} + {} exceeds block size {}",
                  name, field->Offset, writeBytes, m_Block.size());

        std::memcpy(m_Block.data() + field->Offset, &value, writeBytes);

        UploadParams();
    }

    void MaterialInstance::SetParam(std::string_view name, f32 value)
    {
        const MaterialField* field = FindField(name);
        VE_ASSERT(field != nullptr,
                  "MaterialInstance::SetParam: field '{}' not found in instance '{}'", name,
                  m_Name);
        VE_ASSERT(
            field->Kind == MaterialField::FieldKind::Param,
            "MaterialInstance::SetParam: field '{}' in instance '{}' is not a Param (Kind={})",
            name, m_Name, static_cast<u32>(field->Kind));

        // Write only the field's reflected size — for a scalar param that is 4
        // bytes, never spilling into the following bytes of the block.
        const u32 writeBytes = std::min(field->Size, static_cast<u32>(sizeof(f32)));
        VE_ASSERT(field->Offset + writeBytes <= m_Block.size(),
                  "MaterialInstance::SetParam: field '{}' offset {} + {} exceeds block size {}",
                  name, field->Offset, writeBytes, m_Block.size());

        std::memcpy(m_Block.data() + field->Offset, &value, writeBytes);

        UploadParams();
    }

    void MaterialInstance::SetTextureHandle(std::string_view name, Renderer::TextureHandle handle)
    {
        const MaterialField* field = FindField(name);
        VE_ASSERT(field != nullptr,
                  "MaterialInstance::SetTextureHandle: field '{}' not found in instance '{}'", name,
                  m_Name);
        VE_ASSERT(field->Kind == MaterialField::FieldKind::TextureHandle,
                  "MaterialInstance::SetTextureHandle: field '{}' in instance '{}' is not a "
                  "TextureHandle (Kind={})",
                  name, m_Name, static_cast<u32>(field->Kind));
        VE_ASSERT(field->Offset + sizeof(u32) <= m_Block.size(),
                  "MaterialInstance::SetTextureHandle: field '{}' offset {} + 4 exceeds block "
                  "size {}",
                  name, field->Offset, m_Block.size());

        const u32 index = handle.Index;
        std::memcpy(m_Block.data() + field->Offset, &index, sizeof(u32));

        UploadParams();
    }

    void MaterialInstance::SetSamplerHandle(std::string_view name, Renderer::SamplerHandle handle)
    {
        const MaterialField* field = FindField(name);
        VE_ASSERT(field != nullptr,
                  "MaterialInstance::SetSamplerHandle: field '{}' not found in instance '{}'", name,
                  m_Name);
        VE_ASSERT(field->Kind == MaterialField::FieldKind::SamplerHandle,
                  "MaterialInstance::SetSamplerHandle: field '{}' in instance '{}' is not a "
                  "SamplerHandle (Kind={})",
                  name, m_Name, static_cast<u32>(field->Kind));
        VE_ASSERT(field->Offset + sizeof(u32) <= m_Block.size(),
                  "MaterialInstance::SetSamplerHandle: field '{}' offset {} + 4 exceeds block "
                  "size {}",
                  name, field->Offset, m_Block.size());

        const u32 index = handle.Index;
        std::memcpy(m_Block.data() + field->Offset, &index, sizeof(u32));

        UploadParams();
    }
}
