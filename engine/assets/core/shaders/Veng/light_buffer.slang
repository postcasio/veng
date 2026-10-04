// The per-view light buffers and the packed light record: what the renderer writes for each view's
// lights, and the accessors that read them back. Shared by the lighting core (Veng/lighting.slang)
// and by any pass that reasons about the view's lights without shading — the light-tile cull reads
// the same records to bound each light's reach — so the layout is declared once.
//
// The includer declares the view block (view_constants.slang) before including this. This file
// declares the set-0 light and area-vertex buffers (bindings 6 and 7) and nothing else.

[[vk::binding(6, 0)]] ByteAddressBuffer g_Lights;
// The per-view area-light polygon-vertex buffer (Rect/Polygon world-space vertices, one vec4
// each). A light's Area.y/z give the base/count relative to this view's region; the view block's
// area-vertex base folds in the region start, exactly as its light base does for the light buffer.
[[vk::binding(7, 0)]] ByteAddressBuffer g_AreaVertices;

static const uint LightStride = 112;
static const uint AreaVertexStride = 16;

// LightType, matching the Light builtin's enum.
static const float LightTypeDirectional = 0.0;
static const float LightTypePoint = 1.0;
static const float LightTypeSpot = 2.0;
static const float LightTypeRect = 3.0;
static const float LightTypeSphere = 4.0;
static const float LightTypePolygon = 5.0;

// One light, packed by the renderer into the ring-buffered light buffer. Position
// is the entity's world transform; the type rides DirectionType.w. Area and AreaNormal
// carry the source/shape size: a Sphere's or a Point/Spot's source radius, a
// Rect/Polygon's vertex range into the area-vertex buffer, the area-shadow slot, and
// the world area normal. Response weights the light's terms.
struct GpuLight
{
    float4 PositionRange;   // xyz world position, w range
    float4 DirectionType;   // xyz travel direction, w LightType
    float4 ColorIntensity;  // rgb linear color, a intensity
    float4 Cone;            // x cos(inner), y cos(outer), z punctual shadow slot, w LightFlag bits
    float4 Area;            // x sphere/point/spot source radius, y poly vertex base, z poly vertex count, w area-shadow slot
    float4 AreaNormal;      // xyz world area normal (Rect/Polygon), w shadow source radius
    float4 Response;        // x specular scale, yzw unused
};

// The index-th light of this view's region.
GpuLight LoadLight(ViewConstants vc, uint index)
{
    return g_Lights.Load<GpuLight>((vc.LightState.x + index) * LightStride);
}

// Load a packed area-light polygon vertex (world space) by its region-relative index.
float3 LoadAreaVertex(ViewConstants vc, uint index)
{
    return g_AreaVertices.Load<float4>((vc.LightState.z + index) * AreaVertexStride).xyz;
}

// A light mask naming every light of a view holding count lights: bit i is the view's i-th light.
// A view holds at most 16 (the renderer's MaxLights), so one 32-bit word names any subset of them.
uint AllLightsMask(uint count)
{
    return count >= 32u ? 0xFFFFFFFFu : (1u << count) - 1u;
}
