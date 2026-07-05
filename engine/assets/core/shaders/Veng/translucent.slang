// The engine's Translucent material contract: a forward, alpha-blended surface domain that
// writes final HDR radiance directly (not the g-buffer). A Translucent material shares the
// canonical-layout vertex stage and the per-draw plumbing of the Surface domain — it is drawn
// per-submesh from the same gathered geometry, reads its per-draw record from the DrawData SSBO
// by the instance-rate candidate id, and reads its material selector from the v_MaterialIndex
// interpolant — so it `#include`s Veng/surface.slang for the set-0 bindless declarations, the
// per-frame view block, DrawData / LoadDrawData, the surface push block (g_PC), and the
// SurfaceFragmentInput struct. It declares its own MaterialParams beside this include, exactly as
// a Surface material does (the parameter block is per-shader — the cooker reflects each shader's
// own struct to pack its fields at the reflected offsets — so it is not part of this contract).
//
// A Translucent material's fragment stage outputs a single float4 SV_Target0: HDR linear color in
// rgb and coverage in a. The forward translucent pass draws it into the lit HDR scene-color target
// after deferred lighting and the sky composite and before the bloom/tonemap tail, so translucents
// bloom and tonemap with the scene. The pass depth-TESTs against the opaque depth buffer with depth
// writes OFF (a translucent surface never occludes another), sorts submeshes back-to-front by view
// depth, and blends with STRAIGHT (non-premultiplied) alpha: the destination is weighted by
// (1 - a) and the source color by a, so a fragment returns its color un-premultiplied and lets the
// blend apply coverage. A fragment returns final radiance directly — it is responsible for its own
// shading, and the engine runs no forward light loop for it; the set-0 view block (camera/view
// state) is available for a fragment that shades itself.
//
// The Surface, PostProcess, and Sky domains have their own contracts in Veng/surface.slang,
// Veng/postprocess.slang, and Veng/sky.slang.

#include "Veng/surface.slang"
