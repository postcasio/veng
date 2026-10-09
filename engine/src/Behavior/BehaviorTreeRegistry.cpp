#include <Veng/Behavior/BehaviorTreeRegistry.h>

#include <Veng/Assert.h>
#include <Veng/Behavior/BehaviorTree.h>

#include <algorithm>
#include <utility>

namespace Veng
{
    namespace
    {
        /// @brief One registered tree: its entry, how to build it, and the tree once built.
        struct Registration
        {
            /// @brief The tree's id and display name.
            BehaviorTreeEntry Entry;
            /// @brief Builds the tree on its first resolve.
            function<Ref<BehaviorTree>()> Build;
            /// @brief The built tree, shared by every resolve after the first; null until then.
            Ref<BehaviorTree> Tree;
        };
    }

    /// @brief The catalog's registrations, in registration order.
    struct BehaviorTreeRegistry::Impl
    {
        /// @brief Every registered tree.
        vector<Registration> Trees;

        /// @brief Returns the registration claiming @p id, or null.
        [[nodiscard]] Registration* Find(const BehaviorTreeId id)
        {
            const auto it = std::ranges::find(Trees, id, [](const Registration& registration)
                                              { return registration.Entry.Id; });
            return it == Trees.end() ? nullptr : &*it;
        }
    };

    BehaviorTreeRegistry::BehaviorTreeRegistry() : m_Impl(CreateUnique<Impl>()) {}

    BehaviorTreeRegistry::~BehaviorTreeRegistry() = default;

    BehaviorTreeRegistry::BehaviorTreeRegistry(BehaviorTreeRegistry&&) noexcept = default;

    BehaviorTreeRegistry&
    BehaviorTreeRegistry::operator=(BehaviorTreeRegistry&&) noexcept = default;

    void BehaviorTreeRegistry::Register(const BehaviorTreeId id, string name,
                                        function<Ref<BehaviorTree>()> build)
    {
        VE_ASSERT(id != BehaviorTreeId::Null, "behaviour tree '{}' registered with the Null id",
                  name);
        VE_ASSERT(build != nullptr, "behaviour tree '{}' registered with no build function", name);
        const Registration* existing = m_Impl->Find(id);
        VE_ASSERT(existing == nullptr,
                  "BehaviorTreeId collision: '{}' and '{}' both claim BehaviorTreeId {:#018x}",
                  name, existing == nullptr ? string{} : existing->Entry.Name,
                  static_cast<u64>(id));
        m_Impl->Trees.push_back(Registration{
            .Entry = BehaviorTreeEntry{.Id = id, .Name = std::move(name)},
            .Build = std::move(build),
        });
    }

    vector<BehaviorTreeEntry> BehaviorTreeRegistry::Entries() const
    {
        vector<BehaviorTreeEntry> entries;
        entries.reserve(m_Impl->Trees.size());
        for (const Registration& registration : m_Impl->Trees)
        {
            entries.push_back(registration.Entry);
        }
        return entries;
    }

    optional<string> BehaviorTreeRegistry::FindName(const BehaviorTreeId id) const
    {
        if (const Registration* registration = m_Impl->Find(id))
        {
            return registration->Entry.Name;
        }
        return std::nullopt;
    }

    Ref<BehaviorTree> BehaviorTreeRegistry::Resolve(const BehaviorTreeId id) const
    {
        Registration* registration = m_Impl->Find(id);
        if (registration == nullptr)
        {
            return nullptr;
        }
        if (!registration->Tree)
        {
            registration->Tree = registration->Build();
            VE_ASSERT(registration->Tree != nullptr, "behaviour tree '{}' built a null tree",
                      registration->Entry.Name);
        }
        return registration->Tree;
    }

    usize BehaviorTreeRegistry::Count() const
    {
        return m_Impl->Trees.size();
    }
}
