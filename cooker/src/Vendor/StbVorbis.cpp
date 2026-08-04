// The single-header stb_vorbis implementation, compiled once for the cooker. It decodes an Ogg
// Vorbis source to PCM for a sample-mode AudioClip and probes a stream-mode source's shape. The
// decode is driven from an in-memory buffer, so the stdio API is omitted. stb_vorbis also ships in
// libveng (compiled separately there) for the runtime stream-decode path.
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"
