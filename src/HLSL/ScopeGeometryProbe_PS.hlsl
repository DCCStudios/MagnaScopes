// Stage 4d uses this shader only for the exact authored ScopeFade draw.
// SV_Position is the sole input so the shader remains compatible with Fallout
// 4's existing vertex shader output without assuming its private interpolator
// layout.
float4 main(float4 position : SV_Position) : SV_Target0
{
    return float4(0.0f, 0.82f, 1.0f, 1.0f);
}
