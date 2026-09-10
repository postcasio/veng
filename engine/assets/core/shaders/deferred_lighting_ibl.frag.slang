// The IBL-contribution debug deferred lighting variant: defines VE_DEBUG_IBL, then
// includes the shared lighting body, which returns only the IBL ambient term
// (diffuse + specular IBL × ambient occlusion) per fragment and writes it unlit.
// The renderer wires this pipeline for the DebugView::IblContribution arm so the
// environment lighting each pixel receives is directly observable.
#define VE_DEBUG_IBL 1
#include "deferred_lighting.frag.slang"
