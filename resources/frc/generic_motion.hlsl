Texture2D<float4> frameA : register(t0);
Texture2D<float4> frameB : register(t1);
RWStructuredBuffer<int2> flow : register(u0);

cbuffer Parameters : register(b0)
{
    uint frameWidth;
    uint frameHeight;
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
    uint2 blocks = uint2((frameWidth + blockSize - 1) / blockSize,
                         (frameHeight + blockSize - 1) / blockSize);
    if (id.x >= blocks.x || id.y >= blocks.y) return;

    int2 origin = int2(id.xy * blockSize);
    int bestCost = 0x7fffffff;
    int2 bestOffset = int2(0, 0);
    const int displacementStep = 4;
    const int sampleStep = 8;

    [loop]
    for (int dy = -int(searchRadius); dy <= int(searchRadius); dy += displacementStep)
    {
        [loop]
        for (int dx = -int(searchRadius); dx <= int(searchRadius); dx += displacementStep)
        {
            int cost = 0;
            int samples = 0;
            [loop]
            for (int sy = 4; sy < int(blockSize); sy += sampleStep)
            {
                [loop]
                for (int sx = 4; sx < int(blockSize); sx += sampleStep)
                {
                    int2 p = origin + int2(sx, sy);
                    int2 q = p + int2(dx, dy);
                    if (p.x >= int(frameWidth) || p.y >= int(frameHeight) ||
                        q.x < 0 || q.y < 0 || q.x >= int(frameWidth) || q.y >= int(frameHeight))
                    {
                        cost += 255;
                    }
                    else
                    {
                        float a = luma(frameA.Load(int3(p, 0)).rgb);
                        float b = luma(frameB.Load(int3(q, 0)).rgb);
                        cost += int(abs(a - b) * 255.0);
                    }
                    ++samples;
                }
            }
            if (cost < bestCost)
            {
                bestCost = cost;
                bestOffset = int2(dx, dy);
            }
        }
    }

    flow[id.y * blocks.x + id.x] = bestOffset;
}
