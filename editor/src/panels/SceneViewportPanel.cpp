#include "panels/SceneViewportPanel.h"

#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Time.h>
#include <Veng/UI/UI.h>

#include <array>

namespace VengEditor
{
    using namespace Veng;

    SceneViewportPanel::SceneViewportPanel(Renderer::Context& context, AssetManager& assets,
                                           ImGuiLayer& imgui, PrefabEditContext& ctx)
        : m_Context(context), m_Assets(assets), m_ImGui(imgui), m_Ctx(ctx)
    {
        m_RenderExtent = context.GetInternalRenderExtent();

        m_SceneRenderer = Renderer::SceneRenderer::Create({
            .Context = context,
            .Assets = assets,
            .OutputFormat = context.GetOutputFormat(),
            .Extent = m_RenderExtent,
            .Settings = m_Settings,
        });

        UpdateCamera();

        // Edge clamping prevents sampling past the image boundary when the panel size
        // does not align to a texel.
        m_SceneSampler = Renderer::Sampler::Create(
            context, {
                         .Name = "Scene Viewport Sampler",
                         .AddressModeU = Renderer::AddressMode::ClampToEdge,
                         .AddressModeV = Renderer::AddressMode::ClampToEdge,
                         .AddressModeW = Renderer::AddressMode::ClampToEdge,
                     });
        m_SceneTexture = imgui.CreateTexture(*m_SceneSampler, *m_SceneRenderer->GetOutput());
    }

    SceneViewportPanel::~SceneViewportPanel()
    {
        // Dropping the cache's handles retires the streamed primitive meshes.
        m_PrimitiveCache.Entries.clear();
        m_SceneTexture.reset();
        m_SceneSampler.reset();
        m_SceneRenderer.reset();
    }

    void SceneViewportPanel::UpdateCamera()
    {
        const f32 aspect = static_cast<f32>(m_RenderExtent.x) / static_cast<f32>(m_RenderExtent.y);
        m_Camera.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);

        const f32 cp = glm::cos(m_Pitch);
        const vec3 offset{m_Distance * cp * glm::sin(m_Yaw), m_Distance * glm::sin(m_Pitch),
                          m_Distance * cp * glm::cos(m_Yaw)};
        m_Camera.SetView(m_Target + offset, m_Target, vec3(0.0f, 1.0f, 0.0f));
    }

    void SceneViewportPanel::OnRender(Renderer::CommandBuffer& cmd)
    {
        if (m_Ctx.Scene == nullptr)
        {
            return;
        }

        // Resize and Configure both invalidate GetOutput(); rebind the ImGui texture
        // once after both are applied, before recording.
        bool outputInvalidated = false;

        if (m_PendingExtent.x != 0 && m_PendingExtent.y != 0 && m_PendingExtent != m_RenderExtent)
        {
            m_RenderExtent = m_PendingExtent;
            m_SceneRenderer->Resize(m_RenderExtent);
            UpdateCamera();
            outputInvalidated = true;
        }

        if (m_SettingsDirty)
        {
            m_SceneRenderer->Configure(m_Settings);
            m_SettingsDirty = false;
            outputInvalidated = true;
        }

        if (outputInvalidated)
        {
            m_SceneTexture = m_ImGui.CreateTexture(*m_SceneSampler, *m_SceneRenderer->GetOutput());
        }

        // Re-resolve every frame so editing a PrimitiveComponent's shape streams in the new
        // mesh; idempotent for unedited shapes, deduped through the retained cache.
        ResolvePrimitiveMeshes(*m_Ctx.Scene, m_Assets, m_PrimitiveCache);

        const Renderer::SceneView view{
            .World = *m_Ctx.Scene, .Camera = m_Camera, .Delta = Time::GetDeltaTime()};
        m_SceneRenderer->Execute(cmd, view);

        // ImGui's sampled read of the output is recorded outside the graph by
        // ImGuiLayer::Render, so transition the output to a sampleable layout here.
        cmd.PrepareForAccess(m_SceneRenderer->GetOutput(), Renderer::AccessKind::Sample);
    }

    void SceneViewportPanel::OnImGui()
    {
        // Combo index is the DebugView enum value (declaration order). The change is
        // deferred via m_SettingsDirty so Configure runs in OnRender, not mid-ImGui.
        static constexpr std::array<string_view, 10> modeNames{
            "Final",    "Albedo",    "Normal", "Depth",   "Roughness",
            "Metallic", "Occlusion", "AO",     "Shadows", "Cascades"};
        i32 mode = static_cast<i32>(m_Settings.Mode);
        if (UI::Combo("Debug View", mode, modeNames))
        {
            m_Settings.Mode = static_cast<Renderer::DebugView>(mode);
            m_SettingsDirty = true;
        }

        const vec2 available = UI::ContentRegionAvail();
        const uvec2 wanted{static_cast<u32>(available.x), static_cast<u32>(available.y)};
        m_PendingExtent = wanted;

        UI::Image(m_SceneTexture, available);
    }
}
