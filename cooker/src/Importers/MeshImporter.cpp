#include "MeshImporter.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <span>
#include <sstream>

#include <fmt/format.h>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <Veng/Asset/CookedBlobs.h>

#include "SkeletonSource.h"

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
        constexpr u32 FormatRGBA16Uint = 20;
        constexpr u32 IndexTypeU32 = 1; // underlying Renderer::IndexType::U32

        // The maximum bone influences per skinned vertex (aiProcess_LimitBoneWeights caps
        // assimp's output to this); matches the engine's skinned vertex shader.
        constexpr u32 MaxBoneInfluences = 4;

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

        // One interleaved vertex in the skinned layout (72 bytes): the canonical attributes
        // plus four 16-bit bone indices and four float weights. The bone indices reference
        // the canonical bone order shared with the SkeletonImporter (SkeletonSource).
        struct SkinnedVertex
        {
            f32 Position[3];
            f32 Normal[3];
            f32 Tangent[4];
            f32 UV[2];
            u16 BoneIndices[4];
            f32 BoneWeights[4];
        };

        static_assert(sizeof(SkinnedVertex) == 72, "skinned vertex must be tightly packed");

        // Reads a bool field from `import`, returning `fallback` when absent.
        bool ImportFlag(const json& import, const char* key, bool fallback)
        {
            if (import.contains(key) && import[key].is_boolean())
            {
                return import[key].get<bool>();
            }
            return fallback;
        }

        // Fills a CanonicalVertex's shared (non-skin) attributes from assimp mesh data.
        template <typename Vertex>
        void FillCommonAttributes(Vertex& vertex, const aiMesh* mesh, unsigned int v, f32 scale)
        {
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

                // Encode handedness: the sign that makes cross(N, T) * w reproduce assimp's
                // bitangent. This is the single bit that flips across mirrored UV islands and
                // cannot be derived from N and T alone.
                const aiVector3D expected = mesh->mNormals[v] ^ mesh->mTangents[v];
                vertex.Tangent[3] = (expected * mesh->mBitangents[v] < 0.0f) ? -1.0f : 1.0f;
            }

            if (mesh->HasTextureCoords(0))
            {
                vertex.UV[0] = mesh->mTextureCoords[0][v].x;
                vertex.UV[1] = mesh->mTextureCoords[0][v].y;
            }
        }

        // One bone influence on a vertex: the canonical bone index plus its weight.
        struct Influence
        {
            u16 Bone = 0;
            f32 Weight = 0.0f;
        };

        // Reads a per-submesh material override AssetId from { "<index>": <u64> }, or 0.
        u64 SubMeshMaterialId(const json& materials, unsigned int meshIndex)
        {
            const string key = std::to_string(meshIndex);
            if (materials.contains(key) && materials[key].is_number_unsigned())
            {
                return materials[key].get<u64>();
            }
            return 0;
        }
    }

    Result<vector<u8>> MeshImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("mesh importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();

        const std::ifstream sourceFile(sourcePath, std::ios::binary);
        if (!sourceFile)
        {
            return std::unexpected(
                fmt::format("mesh importer: failed to open '{}'", sourcePath.string()));
        }

        std::ostringstream contentStream;
        contentStream << sourceFile.rdbuf();
        const json meshJson = json::parse(contentStream.str(), nullptr, false);
        if (meshJson.is_discarded() || !meshJson.is_object())
        {
            return std::unexpected(
                fmt::format("mesh importer: '{}': invalid JSON", sourcePath.string()));
        }

        if (!meshJson.contains("model") || !meshJson["model"].is_string())
        {
            return std::unexpected(fmt::format("mesh importer: '{}': missing or invalid 'model'",
                                               sourcePath.string()));
        }

        const path modelPath = sourcePath.parent_path() / meshJson["model"].get<string>();
        context.RecordDependency(modelPath);

        // A "skeleton" key marks a skinned mesh: its vertices carry bone indices/weights and
        // the cooked header references the named Skeleton asset.
        const bool skinned = meshJson.contains("skeleton") && meshJson["skeleton"].is_number();
        const u64 skeletonId = skinned ? meshJson["skeleton"].get<u64>() : 0;

        // Import settings -> assimp post-process flags. Defaults: generate normals +
        // tangents, join identical verts.
        const json import = meshJson.contains("import") && meshJson["import"].is_object()
                                ? meshJson["import"]
                                : json::object();

        // A skinned mesh keeps raw model units (its bone bind/animation translations are not
        // scaled here); scale a skinned character via its entity Transform instead.
        const f32 scale = (!skinned && import.contains("scale") && import["scale"].is_number())
                              ? import["scale"].get<f32>()
                              : 1.0f;

        unsigned int flags = aiProcess_Triangulate;
        if (ImportFlag(import, "join_identical_vertices", true))
        {
            flags |= aiProcess_JoinIdenticalVertices;
        }
        if (ImportFlag(import, "generate_normals", true))
        {
            flags |= aiProcess_GenSmoothNormals;
        }
        if (ImportFlag(import, "generate_tangents", true))
        {
            flags |= aiProcess_CalcTangentSpace;
        }
        if (ImportFlag(import, "flip_uv", false))
        {
            flags |= aiProcess_FlipUVs;
        }
        if (skinned)
        {
            // Cap each vertex to four influences (and renormalize), matching the skinned layout.
            flags |= aiProcess_LimitBoneWeights;
        }

        // Drop point/line primitives so every surviving face is a triangle: a rigged export can
        // carry locator/helper geometry as points or lines that triangulation leaves non-3-index.
        flags |= aiProcess_SortByPType;

        Assimp::Importer importer;
        importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
                                    aiPrimitiveType_POINT | aiPrimitiveType_LINE);
        // Collapse FBX pivots so each bone is a single node whose animation channel fully
        // describes its local transform — the runtime sampler resamples T*R*S per node and
        // cannot reconstruct a transform split across synthetic $AssimpFbx$ pivot nodes.
        // Must match the same setting in the skeleton and animation importers: all three
        // share one canonical bone order, so the bone count must agree.
        importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        const aiScene* scene = importer.ReadFile(modelPath.string(), flags);
        if (scene == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 ||
            scene->mRootNode == nullptr)
        {
            return std::unexpected(
                fmt::format("mesh importer: '{}': assimp failed to import '{}': {}",
                            sourcePath.string(), modelPath.string(), importer.GetErrorString()));
        }

        if (scene->mNumMeshes == 0)
        {
            return std::unexpected(fmt::format("mesh importer: '{}': no meshes in '{}'",
                                               sourcePath.string(), modelPath.string()));
        }

        // Per-submesh material overrides: { "<submesh index>": <AssetId u64> }.
        const json materials = meshJson.contains("materials") && meshJson["materials"].is_object()
                                   ? meshJson["materials"]
                                   : json::object();

        // Skinned meshes resolve the canonical bone order so each vertex's per-mesh bone
        // index maps to the skeleton's global bone index.
        ImportedSkeleton skeleton;
        if (skinned)
        {
            const Result<ImportedSkeleton> built = BuildImportedSkeleton(scene);
            if (!built)
            {
                return std::unexpected(
                    fmt::format("mesh importer: '{}': {}", sourcePath.string(), built.error()));
            }
            if (!built->HasSkinningBones)
            {
                return std::unexpected(fmt::format(
                    "mesh importer: '{}': 'skeleton' set but model '{}' has no skinning bones",
                    sourcePath.string(), modelPath.string()));
            }
            skeleton = std::move(*built);
        }

        // Flatten every assimp mesh into one interleaved vertex buffer + one u32 index buffer;
        // each assimp mesh becomes one CookedSubMesh.
        vector<CanonicalVertex> staticVertices;
        vector<SkinnedVertex> skinnedVertices;
        vector<u32> indices;
        vector<CookedSubMesh> subMeshes;

        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
        {
            const aiMesh* mesh = scene->mMeshes[meshIndex];

            const u32 vertexBase =
                static_cast<u32>(skinned ? skinnedVertices.size() : staticVertices.size());
            const u32 indexOffset = static_cast<u32>(indices.size());

            if (skinned)
            {
                // Gather up to four influences per vertex from the mesh's bones (each bone
                // lists the vertices it weights); assimp has already capped/normalized them.
                vector<vector<Influence>> influences(mesh->mNumVertices);
                for (unsigned int b = 0; b < mesh->mNumBones; ++b)
                {
                    const aiBone* bone = mesh->mBones[b];
                    const auto it = skeleton.NameToIndex.find(bone->mName.C_Str());
                    if (it == skeleton.NameToIndex.end())
                    {
                        continue;
                    }
                    const u16 boneIndex = static_cast<u16>(it->second);
                    for (unsigned int w = 0; w < bone->mNumWeights; ++w)
                    {
                        const aiVertexWeight& weight = bone->mWeights[w];
                        if (weight.mWeight > 0.0f)
                        {
                            influences[weight.mVertexId].push_back({boneIndex, weight.mWeight});
                        }
                    }
                }

                for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
                {
                    SkinnedVertex vertex{};
                    FillCommonAttributes(vertex, mesh, v, scale);

                    vector<Influence>& vertexInfluences = influences[v];
                    std::ranges::sort(vertexInfluences, [](const Influence& a, const Influence& b)
                                      { return a.Weight > b.Weight; });
                    if (vertexInfluences.size() > MaxBoneInfluences)
                    {
                        vertexInfluences.resize(MaxBoneInfluences);
                    }

                    f32 total = 0.0f;
                    for (const Influence& influence : vertexInfluences)
                    {
                        total += influence.Weight;
                    }
                    // An unweighted vertex pins to bone 0 so it follows the skeleton root.
                    if (total <= 0.0f)
                    {
                        vertex.BoneIndices[0] = 0;
                        vertex.BoneWeights[0] = 1.0f;
                    }
                    else
                    {
                        for (usize i = 0; i < vertexInfluences.size(); ++i)
                        {
                            vertex.BoneIndices[i] = vertexInfluences[i].Bone;
                            vertex.BoneWeights[i] = vertexInfluences[i].Weight / total;
                        }
                    }

                    skinnedVertices.push_back(vertex);
                }
            }
            else
            {
                for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
                {
                    CanonicalVertex vertex{};
                    FillCommonAttributes(vertex, mesh, v, scale);
                    staticVertices.push_back(vertex);
                }
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

            subMeshes.push_back(CookedSubMesh{
                .IndexOffset = indexOffset,
                .IndexCount = indexCount,
                .MaterialId = SubMeshMaterialId(materials, meshIndex),
            });
        }

        const CookedVertexAttribute staticAttributes[] = {
            {.Format = FormatRGB32Sfloat, .Offset = 0},   // position
            {.Format = FormatRGB32Sfloat, .Offset = 12},  // normal
            {.Format = FormatRGBA32Sfloat, .Offset = 24}, // tangent (xyz + handedness w)
            {.Format = FormatRG32Sfloat, .Offset = 40},   // uv
        };
        const CookedVertexAttribute skinnedAttributes[] = {
            {.Format = FormatRGB32Sfloat, .Offset = 0},   // position
            {.Format = FormatRGB32Sfloat, .Offset = 12},  // normal
            {.Format = FormatRGBA32Sfloat, .Offset = 24}, // tangent (xyz + handedness w)
            {.Format = FormatRG32Sfloat, .Offset = 40},   // uv
            {.Format = FormatRGBA16Uint, .Offset = 48},   // bone indices
            {.Format = FormatRGBA32Sfloat, .Offset = 56}, // bone weights
        };

        const usize vertexCount = skinned ? skinnedVertices.size() : staticVertices.size();
        const usize vertexStride = skinned ? sizeof(SkinnedVertex) : sizeof(CanonicalVertex);
        const usize vertexBytes = vertexCount * vertexStride;
        const void* vertexSource = skinned ? static_cast<const void*>(skinnedVertices.data())
                                           : static_cast<const void*>(staticVertices.data());
        const std::span<const CookedVertexAttribute> attributes =
            skinned ? std::span<const CookedVertexAttribute>(skinnedAttributes)
                    : std::span<const CookedVertexAttribute>(staticAttributes);

        CookedMeshHeader header{};
        header.VertexStride = static_cast<u32>(vertexStride);
        header.VertexCount = static_cast<u32>(vertexCount);
        header.IndexCount = static_cast<u32>(indices.size());
        header.IndexType = IndexTypeU32;
        header.SubMeshCount = static_cast<u32>(subMeshes.size());
        header.AttributeCount = static_cast<u32>(attributes.size());
        header.SkeletonId = skeletonId;

        const usize attributeBytes = attributes.size() * sizeof(CookedVertexAttribute);
        const usize subMeshBytes = subMeshes.size() * sizeof(CookedSubMesh);
        const usize indexBytes = indices.size() * sizeof(u32);

        vector<u8> blob(sizeof(CookedMeshHeader) + attributeBytes + subMeshBytes + vertexBytes +
                        indexBytes);
        usize cursor = 0;
        std::memcpy(blob.data() + cursor, &header, sizeof(header));
        cursor += sizeof(header);
        std::memcpy(blob.data() + cursor, attributes.data(), attributeBytes);
        cursor += attributeBytes;
        std::memcpy(blob.data() + cursor, subMeshes.data(), subMeshBytes);
        cursor += subMeshBytes;
        std::memcpy(blob.data() + cursor, vertexSource, vertexBytes);
        cursor += vertexBytes;
        std::memcpy(blob.data() + cursor, indices.data(), indexBytes);

        return blob;
    }
}
