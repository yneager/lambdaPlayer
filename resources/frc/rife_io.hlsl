// Glue between LAMBDA's full-resolution D3D11 frames and the reduced-size RIFE
// flownet tensors (see rife_ops.hlsl for the tensor layout).
//
//  prepare     full-resolution texture -> area-filtered analysis tensor
//              (3 channels, zero padded to the network's alignment)
//  sceneDiff   mean absolute luma difference of the two analysis tensors,
//              written to dst[0] (read by synthesize on the GPU, no readback)
//  synthesize  RIFE's final step at output resolution: the analysis flow and
//              mask are bilinearly upsampled, the flow is rescaled to output
//              pixels, and the ORIGINAL full-resolution frames are warped and
//              blended: out = warp(A, flowA) * mask + warp(B, flowB) * (1 - mask)

cbuffer Op : register(b0)
{
    uint4 p0;
    uint4 p1;
    uint4 p2;
    uint4 p3;
    float4 f0;
    float4 f1;
};

SamplerState linearClamp : register(s0);

// ---- prepare ---------------------------------------------------------------
// p0 = (source W, H, content W, H), p1 = (padded W, H, taps per axis, dst off)
Texture2D<float4> frame : register(t0);
RWStructuredBuffer<float> tensor : register(u0);

[numthreads(8, 8, 1)]
void prepare(uint3 id : SV_DispatchThreadID)
{
    const uint padW = p1.x, padH = p1.y;
    if (id.x >= padW || id.y >= padH) return;
    float3 value = 0.0;
    if (id.x < p0.z && id.y < p0.w)
    {
        // Each bilinear tap averages 2x2 source texels; `taps` evenly spaced
        // taps per axis approximate an area (box) filter over the footprint.
        const float2 sourceSize = float2(p0.xy);
        const float2 scale = sourceSize / float2(p0.zw);
        const uint taps = p1.z;
        [loop] for (uint j = 0; j < taps; ++j)
        {
            [loop] for (uint i = 0; i < taps; ++i)
            {
                const float2 position = (float2(id.xy) + (float2(i, j) + 0.5) / float(taps)) * scale;
                value += frame.SampleLevel(linearClamp, position / sourceSize, 0).rgb;
            }
        }
        value /= float(taps * taps);
    }
    const uint plane = padW * padH;
    const uint index = p1.w + id.y * padW + id.x;
    tensor[index] = value.r;
    tensor[index + plane] = value.g;
    tensor[index + 2 * plane] = value.b;
}

// ---- sceneDiff -------------------------------------------------------------
// p0 = (content W, H, padded W, H), p1 = (tensor A off, tensor B off)
StructuredBuffer<float> tensorA : register(t0);
StructuredBuffer<float> tensorB : register(t1);
RWStructuredBuffer<float> result : register(u0);
groupshared float partialSums[256];

float lumaAt(StructuredBuffer<float> t, uint off, uint plane, uint index)
{
    return dot(float3(t[off + index], t[off + index + plane], t[off + index + 2 * plane]),
               float3(0.2126, 0.7152, 0.0722));
}

// Every 4th pixel in each direction; one group so the mean needs no second
// pass (about 32k samples at 960x540).
[numthreads(256, 1, 1)]
void sceneDiff(uint gi : SV_GroupIndex)
{
    const uint w = (p0.x + 3) / 4, h = (p0.y + 3) / 4, padW = p0.z, plane = p0.z * p0.w;
    const uint count = w * h;
    float sum = 0.0;
    for (uint i = gi; i < count; i += 256)
    {
        const uint y = i / w;
        const uint index = (y * 4) * padW + (i - y * w) * 4;
        sum += abs(lumaAt(tensorA, p1.x, plane, index) - lumaAt(tensorB, p1.y, plane, index));
    }
    partialSums[gi] = sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128; s > 0; s >>= 1)
    {
        if (gi < s) partialSums[gi] += partialSums[gi + s];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) result[0] = partialSums[0] / float(max(count, 1u));
}

// ---- packMotion --------------------------------------------------------------
// Copies the final flow pair and mask into analysis-size float textures so the
// full-resolution pass can use hardware bilinear filtering.
// p0 = (analysis content W, H, padded W, H), p1 = (flowA off, flowB off, mask off)
StructuredBuffer<float> flowA : register(t0);
StructuredBuffer<float> flowB : register(t1);
StructuredBuffer<float> maskTensor : register(t2);
RWTexture2D<float4> motionOut : register(u0);
RWTexture2D<float> maskOut : register(u1);

[numthreads(8, 8, 1)]
void packMotion(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= p0.x || id.y >= p0.y) return;
    const uint plane = p0.z * p0.w;
    const uint index = id.y * p0.z + id.x;
    motionOut[id.xy] = float4(flowA[p1.x + index], flowA[p1.x + plane + index],
                              flowB[p1.y + index], flowB[p1.y + plane + index]);
    maskOut[id.xy] = maskTensor[p1.z + index];
}

// ---- synthesize --------------------------------------------------------------
// p0 = (output W, H, analysis content W, H), p2.y = scene check enabled,
// f0 = (t, scene threshold). The motion/mask textures cover exactly the
// analysis content, so the output pixel's own uv addresses them (bilinear,
// edge clamped) and flow is rescaled from analysis to output pixels.
Texture2D<float4> frameA : register(t0);
Texture2D<float4> frameB : register(t1);
Texture2D<float4> motion : register(t2);
Texture2D<float> maskTexture : register(t3);
StructuredBuffer<float> sceneBuffer : register(t4);
RWTexture2D<float4> output : register(u0);

[numthreads(8, 8, 1)]
void synthesize(uint3 id : SV_DispatchThreadID)
{
    const uint2 size = p0.xy;
    if (id.x >= size.x || id.y >= size.y) return;
    const float t = f0.x;
    if (p2.y != 0 && sceneBuffer[0] > f0.y)
    {
        // Scene cut: never blend unrelated pictures; show the nearer frame.
        const float3 nearer = t < 0.5 ? frameA.Load(int3(id.xy, 0)).rgb : frameB.Load(int3(id.xy, 0)).rgb;
        output[id.xy] = float4(nearer, 1.0);
        return;
    }
    const float2 invSize = 1.0 / float2(size);
    const float2 center = float2(id.xy) + 0.5;
    const float2 uv = center * invSize;
    const float2 toOutput = float2(size) / float2(p0.zw);
    const float4 flow = motion.SampleLevel(linearClamp, uv, 0);
    const float mask = maskTexture.SampleLevel(linearClamp, uv, 0);
    const float3 a = frameA.SampleLevel(linearClamp, (center + flow.xy * toOutput) * invSize, 0).rgb;
    const float3 b = frameB.SampleLevel(linearClamp, (center + flow.zw * toOutput) * invSize, 0).rgb;
    output[id.xy] = float4(a * mask + b * (1.0 - mask), 1.0);
}
