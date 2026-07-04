#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    /// @brief Builtin system deriving the sun from each TimeOfDay clock and writing the directional light.
    ///
    /// Per scene, reads the first TimeOfDay component and the first directional Light: it computes
    /// the toward-sun direction from Hours, DayOfYear, and the orbital parameters and writes the
    /// light's travel direction from it (the negated toward-sun direction), so direct lighting,
    /// shadows, and the sky all track the one derived sun. It runs at simulation start (so a paused
    /// scene still derives its sun once) and each tick (so advancing Hours animates the cycle). With
    /// no TimeOfDay or no directional light it is a no-op — the light keeps its authored direction.
    /// Ships with the engine; selected per level through the SystemRegistry like any other builtin
    /// system.
    class TimeOfDaySystem final : public SceneSystem
    {
    public:
        /// @brief Derives the sun once at simulation start, so a paused scene has its sun set.
        /// @param scene    The scene whose directional light is written.
        /// @param context  Per-tick services (unused).
        void OnStart(Scene& scene, const SystemContext& context) override;

        /// @brief Derives the sun from the scene's TimeOfDay and writes the first directional light.
        /// @param scene    The scene whose directional light is written.
        /// @param delta    Time in seconds since the previous tick (unused; the sun is a function of the clock).
        /// @param context  Per-tick services (unused).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;

    private:
        /// @brief Derives the toward-sun direction and writes the first directional light's travel direction.
        /// @param scene  The scene whose first TimeOfDay and directional light are read/written.
        static void DriveSun(Scene& scene);
    };
}

VE_SYSTEM(::Veng::TimeOfDaySystem, 0x06552F3EA705C2B8ULL, "Time Of Day");
