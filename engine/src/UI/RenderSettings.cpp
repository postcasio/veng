#include <Veng/UI/RenderSettings.h>

#include <Veng/Renderer/Viewport.h>
#include <Veng/UI/Widgets.h>

#include <array>

namespace Veng::UI
{
    bool DrawViewStateSettings(Renderer::ViewState& state)
    {
        bool changed = false;
        changed |= Drag("Exposure", state.Exposure,
                        {.Speed = 0.01f, .Min = 0.01f, .Max = 8.0f, .Format = "%.2f"});

        // The tone-curve selector: a combo over the Tonemapper enumerators by their canonical names.
        std::array<string_view, Renderer::Tonemappers.size()> tonemapperNames;
        for (usize i = 0; i < Renderer::Tonemappers.size(); ++i)
        {
            tonemapperNames[i] = Renderer::ToString(Renderer::Tonemappers[i]);
        }
        auto tonemapperIndex = static_cast<i32>(static_cast<u32>(state.Tonemapper));
        if (Combo("Tonemapper", tonemapperIndex, tonemapperNames))
        {
            state.Tonemapper = Renderer::Tonemappers[static_cast<usize>(tonemapperIndex)];
            changed = true;
        }

        // Auto-exposure knobs; effective only when the viewport's renderer has AutoExposure on
        // (Exposure then biases the metered result). Harmless to edit otherwise.
        changed |= Drag("AE key", state.AutoExposureKey,
                        {.Speed = 0.005f, .Min = 0.01f, .Max = 1.0f, .Format = "%.3f"});
        changed |= Drag("AE min luminance", state.AutoExposureMinLuminance,
                        {.Speed = 0.005f, .Min = 0.0001f, .Max = 4.0f, .Format = "%.3f"});
        changed |= Drag("AE max luminance", state.AutoExposureMaxLuminance,
                        {.Speed = 0.02f, .Min = 0.01f, .Max = 32768.0f, .Format = "%.2f"});
        changed |= Drag("AE speed", state.AutoExposureSpeed,
                        {.Speed = 0.05f, .Min = 0.0f, .Max = 16.0f, .Format = "%.2f"});
        changed |= Drag("Bloom threshold", state.BloomThreshold,
                        {.Speed = 0.02f, .Min = 0.0f, .Max = 16.0f, .Format = "%.2f"});
        changed |= Drag("Bloom intensity", state.BloomIntensity,
                        {.Speed = 0.01f, .Min = 0.0f, .Max = 2.0f, .Format = "%.2f"});
        changed |= Drag("Bloom radius", state.BloomRadius,
                        {.Speed = 0.01f, .Min = 0.1f, .Max = 3.0f, .Format = "%.2f"});
        return changed;
    }
}
