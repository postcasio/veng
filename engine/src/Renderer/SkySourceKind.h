#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief The kind of the sky source resolved from the scene's Sky component.
    ///
    /// Drives the sky pass topology: no sky, the cubemap skybox (environment source), the
    /// procedural atmosphere, or an authored Sky-domain material. A resolved change to this kind
    /// between Executes triggers the renderer's internal Rebuild — the lights model, driven by the
    /// component rather than a consumer Configure.
    ///
    /// It sits at namespace scope in its own header so the device-free frame-topology resolve can
    /// name it without reaching for the renderer class that owns the resolve state machine.
    enum class SkySourceKind : u8
    {
        /// @brief No Sky component (or an empty source): the flat fallback, no sky pass.
        None,
        /// @brief An EnvironmentSky source: the cubemap SkyboxScenePass samples its radiance cube.
        Environment,
        /// @brief An AtmosphereSky source: the procedural SkyScenePass fills the background.
        Atmosphere,
        /// @brief A MaterialSky source: the SkyMaterialScenePass runs the authored material.
        Material,
        /// @brief A CubeSky source: the cubemap SkyboxScenePass samples a caller-owned baked cube.
        Cube,
    };
}
