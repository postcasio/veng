#include "texture_cache.h"

TextureCache::TextureCache(size_t maxCacheSize) : maxCacheSize(maxCacheSize) {}

TextureCache::~TextureCache()
{
    cacheList.clear();
    cacheMap.clear();
}

std::shared_ptr<Texture> TextureCache::getTexture(const std::string &path, VkFormat format)
{
    auto mapIt = cacheMap.find(path);
    if (mapIt != cacheMap.end())
    {
        // Move the texture to the front of the cache
        cacheList.splice(cacheList.begin(), cacheList, mapIt->second);
        return mapIt->second->texture;
    }
    else
    {
        // Load the texture
        std::shared_ptr<Texture> texture = std::make_shared<Texture>(path, format);
        // Add the texture to the cache
        cacheList.push_front({path, texture});
        cacheMap[path] = cacheList.begin();
        // Evict textures if the cache is over capacity
        while (cacheList.size() > maxCacheSize)
        {
            evict();
        }
        return texture;
    }
}

void TextureCache::evict()
{
    if (!cacheList.empty())
    {
        // Remove the least recently used texture from the cache
        auto listIt = cacheList.end();
        --listIt;
        cacheMap.erase(listIt->path);
        cacheList.erase(listIt);
    }
}