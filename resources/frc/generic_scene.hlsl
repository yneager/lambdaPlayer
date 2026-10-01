// One small GPU reduction per source pair, shared by block and neural modes.
Texture2D<float4> a : register(t0);
Texture2D<float4> b : register(t1);
SamplerState linearClamp : register(s0);
RWStructuredBuffer<float> result : register(u0);
cbuffer Op : register(b0) { uint4 p0; uint4 p1; uint4 p2; uint4 p3; float4 f0; float4 f1; };
groupshared float4 sums[256];
groupshared float4 structure[256];

// Compare a small spatial patch against nearby patches in the incoming shot.
// Allow translation, but reject unrelated structure even with the same palette.
float3 patch(Texture2D<float4> image, float2 uv) {
    float2 d = float2(0.35 / 64.0, 0.35 / 36.0);
    return (image.SampleLevel(linearClamp,uv+float2(-d.x,-d.y),0).rgb
          + image.SampleLevel(linearClamp,uv+float2(d.x,-d.y),0).rgb
          + image.SampleLevel(linearClamp,uv+float2(-d.x,d.y),0).rgb
          + image.SampleLevel(linearClamp,uv+d,0).rgb) * 0.25;
}

[numthreads(256,1,1)]
void main(uint gi : SV_GroupIndex) {
    float4 sum = 0;
    // Fixed 64x36 footprint: independent of source/output resolution.
    for (uint i=gi; i<2304; i+=256) {
        float2 uv=(float2(i%64,i/64)+0.5)/float2(64,36);
        float3 x=a.SampleLevel(linearClamp,uv,0).rgb;
        float3 y=b.SampleLevel(linearClamp,uv,0).rgb;
        float3 d=abs(x-y);
        float delta=max(d.r,max(d.g,d.b));
        float light=max(max(x.r,y.r),max(max(x.g,y.g),max(x.b,y.b)));
        sum += float4(dot(d,1.0/3.0),delta/max(0.04,light),delta>0.04 ? 1.0 : 0.0,0);
    }
    float4 evidence = 0;
    if (gi < 144) {
        float2 uv = (float2(gi%16,gi/16)+0.5)/float2(16,9);
        float2 radius = 1.0/float2(64,36);
        float3 a0=patch(a,uv-radius), a1=patch(a,uv+float2(radius.x,-radius.y));
        float3 a2=patch(a,uv+float2(-radius.x,radius.y)), a3=patch(a,uv+radius);
        float best = 1.0;
        for (int dy=-2;dy<=2;++dy) for (int dx=-2;dx<=2;++dx) {
            float2 center=uv+float2(dx,dy)/float2(64,36);
            float3 residual = (abs(a0-patch(b,center-radius))
                +abs(a1-patch(b,center+float2(radius.x,-radius.y)))
                +abs(a2-patch(b,center+float2(-radius.x,radius.y)))
                +abs(a3-patch(b,center+radius))) * 0.25;
            best=min(best,dot(residual,1.0/3.0));
        }
        float3 b0=patch(b,uv-radius), b1=patch(b,uv+float2(radius.x,-radius.y));
        float3 b2=patch(b,uv+float2(-radius.x,radius.y)), b3=patch(b,uv+radius);
        float detailA=dot(abs(a0-a1)+abs(a0-a2)+abs(a0-a3),1.0/9.0);
        float detailB=dot(abs(b0-b1)+abs(b0-b2)+abs(b0-b3),1.0/9.0);
        float active = max(detailA,detailB)>0.012 ? 1.0 : 0.0;
        float brightness = max(0.04, dot((a0+a1+a2+a3)*0.25,1.0/3.0));
        evidence = float4(best, best/brightness, best>0.015 ? 1.0 : 0.0,1.0)*active;
    }
    structure[gi]=evidence;
    sums[gi]=sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride=128; stride>0; stride>>=1) {
        if(gi<stride) { sums[gi]+=sums[gi+stride]; structure[gi]+=structure[gi+stride]; }
        GroupMemoryBarrierWithGroupSync();
    }
    if(gi==0) {
        float3 m=sums[0].xyz/2304.0;
        result[0]=m.x;
        // RGB catches equally bright color cuts; relative change catches dark shots.
        float3 unmatched=structure[0].xyz/max(1.0,structure[0].w);
        bool structureCut = structure[0].w>=22 && m.x>=0.012 && unmatched.x>=0.020
            && (unmatched.y>=0.12 || unmatched.x>=0.045) && unmatched.z>=0.6;
        result[1]=f0.x>0 && (m.x>=f0.x || (m.x>=0.02 && m.y>=0.55 && m.z>=0.6)
            || structureCut) ? 1.0 : 0.0;
    }
}
