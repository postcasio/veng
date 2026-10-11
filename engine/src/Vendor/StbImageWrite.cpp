// A thin aggregation TU that compiles stb_image_write's implementation once into libveng, behind
// Veng/Capture/Screenshot.h's PNG encode. Kept off clang-tidy by the sibling .clang-tidy.

#define STB_IMAGE_WRITE_IMPLEMENTATION

// stb_image_write asserts through <assert.h> by default; leaving it means a malformed encode aborts
// rather than throwing, which is what -fno-exceptions wants anyway.
#include <stb_image_write.h>
