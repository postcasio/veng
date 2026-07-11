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
    /// The single reader of raw device state: for each locally-owned seat it builds a SeatInputView
    /// over the always-present Veng::Input snapshot scoped to that seat's SeatInput devices, calls
    /// ResolveActions over the seat's InputContextStack, and stores the result in the seat's
    /// PlayerInput, threading the previous PlayerInput for phase derivation. It also folds each
    /// step's action edges into the sample's frame-accumulated StartedThisFrame/ReleasedThisFrame
    /// (reset on the frame's first step, per SystemContext::FirstStepThisFrame), so a once-per-frame
    /// reader sees an edge that a later step of a multi-step frame would erase from Phase. Because
    /// the query includes SeatInput, a seat lacking it is skipped — its PlayerInput is synthesized or
    /// replicated (the AI/remote path). Registered first in RegisterBuiltinSystems so it runs ahead
    /// of any control system; in headless the neutral snapshot resolves to all-None with no guard.
    class InputMappingSystem final : public SceneSystem
    {
    public:
        /// @brief Resolves every (Viewer, InputContextStack, PlayerInput, SeatInput) seat this tick.
        /// @param scene    The scene whose seats are resolved.
        /// @param delta    Time in seconds since the previous tick (unused).
        /// @param context  Per-tick services; the raw input snapshot is read here.
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::InputMappingSystem, 0x7B0DCB6C148AB975ULL, "Input Mapping");
