#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeId.h>

namespace Veng
{
    class BehaviorTree;

    /// @brief Stable identity of a registered behaviour tree, authored exactly like a SystemId.
    ///
    /// A module registers each tree it builds in code under one of these, and a BehaviorTreeRef
    /// component names the tree an entity runs by it, so a prefab or a level selects a behaviour
    /// with no code. A u64 leaf in the SystemId/GuiDriverId id family, minted with
    /// `vengc generate-id`, authored in JSON as a hex-id string. Null is the reserved empty id.
    enum class BehaviorTreeId : u64
    {
        /// @brief The empty id, distinct from every minted tree id; names no tree.
        Null = 0
    };

    /// @brief One registered tree's catalog entry: its identity and display name.
    struct BehaviorTreeEntry
    {
        /// @brief The tree's stable identity, the catalog key.
        BehaviorTreeId Id = BehaviorTreeId::Null;
        /// @brief The tree's display name (logs and the editor's picker).
        string Name;
    };

    /// @brief The catalog of behaviour trees a module builds in code, resolved by authored id.
    ///
    /// The tree counterpart of SystemRegistry and GuiDriverRegistry: a module registers a build
    /// function per tree during VengModuleRegister (through `host->Systems.GetBehaviorTrees()`,
    /// since the system catalog already reaches every host that loads a module), and a
    /// BehaviorTreeRef's id resolves against it. Enumerating the catalog builds nothing; a tree is
    /// built on its first Resolve and the same shared, immutable tree is returned after, so every
    /// agent naming one id runs one tree. Registration is GPU-free, so the cooker can hold one.
    ///
    /// Resolve builds lazily and caches, so it is main-thread only, like the simulation that calls
    /// it. Move-only, its storage behind an implementation pointer.
    class VE_API BehaviorTreeRegistry
    {
    public:
        /// @brief Constructs an empty catalog.
        BehaviorTreeRegistry();
        /// @brief Destroys the catalog and every tree it built.
        ~BehaviorTreeRegistry();
        /// @brief Moves a catalog, leaving the source empty and unusable.
        BehaviorTreeRegistry(BehaviorTreeRegistry&&) noexcept;
        /// @brief Move-assigns a catalog.
        /// @return This catalog.
        BehaviorTreeRegistry& operator=(BehaviorTreeRegistry&&) noexcept;
        BehaviorTreeRegistry(const BehaviorTreeRegistry&) = delete;
        BehaviorTreeRegistry& operator=(const BehaviorTreeRegistry&) = delete;

        /// @brief Registers a tree under its authored id.
        ///
        /// The build function runs at most once, on the first Resolve of @p id, and must return a
        /// non-null tree; it is never run by registration or enumeration.
        /// @param id     The tree's minted id; must not be Null.
        /// @param name   The display name the editor's picker shows.
        /// @param build  Builds the tree.
        /// @pre No other tree already claims @p id.
        /// @warning Registering two trees under one id is a fatal collision assert.
        void Register(BehaviorTreeId id, string name, function<Ref<BehaviorTree>()> build);

        /// @brief Returns every registered tree's id and name, in registration order, building none.
        /// @return The catalog entries.
        [[nodiscard]] vector<BehaviorTreeEntry> Entries() const;

        /// @brief Returns the display name registered for @p id.
        /// @param id  The tree id.
        /// @return The name, or nullopt when no tree claims @p id.
        [[nodiscard]] optional<string> FindName(BehaviorTreeId id) const;

        /// @brief Resolves an id to its shared tree, building it on the first call.
        /// @param id  The tree id.
        /// @return The tree, or null for Null or an id no tree claims.
        [[nodiscard]] Ref<BehaviorTree> Resolve(BehaviorTreeId id) const;

        /// @brief Returns the number of registered trees.
        /// @return The tree count.
        [[nodiscard]] usize Count() const;

    private:
        struct Impl;
        /// @brief The entries, build functions and built trees.
        Unique<Impl> m_Impl;
    };
}

VE_LEAF(::Veng::BehaviorTreeId, 0x5910A5AC475278B0ULL, ::Veng::FieldClass::Scalar);
