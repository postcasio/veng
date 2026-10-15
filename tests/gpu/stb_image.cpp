// The gpu suite's own stb_image, which golden_image.h declares. libveng_cook compiles a copy too,
// but keeps it hidden: stb is the cooker's implementation detail, not part of its API.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
