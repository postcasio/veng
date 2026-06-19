// The SSAO-enabled deferred lighting variant: defines VE_USE_SSAO, then includes
// the shared lighting body, which samples the screen-space AO target and folds it
// into the ambient/occlusion term. The renderer selects this pipeline over the
// plain deferred_lighting.frag when its AO setting is on — SSAO is a compile-time
// variant, not a per-frame branch.
#define VE_USE_SSAO 1
#include "deferred_lighting.frag.slang"
