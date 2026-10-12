// Screen-space reflections: the codes the classifier's coverage entry point writes per pixel, and
// the coverage debug blit reads, stored as code / 255 in an R8 target.

// No reflection: an offscreen or background pixel, or a rough one with no environment to take.
static const uint SsrCoverageNone = 0;
// The view's environment alone: reflective, but too rough to trace.
static const uint SsrCoverageEnvironment = 1;
// Its own ray: glossy, and its quad's base ray.
static const uint SsrCoverageRay = 2;
// Its own ray, traced only because its temporal variance exceeds the threshold.
static const uint SsrCoverageVarianceRay = 3;
// A quad neighbour's ray, copied by the intersection.
static const uint SsrCoverageCopied = 4;
// Its own mirror ray, which skips the denoiser.
static const uint SsrCoverageMirror = 5;
// Glossy and wanting a neighbour's ray that no neighbour traced.
static const uint SsrCoverageUnreached = 6;
