#include "CollisionShapeImporter.h"
#include <Veng/Asset/Path.h>

#include <cstring>

#include <fmt/format.h>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/JsonFile.h>

#include "ConvexHull.h"
#include "ImportOrientation.h"

namespace Veng::Cook
{
    namespace
    {
        /// @brief Reads a source model's every vertex position and triangle, in one flat set.
        ///
        /// Collision geometry has no submeshes and no materials — the solver wants one surface —
        /// so every assimp mesh is concatenated into a single position and index list.
        /// @param scene        The imported model.
        /// @param scale        Uniform scale applied to each position.
        /// @param orientation  Rotation reconciling the source's axis convention with the engine's.
        /// @param points       Destination for the positions.
        /// @param indices      Destination for the triangle indices.
        /// @return An error when a face survived triangulation with other than three indices.
        [[nodiscard]] VoidResult GatherGeometry(const aiScene* scene, const f32 scale,
                                                const ImportOrientation& orientation,
                                                vector<vec3>& points, vector<u32>& indices)
        {
            for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
            {
                const aiMesh* mesh = scene->mMeshes[meshIndex];
                const auto base = static_cast<u32>(points.size());
                for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
                {
                    const aiVector3D position = mesh->mVertices[v];
                    points.emplace_back(orientation.Reorient(
                        vec3(position.x * scale, position.y * scale, position.z * scale)));
                }
                for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
                {
                    const aiFace& face = mesh->mFaces[f];
                    if (face.mNumIndices != 3)
                    {
                        return std::unexpected(string("non-triangle face survived triangulation"));
                    }
                    indices.emplace_back(base + face.mIndices[0]);
                    indices.emplace_back(base + face.mIndices[1]);
                    indices.emplace_back(base + face.mIndices[2]);
                }
            }
            return {};
        }

        /// @brief Packs a header, positions and indices into the cooked blob's byte layout.
        /// @param mode     The geometry the blob carries.
        /// @param points   The positions.
        /// @param indices  The triangle indices; empty for a convex hull.
        /// @return The cooked bytes.
        [[nodiscard]] vector<u8> PackBlob(const CookedCollisionGeometry mode,
                                          const vector<vec3>& points, const vector<u32>& indices)
        {
            const CookedCollisionShapeHeader header{
                .Version = CookedCollisionShapeVersion,
                .Mode = static_cast<u32>(mode),
                .PointCount = static_cast<u32>(points.size()),
                .IndexCount = static_cast<u32>(indices.size()),
            };

            const usize pointBytes = points.size() * 3 * sizeof(f32);
            const usize indexBytes = indices.size() * sizeof(u32);
            vector<u8> blob(sizeof(header) + pointBytes + indexBytes);

            usize cursor = 0;
            std::memcpy(blob.data() + cursor, &header, sizeof(header));
            cursor += sizeof(header);
            for (const vec3 point : points)
            {
                const f32 xyz[3] = {point.x, point.y, point.z};
                std::memcpy(blob.data() + cursor, xyz, sizeof(xyz));
                cursor += sizeof(xyz);
            }
            if (indexBytes > 0)
            {
                std::memcpy(blob.data() + cursor, indices.data(), indexBytes);
            }
            return blob;
        }

        /// @brief Packs a compound's child table plus its shared point/index region into a blob.
        /// @param children  The child table, each naming its slice of the shared regions.
        /// @param points    Every Convex/Mesh child's vertices, concatenated.
        /// @param indices   Every Mesh child's triangle indices, concatenated (child-local).
        [[nodiscard]] vector<u8> PackCompoundBlob(const vector<CookedCollisionChild>& children,
                                                  const vector<vec3>& points,
                                                  const vector<u32>& indices)
        {
            const CookedCollisionShapeHeader header{
                .Version = CookedCollisionShapeVersion,
                .Mode = static_cast<u32>(CookedCollisionGeometry::Compound),
                .PointCount = static_cast<u32>(points.size()),
                .IndexCount = static_cast<u32>(indices.size()),
                .ChildCount = static_cast<u32>(children.size()),
            };

            const usize childBytes = children.size() * sizeof(CookedCollisionChild);
            const usize pointBytes = points.size() * 3 * sizeof(f32);
            const usize indexBytes = indices.size() * sizeof(u32);
            vector<u8> blob(sizeof(header) + childBytes + pointBytes + indexBytes);

            usize cursor = 0;
            std::memcpy(blob.data() + cursor, &header, sizeof(header));
            cursor += sizeof(header);
            if (childBytes > 0)
            {
                std::memcpy(blob.data() + cursor, children.data(), childBytes);
                cursor += childBytes;
            }
            for (const vec3 point : points)
            {
                const f32 xyz[3] = {point.x, point.y, point.z};
                std::memcpy(blob.data() + cursor, xyz, sizeof(xyz));
                cursor += sizeof(xyz);
            }
            if (indexBytes > 0)
            {
                std::memcpy(blob.data() + cursor, indices.data(), indexBytes);
            }
            return blob;
        }

        /// @brief The uniform scale and orientation an "import" block declares for a model.
        struct ParsedImport
        {
            /// @brief Uniform scale applied to each source vertex.
            f32 Scale = 1.0f;
            /// @brief Rotation reconciling the source's axis convention with the engine's.
            ImportOrientation Orientation;
        };

        /// @brief Parses the shared "import": { "scale", "orientation" } block; absent is identity.
        [[nodiscard]] Result<ParsedImport> ParseImport(const json& source)
        {
            const json import = source.contains("import") && source["import"].is_object()
                                    ? source["import"]
                                    : json::object();
            ParsedImport parsed;
            parsed.Scale = import.contains("scale") && import["scale"].is_number()
                               ? import["scale"].get<f32>()
                               : 1.0f;
            const Result<ImportOrientation> orientation = ParseImportOrientation(import);
            if (!orientation)
            {
                return std::unexpected(orientation.error());
            }
            parsed.Orientation = *orientation;
            return parsed;
        }

        /// @brief Reads a model file into raw (unwelded, unhulled) collision points and indices.
        [[nodiscard]] VoidResult ReadModel(const path& modelPath, const ParsedImport& import,
                                           vector<vec3>& points, vector<u32>& indices)
        {
            Assimp::Importer importer;
            // Drop point/line primitives so every surviving face is a triangle; a modelled hull can
            // carry locator geometry triangulation would leave non-3-index.
            importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
                                        aiPrimitiveType_POINT | aiPrimitiveType_LINE);
            const aiScene* scene = importer.ReadFile(modelPath.string(),
                                                     aiProcess_Triangulate | aiProcess_SortByPType);
            if (scene == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 ||
                scene->mRootNode == nullptr)
            {
                return std::unexpected(fmt::format("assimp failed to import '{}': {}",
                                                   modelPath.string(), importer.GetErrorString()));
            }
            if (scene->mNumMeshes == 0)
            {
                return std::unexpected(fmt::format("no meshes in '{}'", modelPath.string()));
            }
            if (const VoidResult gathered =
                    GatherGeometry(scene, import.Scale, import.Orientation, points, indices);
                !gathered)
            {
                return std::unexpected(
                    fmt::format("{} in '{}'", gathered.error(), modelPath.string()));
            }
            if (points.empty())
            {
                return std::unexpected(fmt::format(
                    "'{}' has no vertices to build collision geometry from", modelPath.string()));
            }
            return {};
        }

        /// @brief Parses one compound child, appending any hull/mesh geometry to the shared regions.
        ///
        /// A primitive child (box/sphere/capsule) is described inline by its dimensions; a convex
        /// or mesh child names a model whose geometry is hulled/welded and appended to
        /// @p sharedPoints (and, for a mesh, @p sharedIndices), the child recording its slice.
        [[nodiscard]] Result<CookedCollisionChild>
        AppendChild(const CookContext& context, const path& sourcePath, const json& child,
                    const ParsedImport& topImport, vector<vec3>& sharedPoints,
                    vector<u32>& sharedIndices)
        {
            if (!child.is_object() || !child.contains("shape") || !child["shape"].is_string())
            {
                return std::unexpected(string("a compound child needs a string \"shape\""));
            }
            const string shape = child["shape"].get<string>();

            CookedCollisionChild out;
            if (child.contains("offset"))
            {
                const json& offset = child["offset"];
                if (!offset.is_array() || offset.size() != 3)
                {
                    return std::unexpected(string("a child \"offset\" must be [x, y, z]"));
                }
                for (u32 i = 0; i < 3; ++i)
                {
                    out.Offset[i] = offset[i].get<f32>();
                }
            }
            if (child.contains("rotation"))
            {
                const json& rotation = child["rotation"];
                if (!rotation.is_array() || rotation.size() != 4)
                {
                    return std::unexpected(string("a child \"rotation\" must be [x, y, z, w]"));
                }
                for (u32 i = 0; i < 4; ++i)
                {
                    out.Rotation[i] = rotation[i].get<f32>();
                }
            }

            if (shape == "box")
            {
                if (!child.contains("halfExtents") || !child["halfExtents"].is_array() ||
                    child["halfExtents"].size() != 3)
                {
                    return std::unexpected(string("a box child needs \"halfExtents\": [x, y, z]"));
                }
                out.Kind = static_cast<u32>(CookedCollisionChildKind::Box);
                for (u32 i = 0; i < 3; ++i)
                {
                    out.Extents[i] = child["halfExtents"][i].get<f32>();
                }
                return out;
            }
            if (shape == "sphere")
            {
                if (!child.contains("radius") || !child["radius"].is_number())
                {
                    return std::unexpected(string("a sphere child needs a numeric \"radius\""));
                }
                out.Kind = static_cast<u32>(CookedCollisionChildKind::Sphere);
                out.Extents[0] = child["radius"].get<f32>();
                return out;
            }
            if (shape == "capsule")
            {
                if (!child.contains("radius") || !child["radius"].is_number() ||
                    !child.contains("halfHeight") || !child["halfHeight"].is_number())
                {
                    return std::unexpected(
                        string("a capsule child needs numeric \"radius\" and \"halfHeight\""));
                }
                out.Kind = static_cast<u32>(CookedCollisionChildKind::Capsule);
                out.Extents[0] = child["radius"].get<f32>();
                out.Extents[1] = child["halfHeight"].get<f32>();
                return out;
            }
            if (shape == "convex" || shape == "mesh")
            {
                if (!child.contains("model") || !child["model"].is_string())
                {
                    return std::unexpected(
                        fmt::format("a {} child needs a string \"model\"", shape));
                }
                const path modelPath = sourcePath.parent_path() / child["model"].get<string>();
                context.RecordDependency(modelPath);

                // A child inherits the compound's import block unless it declares its own.
                ParsedImport import = topImport;
                if (child.contains("import"))
                {
                    const Result<ParsedImport> parsed = ParseImport(child);
                    if (!parsed)
                    {
                        return std::unexpected(parsed.error());
                    }
                    import = *parsed;
                }

                vector<vec3> raw;
                vector<u32> rawIndices;
                if (const VoidResult read = ReadModel(modelPath, import, raw, rawIndices); !read)
                {
                    return std::unexpected(read.error());
                }

                if (shape == "convex")
                {
                    vector<vec3> hull;
                    if (!BuildConvexHull(raw, hull))
                    {
                        return std::unexpected(
                            fmt::format("the convex hull of '{}' exceeds {} points",
                                        modelPath.string(), MaxConvexHullPoints));
                    }
                    out.Kind = static_cast<u32>(CookedCollisionChildKind::Convex);
                    out.PointOffset = static_cast<u32>(sharedPoints.size());
                    out.PointCount = static_cast<u32>(hull.size());
                    sharedPoints.insert(sharedPoints.end(), hull.begin(), hull.end());
                    return out;
                }

                vector<vec3> welded;
                vector<u32> weldedIndices;
                WeldTriangleMesh(raw, rawIndices, welded, weldedIndices);
                if (weldedIndices.empty())
                {
                    return std::unexpected(fmt::format("'{}' has no triangles left after welding",
                                                       modelPath.string()));
                }
                out.Kind = static_cast<u32>(CookedCollisionChildKind::Mesh);
                out.PointOffset = static_cast<u32>(sharedPoints.size());
                out.PointCount = static_cast<u32>(welded.size());
                out.IndexOffset = static_cast<u32>(sharedIndices.size());
                out.IndexCount = static_cast<u32>(weldedIndices.size());
                sharedPoints.insert(sharedPoints.end(), welded.begin(), welded.end());
                sharedIndices.insert(sharedIndices.end(), weldedIndices.begin(),
                                     weldedIndices.end());
                return out;
            }

            return std::unexpected(fmt::format(
                "unknown child shape '{}' (expected box, sphere, capsule, convex or mesh)", shape));
        }
    }

    Result<vector<u8>> CollisionShapeImporter::Cook(const CookContext& context,
                                                    const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("collision shape importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();

        const Result<json> sourceJsonResult = ReadJsonFile(sourcePath, "collision shape importer");
        if (!sourceJsonResult)
        {
            return std::unexpected(sourceJsonResult.error());
        }
        const json& shapeJson = *sourceJsonResult;

        if (!shapeJson.contains("mode") || !shapeJson["mode"].is_string())
        {
            return std::unexpected(fmt::format(
                "collision shape importer: '{}': missing or invalid 'mode' (expected \"convex\", "
                "\"mesh\" or \"compound\")",
                sourcePath.string()));
        }
        const string mode = shapeJson["mode"].get<string>();

        // The same "import": { "scale": …, "orientation": … } a *.mesh.json takes, so a collision
        // shape and the render mesh cooked from one model stay the same size and the same way round.
        // For a compound it applies to every child model that does not override it.
        const Result<ParsedImport> import = ParseImport(shapeJson);
        if (!import)
        {
            return std::unexpected(fmt::format("collision shape importer: '{}': {}",
                                               sourcePath.string(), import.error()));
        }

        if (mode == "compound")
        {
            if (!shapeJson.contains("children") || !shapeJson["children"].is_array() ||
                shapeJson["children"].empty())
            {
                return std::unexpected(
                    fmt::format("collision shape importer: '{}': a compound needs a non-empty "
                                "\"children\" array",
                                sourcePath.string()));
            }
            vector<CookedCollisionChild> children;
            vector<vec3> points;
            vector<u32> indices;
            for (const json& child : shapeJson["children"])
            {
                const Result<CookedCollisionChild> built =
                    AppendChild(context, sourcePath, child, *import, points, indices);
                if (!built)
                {
                    return std::unexpected(fmt::format("collision shape importer: '{}': {}",
                                                       sourcePath.string(), built.error()));
                }
                children.push_back(*built);
            }
            return PackCompoundBlob(children, points, indices);
        }

        if (mode != "convex" && mode != "mesh")
        {
            return std::unexpected(fmt::format(
                "collision shape importer: '{}': unknown mode '{}' (expected \"convex\", \"mesh\" "
                "or \"compound\")",
                sourcePath.string(), mode));
        }
        if (!shapeJson.contains("model") || !shapeJson["model"].is_string())
        {
            return std::unexpected(fmt::format(
                "collision shape importer: '{}': missing or invalid 'model'", sourcePath.string()));
        }

        const path modelPath = sourcePath.parent_path() / shapeJson["model"].get<string>();
        context.RecordDependency(modelPath);

        vector<vec3> points;
        vector<u32> indices;
        if (const VoidResult read = ReadModel(modelPath, *import, points, indices); !read)
        {
            return std::unexpected(fmt::format("collision shape importer: '{}': {}",
                                               sourcePath.string(), read.error()));
        }

        if (mode == "convex")
        {
            vector<vec3> hull;
            if (!BuildConvexHull(points, hull))
            {
                return std::unexpected(fmt::format(
                    "collision shape importer: '{}': the convex hull of '{}' exceeds {} points; "
                    "author a simpler source, or split the body across several shape assets",
                    sourcePath.string(), modelPath.string(), MaxConvexHullPoints));
            }
            return PackBlob(CookedCollisionGeometry::Convex, hull, {});
        }

        vector<vec3> weldedPoints;
        vector<u32> weldedIndices;
        WeldTriangleMesh(points, indices, weldedPoints, weldedIndices);
        if (weldedIndices.empty())
        {
            return std::unexpected(fmt::format(
                "collision shape importer: '{}': '{}' has no triangles left after welding",
                sourcePath.string(), modelPath.string()));
        }
        return PackBlob(CookedCollisionGeometry::Mesh, weldedPoints, weldedIndices);
    }
}
