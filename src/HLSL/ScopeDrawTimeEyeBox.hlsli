// ScopeFade is transformed by Fallout's authored vertex shader immediately
// before this code runs. Reconstructing the lens frame from that SV_Position
// output therefore observes the exact recoil, sway, and inertia pose submitted
// for this draw. No CPU scene-graph read or FPGunplayOverhaul integration is
// involved.

struct ScopeDrawTimeLensFrame
{
    float4 centerClip;
    float4 columnXClip;
    float4 columnZClip;
    float2 centerPixels;
    float2 basisX;
    float2 basisZ;
    float determinant;
    float valid;
};

float2 ScopeClipToPixels(float4 clipPosition)
{
    const float inverseW =
        abs(clipPosition.w) > 0.00001f ?
            rcp(clipPosition.w) :
            0.0f;
    const float2 ndc = clipPosition.xy * inverseW;
    return float2(
        (ndc.x * 0.5f + 0.5f) * BUFFER_WIDTH,
        (0.5f - ndc.y * 0.5f) * BUFFER_HEIGHT);
}

float2 ScopePixelsToNdc(float2 pixels)
{
    return float2(
        pixels.x * (2.0f / BUFFER_WIDTH) - 1.0f,
        1.0f - pixels.y * (2.0f / BUFFER_HEIGHT));
}

float2 ScopeLensCoordinatesToPixels(
    ScopeDrawTimeLensFrame frame,
    float2 lensCoordinates)
{
    return ScopeClipToPixels(
        frame.centerClip +
        frame.columnXClip * lensCoordinates.x +
        frame.columnZClip * lensCoordinates.y);
}

float2 ScopeLensDisplacementToPixels(
    ScopeDrawTimeLensFrame frame,
    float2 lensCoordinates)
{
    return
        ScopeLensCoordinatesToPixels(frame, lensCoordinates) -
        frame.centerPixels;
}

ScopeDrawTimeLensFrame BuildScopeDrawTimeLensFrame(
    float4 centerClip,
    float4 outerCurrentClip,
    float4 outerNextClip,
    float2 localOuterCurrentDirection,
    float2 localOuterNextDirection)
{
    ScopeDrawTimeLensFrame result = (ScopeDrawTimeLensFrame)0;
    result.centerClip = centerClip;
    result.centerPixels = ScopeClipToPixels(centerClip);

    const float currentLength = length(localOuterCurrentDirection);
    const float nextLength = length(localOuterNextDirection);
    const float2 currentDirection =
        localOuterCurrentDirection / max(currentLength, 0.0001f);
    const float2 nextDirection =
        localOuterNextDirection / max(nextLength, 0.0001f);
    const float directionDeterminant =
        currentDirection.x * nextDirection.y -
        currentDirection.y * nextDirection.x;
    const float safeDirectionDeterminant =
        abs(directionDeterminant) > 0.00001f ?
            directionDeterminant :
            1.0f;

    const float4 currentRadialClip =
        outerCurrentClip - centerClip;
    const float4 nextRadialClip =
        outerNextClip - centerClip;
    result.columnXClip =
        (currentRadialClip * nextDirection.y -
         nextRadialClip * currentDirection.y) /
        safeDirectionDeterminant;
    result.columnZClip =
        (-currentRadialClip * nextDirection.x +
         nextRadialClip * currentDirection.x) /
        safeDirectionDeterminant;

    // These projected unit-axis deltas remain useful for the established CPU
    // fallback and diagnostics. Unlike the old construction, neither column
    // is forced perpendicular to the other.
    result.basisX =
        ScopeLensCoordinatesToPixels(result, float2(1.0f, 0.0f)) -
        result.centerPixels;
    result.basisZ =
        ScopeLensCoordinatesToPixels(result, float2(0.0f, 1.0f)) -
        result.centerPixels;
    result.determinant =
        result.basisX.x * result.basisZ.y -
        result.basisX.y * result.basisZ.x;
    result.valid =
        currentLength > 0.0001f &&
        nextLength > 0.0001f &&
        abs(directionDeterminant) > 0.00001f &&
        abs(centerClip.w) > 0.00001f &&
        dot(result.basisX, result.basisX) > 0.0001f &&
        dot(result.basisZ, result.basisZ) > 0.0001f &&
        abs(result.determinant) > 0.0001f ?
            1.0f :
            0.0f;
    return result;
}

float2 SolveScopeLensCoordinates(
    float2 targetPixels,
    ScopeDrawTimeLensFrame frame)
{
    if (frame.valid < 0.5f) {
        return float2(0.0f, 0.0f);
    }
    const float2 targetNdc = ScopePixelsToNdc(targetPixels);
    const float2 columnX = float2(
        frame.columnXClip.x -
            targetNdc.x * frame.columnXClip.w,
        frame.columnXClip.y -
            targetNdc.y * frame.columnXClip.w);
    const float2 columnZ = float2(
        frame.columnZClip.x -
            targetNdc.x * frame.columnZClip.w,
        frame.columnZClip.y -
            targetNdc.y * frame.columnZClip.w);
    const float2 constant = float2(
        frame.centerClip.x -
            targetNdc.x * frame.centerClip.w,
        frame.centerClip.y -
            targetNdc.y * frame.centerClip.w);
    const float homogeneousDeterminant =
        columnX.x * columnZ.y -
        columnX.y * columnZ.x;
    if (abs(homogeneousDeterminant) <= 0.00001f) {
        return float2(0.0f, 0.0f);
    }
    return float2(
        (-constant.x * columnZ.y +
         constant.y * columnZ.x) /
            homogeneousDeterminant,
        (-columnX.x * constant.y +
         columnX.y * constant.x) /
            homogeneousDeterminant);
}

float2 CalculateScopeDrawTimeEyeTravel(
    ScopeDrawTimeLensFrame frame,
    float2 authoredAimOffsetLocal,
    float activation,
    float maximumTravel)
{
    if (frame.valid < 0.5f) {
        return float2(0.0f, 0.0f);
    }

    // Existing STS setups align their reticle to the camera axis through NIF
    // and weapon offsets. Solve the camera-axis intersection on the exact
    // projective lens plane, then preserve that authored local relationship.
    // This remains correct when the two projected axes are sheared or have
    // different homogeneous W gradients.
    const float2 cameraAxisLocal = SolveScopeLensCoordinates(
        0.5f * float2(BUFFER_WIDTH, BUFFER_HEIGHT),
        frame);
    float2 travel = cameraAxisLocal - authoredAimOffsetLocal;
    const float boundedMaximum = clamp(maximumTravel, 0.0f, 4.0f);
    const float travelLength = length(travel);
    if (travelLength > boundedMaximum && travelLength > 0.00001f) {
        travel *= boundedMaximum / travelLength;
    }
    return travel * saturate(activation);
}
