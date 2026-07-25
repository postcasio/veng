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

        if (!shapeJson.contains("model") || !shapeJson["model"].is_string())
        {
            return std::unexpected(fmt::format(
                "collision shape importer: '{}': missing or invalid 'model'", sourcePath.string()));
        }
        if (!shapeJson.contains("mode") || !shapeJson["mode"].is_string())
        {
            return std::unexpected(fmt::format(
                "collision shape importer: '{}': missing or invalid 'mode' (expected \"convex\" "
                "or \"mesh\")",
                sourcePath.string()));
        }

        const string mode = shapeJson["mode"].get<string>();
        if (mode != "convex" && mode != "mesh")
        {
            return std::unexpected(fmt::format(
                "collision shape importer: '{}': unknown mode '{}' (expected \"convex\" or "
                "\"mesh\")",
                sourcePath.string(), mode));
        }

        const path modelPath = sourcePath.parent_path() / shapeJson["model"].get<string>();
        context.RecordDependency(modelPath);

        // The same "import": { "scale": …, "orientation": … } a *.mesh.json takes, so a collision
        // shape and the render mesh cooked from one model stay the same size and the same way round.
        const json import = shapeJson.contains("import") && shapeJson["import"].is_object()
                                ? shapeJson["import"]
                                : json::object();
        const f32 scale = import.contains("scale") && import["scale"].is_number()
                              ? import["scale"].get<f32>()
                              : 1.0f;
        const Result<ImportOrientation> parsedOrientation = ParseImportOrientation(import);
        if (!parsedOrientation)
        {
            return std::unexpected(fmt::format("collision shape importer: '{}': {}",
                                               sourcePath.string(), parsedOrientation.error()));
        }

        Assimp::Importer importer;
        // Drop point/line primitives so every surviving face is a triangle; a modelled hull can
        // carry locator geometry triangulation would leave non-3-index.
        importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
                                    aiPrimitiveType_POINT | aiPrimitiveType_LINE);
        const aiScene* scene =
            importer.ReadFile(modelPath.string(), aiProcess_Triangulate | aiProcess_SortByPType);
        if (scene == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 ||
            scene->mRootNode == nullptr)
        {
            return std::unexpected(
                fmt::format("collision shape importer: '{}': assimp failed to import '{}': {}",
                            sourcePath.string(), modelPath.string(), importer.GetErrorString()));
        }
        if (scene->mNumMeshes == 0)
        {
            return std::unexpected(fmt::format("collision shape importer: '{}': no meshes in '{}'",
                                               sourcePath.string(), modelPath.string()));
        }

        vector<vec3> points;
        vector<u32> indices;
        if (const VoidResult gathered =
                GatherGeometry(scene, scale, *parsedOrientation, points, indices);
            !gathered)
        {
            return std::unexpected(fmt::format("collision shape importer: '{}': {} in '{}'",
                                               sourcePath.string(), gathered.error(),
                                               modelPath.string()));
        }
        if (points.empty())
        {
            return std::unexpected(fmt::format("collision shape importer: '{}': '{}' has no "
                                               "vertices to build collision geometry from",
                                               sourcePath.string(), modelPath.string()));
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
