#include <Veng/Asset/Material.h>

#include <cstring>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetBuild.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng
{
    using namespace Renderer;

    Material::Material(const MaterialInfo& info)
        : m_Context(*info.Context), m_Name(info.Name), m_Domain(info.Domain),
          m_Pipeline(info.Pipeline), m_VertexShader(info.VertexShader),
          m_FragmentShader(info.FragmentShader), m_Textures(info.Textures), m_Block(info.Block),
          m_Fields(info.Fields), m_SelectorOffset(info.SelectorOffset)
    {
        // Unfinalized at construction: the default block's handle slots are patched and the
        // pipeline is stored in Finalize().
    }

    Task<Detail::BuiltAsset<Material>>
    Detail::SubmitAssetBuild(Renderer::Context&, TaskSystem& tasks, MaterialInfo info,
                             Ref<Renderer::PipelineLayout> layout)
    {
        VE_ASSERT(layout != nullptr,
                  "AssetManager::Build<Material>: '{}' given a null pipeline "
                  "layout",
                  info.Name);

        return tasks.Submit(
            [info = std::move(info), layout = std::move(layout)]() mutable
            {
                const Ref<Material> material = Material::Prepare(info);

                // Patching the default block with the resolved bindless indices is render-thread
                // work (the textures finalize there), so it is deferred to the continuation.
                return Detail::BuiltAsset<Material>{
                    .Resource = material,
                    .Finalize = [material, layout = std::move(layout),
                                 pipeline = info.Pipeline]() mutable -> VoidResult
                    {
                        material->Finalize(std::move(layout), pipeline);
                        return {};
                    },
                };
            });
    }

    Ref<Material> Detail::BuildAssetSync(Renderer::Context&, const MaterialInfo& data,
                                         Ref<Renderer::PipelineLayout> layout)
    {
        VE_ASSERT(layout != nullptr,
                  "AssetManager::BuildSync<Material>: '{}' given a null pipeline layout",
                  data.Name);

        const Ref<Material> material = Material::Prepare(data);
        material->Finalize(std::move(layout), data.Pipeline);
        return material;
    }

    Material::~Material() = default;

    const Ref<Renderer::ShaderModule>& Material::GetVertexModule() const
    {
        return m_VertexShader.Get()->Module;
    }

    const Ref<Renderer::ShaderModule>& Material::GetFragmentModule() const
    {
        return m_FragmentShader.Get()->Module;
    }

    void Material::Finalize(Ref<Renderer::PipelineLayout> layout,
                            Ref<Renderer::GraphicsPipeline> pipeline)
    {
        VE_ASSERT(!m_Finalized, "Material::Finalize: '{}' already finalized", m_Name);
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
        // valid: patch each TextureHandle/SamplerHandle field in the default block with the
        // resolved index of the texture it references. A handle field with no
        // cooked asset (TextureId == 0) is runtime-bound — its slot stays zero
        // here and is written each frame via an instance's SetTextureHandle/SetSamplerHandle.
        for (const MaterialField& field : m_Fields)
        {
            if (field.Kind != MaterialField::FieldKind::TextureHandle &&
                field.Kind != MaterialField::FieldKind::SamplerHandle)
            {
                continue;
            }

            if (field.TextureId == 0)
            {
                continue;
            }

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

        m_Finalized = true;
    }
}
