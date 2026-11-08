// The Metal backend's count-driven indirect draw: Metal draws one indirect record per call and has
// no draw whose count a buffer supplies, so CommandBuffer::PrepareIndirectCount dispatches this
// kernel to translate a range of the engine's indirect records into an indirect command buffer, and
// the draw executes that buffer over a range the kernel writes from the GPU-side count. One thread
// per record the range may hold; thread 0 writes the execution range.
//
// It writes Metal command_buffer objects, which the shader cook's language cannot express, so it is
// written in Metal Shading Language and compiled at build time into the backend's own library.

#include <metal_stdlib>
using namespace metal;

// Mirrors Metal::IndirectCountParams (CommandBuffer.mm) field for field.
struct IndirectCountParams
{
    command_buffer Commands;      // the indirect command buffer the range is encoded into
    device const uchar* Records;  // the first engine record
    device const uint* Count;     // the draw count the producer wrote
    device const uchar* Indices;  // the bound index buffer, for an indexed draw
    uint Stride;                  // bytes between records
    uint MaxDraws;                // the most records drawn
    uint First;                   // the range's first command in Commands
    uint IndexType;               // 0 a non-indexed draw, 1 16-bit indices, 2 32-bit
    uint Primitive;               // the draw's primitive type, or Undrawn
    uint Pad;
    uint RangeLocation;           // the execution range the draw reads (written here)
    uint RangeLength;
};

// A prepared range no draw consumed in its recording.
constant uint Undrawn = 0xFFFFFFFFu;

kernel void EncodeIndirectCount(device IndirectCountParams& params [[buffer(0)]],
                                uint index [[thread_position_in_grid]])
{
    const bool drawn = params.Primitive != Undrawn;
    const uint count = drawn ? min(*params.Count, params.MaxDraws) : 0u;
    if (index == 0u)
    {
        params.RangeLocation = params.First;
        params.RangeLength = count;
    }
    if (index >= count)
    {
        return;
    }

    render_command command(params.Commands, params.First + index);
    device const uint* record = (device const uint*)(params.Records + index * params.Stride);
    const primitive_type primitive = primitive_type(params.Primitive);
    if (params.IndexType == 0u)
    {
        // VertexCount, InstanceCount, FirstVertex, FirstInstance.
        if (record[0] == 0u || record[1] == 0u)
        {
            command.reset();
            return;
        }
        command.draw_primitives(primitive, record[2], record[0], record[1], record[3]);
        return;
    }
    // IndexCount, InstanceCount, FirstIndex, VertexOffset, FirstInstance.
    if (record[0] == 0u || record[1] == 0u)
    {
        command.reset();
        return;
    }
    if (params.IndexType == 1u)
    {
        command.draw_indexed_primitives(primitive, record[0],
                                        (device const ushort*)params.Indices + record[2],
                                        record[1], record[3], record[4]);
    }
    else
    {
        command.draw_indexed_primitives(primitive, record[0],
                                        (device const uint*)params.Indices + record[2], record[1],
                                        record[3], record[4]);
    }
}
