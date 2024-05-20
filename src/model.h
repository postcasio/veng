#ifndef _MODEL_H_
#define _MODEL_H_

#include "gfxcommon.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <vector>
#include <filesystem>

#include "mesh.h"
#include "material.h"
#include "gpu/descriptor_set.h"

void processNode(aiNode *node, const aiScene *scene, std::vector<std::shared_ptr<Mesh>> &meshes, std::filesystem::path directory);
std::shared_ptr<Mesh> processMesh(aiMesh *mesh, const aiScene *scene, std::filesystem::path directory);
std::filesystem::path loadMaterialTexture(aiMaterial *mat, aiTextureType type, std::string typeName, std::filesystem::path directory);

class Model
{
public:
    Model(std::vector<std::shared_ptr<Mesh>> meshes, std::vector<std::shared_ptr<Material>> materials);
    ~Model() = default;

    void updateDescriptorSets();

    DescriptorSet *descriptorSets;

    TransformUniforms uniforms;

    std::vector<std::shared_ptr<Mesh>> getMeshes();

    static std::shared_ptr<Model> fromPath(std::filesystem::path const &path);

    void draw(VkCommandBuffer commandBuffer);
    std::vector<std::shared_ptr<Material>> materials;

private:
    std::vector<std::shared_ptr<Mesh>> meshes;
    void createDescriptorSets();
};

#endif