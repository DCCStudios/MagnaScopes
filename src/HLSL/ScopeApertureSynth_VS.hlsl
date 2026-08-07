// Vertex stage for the synthesized aperture -- the ScopeFade-equivalent ring
// generated for scopes that ship no authored ScopeFade.
//
// It does nothing, and that is the point. The game thread already projected
// the ring through the same camera contract the published lens basis uses, so
// the positions arriving here are finished clip-space coordinates. Re-deriving
// them from a world position would mean reconstructing a view-projection
// matrix on the render thread and hoping it matched, which is exactly the
// class of duplicated-derivation bug this project keeps paying for.
//
// The output signature is ScopeGeometryFill_GS's GeometryInput exactly. That
// shader then fabricates the centre fan and publishes lens coordinates from
// primitive order, unchanged, because the ring was generated in ScopeFade's
// own 24-segment topology.

struct SynthVertex
{
    float4 position : POSITION;
};

struct SynthOutput
{
    float4 position : SV_Position;
};

SynthOutput main(SynthVertex input)
{
    SynthOutput output;
    output.position = input.position;
    return output;
}
