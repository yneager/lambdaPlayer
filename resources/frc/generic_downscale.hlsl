Texture2D<float4> sourceFrame : register(t0);
RWTexture2D<float4> motionFrame : register(u0);
SamplerState linearClamp : register(s0);

cbuffer Parameters : register(b0)
{
    uint inputWidth;
    uint inputHeight;
    uint outputWidth;
    uint outputHeight;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outputWidth || id.y >= outputHeight) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(outputWidth, outputHeight);
    float2 tap = 0.5 / float2(outputWidth, outputHeight);
    float4 color = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            color += sourceFrame.SampleLevel(linearClamp, uv + float2(x, y) * tap, 0);
        }
    }
    motionFrame[id.xy] = color / 9.0;
}
