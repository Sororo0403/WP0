#include "../mesh/Mesh.hlsli"
#include "../mesh/MeshSceneParams.hlsli"

float GridCoverage(float2 position, float spacing)
{
    const float2 coordinate = position / spacing;
    const float2 derivative = max(fwidth(coordinate), float2(0.0001f, 0.0001f));
    const float2 distanceToLine = abs(frac(coordinate - 0.5f) - 0.5f) / derivative;
    return 1.0f - saturate(min(distanceToLine.x, distanceToLine.y));
}

float4 main(MeshVSOutput input) : SV_TARGET
{
    const float2 position = input.worldPos.xz;
    const float minor = GridCoverage(position, 1.0f);
    const float major = GridCoverage(position, 5.0f);
    const float distanceFade = saturate(1.0f - distance(cameraPos.xz, position) / 70.0f);

    float3 color = lerp(float3(0.28f, 0.31f, 0.37f),
                        float3(0.48f, 0.51f, 0.58f), major);
    float alpha = max(minor * 0.20f, major * 0.42f) * distanceFade;

    const float2 axisDerivative = max(fwidth(position), float2(0.0001f, 0.0001f));
    const float xAxis = 1.0f - saturate(abs(position.y) / axisDerivative.y);
    const float zAxis = 1.0f - saturate(abs(position.x) / axisDerivative.x);
    color = lerp(color, float3(0.78f, 0.25f, 0.20f), xAxis);
    color = lerp(color, float3(0.20f, 0.38f, 0.78f), zAxis);
    alpha = max(alpha, max(xAxis, zAxis) * 0.72f * distanceFade);

    if (alpha < 0.003f)
    {
        discard;
    }
    return float4(color, alpha);
}
