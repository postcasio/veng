// A minimal game module: exports the ABI version + VengModuleRegister, and on
// registration registers a game component into the host's TypeRegistry, a game-defined asset
// type — identity, handle-leaf mapping, and loader factory — into the host's asset registries,
// stores a trivial Application factory (never invoked here, so no Context/Window is
// constructed), and asserts the host's Editor slot is null.

#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Asset/AssetLoaderRegistry.h>
#include <Veng/Module/Module.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/SystemRegistry.h>

#include "probe_component.h"

namespace
{
    class ProbeApp : public Veng::Application
    {
    public:
        ProbeApp(Veng::TypeRegistry& types, Veng::SystemRegistry& systems)
            : Veng::Application(Veng::ApplicationInfo{}, types, systems)
        {
        }
    };

    // The runtime half of the module-defined asset type. Registered as a factory, so no Context
    // or AssetManager need exist when VengModuleRegister runs; the tests instantiate it by
    // building an AssetManager over these registries and loading a cooked probe blob.
    class ProbeAssetLoader final : public Veng::AssetLoader
    {
    public:
        [[nodiscard]] Veng::AssetTypeId Type() const override { return ProbeAssetType; }

        [[nodiscard]] Veng::AssetResult<Veng::Detail::LoadJob>
        Load(Veng::AssetManager&, Veng::Renderer::Context&, Veng::TaskSystem&, Veng::TypeRegistry&,
             Veng::AssetId id, std::span<const Veng::u8> cooked, bool) const override
        {
            // An empty blob stands in for a malformed one: a decode failure is recoverable, so
            // the tests can drive the error path without aborting the process.
            if (cooked.empty())
            {
                return std::unexpected(Veng::AssetLoadError{.Kind = Veng::AssetError::Corrupt,
                                                            .Id = id,
                                                            .Detail = "probe asset blob is empty"});
            }

            auto asset = Veng::CreateRef<ProbeAsset>();
            asset->Bytes.assign(cooked.begin(), cooked.end());
            return Veng::Detail::LoadJob{
                .Resource = std::static_pointer_cast<void>(std::move(asset)),
                .Dependencies = {},
                .Finalize = {},
            };
        }
    };
}

VE_EXPORT_MODULE_ABI()

extern "C" VE_MODULE_EXPORT void VengModuleRegister(Veng::VengModuleHost* host)
{
    VE_ASSERT(host != nullptr, "host must be non-null");
    VE_ASSERT(host->Editor == nullptr, "a game module is never loaded by the editor here");

    host->Types.Register<Probe>();

    // HandleFieldType is what makes Probe::Asset resolvable: the prefab loader, the cooker's
    // handle validation, and the editor's picker all turn that field's leaf TypeId back into
    // this asset type through it. Only the module knows the pairing for its own type.
    host->AssetTypes.Register(
        Veng::AssetTypeInfo{.Id = ProbeAssetType,
                            .Name = ProbeAssetTypeName,
                            .DisplayName = "Probe Asset",
                            .Glyph = "PRB",
                            .HandleFieldType = Veng::TypeIdOf<Veng::AssetHandle<ProbeAsset>>()});
    host->AssetLoaders.Register(
        ProbeAssetType, [] { return Veng::Unique<Veng::AssetLoader>(new ProbeAssetLoader()); });

    host->App.RegisterApplication(
        [](Veng::TypeRegistry& types, Veng::SystemRegistry& systems)
        { return Veng::Unique<Veng::Application>(new ProbeApp(types, systems)); });
}
