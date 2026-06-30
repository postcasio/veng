// Order-2 real spherical-harmonics evaluation, the GPU counterpart of the CPU
// SphericalHarmonics.h. The basis polynomials and band order here are byte-for-byte the
// CPU ShBasis: index 0 the constant band l=0, indices 1-3 the linear band l=1 (y, z, x),
// indices 4-8 the quadratic band l=2. A consumer reconstructing irradiance from a
// cosine-convolved coefficient set must match the CPU exactly, since the coefficients are
// projected and convolved on the CPU and only evaluated here — the CPU↔GPU consistency
// test pins this equality.

// Evaluates the nine order-2 real-SH basis functions in a direction. Direction is assumed
// unit length; a non-normalized direction scales the higher bands and is caller error.
void ShBasis(float3 direction, out float basis[9])
{
    const float x = direction.x;
    const float y = direction.y;
    const float z = direction.z;

    basis[0] = 0.282095;                       // l=0
    basis[1] = 0.488603 * y;                    // l=1, m=-1
    basis[2] = 0.488603 * z;                    // l=1, m=0
    basis[3] = 0.488603 * x;                    // l=1, m=+1
    basis[4] = 1.092548 * x * y;                // l=2, m=-2
    basis[5] = 1.092548 * y * z;                // l=2, m=-1
    basis[6] = 0.315392 * (3.0 * z * z - 1.0);  // l=2, m=0
    basis[7] = 1.092548 * x * z;                // l=2, m=+1
    basis[8] = 0.546274 * (x * x - y * y);      // l=2, m=+2
}

// Reconstructs irradiance from a cosine-convolved SH set in a normal direction: the
// nine-term SH dot product sum(coefficients[i] * ShBasis(normal)[i]). The coefficients
// must already be cosine-convolved (ConvolveCosine on the CPU); evaluating a raw radiance
// set reconstructs radiance, not irradiance. The .xyz of each vec4 carries one RGB
// coefficient (the .w is std140 padding and ignored). May be negative for a set whose
// higher bands ring below zero; the caller clamps to non-negative where required.
float3 ShEvalIrradiance(float4 coefficients[9], float3 normal)
{
    float basis[9];
    ShBasis(normal, basis);

    float3 result = float3(0.0);
    for (uint i = 0; i < 9; ++i)
    {
        result += coefficients[i].xyz * basis[i];
    }
    return result;
}
