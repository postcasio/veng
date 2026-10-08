#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Scene/PresentationScope.h>

#include <span>

namespace Veng::Audio
{
    /// @brief The audio engine as one scene reaches it: voices are owned by the scene's
    ///        presentation scope, and starts are gated on a reconciliation replay.
    ///
    /// What SystemContext::Audio and GuiDriverFrame::Audio are, held by value: the engine, the
    /// calling scene's presentation scope, and whether the call runs inside a replay. Bound by the
    /// context factory (and, for a Gui driver, by the presenting viewport), so a system never names a
    /// scope itself — what it starts holds while its world is paused, is silent while nothing
    /// presents the scene, and stops when the scene goes. Application::GetApplicationAudio is the same
    /// facade over the application scope, for code outside every scene.
    ///
    /// An unbound facade (Unbound()) is over nothing — a driver in a viewport handed no engine — and
    /// starts nothing, so a caller needs no null-guard; so does one bound to a scope that has closed
    /// or was never handed out (a scene with no scope). It has no public default, so a context that
    /// omits its facade does not compile.
    ///
    /// Its music request is the scope's too: the engine arbitrates every scope's request and plays
    /// the winner, so a scene asks for music rather than setting it, and its ask ends with the scene.
    ///
    /// Cheap to copy: it borrows the engine, which outlives every context it rides on. Bus gains, bus
    /// DSP and the master reverb are device-wide settings, not a scene's sound, so they stay on the
    /// engine and are not reached through here.
    class ScopedAudio
    {
    public:
        /// @brief Returns a facade over nothing: every start returns an invalid handle.
        [[nodiscard]] static ScopedAudio Unbound() { return {}; }

        /// @brief Binds the facade.
        /// @param engine    The engine voices route into.
        /// @param scope     The scope that owns every voice started through this facade.
        /// @param isReplay  Whether calls run inside a reconciliation replay (SystemContext::IsReplay).
        ScopedAudio(AudioEngine& engine, PresentationScopeId scope, bool isReplay);

        /// @brief Fires a non-spatial one-shot in this facade's scope (AudioEngine::PlayOneShot).
        ///
        /// Inside a replay it starts nothing, so a Sim system re-run to re-derive predicted state
        /// never re-triggers a sound and needs no IsReplay gate of its own.
        /// @param clip    The clip to play.
        /// @param params  The mix parameters.
        /// @return A handle to the voice, or an invalid handle when nothing started.
        VoiceHandle PlayOneShot(const AssetHandle<AudioClip>& clip,
                                const OneShotParams& params = {}) const;

        /// @brief Fires a spatial voice at a world position in this facade's scope (AudioEngine::PlayAt).
        ///
        /// Spatialized against this scope's listener; starts nothing inside a replay.
        /// @param clip      The clip to play.
        /// @param worldPos  The voice's world position.
        /// @param params    The spatial mix parameters.
        /// @return A handle to the voice, or an invalid handle when nothing started.
        VoiceHandle PlayAt(const AssetHandle<AudioClip>& clip, vec3 worldPos,
                           const SpatialOneShotParams& params = {}) const;

        /// @brief Registers a generator voice in this facade's scope (AudioEngine::PlayGenerator).
        ///
        /// Starts nothing inside a replay (the engine then holds no reference to @p generator).
        /// @param generator  The sample source (non-null); the voice shares ownership of it.
        /// @param params     The voice registration parameters.
        /// @return A handle to the voice, or an invalid handle when nothing started.
        VoiceHandle PlayGenerator(Ref<IAudioGenerator> generator,
                                  const GeneratorVoiceParams& params) const;

        /// @brief Registers a clip voice in this facade's scope (AudioEngine::AddClipVoice).
        ///
        /// AudioSystem's path for authored sources; starts nothing inside a replay.
        /// @param clip    The clip to play.
        /// @param params  The mix parameters.
        /// @return A handle to the voice, or an invalid handle when nothing started.
        VoiceHandle AddClipVoice(const AssetHandle<AudioClip>& clip,
                                 const VoiceParams& params) const;

        /// @brief Repositions a spatial voice (AudioEngine::SetVoicePose); works inside a replay.
        void SetVoicePose(VoiceHandle voice, vec3 worldPos, vec3 velocity) const;

        /// @brief Retunes a positioned voice's authored mix (AudioEngine::SetVoiceMix); works inside
        ///        a replay.
        void SetVoiceMix(VoiceHandle voice, const SpatialVoiceMix& mix) const;

        /// @brief Returns whether the engine still holds @p generator for any voice
        ///        (AudioEngine::IsGeneratorInUse); false over nothing.
        [[nodiscard]] bool IsGeneratorInUse(const IAudioGenerator& generator) const;

        /// @brief Updates a live voice's mix parameters (AudioEngine::SetVoiceParams).
        void SetVoiceParams(VoiceHandle voice, const VoiceParams& params) const;

        /// @brief Stops a voice (AudioEngine::StopVoice); works inside a replay.
        void StopVoice(VoiceHandle voice) const;

        /// @brief Returns whether a handle still names a live voice; false over nothing.
        [[nodiscard]] bool IsVoiceLive(VoiceHandle voice) const;

        /// @brief Returns a live voice's mix parameters, or nullopt (always nullopt over nothing).
        [[nodiscard]] optional<VoiceParams> GetVoiceParams(VoiceHandle voice) const;

        /// @brief Sets this scope's listener, the pose its spatial voices are placed against.
        /// @param listener  The listener pose.
        void SetListener(const ListenerPose& listener) const;

        /// @brief Wraps a code-built sample buffer as a clip (AudioEngine::CreateClip).
        ///
        /// A clip is an asset, not a voice, so this is unscoped and not gated; over nothing it
        /// returns an invalid handle.
        [[nodiscard]] AssetHandle<AudioClip> CreateClip(std::span<const f32> samples,
                                                        AudioBufferFormat format) const;

        /// @brief Returns the device's output sample rate in Hz, or 0 over nothing.
        [[nodiscard]] u32 GetOutputSampleRate() const;

        /// @brief Sets or withdraws this scope's standing music request (AudioEngine::SetMusicRequest).
        ///
        /// The request stands until replaced or until the scope closes, and the engine plays the
        /// highest-priority eligible request each frame. Not gated on a replay: it is state, not a
        /// start, and re-submitting it is idempotent. Over nothing it does nothing.
        /// @param request  The request, or nullopt to withdraw it.
        void SetMusicRequest(const optional<MusicRequest>& request) const;

        /// @brief Returns whether the facade is bound to an engine.
        [[nodiscard]] bool IsBound() const { return m_Engine != nullptr; }

        /// @brief Returns the engine voices route into, or null over nothing.
        [[nodiscard]] AudioEngine* GetEngine() const { return m_Engine; }

        /// @brief Returns the scope that owns what this facade starts.
        [[nodiscard]] PresentationScopeId GetScope() const { return m_Scope; }

        /// @brief Whether calls run inside a reconciliation replay, so starts start nothing.
        [[nodiscard]] bool IsReplay() const { return m_IsReplay; }

    private:
        /// @brief The unbound facade Unbound returns.
        ScopedAudio() = default;

        /// @brief Returns whether a start may proceed: bound, and not inside a replay.
        [[nodiscard]] bool CanStart() const { return m_Engine != nullptr && !m_IsReplay; }

        /// @brief The engine voices route into; null over nothing.
        AudioEngine* m_Engine = nullptr;
        /// @brief The owning scope.
        PresentationScopeId m_Scope;
        /// @brief Whether calls run inside a reconciliation replay.
        bool m_IsReplay = false;
    };
}
