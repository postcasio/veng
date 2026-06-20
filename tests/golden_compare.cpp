// golden_compare — fuzzy image equality for the golden smoke test.
//
//   veng_golden_compare <actual> <golden> [maxChannelDelta] [maxMismatchFraction]
//
// Loads both images (PPM or PNG — stb_image decodes either) as 8-bit RGB and
// reports them equal when no more than maxMismatchFraction of pixels differ by
// more than maxChannelDelta on any channel. The tolerance absorbs the small
// float jitter a GPU/driver can introduce between runs while still catching a
// real render regression. Exits 0 on match, 1 on mismatch or load failure.

#include <cstdio>
#include <cstdlib>
#include <cstdint>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace
{
    struct Image
    {
        int Width = 0;
        int Height = 0;
        std::uint8_t* Pixels = nullptr; // RGB, row-major, owned by stb.
    };

    bool Load(const char* path, Image& out)
    {
        int channels = 0;
        out.Pixels = stbi_load(path, &out.Width, &out.Height, &channels, 3);
        if (!out.Pixels)
        {
            std::fprintf(stderr, "golden_compare: failed to load '%s': %s\n", path,
                         stbi_failure_reason());
            return false;
        }
        return true;
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr,
                     "usage: %s <actual> <golden> [maxChannelDelta] [maxMismatchFraction]\n",
                     argv[0]);
        return 1;
    }

    const int maxChannelDelta = argc > 3 ? std::atoi(argv[3]) : 8;
    const double maxMismatchFraction = argc > 4 ? std::atof(argv[4]) : 0.005;

    Image actual;
    Image golden;
    if (!Load(argv[1], actual) || !Load(argv[2], golden))
    {
        return 1;
    }

    if (actual.Width != golden.Width || actual.Height != golden.Height)
    {
        std::fprintf(stderr, "golden_compare: dimension mismatch: actual %dx%d, golden %dx%d\n",
                     actual.Width, actual.Height, golden.Width, golden.Height);
        return 1;
    }

    const long pixelCount = static_cast<long>(actual.Width) * actual.Height;
    long mismatched = 0;
    int worstDelta = 0;

    for (long i = 0; i < pixelCount; i++)
    {
        int pixelDelta = 0;
        for (int channel = 0; channel < 3; channel++)
        {
            const int a = actual.Pixels[i * 3 + channel];
            const int g = golden.Pixels[i * 3 + channel];
            const int d = a > g ? a - g : g - a;
            if (d > pixelDelta)
            {
                pixelDelta = d;
            }
        }
        if (pixelDelta > worstDelta)
        {
            worstDelta = pixelDelta;
        }
        if (pixelDelta > maxChannelDelta)
        {
            mismatched++;
        }
    }

    const double fraction = static_cast<double>(mismatched) / static_cast<double>(pixelCount);

    std::printf("golden_compare: %ld/%ld pixels exceed delta %d (%.4f%%, limit %.4f%%); worst "
                "channel delta %d\n",
                mismatched, pixelCount, maxChannelDelta, fraction * 100.0,
                maxMismatchFraction * 100.0, worstDelta);

    if (fraction > maxMismatchFraction)
    {
        std::fprintf(stderr,
                     "golden_compare: FAIL — capture differs from golden beyond tolerance\n");
        return 1;
    }

    std::printf("golden_compare: OK — capture matches golden within tolerance\n");
    return 0;
}
