#include <Veng/Asset/CookedProject.h>
#include <Veng/Cook/AssetPack.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/CookCache.h>
#include <Veng/Cook/CookModule.h>
#include <charconv>

#include <Veng/Cook/CookTiming.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/ModuleTypes.h>
#include <Veng/Cook/Verify.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <random>

#include <fmt/format.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    void PrintUsage()
    {
        fmt::print(stderr, "usage:\n"
                           "  vengc cook <pack.json> [-o <out.vengpack>] [--reference "
                           "<pack.json>]... [--module <lib>] [--cook-module <lib>] "
                           "[--config <file.buildcfg>] "
                           "[--shader-include <dir>] [--cache-dir <dir>] [--depfile <out.d>] "
                           "[--timing[=<out.csv>]] [--jobs <n>]\n"
                           "  vengc cook-project <project.veng> --config <name> --out-dir <dir> "
                           "[--reference <pack.json>]... [--module <lib>] [--cook-module <lib>] "
                           "[--shader-include <dir>] "
                           "[--cache-dir <dir>] [--depfile <out.d>] [--timing[=<out.csv>]] "
                           "[--jobs <n>]\n"
                           "  vengc generate-id [--reference <pack.json>]... [--module <lib>]\n"
                           "  vengc generate-type-id [--module <lib>]\n"
                           "  vengc generate-asset-type [--module <lib>]\n"
                           "  vengc generate-family-id\n"
                           "  vengc verify <archive.vengpack> [--module <lib>]\n");
    }

    // Opens the cooked-blob cache at `cacheDir`, keyed on the cook tool and — for the entries that
    // read them — on the images this cook dlopens. Returns nullopt (no caching) when `cacheDir` is
    // unset; a real open failure is fatal so a broken cache dir surfaces loudly rather than
    // silently disabling the cache.
    optional<CookCache> OpenCache(const optional<path>& cacheDir, const char* argv0,
                                  const optional<path>& modulePath, const path& cookModulePath)
    {
        if (!cacheDir)
        {
            return std::nullopt;
        }
        string baseTag = ComputeCookBaseTag(path(argv0));
        string moduleTag = ComputeCookModuleTag(modulePath ? *modulePath : path{}, cookModulePath);
        Result<CookCache> opened =
            CookCache::Open(*cacheDir, std::move(baseTag), std::move(moduleTag));
        if (!opened)
        {
            fmt::print(stderr, "vengc: {}\n", opened.error());
            std::exit(1);
        }
        return std::move(*opened);
    }

    // The cook module this invocation will load, or an empty path when there is none. An explicit
    // --cook-module replaces the sibling lookup entirely (so a path that does not exist is still
    // returned, and fails loudly at load); an absent sibling simply means the game defines no
    // importers of its own. Resolved once so the cache key and the load agree on one path.
    path ResolveCookModulePath(const optional<path>& modulePath,
                               const optional<path>& cookModulePath)
    {
        if (cookModulePath)
        {
            return *cookModulePath;
        }
        if (modulePath)
        {
            const path sibling = SiblingCookModulePath(*modulePath);
            if (std::filesystem::exists(sibling))
            {
                return sibling;
            }
        }
        return {};
    }

    // Loads the game's optional cook module from an already-resolved path. A module that loads but
    // fails its ABI handshake is fatal — a silently skipped stale cook module would surface as an
    // unregistered-type cook error a long way from its cause.
    optional<LoadedCookModule> LoadGameCookModule(const path& resolved)
    {
        if (resolved.empty())
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

    // Fills in the runtime module implied by an explicit --cook-module. A cook module links its
    // runtime module, so that image is always present; without loading it the cook module's
    // importers install keyed on asset-type ids the type registry never heard of, and every
    // manifest entry naming one fails as an unknown type. The path is derived and loaded
    // explicitly rather than resolved through the cook module's own handle: dlsym searches
    // dependent images, GetProcAddress does not.
    void ImplyRuntimeModule(optional<path>& modulePath, const optional<path>& cookModulePath)
    {
        if (modulePath || !cookModulePath)
        {
            return;
        }

        const optional<path> runtime = SiblingRuntimeModulePath(*cookModulePath);
        if (runtime && std::filesystem::exists(*runtime))
        {
            modulePath = *runtime;
        }
    }

    // Parses a --jobs value: a positive decimal count, or 0 for "the hardware concurrency".
    // Rejects anything else rather than silently cooking at a count the operator did not ask for.
    bool ParseJobs(const string& text, u32& jobs)
    {
        u32 parsed = 0;
        const char* first = text.data();
        const char* last = first + text.size();
        const std::from_chars_result result = std::from_chars(first, last, parsed);
        if (result.ec != std::errc{} || result.ptr != last)
        {
            fmt::print(stderr, "vengc: --jobs expects a non-negative count, got '{}'\n", text);
            return false;
        }
        jobs = parsed;
        return true;
    }

    // Recognizes `--timing` and `--timing=<file>`. The flag's value is optional and attached with
    // '=' rather than taken as the next argument, so the bare form stays unambiguous next to a
    // positional pack path. Returns true when the argument was the flag.
    bool ParseTimingArg(const string& arg, bool& enabled, optional<path>& file)
    {
        constexpr std::string_view Flag = "--timing";
        if (arg == Flag)
        {
            enabled = true;
            return true;
        }
        if (arg.starts_with(string(Flag) + "="))
        {
            enabled = true;
            file = path(arg.substr(Flag.size() + 1));
            return true;
        }
        return false;
    }

    // Prints the operator summary and, when a file was named, writes the full per-asset table.
    // Returns false on a write failure so the caller exits nonzero.
    bool ReportTiming(const CookTiming& timing, const optional<path>& file)
    {
        fmt::print("{}", FormatCookTimingSummary(timing));
        if (!file)
        {
            return true;
        }
        const VoidResult written = WriteCookTimingTable(*file, timing);
        if (!written)
        {
            fmt::print(stderr, "vengc: {}\n", written.error());
            return false;
        }
        return true;
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
    // Started before anything else so a --timing report's total covers the whole invocation, not
    // just the cook; the per-asset sum is then a share of something an operator can reproduce with
    // an external stopwatch.
    const CookStopwatch invocationWatch;

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
        bool timingEnabled = false;
        optional<path> timingFile;
        // The cook's whole concurrency budget, shared by the per-asset pool and any threading an
        // importer does inside its own Cook. Zero means the hardware concurrency.
        u32 jobs = 0;

        for (usize i = 1; i < args.size(); ++i)
        {
            if (ParseTimingArg(args[i], timingEnabled, timingFile))
            {
                // The flag carries its own optional value; nothing further to consume.
            }
            else if (args[i] == "-o")
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
            else if (args[i] == "--jobs")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --jobs requires an argument\n");
                    return 1;
                }
                if (!ParseJobs(args[++i], jobs))
                {
                    return 1;
                }
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

        ImplyRuntimeModule(modulePath, cookModulePath);

        CookTiming timing;
        const CookStopwatch moduleWatch;

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
        const path cookModuleFile = ResolveCookModulePath(modulePath, cookModulePath);
        optional<LoadedCookModule> cookModule = LoadGameCookModule(cookModuleFile);
        timing.ModuleLoadSeconds = moduleWatch.Elapsed();

        // The resolved build configuration drives the texture role → format resolution, the
        // archive compression level, and is recorded as a central depfile input. Absent --config
        // the cook is the zero-config ASTC default.
        const CookStopwatch configWatch;
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
        timing.ProjectParseSeconds = configWatch.Elapsed();

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

        const optional<CookCache> cache =
            OpenCache(cacheDirPath, argv[0], modulePath, cookModuleFile);

        vector<path> dependencies;
        const VoidResult result = cooker.CookPack(
            *packPath, *outPath, referencePacks, types, systems,
            depfilePath ? &dependencies : nullptr, config ? &*config : nullptr,
            configPath ? *configPath : path{}, shaderIncludePath ? *shaderIncludePath : path{},
            cache ? &*cache : nullptr, timingEnabled ? &timing : nullptr, jobs);
        if (!result)
        {
            fmt::print(stderr, "vengc: {}\n", result.error());
            return 1;
        }

        if (depfilePath)
        {
            const CookStopwatch depfileWatch;
            const VoidResult depResult = WriteDepfile(*depfilePath, *outPath, dependencies);
            if (!depResult)
            {
                fmt::print(stderr, "vengc: {}\n", depResult.error());
                return 1;
            }
            timing.DepfileSeconds = depfileWatch.Elapsed();
        }

        if (timingEnabled)
        {
            timing.TotalSeconds = invocationWatch.Elapsed();
            if (!ReportTiming(timing, timingFile))
            {
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
        bool timingEnabled = false;
        optional<path> timingFile;
        // The cook's whole concurrency budget, shared by the per-asset pool and any threading an
        // importer does inside its own Cook. Zero means the hardware concurrency.
        u32 jobs = 0;

        for (usize i = 1; i < args.size(); ++i)
        {
            if (ParseTimingArg(args[i], timingEnabled, timingFile))
            {
                // The flag carries its own optional value; nothing further to consume.
            }
            else if (args[i] == "--config")
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
            else if (args[i] == "--jobs")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --jobs requires an argument\n");
                    return 1;
                }
                if (!ParseJobs(args[++i], jobs))
                {
                    return 1;
                }
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

        CookTiming timing;
        const CookStopwatch projectWatch;

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
        timing.ProjectParseSeconds = projectWatch.Elapsed();

        ImplyRuntimeModule(modulePath, cookModulePath);

        const CookStopwatch moduleWatch;

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
        const path cookModuleFile = ResolveCookModulePath(modulePath, cookModulePath);
        optional<LoadedCookModule> cookModule = LoadGameCookModule(cookModuleFile);
        timing.ModuleLoadSeconds = moduleWatch.Elapsed();

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

        const optional<CookCache> cache =
            OpenCache(cacheDirPath, argv[0], modulePath, cookModuleFile);

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
                cache ? &*cache : nullptr, timingEnabled ? &timing : nullptr, jobs);
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
            const CookStopwatch depfileWatch;
            const VoidResult depResult = WriteDepfile(*depfilePath, outProject, dependencies);
            if (!depResult)
            {
                fmt::print(stderr, "vengc: {}\n", depResult.error());
                return 1;
            }
            timing.DepfileSeconds = depfileWatch.Elapsed();
        }

        if (timingEnabled)
        {
            timing.TotalSeconds = invocationWatch.Elapsed();
            if (!ReportTiming(timing, timingFile))
            {
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
    // vengc generate-family-id
    // -------------------------------------------------------------------
    if (subcommand == "generate-family-id")
    {
        if (args.size() > 1)
        {
            fmt::print(stderr, "vengc: unexpected argument '{}'\n", args[1]);
            return 1;
        }

        // There is deliberately no --module and no --reference: a store family id lives in a
        // consumer's own source, in no registry this tool can load, so there is nothing to
        // collision-check against. The id space is 64 bits of randomness and a collision at
        // registration is fatal at runtime.
        std::random_device rd;
        std::mt19937_64 rng(rd());
        std::uniform_int_distribution<u64> dist(1, UINT64_MAX);
        const u64 id = dist(rng);

        // One canonical spelling: a zero-padded 16-digit hex literal in C++, the same string
        // quoted in JSON.
        fmt::print("hex (C++):   0x{:016X}ULL\n", id);
        fmt::print("hex (JSON):  \"0x{:016X}\"\n", id);
        return 0;
    }

    // -------------------------------------------------------------------
    // vengc verify
    // -------------------------------------------------------------------
    if (subcommand == "verify")
    {
        optional<path> archivePath;
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
            else if (!archivePath)
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

        // Presentation only: the verdict is a byte re-hash either way, but a game-typed asset
        // prints as a raw hex id unless the module that named the type is loaded. The module image
        // must outlive the registry it populates.
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
        }

        return VerifyArchiveCli(*archivePath, moduleTypes ? &moduleTypes->AssetTypes : nullptr);
    }

    PrintUsage();
    return 1;
}
