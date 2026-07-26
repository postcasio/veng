#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class Scene;

    /// @brief Whether a seat is owned by this peer, so its input resolves from local devices.
    ///
    /// A locally-owned seat's PlayerInput is filled by resolving the raw snapshot against its
    /// InputContextStack; a remote or AI seat's arrives replicated or synthesized. Because Viewer is
    /// always-relevant a peer receives one replicated seat per peer, so the answer is decided in
    /// three steps:
    ///
    /// - A joining client publishes a LocalSeat marker on its own seat. When any seat in the scene
    ///   carries it, exactly the marked seat is locally owned and the peers' replicated seats are not.
    /// - Otherwise a host reads Authority: a seat a remote connection owns (Owner != 0) is that
    ///   peer's, not this one's. Authority does not replicate, so this never fires on a client.
    /// - With nothing published and no remote owner — single-player, headless, a host's own seat —
    ///   the seat is locally owned, the pre-replication default.
    /// @param scene  The scene the seat lives in.
    /// @param seat   The seat entity to test.
    /// @return True while the seat is owned by this peer.
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
