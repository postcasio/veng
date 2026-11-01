// Copyright (c) 2015 Krzysztof Narkowicz
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// A BC6H (unsigned half float) block encoder, one thread per 4x4 block. Every block is encoded in
// the one-region mode at 10-bit endpoints (twice: on its bounding-box diagonal, and with each
// channel oriented by its covariance), then in the two-region modes (7.6 and 9.5 delta) on the
// best-fitting of the 32 partitions, keeping whichever has the lower squared log error. The
// two-region search keeps an isolated bright texel from dragging the line every other texel of its
// block is placed on. Texels beyond the source's edge repeat its last row and column, so a mip
// smaller than a block encodes as one partial block.
//
// The source is one mip of a float image as a layered view; the blocks of every layer are written
// layer-major, then row-major, into a buffer a copy moves into the compressed image.

[[vk::binding(0, 3)]] Texture2DArray<float4> g_Source;
[[vk::binding(1, 3)]] RWStructuredBuffer<uint4> g_Blocks;

struct PushConstants
{
    uint2 SourceSize;    // the source mip's extent in texels
    uint2 BlocksPerAxis; // the blocks along each axis, ceil(SourceSize / 4)
    uint OutputBase;     // the first block this dispatch writes
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

static const float HalfMax = 65504.0;

// The squared log error, weighted by luminance, between a source and a decoded texel. The offset
// inside the logarithm is a floor set from the block's brightest texel, so the error is relative
// and the same at any overall brightness: a sky encodes alike whatever scale it is authored at.
float CalcMsle(float3 a, float3 b, float errorFloor)
{
    float3 delta = log2((b + errorFloor) / (a + errorFloor));
    float3 deltaSq = delta * delta;
    deltaSq *= float3(0.299, 0.587, 0.114);
    return deltaSq.x + deltaSq.y + deltaSq.z;
}

uint PatternFixupId(uint i)
{
    uint ret = 15;
    ret = ((3441033216u >> i) & 0x1u) != 0 ? 2 : ret;
    ret = ((845414400u >> i) & 0x1u) != 0 ? 8 : ret;
    return ret;
}

uint Pattern(uint p, uint i)
{
    uint p2 = p / 2;
    uint p3 = p - p2 * 2;

    uint enc = 0;
    enc = p2 == 0 ? 2290666700u : enc;
    enc = p2 == 1 ? 3972591342u : enc;
    enc = p2 == 2 ? 4276930688u : enc;
    enc = p2 == 3 ? 3967876808u : enc;
    enc = p2 == 4 ? 4293707776u : enc;
    enc = p2 == 5 ? 3892379264u : enc;
    enc = p2 == 6 ? 4278255592u : enc;
    enc = p2 == 7 ? 4026597360u : enc;
    enc = p2 == 8 ? 9369360u : enc;
    enc = p2 == 9 ? 147747072u : enc;
    enc = p2 == 10 ? 1930428556u : enc;
    enc = p2 == 11 ? 2362323200u : enc;
    enc = p2 == 12 ? 823134348u : enc;
    enc = p2 == 13 ? 913073766u : enc;
    enc = p2 == 14 ? 267393000u : enc;
    enc = p2 == 15 ? 966553998u : enc;

    enc = p3 != 0 ? enc >> 16 : enc;
    return (enc >> i) & 0x1u;
}

float3 HalfBits(float3 x)
{
    return float3(f32tof16(x));
}

float3 Quantize7(float3 x)
{
    return (HalfBits(x) * 128.0) / (0x7bff + 1.0);
}

float3 Quantize9(float3 x)
{
    return (HalfBits(x) * 512.0) / (0x7bff + 1.0);
}

float3 Quantize10(float3 x)
{
    return (HalfBits(x) * 1024.0) / (0x7bff + 1.0);
}

float3 Unquantize7(float3 x)
{
    return (x * 65536.0 + 0x8000) / 128.0;
}

float3 Unquantize9(float3 x)
{
    return (x * 65536.0 + 0x8000) / 512.0;
}

float3 Unquantize10(float3 x)
{
    return (x * 65536.0 + 0x8000) / 1024.0;
}

// The half-float bits the decoder interpolates to between two unquantized endpoints.
float3 InterpolatedBits(float3 endpoint0Unq, float3 endpoint1Unq, float weight)
{
    return (endpoint0Unq * (64.0 - weight) + endpoint1Unq * weight + 32.0) * (31.0 / 4096.0);
}

float3 FinishUnquantize(float3 endpoint0Unq, float3 endpoint1Unq, float weight)
{
    return f16tof32(uint3(InterpolatedBits(endpoint0Unq, endpoint1Unq, weight)));
}

void Swap(inout float3 a, inout float3 b)
{
    float3 tmp = a;
    a = b;
    b = tmp;
}

void Swap(inout float a, inout float b)
{
    float tmp = a;
    a = b;
    b = tmp;
}

// A block's direction from its low to its high endpoint, normalized to a unit component sum. A
// block whose endpoints coincide has no direction; any one serves, since every texel then projects
// to the same position.
float3 BlockDirection(float3 low, float3 high)
{
    float3 dir = high - low;
    float sum = dir.x + dir.y + dir.z;
    return abs(sum) > 1e-20 ? dir / sum : float3(1.0 / 3.0);
}

// Where a texel lies between two endpoint positions, or at the first when they coincide.
float EndpointFraction(float texelPos, float endPoint0Pos, float endPoint1Pos)
{
    float span = endPoint1Pos - endPoint0Pos;
    return abs(span) > 0.0 ? (texelPos - endPoint0Pos) / span : 0.0;
}

uint ComputeIndex3(float texelPos, float endPoint0Pos, float endPoint1Pos)
{
    float r = EndpointFraction(texelPos, endPoint0Pos, endPoint1Pos);
    return uint(clamp(r * 6.98182 + 0.00909 + 0.5, 0.0, 7.0));
}

uint ComputeIndex4(float texelPos, float endPoint0Pos, float endPoint1Pos)
{
    float r = EndpointFraction(texelPos, endPoint0Pos, endPoint1Pos);
    return uint(clamp(r * 14.93333 + 0.03333 + 0.5, 0.0, 15.0));
}

void SignExtend(inout float3 v1, uint mask, uint signFlag)
{
    int3 v = int3(v1);
    v.x = (v.x & int(mask)) | (v.x < 0 ? int(signFlag) : 0);
    v.y = (v.y & int(mask)) | (v.y < 0 ? int(signFlag) : 0);
    v.z = (v.z & int(mask)) | (v.z < 0 ? int(signFlag) : 0);
    v1 = float3(v);
}

// Refines the endpoints by insetting the bounding box in log2 RGB space.
void InsetColorBBoxP1(float3 texels[16], inout float3 blockMin, inout float3 blockMax)
{
    float3 refinedBlockMin = blockMax;
    float3 refinedBlockMax = blockMin;

    for (uint i = 0; i < 16; ++i)
    {
        refinedBlockMin = min(refinedBlockMin, select(texels[i] == blockMin, refinedBlockMin, texels[i]));
        refinedBlockMax = max(refinedBlockMax, select(texels[i] == blockMax, refinedBlockMax, texels[i]));
    }

    float3 logRefinedBlockMax = log2(refinedBlockMax + 1.0);
    float3 logRefinedBlockMin = log2(refinedBlockMin + 1.0);

    float3 logBlockMax = log2(blockMax + 1.0);
    float3 logBlockMin = log2(blockMin + 1.0);
    float3 logBlockMaxExt = (logBlockMax - logBlockMin) * (1.0 / 32.0);

    logBlockMin += min(logRefinedBlockMin - logBlockMin, logBlockMaxExt);
    logBlockMax -= min(logBlockMax - logRefinedBlockMax, logBlockMaxExt);

    blockMin = exp2(logBlockMin) - 1.0;
    blockMax = exp2(logBlockMax) - 1.0;
}

// Least-squares endpoints for the indices the current endpoints select.
void OptimizeEndpointsP1(float3 texels[16], inout float3 blockMin, inout float3 blockMax,
                         float3 blockMinNonInset, float3 blockMaxNonInset)
{
    float3 blockDir = BlockDirection(blockMin, blockMax);

    float endPoint0Pos = float(f32tof16(dot(blockMin, blockDir)));
    float endPoint1Pos = float(f32tof16(dot(blockMax, blockDir)));

    float3 alphaTexelSum = 0.0;
    float3 betaTexelSum = 0.0;
    float alphaBetaSum = 0.0;
    float alphaSqSum = 0.0;
    float betaSqSum = 0.0;

    for (uint i = 0; i < 16; i++)
    {
        float texelPos = float(f32tof16(dot(texels[i], blockDir)));
        uint texelIndex = ComputeIndex4(texelPos, endPoint0Pos, endPoint1Pos);

        float beta = saturate(float(texelIndex) / 15.0);
        float alpha = 1.0 - beta;

        float3 texelF16 = HalfBits(texels[i]);
        alphaTexelSum += alpha * texelF16;
        betaTexelSum += beta * texelF16;

        alphaBetaSum += alpha * beta;

        alphaSqSum += alpha * alpha;
        betaSqSum += beta * beta;
    }

    float det = alphaSqSum * betaSqSum - alphaBetaSum * alphaBetaSum;

    if (abs(det) > 0.00001)
    {
        float detRcp = rcp(det);
        blockMin = clamp(f16tof32(uint3(clamp(detRcp * (alphaTexelSum * betaSqSum - betaTexelSum * alphaBetaSum), 0.0, HalfMax))),
                         blockMinNonInset, blockMaxNonInset);
        blockMax = clamp(f16tof32(uint3(clamp(detRcp * (betaTexelSum * alphaSqSum - alphaTexelSum * alphaBetaSum), 0.0, HalfMax))),
                         blockMinNonInset, blockMaxNonInset);
    }
}

// Least-squares endpoints for one region of a partition.
void OptimizeEndpointsP2(float3 texels[16], uint pattern, uint patternSelector, inout float3 blockMin,
                         inout float3 blockMax)
{
    float3 blockDir = BlockDirection(blockMin, blockMax);

    float endPoint0Pos = float(f32tof16(dot(blockMin, blockDir)));
    float endPoint1Pos = float(f32tof16(dot(blockMax, blockDir)));

    float3 alphaTexelSum = 0.0;
    float3 betaTexelSum = 0.0;
    float alphaBetaSum = 0.0;
    float alphaSqSum = 0.0;
    float betaSqSum = 0.0;

    for (uint i = 0; i < 16; i++)
    {
        uint paletteId = Pattern(pattern, i);
        if (paletteId == patternSelector)
        {
            float texelPos = float(f32tof16(dot(texels[i], blockDir)));
            uint texelIndex = ComputeIndex3(texelPos, endPoint0Pos, endPoint1Pos);

            float beta = saturate(float(texelIndex) / 7.0);
            float alpha = 1.0 - beta;

            float3 texelF16 = HalfBits(texels[i]);
            alphaTexelSum += alpha * texelF16;
            betaTexelSum += beta * texelF16;

            alphaBetaSum += alpha * beta;

            alphaSqSum += alpha * alpha;
            betaSqSum += beta * beta;
        }
    }

    float det = alphaSqSum * betaSqSum - alphaBetaSum * alphaBetaSum;

    if (abs(det) > 0.00001)
    {
        float detRcp = rcp(det);
        blockMin = f16tof32(uint3(clamp(detRcp * (alphaTexelSum * betaSqSum - betaTexelSum * alphaBetaSum), 0.0, HalfMax)));
        blockMax = f16tof32(uint3(clamp(detRcp * (betaTexelSum * alphaSqSum - alphaTexelSum * alphaBetaSum), 0.0, HalfMax)));
    }
}

// Packs a one-region mode 11 block: 10-bit endpoints and 4-bit indices, the first index's high bit
// implicit.
uint4 PackMode11(float3 endpoint0, float3 endpoint1, uint indices[16])
{
    uint4 block = uint4(0, 0, 0, 0);
    block.x = 0x03;
    block.x |= uint(endpoint0.x) << 5;
    block.x |= uint(endpoint0.y) << 15;
    block.x |= uint(endpoint0.z) << 25;
    block.y |= uint(endpoint0.z) >> 7;
    block.y |= uint(endpoint1.x) << 3;
    block.y |= uint(endpoint1.y) << 13;
    block.y |= uint(endpoint1.z) << 23;
    block.z |= uint(endpoint1.z) >> 9;

    block.z |= indices[0] << 1;
    block.z |= indices[1] << 4;
    block.z |= indices[2] << 8;
    block.z |= indices[3] << 12;
    block.z |= indices[4] << 16;
    block.z |= indices[5] << 20;
    block.z |= indices[6] << 24;
    block.z |= indices[7] << 28;
    block.w |= indices[8] << 0;
    block.w |= indices[9] << 4;
    block.w |= indices[10] << 8;
    block.w |= indices[11] << 12;
    block.w |= indices[12] << 16;
    block.w |= indices[13] << 20;
    block.w |= indices[14] << 24;
    block.w |= indices[15] << 28;
    return block;
}

// The one-region mode (mode 11): 10-bit endpoints, 4-bit indices.
void EncodeP1(inout uint4 block, inout float blockMsle, float3 texels[16], float errorFloor)
{
    float3 blockMin = texels[0];
    float3 blockMax = texels[0];
    for (uint i = 1; i < 16; ++i)
    {
        blockMin = min(blockMin, texels[i]);
        blockMax = max(blockMax, texels[i]);
    }

    float3 blockMinNonInset = blockMin;
    float3 blockMaxNonInset = blockMax;
    InsetColorBBoxP1(texels, blockMin, blockMax);
    OptimizeEndpointsP1(texels, blockMin, blockMax, blockMinNonInset, blockMaxNonInset);

    float3 blockDir = BlockDirection(blockMin, blockMax);

    float3 endpoint0 = Quantize10(blockMin);
    float3 endpoint1 = Quantize10(blockMax);
    float endPoint0Pos = float(f32tof16(dot(blockMin, blockDir)));
    float endPoint1Pos = float(f32tof16(dot(blockMax, blockDir)));

    // The first texel's index has an implicit high bit of zero, so swap the endpoints if it would
    // need one.
    float fixupTexelPos = float(f32tof16(dot(texels[0], blockDir)));
    uint fixupIndex = ComputeIndex4(fixupTexelPos, endPoint0Pos, endPoint1Pos);
    if (fixupIndex > 7)
    {
        Swap(endPoint0Pos, endPoint1Pos);
        Swap(endpoint0, endpoint1);
    }

    uint indices[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    for (uint i = 0; i < 16; ++i)
    {
        float texelPos = float(f32tof16(dot(texels[i], blockDir)));
        indices[i] = ComputeIndex4(texelPos, endPoint0Pos, endPoint1Pos);
    }

    float3 endpoint0Unq = Unquantize10(endpoint0);
    float3 endpoint1Unq = Unquantize10(endpoint1);
    float msle = 0.0;
    for (uint i = 0; i < 16; ++i)
    {
        float weight = floor((float(indices[i]) * 64.0) / 15.0 + 0.5);
        float3 texelUnc = FinishUnquantize(endpoint0Unq, endpoint1Unq, weight);

        msle += CalcMsle(texels[i], texelUnc, errorFloor);
    }

    blockMsle = msle;
    block = PackMode11(endpoint0, endpoint1, indices);
}

// The luminance-weighted index of every texel projected onto the segment between two quantized
// endpoints' decoded half-float bits: the space the decoder interpolates in.
void ProjectIndicesP1(float3 bits[16], float3 endpoint0, float3 endpoint1, out uint indices[16])
{
    const float3 weights = float3(0.299, 0.587, 0.114);
    const float3 low = InterpolatedBits(Unquantize10(endpoint0), Unquantize10(endpoint1), 0.0);
    const float3 high = InterpolatedBits(Unquantize10(endpoint0), Unquantize10(endpoint1), 64.0);
    const float3 span = high - low;
    const float denominator = dot(span * weights, span);
    for (uint i = 0; i < 16; ++i)
    {
        const float r = denominator > 0.0 ? dot((bits[i] - low) * weights, span) / denominator : 0.0;
        indices[i] = uint(clamp(r * 15.0 + 0.5, 0.0, 15.0));
    }
}

float3 QuantizeBits10(float3 bits)
{
    return min(floor(bits * 1024.0 / (0x7bff + 1.0)), 1023.0);
}

// The one-region mode with each channel's endpoints oriented by how it varies with the block's
// dominant channel. The bounding-box diagonal EncodeP1 fits runs every channel low to high
// together, so a block whose channels change in opposite directions (a hue gradient) cannot lie
// on it; this candidate can, and the lower error of the two is kept.
void EncodeP1Oriented(inout uint4 block, inout float blockMsle, float3 texels[16],
                      float errorFloor)
{
    float3 bits[16];
    float3 low = float3(65535.0);
    float3 high = float3(0.0);
    float3 mean = float3(0.0);
    for (uint i = 0; i < 16; ++i)
    {
        bits[i] = HalfBits(texels[i]);
        low = min(low, bits[i]);
        high = max(high, bits[i]);
        mean += bits[i] * (1.0 / 16.0);
    }
    const float3 range = high - low;
    const uint dominant = (range.x >= range.y && range.x >= range.z) ? 0u : (range.y >= range.z ? 1u : 2u);
    float3 covariance = float3(0.0);
    for (uint i = 0; i < 16; ++i)
    {
        const float3 d = bits[i] - mean;
        covariance += d * d[dominant];
    }
    float3 bits0 = select(covariance >= 0.0, low, high);
    float3 bits1 = select(covariance >= 0.0, high, low);

    // One least-squares refinement of the endpoints for the indices they select.
    uint indices[16];
    ProjectIndicesP1(bits, QuantizeBits10(bits0), QuantizeBits10(bits1), indices);
    float3 alphaTexelSum = 0.0;
    float3 betaTexelSum = 0.0;
    float alphaBetaSum = 0.0;
    float alphaSqSum = 0.0;
    float betaSqSum = 0.0;
    for (uint i = 0; i < 16; ++i)
    {
        const float beta = float(indices[i]) / 15.0;
        const float alpha = 1.0 - beta;
        alphaTexelSum += alpha * bits[i];
        betaTexelSum += beta * bits[i];
        alphaBetaSum += alpha * beta;
        alphaSqSum += alpha * alpha;
        betaSqSum += beta * beta;
    }
    const float det = alphaSqSum * betaSqSum - alphaBetaSum * alphaBetaSum;
    if (abs(det) > 0.00001)
    {
        const float detRcp = rcp(det);
        bits0 = clamp(detRcp * (alphaTexelSum * betaSqSum - betaTexelSum * alphaBetaSum), 0.0, 31743.0);
        bits1 = clamp(detRcp * (betaTexelSum * alphaSqSum - alphaTexelSum * alphaBetaSum), 0.0, 31743.0);
    }

    float3 endpoint0 = QuantizeBits10(bits0);
    float3 endpoint1 = QuantizeBits10(bits1);
    ProjectIndicesP1(bits, endpoint0, endpoint1, indices);
    if (indices[0] > 7)
    {
        Swap(endpoint0, endpoint1);
        for (uint i = 0; i < 16; ++i)
        {
            indices[i] = 15 - indices[i];
        }
    }

    const float3 endpoint0Unq = Unquantize10(endpoint0);
    const float3 endpoint1Unq = Unquantize10(endpoint1);
    float msle = 0.0;
    for (uint i = 0; i < 16; ++i)
    {
        const float weight = floor((float(indices[i]) * 64.0) / 15.0 + 0.5);
        msle += CalcMsle(texels[i], FinishUnquantize(endpoint0Unq, endpoint1Unq, weight),
                         errorFloor);
    }
    if (msle < blockMsle)
    {
        blockMsle = msle;
        block = PackMode11(endpoint0, endpoint1, indices);
    }
}

float DistToLineSq(float3 pointOnLine, float3 lineDirection, float3 p)
{
    float3 w = p - pointOnLine;
    float3 x = w - dot(w, lineDirection) * lineDirection;
    return dot(x, x);
}

float3 SafeNormalize(float3 v)
{
    float len = length(v);
    return len > 0.0 ? v / len : float3(0.0);
}

// How well a partition fits the block: the summed squared distance of each texel from its region's
// bounding-box diagonal.
float EvaluateP2Pattern(uint pattern, float3 texels[16])
{
    float3 p0BlockMin = float3(HalfMax);
    float3 p0BlockMax = float3(0.0);
    float3 p1BlockMin = float3(HalfMax);
    float3 p1BlockMax = float3(0.0);

    for (uint i = 0; i < 16; ++i)
    {
        uint paletteId = Pattern(pattern, i);
        if (paletteId == 0)
        {
            p0BlockMin = min(p0BlockMin, texels[i]);
            p0BlockMax = max(p0BlockMax, texels[i]);
        }
        else
        {
            p1BlockMin = min(p1BlockMin, texels[i]);
            p1BlockMax = max(p1BlockMax, texels[i]);
        }
    }

    float3 p0BlockDir = SafeNormalize(p0BlockMax - p0BlockMin);
    float3 p1BlockDir = SafeNormalize(p1BlockMax - p1BlockMin);

    float sqDistanceFromLine = 0.0;

    for (uint i = 0; i < 16; ++i)
    {
        uint paletteId = Pattern(pattern, i);
        if (paletteId == 0)
        {
            sqDistanceFromLine += DistToLineSq(p0BlockMin, p0BlockDir, texels[i]);
        }
        else
        {
            sqDistanceFromLine += DistToLineSq(p1BlockMin, p1BlockDir, texels[i]);
        }
    }

    return sqDistanceFromLine;
}

// The two-region modes on one partition, replacing the block when either beats its error.
void EncodeP2Pattern(inout uint4 block, inout float blockMsle, uint pattern, float3 texels[16],
                     float errorFloor)
{
    float3 p0BlockMin = float3(HalfMax);
    float3 p0BlockMax = float3(0.0);
    float3 p1BlockMin = float3(HalfMax);
    float3 p1BlockMax = float3(0.0);

    for (uint i = 0; i < 16; ++i)
    {
        uint paletteId = Pattern(pattern, i);
        if (paletteId == 0)
        {
            p0BlockMin = min(p0BlockMin, texels[i]);
            p0BlockMax = max(p0BlockMax, texels[i]);
        }
        else
        {
            p1BlockMin = min(p1BlockMin, texels[i]);
            p1BlockMax = max(p1BlockMax, texels[i]);
        }
    }

    OptimizeEndpointsP2(texels, pattern, 0, p0BlockMin, p0BlockMax);
    OptimizeEndpointsP2(texels, pattern, 1, p1BlockMin, p1BlockMax);

    float3 p0BlockDir = BlockDirection(p0BlockMin, p0BlockMax);
    float3 p1BlockDir = BlockDirection(p1BlockMin, p1BlockMax);

    float p0Endpoint0Pos = float(f32tof16(dot(p0BlockMin, p0BlockDir)));
    float p0Endpoint1Pos = float(f32tof16(dot(p0BlockMax, p0BlockDir)));
    float p1Endpoint0Pos = float(f32tof16(dot(p1BlockMin, p1BlockDir)));
    float p1Endpoint1Pos = float(f32tof16(dot(p1BlockMax, p1BlockDir)));

    uint fixupId = PatternFixupId(pattern);
    float p0FixupTexelPos = float(f32tof16(dot(texels[0], p0BlockDir)));
    float p1FixupTexelPos = float(f32tof16(dot(texels[fixupId], p1BlockDir)));
    uint p0FixupIndex = ComputeIndex3(p0FixupTexelPos, p0Endpoint0Pos, p0Endpoint1Pos);
    uint p1FixupIndex = ComputeIndex3(p1FixupTexelPos, p1Endpoint0Pos, p1Endpoint1Pos);
    if (p0FixupIndex > 3)
    {
        Swap(p0Endpoint0Pos, p0Endpoint1Pos);
        Swap(p0BlockMin, p0BlockMax);
    }
    if (p1FixupIndex > 3)
    {
        Swap(p1Endpoint0Pos, p1Endpoint1Pos);
        Swap(p1BlockMin, p1BlockMax);
    }

    uint indices[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    for (uint i = 0; i < 16; ++i)
    {
        float p0TexelPos = float(f32tof16(dot(texels[i], p0BlockDir)));
        float p1TexelPos = float(f32tof16(dot(texels[i], p1BlockDir)));
        uint p0Index = ComputeIndex3(p0TexelPos, p0Endpoint0Pos, p0Endpoint1Pos);
        uint p1Index = ComputeIndex3(p1TexelPos, p1Endpoint0Pos, p1Endpoint1Pos);

        uint paletteId = Pattern(pattern, i);
        indices[i] = paletteId == 0 ? p0Index : p1Index;
    }

    float3 endpoint760 = floor(Quantize7(p0BlockMin));
    float3 endpoint761 = floor(Quantize7(p0BlockMax));
    float3 endpoint762 = floor(Quantize7(p1BlockMin));
    float3 endpoint763 = floor(Quantize7(p1BlockMax));

    float3 endpoint950 = floor(Quantize9(p0BlockMin));
    float3 endpoint951 = floor(Quantize9(p0BlockMax));
    float3 endpoint952 = floor(Quantize9(p1BlockMin));
    float3 endpoint953 = floor(Quantize9(p1BlockMax));

    endpoint761 = endpoint761 - endpoint760;
    endpoint762 = endpoint762 - endpoint760;
    endpoint763 = endpoint763 - endpoint760;

    endpoint951 = endpoint951 - endpoint950;
    endpoint952 = endpoint952 - endpoint950;
    endpoint953 = endpoint953 - endpoint950;

    float maxVal76 = 31.0;
    endpoint761 = clamp(endpoint761, -maxVal76, maxVal76);
    endpoint762 = clamp(endpoint762, -maxVal76, maxVal76);
    endpoint763 = clamp(endpoint763, -maxVal76, maxVal76);

    float maxVal95 = 15.0;
    endpoint951 = clamp(endpoint951, -maxVal95, maxVal95);
    endpoint952 = clamp(endpoint952, -maxVal95, maxVal95);
    endpoint953 = clamp(endpoint953, -maxVal95, maxVal95);

    float3 endpoint760Unq = Unquantize7(endpoint760);
    float3 endpoint761Unq = Unquantize7(endpoint760 + endpoint761);
    float3 endpoint762Unq = Unquantize7(endpoint760 + endpoint762);
    float3 endpoint763Unq = Unquantize7(endpoint760 + endpoint763);
    float3 endpoint950Unq = Unquantize9(endpoint950);
    float3 endpoint951Unq = Unquantize9(endpoint950 + endpoint951);
    float3 endpoint952Unq = Unquantize9(endpoint950 + endpoint952);
    float3 endpoint953Unq = Unquantize9(endpoint950 + endpoint953);

    float msle76 = 0.0;
    float msle95 = 0.0;
    for (uint i = 0; i < 16; ++i)
    {
        uint paletteId = Pattern(pattern, i);

        float3 tmp760Unq = paletteId == 0 ? endpoint760Unq : endpoint762Unq;
        float3 tmp761Unq = paletteId == 0 ? endpoint761Unq : endpoint763Unq;
        float3 tmp950Unq = paletteId == 0 ? endpoint950Unq : endpoint952Unq;
        float3 tmp951Unq = paletteId == 0 ? endpoint951Unq : endpoint953Unq;

        float weight = floor((float(indices[i]) * 64.0) / 7.0 + 0.5);
        float3 texelUnc76 = FinishUnquantize(tmp760Unq, tmp761Unq, weight);
        float3 texelUnc95 = FinishUnquantize(tmp950Unq, tmp951Unq, weight);

        msle76 += CalcMsle(texels[i], texelUnc76, errorFloor);
        msle95 += CalcMsle(texels[i], texelUnc95, errorFloor);
    }

    SignExtend(endpoint761, 0x1Fu, 0x20u);
    SignExtend(endpoint762, 0x1Fu, 0x20u);
    SignExtend(endpoint763, 0x1Fu, 0x20u);

    SignExtend(endpoint951, 0xFu, 0x10u);
    SignExtend(endpoint952, 0xFu, 0x10u);
    SignExtend(endpoint953, 0xFu, 0x10u);

    float p2Msle = min(msle76, msle95);
    if (p2Msle < blockMsle)
    {
        blockMsle = p2Msle;
        block = uint4(0, 0, 0, 0);

        if (p2Msle == msle76)
        {
            // 7.6
            block.x = 0x1;
            block.x |= (uint(endpoint762.y) & 0x20u) >> 3;
            block.x |= (uint(endpoint763.y) & 0x10u) >> 1;
            block.x |= (uint(endpoint763.y) & 0x20u) >> 1;
            block.x |= uint(endpoint760.x) << 5;
            block.x |= (uint(endpoint763.z) & 0x01u) << 12;
            block.x |= (uint(endpoint763.z) & 0x02u) << 12;
            block.x |= (uint(endpoint762.z) & 0x10u) << 10;
            block.x |= uint(endpoint760.y) << 15;
            block.x |= (uint(endpoint762.z) & 0x20u) << 17;
            block.x |= (uint(endpoint763.z) & 0x04u) << 21;
            block.x |= (uint(endpoint762.y) & 0x10u) << 20;
            block.x |= uint(endpoint760.z) << 25;
            block.y |= (uint(endpoint763.z) & 0x08u) >> 3;
            block.y |= (uint(endpoint763.z) & 0x20u) >> 4;
            block.y |= (uint(endpoint763.z) & 0x10u) >> 2;
            block.y |= uint(endpoint761.x) << 3;
            block.y |= (uint(endpoint762.y) & 0x0Fu) << 9;
            block.y |= uint(endpoint761.y) << 13;
            block.y |= (uint(endpoint763.y) & 0x0Fu) << 19;
            block.y |= uint(endpoint761.z) << 23;
            block.y |= (uint(endpoint762.z) & 0x07u) << 29;
            block.z |= (uint(endpoint762.z) & 0x08u) >> 3;
            block.z |= uint(endpoint762.x) << 1;
            block.z |= uint(endpoint763.x) << 7;
        }
        else
        {
            // 9.5
            block.x = 0xE;
            block.x |= uint(endpoint950.x) << 5;
            block.x |= (uint(endpoint952.z) & 0x10u) << 10;
            block.x |= uint(endpoint950.y) << 15;
            block.x |= (uint(endpoint952.y) & 0x10u) << 20;
            block.x |= uint(endpoint950.z) << 25;
            block.y |= uint(endpoint950.z) >> 7;
            block.y |= (uint(endpoint953.z) & 0x10u) >> 2;
            block.y |= uint(endpoint951.x) << 3;
            block.y |= (uint(endpoint953.y) & 0x10u) << 4;
            block.y |= (uint(endpoint952.y) & 0x0Fu) << 9;
            block.y |= uint(endpoint951.y) << 13;
            block.y |= (uint(endpoint953.z) & 0x01u) << 18;
            block.y |= (uint(endpoint953.y) & 0x0Fu) << 19;
            block.y |= uint(endpoint951.z) << 23;
            block.y |= (uint(endpoint953.z) & 0x02u) << 27;
            block.y |= uint(endpoint952.z) << 29;
            block.z |= (uint(endpoint952.z) & 0x08u) >> 3;
            block.z |= uint(endpoint952.x) << 1;
            block.z |= (uint(endpoint953.z) & 0x04u) << 4;
            block.z |= uint(endpoint953.x) << 7;
            block.z |= (uint(endpoint953.z) & 0x08u) << 9;
        }

        block.z |= pattern << 13;
        uint blockFixupId = PatternFixupId(pattern);
        if (blockFixupId == 15)
        {
            block.z |= indices[0] << 18;
            block.z |= indices[1] << 20;
            block.z |= indices[2] << 23;
            block.z |= indices[3] << 26;
            block.z |= indices[4] << 29;
            block.w |= indices[5] << 0;
            block.w |= indices[6] << 3;
            block.w |= indices[7] << 6;
            block.w |= indices[8] << 9;
            block.w |= indices[9] << 12;
            block.w |= indices[10] << 15;
            block.w |= indices[11] << 18;
            block.w |= indices[12] << 21;
            block.w |= indices[13] << 24;
            block.w |= indices[14] << 27;
            block.w |= indices[15] << 30;
        }
        else if (blockFixupId == 2)
        {
            block.z |= indices[0] << 18;
            block.z |= indices[1] << 20;
            block.z |= indices[2] << 23;
            block.z |= indices[3] << 25;
            block.z |= indices[4] << 28;
            block.z |= indices[5] << 31;
            block.w |= indices[5] >> 1;
            block.w |= indices[6] << 2;
            block.w |= indices[7] << 5;
            block.w |= indices[8] << 8;
            block.w |= indices[9] << 11;
            block.w |= indices[10] << 14;
            block.w |= indices[11] << 17;
            block.w |= indices[12] << 20;
            block.w |= indices[13] << 23;
            block.w |= indices[14] << 26;
            block.w |= indices[15] << 29;
        }
        else
        {
            block.z |= indices[0] << 18;
            block.z |= indices[1] << 20;
            block.z |= indices[2] << 23;
            block.z |= indices[3] << 26;
            block.z |= indices[4] << 29;
            block.w |= indices[5] << 0;
            block.w |= indices[6] << 3;
            block.w |= indices[7] << 6;
            block.w |= indices[8] << 9;
            block.w |= indices[9] << 11;
            block.w |= indices[10] << 14;
            block.w |= indices[11] << 17;
            block.w |= indices[12] << 20;
            block.w |= indices[13] << 23;
            block.w |= indices[14] << 26;
            block.w |= indices[15] << 29;
        }
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_PC.BlocksPerAxis))
    {
        return;
    }

    // Texels in block order, row-major. The encoder works in half-float space, so a value is
    // clamped to the range it can store and a NaN reads as black.
    const int2 lastTexel = int2(g_PC.SourceSize) - 1;
    float3 texels[16];
    for (uint y = 0; y < 4; ++y)
    {
        for (uint x = 0; x < 4; ++x)
        {
            const int2 texel = min(int2(id.xy * 4 + uint2(x, y)), lastTexel);
            float3 value = g_Source.Load(int4(texel, int(id.z), 0)).rgb;
            value = select(isnan(value), float3(0.0), value);
            texels[y * 4 + x] = clamp(value, 0.0, HalfMax);
        }
    }

    // Errors below 1/256 of the block's brightest channel are not told apart: a texel that dim
    // beside a bright one, a dark sky beside a star, is not worth the bright texel's precision.
    float peak = 0.0;
    for (uint i = 0; i < 16; ++i)
    {
        peak = max(peak, max(texels[i].x, max(texels[i].y, texels[i].z)));
    }
    const float errorFloor = max(peak * (1.0 / 256.0), 1e-20);

    uint4 block = uint4(0, 0, 0, 0);
    float blockMsle = 0.0;
    EncodeP1(block, blockMsle, texels, errorFloor);
    EncodeP1Oriented(block, blockMsle, texels, errorFloor);

    uint bestPattern = 0;
    float bestScore = EvaluateP2Pattern(0, texels);
    for (uint patternIndex = 1; patternIndex < 32; ++patternIndex)
    {
        float score = EvaluateP2Pattern(patternIndex, texels);
        if (score < bestScore)
        {
            bestPattern = patternIndex;
            bestScore = score;
        }
    }
    EncodeP2Pattern(block, blockMsle, bestPattern, texels, errorFloor);

    const uint index = (id.z * g_PC.BlocksPerAxis.y + id.y) * g_PC.BlocksPerAxis.x + id.x;
    g_Blocks[g_PC.OutputBase + index] = block;
}
