#include "GraphShaderSource.h"

#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include <VengGraph/MaterialCatalog.h>
#include <VengGraph/MaterialCompile.h>
#include <VengGraph/NodeGraph.h>
#include <VengGraph/NodeGraphSerialize.h>
#include <VengGraph/NodeType.h>

namespace Veng::Cook
{
    namespace
    {
        // A *.shader.json source naming a graph ends in this suffix; a .slang source
        // does not, so the suffix alone classifies the two.
        constexpr std::string_view GraphSourceSuffix = ".graph.json";

        bool EndsWith(std::string_view text, std::string_view suffix)
        {
            return text.size() >= suffix.size() &&
                   text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
    }

    Result<GraphShaderSource> ResolveGraphShaderSource(const json& shaderJson,
                                                       const path& shaderJsonDir)
    {
        if (!shaderJson.contains("source") || !shaderJson["source"].is_string())
        {
            return std::unexpected("shader importer: missing or invalid 'source'");
        }

        const string sourceField = shaderJson["source"].get<string>();
        if (!EndsWith(sourceField, GraphSourceSuffix))
        {
            return GraphShaderSource{.IsGraph = false};
        }

        // A graph-sourced shader names its material domain, which fixes the emit walk's
        // entry-point signature and MaterialOutput sink set. Default surface; an unknown
        // value is a located error.
        MaterialDomain domain = MaterialDomain::Surface;
        if (shaderJson.contains("domain"))
        {
            if (!shaderJson["domain"].is_string())
            {
                return std::unexpected("shader importer: graph 'domain' must be a string "
                                       "(\"surface\" or \"postprocess\")");
            }
            const string domainStr = shaderJson["domain"].get<string>();
            if (domainStr == "surface")
            {
                domain = MaterialDomain::Surface;
            }
            else if (domainStr == "postprocess")
            {
                domain = MaterialDomain::PostProcess;
            }
            else
            {
                return std::unexpected(
                    fmt::format("shader importer: unknown graph domain '{}' (expected "
                                "\"surface\" or \"postprocess\")",
                                domainStr));
            }
        }

        const path graphPath = shaderJsonDir / sourceField;

        const std::ifstream graphFile(graphPath, std::ios::binary);
        if (!graphFile)
        {
            return std::unexpected(fmt::format("shader importer: failed to open graph source '{}'",
                                               graphPath.string()));
        }
        std::ostringstream graphStream;
        graphStream << graphFile.rdbuf();
        const string graphDoc = graphStream.str();

        // Build the schema-independent catalog + emit table for the domain, then read the
        // graph into a fresh NodeGraph wired to the catalog's hooks and run the walk — the
        // identical sequence the editor preview runs, in the shared veng::graph lib.
        VengGraph::NodeCatalog catalog;
        VengGraph::MaterialEmitTable emit;
        VengGraph::RegisterMaterialNodeTypes(catalog, emit, domain);

        VengGraph::NodeGraph graph(
            VengGraph::MaterialCanConnect,
            [&catalog](VengGraph::NodeTypeId id) { return catalog.ShapeOf(id); },
            [&catalog](VengGraph::NodeTypeId id)
            {
                const VengGraph::NodeType* type = catalog.Find(id);
                return type != nullptr ? type->PropertySize : usize{0};
            });

        const VengGraph::NodeGraphReadOutcome outcome =
            VengGraph::ReadNodeGraph(graphDoc, graph, catalog);
        if (outcome != VengGraph::NodeGraphReadOutcome::Loaded)
        {
            return std::unexpected(fmt::format(
                "shader importer: graph source '{}' is malformed or has a newer format version",
                graphPath.string()));
        }

        const Result<VengGraph::GeneratedFragment> generated =
            VengGraph::CompileMaterialGraph(graph, catalog, emit, domain);
        if (!generated)
        {
            return std::unexpected(fmt::format("shader importer: graph source '{}': {}",
                                               graphPath.string(), generated.error()));
        }

        return GraphShaderSource{
            .IsGraph = true, .Source = generated->Source, .GraphPath = graphPath, .Domain = domain};
    }
}
