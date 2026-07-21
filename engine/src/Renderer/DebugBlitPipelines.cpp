#include "DebugBlitPipelines.h"

#include "Passes/DebugBlitScenePasses.h"
#include "SceneRendererIds.h"

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>

namespace Veng::Renderer
{
    namespace
    {
        // The engine core pack's debug-blit fragment shaders, one per blit pipeline.
        constexpr AssetId AlbedoBlitFragId{0xF90F709155D04BE7ULL};
        constexpr AssetId NormalBlitFragId{0x5A2CD7B270EAE5CDULL};
        constexpr AssetId DepthBlitFragId{0xE05F5F86E72F96D5ULL};
        constexpr AssetId OrmBlitFragId{0x7992B54A844CB1E1ULL};
        constexpr AssetId AoBlitFragId{0x97974B40192934E4ULL};
        constexpr AssetId MotionBlitFragId{0xCCD40C76935382FDULL};
        constexpr AssetId ShadowBlitFragId{0x0B61D5D42DAEF190ULL};
        constexpr AssetId CocBlitFragId{0x878C4C3C6E247858ULL};
    }

    Unique<DebugBlitPipelines>
    DebugBlitPipelines::Create(Context& context, AssetManager& assets, const Format outputFormat,
                               const Ref<DescriptorSetLayout>& shadowBlitSetLayout)
    {
        auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "SceneRenderer: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };

        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");

        // Builds a fullscreen pipeline (shared vertex stage) over a layout, writing the output format.
        auto MakePipeline = [&](const char* name, const Ref<PipelineLayout>& layout,
                                const AssetHandle<Veng::Shader>& fs) -> Ref<GraphicsPipeline>
        {
            return GraphicsPipeline::Create(
                context, {
                             .Name = name,
                             .ColorAttachments = {{.Format = outputFormat}},
                             .PipelineLayout = layout,
                             .ShaderStages =
                                 {
                                     {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                     {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                                 },
                         });
        };

        // The g-buffer debug blits share the BlitPushConstants layout; only the fragment differs.
        const PushConstantRange blitRange =
            PushConstantRange::Of<BlitPushConstants>(ShaderStage::Fragment);
        auto MakeBlitLayout = [&](const char* name) -> Ref<PipelineLayout>
        {
            return PipelineLayout::Create(context,
                                          {.Name = name, .PushConstantRanges = {blitRange}});
        };

        Unique<DebugBlitPipelines> blits = CreateUnique<DebugBlitPipelines>();

        blits->AlbedoLayout = MakeBlitLayout("SceneRenderer Albedo Blit Layout");
        blits->Albedo = MakePipeline("SceneRenderer Albedo Blit Pipeline", blits->AlbedoLayout,
                                     LoadShader(AlbedoBlitFragId, "albedo-blit fragment"));

        blits->NormalLayout = MakeBlitLayout("SceneRenderer Normal Blit Layout");
        blits->Normal = MakePipeline("SceneRenderer Normal Blit Pipeline", blits->NormalLayout,
                                     LoadShader(NormalBlitFragId, "normal-blit fragment"));

        blits->DepthLayout = MakeBlitLayout("SceneRenderer Depth Blit Layout");
        blits->Depth = MakePipeline("SceneRenderer Depth Blit Pipeline", blits->DepthLayout,
                                    LoadShader(DepthBlitFragId, "depth-blit fragment"));

        blits->AoLayout = MakeBlitLayout("SceneRenderer AO Blit Layout");
        blits->Ao = MakePipeline("SceneRenderer AO Blit Pipeline", blits->AoLayout,
                                 LoadShader(AoBlitFragId, "AO-blit fragment"));

        // Motion-vector blit: samples the velocity target through the same texture+sampler push.
        blits->MotionLayout = MakeBlitLayout("SceneRenderer Motion Blit Layout");
        blits->Motion = MakePipeline("SceneRenderer Motion Blit Pipeline", blits->MotionLayout,
                                     LoadShader(MotionBlitFragId, "motion-vector-blit fragment"));

        // The CoC blit normalizes against the frame's budget, so it carries its own push block.
        blits->CocLayout = PipelineLayout::Create(
            context, {
                         .Name = "SceneRenderer CoC Blit Layout",
                         .PushConstantRanges = {PushConstantRange::Of<CocBlitPushConstants>(
                             ShaderStage::Fragment)},
                     });
        blits->Coc = MakePipeline("SceneRenderer CoC Blit Pipeline", blits->CocLayout,
                                  LoadShader(CocBlitFragId, "circle-of-confusion-blit fragment"));

        // Shadow blit reads raw depth through a dedicated set 1, not bindless, so its layout carries
        // that set and no push block.
        blits->ShadowLayout =
            PipelineLayout::Create(context, {
                                                .Name = "SceneRenderer Shadow Blit Layout",
                                                .DescriptorSetLayouts = {shadowBlitSetLayout},
                                            });
        blits->Shadow = MakePipeline("SceneRenderer Shadow Blit Pipeline", blits->ShadowLayout,
                                     LoadShader(ShadowBlitFragId, "shadow-blit fragment"));

        // The channel select for the ORM blit is a push value, not a separate pipeline.
        blits->OrmLayout = PipelineLayout::Create(
            context, {
                         .Name = "SceneRenderer ORM Blit Layout",
                         .PushConstantRanges = {PushConstantRange::Of<OrmBlitPushConstants>(
                             ShaderStage::Fragment)},
                     });
        blits->Orm = MakePipeline("SceneRenderer ORM Blit Pipeline", blits->OrmLayout,
                                  LoadShader(OrmBlitFragId, "ORM-blit fragment"));

        return blits;
    }
}
