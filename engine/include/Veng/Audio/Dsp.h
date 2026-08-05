#pragma once

/// @file
/// @brief Umbrella include for the Veng::Audio::Dsp synthesis primitive library.
///
/// The reusable DSP parts a consumer composes to invent a sound inside its own IAudioGenerator: a
/// morphable anti-aliased oscillator, white/pink noise, an ADSR envelope, a resonant state-variable
/// filter, an LFO, a fractional delay line, a parameter smoother, and the custom-source / custom-
/// filter escape-hatch nodes. Every primitive is header-inline and allocation-free on the real-time
/// thread — allocating only in an explicit Prepare — and advances purely by the sample count it is
/// handed, so it makes no block-size assumption. Include this to pull the whole kit, or a single
/// Veng/Audio/Dsp/<primitive>.h for one part.

#include <Veng/Audio/Dsp/CustomNode.h>
#include <Veng/Audio/Dsp/DelayLine.h>
#include <Veng/Audio/Dsp/Envelope.h>
#include <Veng/Audio/Dsp/Filter.h>
#include <Veng/Audio/Dsp/Lfo.h>
#include <Veng/Audio/Dsp/Noise.h>
#include <Veng/Audio/Dsp/Oscillator.h>
#include <Veng/Audio/Dsp/Smoother.h>
