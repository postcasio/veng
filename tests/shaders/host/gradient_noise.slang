// Gradient noise with an analytic gradient, and a fractal sum of it, written once over the
// floating-point type. GPU shaders include this file directly; the host exports file
// (gradient_noise_host.slang) instantiates it in float and in double for the CPU.
//
// The lattice hash runs in uint only: unsigned overflow wraps identically on both targets, where
// signed overflow is undefined behaviour in the emitted C++.

/// @brief A floating-point type that converts to a lattice integer.
///
/// A generic floating-point type offers no conversion to int that every target compiles, so the
/// conversion is spelled through this interface, which float and double are extended with.
interface IGradientNoiseReal
{
    /// @brief This value converted to int, truncating toward zero.
    int ToLatticeInt();
}

extension float : IGradientNoiseReal
{
    int ToLatticeInt()
    {
        return int(this);
    }
}

extension double : IGradientNoiseReal
{
    int ToLatticeInt()
    {
        return int(this);
    }
}

/// @brief Hashes a lattice cell to 32 well-mixed bits (a 3D integer avalanche, uint only).
uint GradientNoiseHash(uint3 cell)
{
    uint h = cell.x * 0x8DA6B343u;
    h ^= cell.y * 0xD8163841u;
    h ^= cell.z * 0xCB1AB31Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

/// @brief One of the twelve cube-edge gradients, chosen by the hash's low bits.
vector<T, 3> GradientNoiseDirection<T : __BuiltinFloatingPointType>(uint hash)
{
    const uint index = hash % 12u;
    const T one = T(1.0);
    const T zero = T(0.0);
    const T a = (index & 1u) != 0u ? -one : one;
    const T b = (index & 2u) != 0u ? -one : one;
    if (index < 4u)
    {
        return vector<T, 3>(a, b, zero);
    }
    if (index < 8u)
    {
        return vector<T, 3>(a, zero, b);
    }
    return vector<T, 3>(zero, a, b);
}

/// @brief 3D gradient noise at p: value in .x, analytic gradient in .yzw.
///
/// The value is zero at every lattice point and lies in roughly [-1, 1].
vector<T, 4> GradientNoise<T : __BuiltinFloatingPointType & IGradientNoiseReal>(vector<T, 3> p)
{
    const vector<T, 3> cellFloor = floor(p);
    const vector<T, 3> f = p - cellFloor;
    // In range by the caller's contract (|p| well below 2^31), so the conversion is defined on
    // both targets; the cast to uint then wraps a negative cell identically.
    const int3 cellSigned =
        int3(cellFloor.x.ToLatticeInt(), cellFloor.y.ToLatticeInt(), cellFloor.z.ToLatticeInt());
    const uint3 cell = uint3(cellSigned);

    const T six = T(6.0);
    const T fifteen = T(15.0);
    const T ten = T(10.0);
    const T thirty = T(30.0);
    const T sixty = T(60.0);
    // Quintic fade and its derivative.
    const vector<T, 3> u = f * f * f * (f * (f * six - fifteen) + ten);
    const vector<T, 3> du = f * f * (f * (f * thirty - sixty) + thirty);

    const vector<T, 3> g000 = GradientNoiseDirection<T>(GradientNoiseHash(cell + uint3(0u, 0u, 0u)));
    const vector<T, 3> g100 = GradientNoiseDirection<T>(GradientNoiseHash(cell + uint3(1u, 0u, 0u)));
    const vector<T, 3> g010 = GradientNoiseDirection<T>(GradientNoiseHash(cell + uint3(0u, 1u, 0u)));
    const vector<T, 3> g110 = GradientNoiseDirection<T>(GradientNoiseHash(cell + uint3(1u, 1u, 0u)));
    const vector<T, 3> g001 = GradientNoiseDirection<T>(GradientNoiseHash(cell + uint3(0u, 0u, 1u)));
    const vector<T, 3> g101 = GradientNoiseDirection<T>(GradientNoiseHash(cell + uint3(1u, 0u, 1u)));
    const vector<T, 3> g011 = GradientNoiseDirection<T>(GradientNoiseHash(cell + uint3(0u, 1u, 1u)));
    const vector<T, 3> g111 = GradientNoiseDirection<T>(GradientNoiseHash(cell + uint3(1u, 1u, 1u)));

    const T one = T(1.0);
    const T v000 = dot(g000, f);
    const T v100 = dot(g100, f - vector<T, 3>(one, T(0.0), T(0.0)));
    const T v010 = dot(g010, f - vector<T, 3>(T(0.0), one, T(0.0)));
    const T v110 = dot(g110, f - vector<T, 3>(one, one, T(0.0)));
    const T v001 = dot(g001, f - vector<T, 3>(T(0.0), T(0.0), one));
    const T v101 = dot(g101, f - vector<T, 3>(one, T(0.0), one));
    const T v011 = dot(g011, f - vector<T, 3>(T(0.0), one, one));
    const T v111 = dot(g111, f - vector<T, 3>(one, one, one));

    // Trilinear blend written as a polynomial in u, so the gradient follows term by term.
    const T k0 = v000;
    const T k1 = v100 - v000;
    const T k2 = v010 - v000;
    const T k3 = v001 - v000;
    const T k4 = v000 - v100 - v010 + v110;
    const T k5 = v000 - v010 - v001 + v011;
    const T k6 = v000 - v100 - v001 + v101;
    const T k7 = -v000 + v100 + v010 - v110 + v001 - v101 - v011 + v111;

    const vector<T, 3> gk0 = g000;
    const vector<T, 3> gk1 = g100 - g000;
    const vector<T, 3> gk2 = g010 - g000;
    const vector<T, 3> gk3 = g001 - g000;
    const vector<T, 3> gk4 = g000 - g100 - g010 + g110;
    const vector<T, 3> gk5 = g000 - g010 - g001 + g011;
    const vector<T, 3> gk6 = g000 - g100 - g001 + g101;
    const vector<T, 3> gk7 = -g000 + g100 + g010 - g110 + g001 - g101 - g011 + g111;

    const T value = k0 + k1 * u.x + k2 * u.y + k3 * u.z + k4 * u.x * u.y + k5 * u.y * u.z +
                    k6 * u.z * u.x + k7 * u.x * u.y * u.z;

    const vector<T, 3> gradient =
        gk0 + gk1 * u.x + gk2 * u.y + gk3 * u.z + gk4 * u.x * u.y + gk5 * u.y * u.z +
        gk6 * u.z * u.x + gk7 * u.x * u.y * u.z +
        du * vector<T, 3>(k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z,
                          k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x,
                          k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y);

    return vector<T, 4>(value, gradient.x, gradient.y, gradient.z);
}

/// @brief Maximum octave count GradientNoiseFbm sums.
static const uint GradientNoiseMaxOctaves = 16u;

/// @brief A fractal sum of gradient noise: value in .x, analytic gradient in .yzw.
///
/// Each octave doubles the frequency and halves the amplitude, so the gradient of octave k scales
/// by 2^k / 2^k = 1 and the summed gradient grows with the octave count.
vector<T, 4> GradientNoiseFbm<T : __BuiltinFloatingPointType & IGradientNoiseReal>(vector<T, 3> p,
                                                                          uint octaves)
{
    vector<T, 4> sum = vector<T, 4>(T(0.0));
    T amplitude = T(1.0);
    T frequency = T(1.0);
    const uint count = min(octaves, GradientNoiseMaxOctaves);
    for (uint octave = 0u; octave < count; ++octave)
    {
        const vector<T, 4> n = GradientNoise<T>(p * frequency);
        sum.x += amplitude * n.x;
        sum.yzw += (amplitude * frequency) * n.yzw;
        amplitude *= T(0.5);
        frequency *= T(2.0);
    }
    return sum;
}
