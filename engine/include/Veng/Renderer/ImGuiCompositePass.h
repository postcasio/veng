#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/RenderGraph.h>

// The scene-output → ImGui plumbing every SceneRenderer app surfaces through ImGui.
//
// Surfacing a SceneRenderer's offscreen output through ImGui has a universal part
// (an ImGui texture over the scene output, plus an explicit pre-Render barrier —
// ImGui samples outside the render graph) and a full-screen-composite part (a
// fullscreen pass that blends the scene behind the ImGui overlay into the
// swapchain). ImGuiCompositePass owns both so a consumer hand-writes neither.
//
// Two modes, selected by ImGuiCompositePassInfo::SwapChainFormat:
//  - Composite mode (SwapChainFormat set): also owns the fullscreen composite
//    pipeline and the three bindless registrations (scene, ImGui output, sampler);
//    Compile adds the composite pass to the app's graph.
//  - Panel-only mode (SwapChainFormat == nullopt): owns only the ImGui scene
//    texture and the barrier — the scene goes inside an ImGui panel via
//    ImGui::Image. Compile is misuse and asserts.
//
// Windowed-only: it builds on the ImGui + swapchain path, never headless/smoke.
namespace Veng
{
    class AssetManager;
    class ImGuiLayer;
    class ImGuiTexture;
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class ImageView;

    struct ImGuiCompositePassInfo
    {
        Context& Context;
        ImGuiLayer& ImGui;

        // Composite mode loads its fragment shader from the engine core pack
        // through this manager; it must outlive the pass.
        AssetManager& Assets;

        // The scene output to surface — the initial SceneRenderer::GetOutput().
        // Rebound through SetSource after Resize/Configure invalidates it.
        Ref<ImageView> SceneSource;

        // Set → composite mode (the swapchain color format the composite pass
        // writes); nullopt → panel-only mode.
        optional<Format> SwapChainFormat = std::nullopt;

        // ClampToEdge so a fullscreen blit never samples garbage past the edges.
        Filter Filter = Filter::Linear;
        AddressMode Wrap = AddressMode::ClampToEdge;
    };

    class ImGuiCompositePass
    {
    public:
        static Unique<ImGuiCompositePass> Create(const ImGuiCompositePassInfo& info);
        ~ImGuiCompositePass();

        ImGuiCompositePass(const ImGuiCompositePass&) = delete;
        ImGuiCompositePass& operator=(const ImGuiCompositePass&) = delete;

        // The single re-binding call when the scene output changes (after
        // SceneRenderer::Resize/Configure recreates it). Recreates the ImGui scene
        // texture in both modes and, in composite mode, re-registers the scene
        // bindless slot. No recompile — the composite reads the scene texture's
        // bindless index live per frame, so the swap takes effect on the next replay.
        void SetSource(const Ref<ImageView>& sceneSource);

        // Add the fullscreen composite pass to `graph` writing `swapChainTarget`,
        // then Compile() it. The recompile seam on swapchain resize. Composite mode
        // only — asserts in panel-only mode.
        [[nodiscard]] Unique<CompiledGraph> Compile(RenderGraph& graph, ResourceId swapChainTarget);

        // Issue the PrepareForAccess(Sample) barrier on the scene output that must
        // precede ImGuiLayer::Render — ImGui's sampled read is outside the graph, so
        // no .Sample() declaration covers it. Both modes.
        void PrepareSceneForImGui(CommandBuffer& cmd) const;

        // The stable ImGui texture handle for the scene output — what a viewport
        // panel passes to ImGui::Image. The scene output, not the composited result
        // or the ImGui layer's own output. Both modes.
        [[nodiscard]] ImGuiTexture& GetSceneTexture() const;

        // The same scene texture as a shared reference — what a viewport panel
        // passes to UI::Image, which owns the id→ImTextureID cast. Both modes.
        [[nodiscard]] const Ref<ImGuiTexture>& GetSceneTextureRef() const;

        // Replay the compiled composite graph: bind the per-frame swapchain view plus
        // the pass-owned scene and ImGui-layer views, then execute. The single
        // per-frame call a composite-mode consumer makes after ImGuiLayer::Render.
        // Composite mode only.
        void Execute(CommandBuffer& cmd, CompiledGraph& graph, const Ref<ImageView>& swapChainView) const;

    private:
        explicit ImGuiCompositePass(const ImGuiCompositePassInfo& info);

        struct Impl;
        Unique<Impl> m_Impl;
    };
}
