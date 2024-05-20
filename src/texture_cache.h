#ifndef _TEXTURE_CACHE_H_
#define _TEXTURE_CACHE_H_

#include "gfxcommon.h"
#include "texture.h"

#include <list>
#include <unordered_map>
#include <memory>

class TextureCache
{
public:
    TextureCache(size_t maxCacheSize);
    ~TextureCache();

    std::shared_ptr<Texture> getTexture(const std::string &path);

private:
    struct Entry
    {
        std::string path;
        std::shared_ptr<Texture> texture;
    };

    std::list<Entry> cacheList; // MRU at front, LRU at back
    std::unordered_map<std::string, std::list<Entry>::iterator> cacheMap;
    size_t maxCacheSize;

    void evict();
};

#endif