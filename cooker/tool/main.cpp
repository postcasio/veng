#include <Veng/Asset/CookedProject.h>
#include <Veng/Cook/AssetPack.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/CookCache.h>
#include <Veng/Cook/CookModule.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/ModuleTypes.h>
#include <Veng/Cook/Verify.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include <fmt/format.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    // The cook cache's own format version. Folded into the tool tag so a change to the cache file
    // layout or the manifest schema invalidates every cached entry without a manual sweep.
    constexpr u32 CookCacheFormatVersion = 3;

    void PrintUsage()
    {
        fmt::print(stderr, "usage:\n"
                           "  vengc cook <pack.json> [-o <out.vengpack>] [--reference "
                           "<pack.json>]... [--module <lib>] [--cook-module <lib>] "
                           "[--config <file.buildcfg>] "
                           "[--shader-include <dir>] [--cache-dir <dir>] [--depfile <out.d>]\n"
                           "  vengc cook-project <project.veng> --config <name> --out-dir <dir> "
                           "[--reference <pack.json>]... [--module <lib>] [--cook-module <lib>] "
                           "[--shader-include <dir>] "
                           "[--cache-dir <dir>] [--depfile <out.d>]\n"
                           "  vengc generate-id [--reference <pack.json>]... [--module <lib>]\n"
                           "  vengc generate-type-id [--module <lib>]\n"
                           "  vengc generate-asset-type [--module <lib>]\n"
                           "  vengc verify <archive.vengpack>\n");
    }

    // A fingerprint of this cooker binary, so any rebuild of vengc (an importer or format change
    // relinks it) invalidates the whole cache — no manual version bump per importer needed. Folds
    // the cache-format version with the executable's own path, size, and mtime; the size/mtime are
    // dropped when the executable cannot be stat'd (a bare-name PATH launch), leaving the format
    // version as the guaranteed component.
    string ComputeToolTag(const char* argv0)
    {
        string tag = fmt::format("cachefmt={}", CookCacheFormatVersion);
        std::error_code ec;
        const path exe = std::filesystem::weakly_canonical(path(argv0), ec);
        if (!ec && std::filesystem::exists(exe, ec))
        {
            const auto size = std::filesystem::file_size(exe, ec);
            if (!ec)
            {
                const auto mtime = std::filesystem::last_write_time(exe, ec);
                if (!ec)
                {
                    tag += fmt::format(";exe={};size={};mtime={}", exe.string(), size,
                                       mtime.time_since_epoch().count());
                }
            }
        }
        return tag;
    }

    // Opens the cooked-blob cache at `cacheDir` with the tool tag derived from `argv0`. Returns
    // nullopt (no caching) when `cacheDir` is unset; a real open failure is fatal so a broken cache
    // dir surfaces loudly rather than silently disabling the cache.
    optional<CookCache> OpenCache(const optional<path>& cacheDir, const char* argv0)
    {
        if (!cacheDir)
        {
            return std::nullopt;
        }
        Result<CookCache> opened = CookCache::Open(*cacheDir, ComputeToolTag(argv0));
        if (!opened)
        {
            fmt::print(stderr, "vengc: {}\n", opened.error());
            std::exit(1);
        }
        return std::move(*opened);
    }

    // Resolves and loads the game's optional cook module. An explicit --cook-module replaces the
    // sibling lookup entirely, so a path that does not load is fatal; an absent sibling simply
    // means the game defines no importers of its own. A module that loads but fails its ABI
    // handshake is fatal either way — a silently skipped stale cook module would surface as an
    // unregistered-type cook error a long way from its cause.
    optional<LoadedCookModule> LoadGameCookModule(const optional<path>& modulePath,
                                                  const optional<path>& cookModulePath)
    {
        path resolved;
        if (cookModulePath)
        {
            resolved = *cookModulePath;
        }
        else if (modulePath)
        {
            resolved = SiblingCookModulePath(*modulePath);
            if (!std::filesystem::exists(resolved))
            {
                return std::nullopt;
            }
        }
        else
        {
            return std::nullopt;
        }

        Result<LoadedCookModule> loaded = LoadCookModule(resolved);
        if (!loaded)
        {
            fmt::print(stderr, "vengc: {}\n", loaded.error());
            std::exit(1);
        }
        return std::move(*loaded);
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
        optional<path> cookModulePath;
        optional<path> configPath;
        optional<path> shaderIncludePath;
        optional<path> cacheDirPath;
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
            else if (args[i] == "--cook-module")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --cook-module requires an argument\n");
                    return 1;
                }
                cookModulePath = path(args[++i]);
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
            else if (args[i] == "--shader-include")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --shader-include requires an argument\n");
                    return 1;
                }
                shaderIncludePath = path(args[++i]);
            }
            else if (args[i] == "--cache-dir")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --cache-dir requires an argument\n");
                    return 1;
                }
                cacheDirPath = path(args[++i]);
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

        // Declared before the Cooker so it is destroyed after it: the importers move into the
        // cooker, and their code lives in this image.
        optional<LoadedCookModule> cookModule = LoadGameCookModule(modulePath, cookModulePath);

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
        // The game module's asset-type names must reach the cooker's registry before the manifest
        // is parsed, or an entry naming a game type resolves to nothing; its importers come from
        // the cook module.
        if (moduleTypes)
        {
            MergeAssetTypes(moduleTypes->AssetTypes, cooker.GetAssetTypes());
        }
        if (cookModule)
        {
            cookModule->Importers.MoveInto(cooker);
        }

        const optional<CookCache> cache = OpenCache(cacheDirPath, argv[0]);

        vector<path> dependencies;
        const VoidResult result = cooker.CookPack(
            *packPath, *outPath, referencePacks, types, systems,
            depfilePath ? &dependencies : nullptr, config ? &*config : nullptr,
            configPath ? *configPath : path{}, shaderIncludePath ? *shaderIncludePath : path{},
            cache ? &*cache : nullptr);
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
    // vengc cook-project
    // -------------------------------------------------------------------
    if (subcommand == "cook-project")
    {
        optional<path> projectPath;
        optional<string> configName;
        optional<path> outDir;
        optional<path> modulePath;
        optional<path> cookModulePath;
        optional<path> shaderIncludePath;
        optional<path> cacheDirPath;
        optional<path> depfilePath;
        vector<path> referencePacks;

        for (usize i = 1; i < args.size(); ++i)
        {
            if (args[i] == "--config")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --config requires an argument\n");
                    return 1;
                }
                configName = args[++i];
            }
            else if (args[i] == "--shader-include")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --shader-include requires an argument\n");
                    return 1;
                }
                shaderIncludePath = path(args[++i]);
            }
            else if (args[i] == "--out-dir")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --out-dir requires an argument\n");
                    return 1;
                }
                outDir = path(args[++i]);
            }
            else if (args[i] == "--cache-dir")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --cache-dir requires an argument\n");
                    return 1;
                }
                cacheDirPath = path(args[++i]);
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
            else if (args[i] == "--cook-module")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --cook-module requires an argument\n");
                    return 1;
                }
                cookModulePath = path(args[++i]);
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
            else if (args[i] == "--depfile")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --depfile requires an argument\n");
                    return 1;
                }
                depfilePath = path(args[++i]);
            }
            else if (!projectPath)
            {
                projectPath = path(args[i]);
            }
            else
            {
                fmt::print(stderr, "vengc: unexpected argument '{}'\n", args[i]);
                return 1;
            }
        }

        if (!projectPath || !configName || !outDir)
        {
            PrintUsage();
            return 1;
        }

        const Result<CookProject> project = ParseProject(*projectPath);
        if (!project)
        {
            fmt::print(stderr, "vengc: {}\n", project.error());
            return 1;
        }

        // Select the named configuration by parsing each *.buildcfg and matching its Name.
        optional<BuildConfiguration> config;
        path configFile;
        for (const path& candidate : project->ConfigFiles)
        {
            Result<BuildConfiguration> parsed = ParseBuildConfiguration(candidate);
            if (!parsed)
            {
                fmt::print(stderr, "vengc: {}\n", parsed.error());
                return 1;
            }
            if (parsed->Name == *configName)
            {
                config = std::move(*parsed);
                configFile = candidate;
                break;
            }
        }
        if (!config)
        {
            fmt::print(stderr, "vengc: project '{}' has no configuration named '{}'\n",
                       projectPath->string(), *configName);
            return 1;
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

        // Declared before the Cooker so it is destroyed after it: the importers move into the
        // cooker, and their code lives in this image.
        optional<LoadedCookModule> cookModule = LoadGameCookModule(modulePath, cookModulePath);

        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        if (moduleTypes)
        {
            MergeAssetTypes(moduleTypes->AssetTypes, cooker.GetAssetTypes());
        }
        if (cookModule)
        {
            cookModule->Importers.MoveInto(cooker);
        }

        const optional<CookCache> cache = OpenCache(cacheDirPath, argv[0]);

        // Cook each pack under the selected configuration and collect both the runtime mount names
        // (un-suffixed, the names the launcher mounts) and every source for one combined depfile.
        CookedProject cooked;
        cooked.StartupLevel = project->StartupLevel;

        vector<path> dependencies;
        for (const path& packManifest : project->Packs)
        {
            const path mountName =
                packManifest.stem(); // template.vengpack.json -> template.vengpack
            const string outPackName =
                mountName.stem().string() + config->OutputSuffix + mountName.extension().string();
            const path outPack = *outDir / outPackName;

            // A project's packs share one AssetId namespace: every pack resolves the others'
            // ids at cook time, so an asset in one pack may reference an asset declared in a
            // sibling. The reference set is the CLI references (e.g. the engine core pack) plus
            // every other project pack.
            vector<path> packRefs = referencePacks;
            for (const path& sibling : project->Packs)
            {
                if (sibling != packManifest)
                {
                    packRefs.push_back(sibling);
                }
            }

            vector<path> packDeps;
            const VoidResult result = cooker.CookPack(
                packManifest, outPack, packRefs, types, systems, depfilePath ? &packDeps : nullptr,
                &*config, configFile, shaderIncludePath ? *shaderIncludePath : path{},
                cache ? &*cache : nullptr);
            if (!result)
            {
                fmt::print(stderr, "vengc: {}\n", result.error());
                return 1;
            }

            cooked.PackMountNames.push_back(mountName.string());
            dependencies.insert(dependencies.end(), packDeps.begin(), packDeps.end());
        }

        const string projStem = projectPath->stem().string();
        const path outProject = *outDir / (projStem + config->OutputSuffix + ".vengproj");
        const VoidResult written = WriteCookedProject(outProject, cooked);
        if (!written)
        {
            fmt::print(stderr, "vengc: {}\n", written.error());
            return 1;
        }

        if (depfilePath)
        {
            // The single command produces every pack plus the .vengproj together; a depfile keyed on
            // the project output re-runs the whole cook when any pack source or the project changes.
            dependencies.push_back(*projectPath);
            const VoidResult depResult = WriteDepfile(*depfilePath, outProject, dependencies);
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
        optional<path> modulePath;

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
            else if (args[i] == "--module")
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

        // A reference manifest naming a module-defined type only parses once that module's type
        // names are known, so --module is required whenever a reference pack carries one. The
        // module image must outlive the registry it populates.
        AssetTypeRegistry builtins;
        RegisterBuiltinAssetTypes(builtins);
        optional<LoadedModuleTypes> moduleTypes;
        const AssetTypeRegistry* assetTypes = &builtins;
        if (modulePath)
        {
            Result<LoadedModuleTypes> loaded = LoadModuleTypes(*modulePath);
            if (!loaded)
            {
                fmt::print(stderr, "vengc: {}\n", loaded.error());
                return 1;
            }
            moduleTypes = std::move(*loaded);
            assetTypes = &moduleTypes->AssetTypes;
        }

        vector<AssetPack> packs;
        packs.reserve(referencePacks.size());
        for (const path& refPath : referencePacks)
        {
            Result<AssetPack> packResult = ParseAssetPack(refPath, *assetTypes);
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
        // One canonical spelling: a zero-padded 16-digit hex literal in C++, the same
        // string quoted in JSON.
        fmt::print("hex (C++):   0x{:016X}ULL\n", id.Value);
        fmt::print("hex (JSON):  \"0x{:016X}\"\n", id.Value);
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
    // vengc generate-asset-type
    // -------------------------------------------------------------------
    if (subcommand == "generate-asset-type")
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

        // There is deliberately no --reference: a pack manifest carries type *names*, so it has
        // no minted type id to collide with. The id space lives in the builtin table and in the
        // registrations a loaded module contributes.
        AssetTypeRegistry builtins;
        RegisterBuiltinAssetTypes(builtins);

        // The module image must outlive the registry it populates.
        optional<LoadedModuleTypes> moduleTypes;
        const AssetTypeRegistry* assetTypes = &builtins;
        if (modulePath)
        {
            Result<LoadedModuleTypes> loaded = LoadModuleTypes(*modulePath);
            if (!loaded)
            {
                fmt::print(stderr, "vengc: {}\n", loaded.error());
                return 1;
            }
            moduleTypes = std::move(*loaded);
            assetTypes = &moduleTypes->AssetTypes;
        }

        const AssetTypeId id = GenerateAssetTypeId(*assetTypes);
        // One canonical spelling: a zero-padded 16-digit hex literal in C++, the same string
        // quoted in JSON.
        fmt::print("hex (C++):   0x{:016X}ULL\n", id.Value);
        fmt::print("hex (JSON):  \"0x{:016X}\"\n", id.Value);
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
