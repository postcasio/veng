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

void RenderList::build(Object *object, Camera &camera)
{
    if (object->type == ObjectType::Object && object->hasModel() && object->visible)
    {
        auto model = object->getModel();

        for (auto material : model->materials)
        {
            addMaterial(material.get());
        }

        for (auto mesh : model->getMeshes())
        {
            if (model->materials[mesh->materialIndex]->visible)
            {
                addOpaque(model.get(), mesh.get(), object, mesh->materialIndex);
            }
        }
    }
    else if (object->type == ObjectType::PointLight)
    {
        addPointLight((PointLight *)object);
    }

    for (auto child : object->children)
    {
        build(child.get(), camera);
    }
}