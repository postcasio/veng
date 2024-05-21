#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <filesystem>
#include <vector>
#include <iostream>
#include <regex>
#include "gfxcommon.h"

#include "model.h"
#include "texture.h"
#include "mesh.h"
#include "engine.h"

std::vector<std::shared_ptr<Mesh>> Model::getMeshes() { return meshes; }

Model::Model(std::vector<std::shared_ptr<Mesh>> meshes, std::vector<std::shared_ptr<Material>> materials)
{
    this->meshes = meshes;
    this->materials = materials;

    createDescriptorSet();
}

std::shared_ptr<Model> Model::fromPath(std::filesystem::path const &path)
{
    Assimp::Importer importer;

    const aiScene *scene = importer.ReadFile(path.string(), aiProcess_GenNormals | aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
    {
        throw std::runtime_error(std::string("Error loading model: ") + importer.GetErrorString());
    }

    std::vector<std::shared_ptr<Mesh>> meshes;

    processNode(scene->mRootNode, scene, meshes, path.parent_path());
    std::cout << " Loaded " << meshes.size() << " meshes" << std::endl;
    std::vector<std::shared_ptr<Material>> materials;
    std::cout << " Loading " << scene->mNumMaterials << " materials" << std::endl;
    materials.resize(scene->mNumMaterials);

    for (int i = 0; i < scene->mNumMaterials; i++)
    {
        auto aiMaterial = scene->mMaterials[i];

        MaterialDefinition definition;

        std::string diffuseMapPath = loadMaterialTexture(aiMaterial, aiTextureType_DIFFUSE, "texture_diffuse", path.parent_path());

        if (diffuseMapPath.empty() || !std::filesystem::exists(diffuseMapPath))
        {
            definition.diffuseMapPath = "textures/null.png";
            definition.displacementMapPath = "textures/null.png";
            definition.normalMapPath = "textures/null.png";
            definition.occlusionMapPath = "textures/null.png";
        }
        else
        {
            definition.diffuseMapPath = diffuseMapPath;

            std::filesystem::path normalMapPath(std::regex_replace(diffuseMapPath, std::regex("_Color"), "_NormalGL"));

            if (!std::filesystem::exists(normalMapPath))
            {
                definition.normalMapPath = "textures/null.png";
            }
            else
            {
                definition.normalMapPath = normalMapPath;
            }

            std::filesystem::path displacementMapPath(std::regex_replace(diffuseMapPath, std::regex("_Color"), "_Displacement"));

            if (!std::filesystem::exists(displacementMapPath))
            {
                definition.displacementMapPath = "textures/null.png";
            }
            else
            {
                definition.displacementMapPath = displacementMapPath;
            }

            std::filesystem::path occlusionMapPath(std::regex_replace(diffuseMapPath, std::regex("_Color"), "_AmbientOcclusion"));

            if (!std::filesystem::exists(occlusionMapPath))
            {
                definition.occlusionMapPath = "textures/null.png";
            }
            else
            {
                definition.occlusionMapPath = occlusionMapPath;
            }
        }

        materials[i] = engine()->materialCache->getMaterial(definition);
    }

    return std::make_shared<Model>(meshes, materials);
}

void processNode(aiNode *node, const aiScene *scene, std::vector<std::shared_ptr<Mesh>> &meshes, std::filesystem::path directory)
{
    // process all the node's meshes (if any)
    for (unsigned int i = 0; i < node->mNumMeshes; i++)
    {
        aiMesh *mesh = scene->mMeshes[node->mMeshes[i]];
        meshes.push_back(processMesh(mesh, scene, directory));
    }
    // then do the same for each of its children
    for (unsigned int i = 0; i < node->mNumChildren; i++)
    {
        processNode(node->mChildren[i], scene, meshes, directory);
    }
}

std::shared_ptr<Mesh> processMesh(aiMesh *mesh, const aiScene *scene, std::filesystem::path directory)
{
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<Texture> textures;

    for (unsigned int i = 0; i < mesh->mNumVertices; i++)
    {
        Vertex vertex;
        glm::vec3 vector;

        vector.x = mesh->mVertices[i].x;
        vector.y = mesh->mVertices[i].y;
        vector.z = mesh->mVertices[i].z;
        vertex.pos = vector;

        vector.x = mesh->mNormals[i].x;
        vector.y = mesh->mNormals[i].y;
        vector.z = mesh->mNormals[i].z;
        vertex.normal = vector;

        vector.x = mesh->mTangents[i].x;
        vector.y = mesh->mTangents[i].y;
        vector.z = mesh->mTangents[i].z;
        vertex.tangent = vector;

        if (mesh->mTextureCoords[0]) // does the mesh contain texture coordinates?
        {
            glm::vec2 vec;
            vec.x = mesh->mTextureCoords[0][i].x;
            vec.y = -mesh->mTextureCoords[0][i].y;
            vertex.texCoord = vec;
        }
        else
            vertex.texCoord = glm::vec2(0.0f, 0.0f);

        vertices.push_back(vertex);
    }

    for (unsigned int i = 0; i < mesh->mNumFaces; i++)
    {
        aiFace face = mesh->mFaces[i];
        for (unsigned int j = 0; j < face.mNumIndices; j++)
            indices.push_back(face.mIndices[j]);
    }

    // if (mesh->mMaterialIndex >= 0)
    // {
    //     aiMaterial *material = scene->mMaterials[mesh->mMaterialIndex];
    //     std::vector<Texture> diffuseMaps = loadMaterialTextures(material,
    //                                                             aiTextureType_DIFFUSE, "texture_diffuse", directory);
    //     textures.insert(textures.end(), diffuseMaps.begin(), diffuseMaps.end());
    //     // std::vector<Texture> specularMaps = loadMaterialTextures(material,
    //     //                                                          aiTextureType_SPECULAR, "texture_specular", directory);
    //     // textures.insert(textures.end(), specularMaps.begin(), specularMaps.end());
    // }

    return std::make_shared<Mesh>(vertices, indices, mesh->mMaterialIndex);
}

void printMaterialTextures(aiMaterial *mat)
{
    // Iterate over all possible texture types
    for (int type = aiTextureType_NONE; type != aiTextureType_UNKNOWN; ++type)
    {
        aiTextureType textureType = static_cast<aiTextureType>(type);

        // Iterate over all textures of this type
        for (unsigned int i = 0; i < mat->GetTextureCount(textureType); i++)
        {
            aiString str;
            mat->GetTexture(textureType, i, &str);

            std::cout << "Texture type: " << type
                      << ", path: " << str.C_Str() << std::endl;
        }
    }
}

std::filesystem::path loadMaterialTexture(aiMaterial *mat, aiTextureType type, std::string typeName, std::filesystem::path directory)
{

    // printMaterialTextures(mat);

    std::cout << "Looking for a diffuse texture among " << mat->GetTextureCount(type) << " textures" << std::endl;

    for (unsigned int i = 0; i < mat->GetTextureCount(type); i++)
    {
        aiString str;
        mat->GetTexture(type, i, &str);

        std::filesystem::path path = directory / str.C_Str();

        return path;
    }

    std::cout << "No texture found, using fallback" << std::endl;

    return "";
}

void Model::draw(CommandBuffer &commandBuffer)
{
    for (auto mesh : meshes)
    {
        mesh->draw(commandBuffer);
    }
}

void Model::updateDescriptorSets()
{
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        VkDescriptorBufferInfo bufferInfoTransform{};
        bufferInfoTransform.buffer = uniforms.uniformBufferAllocations[i]->buffer;
        bufferInfoTransform.offset = 0;
        bufferInfoTransform.range = sizeof(TransformUniformBufferObject);

        std::array<VkWriteDescriptorSet, 1> descriptorWrites{};

        descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[0].dstSet = descriptorSet->sets[i];
        descriptorWrites[0].dstBinding = 0;
        descriptorWrites[0].dstArrayElement = 0;
        descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrites[0].descriptorCount = 1;
        descriptorWrites[0].pBufferInfo = &bufferInfoTransform;

        vkUpdateDescriptorSets(renderer()->device->device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
    }
}

void Model::createDescriptorSet()
{
    descriptorSet = renderer()->descriptorPool->createDescriptorSet(*renderer()->matricesDescriptorSetLayout);
    updateDescriptorSets();
}
