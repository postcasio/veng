#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/AudioBus.h>
#include <Veng/Audio/AudioBuffer.h>
#include <Veng/Audio/AudioClip.h>
#include <Veng/Audio/AudioGenerator.h>
#include <Veng/Audio/Voice.h>
#include <Veng/Asset/AssetHandle.h>

#include <array>
#include <span>

namespace Veng::Audio
{
    class AudioDevice;
    class MusicDirector;
    struct StreamVoice;
    struct BufferedGenerator;

    /// @brief Parameters of a code-triggered non-spatial one-shot voice.
    struct OneShotParams
    {
        /// @brief The bus the voice mixes into.
        AudioBus Bus = AudioBus::SFX;
        /// @brief Linear gain, 0 = silent, 1 = unity.
        f32 Gain = 1.0f;
        /// @brief Playback pitch (resample ratio); 1 = the clip's native rate.
        f32 Pitch = 1.0f;
        /// @brief Whether the voice loops; a one-shot (false) retires when the clip ends.
        bool Loop = false;
    };

    /// @brief Parameters of a code-triggered spatial voice placed at a fixed world position.
    ///
    /// The voice is attenuated, panned, Doppler-shifted, and reverb-sent against the current
    /// listener exactly as an authored AudioSource is. A moving positioned voice is registered with
    /// PlayAt and then repositioned each frame through AudioEngine::SetVoicePose.
    struct SpatialOneShotParams
    {
        /// @brief The bus the voice mixes into.
        AudioBus Bus = AudioBus::SFX;
        /// @brief Linear gain applied before spatialization; 0 = silent, 1 = unity.
        f32 Gain = 1.0f;
        /// @brief Base playback pitch (resample ratio); Doppler multiplies this.
        f32 Pitch = 1.0f;
        /// @brief Whether the voice loops; false retires it when the clip ends.
        bool Loop = false;
        /// @brief Distance at or within which the voice plays at full Gain.
        f32 MinDistance = 1.0f;
        /// @brief Distance at or beyond which the voice is silent.
        f32 MaxDistance = 50.0f;
        /// @brief Occlusion low-pass drive, 0 = clear (bypass) to 1 = fully occluded.
        f32 OcclusionFactor = 0.0f;
        /// @brief Initial world velocity for Doppler, units per second (updated via SetVoicePose).
        vec3 Velocity{0.0f};
    };

    /// @brief What role a live voice plays, for read-only inspection.
    enum class VoiceOrigin : u8
    {
        /// @brief An authored AudioSource voice the AudioSystem drives.
        Source,
        /// @brief A non-spatial fire-and-forget one-shot (PlayOneShot).
        OneShot,
        /// @brief A positioned voice re-spatialized each frame (PlayAt / a spatial generator).
        Spatial,
        /// @brief A music-director voice.
        Music,
    };

    /// @brief A read-only snapshot of one live voice, for inspection.
    ///
    /// The shape AudioEngine::GetVoiceInfos reports each active voice as. Position and Velocity are
    /// meaningful only for a Spatial voice; a non-spatial voice leaves them zero.
    struct VoiceInfo
    {
        /// @brief The voice handle (slot + generation).
        VoiceHandle Handle;
        /// @brief The bus the voice mixes into.
        AudioBus Bus = AudioBus::SFX;
        /// @brief What role the voice plays.
        VoiceOrigin Origin = VoiceOrigin::Source;
        /// @brief Whether the voice is fed by a live generator (true) or a PCM buffer (false).
        bool Generator = false;
        /// @brief Final linear gain the mixer applies.
        f32 Gain = 0.0f;
        /// @brief Stereo pan, -1 = left, 0 = centre, +1 = right.
        f32 Pan = 0.0f;
        /// @brief Resample ratio applied on top of the clip-rate conversion (Doppler pitch).
        f32 Pitch = 1.0f;
        /// @brief Occlusion low-pass amount, 0 = open, 1 = fully occluded.
        f32 Occlusion = 0.0f;
        /// @brief Send level into the master reverb, 0 = dry, 1 = full send.
        f32 ReverbSend = 0.0f;
        /// @brief Whether the voice loops.
        bool Loop = false;
        /// @brief Whether the voice is spatialized (Position/Velocity meaningful).
        bool Spatial = false;
        /// @brief World position, for a Spatial voice.
        vec3 Position{0.0f};
        /// @brief World velocity (units per second), for a Spatial voice.
        vec3 Velocity{0.0f};
    };

    /// @brief How the music director transitions to a new track.
    struct MusicTransition
    {
        /// @brief Crossfade duration in seconds; 0 is a hard cut.
        f32 FadeSeconds = 0.0f;
        /// @brief Whether the incoming track loops.
        bool Loop = true;
    };

    /// @brief The mixer-facing, main-thread API: buses and voices.
    ///
    /// The engine holds the authoritative bus parameters and voice table, publishes an immutable
    /// snapshot to the device's mixing thread each frame, and reaps finished voices and reclaimed
    /// resources through the device's generation counter. Every call is main-thread only; the
    /// real-time thread never touches it.
    class AudioEngine
    {
    public:
        /// @brief Constructs the engine over its owning device.
        /// @param device The device whose snapshot bridge and generation counter the engine drives.
        explicit AudioEngine(AudioDevice& device);

        /// @brief Destroys the engine and its music director.
        ~AudioEngine();

        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;

        /// @brief The device's negotiated output sample rate in Hz.
        ///
        /// The rate every registered generator's Render is invoked at. A consumer that must size
        /// rate-dependent state before its first Render — a generator allocating delay lines for an
        /// embedded reverb, say — reads the real rate here instead of assuming a default.
        [[nodiscard]] u32 GetOutputSampleRate() const;

        /// @brief Sets a bus's linear gain.
        /// @param bus  The bus.
        /// @param gain Linear gain (clamped to >= 0).
        void SetBusGain(AudioBus bus, f32 gain);
        /// @brief Returns a bus's linear gain.
        [[nodiscard]] f32 GetBusGain(AudioBus bus) const;

        /// @brief Sets a bus's low-pass cutoff in Hz; 0 (or above Nyquist) bypasses the filter.
        /// @param bus       The bus.
        /// @param cutoffHz  Cutoff frequency in Hz.
        void SetBusLowpassCutoff(AudioBus bus, f32 cutoffHz);
        /// @brief Returns a bus's low-pass cutoff in Hz.
        [[nodiscard]] f32 GetBusLowpassCutoff(AudioBus bus) const;

        /// @brief Sets a bus's send level into the master reverb, 0..1.
        /// @param bus  The bus.
        /// @param send Send level, clamped to 0..1.
        void SetBusReverbSend(AudioBus bus, f32 send);
        /// @brief Returns a bus's send level into the master reverb.
        [[nodiscard]] f32 GetBusReverbSend(AudioBus bus) const;

        /// @brief Sets the master reverb parameters.
        /// @param params The reverb parameters.
        void SetReverbParams(const ReverbParams& params);
        /// @brief Returns the master reverb parameters.
        [[nodiscard]] ReverbParams GetReverbParams() const;

        /// @brief Wraps a code-built sample buffer as an AudioClip handle.
        ///
        /// Copies @p samples into a device-readable buffer and Adopts it as an AudioClip: the
        /// result plays through PlayOneShot/PlayAt, attaches to an AudioSource, or feeds the music
        /// director — a clip in every respect except provenance. The finite-one-shot runtime path
        /// (the generator path is IAudioGenerator + PlayGenerator).
        /// @param samples Interleaved float PCM (length must be a multiple of format.Channels).
        /// @param format  The sample rate and channel count; a 0 rate uses the device output rate.
        /// @return A resident clip handle.
        [[nodiscard]] AssetHandle<AudioClip> CreateClip(std::span<const f32> samples,
                                                        AudioBufferFormat format);

        /// @brief Registers an on-demand generator as a voice, arbitrating against the voice budget.
        ///
        /// The engine holds the borrowed @p generator pointer for the voice's lifetime; the caller
        /// owns it and guarantees it outlives the voice, releasing it only after StopVoice returns.
        /// A Spatial voice is placed at params.Position and spatialized against the listener exactly
        /// as a clip is (move it later with SetVoicePose); a non-spatial voice routes to its bus at
        /// params.Gain. A Buffered voice renders ahead of time on the fill thread into a ring the
        /// real-time callback only drains, moving heavy synthesis off the real-time thread; it is
        /// non-spatial, so a Buffered && Spatial request is rejected. Same budget arbitration as
        /// AddVoice.
        /// @param generator The sample source (must be non-null; not owned).
        /// @param params    The voice registration parameters.
        /// @return A handle to the voice, or an invalid handle if it was rejected.
        VoiceHandle PlayGenerator(IAudioGenerator* generator, const GeneratorVoiceParams& params);

        /// @brief Registers a voice playing a buffer, arbitrating against the voice budget.
        ///
        /// Takes a free slot when one exists; when the budget is full, evicts the quietest active
        /// voice if the incoming voice is louder, and otherwise rejects the request. The evicted or
        /// rejected outcome is reported by an invalid handle.
        /// @param buffer The PCM source (held for the voice's lifetime).
        /// @param params The mix parameters.
        /// @return A handle to the voice, or an invalid handle if it was rejected.
        VoiceHandle AddVoice(const Ref<AudioBuffer>& buffer, const VoiceParams& params);

        /// @brief Registers a voice for a clip, choosing the resident or streaming path by storage.
        ///
        /// A Pcm clip plays through AddVoice off its resident buffer; an Encoded clip plays through a
        /// streaming voice, decoded incrementally on the engine's decode thread and drained by the
        /// mixer exactly as a resident voice is — indistinguishable downstream. Same budget
        /// arbitration as AddVoice. A null or unresident clip is rejected with an invalid handle.
        /// @param clip   The clip to play.
        /// @param params The mix parameters.
        /// @return A handle to the voice, or an invalid handle if it was rejected.
        VoiceHandle AddClipVoice(const AssetHandle<AudioClip>& clip, const VoiceParams& params);

        /// @brief Returns whether a handle still names a live voice.
        /// @param voice The handle.
        [[nodiscard]] bool IsVoiceLive(VoiceHandle voice) const;

        /// @brief Updates a live voice's mix parameters (no effect on a stale handle).
        /// @param voice  The handle.
        /// @param params The new parameters.
        void SetVoiceParams(VoiceHandle voice, const VoiceParams& params);

        /// @brief Stops a voice and routes its source to reclamation (no effect on a stale handle).
        ///
        /// A buffer voice's source is queued for deferred free and released once the mixing thread's
        /// generation counter passes the last snapshot that referenced it, so it can never be freed
        /// mid-mix. A generator voice's reclamation is the same handshake made synchronous: the call
        /// removes the generator from the live snapshot and returns only once the callback has
        /// consumed a frame past it, so the caller may then free the borrowed generator with no
        /// use-after-free. A buffered generator voice adds the fill thread as a second party: the call
        /// also posts a Remove and returns only once the fill thread has acknowledged it, so no thread
        /// can render the borrowed generator after this returns.
        /// @param voice The handle.
        void StopVoice(VoiceHandle voice);

        /// @brief Returns the number of live voices.
        [[nodiscard]] usize GetActiveVoiceCount() const;

        /// @brief Returns a read-only snapshot of every live voice, for inspection.
        ///
        /// One VoiceInfo per active slot, in slot order — the mix parameters, role, and (for a
        /// spatial voice) world pose the engine holds. A read seam for tooling; it takes no locks
        /// and mutates nothing.
        [[nodiscard]] vector<VoiceInfo> GetVoiceInfos() const;

        /// @brief Fires a non-spatial one-shot voice on a chosen bus.
        ///
        /// A fire-and-forget voice: a non-looping clip's voice retires when it ends. Backed by the
        /// engine one-shot pool; when the pool is full the quietest pooled voice is dropped if the
        /// incoming voice is louder, and otherwise the request is rejected. An Encoded clip plays
        /// through the streaming path like any other; an unresident clip plays nothing.
        /// @param clip   The clip to play.
        /// @param params The mix parameters (bus, gain, pitch, loop).
        /// @return A handle to the voice for early stop, or an invalid handle if it was rejected.
        VoiceHandle PlayOneShot(const AssetHandle<AudioClip>& clip,
                                const OneShotParams& params = {});

        /// @brief Fires a spatial one-shot voice at a fixed world position.
        ///
        /// The voice is spatialized against the current listener like an authored AudioSource. It is
        /// fixed by default; call SetVoicePose each frame to move it. Same pool policy as
        /// PlayOneShot; an Encoded clip plays through the streaming path, an unresident clip nothing.
        /// @param clip     The clip to play.
        /// @param worldPos The voice's world position.
        /// @param params   The spatial mix parameters.
        /// @return A handle to the voice for early stop or repositioning, or an invalid handle.
        VoiceHandle PlayAt(const AssetHandle<AudioClip>& clip, vec3 worldPos,
                           const SpatialOneShotParams& params = {});

        /// @brief Repositions an imperatively-placed spatial voice (no effect on a stale handle).
        ///
        /// Folds the new pose into the next published snapshot. Applies only to a voice registered
        /// spatial through PlayAt (a non-spatial one-shot ignores it); the general capability a
        /// consumer tracking a moving emitter every frame reaches for.
        /// @param voice    The handle returned by PlayAt.
        /// @param worldPos The new world position.
        /// @param velocity The new world velocity, units per second (for Doppler).
        void SetVoicePose(VoiceHandle voice, vec3 worldPos, vec3 velocity);

        /// @brief Advances the engine-owned managed voices against the listener for one frame.
        ///
        /// Advances the music director's crossfade and re-spatializes every positioned one-shot
        /// against @p listener, folding both into the voice table before the next Publish. The
        /// View-phase AudioSystem calls this each update with the resolved listener pose.
        /// @param listener The listener pose to spatialize positioned voices against.
        /// @param delta    Time in seconds since the previous update.
        void UpdateManagedVoices(const ListenerPose& listener, f32 delta);

        /// @brief Returns the music director, the one-track policy over the Music bus.
        [[nodiscard]] MusicDirector& Music();

        /// @brief Returns a live voice's current mix parameters, or nullopt for a stale handle.
        /// @param voice The handle.
        [[nodiscard]] optional<VoiceParams> GetVoiceParams(VoiceHandle voice) const;

        /// @brief Returns the number of live one-shot / positioned voices in the pool (test seam).
        [[nodiscard]] usize GetManagedVoiceCount() const;

        /// @brief Returns the number of sources awaiting deferred reclamation (test seam).
        ///
        /// A retired buffer or streaming source stays counted here until the reclamation handshake
        /// lets it free, so a test can watch a decoder outlive its stop and then be released.
        [[nodiscard]] usize GetPendingReclaimCount() const { return m_Deferred.size(); }

        /// @brief Publishes a snapshot of the current bus and voice state to the mixing thread.
        void Publish();

        /// @brief Reaps voices the mixing thread reported finished, routing sources to reclamation.
        void DrainRetired();

        /// @brief Frees reclaimed sources whose referencing generation the mixer has passed.
        void CollectDeferred();

    private:
        /// @brief One main-thread voice record.
        struct Voice
        {
            /// @brief Whether the slot holds a live voice.
            bool Active = false;
            /// @brief The slot's current generation.
            u32 Generation = 0;
            /// @brief The owned PCM source (null for a generator or stream voice).
            Ref<AudioBuffer> Source;
            /// @brief The borrowed on-demand source (null for a buffer or stream voice); not owned.
            IAudioGenerator* Generator = nullptr;
            /// @brief Rendered channel count of a generator voice: 1 (mono) or 2 (interleaved stereo).
            u32 GeneratorChannels = 1;
            /// @brief The owned streaming source (null for a buffer or generator voice).
            Unique<StreamVoice> Stream;
            /// @brief The owned buffered-generator ring wrapper (null unless the voice is buffered).
            ///
            /// Wraps the borrowed Generator, which the fill thread renders off the real-time thread;
            /// the wrapper is engine-owned and rides the reclamation handshake, the generator is not.
            Unique<BufferedGenerator> Buffered;
            /// @brief The mix parameters.
            VoiceParams Params;
        };

        /// @brief A source awaiting reclamation once the referencing threads are provably past it.
        struct Deferred
        {
            /// @brief The buffer source to release (set for a buffer voice).
            Ref<AudioBuffer> Source;
            /// @brief The streaming source to release (set for a stream voice).
            ///
            /// A stream also rides the decode thread's release ack: it is freed only once the mixer's
            /// consumed serial passes SafeAfterSerial and the decode thread reports it released.
            Unique<StreamVoice> Stream;
            /// @brief The buffered-generator wrapper to release (set for a buffered generator voice).
            ///
            /// Rides the same dual handshake as Stream: freed only once the mixer's consumed serial
            /// passes SafeAfterSerial and the fill thread reports it released.
            Unique<BufferedGenerator> Buffered;
            /// @brief Free once the consumed generation exceeds this serial.
            u64 SafeAfterSerial = 0;
        };

        /// @brief What an engine-owned voice is, beyond a raw AudioSource-driven one.
        enum class ManagedKind : u8
        {
            /// @brief Not engine-owned: an AudioSource voice the AudioSystem drives directly.
            None,
            /// @brief A non-spatial fire-and-forget one-shot (PlayOneShot).
            OneShot,
            /// @brief A positioned voice re-spatialized each frame (PlayAt / SetVoicePose).
            Spatial,
            /// @brief A music-director voice, its gain driven by the crossfade envelope.
            Music,
        };

        /// @brief Per-slot metadata for an engine-owned voice (empty for an AudioSource voice).
        struct Managed
        {
            /// @brief What kind of engine-owned voice occupies this slot.
            ManagedKind Kind = ManagedKind::None;
            /// @brief The voice's bus.
            AudioBus Bus = AudioBus::SFX;
            /// @brief Pre-spatialization linear gain.
            f32 BaseGain = 1.0f;
            /// @brief Base pitch, before any Doppler multiply.
            f32 BasePitch = 1.0f;
            /// @brief Whether the voice loops.
            bool Loop = false;
            /// @brief World position (Spatial voices).
            vec3 WorldPos{0.0f};
            /// @brief World velocity for Doppler (Spatial voices).
            vec3 Velocity{0.0f};
            /// @brief Full-gain rolloff distance (Spatial voices).
            f32 MinDistance = 1.0f;
            /// @brief Silence rolloff distance (Spatial voices).
            f32 MaxDistance = 50.0f;
            /// @brief Occlusion low-pass drive (Spatial voices).
            f32 Occlusion = 0.0f;
        };

        /// @brief Registers a streaming voice for an Encoded clip, arbitrating against the budget.
        ///
        /// Opens an incremental decoder over the clip, hands it to a StreamVoice the decode thread
        /// fills, and takes a slot (evicting the quietest louder-than-incoming voice when full). The
        /// clip handle is held on the StreamVoice so its bytes outlive the borrowing decoder.
        /// @param clip   The Encoded clip to stream.
        /// @param params The mix parameters.
        /// @return A handle to the voice, or an invalid handle if it was rejected or undecodable.
        VoiceHandle AddStreamVoice(const AssetHandle<AudioClip>& clip, const VoiceParams& params);

        /// @brief Deactivates a slot and routes its source to reclamation.
        void RetireSlot(u32 slot);

        /// @brief Reserves a voice slot, evicting the quietest active voice when the budget is full.
        ///
        /// Returns a free slot, or the slot of a voice evicted because @p incomingGain is louder,
        /// or InvalidSlot when the budget is full and the incoming voice would be the quietest.
        /// @param incomingGain The incoming voice's pre-spatialization gain.
        [[nodiscard]] u32 AllocateSlot(f32 incomingGain);

        /// @brief Folds a positioned voice's metadata and the listener into final mix parameters.
        [[nodiscard]] VoiceParams SpatializeManaged(const Managed& managed,
                                                    const ListenerPose& listener) const;

        /// @brief Makes room in the one-shot pool for an incoming voice, or reports it rejected.
        ///
        /// Evicts the quietest pooled voice when the pool is full and the incoming voice is louder;
        /// returns false when the pool is full and the incoming voice would be the quietest.
        /// @param incomingGain The incoming voice's pre-spatialization gain.
        [[nodiscard]] bool ReserveOneShotSlot(f32 incomingGain);

        /// @brief The owning device.
        AudioDevice& m_Device;
        /// @brief Per-bus linear gain.
        std::array<f32, AudioBusCount> m_BusGain = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        /// @brief Per-bus low-pass cutoff in Hz (0 = bypass).
        std::array<f32, AudioBusCount> m_BusLowpassCutoff = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        /// @brief Per-bus reverb send, 0..1.
        std::array<f32, AudioBusCount> m_BusReverbSend = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        /// @brief The master reverb parameters.
        ReverbParams m_Reverb;
        /// @brief The voice table.
        std::array<Voice, MaxVoices> m_Voices;
        /// @brief Per-slot engine-owned-voice metadata, parallel to m_Voices.
        std::array<Managed, MaxVoices> m_Managed;
        /// @brief The one-track policy over the Music bus.
        Unique<MusicDirector> m_Music;
        /// @brief The last listener pose managed voices were spatialized against.
        ListenerPose m_Listener;
        /// @brief Sources awaiting reclamation.
        vector<Deferred> m_Deferred;
        /// @brief The last published snapshot serial.
        u64 m_PublishedSerial = 0;
        /// @brief The number of live voices.
        usize m_ActiveCount = 0;

        friend class MusicDirector;
    };

    /// @brief The one-track background-music policy over the Music bus.
    ///
    /// Keeps exactly one logical track playing, crossfading (equal-power) to a new track on Set and
    /// looping it. It holds at most two live Music voices — the crossfade pair — collapsing to one
    /// when a fade completes. It does not layer, stinger, or sequence; that richer interactive-music
    /// surface is a separate capability. A stream-mode (Encoded) clip is the expected long-track
    /// input, decoded incrementally through a streaming voice, and crossfades against a resident
    /// track identically; the crossfade pair may mix the two freely.
    class MusicDirector
    {
    public:
        /// @brief Constructs the director over its owning engine.
        /// @param engine The engine whose Music-bus voices the director drives.
        explicit MusicDirector(AudioEngine& engine);

        MusicDirector(const MusicDirector&) = delete;
        MusicDirector& operator=(const MusicDirector&) = delete;

        /// @brief Makes @p track the one logical background track, crossfading from the current one.
        ///
        /// Fades the outgoing track out and the incoming in over the transition's FadeSeconds (0 is a
        /// hard cut). Calling it with the already-playing track is a no-op — no re-trigger, no gain
        /// glitch.
        /// @param track      The clip to play as the background track.
        /// @param transition The crossfade duration and loop flag.
        void Set(const AssetHandle<AudioClip>& track, const MusicTransition& transition = {});

        /// @brief Fades the current track out over @p fadeSeconds, leaving the Music bus silent.
        /// @param fadeSeconds The fade-out duration in seconds; 0 stops immediately.
        void Stop(f32 fadeSeconds);

        /// @brief Sets the director's overall linear gain, scaling every Music voice.
        /// @param gain Linear gain (clamped to >= 0).
        void SetGain(f32 gain);

        /// @brief Returns the director's overall linear gain.
        [[nodiscard]] f32 GetGain() const { return m_Gain; }

        /// @brief Returns the current logical track, or an invalid handle when none plays.
        [[nodiscard]] AssetHandle<AudioClip> Current() const { return m_Current; }

        /// @brief Advances the crossfade envelope one frame, retuning and reaping Music voices.
        /// @param delta Time in seconds since the previous update.
        void Advance(f32 delta);

        /// @brief A live Music voice's state (test seam).
        struct VoiceState
        {
            /// @brief The voice handle.
            VoiceHandle Voice;
            /// @brief The clip it plays.
            AssetHandle<AudioClip> Clip;
            /// @brief Its current applied linear gain.
            f32 Gain = 0.0f;
            /// @brief Whether it is fading out (the outgoing member of the pair).
            bool FadingOut = false;
        };

        /// @brief Returns the live Music voices — one, or the crossfade pair (test seam).
        [[nodiscard]] vector<VoiceState> GetVoiceStates() const;

        /// @brief Returns the number of live Music voices (0, 1, or 2).
        [[nodiscard]] usize GetVoiceCount() const { return m_Tracks.size(); }

    private:
        /// @brief One live Music voice and its fade state.
        struct Track
        {
            /// @brief The engine voice handle.
            VoiceHandle Voice;
            /// @brief The clip it plays.
            AssetHandle<AudioClip> Clip;
            /// @brief Fade progress, 0..1.
            f32 Phase = 0.0f;
            /// @brief Fade duration in seconds (0 is an immediate transition).
            f32 FadeDuration = 0.0f;
            /// @brief Whether the track is fading out (true) or in / steady (false).
            bool FadingOut = false;
            /// @brief Whether the track has reached full gain and no longer fades.
            bool Steady = false;
            /// @brief Whether the voice loops.
            bool Loop = true;
        };

        /// @brief The applied linear gain of a track: its equal-power envelope times the overall gain.
        [[nodiscard]] f32 TrackGain(const Track& track) const;

        /// @brief Pushes a track's current gain into its engine voice.
        void Apply(const Track& track) const;

        /// @brief The owning engine.
        AudioEngine& m_Engine;
        /// @brief The current logical track (invalid when stopped).
        AssetHandle<AudioClip> m_Current;
        /// @brief The live voices: the incoming/steady track and at most one outgoing.
        vector<Track> m_Tracks;
        /// @brief The overall linear gain scaling every Music voice.
        f32 m_Gain = 1.0f;
    };
}
