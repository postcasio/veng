#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <fmt/format.h>

// A minimal cooker that links only the veng-free cook core (no veng::veng, no
// module reflection). It exists to cook veng's embedded core pack at build time
// without forming a build cycle: the full vengc links veng::veng for module
// reflection, which veng's own build cannot depend on. This tool cooks plain
// packs (no --module); module-dependent packs use vengc.

using namespace Veng;
using namespace Veng::Cook;

int main(int argc, char** argv)
{
    const vector<string> args(argv + 1, argv + argc);

    if (args.size() < 2 || args[0] != "cook")
    {
        fmt::print(stderr, "usage: veng_cook_bootstrap cook <pack.json> [-o <out.vengpack>] "
                           "[--reference <pack.json>]... [--depfile <out.d>]\n");
        return 1;
    }

    optional<path> packPath;
    optional<path> outPath;
    vector<path> referencePacks;
    optional<path> depfilePath;

    for (usize i = 1; i < args.size(); ++i)
    {
        if (args[i] == "-o")
        {
            if (i + 1 >= args.size())
            {
                fmt::print(stderr, "veng_cook_bootstrap: -o requires an argument\n");
                return 1;
            }
            outPath = path(args[++i]);
        }
        else if (args[i] == "--depfile")
        {
            if (i + 1 >= args.size())
            {
                fmt::print(stderr, "veng_cook_bootstrap: --depfile requires an argument\n");
                return 1;
            }
            depfilePath = path(args[++i]);
        }
        else if (args[i] == "--reference")
        {
            if (i + 1 >= args.size())
            {
                fmt::print(stderr, "veng_cook_bootstrap: --reference requires an argument\n");
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
            fmt::print(stderr, "veng_cook_bootstrap: unexpected argument '{}'\n", args[i]);
            return 1;
        }
    }

    if (!packPath)
    {
        fmt::print(stderr, "veng_cook_bootstrap: missing pack path\n");
        return 1;
    }

    if (!outPath)
    {
        outPath = *packPath;
        outPath->replace_extension(".vengpack");
    }

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    vector<path> dependencies;
    const VoidResult result = cooker.CookPack(*packPath, *outPath, referencePacks, nullptr,
                                              depfilePath ? &dependencies : nullptr);
    if (!result)
    {
        fmt::print(stderr, "veng_cook_bootstrap: {}\n", result.error());
        return 1;
    }

    if (depfilePath)
    {
        const VoidResult depResult = WriteDepfile(*depfilePath, *outPath, dependencies);
        if (!depResult)
        {
            fmt::print(stderr, "veng_cook_bootstrap: {}\n", depResult.error());
            return 1;
        }
    }

    return 0;
}
