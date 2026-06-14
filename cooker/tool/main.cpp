#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <fmt/format.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    void PrintUsage()
    {
        fmt::print(stderr, "usage: vengc cook <pack.json> [-o <out.vengpack>]\n");
    }
}

int main(int argc, char** argv)
{
    const vector<string> args(argv + 1, argv + argc);

    if (args.empty() || args[0] != "cook")
    {
        PrintUsage();
        return 1;
    }

    optional<path> packPath;
    optional<path> outPath;

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

    const VoidResult result = cooker.CookPack(*packPath, *outPath);
    if (!result)
    {
        fmt::print(stderr, "vengc: {}\n", result.error());
        return 1;
    }

    return 0;
}
