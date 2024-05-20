#ifndef _RENDER_LIST_H_
#define _RENDER_LIST_H_

#include "gfxcommon.h"

#include <vector>

#include "model.h"
#include "mesh.h"
#include "object.h"
#include "point_light.h"
#include "material.h"

struct RenderItem
{
    Model *model;
    Mesh *mesh;
    Object *object;
    uint32_t materialIndex;
};

class RenderList
{
public:
    RenderList();
    ~RenderList();

    std::vector<RenderItem> opaque;
    std::vector<PointLight *> pointLights;
    std::vector<Material *> materials;

    void clear();
    void addOpaque(Model *model, Mesh *mesh, Object *object, uint32_t materialIndex);
    void addPointLight(PointLight *pointLight);
    void addMaterial(Material *material);
};

#endif