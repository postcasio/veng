#include "MeshImporter.h"

#include <cstring>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <Veng/Asset/CookedBlobs.h>

namespace Veng::Cook
{
    namespace
    {
        // Canonical mesh vertex layout: position/normal/tangent/uv, interleaved,
        // all 32-bit float. Format ordinals mirror Veng::Renderer::Format
        // (Renderer/Types.h) — kept in sync by hand per the cycle-avoidance rule
        // in assetpack's CookedBlobs.h. MeshLoader validates the cooked attribute
        // descriptor against this same layout.
        constexpr u32 FormatRGBA32Sfloat = 10;
        constexpr u32 FormatRGB32Sfloat = 9;
        constexpr u32 FormatRG32Sfloat = 8;
        constexpr u32 IndexTypeU32 = 1; // underlying Renderer::IndexType::U32

        // One interleaved vertex in the canonical layout (48 bytes, 12 floats).
        // Tangent is a vec4: xyz is the tangent direction, w is the handedness sign (±1)
        // for reconstructing the bitangent in-shader as cross(N, T.xyz) * T.w.
        struct CanonicalVertex
        {
            f32 Position[3];
            f32 Normal[3];
            f32 Tangent[4];
            f32 UV[2];
        };

        static_assert(sizeof(CanonicalVertex) == 48, "canonical vertex must be tightly packed");

        // Reads a bool field from `import`, returning `fallback` when absent.
        bool ImportFlag(const json& import, const char* key, bool fallback)
        {
            if (import.contains(key) && import[key].is_boolean())
                return import[key].get<bool>();
            return fallback;
        }
    }

    Result<vector<u8>> MeshImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
            return std::unexpected("mesh importer: missing or invalid 'source'");

        const path sourcePath = context.PackDir / entry["source"].get<string>();

        std::ifstream sourceFile(sourcePath, std::ios::binary);
        if (!sourceFile)
            return std::unexpected(
                fmt::format("mesh importer: failed to open '{}'", sourcePath.string()));

        std::ostringstream contentStream;
        contentStream << sourceFile.rdbuf();
        const json meshJson = json::parse(contentStream.str(), nullptr, false);
        if (meshJson.is_discarded() || !meshJson.is_object())
            return std::unexpected(
                fmt::format("mesh importer: '{}': invalid JSON", sourcePath.string()));

        if (!meshJson.contains("model") || !meshJson["model"].is_string())
        {
            return std::unexpected(fmt::format("mesh importer: '{}': missing or invalid 'model'",
                                               sourcePath.string()));
        }

        const path modelPath = sourcePath.parent_path() / meshJson["model"].get<string>();
        context.RecordDependency(modelPath);

        // Import settings -> assimp post-process flags. Defaults: generate normals +
        // tangents, join identical verts.
        const json import = meshJson.contains("import") && meshJson["import"].is_object()
                                ? meshJson["import"]
                                : json::object();

        const f32 scale = import.contains("scale") && import["scale"].is_number()
                              ? import["scale"].get<f32>()
                              : 1.0f;

        unsigned int flags = aiProcess_Triangulate;
        if (ImportFlag(import, "join_identical_vertices", true))
            flags |= aiProcess_JoinIdenticalVertices;
        if (ImportFlag(import, "generate_normals", true))
            flags |= aiProcess_GenSmoothNormals;
        if (ImportFlag(import, "generate_tangents", true))
            flags |= aiProcess_CalcTangentSpace;
        if (ImportFlag(import, "flip_uv", false))
            flags |= aiProcess_FlipUVs;

        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(modelPath.string(), flags);
        if (scene == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 ||
            scene->mRootNode == nullptr)
        {
            return std::unexpected(
                fmt::format("mesh importer: '{}': assimp failed to import '{}': {}",
                            sourcePath.string(), modelPath.string(), importer.GetErrorString()));
        }

        if (scene->mNumMeshes == 0)
            return std::unexpected(fmt::format("mesh importer: '{}': no meshes in '{}'",
                                               sourcePath.string(), modelPath.string()));

        // Per-submesh material overrides: { "<submesh index>": <AssetId u64> }.
        const json materials = meshJson.contains("materials") && meshJson["materials"].is_object()
                                   ? meshJson["materials"]
                                   : json::object();

        // Flatten every assimp mesh into one interleaved vertex buffer + one u32
        // index buffer; each assimp mesh becomes one CookedSubMesh.
        vector<CanonicalVertex> vertices;
        vector<u32> indices;
        vector<CookedSubMesh> subMeshes;

        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
        {
            const aiMesh* mesh = scene->mMeshes[meshIndex];

            const u32 vertexBase = static_cast<u32>(vertices.size());
            const u32 indexOffset = static_cast<u32>(indices.size());

            for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
            {
                CanonicalVertex vertex{};

                const aiVector3D position = mesh->mVertices[v];
                vertex.Position[0] = position.x * scale;
                vertex.Position[1] = position.y * scale;
                vertex.Position[2] = position.z * scale;

                if (mesh->HasNormals())
                {
                    vertex.Normal[0] = mesh->mNormals[v].x;
                    vertex.Normal[1] = mesh->mNormals[v].y;
                    vertex.Normal[2] = mesh->mNormals[v].z;
                }

                if (mesh->HasTangentsAndBitangents())
                {
                    vertex.Tangent[0] = mesh->mTangents[v].x;
                    vertex.Tangent[1] = mesh->mTangents[v].y;
                    vertex.Tangent[2] = mesh->mTangents[v].z;

                    // Encode handedness: the sign that makes
                    // cross(N, T) * w reproduce assimp's bitangent. This is
                    // the single bit that flips across mirrored UV islands and
                    // cannot be derived from N and T alone.
                    const aiVector3D expected = mesh->mNormals[v] ^ mesh->mTangents[v];
                    vertex.Tangent[3] = (expected * mesh->mBitangents[v] < 0.0f) ? -1.0f : 1.0f;
                }

                if (mesh->HasTextureCoords(0))
                {
                    vertex.UV[0] = mesh->mTextureCoords[0][v].x;
                    vertex.UV[1] = mesh->mTextureCoords[0][v].y;
                }

                vertices.push_back(vertex);
            }

            for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
            {
                const aiFace& face = mesh->mFaces[f];
                if (face.mNumIndices != 3)
                {
                    return std::unexpected(fmt::format(
                        "mesh importer: '{}': non-triangle face after triangulation in '{}'",
                        sourcePath.string(), modelPath.string()));
                }

                indices.push_back(vertexBase + face.mIndices[0]);
                indices.push_back(vertexBase + face.mIndices[1]);
                indices.push_back(vertexBase + face.mIndices[2]);
            }

            const u32 indexCount = static_cast<u32>(indices.size()) - indexOffset;

            u64 materialId = 0;
            const string key = std::to_string(meshIndex);
            if (materials.contains(key) && materials[key].is_number_unsigned())
                materialId = materials[key].get<u64>();

            subMeshes.push_back(CookedSubMesh{
                .IndexOffset = indexOffset,
                .IndexCount = indexCount,
                .MaterialId = materialId,
            });
        }

        const CookedVertexAttribute attributes[] = {
            {.Format = FormatRGB32Sfloat, .Offset = 0},   // position
            {.Format = FormatRGB32Sfloat, .Offset = 12},  // normal
            {.Format = FormatRGBA32Sfloat, .Offset = 24}, // tangent (xyz + handedness w)
            {.Format = FormatRG32Sfloat, .Offset = 40},   // uv
        };

        CookedMeshHeader header{};
        header.VertexStride = sizeof(CanonicalVertex);
        header.VertexCount = static_cast<u32>(vertices.size());
        header.IndexCount = static_cast<u32>(indices.size());
        header.IndexType = IndexTypeU32;
        header.SubMeshCount = static_cast<u32>(subMeshes.size());
        header.AttributeCount = static_cast<u32>(std::size(attributes));

        const usize attributeBytes = sizeof(attributes);
        const usize subMeshBytes = subMeshes.size() * sizeof(CookedSubMesh);
        const usize vertexBytes = vertices.size() * sizeof(CanonicalVertex);
        const usize indexBytes = indices.size() * sizeof(u32);

        vector<u8> blob(sizeof(CookedMeshHeader) + attributeBytes + subMeshBytes + vertexBytes +
                        indexBytes);
        usize cursor = 0;
        std::memcpy(blob.data() + cursor, &header, sizeof(header));
        cursor += sizeof(header);
        std::memcpy(blob.data() + cursor, attributes, attributeBytes);
        cursor += attributeBytes;
        std::memcpy(blob.data() + cursor, subMeshes.data(), subMeshBytes);
        cursor += subMeshBytes;
        std::memcpy(blob.data() + cursor, vertices.data(), vertexBytes);
        cursor += vertexBytes;
        std::memcpy(blob.data() + cursor, indices.data(), indexBytes);

        return blob;
    }
}
