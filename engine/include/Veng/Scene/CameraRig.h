#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    struct Transform;
    struct CameraFollow;
    struct CameraLook;
    struct CameraOrbit;
    struct FirstPersonRig;
    class Scene;

    /// @brief Computes the camera Transform that trails a target by a follow relationship.
    ///
    /// The camera is placed at the target's world position plus the follow Offset rotated
    /// by the target's yaw (its heading about world up) — not its full orientation, so a
    /// target that pitches or rolls keeps the camera at the offset's authored height — then
    /// orbited up/down around the target by the follow Pitch, and oriented to look at the
    /// target. When the
    /// follow's Damping is positive, the result is exponentially smoothed from the camera's
    /// current Transform toward that goal over delta; a zero Damping snaps to the goal. Pure
    /// math — no scene, no device — so it is the deterministic core both the camera-rig
    /// system and the unit tests drive.
    /// @param current     The camera's current Transform (the smoothing start point).
    /// @param targetWorld The target entity's world matrix (from WorldMatrix in Transforms.h).
    /// @param follow      The follow Offset and Damping.
    /// @param delta       Time in seconds since the previous tick.
    /// @return The camera Transform for this tick.
    [[nodiscard]] Transform FollowCamera(const Transform& current, const mat4& targetWorld,
                                         const CameraFollow& follow, f32 delta);

    /// @brief Computes the camera Transform that orbits a focus point at a distance.
    ///
    /// Integrates the orbit's optional focus glide (Focus eased toward FocusTarget by the same
    /// frame-rate-independent exponential smoothing FollowCamera uses; a zero FocusDamping
    /// snaps), clamps Distance into [MinDistance, MaxDistance] and Pitch into ±PitchLimit, then
    /// places the eye at focus + LookRotation({Yaw, Pitch}) · (0, 0, Distance) and orients it
    /// back at the focus. y-up throughout, like every other engine rig. Pure math — no scene, no
    /// device — so it is the deterministic core both the camera-rig system and the unit tests
    /// drive.
    /// @param orbit   The focus, distance and clamps, orbit angles, and focus-glide parameters.
    /// @param current The camera's current Transform (the focus-glide start point).
    /// @param delta   Time in seconds since the previous tick.
    /// @return The camera Transform for this tick.
    [[nodiscard]] Transform OrbitCamera(const CameraOrbit& orbit, const Transform& current,
                                        f32 delta);

    /// @brief Computes the first-person rotation a look heading resolves to.
    ///
    /// Yaw about world up composed with pitch about the yawed right axis, the pitch
    /// clamped into ±PitchLimit — so the camera never crosses the poles. Pure math with
    /// no scene or device, the deterministic core both the camera-rig system and the
    /// unit tests drive.
    /// @param look The yaw/pitch heading and its pitch limit.
    /// @return The world-space rotation facing along the heading.
    [[nodiscard]] quat LookRotation(const CameraLook& look);

    /// @brief An orthonormal camera basis: the right, up, and view-forward axes.
    ///
    /// Right is the screen-right axis, Up the screen-up axis, and Forward the view direction the
    /// camera looks along (its local -Z). The three are mutually perpendicular and unit length.
    struct CameraBasis
    {
        /// @brief Screen-right axis (camera local +X).
        vec3 Right;
        /// @brief Screen-up axis (camera local +Y).
        vec3 Up;
        /// @brief View direction the camera looks along (camera local -Z).
        vec3 Forward;
    };

    /// @brief Builds the first-person camera basis about a character's up.
    ///
    /// Projects the target's forward onto the plane perpendicular to @p characterUp, yaws it about
    /// that up by @p yaw, takes right = normalize(cross(forward, up)) — so right is always
    /// perpendicular to the character's up and the horizon stays level — then pitches the forward
    /// about right by @p pitch, clamped into [minPitch, maxPitch]. The returned basis's Right is
    /// coplanar-perpendicular to @p characterUp for every up, which is the property a rig yawing
    /// about world up cannot hold under a varying up. Pure math — no scene, no device — the
    /// deterministic core both the rig system and the unit tests drive.
    /// @param characterUp    The character's up this tick (need not be world up); normalized inside.
    /// @param targetForward  The target's forward; its component along the up is projected out.
    /// @param yaw            Heading about the up axis, in radians, relative to the projected forward.
    /// @param pitch          Elevation about the right axis, in radians, before clamping.
    /// @param minPitch       Lower pitch clamp, in radians.
    /// @param maxPitch       Upper pitch clamp, in radians.
    /// @return The orthonormal right/up/forward basis.
    [[nodiscard]] CameraBasis FirstPersonBasis(vec3 characterUp, vec3 targetForward, f32 yaw,
                                               f32 pitch, f32 minPitch, f32 maxPitch);

    /// @brief Computes the first-person camera Transform for one tick.
    ///
    /// Builds the basis through FirstPersonBasis, orients the camera down its forward with the
    /// horizon level against @p characterUp, and places the eye at @p eyeAnchor plus a view bob (a
    /// vertical and lateral oscillation of amplitude rig.BobAmplitude at phase @p bobPhase; zero
    /// amplitude leaves the eye exactly at the anchor). Pure math — no scene, no device.
    /// @param eyeAnchor     The resolved world-space eye position, before the bob.
    /// @param characterUp   The character's up this tick.
    /// @param targetForward The target's forward, for the yaw reference.
    /// @param look          The accumulated Yaw/Pitch heading.
    /// @param rig           The eye/pitch/bob parameters.
    /// @param bobPhase      The accumulated bob phase, in radians.
    /// @return The camera Transform for this tick.
    [[nodiscard]] Transform FirstPersonCamera(vec3 eyeAnchor, vec3 characterUp, vec3 targetForward,
                                              const CameraLook& look, const FirstPersonRig& rig,
                                              f32 bobPhase);

    /// @brief View-phase system that resolves each rigged camera's pose.
    ///
    /// Runs in the View phase, so it reads pawn state the Sim phase finalized this tick.
    /// For every entity with (Transform, CameraFollow) whose Target is a live entity with a
    /// Transform, it writes the camera entity's Transform through FollowCamera; for every
    /// entity with (Transform, CameraOrbit) it writes the pose through OrbitCamera; for every
    /// entity with (Transform, FirstPersonRig) whose Target is live it writes the pose through
    /// FirstPersonCamera (eye-anchored, yawing about the target's up); for every entity with
    /// (Transform, CameraLook) and no FirstPersonRig it clamps the look pitch and writes the
    /// entity's rotation through LookRotation. All go through the scene accessor so the
    /// spatial-version bookkeeping is correct. The produced camera pose is purely local —
    /// never authoritative, never on the wire.
    class CameraRigSystem final : public SceneSystem
    {
    public:
        /// @brief Returns Phase::View — the rig derives view state after the Sim phase.
        [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

        /// @brief Resolves each follow camera behind its Target, each orbit camera around its
        ///        Focus, each first-person camera out of its Target's eye, and each look camera's
        ///        rotation.
        /// @param scene    The scene whose rigged cameras are updated.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services (unused).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::CameraRigSystem, 0x4EBD17824A9652D8ULL, "Camera Rig");
