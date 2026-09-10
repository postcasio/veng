#include "GuiHdrOverlayScenePass.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/Camera.h>

#include "GuiScenePass.h"
#include "../GuiOverlayProjection.h"

namespace Veng::Renderer
{
    GuiHdrOverlayScenePass::GuiHdrOverlayScenePass(Context& context, AssetManager& assets,
                                                   Format outputFormat, uvec2 extent)
        : m_Extent(extent)
    {
        // The recorder is driven only through RecordInto, so its composite images stay unbuilt (the
        // lazy path in GuiScenePass); it supplies the geometry rings, the shape/msdf/material
        // pipelines, and the premultiplied-over HDR blend this pass reuses.
        m_Gui = GuiScenePass::Create({
            .Context = context,
            .Assets = assets,
            .Extent = extent,
            .OutputFormat = outputFormat,
        });
    }

    GuiHdrOverlayScenePass::~GuiHdrOverlayScenePass() = default;

    void GuiHdrOverlayScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        // Blend the overlays into the scene color in place: load the resolved scene-color target
        // (the effect chain's output, or the raw HDR) and store it back, so bloom — which reads the
        // same id — sees the overlay composited over the scene.
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

                    // Merge every conveyed overlay into one screen-space draw list: a screen-space
                    // overlay scales its logical extent to fill the scene-color region; a
                    // world-anchored one projects each vertex through the live camera onto its plane.
                    // Both land in the same PostResolveExtent pixel space, so one record draws them.
                    const vec2 target = vec2(view.PostResolveExtent);
                    m_Merged.Clear();
                    for (const GuiHdrOverlayView& overlay : view.HdrOverlays)
                    {
                        if (overlay.DrawList == nullptr || overlay.DrawList->IsEmpty())
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
                            // Fill the scene-color region: the document laid out at DocExtent points
                            // scales to the target pixels, so it fills the viewport regardless of the
                            // sub-native pre-bloom extent (the tonemap upscales it to output).
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

                    // The merged geometry is already in target pixels, so the recorder draws it 1:1
                    // (UiScale 1) over the loaded scene color at the scene-color region extent.
                    m_Gui->SetUiScale(1.0f);
                    m_Gui->SetTime(view.GuiTime);
                    m_Gui->SetDrawList(m_Merged);
                    m_Gui->RecordInto(ctx.Cmd(), view.PostResolveExtent);
                });
    }
}
