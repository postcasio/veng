#include <Veng/UI/DebugPanels.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/UI/Layout.h>
#include <Veng/UI/Query.h>
#include <Veng/UI/Scopes.h>
#include <Veng/UI/Widgets.h>

#include <algorithm>
#include <limits>

namespace Veng::UI
{
    using Renderer::SceneRendererSettings;
    using Renderer::Viewport;

    void RendererStatsPanel(const Viewport& viewport)
    {
        const Renderer::SceneRenderer& renderer = viewport.GetRenderer();
        const Renderer::Context& context = renderer.GetOutput()->GetImage()->GetContext();

        UI::Text(fmt::format("{:.1f} fps ({:.2f} ms)", UI::FrameRate(), 1000.0f / UI::FrameRate()));

        if (context.IsGpuTimingSupported())
        {
            UI::Text(fmt::format("GPU frame: {:.2f} ms", context.GetLastGpuFrameTimeMs()));
        }

        // The live sub-rect scale (the inner loop writes it while dynamic resolution is on) and
        // the rendered extent, which shrinks with the scale while the window stays full size.
        UI::Text(fmt::format("Render scale: {:.2f}{}", viewport.GetRenderScale(),
                             viewport.IsDynamicResolutionEnabled() ? " (auto)" : ""));
        const Ref<Renderer::Image> target = renderer.GetOutput()->GetImage();
        UI::Text(fmt::format("Render target: {}x{}", target->GetWidth(), target->GetHeight()));

        // The fixed allocation the targets are sized at: its scale and extent. The per-frame
        // sub-rect renders inside this and the tonemap upscales it to the full output, so the
        // allocation extent holds steady while the rendered sub-rect shrinks with the scale.
        const uvec2 allocExtent = viewport.GetAllocationExtent();
        UI::Text(fmt::format("Allocation: {:.2f}", viewport.GetAllocationScale()));
        UI::Text(fmt::format("Allocation extent: {}x{}", allocExtent.x, allocExtent.y));

        // The cull funnel: gathered submesh candidates → frustum survivors → draws issued.
        UI::Text(fmt::format("Gathered: {}", renderer.GetLastVisibleCount()));
        UI::Text(fmt::format("Frustum survived: {}", renderer.GetFrustumSurvivedCount()));
        UI::Text(fmt::format("Drawn: {}", renderer.GetLastDrawnCount()));

        // Under the GPU path the occlusion test zeroes occluded commands' instanceCount; the
        // survivor count is read back one frame late. The active line shows the real mode (GPU
        // degrades to CPU on a device without multiDrawIndirect).
        const bool gpuActive = renderer.GetActiveCullMode() == SceneRendererSettings::CullMode::GPU;
        UI::Text(fmt::format("Cull mode: {}", gpuActive ? "GPU" : "CPU"));
        if (gpuActive)
        {
            UI::Text(fmt::format("Occlusion survived: {}", renderer.GetLastGpuSurvivorCount()));
        }

        const bool rebuilt = renderer.DidBroadphaseRebuildLastFrame();
        UI::Text(fmt::format("Broadphase: {} ({} nodes)", rebuilt ? "rebuilt" : "static",
                             renderer.GetBroadphaseNodeCount()));
    }

    void FrameTimeGraph::Push(const f32 milliseconds)
    {
        m_Samples[m_Head] = milliseconds;
        m_Head = (m_Head + 1) % Capacity;
        m_Count = std::min(m_Count + 1, Capacity);
    }

    void FrameTimeGraph::Draw(const Viewport& viewport)
    {
        const Renderer::Context& context =
            viewport.GetRenderer().GetOutput()->GetImage()->GetContext();
        if (!context.IsGpuTimingSupported())
        {
            UI::TextDisabled("GPU timing unsupported on this device");
            return;
        }

        Push(context.GetLastGpuFrameTimeMs());
        Draw();
    }

    void FrameTimeGraph::Draw()
    {
        // Reduce the valid samples to min/max/average. The buffer fills from index 0 before it
        // wraps, so [0, m_Count) is always the populated set regardless of m_Head.
        f32 minimum = std::numeric_limits<f32>::max();
        f32 maximum = 0.0f;
        f32 sum = 0.0f;
        for (usize i = 0; i < m_Count; i++)
        {
            const f32 sample = m_Samples[i];
            minimum = std::min(minimum, sample);
            maximum = std::max(maximum, sample);
            sum += sample;
        }
        if (m_Count == 0)
        {
            minimum = 0.0f;
        }
        const f32 average = m_Count == 0 ? 0.0f : sum / static_cast<f32>(m_Count);
        const f32 last = m_Count == 0 ? 0.0f : m_Samples[(m_Head + Capacity - 1) % Capacity];

        UI::Text(fmt::format("GPU: {:.2f} ms  (avg {:.2f}  min {:.2f}  max {:.2f})", last, average,
                             minimum, maximum));

        // A full buffer wraps, so the oldest sample sits at the write head; until then it is
        // filled from index 0 and plots in array order. The axis is pinned so a line's height
        // reads as absolute milliseconds.
        const i32 offset = m_Count == Capacity ? static_cast<i32>(m_Head) : 0;
        const f32 scaleMax = glm::max(maximum * 1.25f, 1000.0f / 60.0f);
        UI::PlotLines("##gpuframetime", {m_Samples.data(), m_Count},
                      {
                          .OverlayText = fmt::format("{:.2f} ms", last),
                          .ScaleMin = 0.0f,
                          .ScaleMax = scaleMax,
                          .Offset = offset,
                          .Size = {0.0f, 80.0f},
                      });
    }

    namespace
    {
        // Draws the viewport's adaptive-resolution and manual render-scale controls. These drive
        // the viewport imperatively (SetDynamicResolution/ClearDynamicResolution/SetRenderScale),
        // recreating or resizing renderer resources directly rather than through the Configure
        // recompile RenderSettingsEditor reports. The dynamic-resolution checkbox greys out without
        // device timestamp support.
        void DrawResolutionControls(Viewport& viewport, const Renderer::Context& context)
        {
            bool dynamic = viewport.IsDynamicResolutionEnabled();
            {
                auto timingDisabled = UI::Disabled(!context.IsGpuTimingSupported());
                if (UI::Checkbox("Dynamic resolution", dynamic))
                {
                    if (dynamic)
                    {
                        viewport.SetDynamicResolution(Renderer::DynamicResolutionSettings{});
                    }
                    else
                    {
                        viewport.ClearDynamicResolution();
                    }
                }
            }

            // Render scale is a per-viewport property. While dynamic resolution is on it reads out
            // the live sub-rect scale; touching it is the manual override — it drops dynamic
            // resolution and holds the value, sizing the render target directly. Steps by 0.05 from
            // a 0.25 floor.
            f32 renderScale = viewport.GetRenderScale();
            if (UI::Drag("Render scale", renderScale,
                         {.Speed = 0.05f, .Min = 0.25f, .Max = 2.0f, .Format = "%.2f"}))
            {
                renderScale = glm::clamp(glm::round(renderScale / 0.05f) * 0.05f, 0.25f, 2.0f);
                if (viewport.IsDynamicResolutionEnabled())
                {
                    viewport.ClearDynamicResolution();
                }
                viewport.SetRenderScale(renderScale);
            }
        }
    }

    bool RenderSettingsEditor(SceneRendererSettings& settings, Renderer::ViewState& view,
                              Viewport& viewport)
    {
        const Renderer::Context& context =
            viewport.GetRenderer().GetOutput()->GetImage()->GetContext();
        const Renderer::SceneRenderer& renderer = viewport.GetRenderer();

        // Accumulates whether any topology field changed; the caller reconfigures on a true return.
        bool changed = false;

        // Entries mirror the DebugView enum in declaration order; combo index == enum value.
        static constexpr std::array<string_view, 15> modeNames{
            "Final",          "Albedo",      "Normal",  "Depth",    "Roughness",        "Metallic",
            "Occlusion",      "AO",          "Shadows", "Cascades", "Punctual shadows", "Bloom",
            "Motion vectors", "Reflections", "Emissive"};
        i32 mode = static_cast<i32>(settings.Mode);
        if (UI::Combo("View", mode, modeNames))
        {
            settings.Mode = static_cast<Renderer::DebugView>(mode);
            changed = true;
        }

        changed |= UI::Checkbox("SSAO", settings.AO);
        changed |= UI::Checkbox("TAA", settings.TAA);
        changed |= UI::Checkbox("SSR", settings.SSR);
        {
            // The trace/min-Z/blur resolution sizes the SSR working set; the combo greys out
            // when SSR is off.
            auto ssrDisabled = UI::Disabled(!settings.SSR);
            static constexpr std::array<string_view, 3> ssrResolutionNames{"Full", "Half",
                                                                           "Quarter"};
            i32 ssrResolution = static_cast<i32>(settings.SsrResolutionScale);
            if (UI::Combo("SSR resolution", ssrResolution, ssrResolutionNames))
            {
                settings.SsrResolutionScale =
                    static_cast<SceneRendererSettings::SsrResolution>(ssrResolution);
                changed = true;
            }
        }

        changed |= UI::Checkbox("Shadows", settings.Shadows);

        i32 cascadeCount = static_cast<i32>(settings.CascadeCount);
        if (UI::Slider("Cascades##count", cascadeCount, 1, static_cast<i32>(Renderer::MaxCascades)))
        {
            settings.CascadeCount = static_cast<u32>(cascadeCount);
            changed = true;
        }

        i32 shadowResolution = static_cast<i32>(settings.ShadowResolution);
        if (UI::Drag("Shadow resolution", shadowResolution,
                     {.Speed = 16.0f,
                      .Min = 256.0f,
                      .Max = static_cast<f32>(renderer.GetMaxShadowResolution())}))
        {
            settings.ShadowResolution = static_cast<u32>(shadowResolution);
            changed = true;
        }

        if (UI::Slider("Split lambda", settings.CascadeSplitLambda, {.Min = 0.0f, .Max = 1.0f}))
        {
            changed = true;
        }

        changed |= UI::Checkbox("Punctual shadows", settings.PunctualShadows);

        i32 punctualResolution = static_cast<i32>(settings.PunctualShadowResolution);
        if (UI::Drag("Punctual shadow resolution", punctualResolution,
                     {.Speed = 16.0f,
                      .Min = 256.0f,
                      .Max = static_cast<f32>(renderer.GetMaxPunctualShadowResolution())}))
        {
            settings.PunctualShadowResolution = static_cast<u32>(punctualResolution);
            changed = true;
        }

        changed |= UI::Checkbox("Frustum culling", settings.FrustumCull);

        // The GPU arm is a different pass topology, so the selector and the occlusion toggle both
        // drive a recompile. CullMode::GPU degrades to CPU on a device without multiDrawIndirect;
        // the active-mode line in the stats panel shows the real path.
        static constexpr std::array<string_view, 2> cullNames{"CPU", "GPU"};
        i32 cull = static_cast<i32>(settings.Cull);
        if (UI::Combo("Cull mode", cull, cullNames))
        {
            settings.Cull = static_cast<SceneRendererSettings::CullMode>(cull);
            changed = true;
        }

        changed |= UI::Checkbox("GPU occlusion", settings.Occlusion);

        // The immediate-mode debug-draw flush pass (lines + billboards), off by default.
        changed |= UI::Checkbox("Debug draw", settings.DebugDraw);

        // Adaptive resolution and the manual render-scale override drive the viewport
        // imperatively — they recreate or resize renderer resources directly, not through the
        // Configure recompile this function reports.
        DrawResolutionControls(viewport, context);

        // Tonemap exposure is a per-frame ViewState value; the drag edits it in place and rides
        // the next push with no reconfigure.
        (void)UI::Drag("Exposure", view.Exposure, {.Speed = 0.01f, .Min = 0.0f, .Max = 16.0f});

        // Skybox is a topology toggle (its own pass); environment intensity is a per-frame value.
        changed |= UI::Checkbox("Skybox", settings.Skybox);
        (void)UI::Drag("Env Intensity", view.EnvironmentIntensity,
                       {.Speed = 0.01f, .Min = 0.0f, .Max = 8.0f});

        // The procedural atmosphere is a topology toggle (its own sky pass); the per-frame enable
        // and the sun direction ride the ViewState. The sun elevation/azimuth drag drives the
        // day/night look with no precompute — only the LUT-affecting Atmosphere fields regenerate.
        changed |= UI::Checkbox("Atmosphere", settings.Atmosphere);
        {
            auto atmosphereDisabled = UI::Disabled(!settings.Atmosphere);
            (void)UI::Checkbox("Atmosphere sky", view.AtmosphereEnabled);
            if (UI::Drag("Sun direction", view.SunDirection, {.Speed = 0.01f}))
            {
                const f32 length = glm::length(view.SunDirection);
                view.SunDirection =
                    length > 1e-4f ? view.SunDirection / length : vec3(0.0f, 1.0f, 0.0f);
            }
        }

        // The dynamic SH skylight is a topology toggle (the lighting pass's three-way ambient
        // branch); its intensity is a per-frame ViewState value. It projects the same Atmosphere
        // sky into SH, so the sun direction above drives it too. The intensity greys out when off.
        changed |= UI::Checkbox("Skylight", settings.Skylight);
        {
            auto skylightDisabled = UI::Disabled(!settings.Skylight);
            (void)UI::Drag("Skylight intensity", view.SkylightIntensity,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 8.0f});
        }

        // Bloom on/off and the kernel are topology; threshold/intensity/radius are per-frame
        // ViewState values. The per-bloom knobs grey out when bloom is off.
        changed |= UI::Checkbox("Bloom", settings.Bloom);
        {
            auto bloomDisabled = UI::Disabled(!settings.Bloom);
            (void)UI::Drag("Bloom threshold", view.BloomThreshold,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 8.0f});
            (void)UI::Drag("Bloom intensity", view.BloomIntensity,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 4.0f});
            (void)UI::Drag("Bloom radius", view.BloomRadius,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 4.0f});

            static constexpr std::array<string_view, 2> kernelNames{"COD (13-tap/tent)",
                                                                    "Dual Kawase"};
            i32 kernel = static_cast<i32>(settings.Kernel);
            if (UI::Combo("Bloom kernel", kernel, kernelNames))
            {
                settings.Kernel = static_cast<Renderer::BloomKernel>(kernel);
                changed = true;
            }
        }

        return changed;
    }
}
