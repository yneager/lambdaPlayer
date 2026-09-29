Texture2D<float4> sourceFlow : register(t0);
RWTexture2D<float4> cleanedFlow : register(u0);

cbuffer Parameters : register(b0)
{
    uint flowWidth;
    uint flowHeight;
    float padding0;
    float padding1;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= flowWidth || id.y >= flowHeight) return;
    int2 center = int2(id.xy);
    float4 centerFlow = sourceFlow.Load(int3(center, 0));

    // Preserve a well-supported match. Repair only uncertain blocks, choosing
    // the neighbour vector with the smallest confidence-weighted L1 cost.
    if (centerFlow.z >= 0.55)
    {
        cleanedFlow[id.xy] = centerFlow;
        return;
    }

    float4 candidates[9];
    int2 positions[9];
    uint count = 0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 p = clamp(center + int2(x, y), int2(0, 0), int2(flowWidth - 1, flowHeight - 1));
            float4 candidate = sourceFlow.Load(int3(p, 0));
            if (candidate.z > 0.15)
            {
                candidates[count] = candidate;
                positions[count] = int2(x, y);
                ++count;
            }
        }
    }

    if (count == 0)
    {
        cleanedFlow[id.xy] = float4(0.0, 0.0, 0.0, centerFlow.w);
        return;
    }

    float bestCost = 3.402823466e+38;
    float4 selected = candidates[0];
    [loop]
    for (uint i = 0; i < count; ++i)
    {
        float cost = 0.0;
        [loop]
        for (uint j = 0; j < count; ++j)
        {
            float spatial = 1.0 / (1.0 + length(float2(positions[i] - positions[j])));
            float reliability = 0.25 + 0.75 * candidates[j].z;
            cost += length(candidates[i].xy - candidates[j].xy) * spatial * reliability;
        }
        if (cost < bestCost)
        {
            bestCost = cost;
            selected = candidates[i];
        }
    }

    selected.z *= 0.85;
    cleanedFlow[id.xy] = selected;
}
