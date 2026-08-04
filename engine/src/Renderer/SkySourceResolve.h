#pragma once

#include <Veng/Scene/Components.h>
#include <Veng/Veng.h>

#include "SkySourceKind.h"

namespace Veng::Renderer
{
    struct SceneView;

    /// @brief The sky source resolved from the scene's Sky component, device-free.
    struct ResolvedSkySource
    {
        /// @brief The resolved source kind (drives the sky-pass topology).
        SkySourceKind Kind = SkySourceKind::None;
        /// @brief The requested lighting tier (None / SH / IBL).
        SkyLighting Lighting = SkyLighting::None;
        /// @brief Whether the source bakes to a radiance cube (else it is a per-pixel direct source).
        bool Baked = false;
    };

    /// @brief Maps the scene's authored Sky component onto the view's sky-source fields.
    ///
    /// Sets the environment / atmosphere / material handles and their intensities from the source,
    /// returning the resolved kind, lighting tier, and bake mode. Every source that lights via SH
    /// scales its ambient by Sky::Intensity, so a baked material sky and the procedural atmosphere
    /// set SkylightIntensity identically. Pure and device-free — no Context, no bake — so the
    /// intensity/source-field logic is unit-testable without a device.
    ///
    /// @param sky The scene's resolved Sky component, or nullptr for no sky.
    /// @param view The view whose sky-source fields are written; its fields must already sit at
    ///        their reset defaults (a fresh SceneView, or after SkyResolver's per-Execute reset).
    /// @return The resolved source kind, lighting tier, and bake mode.
    ResolvedSkySource ResolveSkySource(const Sky* sky, SceneView& view);
}
