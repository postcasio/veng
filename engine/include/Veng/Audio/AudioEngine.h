#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/AudioBus.h>
#include <Veng/Audio/AudioBusGraph.h>
#include <Veng/Audio/AudioBuffer.h>
#include <Veng/Audio/AudioClip.h>
#include <Veng/Audio/AudioGenerator.h>
#include <Veng/Audio/Voice.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Scene/PresentationScope.h>

#include <array>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Veng::Audio
{
    class AudioDevice;
    class MusicDirector;
    struct StreamVoice;
    struct BufferedGenerator;

    /// @brief Parameters of a code-triggered non-spatial one-shot voice.
    struct OneShotParams
    {
        /// @brief The bus the voice mixes into; an id absent from the active graph routes to Master.
        BusId Bus = AudioBuses::SFX();
        /// @brief Linear gain, 0 = silent, 1 = unity.
        f32 Gain = 1.0f;
        /// @brief Playback pitch (resample ratio); 1 = the clip's native rate.
        f32 Pitch = 1.0f;
        /// @brief Whether the voice loops; a one-shot (false) retires when the clip ends.
        bool Loop = false;
    };

    /// @brief Parameters of a code-triggered spatial voice placed at a fixed world position.
    ///
    /// The voice is attenuated, panned, Doppler-shifted, and reverb-sent against the listener of the
    /// scope it plays in, exactly as an authored AudioSource is. A moving positioned voice is registered with
    /// PlayAt and then repositioned each frame through AudioEngine::SetVoicePose.
    struct SpatialOneShotParams
    {
        /// @brief The bus the voice mixes into; an id absent from the active graph routes to Master.
        BusId Bus = AudioBuses::SFX();
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

    /// @brief The authored mix of a positioned voice apart from its pose (AudioEngine::SetVoiceMix).
    ///
    /// What a caller retunes on a live PlayAt voice or spatial generator while SetVoicePose moves it:
    /// the engine re-spatializes the voice against its scope's listener from these and its pose.
    struct SpatialVoiceMix
    {
        /// @brief The bus the voice mixes into; an id absent from the active graph routes to Master.
        BusId Bus = AudioBuses::SFX();
        /// @brief Linear gain applied before spatialization; 0 = silent, 1 = unity.
        f32 Gain = 1.0f;
        /// @brief Base playback pitch (resample ratio); Doppler multiplies this.
        f32 Pitch = 1.0f;
        /// @brief Distance at or within which the voice plays at full Gain.
        f32 MinDistance = 1.0f;
        /// @brief Distance at or beyond which the voice is silent.
        f32 MaxDistance = 50.0f;
        /// @brief Occlusion low-pass drive, 0 = clear (bypass) to 1 = fully occluded.
        f32 OcclusionFactor = 0.0f;
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
        /// @brief The bus the voice mixes into; an id absent from the active graph routes to Master.
        BusId Bus = AudioBuses::SFX();
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
        /// @brief The presentation scope the voice belongs to.
        PresentationScopeId Scope;
        /// @brief The scope's state as the voice was last judged: Held is frozen, Muted is silent.
        PresentationState State = PresentationState::Live;
    };

    /// @brief How the music director transitions to a new track.
    struct MusicTransition
    {
        /// @brief Crossfade duration in seconds; 0 is a hard cut.
        f32 FadeSeconds = 0.0f;
        /// @brief Whether the incoming track loops.
        bool Loop = true;
    };

    /// @brief What one presentation scope asks the music director to play.
    ///
    /// A scope's standing request (AudioEngine::SetMusicRequest), held until it is replaced or the
    /// scope closes. Each frame the engine plays the request of the highest-priority eligible scope
    /// (see AudioEngine::Update); a request with no Track asks for silence at its priority.
    struct MusicRequest
    {
        /// @brief The track to play; an empty handle asks for silence.
        AssetHandle<AudioClip> Track;
        /// @brief The crossfade into this track in seconds (0 is a hard cut); also the fade-out when
        ///        this request was the last one playing and no request remains.
        f32 FadeSeconds = 0.0f;
        /// @brief Whether the track loops.
        bool Loop = true;
        /// @brief The request's priority: the highest eligible one plays.
        i32 Priority = 0;
    };

    /// @brief One scope's standing music request, as AudioEngine::GetMusicRequests lists it.
    struct MusicRequestInfo
    {
        /// @brief The requesting scope.
        PresentationScopeId Scope;
        /// @brief Its standing request.
        MusicRequest Request;
        /// @brief Whether the request could play this frame (its scope is eligible).
        bool Eligible = false;
    };

    /// @brief The mixer-facing, main-thread API: buses and voices.
    ///
    /// The engine holds the authoritative bus parameters and voice table, publishes an immutable
    /// snapshot to the device's mixing thread each frame, and reaps finished voices and reclaimed
    /// resources through the device's generation counter. Every call is main-thread only; the
    /// real-time thread never touches it.
    ///
    /// Every voice belongs to a presentation scope, named by whatever starts it, and the engine
    /// judges it by that scope's state: a Held voice is frozen, a Muted one advances silently, a
    /// Closed one is stopped. A scene's systems reach the engine through Audio::ScopedAudio, which
    /// names their scene's scope; the bus, reverb and device configuration here is device-wide.
    class AudioEngine
    {
    public:
        /// @brief Constructs the engine over its owning device.
        /// @param device The device whose snapshot bridge and generation counter the engine drives.
        /// @param scopes The registry every voice's scope is judged by; outlives the engine.
        AudioEngine(AudioDevice& device, const PresentationScopes& scopes);

        /// @brief Destroys the engine and its music director.
        ~AudioEngine();

        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;

        /// @brief Returns the registry the engine judges every voice's scope by.
        [[nodiscard]] const PresentationScopes& GetScopes() const { return m_Scopes; }

        /// @brief The device's negotiated output sample rate in Hz.
        ///
        /// The rate every registered generator's Render is invoked at. A consumer that must size
        /// rate-dependent state before its first Render — a generator allocating delay lines for an
        /// embedded reverb, say — reads the real rate here instead of assuming a default.
        [[nodiscard]] u32 GetOutputSampleRate() const;

        /// @brief Adopts a bus graph as the complete mixer topology (main thread).
        ///
        /// Replaces the active graph — the engine does not merge or extend. The graph is validated
        /// (single root Master, every parent resolves, acyclic, within the depth/count caps, no
        /// interned-hash collision); an invalid graph is rejected with a logged error and the
        /// roots-only default is installed instead. Each bus's gain is seeded from its
        /// AudioBusDef.DefaultGain. The flatten (a deterministic, index-stable child-before-parent
        /// order) is recomputed here and copied into the snapshot by the next Publish; the real-time
        /// thread's per-bus accumulators and filter state are not resized. Adopt at boot, before
        /// meaningful mixing: a mid-run re-topology may transiently reset per-bus filter state.
        /// @param graph  The complete authored graph.
        void ConfigureBusGraph(const AudioBusGraph& graph);

        /// @brief Returns the authored data of the active bus graph (the roots-only default when none).
        ///
        /// The complete topology currently installed — every bus with its parent and default DSP —
        /// so a consumer can enumerate the buses and their default gains without holding the graph
        /// asset (an audio-settings resolve pre-fills its output from these defaults). Reflects the
        /// last ConfigureBusGraph, or the roots-only default before any adoption.
        /// @return The active graph's authored data.
        [[nodiscard]] const AudioBusGraphData& GetActiveBusGraphData() const
        {
            return m_ActiveGraph;
        }

        /// @brief Resolves a bus name to its BusId against the active graph.
        ///
        /// A name the active graph declares returns its BusId; a name it does not declare returns
        /// the Master fallback id with a one-time warning. The graph-aware complement to the
        /// graph-independent BusId{name} constructor.
        /// @param name  The bus name.
        /// @return The bus's id, or Master's id when the name is absent.
        [[nodiscard]] BusId ResolveBus(std::string_view name);

        /// @brief Returns the name of the bus a BusId resolves to in the active graph.
        ///
        /// The graph's declared name for @p bus, or — for an id the graph does not declare, which
        /// routes to Master — the Master bus's name. A read seam for tooling (audio.list_voices).
        /// @param bus  The bus id.
        /// @return The resolved bus's name.
        [[nodiscard]] string GetBusName(BusId bus) const;

        /// @brief Sets a bus's linear gain; an unknown id targets Master with a one-time warning.
        /// @param bus  The bus.
        /// @param gain Linear gain (clamped to >= 0).
        void SetBusGain(BusId bus, f32 gain);
        /// @brief Returns a bus's linear gain (Master's when the id is unknown).
        [[nodiscard]] f32 GetBusGain(BusId bus) const;

        /// @brief Sets a bus's low-pass cutoff in Hz; 0 (or above Nyquist) bypasses the filter.
        ///
        /// Honoured only on a leaf bus (one with no children): applying a low-pass to a bus that
        /// sums children would filter that summed mix, which is hierarchical DSP routing and out of
        /// scope, so this call is ignored (with a one-time warning) on a non-leaf bus.
        /// @param bus       The bus.
        /// @param cutoffHz  Cutoff frequency in Hz.
        void SetBusLowpassCutoff(BusId bus, f32 cutoffHz);
        /// @brief Returns a bus's low-pass cutoff in Hz (0 for a non-leaf bus).
        [[nodiscard]] f32 GetBusLowpassCutoff(BusId bus) const;

        /// @brief Sets a bus's send level into the master reverb, 0..1.
        ///
        /// Honoured only on a leaf bus, for the same reason as SetBusLowpassCutoff: a non-leaf bus's
        /// send would route its summed children through the reverb, so the call is ignored (with a
        /// one-time warning) on a bus with children.
        /// @param bus  The bus.
        /// @param send Send level, clamped to 0..1.
        void SetBusReverbSend(BusId bus, f32 send);
        /// @brief Returns a bus's send level into the master reverb (0 for a non-leaf bus).
        [[nodiscard]] f32 GetBusReverbSend(BusId bus) const;

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
        /// The voice holds a reference to @p generator, so the generator lives as long as any thread
        /// can render it: once the voice stops (or is evicted), the engine's reference rides the same
        /// deferred reclamation as a buffer voice's source and is dropped on the main thread only after
        /// every audio thread is provably past it. The caller may keep its own reference to drive the
        /// generator's parameters, or drop it at any time; the generator is destroyed by whichever
        /// reference goes last, never on the real-time thread. Because a stopped voice's generator may
        /// still be rendered until it is reclaimed, a caller holding a reference changes it only
        /// through its GeneratorParams block, even after StopVoice. A Spatial voice is placed at
        /// params.Position and spatialized against its scope's listener exactly as a clip is (move it
        /// later with SetVoicePose); a non-spatial voice routes to its bus at params.Gain. A Buffered
        /// voice renders ahead of time on the fill thread into a ring the real-time callback only
        /// drains, moving heavy synthesis off the real-time thread; it is non-spatial, so a Buffered
        /// && Spatial request is rejected. A held generator is not rendered at all. Same budget
        /// arbitration as AddVoice.
        /// @param scope     The presentation scope the voice belongs to; a Closed one starts nothing.
        /// @param generator The sample source (must be non-null); the voice shares ownership of it.
        /// @param params    The voice registration parameters.
        /// @return A handle to the voice, or an invalid handle if it was rejected (the engine then
        ///         holds no reference to @p generator).
        VoiceHandle PlayGenerator(PresentationScopeId scope, Ref<IAudioGenerator> generator,
                                  const GeneratorVoiceParams& params);

        /// @brief Registers a voice playing a buffer, arbitrating against the voice budget.
        ///
        /// Takes a free slot when one exists; when the budget is full, evicts the quietest active
        /// voice if the incoming voice is louder, and otherwise rejects the request. A voice in a
        /// Muted scope ranks as silent, so it is evicted first. The rejected outcome is reported by
        /// an invalid handle.
        /// @param scope  The presentation scope the voice belongs to; a Closed one starts nothing.
        /// @param buffer The PCM source (held for the voice's lifetime).
        /// @param params The mix parameters.
        /// @return A handle to the voice, or an invalid handle if it was rejected.
        VoiceHandle AddVoice(PresentationScopeId scope, const Ref<AudioBuffer>& buffer,
                             const VoiceParams& params);

        /// @brief Registers a voice for a clip, choosing the resident or streaming path by storage.
        ///
        /// A Pcm clip plays through AddVoice off its resident buffer; an Encoded clip plays through a
        /// streaming voice, decoded incrementally on the engine's decode thread and drained by the
        /// mixer exactly as a resident voice is — indistinguishable downstream. Same budget
        /// arbitration as AddVoice. A null or unresident clip is rejected with an invalid handle.
        /// @param scope  The presentation scope the voice belongs to; a Closed one starts nothing.
        /// @param clip   The clip to play.
        /// @param params The mix parameters.
        /// @return A handle to the voice, or an invalid handle if it was rejected.
        VoiceHandle AddClipVoice(PresentationScopeId scope, const AssetHandle<AudioClip>& clip,
                                 const VoiceParams& params);

        /// @brief Returns whether a handle still names a live voice.
        /// @param voice The handle.
        [[nodiscard]] bool IsVoiceLive(VoiceHandle voice) const;

        /// @brief Updates a live voice's mix parameters (no effect on a stale handle).
        /// @param voice  The handle.
        /// @param params The new parameters.
        void SetVoiceParams(VoiceHandle voice, const VoiceParams& params);

        /// @brief Stops a voice and routes its source to reclamation (no effect on a stale handle).
        ///
        /// Never blocks. The voice's source — a buffer, a stream, or a generator — is queued for
        /// deferred free and released once the mixing thread's consumed serial passes the last
        /// snapshot that could name it, so it can never be freed mid-mix. A stream or buffered
        /// generator voice adds its off-thread party (the decode or fill thread) to that handshake:
        /// the source is released only once that thread has also acknowledged the removal. The
        /// release happens inside the device's Pump, on the main thread.
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
        /// engine one-shot pool; when the pool is full the quietest pooled voice (a Muted one ranking
        /// as silent) is dropped if the incoming voice is louder, and otherwise the request is
        /// rejected. An Encoded clip plays
        /// through the streaming path like any other; an unresident clip plays nothing.
        /// @param scope  The presentation scope the voice belongs to; a Closed one starts nothing.
        /// @param clip   The clip to play.
        /// @param params The mix parameters (bus, gain, pitch, loop).
        /// @return A handle to the voice for early stop, or an invalid handle if it was rejected.
        VoiceHandle PlayOneShot(PresentationScopeId scope, const AssetHandle<AudioClip>& clip,
                                const OneShotParams& params = {});

        /// @brief Fires a spatial one-shot voice at a fixed world position.
        ///
        /// The voice is spatialized against its scope's listener like an authored AudioSource. It is
        /// fixed by default; call SetVoicePose each frame to move it. Same pool policy as
        /// PlayOneShot; an Encoded clip plays through the streaming path, an unresident clip nothing.
        /// @param scope    The presentation scope the voice belongs to; a Closed one starts nothing.
        /// @param clip     The clip to play.
        /// @param worldPos The voice's world position.
        /// @param params   The spatial mix parameters.
        /// @return A handle to the voice for early stop or repositioning, or an invalid handle.
        VoiceHandle PlayAt(PresentationScopeId scope, const AssetHandle<AudioClip>& clip,
                           vec3 worldPos, const SpatialOneShotParams& params = {});

        /// @brief Repositions an imperatively-placed spatial voice (no effect on a stale handle).
        ///
        /// Folds the new pose into the next published snapshot. Applies only to a voice registered
        /// spatial through PlayAt (a non-spatial one-shot ignores it); the general capability a
        /// consumer tracking a moving emitter every frame reaches for.
        /// @param voice    The handle returned by PlayAt.
        /// @param worldPos The new world position.
        /// @param velocity The new world velocity, units per second (for Doppler).
        void SetVoicePose(VoiceHandle voice, vec3 worldPos, vec3 velocity);

        /// @brief Retunes a positioned voice's authored mix, keeping its pose (no effect on a stale
        ///        handle or a voice that is not positioned).
        ///
        /// The companion of SetVoicePose for a caller whose emitter's gain, rolloff or occlusion
        /// changes while it plays: the voice is re-spatialized against its scope's listener from
        /// @p mix and its current pose. Applies to a PlayAt voice and a spatial generator voice.
        /// @param voice The handle returned by PlayAt or a spatial PlayGenerator.
        /// @param mix   The voice's new bus, base gain and pitch, rolloff and occlusion.
        void SetVoiceMix(VoiceHandle voice, const SpatialVoiceMix& mix);

        /// @brief Returns whether the engine still holds @p generator for any voice.
        ///
        /// True while a voice renders it, and after that voice stops until the reclamation
        /// handshake releases the engine's reference (for a buffered voice, once the fill thread has
        /// acknowledged the removal too). A caller restarting a voice on the same generator waits for
        /// false: registering an instance the engine still holds would have two voices render it.
        /// @param generator The generator.
        [[nodiscard]] bool IsGeneratorInUse(const IAudioGenerator& generator) const;

        /// @brief Sets the listener a scope's spatial voices are spatialized against.
        ///
        /// One pose per open scope: a scene's AudioSystem sets its scene's every View update, so a
        /// voice is placed against the listener of the scene that started it, whatever order the
        /// worlds tick in. A scope that never set one listens from the origin; a closed scope's pose is
        /// dropped at the next Update.
        /// @param scope    The scope whose listener this is.
        /// @param listener The listener pose.
        void SetListener(PresentationScopeId scope, const ListenerPose& listener);

        /// @brief Returns a scope's listener pose, or the origin pose when it set none.
        /// @param scope The scope.
        [[nodiscard]] ListenerPose GetListener(PresentationScopeId scope) const;

        /// @brief Judges every voice by its scope, re-spatializes the positioned ones, arbitrates the
        ///        music requests and advances the music director — once per frame.
        ///
        /// Per voice, by its scope's state as the registry latched it this frame: Closed stops it
        /// (through the ordinary reclamation handshake), Held publishes it frozen, Muted publishes it
        /// at zero gain while it advances, and Live publishes it as authored. Then every spatial
        /// voice is re-spatialized against its own scope's listener. Then the music requests are
        /// arbitrated: a closed scope's request is dropped, and among the eligible ones — the
        /// application scope's, a Live scope's, and a Held scope's whose scene is still presented
        /// (it has a presentation rank) — the highest Priority wins, ties going to the lower
        /// presentation rank (a ranked scope beats an unranked one) and then to the older scope. A
        /// Muted scope's request is never eligible. The director crossfades to the winner's track
        /// when its track or loop differs from what plays, and fades out over the last winner's fade
        /// when nothing is eligible. Finally the music crossfade advances by @p delta. The
        /// application calls it in its presentation step, right after PresentationScopes::Resolve and
        /// never from a system, so the crossfade advances once a frame however many worlds run.
        /// @param delta Time in seconds since the previous update.
        void Update(f32 delta);

        /// @brief Sets or withdraws a scope's standing music request.
        ///
        /// The request stands until it is replaced or the scope closes, so a scene that stops
        /// submitting — a paused one, whose View phase does not run — keeps its last request. A
        /// scene's AudioSystem submits its MusicState here every View update; application code
        /// submits through Application::GetApplicationAudio. Takes effect at the next Update.
        /// @param scope    The requesting scope; a closed scope's request is refused.
        /// @param request  The request, or nullopt to withdraw it.
        void SetMusicRequest(PresentationScopeId scope, const optional<MusicRequest>& request);

        /// @brief Lists every standing music request, oldest scope first (for tooling and tests).
        /// @return One entry per scope holding a request, with its eligibility as of the last Update.
        [[nodiscard]] vector<MusicRequestInfo> GetMusicRequests() const;

        /// @brief Returns the scope whose request the last Update chose, or an invalid id for none.
        [[nodiscard]] PresentationScopeId GetMusicWinner() const { return m_MusicWinner; }

        /// @brief Returns the music director, the one-track policy over the Music bus.
        ///
        /// For inspection and the music volume; what it plays is decided by the music requests.
        [[nodiscard]] MusicDirector& Music();

        /// @brief Returns a live voice's current mix parameters, or nullopt for a stale handle.
        /// @param voice The handle.
        [[nodiscard]] optional<VoiceParams> GetVoiceParams(VoiceHandle voice) const;

        /// @brief Returns the number of live one-shot / positioned voices in the pool (test seam).
        [[nodiscard]] usize GetManagedVoiceCount() const;

        /// @brief Returns the number of sources awaiting deferred reclamation (test seam).
        ///
        /// A retired buffer, streaming, or generator source stays counted here until the reclamation
        /// handshake lets it free, so a test can watch a source outlive its stop and then be released.
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
            /// @brief The presentation scope the voice belongs to.
            PresentationScopeId Scope;
            /// @brief The scope's state as the voice was last judged (at start, then every Update).
            PresentationState State = PresentationState::Held;
            /// @brief The owned PCM source (null for a generator or stream voice).
            Ref<AudioBuffer> Source;
            /// @brief The shared on-demand source (null for a buffer or stream voice).
            Ref<IAudioGenerator> Generator;
            /// @brief Rendered channel count of a generator voice: 1 (mono) or 2 (interleaved stereo).
            u32 GeneratorChannels = 1;
            /// @brief The owned streaming source (null for a buffer or generator voice).
            Unique<StreamVoice> Stream;
            /// @brief The owned buffered-generator ring wrapper (null unless the voice is buffered).
            ///
            /// Holds its own reference to Generator, which the fill thread renders off the real-time
            /// thread; the wrapper rides the reclamation handshake and releases that reference with it.
            Unique<BufferedGenerator> Buffered;
            /// @brief The mix parameters.
            VoiceParams Params;
        };

        /// @brief A source awaiting reclamation once the referencing threads are provably past it.
        struct Deferred
        {
            /// @brief The buffer source to release (set for a buffer voice).
            Ref<AudioBuffer> Source;
            /// @brief The generator to release (set for a plain generator voice).
            ///
            /// A buffered generator voice releases its generator through Buffered instead, since the
            /// fill thread renders it and must acknowledge the removal first.
            Ref<IAudioGenerator> Generator;
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
            BusId Bus = AudioBuses::SFX();
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
        /// @param scope  The presentation scope the voice belongs to.
        /// @param clip   The Encoded clip to stream.
        /// @param params The mix parameters.
        /// @return A handle to the voice, or an invalid handle if it was rejected or undecodable.
        VoiceHandle AddStreamVoice(PresentationScopeId scope, const AssetHandle<AudioClip>& clip,
                                   const VoiceParams& params);

        /// @brief Returns whether a voice may start in @p scope: false once it has closed.
        [[nodiscard]] bool CanStartIn(PresentationScopeId scope) const;

        /// @brief Returns whether a scope's music request may play this frame.
        ///
        /// The application scope always; a Live scope; a Held scope whose scene is still presented
        /// (paused on screen); never a Muted or Closed one.
        [[nodiscard]] bool IsMusicEligible(PresentationScopeId scope) const;

        /// @brief Drops closed scopes' music requests and points the director at the winner.
        void ArbitrateMusic();

        /// @brief Stamps a freshly taken slot with its scope and that scope's current state.
        ///
        /// A voice started in a Held scope is published held from its first snapshot, so a voice
        /// started while its scene is paused or not yet renewed stays fresh until the lease returns.
        void BindScope(u32 slot, PresentationScopeId scope);

        /// @brief Deactivates a slot and routes its source to reclamation.
        void RetireSlot(u32 slot);

        /// @brief Fires a clip into the one-shot pool: the shared body of PlayOneShot and PlayAt.
        /// @param scope    The presentation scope the voice belongs to.
        /// @param clip     The clip to play.
        /// @param managed  The voice's managed metadata (its kind, base gain and pose).
        /// @param params   The voice's initial mix parameters.
        /// @return A handle to the voice, or an invalid handle if it was rejected.
        VoiceHandle FireManaged(PresentationScopeId scope, const AssetHandle<AudioClip>& clip,
                                const Managed& managed, const VoiceParams& params);

        /// @brief Reserves a voice slot, evicting the quietest active voice when the budget is full.
        ///
        /// Returns a free slot, or the slot of a voice evicted because @p incomingGain is louder,
        /// or InvalidSlot when the budget is full and the incoming voice would be the quietest. A
        /// voice is ranked at its authored gain, or at zero while its scope is Muted.
        /// @param incomingGain The incoming voice's pre-spatialization gain.
        [[nodiscard]] u32 AllocateSlot(f32 incomingGain);

        /// @brief Folds a positioned voice's metadata and the listener into final mix parameters.
        [[nodiscard]] VoiceParams SpatializeManaged(const Managed& managed,
                                                    const ListenerPose& listener) const;

        /// @brief Makes room in the one-shot pool for an incoming voice, or reports it rejected.
        ///
        /// Evicts the quietest pooled voice when the pool is full and the incoming voice is louder;
        /// returns false when the pool is full and the incoming voice would be the quietest. A
        /// voice is ranked at its base gain, or at zero while its scope is Muted.
        /// @param incomingGain The incoming voice's pre-spatialization gain.
        [[nodiscard]] bool ReserveOneShotSlot(f32 incomingGain);

        /// @brief One bus of the flattened, deterministic child-before-parent bus order.
        ///
        /// The control thread's view of the active graph: an index-stable topological order (a
        /// child strictly before its parent, Master last) that Publish copies into the snapshot's
        /// flat POD arrays for the real-time fold. A fixed graph yields the same order across
        /// republishes, so a bus keeps its slot and its persistent filter state stays coherent.
        struct BusRuntime
        {
            /// @brief The bus's interned id.
            BusId Id;
            /// @brief The bus's declared name (for GetBusName / tooling readout).
            string Name;
            /// @brief Index of this bus's parent in the flattened order (Master indexes itself).
            u32 ParentIndex = 0;
            /// @brief The bus's current linear gain; seeded from DefaultGain, set by SetBusGain.
            f32 Gain = 1.0f;
            /// @brief The bus's low-pass cutoff in Hz (leaf-only; a non-leaf bus keeps 0).
            f32 LowpassCutoff = 0.0f;
            /// @brief The bus's reverb send, 0..1 (leaf-only; a non-leaf bus keeps 0).
            f32 ReverbSend = 0.0f;
            /// @brief Whether the bus has no children — DSP is honoured only when true.
            bool IsLeaf = true;
        };

        /// @brief Flattens a graph into m_Buses / m_BusIndexById / m_MasterIndex (input assumed valid).
        /// @param data  The graph to install.
        void InstallGraph(const AudioBusGraphData& data);

        /// @brief Resolves a BusId to its flattened index, or the Master index when unknown (no warn).
        /// @param bus  The bus id.
        /// @return The bus's index, or the Master index.
        [[nodiscard]] u32 ResolveBusIndex(BusId bus) const;

        /// @brief Resolves a BusId to its index, warning once when the id is absent (routes to Master).
        /// @param bus  The bus id.
        /// @return The bus's index, or the Master index.
        u32 ResolveBusIndexWarn(BusId bus);

        /// @brief The owning device.
        AudioDevice& m_Device;
        /// @brief The registry every voice's scope is judged by.
        const PresentationScopes& m_Scopes;
        /// @brief The authored data of the active graph, kept for GetActiveBusGraphData.
        ///
        /// The topology as adopted (or the roots-only default), retained so a consumer can read each
        /// bus's default gain; the flattened runtime view lives in m_Buses.
        AudioBusGraphData m_ActiveGraph;
        /// @brief The active graph's buses in flattened child-before-parent order (Master last).
        vector<BusRuntime> m_Buses;
        /// @brief BusId value → index into m_Buses, for O(1) resolution.
        std::unordered_map<u64, u32> m_BusIndexById;
        /// @brief Index of the Master (root) bus in m_Buses — the fallback for an unknown id.
        u32 m_MasterIndex = 0;
        /// @brief Ids already warned about as absent, so the fallback warning fires once per id.
        std::unordered_set<u64> m_WarnedMissingBuses;
        /// @brief The master reverb parameters.
        ReverbParams m_Reverb;
        /// @brief The voice table.
        std::array<Voice, MaxVoices> m_Voices;
        /// @brief Per-slot engine-owned-voice metadata, parallel to m_Voices.
        std::array<Managed, MaxVoices> m_Managed;
        /// @brief The one-track policy over the Music bus.
        Unique<MusicDirector> m_Music;
        /// @brief Each scope's listener pose, keyed by scope id; a scope absent here listens from the
        ///        origin. Pruned of closed scopes by Update.
        std::unordered_map<u64, ListenerPose> m_Listeners;
        /// @brief Each scope's standing music request, in ascending scope id (oldest first). Pruned
        ///        of closed scopes by Update.
        vector<std::pair<PresentationScopeId, MusicRequest>> m_MusicRequests;
        /// @brief The scope whose request the last Update chose; invalid for none.
        PresentationScopeId m_MusicWinner;
        /// @brief The last winner's fade, the fade-out when no request remains.
        f32 m_MusicFade = 0.0f;
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
    /// Driven by the engine's music arbitration (AudioEngine::SetMusicRequest), which is its one
    /// caller: it keeps exactly one logical track playing, crossfading (equal-power) to a new track
    /// when the winning request changes and looping it. It holds at most two live Music voices — the crossfade pair — collapsing to one
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

        /// @brief Sets the director's overall linear gain, scaling every Music voice.
        /// @param gain Linear gain (clamped to >= 0).
        void SetGain(f32 gain);

        /// @brief Returns the director's overall linear gain.
        [[nodiscard]] f32 GetGain() const { return m_Gain; }

        /// @brief Returns the current logical track, or an invalid handle when none plays.
        [[nodiscard]] AssetHandle<AudioClip> Current() const { return m_Current; }

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
        friend class AudioEngine;

        /// @brief Makes @p track the one logical background track, crossfading from the current one.
        ///
        /// Fades the outgoing track out and the incoming in over the transition's FadeSeconds (0 is a
        /// hard cut). Calling it with the current track and loop is a no-op — no re-trigger, no gain
        /// glitch — except that a track whose clip was still loading when it became current starts
        /// once it is resident. An empty @p track fades the current one out.
        /// @param track      The clip to play as the background track.
        /// @param transition The crossfade duration and loop flag.
        void Set(const AssetHandle<AudioClip>& track, const MusicTransition& transition);

        /// @brief Fades the current track out over @p fadeSeconds, leaving the Music bus silent.
        /// @param fadeSeconds The fade-out duration in seconds; 0 stops immediately.
        void Stop(f32 fadeSeconds);

        /// @brief Advances the crossfade envelope one frame, retuning and reaping Music voices.
        /// @param delta Time in seconds since the previous update.
        void Advance(f32 delta);

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

        /// @brief Starts @p track's voice as the incoming member of the pair, fading in.
        /// @param track      The clip to start; nothing starts when it is not resident.
        /// @param transition The fade-in duration and loop flag.
        void StartIncoming(const AssetHandle<AudioClip>& track, const MusicTransition& transition);

        /// @brief The owning engine.
        AudioEngine& m_Engine;
        /// @brief The current logical track (invalid when stopped).
        AssetHandle<AudioClip> m_Current;
        /// @brief Whether the current logical track loops.
        bool m_CurrentLoop = true;
        /// @brief The live voices: the incoming/steady track and at most one outgoing.
        vector<Track> m_Tracks;
        /// @brief The overall linear gain scaling every Music voice.
        f32 m_Gain = 1.0f;
    };
}
