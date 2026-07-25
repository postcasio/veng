#include "MeshImporter.h"
#include <Veng/Asset/Path.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <span>
#include <sstream>
#include <unordered_set>

#include <fmt/format.h>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Renderer/Types.h>

#include "ImportOrientation.h"
#include "SkeletonSource.h"

namespace Veng::Cook
{
    namespace
    {
        // Canonical mesh vertex layout: position/normal/tangent/uv, interleaved,
        // all 32-bit float. The cooked attribute descriptor stores Renderer::Format
        // ordinals; MeshLoader validates it against this same layout.
        constexpr u32 FormatRGBA32Sfloat = static_cast<u32>(Renderer::Format::RGBA32Sfloat);
        constexpr u32 FormatRGB32Sfloat = static_cast<u32>(Renderer::Format::RGB32Sfloat);
        constexpr u32 FormatRG32Sfloat = static_cast<u32>(Renderer::Format::RG32Sfloat);
        constexpr u32 FormatRGBA16Uint = static_cast<u32>(Renderer::Format::RGBA16Uint);
        constexpr u32 IndexTypeU32 = static_cast<u32>(Renderer::IndexType::U32);

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

        // An assimp vector in the house vocabulary, so the orientation applies to it.
        vec3 ToVec3(const aiVector3D& v)
        {
            return vec3(v.x, v.y, v.z);
        }

        // Fills a CanonicalVertex's shared (non-skin) attributes from assimp mesh data, reoriented
        // out of the source file's declared convention.
        template <typename Vertex>
        void FillCommonAttributes(Vertex& vertex, const aiMesh* mesh, unsigned int v, f32 scale,
                                  const ImportOrientation& orientation)
        {
            const vec3 position = orientation.Reorient(ToVec3(mesh->mVertices[v]) * scale);
            vertex.Position[0] = position.x;
            vertex.Position[1] = position.y;
            vertex.Position[2] = position.z;

            if (mesh->HasNormals())
            {
                const vec3 normal = orientation.Reorient(ToVec3(mesh->mNormals[v]));
                vertex.Normal[0] = normal.x;
                vertex.Normal[1] = normal.y;
                vertex.Normal[2] = normal.z;
            }

            if (mesh->HasTangentsAndBitangents())
            {
                const vec3 tangent = orientation.Reorient(ToVec3(mesh->mTangents[v]));
                vertex.Tangent[0] = tangent.x;
                vertex.Tangent[1] = tangent.y;
                vertex.Tangent[2] = tangent.z;

                // Encode handedness: the sign that makes cross(N, T) * w reproduce assimp's
                // bitangent. This is the single bit that flips across mirrored UV islands and
                // cannot be derived from N and T alone. It is read off the source vectors because
                // the orientation is a proper rotation, which preserves a cross product's sign.
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

        // Writes a nul-terminated name into a fixed cooked-name field, truncating to capacity.
        void SetSocketName(char (&dest)[ShaderNameCapacity], const std::string& name)
        {
            const usize n = std::min(name.size(), static_cast<usize>(ShaderNameCapacity) - 1);
            std::memcpy(dest, name.data(), n);
            dest[n] = '\0';
        }

        // Names of every node the socket rule excludes on grounds other than carrying geometry:
        // the skin joints (an aiBone per entry of the model's skins[*].joints) and the camera
        // nodes. Without the joint exclusion an entire rig becomes sockets — a joint is exactly
        // a node with no mesh and no camera.
        std::unordered_set<std::string> ExcludedNodeNames(const aiScene* scene)
        {
            std::unordered_set<std::string> excluded;
            for (unsigned int m = 0; m < scene->mNumMeshes; ++m)
            {
                const aiMesh* mesh = scene->mMeshes[m];
                for (unsigned int b = 0; b < mesh->mNumBones; ++b)
                {
                    excluded.insert(mesh->mBones[b]->mName.C_Str());
                }
            }
            for (unsigned int c = 0; c < scene->mNumCameras; ++c)
            {
                excluded.insert(scene->mCameras[c]->mName.C_Str());
            }
            return excluded;
        }

        // One authored attachment point: a node name plus its transform in the model's root space.
        struct SocketNode
        {
            std::string Name;
            aiMatrix4x4 Transform;
        };

        // Depth-first collects every descendant of `node` that carries no mesh and is not
        // excluded, composing each one's transform down from `parentTransform`. Called with the
        // scene root and identity, so the root itself never becomes a socket and the transforms
        // land in root space — the space the flattened vertices are in.
        void CollectSockets(const aiNode* node, const aiMatrix4x4& parentTransform,
                            const std::unordered_set<std::string>& excluded,
                            vector<SocketNode>& out)
        {
            for (unsigned int i = 0; i < node->mNumChildren; ++i)
            {
                const aiNode* child = node->mChildren[i];
                const aiMatrix4x4 transform = parentTransform * child->mTransformation;
                const std::string name = child->mName.C_Str();
                if (child->mNumMeshes == 0 && !name.empty() && !excluded.contains(name))
                {
                    out.emplace_back(name, transform);
                }
                CollectSockets(child, transform, excluded, out);
            }
        }

        // Reads a per-submesh material override AssetId from { "<index>": "<hexId>" }, or 0 when
        // the submesh has no override. The map key is the decimal submesh index (not an id); the
        // value is the material's hex id string. A present-but-malformed value is a located error.
        Result<u64> SubMeshMaterialId(const json& materials, unsigned int meshIndex)
        {
            const string key = std::to_string(meshIndex);
            if (!materials.contains(key))
            {
                return 0;
            }
            if (!materials[key].is_string())
            {
                return std::unexpected(
                    fmt::format("mesh importer: 'materials[\"{}\"]' must be a hex id string", key));
            }
            const optional<AssetId> parsed = ParseAssetId(materials[key].get<string>());
            if (!parsed)
            {
                return std::unexpected(
                    fmt::format("mesh importer: 'materials[\"{}\"]' is a malformed hex id '{}'",
                                key, materials[key].get<string>()));
            }
            return parsed->Value;
        }
    }

    Result<vector<u8>> MeshImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("mesh importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();

        const Result<json> meshJsonResult = ReadJsonFile(sourcePath, "mesh importer");
        if (!meshJsonResult)
        {
            return std::unexpected(meshJsonResult.error());
        }
        const json& meshJson = *meshJsonResult;

        if (!meshJson.contains("model") || !meshJson["model"].is_string())
        {
            return std::unexpected(fmt::format("mesh importer: '{}': missing or invalid 'model'",
                                               sourcePath.string()));
        }

        const path modelPath = sourcePath.parent_path() / meshJson["model"].get<string>();
        context.RecordDependency(modelPath);

        // A "skeleton" key marks a skinned mesh: its vertices carry bone indices/weights and
        // the cooked header references the named Skeleton asset.
        const bool skinned = meshJson.contains("skeleton");
        u64 skeletonId = 0;
        if (skinned)
        {
            if (!meshJson["skeleton"].is_string())
            {
                return std::unexpected(
                    fmt::format("mesh importer: '{}': 'skeleton' must be a hex id string",
                                sourcePath.string()));
            }
            const optional<AssetId> parsed = ParseAssetId(meshJson["skeleton"].get<string>());
            if (!parsed)
            {
                return std::unexpected(
                    fmt::format("mesh importer: '{}': 'skeleton' is a malformed hex id '{}'",
                                sourcePath.string(), meshJson["skeleton"].get<string>()));
            }
            skeletonId = parsed->Value;
        }

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

        // The source file's own forward/up convention, reconciled with the engine's here so every
        // position, normal, tangent and socket this cook derives lands in one space.
        const Result<ImportOrientation> parsedOrientation = ParseImportOrientation(import);
        if (!parsedOrientation)
        {
            return std::unexpected(fmt::format("mesh importer: '{}': {}", sourcePath.string(),
                                               parsedOrientation.error()));
        }
        const ImportOrientation& orientation = *parsedOrientation;

        // A skinned mesh's vertices are in bind space, and the bind pose and its animation channels
        // are cooked from the same model by the Skeleton and Animation importers, which keep the
        // source's convention. Rotating the geometry alone would leave the skin disagreeing with
        // the palette that drives it, so the declaration is refused rather than half-applied.
        if (skinned && !orientation.IsIdentity)
        {
            return std::unexpected(fmt::format(
                "mesh importer: '{}': 'import.orientation' is not supported on a skinned mesh; the "
                "Skeleton and Animation assets cooked from '{}' keep the source's convention, so a "
                "rotated skin would disagree with its bind pose",
                sourcePath.string(), modelPath.string()));
        }

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
        // assimp emits UVs in the OpenGL convention (origin bottom-left, V up); veng uploads
        // textures top-row-first and samples them Vulkan-style (origin top-left, V down), so a
        // model's V must be flipped at import to map correctly. Default on; an asset whose
        // source is already authored top-left overrides it to false.
        if (ImportFlag(import, "flip_uv", true))
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
                    FillCommonAttributes(vertex, mesh, v, scale, orientation);

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
                    FillCommonAttributes(vertex, mesh, v, scale, orientation);
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

            const Result<u64> materialId = SubMeshMaterialId(materials, meshIndex);
            if (!materialId)
            {
                return std::unexpected(fmt::format("mesh importer: '{}': {}", sourcePath.string(),
                                                   materialId.error()));
            }

            subMeshes.push_back(CookedSubMesh{
                .IndexOffset = indexOffset,
                .IndexCount = indexCount,
                .MaterialId = *materialId,
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

        // Sockets: every mesh-free, non-joint, non-camera node below the scene root, recorded by
        // name with its root-space transform. The translation takes import.scale and the whole
        // transform takes import.orientation exactly as the vertices do, so a socket lands in the
        // same space as the geometry — a socket rotated without the geometry (or the reverse) would
        // silently place whatever attaches there in mid-air.
        vector<SocketNode> socketNodes;
        CollectSockets(scene->mRootNode, aiMatrix4x4(), ExcludedNodeNames(scene), socketNodes);
        std::ranges::sort(socketNodes,
                          [](const SocketNode& a, const SocketNode& b) { return a.Name < b.Name; });

        vector<CookedMeshSocket> sockets;
        sockets.reserve(socketNodes.size());
        for (const SocketNode& node : socketNodes)
        {
            aiVector3D position;
            aiVector3D socketScale;
            aiQuaternion rotation;
            node.Transform.Decompose(socketScale, rotation, position);

            // A rotation composed onto the socket's own leaves its scale alone: reorienting
            // T * R * S rotates the translation and the rotation and touches neither the axes S
            // measures along nor their lengths.
            const vec3 place = orientation.Reorient(ToVec3(position) * scale);
            const quat facing =
                orientation.Reorient(quat(rotation.w, rotation.x, rotation.y, rotation.z));

            CookedMeshSocket socket{};
            SetSocketName(socket.Name, node.Name);
            socket.Position[0] = place.x;
            socket.Position[1] = place.y;
            socket.Position[2] = place.z;
            socket.Rotation[0] = facing.x;
            socket.Rotation[1] = facing.y;
            socket.Rotation[2] = facing.z;
            socket.Rotation[3] = facing.w;
            socket.Scale[0] = socketScale.x;
            socket.Scale[1] = socketScale.y;
            socket.Scale[2] = socketScale.z;

            // FindSocket resolves a name to one place, so two sockets sharing a name (or sharing
            // a truncated name) have no answer — a located cook error, not a silent pick.
            if (!sockets.empty() && std::strcmp(sockets.back().Name, socket.Name) == 0)
            {
                return std::unexpected(
                    fmt::format("mesh importer: '{}': '{}' declares two sockets named '{}'",
                                sourcePath.string(), modelPath.string(), socket.Name));
            }
            sockets.push_back(socket);
        }

        const usize vertexCount = skinned ? skinnedVertices.size() : staticVertices.size();
        const usize vertexStride = skinned ? sizeof(SkinnedVertex) : sizeof(CanonicalVertex);
        const usize vertexBytes = vertexCount * vertexStride;
        const void* vertexSource = skinned ? static_cast<const void*>(skinnedVertices.data())
                                           : static_cast<const void*>(staticVertices.data());
        const std::span<const CookedVertexAttribute> attributes =
            skinned ? std::span<const CookedVertexAttribute>(skinnedAttributes)
                    : std::span<const CookedVertexAttribute>(staticAttributes);

        CookedMeshHeader header{};
        header.Version = CookedMeshVersion;
        header.VertexStride = static_cast<u32>(vertexStride);
        header.VertexCount = static_cast<u32>(vertexCount);
        header.IndexCount = static_cast<u32>(indices.size());
        header.IndexType = IndexTypeU32;
        header.SubMeshCount = static_cast<u32>(subMeshes.size());
        header.AttributeCount = static_cast<u32>(attributes.size());
        header.SocketCount = static_cast<u32>(sockets.size());
        header.SkeletonId = skeletonId;

        const usize attributeBytes = attributes.size() * sizeof(CookedVertexAttribute);
        const usize subMeshBytes = subMeshes.size() * sizeof(CookedSubMesh);
        const usize socketBytes = sockets.size() * sizeof(CookedMeshSocket);
        const usize indexBytes = indices.size() * sizeof(u32);

        vector<u8> blob(sizeof(CookedMeshHeader) + attributeBytes + subMeshBytes + socketBytes +
                        vertexBytes + indexBytes);
        usize cursor = 0;
        std::memcpy(blob.data() + cursor, &header, sizeof(header));
        cursor += sizeof(header);
        std::memcpy(blob.data() + cursor, attributes.data(), attributeBytes);
        cursor += attributeBytes;
        std::memcpy(blob.data() + cursor, subMeshes.data(), subMeshBytes);
        cursor += subMeshBytes;
        if (socketBytes > 0)
        {
            std::memcpy(blob.data() + cursor, sockets.data(), socketBytes);
            cursor += socketBytes;
        }
        std::memcpy(blob.data() + cursor, vertexSource, vertexBytes);
        cursor += vertexBytes;
        std::memcpy(blob.data() + cursor, indices.data(), indexBytes);

        return blob;
    }
}
