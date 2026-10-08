#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class PresentationScopes;

    /// @brief A stable, never-reused identity for a presentation scope.
    ///
    /// What a device engine tags the output a scope owns with — a voice, a rumble, a music request —
    /// and asks PresentationScopes::GetState about once per frame. Minted by PresentationScopes::Open
    /// from a counter that never repeats for the registry's life, so a closed scope's id reads Closed
    /// forever rather than naming a later scope. Zero is the invalid, names-no-scope id.
    struct PresentationScopeId
    {
        /// @brief The identity value; zero is the invalid, names-no-scope id.
        u64 Value = 0;

        /// @brief Returns whether this id names a minted scope.
        [[nodiscard]] bool IsValid() const { return Value != 0; }

        /// @brief Member-wise equality on the identity value.
        bool operator==(const PresentationScopeId&) const = default;
    };

    /// @brief What the output a presentation scope owns does this frame.
    ///
    /// Latched for every open scope by PresentationScopes::Resolve, once per frame, from whether the
    /// scope's lease was renewed and whether that renewal was audible. The contract a device engine
    /// implements for each state is stated on the enumerators.
    enum class PresentationState : u8
    {
        /// @brief Renewed this frame by a presented scene: owned output advances and is heard or felt.
        Live,
        /// @brief Renewed this frame by an unpresented scene: owned output advances in time and
        /// produces nothing (a voice mixes at zero gain, a rumble contributes zero), so the scene
        /// resumes in step when it becomes presented.
        Muted,
        /// @brief Not renewed this frame: owned output is frozen — a voice does not advance its
        /// cursor, a rumble does not advance its time, nothing is heard or felt — and resumes exactly
        /// where it stopped once the lease is renewed. A freshly opened scope is Held until its first
        /// renewal.
        Held,
        /// @brief Closed, or never handed out: everything the scope owns is stopped and released at
        /// the device's next update.
        Closed,
    };

    /// @brief One scope's identity and latched state, as PresentationScopes::GetScopes lists it.
    struct PresentationScopeStatus
    {
        /// @brief The scope's identity.
        PresentationScopeId Id;
        /// @brief The state the last Resolve latched (Held for a scope opened since).
        PresentationState State = PresentationState::Held;
        /// @brief The presentation rank the last Resolve latched, or nullopt when nothing presented
        ///        the scope's scene (see PresentationScopes::SetPresentationRank).
        optional<u32> Rank;
    };

    /// @brief The owning handle of one presentation scope: renews its lease, and closes it on drop.
    ///
    /// A scene owns one (Scene::SetPresentationScope), as it owns its effect pool, so the scope lives
    /// exactly as long as the scene: destroying the scene, or detaching the handle, closes the scope,
    /// and that closure is what ends everything the scope owns. Nothing else holds a pointer to it.
    /// Obtained only from PresentationScopes::Open; neither copyable nor movable, since the registry
    /// it closes into is borrowed for its life.
    class PresentationScope
    {
    public:
        /// @brief Closes the scope in its registry; its id reads Closed from here on.
        ~PresentationScope();

        PresentationScope(const PresentationScope&) = delete;
        PresentationScope& operator=(const PresentationScope&) = delete;
        PresentationScope(PresentationScope&&) = delete;
        PresentationScope& operator=(PresentationScope&&) = delete;

        /// @brief Returns the scope's identity, the tag a device engine files its owned output under.
        [[nodiscard]] PresentationScopeId GetId() const { return m_Id; }

        /// @brief Renews the scope's lease for the current frame.
        ///
        /// The owning scene's View phase calls it once per frame (Scene::TickSimulationPhase), so a
        /// scene whose View phase does not run — paused, unstarted, or on a process that runs no View
        /// phase — leaves its scope Held. Several renewals in one frame are one renewal, audible when
        /// any of them was.
        /// @param audible  Whether the scene is presented this frame (its SystemContext::View is set).
        void Renew(bool audible);

    private:
        friend class PresentationScopes;

        /// @brief Binds the handle to its registry and minted id; only PresentationScopes::Open calls it.
        PresentationScope(PresentationScopes& registry, PresentationScopeId id);

        /// @brief The registry the scope renews into and closes into; outlives the handle.
        PresentationScopes* m_Registry;
        /// @brief The scope's minted identity.
        PresentationScopeId m_Id;
    };

    /// @brief The registry of every open presentation scope and the state each one latched this frame.
    ///
    /// Device-free and main-thread only, like the rest of the frame. Application owns one beside its
    /// device engines and hands it to its WorldRunner, which installs a scope on every scene it holds;
    /// once per frame, after every scene's View phase and after OnUpdate, Application calls Resolve and
    /// then runs each device engine's once-per-frame update, which reads the latched states. One
    /// reserved application scope (GetApplicationScope) owns what plays outside any scene and is
    /// always Live.
    ///
    /// Every handle Open returns borrows the registry, so the registry must outlive them all — an
    /// owner declares it ahead of whatever holds the scenes. Destroying it while a scope it handed out
    /// is still open asserts.
    class PresentationScopes
    {
    public:
        /// @brief Constructs the registry holding only the application scope.
        PresentationScopes();

        /// @brief Destroys the registry.
        /// @pre Every scope Open handed out has closed.
        ~PresentationScopes();

        PresentationScopes(const PresentationScopes&) = delete;
        PresentationScopes& operator=(const PresentationScopes&) = delete;
        PresentationScopes(PresentationScopes&&) = delete;
        PresentationScopes& operator=(PresentationScopes&&) = delete;

        /// @brief Mints a scope and returns its owning handle.
        ///
        /// The scope reads Held until its first renewal is latched by Resolve.
        /// @return The handle; dropping it closes the scope.
        [[nodiscard]] Unique<PresentationScope> Open();

        /// @brief Returns a scope's state as the last Resolve latched it.
        ///
        /// A scope opened since the last Resolve reads Held. Closure is immediate: a closed scope reads
        /// Closed from the moment its handle drops, without waiting for a Resolve, as does an invalid
        /// id and one this registry never handed out.
        /// @param id  The scope to query.
        /// @return Its latched state, or Closed.
        [[nodiscard]] PresentationState GetState(PresentationScopeId id) const;

        /// @brief Latches every open scope's state from this frame's renewals, then clears them.
        ///
        /// A scope renewed audible reads Live, one renewed only inaudibly reads Muted, and one not
        /// renewed reads Held; the application scope reads Live regardless. A lease is therefore
        /// exactly one frame. Each scope's presentation rank is latched the same way, from the ranks
        /// stamped since the last Resolve. Called once per frame, before the device engines' updates.
        void Resolve();

        /// @brief Stamps where a scope's scene is presented this frame, for the next Resolve to latch.
        ///
        /// The rank is the position, in the compositor's registration order, of the first viewport
        /// presenting the scope's scene — 0 is the primary viewport — so a device engine choosing
        /// between scopes (the music arbitration) can prefer the one on the primary viewport. Unlike
        /// the lease it is independent of the View phase, so a paused scene still on screen keeps its
        /// rank. Several stamps in one frame keep the lowest; a scope not stamped latches no rank. The
        /// application stamps every scope it presents in its presentation step, just before Resolve.
        /// @param id    The scope whose scene is presented; the application scope and an id not open
        ///              are ignored.
        /// @param rank  The presenting viewport's registration index.
        void SetPresentationRank(PresentationScopeId id, u32 rank);

        /// @brief Returns a scope's presentation rank as the last Resolve latched it.
        /// @param id  The scope to query.
        /// @return Its rank, or nullopt when nothing presented its scene (always for the application
        ///         scope, a closed scope and an id never handed out).
        [[nodiscard]] optional<u32> GetPresentationRank(PresentationScopeId id) const;

        /// @brief Returns the reserved scope owning what plays outside any scene.
        ///
        /// A debug panel's rumble test, an editor's audition of a clip, application-level code: the
        /// one sanctioned application-owned path. It is always Live — never held, never muted — and
        /// closes only with the registry. Nothing a scene's system starts belongs in it.
        [[nodiscard]] PresentationScopeId GetApplicationScope() const { return m_Application; }

        /// @brief Lists every open scope, the application scope first, with its latched state.
        ///
        /// For tooling and tests; a device engine asks GetState about the ids it holds instead.
        /// @return One entry per open scope, in ascending id order.
        [[nodiscard]] vector<PresentationScopeStatus> GetScopes() const;

    private:
        friend class PresentationScope;

        /// @brief One open scope: its latched state and this frame's renewal.
        struct Record
        {
            /// @brief The scope's identity.
            PresentationScopeId Id;
            /// @brief The state the last Resolve latched.
            PresentationState State = PresentationState::Held;
            /// @brief Whether the scope was renewed since the last Resolve.
            bool Renewed = false;
            /// @brief Whether any renewal since the last Resolve was audible.
            bool Audible = false;
            /// @brief The rank the last Resolve latched.
            optional<u32> Rank;
            /// @brief The lowest rank stamped since the last Resolve.
            optional<u32> PendingRank;
        };

        /// @brief Records a renewal of an open scope (PresentationScope::Renew).
        /// @param id       The scope renewed.
        /// @param audible  Whether the renewing scene is presented.
        void Renew(PresentationScopeId id, bool audible);

        /// @brief Removes a closed scope (PresentationScope's destructor).
        /// @param id  The scope closed.
        void Close(PresentationScopeId id);

        /// @brief Returns an open scope's record, or null when the id is not open.
        /// @param id  The scope to find.
        [[nodiscard]] const Record* Find(PresentationScopeId id) const;

        /// @brief Returns an open scope's record for writing, or null when the id is not open.
        /// @param id  The scope to find.
        [[nodiscard]] Record* Find(PresentationScopeId id);

        /// @brief The open scopes in ascending id order; minting appends, so the order is free.
        vector<Record> m_Records;

        /// @brief The reserved, always-Live application scope.
        PresentationScopeId m_Application;

        /// @brief The counter minting scope ids; never reused, so a closed id resolves to nothing.
        u64 m_NextId = 1;
    };
}
