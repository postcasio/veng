// The single-header stb_vorbis implementation, compiled once for the whole library. miniaudio has
// no built-in Vorbis decoder, so this provides the incremental Ogg Vorbis decode the stream-mode
// AudioClip path needs. The decoder is driven from memory, never from a file, so the stdio API is
// omitted; the header is a private include of libveng, so no public header sees a stb_vorbis type.
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"
