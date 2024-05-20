#ifndef _MATERIAL_CACHE_H_
#define _MATERIAL_CACHE_H_

#include "gfxcommon.h"
#include "material.h"

#include <list>
#include <unordered_map>

class MaterialCache
{
public:
    MaterialCache(size_t maxCacheSize);
    ~MaterialCache();

    std::shared_ptr<Material> getMaterial(
        MaterialDefinition materialDefinition);

private:
    void evict();

    size_t maxCacheSize;
    std::list<std::shared_ptr<Material>> cacheList;
};

#endif