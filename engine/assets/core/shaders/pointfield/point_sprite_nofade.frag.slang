// The depth-fade-free point-field sprite fragment variant: defines VE_POINT_DEPTH_FADE 0, then
// includes the shared sprite body, which drops the g-buffer depth sample, its sub-rect remap, and
// the occluded-fade compare entirely. The pass selects this pipeline for a field whose LOD sets
// DepthFade off — a field composited over background with no occluding geometry, where the fade
// can never trigger, so it pays no bindless depth sample per fragment.
#define VE_POINT_DEPTH_FADE 0
#include "point_sprite.frag.slang"
