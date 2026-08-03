#include "Triangle.hlsli"

Texture2D outPutRT : register(t6);

///fov 兼容

float4 main(float4 position : SV_Position ,float2 texcoord : TEXCOORD0) : SV_Target
{
    float2 adjTex = texcoord - float2(0.5, 0.5);
    adjTex *= rcp(2.0F);
    adjTex += float2(0.5, 0.5);
    float2 pos = adjTex / PixelSize;
    float2 ScopeScreenPosInPixel = ScopeScreenPos * PixelSize;
    float2 ScopeScreenPosInPixelForDisplay = ScopeScreenPos * PixelSize;

    float2 basePos = float2(0.5, 0.5) + ScopeEffect_OriPositionOffset * float2(-1, 1);
    float2 texcoordOffset = (basePos - ScopeScreenPosInPixel) * camDepth;
    float2 screenSize = float2(BUFFER_WIDTH, BUFFER_HEIGHT);

    float2 ScopeEffect_OffsetA = (ScopeEffect_Offset);
    float2 ScopeEffect_SizeA = (ScopeEffect_Size);

    float4 color = outPutRT.Sample(gSamLinear, adjTex + texcoordOffset);
    float dx = texcoord.x - 0.5 - ScopeEffect_OffsetA.x;
    float dy = texcoord.y - 0.5 + ScopeEffect_OffsetA.y;
    float r = (ScopeEffect_SizeA.x) * 0.5;

    float4 rect = ScopeEffect_Rect + ScopeEffect_OffsetA.xyxy;
    bool isRender = isCircle ? (dx * dx + dy * dy < r * r) : (texcoord.x >= rect.x && texcoord.y >= rect.y && texcoord.x <= rect.z && texcoord.y <= rect.w);

    return color * isRender * EnableMerge;
   
}
