// Scene-light queries for a participating medium a Translucent material marches itself: volumetric
// fog with light shafts, dust or smoke lit through a window, any medium filling the view. Where
// Veng/forward_lighting.slang lights a translucent *surface* (a BRDF against a normal, once per
// fragment), this answers the two questions a medium asks at many points along its ray: which
// light is the one it wants (FindLightByTag), and how much of that light's shadow-mapped visibility
// reaches a world position (LightVisibility). The queries themselves are
// Veng/light_visibility.slang's, which this re-exports with the declarations the Translucent
// domain's forward-lit layout needs beside them.
//
// The includer declares the view block (view_constants.slang) first; a Translucent material has it
// through Veng/translucent.slang, and a material that also lights a surface includes
// Veng/forward_lighting.slang (which re-exports that contract) and this after it, the two sharing
// the shadow system and light buffer. Nothing here reads a push block.
//
// Bindings: the set-0 light buffer (binding 6, through Veng/light_buffer.slang), the shadow system
// at set 5 (through shadow.slang), and the IBL maps' declarations at set 4, which nothing here
// samples. A Translucent fragment declaring those sets is recognised as forward-lit and drawn with
// the renderer's own IBL and shadow sets bound, in the full- and the reduced-resolution translucent
// layer alike (see Veng/forward_lighting.slang, "Bindings").
//
// Everything here takes no screen-space derivative, so it is legal in non-uniform control flow
// and inside a loop: per march step, after a discard, in a branch some fragments of a quad skip.

#pragma once

// The light buffer, the shadow system at set 5 and the queries over them.
#include "Veng/light_visibility.slang"
// Nothing here reads the IBL maps, but a material's declared sets must be contiguous and the
// forward-lit layout binds set 4 ahead of the shadow set, so set 4 is declared all the same.
#include "Veng/ibl_maps.slang"
