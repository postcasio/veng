#include "material_cache.h"

MaterialCache::MaterialCache(size_t maxCacheSize) : maxCacheSize(maxCacheSize) {}

MaterialCache::~MaterialCache()
{
    cacheList.clear();
}

std::shared_ptr<Material> MaterialCache::getMaterial(
    MaterialDefinition materialDefinition)
{
    // Check if the material is already in the cache
    for (auto it = cacheList.begin(); it != cacheList.end(); ++it)
    {
        if ((*it)->matchesDefinition(materialDefinition))
        {
            // Move the material to the front of the cache list
            cacheList.splice(cacheList.begin(), cacheList, it);
            return *it;
        }
    }

    std::cout << "Creating new material dif map" << materialDefinition.diffuseMapPath << std::endl;

    // Create a new material
    auto material = std::make_shared<Material>(materialDefinition);

    // Add the new material to the cache
    cacheList.push_front(material);

    // Evict the least recently used material if the cache is full
    if (cacheList.size() > maxCacheSize)
    {
        evict();
    }

    return material;
}

void MaterialCache::evict()
{
    if (!cacheList.empty())
    {
        // Remove the least recently used material from the cache
        auto listIt = cacheList.end();
        --listIt;
        cacheList.erase(listIt);
    }
}