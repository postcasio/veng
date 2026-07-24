#include <Veng/Scene/BuiltinSystems.h>

#include <Veng/Scene/AnimationSystem.h>
#include <Veng/Scene/CameraRig.h>
#include <Veng/Scene/CharacterAnimationSystem.h>
#include <Veng/Scene/DeviceAssignmentSystem.h>
#include <Veng/Scene/InputMappingSystem.h>
#include <Veng/Physics/CharacterMovementSystem.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Scene/Motion.h>
#include <Veng/Scene/Movement.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/RootMotion.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/Scene/TimeOfDay.h>

namespace Veng
{
    void RegisterBuiltinSystems(SystemRegistry& registry)
    {
        // Builtins go through the same Register<T> path as game systems; no special-casing.
        // A level names the subset and order it runs — this only makes them resolvable.

        // Auto-assigns a connected pad into an opted-in seat's SeatInput slot; ordered before
        // InputMappingSystem so a pad connected this tick is assigned before this tick's resolve.
        registry.Register<DeviceAssignmentSystem>();

        // The sole reader of raw device state, registered first so it runs ahead of any
        // control system that reads the resolved PlayerInput.
        registry.Register<InputMappingSystem>();

        registry.Register<MovementSystem>();

        // The character analogue of MovementSystem and its alternative rather than its companion: a
        // level names one or the other, so a pawn either flies through MovementSystem or walks
        // through this. Named before PhysicsSystem, like MovementSystem, so the capsule moves
        // against the previous tick's finalized world.
        registry.Register<CharacterMovementSystem>();

        registry.Register<RootMotionDriveSystem>();
        registry.Register<CameraRigSystem>();

        // Maps a character's finalized motion state onto its animation drive fields (speed →
        // AnimationBlend::Parameter, airborne → AnimationStateSet::RequestedState). View-phase and
        // registered before AnimationSystem so a level naming both feeds the blend the same tick it
        // is posed; a level driving the blend from elsewhere simply does not name it.
        registry.Register<CharacterAnimationSystem>();

        registry.Register<AnimationSystem>();
        registry.Register<ConstantMotionSystem>();

        // Steps the scene's PhysicsWorld once per Sim tick. A level names it after the systems that
        // produce motion, so a kinematic body's target pose for the tick is already written; a level
        // that does not name it runs no solver.
        registry.Register<PhysicsSystem>();

        // Writes each Remote-tier entity's displayed Transform from its snapshot buffer, in the past.
        // View-phase — presentation only. Idles with no remote entities, so single-player is untouched.
        registry.Register<RemoteInterpolationSystem>();

        // Derives the sun from the scene's TimeOfDay and writes the first directional light,
        // so the sun the sky and shadows read is real world state any system can also read.
        registry.Register<TimeOfDaySystem>();
    }
}
