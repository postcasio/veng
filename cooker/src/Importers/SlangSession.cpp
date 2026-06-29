#include "SlangSession.h"

namespace Veng::Cook
{
    std::vector<std::string> BuildSlangSearchPaths(const path& sourceDir,
                                                   const path& engineShaderIncludeDir)
    {
        std::vector<std::string> paths;
        paths.push_back(sourceDir.string());
        if (!engineShaderIncludeDir.empty())
        {
            paths.push_back(engineShaderIncludeDir.string());
        }
        return paths;
    }

    void ApplySlangSearchPaths(slang::SessionDesc& desc, const std::vector<std::string>& paths,
                               std::vector<const char*>& pointers)
    {
        pointers.clear();
        pointers.reserve(paths.size());
        for (const std::string& p : paths)
        {
            pointers.push_back(p.c_str());
        }
        desc.searchPaths = pointers.data();
        desc.searchPathCount = static_cast<SlangInt>(pointers.size());
    }
}
