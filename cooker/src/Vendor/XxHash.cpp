// xxHash is single-header but not header-only by default (stb pattern):
// XXH_IMPLEMENTATION emits the definitions in exactly this one TU.
// XXH_STATIC_LINKING_ONLY exposes the full state-struct definitions the
// implementation needs (the public header only forward-declares them).
#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#include <xxhash.h>
