#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/Reflect.h>

#include <array>
#include <string_view>

namespace Veng::Renderer
{
    /// @brief The display-transform curve the terminal tonemap pass maps linear HDR through.
    ///
    /// A view-wide post choice, carried on the per-frame SceneView and written into the tonemap
    /// material's parameter block each Execute; the tonemap fragment branches on it. Selectable per
    /// level (the LevelRenderSettings "render" block) and from the render-settings debug panel, so
    /// the look is data, not a shader edit. Serialized by name (never ordinal) in the JSON blocks.
    enum class Tonemapper : u32
    {
        /// @brief No curve — a straight clamp of the exposed linear color to [0,1]. Highlights
        ///        hard-clip; the reference for what the curves change.
        None,
        /// @brief The Reinhard operator x/(1+x), per channel. Simple, cheap, desaturates and washes
        ///        highlights toward grey without a shoulder.
        Reinhard,
        /// @brief The Narkowicz ACES filmic approximation. A contrasty film look that lifts the
        ///        low-mids and desaturates highlights hard.
        ACES,
        /// @brief The AgX display transform (minimal fit). A neutral filmic curve with a graceful,
        ///        hue-stable highlight rolloff; slightly lower contrast than ACES.
        AgX,
        /// @brief The Khronos PBR Neutral tone mapper. Hue- and saturation-preserving: colors keep
        ///        their hue and desaturate to white only at the very top, and blacks stay black.
        PBRNeutral,
    };

    /// @brief The ordered list of every Tonemapper, for enumeration and name-table lookup.
    inline constexpr std::array<Tonemapper, 5> Tonemappers = {
        Tonemapper::None, Tonemapper::Reinhard, Tonemapper::ACES, Tonemapper::AgX,
        Tonemapper::PBRNeutral};

    /// @brief The canonical authoring name of a tonemapper (e.g. "PBRNeutral").
    ///
    /// The single source of truth for the tonemapper's JSON/UI spelling, shared by the level
    /// reflection and the debug combo.
    /// @param tonemapper  The tonemapper to name.
    /// @return The tonemapper's stable name; an empty view for an out-of-range value.
    [[nodiscard]] constexpr std::string_view ToString(Tonemapper tonemapper)
    {
        switch (tonemapper)
        {
        case Tonemapper::None:
            return "None";
        case Tonemapper::Reinhard:
            return "Reinhard";
        case Tonemapper::ACES:
            return "ACES";
        case Tonemapper::AgX:
            return "AgX";
        case Tonemapper::PBRNeutral:
            return "PBRNeutral";
        }
        return {};
    }

    /// @brief Parses a tonemapper name back to its enumerator.
    ///
    /// The inverse of ToString over the same canonical table; matching is exact and case-sensitive.
    /// @param name  The authoring name, e.g. "PBRNeutral".
    /// @return The tonemapper, or nullopt when `name` matches no tonemapper.
    [[nodiscard]] constexpr optional<Tonemapper> ParseTonemapper(std::string_view name)
    {
        for (const Tonemapper tonemapper : Tonemappers)
        {
            if (ToString(tonemapper) == name)
            {
                return tonemapper;
            }
        }
        return std::nullopt;
    }
}

VE_ENUM(::Veng::Renderer::Tonemapper, 0xFAD7E96ED0017167ULL)
VE_ENUMERATOR(None)
VE_ENUMERATOR(Reinhard)
VE_ENUMERATOR(ACES)
VE_ENUMERATOR(AgX)
VE_ENUMERATOR(PBRNeutral)
VE_ENUM_END();
