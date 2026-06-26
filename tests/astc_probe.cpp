// ASTC capability probe for the codec-dependent golden smoke test.
//
// Exits 0 when a present physical device reports textureCompressionASTC_LDR,
// and 77 (the skip code) when no usable driver is present or none of the
// devices support the codec. smoke_golden runs this before asserting the
// golden, because the migrated hello-triangle scene cooks ASTC textures: a
// device without the codec samples the untextured fallback, so the golden is
// only reproducible where ASTC is available.

#include <support/GpuProbe.h>

int main()
{
    return Veng::Test::HasAstcSupport() ? 0 : 77;
}
