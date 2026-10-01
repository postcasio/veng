#include "GuiHdrOverlayScenePass.h"

#include <limits>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Log.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Scene/Camera.h>

#include "GuiScenePass.h"

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
        : m_Context(context), m_Assets(assets), m_Extent(extent), m_OutputFormat(outputFormat)
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

    void GuiHdrOverlayScenePass::SetComposite(ResourceId docTarget, ResourceId bloomMask,
                                              Format bloomMaskFormat)
    {
        m_DocTargetId = docTarget;
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

    bool GuiHdrOverlayScenePass::AppendOverlay(Gui::DrawList& into,
                                               const GuiHdrOverlayView& overlay,
                                               const SceneView& view)
    {
        // A screen-space overlay scales its logical extent to the scene-color region; a
        // world-anchored one projects through the live camera onto its plane.
        const vec2 target = vec2(view.PostResolveExtent);
        if (overlay.WorldAnchored)
        {
            const mat4 model = overlay.Model;
            const vec2 surfaceSize = overlay.SurfaceSize;
            const vec2 docExtent = overlay.DocExtent;
            const CameraView& camera = view.Camera;
            return into.AppendProjected(*overlay.DrawList,
                                        [&](vec2 point) -> optional<vec2>
                                        {
                                            return ProjectGuiOverlayPoint(point, docExtent,
                                                                          surfaceSize, model,
                                                                          camera, target);
                                        });
        }
        const vec2 scale = target / glm::max(overlay.DocExtent, vec2(1.0f));
        return into.AppendProjected(*overlay.DrawList, [scale](vec2 point) -> optional<vec2>
                                    { return point * scale; });
    }

    uvec2 GuiHdrOverlayScenePass::PrepareDocuments(const SceneView& view)
    {
        m_DocLists.resize(m_CompositeCount);
        m_DocRects.assign(m_CompositeCount, GuiOverlayDocumentRect{});

        uvec2 required{0, 0};
        for (u32 i = 0; i < m_CompositeCount; ++i)
        {
            Gui::DrawList& projected = m_DocLists[i];
            projected.Clear();
            const GuiHdrOverlayView* overlay = MaterialOverlay(view, i);
            if (overlay == nullptr || overlay->DrawList == nullptr ||
                overlay->DrawList->IsEmpty() || !AppendOverlay(projected, *overlay, view) ||
                projected.IsEmpty())
            {
                continue;
            }

            // Fragments exist only inside the projected triangles, so the vertices' bounds hold
            // every pixel the document can cover.
            vec2 lo(std::numeric_limits<f32>::max());
            vec2 hi(std::numeric_limits<f32>::lowest());
            for (const Gui::GuiVertex& vertex : projected.GetVertices())
            {
                lo = glm::min(lo, vertex.Position);
                hi = glm::max(hi, vertex.Position);
            }
            m_DocRects[i] = ComputeGuiOverlayDocumentRect(lo, hi, view.PostResolveExtent);
            required = glm::max(required, m_DocRects[i].Size);
        }
        return required;
    }

    void GuiHdrOverlayScenePass::RecordDocument(const ScenePassContext& ctx, const u32 index)
    {
        if (index >= m_DocRects.size() || m_DocRects[index].IsEmpty())
        {
            return;
        }
        const GuiOverlayDocumentRect& rect = m_DocRects[index];
        VE_ASSERT(rect.Size.x <= m_DocTargetExtent.x && rect.Size.y <= m_DocTargetExtent.y,
                  "GuiHdrOverlayScenePass: document rect {}x{} exceeds the {}x{} intermediate",
                  rect.Size.x, rect.Size.y, m_DocTargetExtent.x, m_DocTargetExtent.y);

        // The document was projected into target pixels ahead of the graph; shifting it by the
        // rect's origin lands the rect at the intermediate's origin, and the record draws only the
        // rect's own extent of the intermediate.
        const vec2 origin = vec2(rect.Origin);
        m_Merged.Clear();
        m_Merged.AppendProjected(m_DocLists[index],
                                 [origin](vec2 point) -> optional<vec2> { return point - origin; });

        // The projected geometry is already in target pixels, so the recorder draws it 1:1 (UiScale 1).
        m_Gui->SetUiScale(1.0f);
        m_Gui->SetTime(ctx.View().GuiTime);
        m_Gui->SetDrawList(m_Merged);
        m_Gui->RecordInto(ctx.Cmd(), rect.Size);
    }

    void GuiHdrOverlayScenePass::RecordComposite(const ScenePassContext& ctx, const u32 index)
    {
        const SceneView& view = ctx.View();
        const GuiHdrOverlayView* overlay = MaterialOverlay(view, index);
        if (overlay == nullptr || overlay->Material == nullptr || index >= m_DocRects.size() ||
            m_DocRects[index].IsEmpty())
        {
            return;
        }
        const GuiOverlayDocumentRect& rect = m_DocRects[index];

        // The intermediate holds only the document rect, so a material reading it by scene pixel
        // would read the wrong texels; one that cannot be told where the rect lies is not drawn.
        const MaterialFieldHandle rectField = overlay->Material->Field("DocumentRect");
        if (!rectField.IsValid())
        {
            if (m_ReportedWithoutRect.insert(overlay->Material->GetParent().Get()).second)
            {
                Log::Error("GuiHdrOverlayScenePass: composite material '{}' declares no "
                           "'DocumentRect' field (float4: origin, size); the overlay is not "
                           "composited. Read the document through LoadOverlayDocument "
                           "(Veng/overlay_composite.slang).",
                           overlay->Material->GetName());
            }
            return;
        }

        CommandBuffer& cmd = ctx.Cmd();

        // The intermediate is this renderer's own, so its slot is a per-view value and a material's
        // block rings by frame-in-flight rather than by view: written into the overlay's shared
        // instance it would hand every viewport the last one recorded. The composite draws through
        // this renderer's mirror instead.
        if (index >= m_ViewMaterials.size())
        {
            m_ViewMaterials.resize(index + 1);
        }
        MaterialInstance& material =
            m_ViewMaterials[index].Resolve(m_Assets, m_Context, *overlay->Material,
                                           fmt::format("{} (View)", overlay->Material->GetName()));

        // Write the intermediate's live bindless slot and where the document sits in it; must
        // precede Material::Bind so the pushed selector reads a param block carrying both.
        material.SetTextureHandle("Document", m_DocTargetHandle);
        material.SetParam(rectField, vec4(vec2(rect.Origin), vec2(rect.Size)));

        // The viewport stays the full target so sv_position is a scene pixel; the scissor keeps the
        // fragment to the pixels the document can cover.
        const Ref<GraphicsPipeline>& pipeline = CompositePipeline(material);
        cmd.BindPipeline(pipeline);
        cmd.SetViewport({0, 0}, view.PostResolveExtent);
        cmd.SetScissor(ivec2(rect.Origin), rect.Size);
        m_Context.GetBindlessRegistry().Bind(cmd);
        material.Bind(cmd);
        cmd.DrawFullscreenTriangle();
    }

    void GuiHdrOverlayScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        // The direct path: merge every overlay WITHOUT a composite material and blend it into the scene
        // color in place. Declared only while such an overlay is conveyed.
        if (m_HasDirect)
        {
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
                        m_Merged.Clear();
                        for (const GuiHdrOverlayView& overlay : view.HdrOverlays)
                        {
                            if (overlay.Material != nullptr || overlay.DrawList == nullptr ||
                                overlay.DrawList->IsEmpty())
                            {
                                continue;
                            }
                            AppendOverlay(m_Merged, overlay, view);
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
        }

        // The material path: one render-to-intermediate pass and one composite pass per material
        // overlay, in order, all reusing the single intermediate — the graph serializes the reuse. The
        // renderer recompiles when the count changes, so each material overlay has its declared pair.
        // The intermediate is sized to the largest document rect, so its clear and store cost what
        // the documents cover.
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
                // The promotion (or, unpromoted, the translucent pass's clear) has filled every
                // texel of the mask; the composite loads it and adds its amplitude, so a masking
                // overlay glows alongside any translucent's contribution.
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
