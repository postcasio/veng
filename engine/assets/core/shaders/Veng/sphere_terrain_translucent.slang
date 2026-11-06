// The translucent level-layer contract: what a Translucent material declaring "sphereTerrain" reads.
// Such a material draws a sphere terrain's level layer in the translucent pass, over the lit opaque
// scene, so a surface at a constant height can show what lies beneath it. Its vertex stage is the
// engine's translucent_terrain.vert; its fragment includes this file (in place of
// Veng/forward_lighting.slang, which it re-exports, and Veng/sphere_terrain.slang, which it re-exports
// with the terrain set moved) and takes SphereTerrainFragmentInput.
//
// - Sets: 3 the per-draw DrawData, 4 the IBL maps and 5 the shadow system (forward lighting's), 6 the
//   terrain's pool and records. The pass binds all four on every draw, so EvaluateForwardLighting and
//   LightVisibility are available whether or not the fragment calls them.
// - Every Terrain* helper of Veng/sphere_terrain.slang reads as it does for the opaque surface.
// - The Veng/translucent.slang contract holds: the fragment returns straight colour and coverage (or
//   one of its output structs), the pass blends it over the scene with depth writes off, and the
//   scene colour and depth copies are read through SampleSceneColor / LoadSceneDepth once the material
//   declares "readsSceneColor" or "readsSceneDepth".
// - The pass depth-tests against the opaque depth, which a terrain drawn at a scale about the eye
//   holds at its true depth. A layer drawn at a scale other than one therefore returns
//   TranslucentDepthOutput with TerrainTrueDeviceDepth, so it tests at its own true depth too; at
//   scale one the rasterized depth already is.
// - A layer seen from below as well as above authors "cull": "None"; the grid is wound
//   counter-clockwise seen from outside the sphere.
#pragma once

#ifdef VE_SPHERE_TERRAIN_SET
#if VE_SPHERE_TERRAIN_SET != 6
#error "Veng/sphere_terrain_translucent.slang must be included before Veng/sphere_terrain.slang"
#endif
#endif
#define VE_SPHERE_TERRAIN_SET 6

#include "Veng/forward_lighting.slang"
#include "Veng/sphere_terrain.slang"
