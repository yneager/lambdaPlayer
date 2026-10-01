Texture2D<float4> previousFrame : register(t0);
Texture2D<float4> currentFrame : register(t1);
Texture2D<float4> forwardFlow : register(t2);
Texture2D<float4> backwardFlow : register(t3);
StructuredBuffer<float> scene : register(t4);
RWTexture2D<float4> outputFrame : register(u0);
SamplerState linearClamp : register(s0);

cbuffer Parameters : register(b0)
{
    uint frameWidth;
    uint frameHeight;
    uint motionWidth;
    uint motionHeight;
    uint blockSize;
    float interpolationTime;
    float padding0;
    float padding1;
};

float4 sampleFlow(Texture2D<float4> field, float2 pixel)
{
    float2 uv = (clamp(pixel, 0.0, float2(frameWidth - 1, frameHeight - 1)) + 0.5)
                / float2(frameWidth, frameHeight);
    return field.SampleLevel(linearClamp, uv, 0);
}

float4 sampleFrame(Texture2D<float4> frame, float2 pixel)
{
    float2 uv = (clamp(pixel, 0.0, float2(frameWidth - 1, frameHeight - 1)) + 0.5)
                / float2(frameWidth, frameHeight);
    return frame.SampleLevel(linearClamp, uv, 0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= frameWidth || id.y >= frameHeight) return;
    if (padding0 > 0 && scene[1] > 0.5) {
        // Hold the preceding shot until the incoming source frame's timestamp.
        outputFrame[id.xy] = previousFrame.Load(int3(id.xy,0));
        return;
    }
    float2 p = float2(id.xy);
    float2 motionScale = float2(frameWidth, frameHeight) / float2(motionWidth, motionHeight);
    float4 forward = sampleFlow(forwardFlow, p);
    float4 backward = sampleFlow(backwardFlow, p);

    // Forward/backward agreement rejects occlusion boundaries and ambiguous
    // block matches. It is evaluated in reduced-resolution motion pixels.
    float2 reverseAtForward = sampleFlow(backwardFlow, p + forward.xy * motionScale).xy;
    float2 forwardAtReverse = sampleFlow(forwardFlow, p + backward.xy * motionScale).xy;
    float toleranceForward = 1.0 + 0.15 * length(forward.xy);
    float toleranceBackward = 1.0 + 0.15 * length(backward.xy);
    float consistencyForward = saturate(1.0 - length(forward.xy + reverseAtForward) / toleranceForward);
    float consistencyBackward = saturate(1.0 - length(backward.xy + forwardAtReverse) / toleranceBackward);
    float forwardConfidence = forward.z * consistencyForward;
    float backwardConfidence = backward.z * consistencyBackward;

    // A large best-match residual in both directions is a scene cut or a
    // pair with no meaningful correspondence. Show the nearer real frame.
    if (forward.w > 0.30 && backward.w > 0.30)
    {
        outputFrame[id.xy] = interpolationTime < 0.5
            ? previousFrame.Load(int3(id.xy, 0))
            : currentFrame.Load(int3(id.xy, 0));
        return;
    }

    float2 previousPosition = p - forward.xy * motionScale * forwardConfidence * interpolationTime;
    float2 currentPosition = p - backward.xy * motionScale * backwardConfidence * (1.0 - interpolationTime);
    float4 a = sampleFrame(previousFrame, previousPosition);
    float4 b = sampleFrame(currentFrame, currentPosition);

    // Respect arbitrary output timestamps and favor the direction with the
    // stronger match near occlusions. If both matches fail, this degrades to
    // a stable temporal blend with no untrusted motion warp.
    float weightA = (1.0 - interpolationTime) * forwardConfidence;
    float weightB = interpolationTime * backwardConfidence;
    float weightSum = weightA + weightB;
    if (weightSum <= 1e-4)
    {
        weightA = 1.0 - interpolationTime;
        weightB = interpolationTime;
        weightSum = 1.0;
    }
    outputFrame[id.xy] = (a * weightA + b * weightB) / weightSum;
}
