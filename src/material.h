#ifndef _MATERIAL_H_
#define _MATERIAL_H_

#include "gfxcommon.h"
#include "texture.h"
#include "gpu/descriptor_set.h"

struct MaterialDefinition
{
    std::string diffuseMapPath;
    std::string normalMapPath;
    std::string displacementMapPath;
    std::string occlusionMapPath;
    std::string roughnessMapPath;
};

class Material
{
public:
    Material();
    Material(MaterialDefinition materialDefinition);
    ~Material();

    bool matchesDefinition(MaterialDefinition materialDefinition);
    void updateDescriptorSet();
    bool visible = true;
    std::shared_ptr<Texture> diffuseMap;
    std::shared_ptr<Texture> normalMap;
    std::shared_ptr<Texture> displacementMap;
    std::shared_ptr<Texture> occlusionMap;
    std::shared_ptr<Texture> roughnessMap;

    std::unique_ptr<DescriptorSet> descriptorSet;

    MaterialDefinition materialDefinition;

private:
};

#endif