// The cascade-debug deferred lighting variant: defines VE_DEBUG_CASCADES, then
// includes the shared lighting body, which tints each fragment by the cascade its
// view-space depth selects (the same view-depth → cascade logic the shadow sample
// uses) and writes it unlit. The renderer wires this pipeline for the
// DebugView::Cascades arm so cascade selection is directly observable.
#define VE_DEBUG_CASCADES 1
#include "deferred_lighting.frag.slang"
