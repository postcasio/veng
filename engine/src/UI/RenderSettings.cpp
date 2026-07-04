#include <Veng/UI/RenderSettings.h>

#include <Veng/Renderer/Viewport.h>
#include <Veng/UI/Widgets.h>

namespace Veng::UI
{
    bool DrawViewStateSettings(Renderer::ViewState& state)
    {
        bool changed = false;
        changed |= Drag("Exposure", state.Exposure,
                        {.Speed = 0.01f, .Min = 0.01f, .Max = 8.0f, .Format = "%.2f"});
        // Auto-exposure knobs; effective only when the viewport's renderer has AutoExposure on
        // (Exposure then biases the metered result). Harmless to edit otherwise.
        changed |= Drag("AE key", state.AutoExposureKey,
                        {.Speed = 0.005f, .Min = 0.01f, .Max = 1.0f, .Format = "%.3f"});
        changed |= Drag("AE min luminance", state.AutoExposureMinLuminance,
                        {.Speed = 0.005f, .Min = 0.0001f, .Max = 4.0f, .Format = "%.3f"});
        changed |= Drag("AE max luminance", state.AutoExposureMaxLuminance,
                        {.Speed = 0.02f, .Min = 0.01f, .Max = 32.0f, .Format = "%.2f"});
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
