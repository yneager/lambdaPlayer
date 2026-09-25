Texture2D<float4> sourceFrame : register(t0);
RWTexture2D<float4> outputFrame : register(u0);

cbuffer Parameters : register(b0)
{
    uint frameWidth;
    uint frameHeight;
    uint blockSize;
    float unusedValue;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= frameWidth || id.y >= frameHeight) return;
    outputFrame[id.xy] = sourceFrame.Load(int3(id.xy, 0));
}
