Texture2D<float4> frameA : register(t0);
Texture2D<float4> frameB : register(t1);
RWTexture2D<float4> flow : register(u0);

cbuffer Parameters : register(b0)
{
    uint frameWidth;
    uint frameHeight;
    uint motionWidth;
    uint motionHeight;
    uint blockSize;
    uint searchRadius;
};

float luma(float3 rgb)
{
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 blocks = uint2((motionWidth + blockSize - 1) / blockSize,
                         (motionHeight + blockSize - 1) / blockSize);
    if (id.x >= blocks.x || id.y >= blocks.y) return;

    int2 origin = int2(id.xy * blockSize);
    float bestScore = 2.0;
    float secondScore = 2.0;
    float bestSad = 1.0;
    int2 bestOffset = int2(0, 0);
    const int displacementStep = 1;
    const int sampleStep = 2;
    const int sampleOffset = 1;

    [loop]
    for (int dy = -int(searchRadius); dy <= int(searchRadius); dy += displacementStep)
    {
        [loop]
        for (int dx = -int(searchRadius); dx <= int(searchRadius); dx += displacementStep)
        {
            float cost = 0.0;
            uint samples = 0;
            [loop]
            for (int sy = sampleOffset; sy < int(blockSize); sy += sampleStep)
            {
                [loop]
                for (int sx = sampleOffset; sx < int(blockSize); sx += sampleStep)
                {
                    int2 p = origin + int2(sx, sy);
                    int2 q = p + int2(dx, dy);
                    if (p.x >= int(motionWidth) || p.y >= int(motionHeight) ||
                        q.x < 0 || q.y < 0 || q.x >= int(motionWidth) || q.y >= int(motionHeight))
                    {
                        cost += 1.0;
                    }
                    else
                    {
                        float a = luma(frameA.Load(int3(p, 0)).rgb);
                        float b = luma(frameB.Load(int3(q, 0)).rgb);
                        cost += abs(a - b);
                    }
                    ++samples;
                }
            }

            float sad = cost / max(float(samples), 1.0);
            // Prefer the smallest motion when the image provides no evidence
            // that distinguishes candidates (flat/repetitive areas). This
            // prevents equal-cost ties from selecting the first, farthest
            // offset in the search window.
            float score = sad;
            if (score < bestScore - 1e-6 ||
                (abs(score - bestScore) <= 1e-6 && dot(float2(dx, dy), float2(dx, dy)) < dot(float2(bestOffset), float2(bestOffset))))
            {
                secondScore = bestScore;
                bestScore = score;
                bestSad = sad;
                bestOffset = int2(dx, dy);
            }
            else if (score < secondScore)
            {
                secondScore = score;
            }
        }
    }

    // Low uniqueness means the block is ambiguous; its vector is retained
    // for diagnosis but will be suppressed by the cleanup/warp stages.
    float uniqueness = saturate((secondScore - bestScore - 0.003) / 0.05);
    flow[id.xy] = float4(float2(bestOffset), uniqueness, bestSad);
}
