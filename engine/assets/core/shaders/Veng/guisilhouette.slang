// The GUI silhouette distance functions shared by the fixed shape path (gui_shape.frag) and the
// GuiFill material domain (Veng/guifill.slang): the rounded rectangle every element has by default
// and the annular sector an arc-shaped element replaces it with. Each is a signed distance in
// pixels — negative inside, zero on the edge, rate of change one — so a caller's fwidth()
// anti-aliasing stays one pixel wide, a border ring is the same field offset inward, and a shadow
// is the same field displaced and grown.

static const float GuiTau = 6.28318530717958647692;

// Signed distance to a rounded rectangle centered at the origin, half-extent b, corner radius r
// (subtracted from the box so the corners round inward).
float GuiRoundedBoxSdf(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Signed distance to an annular sector centred at the origin, as the gui vertex stage's arc lane
// encodes it: x the start and y the sweep, radians clockwise from 12 o'clock in the y-down space;
// z the band thickness inward from the rim; w the outer radius, negative for round caps. A
// thickness of zero or at least the radius fills to the centre. A sweep of a full turn is a whole
// ring or disc with no seam, and a zero sweep covers nothing.
float GuiArcSdf(float2 p, float4 arc)
{
    float sweep = arc.y;
    float thickness = arc.z;
    float radius = abs(arc.w);
    if (sweep <= 0.0)
    {
        return 1e6;
    }

    bool filled = thickness <= 0.0 || thickness >= radius;
    float halfBand = thickness * 0.5;
    float midRadius = radius - halfBand;
    float r = length(p);
    float ringDist = filled ? r - radius : abs(r - midRadius) - halfBand;
    if (sweep >= GuiTau - 1e-4)
    {
        return ringDist;
    }

    // Fold the plane about the sector's bisector: q.y runs along the bisector and q.x is the
    // unsigned distance off it, so both ends are one mirrored edge at `aperture` from the axis.
    float aperture = sweep * 0.5;
    float bisector = arc.x + aperture;
    float2 axis = float2(sin(bisector), -cos(bisector));
    float2 across = float2(cos(bisector), sin(bisector));
    float2 q = float2(abs(dot(p, across)), dot(p, axis));
    float2 edge = float2(sin(aperture), cos(aperture));
    bool beyondEdge = edge.y * q.x > edge.x * q.y;

    // Round caps: past the end, the distance is to the cap's centre on the band's mid-line; within
    // the aperture, it is the ring's. The caps are semicircles of the band's own thickness.
    if (arc.w < 0.0 && !filled)
    {
        float capDist = beyondEdge ? length(q - edge * midRadius) : abs(r - midRadius);
        return capDist - halfBand;
    }

    // Butt caps: the ring intersected with the wedge. The wedge's distance is to its edge ray (the
    // mirror makes the nearer ray the one on q's side), signed by which side of the edge q lies.
    float wedgeDist = length(q - edge * max(dot(q, edge), 0.0));
    wedgeDist = beyondEdge ? wedgeDist : -wedgeDist;
    return max(ringDist, wedgeDist);
}

// The silhouette a quad is shaped by: its arc when the arc lane is active (a non-zero w), else the
// rounded box of its own rect. The arc reads its own coordinate (relative to the arc's centre), the
// box the quad's (relative to the quad's centre) — they differ wherever a quad is not the element's
// own box, a padding-box image or a nine-slice cell.
float GuiSilhouetteSdf(float2 rectCoord, float2 rectHalf, float radius, float2 arcCoord, float4 arc)
{
    if (arc.w != 0.0)
    {
        return GuiArcSdf(arcCoord, arc);
    }
    return GuiRoundedBoxSdf(rectCoord, rectHalf, radius);
}
