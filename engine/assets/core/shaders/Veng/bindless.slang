// The typed bindless registries, sets 0-2, mirroring BindlessRegistry's layout: each field takes the
// binding its position names (Textures = 0, Samplers = 1, StorageImages = 2, Buffers = 3,
// MaterialParams = 4, ViewConstants = 5, Lights = 6, AreaVertices = 7, NormalVariance = 8), and each
// array is sized by the registry's capacity, since a Metal argument buffer holds no unsized array.
//
// The g_* names every engine contract has always exposed stay available as aliases of the fields.
#pragma once

#include "binding.slang"

struct BindlessSet
{
    // The 2D sampled-image array; a TextureHandle is an index into it.
    Texture2D Textures[1024];
    // The sampler array; a SamplerHandle is an index into it.
    SamplerState Samplers[128];
    // The storage-image array; a StorageImageHandle is an index into it.
    RWTexture2D<float4> StorageImages[512];
    // The byte-address storage-buffer array, each slot a whole buffer a shader reads typed at a byte
    // offset (g_Buffers[handle].Load<T>(offset)).
    ByteAddressBuffer Buffers[256];
    // Every material's parameter block, each at its own byte offset (the selector a draw is handed).
    ByteAddressBuffer MaterialParams;
    // The per-frame view-constants block (view_constants.slang).
    ByteAddressBuffer ViewConstants;
    // The per-view light buffer (Veng/light_buffer.slang).
    ByteAddressBuffer Lights;
    // The area-light polygon vertices (Veng/light_buffer.slang).
    ByteAddressBuffer AreaVertices;
    // The normal-variance array, parallel to Textures: slot i is the companion of texture i.
    Texture2D NormalVariance[1024];
};

struct BindlessVolumeSet
{
    // The 3D sampled-image array; a VolumeHandle is an index into it.
    Texture3D Volumes[256];
};

struct BindlessCubeSet
{
    // The cube sampled-image array; a CubeHandle is an index into it.
    TextureCube Cubes[128];
};

VE_DESCRIPTOR_SET(0, BindlessSet, g_Bindless);
VE_DESCRIPTOR_SET(1, BindlessVolumeSet, g_BindlessVolumes);
VE_DESCRIPTOR_SET(2, BindlessCubeSet, g_BindlessCubes);

#define g_Textures g_Bindless.Textures
#define g_Samplers g_Bindless.Samplers
#define g_StorageImages g_Bindless.StorageImages
#define g_Buffers g_Bindless.Buffers
#define g_MaterialParams g_Bindless.MaterialParams
#define g_ViewConstants g_Bindless.ViewConstants
#define g_Lights g_Bindless.Lights
#define g_AreaVertices g_Bindless.AreaVertices
#define g_NormalVariance g_Bindless.NormalVariance
#define g_Volumes g_BindlessVolumes.Volumes
#define g_Cubes g_BindlessCubes.Cubes
