#include <Veng/Reflection/TypeRegistry.h>

#include <Veng/Assert.h>

#include <atomic>
#include <memory>
#include <utility>

namespace Veng
{
    namespace
    {
        /// @brief Returns a registry serial no other registry in the process has held; never zero.
        u32 NextRegistrySerial()
        {
            static std::atomic<u32> s_Next{1};
            return s_Next.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /// @brief The registry's type table, so no header-including TU instantiates it.
    struct TypeRegistry::Impl
    {
        /// @brief All registered types, keyed by their authored TypeId.
        unordered_map<TypeId, TypeInfo> Types;
    };

    TypeRegistry::TypeRegistry() : m_Impl(std::make_unique<Impl>()), m_Serial(NextRegistrySerial())
    {
    }

    TypeRegistry::~TypeRegistry() = default;

    TypeRegistry::TypeRegistry(TypeRegistry&& other) noexcept
        : m_Impl(std::move(other.m_Impl)),
          m_Serial(std::exchange(other.m_Serial, NextRegistrySerial()))
    {
    }

    TypeRegistry& TypeRegistry::operator=(TypeRegistry&& other) noexcept
    {
        m_Impl = std::move(other.m_Impl);
        m_Serial = std::exchange(other.m_Serial, NextRegistrySerial());
        return *this;
    }

    void TypeRegistry::Insert(TypeId id, TypeInfo info)
    {
        const auto existing = m_Impl->Types.find(id);
        VE_ASSERT(existing == m_Impl->Types.end(),
                  "TypeId collision: '{}' and '{}' both claim TypeId {:#018x}", info.QualifiedName,
                  existing == m_Impl->Types.end() ? string{} : existing->second.Name, id);

        info.Ordinal = static_cast<u32>(m_Impl->Types.size());
        m_Impl->Types.emplace(id, std::move(info));
    }

    u32 TypeRegistry::OrdinalOf(TypeId id) const
    {
        const auto it = m_Impl->Types.find(id);
        return it != m_Impl->Types.end() ? it->second.Ordinal : InvalidTypeOrdinal;
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
