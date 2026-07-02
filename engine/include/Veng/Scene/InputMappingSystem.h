#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class Scene;

    /// @brief Whether a seat is simulated on this client, so its input resolves locally.
    ///
    /// The seam the net layer keys on: a locally-owned seat's PlayerInput is filled by
    /// resolving the raw snapshot against its InputContextStack; a remote or AI seat's
    /// arrives replicated or synthesized. Today it returns true for every seat — no remote
    /// ownership exists yet — threaded now so the resolve-per-seat shape is right when
    /// replication lands. It does not gate on Authority::Tier yet.
    /// @param scene  The scene the seat lives in.
    /// @param seat   The seat entity to test.
    /// @return True while the seat is simulated locally.
    [[nodiscard]] VE_API bool IsLocallyOwned(const Scene& scene, Entity seat);

    /// @brief Builtin Sim system that resolves each seat's active contexts into its PlayerInput.
    ///
    /// The single reader of raw device state: it adapts the always-present Veng::Input snapshot
    /// through RawInput and calls ResolveActions over each locally-owned seat's InputContextStack,
    /// storing the result in the seat's PlayerInput and threading the previous PlayerInput for
    /// phase derivation. Registered first in RegisterBuiltinSystems so it runs ahead of any
    /// control system; in headless the neutral snapshot resolves to all-None with no guard.
    class InputMappingSystem final : public SceneSystem
    {
    public:
        /// @brief Resolves every (Viewer, InputContextStack, PlayerInput) seat's input this tick.
        /// @param scene    The scene whose seats are resolved.
        /// @param delta    Time in seconds since the previous tick (unused).
        /// @param context  Per-tick services; the raw input snapshot is read here.
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::InputMappingSystem, 0x7B0DCB6C148AB975ULL, "Input Mapping");
