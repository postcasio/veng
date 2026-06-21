#include <Veng/UI/Theme.h>

#include <cmath>

namespace Veng::UI
{
    namespace
    {
        /// @brief Builds an opaque color from an `0xRRGGBB` hex triple.
        constexpr vec4 Rgb(u32 hex)
        {
            return vec4(static_cast<f32>((hex >> 16) & 0xFF) / 255.0f,
                        static_cast<f32>((hex >> 8) & 0xFF) / 255.0f,
                        static_cast<f32>(hex & 0xFF) / 255.0f, 1.0f);
        }

        /// @brief Builds a color from an `0xRRGGBB` hex triple with an explicit alpha.
        constexpr vec4 Rgba(u32 hex, f32 alpha)
        {
            vec4 color = Rgb(hex);
            color.a = alpha;
            return color;
        }

        Theme MakeBuiltInDarkTheme()
        {
            return Theme{
                .Background = Rgb(0x15171C),
                .Surface = Rgb(0x1B1E24),
                .SurfaceRaised = Rgb(0x22262E),
                .SurfaceHovered = Rgb(0x2C313B),
                .SurfaceActive = Rgb(0x353B47),
                .Border = Rgb(0x2A2F38),
                .BorderStrong = Rgb(0x3A414E),

                .Text = Rgb(0xE5E8EE),
                .TextMuted = Rgb(0x9BA3B0),
                .TextDisabled = Rgb(0x5A616E),
                .TextOnAccent = Rgb(0xF5F8FF),

                .Accent = Rgb(0x4C8DFF),
                .AccentHovered = Rgb(0x6BA2FF),
                .AccentActive = Rgb(0x3A78E6),
                .AccentMuted = Rgba(0x4C8DFF, 0.24f),

                .Success = Rgb(0x4ADE80),
                .Warning = Rgb(0xFBBF24),
                .Error = Rgb(0xF87171),
                .Info = Rgb(0x60A5FA),

                .WindowRounding = 6.0f,
                .ChildRounding = 6.0f,
                .FrameRounding = 5.0f,
                .PopupRounding = 5.0f,
                .GrabRounding = 4.0f,
                .TabRounding = 5.0f,
                .ScrollbarRounding = 9.0f,
                .BorderSize = 1.0f,

                .WindowPadding = vec2(12.0f, 12.0f),
                .FramePadding = vec2(8.0f, 6.0f),
                .ItemSpacing = vec2(8.0f, 6.0f),
                .ItemInnerSpacing = vec2(6.0f, 5.0f),
                .ScrollbarSize = 12.0f,
                .GrabMinSize = 10.0f,
            };
        }

        // The active theme. veng is single-threaded; no synchronization is provided.
        Theme g_ActiveTheme = MakeBuiltInDarkTheme();
    }

    const Theme& BuiltInDarkTheme()
    {
        static const Theme theme = MakeBuiltInDarkTheme();
        return theme;
    }

    const Theme& GetTheme()
    {
        return g_ActiveTheme;
    }

    void SetTheme(const Theme& theme)
    {
        g_ActiveTheme = theme;
    }

    vec4 SrgbToLinear(vec4 color)
    {
        const auto channel = [](f32 c)
        { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
        return vec4(channel(color.r), channel(color.g), channel(color.b), color.a);
    }
}
