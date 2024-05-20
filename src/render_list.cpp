#include "render_list.h"
#include "engine.h"

RenderList::RenderList()
{
    // Constructor implementation
    // Initialize the opaque vector if necessary
}

RenderList::~RenderList()
{
    // Destructor implementation
    // Clean up resources if necessary
}

void RenderList::clear()
{
    opaque.clear();
    pointLights.clear();
    materials.clear();
}

void RenderList::addOpaque(Model *model, Mesh *mesh, Object *object, uint32_t materialIndex)
{
    // Create a new RenderItem
    RenderItem item;
    item.model = model;
    item.mesh = mesh;
    item.object = object;
    item.materialIndex = materialIndex;

    // Add the item to the opaque vector
    opaque.push_back(item);
}

void RenderList::addPointLight(PointLight *pointLight)
{
    pointLights.push_back(pointLight);
}

void RenderList::addMaterial(Material *material)
{
    if (std::find(materials.begin(), materials.end(), material) == materials.end())
    {
        materials.push_back(material);
    }
}