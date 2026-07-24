#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng
{
    struct Animation;

    /// @brief One clip in a 1-D blend space: the clip and the parameter value it is authored for.
    ///
    /// A blend space is a list of these sorted ascending by Threshold. At a parameter equal to a
    /// sample's Threshold the space plays that sample's clip at full weight; between two samples it
    /// blends the bracketing pair by the normalized distance between their thresholds.
    struct BlendSample
    {
        /// @brief The clip played at Threshold.
        AssetHandle<Animation> Clip;
        /// @brief Parameter value at which this clip is at full weight.
        f32 Threshold = 0.0f;
    };

    /// @brief A 1-D blend space over a single scalar parameter — the small end of a blend space.
    ///
    /// Sits beside an Animator on a skinned-mesh entity and replaces the Animator's single clip while
    /// present: the animation system finds the bracketing pair of samples for Parameter, samples both
    /// against the entity mesh's skeleton, and blends the resulting poses by the normalized distance
    /// between their thresholds. Below the first threshold or above the last, the end sample plays at
    /// full weight. A speed-driven idle/walk/jog/sprint locomotion blend is the worked case; a game
    /// (or the builtin character-animation drive) writes Parameter each tick.
    ///
    /// Clip time is synchronized across the blend by **normalized phase**, not absolute seconds: the
    /// space advances one shared phase and samples each clip at that fraction of its own duration, so
    /// clips of different length stay foot-aligned instead of sliding against each other.
    struct AnimationBlend
    {
        /// @brief The blend samples, sorted ascending by Threshold.
        vector<BlendSample> Samples;
        /// @brief The scalar the blend is evaluated at; a driver writes it each tick.
        f32 Parameter = 0.0f;

        /// @brief Shared normalized playback phase in [0, 1); advanced by the animation system.
        ///
        /// Runtime-only playback state (no reflected field): every sample is sampled at
        /// Phase * clipDuration, which is what keeps the blended clips phase-locked. Starts at zero.
        f32 Phase = 0.0f;
    };

    /// @brief One discrete animation state: a single clip that overrides the blend while active.
    ///
    /// Named so a driver can select it by name. Entering the state crossfades its clip in over
    /// FadeIn seconds; leaving it crossfades back to the blend (or to the next state) over the same.
    struct AnimationState
    {
        /// @brief The state's name, matched against AnimationStateSet::RequestedState.
        string Name;
        /// @brief The clip this state plays.
        AssetHandle<Animation> Clip;
        /// @brief Whether the clip loops while the state is active.
        bool Loop = true;
        /// @brief Crossfade seconds when entering (or leaving) this state.
        f32 FadeIn = 0.2f;
    };

    /// @brief A set of discrete states over the blend, selected by name through one written field.
    ///
    /// Sits beside an Animator (and usually an AnimationBlend) on a skinned-mesh entity. A game — or
    /// the builtin character-animation drive — writes RequestedState; the animation system honors it,
    /// crossfading the named state's clip over the blend while it is requested and crossfading back
    /// when it is cleared. There is no authored transition graph, no editor surface, and no runtime
    /// interpreter: state selection is data plus one field. An empty or unknown RequestedState means
    /// no state overrides the blend.
    struct AnimationStateSet
    {
        /// @brief The states this set declares.
        vector<AnimationState> States;
        /// @brief The state a driver requests this tick; empty (or unknown) means the blend plays.
        string RequestedState;

        /// @brief The source currently crossfading in: a state name, or empty for the blend.
        ///
        /// Runtime-only crossfade state (no reflected field). Starts empty.
        string CurrentState;
        /// @brief The source crossfading out: a state name, or empty for the blend.
        ///
        /// Runtime-only crossfade state (no reflected field). Starts empty.
        string PreviousState;
        /// @brief Crossfade progress from PreviousState to CurrentState, in [0, 1]; 1 = fully current.
        ///
        /// Runtime-only crossfade state (no reflected field). Starts at 1 (no transition in flight).
        f32 Transition = 1.0f;
        /// @brief Playback time in seconds of CurrentState's clip.
        ///
        /// Runtime-only crossfade state (no reflected field). Reset to zero when a state is entered.
        f32 CurrentTime = 0.0f;
        /// @brief Playback time in seconds of PreviousState's clip.
        ///
        /// Runtime-only crossfade state (no reflected field).
        f32 PreviousTime = 0.0f;
    };
}

VE_REFLECT(::Veng::BlendSample, 0x9BEBA98D9D954882ULL)
VE_FIELD(Clip, .DisplayName = "Clip")
VE_FIELD(Threshold, .DisplayName = "Threshold",
         .Tooltip = "Parameter value at which this clip is at full weight")
VE_REFLECT_END();

VE_REFLECT(::Veng::AnimationBlend, 0x04F9D7D06FBC7A09ULL)
VE_ARRAY_FIELD(Samples, .DisplayName = "Samples", .Tooltip = "Sorted ascending by threshold")
VE_FIELD(Parameter, .DisplayName = "Parameter")
VE_REFLECT_END();

VE_REFLECT(::Veng::AnimationState, 0x65CC5853A22F6FA6ULL)
VE_FIELD(Name, .DisplayName = "Name")
VE_FIELD(Clip, .DisplayName = "Clip")
VE_FIELD(Loop, .DisplayName = "Loop")
VE_FIELD(FadeIn, .DisplayName = "Fade In", .Tooltip = "Crossfade seconds entering this state",
         .Display = {.Min = 0.0})
VE_REFLECT_END();

VE_REFLECT(::Veng::AnimationStateSet, 0xCC6723C4ACDFF6C9ULL)
VE_ARRAY_FIELD(States, .DisplayName = "States")
VE_FIELD(RequestedState, .DisplayName = "Requested State",
         .Tooltip = "The state a gameplay system requests; empty plays the blend")
VE_REFLECT_END();
