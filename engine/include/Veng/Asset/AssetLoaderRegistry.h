#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/AssetType.h>

#include <unordered_map>

namespace Veng
{
    /// @brief Host-owned catalog of AssetLoader factories, keyed by the asset type each produces.
    ///
    /// The inert half of loader registration: a module registers a *factory* here at
    /// VengModuleRegister time, long before any Context or AssetManager exists, and the
    /// AssetManager instantiates every registered factory in its constructor. Registration is
    /// therefore GPU-free, which is what lets it ride the module ABI beside the reflected type
    /// descriptors.
    ///
    /// @warning An AssetManager borrows this registry for its whole life, so **the registry must
    ///          outlive every AssetManager built from it** — a fixture or harness that declares
    ///          them in the wrong order gets a dangling read at teardown, with no module involved
    ///          at all. When the registrations came from a loaded module, the module handle must
    ///          in turn outlive the registry, since the factories and the loaders they produce are
    ///          code in a dlclose-able image. Declaring them outermost-first — module handle,
    ///          registry, manager — satisfies both.
    class AssetLoaderRegistry
    {
    public:
        /// @brief Produces a loader for one asset type; invoked once per AssetManager.
        using Factory = function<Unique<AssetLoader>()>;

        /// @brief Records a loader factory for an asset type, aborting on a duplicate registration.
        ///
        /// Two modules claiming one asset type is a fatal collision, the same discipline the
        /// reflection TypeRegistry and AssetTypeRegistry apply.
        /// @param type     The asset type the produced loader handles.
        /// @param factory  Produces the loader; must be non-null.
        void Register(AssetTypeId type, Factory factory)
        {
            VE_ASSERT(type.IsValid(),
                      "AssetLoaderRegistry: cannot register the invalid asset type");
            VE_ASSERT(factory != nullptr,
                      "AssetLoaderRegistry: factory for asset type {:#018X} is "
                      "null",
                      type.Value);
            const auto [it, inserted] = m_Factories.try_emplace(type, std::move(factory));
            VE_ASSERT(inserted,
                      "AssetLoaderRegistry: asset type {:#018X} already has a loader "
                      "factory",
                      type.Value);
        }

        /// @brief Returns whether a factory is registered for an asset type.
        /// @param type  The asset type to query.
        [[nodiscard]] bool IsRegistered(AssetTypeId type) const
        {
            return m_Factories.contains(type);
        }

        /// @brief Returns every registered factory, keyed by asset type.
        [[nodiscard]] const std::unordered_map<AssetTypeId, Factory>& All() const
        {
            return m_Factories;
        }

    private:
        /// @brief Registered factories keyed by the asset type they produce a loader for.
        std::unordered_map<AssetTypeId, Factory> m_Factories;
    };
}
