// Occlusion-sphere gizmo, in-scene variant. The sphere's vertices are
// authored in the ScopeFade mesh's LOCAL space and drawn through the game's
// own vertex shader at the matched ScopeFade draw, so clip position and
// depth are computed by the exact transform that placed the scope housing --
// correctness inherited, not reconstructed. Only SV_Position is declared
// from the game VS's outputs (D3D linkage permits consuming a subset), so
// this shader cannot depend on any of its other semantics and shades flat;
// the wireframe second pass supplies the volume reading.
cbuffer SphereColor : register(b0)
{
    float4 sphereColor;
};

float4 main(float4 position : SV_Position) : SV_Target
{
    return sphereColor;
}
