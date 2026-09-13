#include "GuiHdrOverlayScenePass.h"

#include <algorithm>
#include <span>
#include <string_view>

#include <fmt/format.h>

#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Scene/Camera.h>

#include "GuiScenePass.h"
#include "../GuiOverlayProjection.h"

namespace Veng::Renderer
{
    namespace
    {
        // The overlay composites premultiplied-over the loaded scene color, so a fragment emitting
        // (premultiplied color, coverage) accumulates against OneMinusSrcAlpha — the GuiScenePass HDR
        // blend, restated here for the material-composite attachment.
        BlendState PremultipliedOver()
        {
            return {
                .Enable = true,
                .SrcColorFactor = BlendFactor::One,
                .DstColorFactor = BlendFactor::OneMinusSrcAlpha,
                .ColorOp = BlendOp::Add,
                .SrcAlphaFactor = BlendFactor::One,
                .DstAlphaFactor = BlendFactor::OneMinusSrcAlpha,
                .AlphaOp = BlendOp::Add,
            };
        }
    }

    GuiHdrOverlayScenePass::GuiHdrOverlayScenePass(Context& context, AssetManager& assets,
                                                   Format outputFormat, uvec2 extent)
        : m_Context(context), m_Extent(extent), m_OutputFormat(outputFormat)
    {
        // The recorder is driven only through RecordInto, so its composite images stay unbuilt (the
        // lazy path in GuiScenePass); it supplies the geometry rings, the shape/msdf/material
        // pipelines, and the premultiplied-over HDR blend the direct path and the document render reuse.
        m_Gui = GuiScenePass::Create({
            .Context = context,
            .Assets = assets,
            .Extent = extent,
            .OutputFormat = outputFormat,
        });
    }

    GuiHdrOverlayScenePass::~GuiHdrOverlayScenePass() = default;

    void GuiHdrOverlayScenePass::SetComposite(ResourceId docTarget, TextureHandle docTargetHandle,
                                              ResourceId bloomMask, Format bloomMaskFormat)
    {
        m_DocTargetId = docTarget;
        m_DocTargetHandle = docTargetHandle;
        m_BloomMaskId = bloomMask;
        m_BloomMaskFormat = bloomMaskFormat;

        // The composite pipeline's attachment set depends on whether the bloom-mask target exists
        // (bloom on). A mask-presence flip means every cached pipeline is now wrong, so drop them.
        const bool hasMask = bloomMask.IsValid();
        if (hasMask != m_PipelinesHaveMask)
        {
            m_CompositePipelines.clear();
            m_PipelinesHaveMask = hasMask;
        }
    }

    const GuiHdrOverlayView* GuiHdrOverlayScenePass::MaterialOverlay(const SceneView& view,
                                                                     const u32 index) const
    {
        u32 seen = 0;
        for (const GuiHdrOverlayView& overlay : view.HdrOverlays)
        {
            if (overlay.Material == nullptr)
            {
                continue;
            }
            if (seen == index)
            {
                return &overlay;
            }
            ++seen;
        }
        return nullptr;
    }

    const Ref<GraphicsPipeline>&
    GuiHdrOverlayScenePass::CompositePipeline(const MaterialInstance& material)
    {
        const Material* parent = material.GetParent().Get();
        const auto it = m_CompositePipelines.find(parent);
        if (it != m_CompositePipelines.end())
        {
            return it->second;
        }

        // Attachment 0 is the scene color, blended premultiplied-over the loaded target. Attachment 1
        // is the bloom mask (present only under bloom), additive like the translucent mask writer, so
        // two masking overlays over one pixel glow by the sum of the amplitudes they ask for; the float
        // target does not bound that sum. Writes are gated on the material declaring the output.
        vector<PipelineAttachmentInfo> attachments = {
            {.Format = m_OutputFormat, .Blend = PremultipliedOver()}};
        if (m_BloomMaskId.IsValid())
        {
            attachments.push_back({
                .Format = m_BloomMaskFormat,
                .Blend = BlendState::Additive(),
                .Write = parent->IsBloomMaskWriter(),
            });
        }

        Ref<GraphicsPipeline> pipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = fmt::format("Gui HDR Overlay Composite ({})", material.GetName()),
                .ColorAttachments = std::move(attachments),
                .PipelineLayout = material.GetPipelineLayout(),
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = material.GetVertexModule()},
                        {.Stage = ShaderStage::Fragment, .Module = material.GetFragmentModule()},
                    },
            });
        return m_CompositePipelines.emplace(parent, std::move(pipeline)).first->second;
    }

    void GuiHdrOverlayScenePass::RecordDocument(const ScenePassContext& ctx, const u32 index)
    {
        const SceneView& view = ctx.View();
        const GuiHdrOverlayView* overlay = MaterialOverlay(view, index);
        if (overlay == nullptr || overlay->DrawList == nullptr || overlay->DrawList->IsEmpty())
        {
            return;
        }

        // Project the overlay's document into the cleared intermediate exactly as the direct path
        // projects into the scene color: a screen-space overlay scales its logical extent to the
        // scene-color region, a world-anchored one projects through the live camera onto its plane. The
        // intermediate then holds the document at the same pixels the composite reads by pixel coord.
        const vec2 target = vec2(view.PostResolveExtent);
        m_Merged.Clear();
        if (overlay->WorldAnchored)
        {
            const mat4 model = overlay->Model;
            const vec2 surfaceSize = overlay->SurfaceSize;
            const vec2 docExtent = overlay->DocExtent;
            const CameraView& camera = view.Camera;
            m_Merged.AppendProjected(*overlay->DrawList,
                                     [&](vec2 point) -> optional<vec2>
                                     {
                                         return ProjectGuiOverlayPoint(
                                             point, docExtent, surfaceSize, model, camera, target);
                                     });
        }
        else
        {
            const vec2 scale = target / glm::max(overlay->DocExtent, vec2(1.0f));
            m_Merged.AppendProjected(*overlay->DrawList, [scale](vec2 point) -> optional<vec2>
                                     { return point * scale; });
        }

        if (m_Merged.IsEmpty())
        {
            return;
        }

        // The projected geometry is already in target pixels, so the recorder draws it 1:1 (UiScale 1)
        // into the cleared intermediate at the scene-color region extent.
        m_Gui->SetUiScale(1.0f);
        m_Gui->SetTime(view.GuiTime);
        m_Gui->SetDrawList(m_Merged);
        m_Gui->RecordInto(ctx.Cmd(), view.PostResolveExtent);
    }

    void GuiHdrOverlayScenePass::RecordComposite(const ScenePassContext& ctx, const u32 index)
    {
        const SceneView& view = ctx.View();
        const GuiHdrOverlayView* overlay = MaterialOverlay(view, index);
        if (overlay == nullptr || overlay->Material == nullptr)
        {
            return;
        }

        CommandBuffer& cmd = ctx.Cmd();
        MaterialInstance& material = *overlay->Material;

        // Write the intermediate's live bindless slot into the material's Document field, so the
        // fullscreen fragment samples this frame's rendered document; must precede Material::Bind so
        // the pushed selector reads a param block carrying the current handle.
        material.SetTextureHandle("Document", m_DocTargetHandle);

        // The shared linear-clamp sampler goes in beside it, but only where the material declares
        // one: reading the document 1:1 needs no sampler (a .Load() by pixel coordinate), so the
        // field is what a composite that resamples — warping, offsetting, magnifying — opts in with.
        const auto hasField = [&material](std::string_view name)
        {
            const std::span<const MaterialField> fields = material.GetFields();
            return std::ranges::any_of(fields,
                                       [name](const MaterialField& f) { return f.Name == name; });
        };
        if (hasField("DocumentSampler"))
        {
            material.SetSamplerHandle("DocumentSampler", m_SamplerHandle);
        }

        const Ref<GraphicsPipeline>& pipeline = CompositePipeline(material);
        cmd.BindPipeline(pipeline);
        cmd.SetViewport({0, 0}, view.PostResolveExtent);
        cmd.SetScissor({0, 0}, view.PostResolveExtent);
        m_Context.GetBindlessRegistry().Bind(cmd);
        material.Bind(cmd);
        cmd.DrawFullscreenTriangle();
    }

    void GuiHdrOverlayScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        // The shared fullscreen sampler a composite material declaring a DocumentSampler is bound;
        // the renderer acquires it once and outlives this pass, so holding the slot is safe.
        m_SamplerHandle = io.SamplerHandle;

        // The direct path: merge every overlay WITHOUT a composite material and blend it into the scene
        // color in place. A scene of only direct overlays is byte-identical to the pre-material pass.
        graph.AddPass("Gui HDR Overlay")
            .Color({
                .Resource = m_Output,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            })
            .Execute(
                [this](PassContext& inner)
                {
                    const ScenePassContext ctx = Wrap(inner);
                    const SceneView& view = ctx.View();
                    if (view.HdrOverlays.empty())
                    {
                        return;
                    }

                    const vec2 target = vec2(view.PostResolveExtent);
                    m_Merged.Clear();
                    for (const GuiHdrOverlayView& overlay : view.HdrOverlays)
                    {
                        if (overlay.Material != nullptr || overlay.DrawList == nullptr ||
                            overlay.DrawList->IsEmpty())
                        {
                            continue;
                        }
                        if (overlay.WorldAnchored)
                        {
                            const mat4 model = overlay.Model;
                            const vec2 surfaceSize = overlay.SurfaceSize;
                            const vec2 docExtent = overlay.DocExtent;
                            const CameraView& camera = view.Camera;
                            m_Merged.AppendProjected(*overlay.DrawList,
                                                     [&](vec2 point) -> optional<vec2>
                                                     {
                                                         return ProjectGuiOverlayPoint(
                                                             point, docExtent, surfaceSize, model,
                                                             camera, target);
                                                     });
                        }
                        else
                        {
                            const vec2 scale = target / glm::max(overlay.DocExtent, vec2(1.0f));
                            m_Merged.AppendProjected(*overlay.DrawList,
                                                     [scale](vec2 point) -> optional<vec2>
                                                     { return point * scale; });
                        }
                    }

                    if (m_Merged.IsEmpty())
                    {
                        return;
                    }

                    m_Gui->SetUiScale(1.0f);
                    m_Gui->SetTime(view.GuiTime);
                    m_Gui->SetDrawList(m_Merged);
                    m_Gui->RecordInto(ctx.Cmd(), view.PostResolveExtent);
                });

        // The material path: one render-to-intermediate pass and one composite pass per material
        // overlay, in order, all reusing the single intermediate — the graph serializes the reuse. The
        // renderer recompiles when the count changes, so each material overlay has its declared pair.
        for (u32 i = 0; i < m_CompositeCount; ++i)
        {
            graph.AddPass(fmt::format("Gui HDR Overlay Doc {}", i))
                .Color({
                    .Resource = m_DocTargetId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
                })
                .Execute([this, i](PassContext& inner) { RecordDocument(Wrap(inner), i); });

            RenderGraph::PassBuilder composite =
                graph.AddPass(fmt::format("Gui HDR Overlay {}", i));
            composite.Color({
                .Resource = m_Output,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            });
            if (m_BloomMaskId.IsValid())
            {
                // The translucent pass cleared the mask; the composite loads it and adds its amplitude,
                // so a masking overlay glows alongside any translucent's contribution.
                composite.Color({
                    .Resource = m_BloomMaskId,
                    .Load = LoadOp::Load,
                    .Store = StoreOp::Store,
                });
            }
            composite.Sample(m_DocTargetId)
                .Execute([this, i](PassContext& inner) { RecordComposite(Wrap(inner), i); });
        }
    }
}
