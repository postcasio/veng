// template_capture_check — size + non-blank guard for the template smoke capture.
//
//   template_capture_check <capture> <expectedWidth> <expectedHeight>
//
// Loads the capture (PPM or PNG — stb_image decodes either) as 8-bit RGB, asserts it
// matches the expected dimensions, and asserts at least some pixels differ from the
// first pixel (the clear colour). The template carries no pixel golden — this cheap
// check is the standing guard that the launcher rendered something, so a black-screen
// regression (a material/shader that failed to resolve) fails the test rather than
// passing on size alone. Exits 0 on pass, 1 on load failure, dimension mismatch, or a
// uniform (blank) image.

#include <cstdio>
#include <cstdlib>
#include <cstdint>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <capture> <expectedWidth> <expectedHeight>\n", argv[0]);
        return 1;
    }

    const int expectedWidth = std::atoi(argv[2]);
    const int expectedHeight = std::atoi(argv[3]);

    int width = 0;
    int height = 0;
    int channels = 0;
    std::uint8_t* pixels = stbi_load(argv[1], &width, &height, &channels, 3);
    if (!pixels)
    {
        std::fprintf(stderr, "template_capture_check: failed to load '%s': %s\n", argv[1],
                     stbi_failure_reason());
        return 1;
    }

    if (width != expectedWidth || height != expectedHeight)
    {
        std::fprintf(stderr,
                     "template_capture_check: dimension mismatch: got %dx%d, expected %dx%d\n",
                     width, height, expectedWidth, expectedHeight);
        stbi_image_free(pixels);
        return 1;
    }

    // Non-blank: at least one pixel differs from the first by more than a small threshold,
    // so a uniform clear-colour frame (nothing drawn) fails rather than passes on size.
    const long pixelCount = static_cast<long>(width) * height;
    bool varies = false;
    for (long i = 1; i < pixelCount && !varies; i++)
    {
        for (int channel = 0; channel < 3; channel++)
        {
            const int first = pixels[channel];
            const int here = pixels[i * 3 + channel];
            const int delta = here > first ? here - first : first - here;
            if (delta > 4)
            {
                varies = true;
                break;
            }
        }
    }

    stbi_image_free(pixels);

    if (!varies)
    {
        std::fprintf(stderr,
                     "template_capture_check: FAIL — capture is uniform (blank), nothing drawn\n");
        return 1;
    }

    std::printf("template_capture_check: OK — %dx%d capture is non-blank\n", width, height);
    return 0;
}
