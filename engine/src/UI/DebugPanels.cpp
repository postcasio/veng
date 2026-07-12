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

#include <Veng/Time.h>

#include <algorithm>
#include <limits>
#include <ranges>
#include <span>

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

    namespace
    {
        // One pass's GPU time after grouping: the bloom and hi-Z mip sweeps each collapse to a
        // single named entry, every other pass stays itself.
        struct PassCost
        {
            string Name;
            f32 Milliseconds = 0.0f;
        };

        // Folds a per-scope pass name onto its display group: the per-mip bloom and hi-Z passes
        // ("Bloom Down Mip 3", "HiZ Reduce Mip 2", …) collapse to "Bloom" / "Hi-Z" so the sweep
        // reads as one pass; any other name passes through unchanged.
        string PassGroup(string_view name)
        {
            if (name.starts_with("Bloom"))
            {
                return "Bloom";
            }
            if (name.starts_with("HiZ") || name.starts_with("Hi-Z"))
            {
                return "Hi-Z";
            }
            return string(name);
        }

        // Aggregates the frame's per-scope timings into grouped pass costs, summing each group's
        // contiguous mip passes and preserving first-seen (execution) order.
        vector<PassCost> AggregatePasses(std::span<const Renderer::Context::GpuPassTiming> passes)
        {
            vector<PassCost> costs;
            for (const Renderer::Context::GpuPassTiming& pass : passes)
            {
                string group = PassGroup(pass.Name);
                const auto existing = std::ranges::find(costs, group, &PassCost::Name);
                if (existing == costs.end())
                {
                    costs.push_back({.Name = std::move(group), .Milliseconds = pass.Milliseconds});
                }
                else
                {
                    existing->Milliseconds += pass.Milliseconds;
                }
            }
            return costs;
        }

        // A stable, legible color for a pass's line and legend swatch: a fixed palette indexed by
        // a name hash, so a pass keeps its color regardless of its position in the frame's order.
        vec4 PassColor(string_view name)
        {
            // A hand-tuned categorical palette: widely separated hues with alternating brightness
            // so neighbors in the legend stay distinguishable. White is reserved for the GPU total.
            static const std::array<vec4, 16> Palette{
                vec4{0.90f, 0.10f, 0.10f, 1.0f}, // red
                vec4{0.20f, 0.55f, 1.00f, 1.0f}, // blue
                vec4{1.00f, 0.85f, 0.00f, 1.0f}, // yellow
                vec4{0.55f, 0.20f, 0.90f, 1.0f}, // violet
                vec4{0.10f, 0.80f, 0.30f, 1.0f}, // green
                vec4{1.00f, 0.45f, 0.00f, 1.0f}, // orange
                vec4{0.10f, 0.85f, 0.90f, 1.0f}, // cyan
                vec4{0.95f, 0.35f, 0.70f, 1.0f}, // pink
                vec4{0.60f, 0.80f, 0.10f, 1.0f}, // lime
                vec4{0.50f, 0.35f, 0.20f, 1.0f}, // brown
                vec4{0.45f, 0.95f, 0.65f, 1.0f}, // mint
                vec4{0.80f, 0.55f, 1.00f, 1.0f}, // lavender
                vec4{0.85f, 0.65f, 0.40f, 1.0f}, // tan
                vec4{0.00f, 0.50f, 0.55f, 1.0f}, // teal
                vec4{1.00f, 0.60f, 0.55f, 1.0f}, // salmon
                vec4{0.65f, 0.75f, 0.85f, 1.0f}, // slate
            };

            // FNV-1a over the name.
            u32 hash = 2166136261u;
            for (const char c : name)
            {
                hash = (hash ^ static_cast<u8>(c)) * 16777619u;
            }
            return Palette[hash % Palette.size()];
        }

        // The whole-frame GPU line is drawn in white; the passes take the distinct palette.
        constexpr vec4 GpuLineColor{1.0f, 1.0f, 1.0f, 1.0f};
    }

    void FrameTimeGraph::History::Push(const f32 milliseconds)
    {
        Samples[Head] = milliseconds;
        Head = (Head + 1) % Capacity;
        Count = std::min(Count + 1, Capacity);
    }

    f32 FrameTimeGraph::History::Last() const
    {
        return Count == 0 ? 0.0f : Samples[(Head + Capacity - 1) % Capacity];
    }

    i32 FrameTimeGraph::History::PlotOffset() const
    {
        // A full buffer wraps, so the oldest sample sits at the write head; until then it is
        // filled from index 0 and plots in array order.
        return Count == Capacity ? static_cast<i32>(Head) : 0;
    }

    void FrameTimeGraph::Draw(const Viewport& viewport)
    {
        const Renderer::Context& context =
            viewport.GetRenderer().GetOutput()->GetImage()->GetContext();
        const bool gpuTiming = context.IsGpuTimingSupported();

        // Sample this frame's timelines into their rolling histories: the CPU frame delta the
        // engine just measured, and the GPU whole-frame + per-pass times read back from the
        // device timers (a frame or two late, which is fine for a rolling history). A pass keyed
        // by its group name keeps its own history across frames; one absent this frame simply
        // stops receiving samples until it returns.
        m_Cpu.Push(Time::GetDeltaTime() * 1000.0f);
        vector<PassCost> passes;
        if (gpuTiming)
        {
            m_Gpu.Push(context.GetLastGpuFrameTimeMs());
            passes = AggregatePasses(context.GetLastGpuPassTimings());
            for (const PassCost& pass : passes)
            {
                m_Passes[pass.Name].Push(pass.Milliseconds);
            }
        }

        if (gpuTiming)
        {
            // The whole-frame GPU envelope first (white), then each pass's history — one shared,
            // auto-scaled chart so the passes read against the frame total.
            vector<UI::PlotSeries> series;
            series.reserve(passes.size() + 1);
            series.push_back({
                .Color = GpuLineColor,
                .Values = {m_Gpu.Samples.data(), m_Gpu.Count},
                .Offset = m_Gpu.PlotOffset(),
            });
            for (const PassCost& pass : passes)
            {
                const History& history = m_Passes[pass.Name];
                series.push_back({
                    .Color = PassColor(pass.Name),
                    .Values = {history.Samples.data(), history.Count},
                    .Offset = history.PlotOffset(),
                });
            }
            UI::PlotLinesMulti("##gpu", series, {.ScaleMin = 0.0f, .Size = {0.0f, 160.0f}});

            // Two-column legend: a swatch matching each line, then its label and current cost.
            if (auto legend = UI::Table("GpuLegend", 2))
            {
                const f32 swatch = UI::GetTextLineHeight();
                UI::TableNextColumn();
                UI::Badge("", GpuLineColor, {swatch, swatch});
                UI::SameLine();
                UI::Text(fmt::format("GPU: {:.3f} ms", m_Gpu.Last()));
                for (const PassCost& pass : passes)
                {
                    UI::TableNextColumn();
                    UI::Badge("", PassColor(pass.Name), {swatch, swatch});
                    UI::SameLine();
                    UI::Text(fmt::format("{}: {:.3f} ms", pass.Name, pass.Milliseconds));
                }
            }

            UI::Spacing();
        }
        else
        {
            UI::TextDisabled("GPU timing unsupported on this device");
        }

        // The CPU whole-frame plot sits below the GPU chart, on its own fixed millisecond axis.
        f32 minimum = std::numeric_limits<f32>::max();
        f32 maximum = 0.0f;
        f32 sum = 0.0f;
        for (usize i = 0; i < m_Cpu.Count; i++)
        {
            const f32 sample = m_Cpu.Samples[i];
            minimum = std::min(minimum, sample);
            maximum = std::max(maximum, sample);
            sum += sample;
        }
        if (m_Cpu.Count == 0)
        {
            minimum = 0.0f;
        }
        const f32 average = m_Cpu.Count == 0 ? 0.0f : sum / static_cast<f32>(m_Cpu.Count);
        UI::Text(fmt::format("CPU: {:.2f} ms  (avg {:.2f}  min {:.2f}  max {:.2f})", m_Cpu.Last(),
                             average, minimum, maximum));
        const f32 scaleMax = glm::max(maximum * 1.25f, 1000.0f / 60.0f);
        UI::PlotLines("##cpuframetime", {m_Cpu.Samples.data(), m_Cpu.Count},
                      {
                          .OverlayText = fmt::format("{:.2f} ms", m_Cpu.Last()),
                          .ScaleMin = 0.0f,
                          .ScaleMax = scaleMax,
                          .Offset = m_Cpu.PlotOffset(),
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

            // The controller's tuning: an edit re-applies through SetDynamicResolution, live on the
            // next frame. Greyed out while dynamic resolution is off.
            {
                auto dynamicOff = UI::Disabled(!viewport.IsDynamicResolutionEnabled());
                Renderer::DynamicResolutionSettings settings =
                    viewport.GetDynamicResolution().value_or(Renderer::DynamicResolutionSettings{});
                bool edited = false;
                edited |= UI::Drag("Target frame time (ms)", settings.TargetFrameTimeMs,
                                   {.Speed = 0.1f, .Min = 1.0f, .Max = 100.0f, .Format = "%.2f"});
                edited |= UI::Drag("Min scale", settings.MinScale,
                                   {.Speed = 0.01f, .Min = 0.1f, .Max = 1.0f, .Format = "%.2f"});
                edited |= UI::Drag("Max scale", settings.MaxScale,
                                   {.Speed = 0.01f, .Min = 0.25f, .Max = 2.0f, .Format = "%.2f"});
                edited |= UI::Drag("Headroom", settings.Headroom,
                                   {.Speed = 0.01f, .Min = 0.0f, .Max = 0.5f, .Format = "%.2f"});
                edited |= UI::Drag("Max step", settings.MaxStep,
                                   {.Speed = 0.01f, .Min = 0.01f, .Max = 1.0f, .Format = "%.2f"});
                if (edited && viewport.IsDynamicResolutionEnabled())
                {
                    viewport.SetDynamicResolution(settings);
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

            // The fixed allocation the sub-rect renders inside — read-only facts; the allocation
            // moves only on a region/window change or an explicit scale edit, never on frame-time
            // pressure.
            const uvec2 allocExtent = viewport.GetAllocationExtent();
            UI::Text(fmt::format("Allocation: {:.2f} ({}x{})", viewport.GetAllocationScale(),
                                 allocExtent.x, allocExtent.y));
        }
    }

    bool DebugViewCombo(Renderer::DebugView& mode)
    {
        i32 index = static_cast<i32>(mode);
        if (UI::Combo("View", index, Renderer::DebugViewNames))
        {
            mode = static_cast<Renderer::DebugView>(index);
            return true;
        }
        return false;
    }

    bool RenderSettingsEditor(SceneRendererSettings& settings, Renderer::ViewState& view,
                              Viewport& viewport)
    {
        const Renderer::Context& context =
            viewport.GetRenderer().GetOutput()->GetImage()->GetContext();
        const Renderer::SceneRenderer& renderer = viewport.GetRenderer();

        // Accumulates whether any topology field changed; the caller reconfigures on a true return.
        bool changed = false;

        changed |= DebugViewCombo(settings.Mode);

        if (auto section = UI::CollapsingHeader("Lighting & effects", TreeFlags::DefaultOpen))
        {
            changed |= UI::Checkbox("SSAO", settings.AO);
            changed |= UI::Checkbox("TAA", settings.TAA);
        }

        if (auto section = UI::CollapsingHeader("Bloom", TreeFlags::DefaultOpen))
        {
            // On/off and the kernel are topology; threshold/intensity/radius are per-frame
            // ViewState values. The per-bloom knobs grey out when bloom is off.
            changed |= UI::Checkbox("Enabled##bloom", settings.Bloom);
            auto bloomDisabled = UI::Disabled(!settings.Bloom);

            static constexpr std::array<string_view, 2> kernelNames{"COD (13-tap/tent)",
                                                                    "Dual Kawase"};
            i32 kernel = static_cast<i32>(settings.Kernel);
            if (UI::Combo("Kernel", kernel, kernelNames))
            {
                settings.Kernel = static_cast<Renderer::BloomKernel>(kernel);
                changed = true;
            }

            (void)UI::Drag("Threshold##bloom", view.BloomThreshold,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 8.0f});
            (void)UI::Drag("Intensity##bloom", view.BloomIntensity,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 4.0f});
            (void)UI::Drag("Radius##bloom", view.BloomRadius,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 4.0f});
        }

        if (auto section = UI::CollapsingHeader("Screen-space reflections"))
        {
            // The toggle and the trace resolution are topology; the intensity/distance/thickness/
            // roughness knobs are per-frame ViewState values. Everything greys out when SSR is off.
            changed |= UI::Checkbox("Enabled##ssr", settings.SSR);
            auto ssrDisabled = UI::Disabled(!settings.SSR);

            static constexpr std::array<string_view, 3> ssrResolutionNames{"Full", "Half",
                                                                           "Quarter"};
            i32 ssrResolution = static_cast<i32>(settings.SsrResolutionScale);
            if (UI::Combo("Trace resolution", ssrResolution, ssrResolutionNames))
            {
                settings.SsrResolutionScale =
                    static_cast<SceneRendererSettings::SsrResolution>(ssrResolution);
                changed = true;
            }

            (void)UI::Drag("Intensity##ssr", view.SsrIntensity,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 4.0f});
            (void)UI::Drag("Max distance##ssr", view.SsrMaxDistance,
                           {.Speed = 0.1f, .Min = 0.0f, .Max = 200.0f});
            (void)UI::Drag("Thickness##ssr", view.SsrThickness,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 4.0f});
            (void)UI::Slider("Max roughness##ssr", view.SsrMaxRoughness,
                             {.Min = 0.0f, .Max = 1.0f});
        }

        if (auto section = UI::CollapsingHeader("Shadows", TreeFlags::DefaultOpen))
        {
            changed |= UI::Checkbox("Directional shadows", settings.Shadows);
            {
                auto shadowsDisabled = UI::Disabled(!settings.Shadows);

                i32 cascadeCount = static_cast<i32>(settings.CascadeCount);
                if (UI::Slider("Cascades##count", cascadeCount, 1,
                               static_cast<i32>(Renderer::MaxCascades)))
                {
                    settings.CascadeCount = static_cast<u32>(cascadeCount);
                    changed = true;
                }

                i32 shadowResolution = static_cast<i32>(settings.ShadowResolution);
                if (UI::Drag("Resolution##shadow", shadowResolution,
                             {.Speed = 16.0f,
                              .Min = 256.0f,
                              .Max = static_cast<f32>(renderer.GetMaxShadowResolution())}))
                {
                    settings.ShadowResolution = static_cast<u32>(shadowResolution);
                    changed = true;
                }

                if (UI::Slider("Split lambda", settings.CascadeSplitLambda,
                               {.Min = 0.0f, .Max = 1.0f}))
                {
                    changed = true;
                }

                if (UI::Drag("Max distance##shadow", settings.MaxShadowDistance,
                             {.Speed = 1.0f, .Min = 0.0f, .Max = 10000.0f}))
                {
                    changed = true;
                }
            }

            changed |= UI::Checkbox("Punctual shadows", settings.PunctualShadows);
            {
                auto punctualDisabled = UI::Disabled(!settings.PunctualShadows);

                i32 punctualResolution = static_cast<i32>(settings.PunctualShadowResolution);
                if (UI::Drag("Resolution##punctual", punctualResolution,
                             {.Speed = 16.0f,
                              .Min = 256.0f,
                              .Max = static_cast<f32>(renderer.GetMaxPunctualShadowResolution())}))
                {
                    settings.PunctualShadowResolution = static_cast<u32>(punctualResolution);
                    changed = true;
                }
            }
        }

        if (auto section = UI::CollapsingHeader("Exposure", TreeFlags::DefaultOpen))
        {
            // Exposure and the auto-exposure tuning are per-frame ViewState values; the metering
            // toggle is topology (it inserts the histogram compute pass). With metering on,
            // Exposure biases the adapted value; the tuning knobs grey out with it off. The drag's
            // id is suffixed apart from the section header's (CollapsingHeader pushes no id scope).
            (void)UI::Drag("Exposure##value", view.Exposure,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 16.0f});
            changed |= UI::Checkbox("Auto exposure", settings.AutoExposure);
            auto meteringDisabled = UI::Disabled(!settings.AutoExposure);
            (void)UI::Drag("Key", view.AutoExposureKey,
                           {.Speed = 0.005f, .Min = 0.01f, .Max = 1.0f});
            (void)UI::Drag("Min luminance", view.AutoExposureMinLuminance,
                           {.Speed = 0.001f, .Min = 0.0f, .Max = 8.0f, .Format = "%.4f"});
            (void)UI::Drag("Max luminance", view.AutoExposureMaxLuminance,
                           {.Speed = 1.0f, .Min = 0.01f, .Max = 10000.0f});
            (void)UI::Drag("Adaptation speed", view.AutoExposureSpeed,
                           {.Speed = 0.05f, .Min = 0.0f, .Max = 20.0f});
        }

        // The sky is the scene's Sky component, resolved by the renderer itself — not a topology
        // toggle or a per-frame ViewState value — so it is authored and edited in the inspector,
        // not here.

        if (auto section = UI::CollapsingHeader("Culling"))
        {
            changed |= UI::Checkbox("Frustum culling", settings.FrustumCull);

            // The GPU arm is a different pass topology, so the selector and the occlusion toggle
            // both drive a recompile. The selector greys out where the device cannot honor GPU
            // (it would silently degrade to CPU); the stats panel shows the active path.
            {
                auto gpuUnsupported = UI::Disabled(!context.IsGpuDrivenCullingSupported());
                static constexpr std::array<string_view, 2> cullNames{"CPU", "GPU"};
                i32 cull = static_cast<i32>(settings.Cull);
                if (UI::Combo("Cull mode", cull, cullNames))
                {
                    settings.Cull = static_cast<SceneRendererSettings::CullMode>(cull);
                    changed = true;
                }

                auto cpuPath = UI::Disabled(settings.Cull != SceneRendererSettings::CullMode::GPU);
                changed |= UI::Checkbox("GPU occlusion", settings.Occlusion);
            }
        }

        if (auto section = UI::CollapsingHeader("Resolution"))
        {
            // Adaptive resolution and the manual render-scale override drive the viewport
            // imperatively — they recreate or resize renderer resources directly, not through the
            // Configure recompile this function reports.
            DrawResolutionControls(viewport, context);
        }

        if (auto section = UI::CollapsingHeader("Authoring"))
        {
            // The immediate-mode debug-draw flush pass and the entity-id picking pass, both off
            // by default — authoring aids a consumer enables for a viewport's lifetime.
            changed |= UI::Checkbox("Debug draw", settings.DebugDraw);
            changed |= UI::Checkbox("Picking", settings.Picking);
        }

        return changed;
    }
}
