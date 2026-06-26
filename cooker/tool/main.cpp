#include <Veng/Cook/AssetPack.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/ModuleTypes.h>
#include <Veng/Cook/Verify.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

#include <algorithm>

#include <fmt/format.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    void PrintUsage()
    {
        fmt::print(stderr, "usage:\n"
                           "  vengc cook <pack.json> [-o <out.vengpack>] [--reference "
                           "<pack.json>]... [--module <lib>] [--config <file.buildcfg>] "
                           "[--depfile <out.d>]\n"
                           "  vengc generate-id [--reference <pack.json>]...\n"
                           "  vengc generate-type-id [--module <lib>]\n"
                           "  vengc verify <archive.vengpack>\n");
    }

    // Prints the loaded type table as a name → TypeId manifest (stdout, not persisted).
    void PrintTypeManifest(const TypeRegistry& types)
    {
        vector<const TypeInfo*> infos;
        infos.reserve(types.All().size());
        for (const auto& [id, info] : types.All())
        {
            infos.push_back(&info);
        }

        std::ranges::sort(infos,
                          [](const TypeInfo* a, const TypeInfo* b) { return a->Name < b->Name; });

        fmt::print("reflected types ({}):\n", infos.size());
        for (const TypeInfo* info : infos)
        {
            const string name = info->Name.empty() ? "<leaf>" : info->Name;
            fmt::print("  {:<24} 0x{:016X}\n", name, info->Id);
        }
    }
}

int main(int argc, char** argv)
{
    const vector<string> args(argv + 1, argv + argc);

    if (args.empty())
    {
        PrintUsage();
        return 1;
    }

    const string& subcommand = args[0];

    // -------------------------------------------------------------------
    // vengc cook
    // -------------------------------------------------------------------
    if (subcommand == "cook")
    {
        optional<path> packPath;
        optional<path> outPath;
        vector<path> referencePacks;
        optional<path> modulePath;
        optional<path> configPath;
        optional<path> depfilePath;

        for (usize i = 1; i < args.size(); ++i)
        {
            if (args[i] == "-o")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: -o requires an argument\n");
                    return 1;
                }
                outPath = path(args[++i]);
            }
            else if (args[i] == "--depfile")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --depfile requires an argument\n");
                    return 1;
                }
                depfilePath = path(args[++i]);
            }
            else if (args[i] == "--reference")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --reference requires an argument\n");
                    return 1;
                }
                referencePacks.emplace_back(args[++i]);
            }
            else if (args[i] == "--module")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --module requires an argument\n");
                    return 1;
                }
                modulePath = path(args[++i]);
            }
            else if (args[i] == "--config")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --config requires an argument\n");
                    return 1;
                }
                configPath = path(args[++i]);
            }
            else if (!packPath)
            {
                packPath = path(args[i]);
            }
            else
            {
                fmt::print(stderr, "vengc: unexpected argument '{}'\n", args[i]);
                return 1;
            }
        }

        if (!packPath)
        {
            PrintUsage();
            return 1;
        }

        if (!outPath)
        {
            outPath = *packPath;
            outPath->replace_extension(".vengpack");
        }

        // The module image and its registry must outlive the cook.
        optional<LoadedModuleTypes> moduleTypes;
        if (modulePath)
        {
            Result<LoadedModuleTypes> loaded = LoadModuleTypes(*modulePath);
            if (!loaded)
            {
                fmt::print(stderr, "vengc: {}\n", loaded.error());
                return 1;
            }
            moduleTypes = std::move(*loaded);
            PrintTypeManifest(moduleTypes->Types);
        }

        const TypeRegistry* types = moduleTypes ? &moduleTypes->Types : nullptr;
        const SystemRegistry* systems = moduleTypes ? &moduleTypes->Systems : nullptr;

        // The resolved build configuration drives the texture role → format resolution, the
        // archive compression level, and is recorded as a central depfile input. Absent --config
        // the cook is the zero-config ASTC default.
        optional<BuildConfiguration> config;
        if (configPath)
        {
            Result<BuildConfiguration> parsed = ParseBuildConfiguration(*configPath);
            if (!parsed)
            {
                fmt::print(stderr, "vengc: {}\n", parsed.error());
                return 1;
            }
            config = std::move(*parsed);
        }

        Cooker cooker;
        RegisterBuiltinImporters(cooker);

        vector<path> dependencies;
        const VoidResult result =
            cooker.CookPack(*packPath, *outPath, referencePacks, types, systems,
                            depfilePath ? &dependencies : nullptr, config ? &*config : nullptr,
                            configPath ? *configPath : path{});
        if (!result)
        {
            fmt::print(stderr, "vengc: {}\n", result.error());
            return 1;
        }

        if (depfilePath)
        {
            const VoidResult depResult = WriteDepfile(*depfilePath, *outPath, dependencies);
            if (!depResult)
            {
                fmt::print(stderr, "vengc: {}\n", depResult.error());
                return 1;
            }
        }

        return 0;
    }

    // -------------------------------------------------------------------
    // vengc generate-id
    // -------------------------------------------------------------------
    if (subcommand == "generate-id")
    {
        vector<path> referencePacks;

        for (usize i = 1; i < args.size(); ++i)
        {
            if (args[i] == "--reference")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --reference requires an argument\n");
                    return 1;
                }
                referencePacks.emplace_back(args[++i]);
            }
            else
            {
                fmt::print(stderr, "vengc: unexpected argument '{}'\n", args[i]);
                return 1;
            }
        }

        vector<AssetPack> packs;
        packs.reserve(referencePacks.size());
        for (const path& refPath : referencePacks)
        {
            Result<AssetPack> packResult = ParseAssetPack(refPath);
            if (!packResult)
            {
                fmt::print(stderr, "vengc: {}\n", packResult.error());
                return 1;
            }
            packs.push_back(std::move(*packResult));
        }

        vector<const AssetPack*> packPtrs;
        packPtrs.reserve(packs.size());
        for (const AssetPack& p : packs)
        {
            packPtrs.push_back(&p);
        }

        const AssetId id = GenerateAssetId(packPtrs);
        // Hex for C++ literals, decimal for JSON packs (JSON has no hex literal).
        fmt::print("hex (C++):      0x{:X}\n", id.Value);
        fmt::print("decimal (JSON): {}\n", id.Value);
        return 0;
    }

    // -------------------------------------------------------------------
    // vengc generate-type-id
    // -------------------------------------------------------------------
    if (subcommand == "generate-type-id")
    {
        optional<path> modulePath;

        for (usize i = 1; i < args.size(); ++i)
        {
            if (args[i] == "--module")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --module requires an argument\n");
                    return 1;
                }
                modulePath = path(args[++i]);
            }
            else
            {
                fmt::print(stderr, "vengc: unexpected argument '{}'\n", args[i]);
                return 1;
            }
        }

        // Collision-checks against builtins always, plus game types when --module is given.
        // The module image must outlive the registry it populates.
        optional<LoadedModuleTypes> moduleTypes;
        TypeRegistry builtins;
        const TypeRegistry* registry = nullptr;
        if (modulePath)
        {
            Result<LoadedModuleTypes> loaded = LoadModuleTypes(*modulePath);
            if (!loaded)
            {
                fmt::print(stderr, "vengc: {}\n", loaded.error());
                return 1;
            }
            moduleTypes = std::move(*loaded);
            registry = &moduleTypes->Types;
        }
        else
        {
            RegisterBuiltinTypes(builtins);
            registry = &builtins;
        }

        const TypeId id = GenerateTypeId(*registry);

        // Hex for C++ literals, decimal for JSON packs (JSON has no hex literal).
        fmt::print("hex (C++):      0x{:X}ULL\n", id);
        fmt::print("decimal (JSON): {}\n", id);
        return 0;
    }

    // -------------------------------------------------------------------
    // vengc verify
    // -------------------------------------------------------------------
    if (subcommand == "verify")
    {
        optional<path> archivePath;

        for (usize i = 1; i < args.size(); ++i)
        {
            if (!archivePath)
            {
                archivePath = path(args[i]);
            }
            else
            {
                fmt::print(stderr, "vengc: unexpected argument '{}'\n", args[i]);
                return 1;
            }
        }

        if (!archivePath)
        {
            PrintUsage();
            return 1;
        }

        return VerifyArchiveCli(*archivePath);
    }

    PrintUsage();
    return 1;
}
