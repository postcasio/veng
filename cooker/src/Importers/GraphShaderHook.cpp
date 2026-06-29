#include "GraphShaderSource.h"

// The resolver hook storage. Lives in the veng-free object library so the shader
// and material importers (also veng-free) reach the graph emit walk through a
// function pointer rather than a static link — the walk lives in veng::graph,
// linked only into libveng_cook. RegisterBuiltinImporters installs the real
// resolver; the core-pack bootstrap leaves it null and cooks no graph sources.

namespace Veng::Cook
{
    namespace
    {
        GraphShaderResolver g_GraphShaderResolver;
    }

    void SetGraphShaderResolver(GraphShaderResolver resolver)
    {
        g_GraphShaderResolver = std::move(resolver);
    }

    Result<GraphShaderSource> ResolveGraphShaderSourceHook(const json& shaderJson,
                                                           const path& shaderJsonDir)
    {
        if (!g_GraphShaderResolver)
        {
            return GraphShaderSource{.IsGraph = false};
        }
        return g_GraphShaderResolver(shaderJson, shaderJsonDir);
    }
}
