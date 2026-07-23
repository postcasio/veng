#include <Veng/Asset/AssetLoaderRegistry.h>

#include <Veng/Assert.h>

#include <memory>
#include <unordered_map>
#include <utility>

namespace Veng
{
    /// @brief The registry's factory table, so no header-including TU instantiates it.
    struct AssetLoaderRegistry::Impl
    {
        /// @brief Registered factories keyed by the asset type they produce a loader for.
        std::unordered_map<AssetTypeId, Factory> Factories;
    };

    AssetLoaderRegistry::AssetLoaderRegistry() : m_Impl(std::make_unique<Impl>()) {}

    AssetLoaderRegistry::~AssetLoaderRegistry() = default;

    AssetLoaderRegistry::AssetLoaderRegistry(AssetLoaderRegistry&& other) noexcept = default;

    AssetLoaderRegistry&
    AssetLoaderRegistry::operator=(AssetLoaderRegistry&& other) noexcept = default;

    void AssetLoaderRegistry::Register(AssetTypeId type, Factory factory)
    {
        VE_ASSERT(type.IsValid(), "AssetLoaderRegistry: cannot register the invalid asset type");
        VE_ASSERT(factory != nullptr,
                  "AssetLoaderRegistry: factory for asset type {:#018X} is "
                  "null",
                  type.Value);
        const auto [it, inserted] = m_Impl->Factories.try_emplace(type, std::move(factory));
        VE_ASSERT(inserted,
                  "AssetLoaderRegistry: asset type {:#018X} already has a loader "
                  "factory",
                  type.Value);
    }

    bool AssetLoaderRegistry::IsRegistered(AssetTypeId type) const
    {
        return m_Impl->Factories.contains(type);
    }

    const std::unordered_map<AssetTypeId, AssetLoaderRegistry::Factory>&
    AssetLoaderRegistry::All() const
    {
        return m_Impl->Factories;
    }
}
