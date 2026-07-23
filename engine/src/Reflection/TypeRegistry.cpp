#include <Veng/Reflection/TypeRegistry.h>

#include <Veng/Assert.h>

#include <memory>
#include <utility>

namespace Veng
{
    /// @brief The registry's type table, so no header-including TU instantiates it.
    struct TypeRegistry::Impl
    {
        /// @brief All registered types, keyed by their authored TypeId.
        unordered_map<TypeId, TypeInfo> Types;
    };

    TypeRegistry::TypeRegistry() : m_Impl(std::make_unique<Impl>()) {}

    TypeRegistry::~TypeRegistry() = default;

    TypeRegistry::TypeRegistry(TypeRegistry&& other) noexcept = default;

    TypeRegistry& TypeRegistry::operator=(TypeRegistry&& other) noexcept = default;

    void TypeRegistry::Insert(TypeId id, TypeInfo info)
    {
        const auto existing = m_Impl->Types.find(id);
        VE_ASSERT(existing == m_Impl->Types.end(),
                  "TypeId collision: '{}' and '{}' both claim TypeId {:#018x}", info.QualifiedName,
                  existing == m_Impl->Types.end() ? string{} : existing->second.Name, id);

        m_Impl->Types.emplace(id, std::move(info));
    }

    const TypeInfo& TypeRegistry::Info(TypeId id) const
    {
        const auto it = m_Impl->Types.find(id);
        VE_ASSERT(it != m_Impl->Types.end(), "TypeId {:#018x} is not registered", id);
        return it->second;
    }

    bool TypeRegistry::IsRegistered(TypeId id) const
    {
        return m_Impl->Types.contains(id);
    }

    usize TypeRegistry::Count() const
    {
        return m_Impl->Types.size();
    }

    const unordered_map<TypeId, TypeInfo>& TypeRegistry::All() const
    {
        return m_Impl->Types;
    }

    const TypeInfo* FindTypeByName(const TypeRegistry& registry, std::string_view name)
    {
        for (const auto& [id, info] : registry.All())
        {
            if (TypeNameMatches(info, name))
            {
                return &info;
            }
        }
        return nullptr;
    }
}
