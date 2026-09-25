Texture2D<float4> previousFrame : register(t0);
Texture2D<float4> currentFrame : register(t1);
StructuredBuffer<int2> forwardFlow : register(t2);
StructuredBuffer<int2> backwardFlow : register(t3);
RWTexture2D<float4> outputFrame : register(u0);
SamplerState linearClamp : register(s0);

cbuffer Parameters : register(b0)
{
    uint frameWidth;
    uint frameHeight;
    uint blockSize;
    float interpolationTime;
};

float2 sampleFlow(StructuredBuffer<int2> field, float2 pixel)
{
    float2 p = pixel / float(blockSize) - 0.5;
    int2 dimensions = int2((frameWidth + blockSize - 1) / blockSize,
                           (frameHeight + blockSize - 1) / blockSize);
    int2 lo = clamp(int2(floor(p)), int2(0, 0), dimensions - 1);
    int2 hi = min(lo + 1, dimensions - 1);
    float2 f = saturate(p - floor(p));
    float2 a = float2(field[lo.y * dimensions.x + lo.x]);
    float2 b = float2(field[lo.y * dimensions.x + hi.x]);
    float2 c = float2(field[hi.y * dimensions.x + lo.x]);
    float2 d = float2(field[hi.y * dimensions.x + hi.x]);
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= frameWidth || id.y >= frameHeight) return;
    float2 p = float2(id.xy);
    float2 fwd = sampleFlow(forwardFlow, p);
    float2 back = sampleFlow(backwardFlow, p);
    float2 prevPos = p - fwd * interpolationTime;
    float2 currPos = p - back * (1.0 - interpolationTime);
    float2 maxPos = float2(frameWidth - 1, frameHeight - 1);
    float2 uvPrev = (clamp(prevPos, 0.0, maxPos) + 0.5) / float2(frameWidth, frameHeight);
    float2 uvCurr = (clamp(currPos, 0.0, maxPos) + 0.5) / float2(frameWidth, frameHeight);
    float4 a = previousFrame.SampleLevel(linearClamp, uvPrev, 0);
    float4 b = currentFrame.SampleLevel(linearClamp, uvCurr, 0);
    outputFrame[id.xy] = lerp(a, b, 0.5);
}
