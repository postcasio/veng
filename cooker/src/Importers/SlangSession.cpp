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

    slang::IModule* LoadSlangModule(slang::ISession& session, const SlangModuleSource& source,
                                    slang::IBlob** outDiagnostics)
    {
        if (source.GeneratedSource)
        {
            // The module name is fixed (the stem carries dots Slang would read as an import
            // path); the virtual path keeps the graph file's directory so a relative
            // `#include` resolves through the session search paths (which lead with it).
            const std::string virtualPath =
                (source.Path.parent_path() / "generated.slang").string();
            return session.loadModuleFromSourceString(
                "generated", virtualPath.c_str(), source.GeneratedSource->c_str(), outDiagnostics);
        }
        const std::string moduleName = source.Path.stem().string();
        return session.loadModule(moduleName.c_str(), outDiagnostics);
    }
}
