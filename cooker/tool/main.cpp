#include <Veng/Asset/AssetPack.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <fmt/format.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    void PrintUsage()
    {
        fmt::print(stderr,
            "usage:\n"
            "  vengc cook <pack.json> [-o <out.vengpack>] [--reference <pack.json>]...\n"
            "  vengc generate-id [--reference <pack.json>]...\n");
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
            else if (args[i] == "--reference")
            {
                if (i + 1 >= args.size())
                {
                    fmt::print(stderr, "vengc: --reference requires an argument\n");
                    return 1;
                }
                referencePacks.push_back(path(args[++i]));
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

        Cooker cooker;
        RegisterBuiltinImporters(cooker);

        const VoidResult result = cooker.CookPack(*packPath, *outPath, referencePacks);
        if (!result)
        {
            fmt::print(stderr, "vengc: {}\n", result.error());
            return 1;
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
                referencePacks.push_back(path(args[++i]));
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
            packPtrs.push_back(&p);

        const AssetId id = GenerateAssetId(packPtrs);
        fmt::print("{}\n", id.Value);
        return 0;
    }

    PrintUsage();
    return 1;
}
