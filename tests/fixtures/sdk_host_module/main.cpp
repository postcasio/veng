// Exits 0 when the host build of scale_host.slang returns what the Slang source computes.
#include <ScaleHost.h>

int main()
{
    const glm::vec3 scaled = ScaleHost::ScaleHalf(glm::vec3(2.0f, -4.0f, 8.0f));
    return scaled == glm::vec3(1.0f, -2.0f, 4.0f) ? 0 : 1;
}
