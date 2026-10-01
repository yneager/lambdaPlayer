// One small GPU reduction per source pair, shared by block and neural modes.
Texture2D<float4> a : register(t0);
Texture2D<float4> b : register(t1);
SamplerState linearClamp : register(s0);
RWStructuredBuffer<float> result : register(u0);
cbuffer Op : register(b0) { uint4 p0; uint4 p1; uint4 p2; uint4 p3; float4 f0; float4 f1; };
groupshared float4 sums[256];
groupshared float4 structure[256];
groupshared float4 correspondence[256];
groupshared float4 reverseStructure[256];
groupshared float4 faintStructure[256];
groupshared float4 reverseFaintStructure[256];
groupshared float4 motionEvidence[256];
groupshared float strongDetail[256];
groupshared uint histogramA[64];
groupshared uint histogramB[64];
groupshared float histogramDelta[256];

// Compare a small spatial patch against nearby patches in the incoming shot.
// Allow translation, but reject unrelated structure even with the same palette.
float3 patch(Texture2D<float4> image, float2 uv) {
    float2 d = float2(0.35 / 64.0, 0.35 / 36.0);
    return (image.SampleLevel(linearClamp,uv+float2(-d.x,-d.y),0).rgb
          + image.SampleLevel(linearClamp,uv+float2(d.x,-d.y),0).rgb
          + image.SampleLevel(linearClamp,uv+float2(-d.x,d.y),0).rgb
          + image.SampleLevel(linearClamp,uv+d,0).rgb) * 0.25;
}


// Hue and perceptual brightness remain stable under camera movement. Compare
// the whole shot so a small moving/shared region cannot veto a genuine cut.
uint colorBin(float3 rgb) {
    float hi=max(rgb.r,max(rgb.g,rgb.b));
    float lo=min(rgb.r,min(rgb.g,rgb.b));
    float chroma=hi-lo;
    float hue=0;
    uint h=7; // low-saturation / neutral colors
    if (chroma>0.012 && chroma>hi*0.15) {
        if (hi==rgb.r) hue=(rgb.g-rgb.b)/chroma;
        else if (hi==rgb.g) hue=2.0+(rgb.b-rgb.r)/chroma;
        else hue=4.0+(rgb.r-rgb.g)/chroma;
        h=min(6u,(uint)(frac(hue/6.0)*7.0));
    }
    uint light=min(7u,(uint)(sqrt(saturate(dot(rgb,float3(0.2126,0.7152,0.0722))))*8.0));
    return h*8+light;
}

float3 matchPatch(Texture2D<float4> target, float2 uv, float3 a0, float3 a1, float3 a2, float3 a3) {
    float2 radius = 1.0/float2(64,36);
    float best = 1.0;
    float2 bestOffset = 0;
    // Coarse-to-fine matching tolerates fast pans and moving objects.
    // Raw pixel changes alone cannot distinguish movement from a cut.
    for (int dy=-12;dy<=12;++dy) for (int dx=-12;dx<=12;++dx) {
        float2 offset=float2(dx,dy)/float2(64,36);
        float2 center=uv+offset;
        if (any(center-radius<0) || any(center+radius>1)) continue;
        float3 residual = (abs(a0-patch(target,center-radius))
        +abs(a1-patch(target,center+float2(radius.x,-radius.y)))
        +abs(a2-patch(target,center+float2(-radius.x,radius.y)))
        +abs(a3-patch(target,center+radius))) * 0.25;
        float distance=dot(residual,1.0/3.0);
        if (distance<best) { best=distance; bestOffset=offset; }
    }
    float2 fineOffset = bestOffset;
    for (int fy=-4;fy<=4;++fy) for (int fx=-4;fx<=4;++fx) {
        float2 center=uv+bestOffset+float2(fx,fy)*0.25/float2(64,36);
        if (any(center-radius<0) || any(center+radius>1)) continue;
        float3 residual = (abs(a0-patch(target,center-radius))
        +abs(a1-patch(target,center+float2(radius.x,-radius.y)))
        +abs(a2-patch(target,center+float2(-radius.x,radius.y)))
        +abs(a3-patch(target,center+radius))) * 0.25;
        float distance=dot(residual,1.0/3.0);
        if (distance<best) { best=distance; fineOffset=bestOffset+float2(fx,fy)*0.25/float2(64,36); }
    }
    return float3(best, fineOffset*float2(64,36));
}

[numthreads(256,1,1)]
void main(uint gi : SV_GroupIndex) {
    if (gi<64) { histogramA[gi]=0; histogramB[gi]=0; }
    GroupMemoryBarrierWithGroupSync();
    float4 sum = 0;
    // Fixed 64x36 footprint: independent of source/output resolution.
    for (uint i=gi; i<2304; i+=256) {
        float2 uv=(float2(i%64,i/64)+0.5)/float2(64,36);
        float3 x=a.SampleLevel(linearClamp,uv,0).rgb;
        float3 y=b.SampleLevel(linearClamp,uv,0).rgb;
        InterlockedAdd(histogramA[colorBin(x)],1);
        InterlockedAdd(histogramB[colorBin(y)],1);
        float3 d=abs(x-y);
        float delta=max(d.r,max(d.g,d.b));
        float light=max(max(x.r,y.r),max(max(x.g,y.g),max(x.b,y.b)));
        sum += float4(dot(d,1.0/3.0),delta/max(0.04,light),delta>0.04 ? 1.0 : 0.0,0);
    }
    float4 evidence = 0;
    float active = 0;
    float4 reverseEvidence = 0;
    float4 reverseFaint = 0;
    float4 motion = 0;
    float strong = 0;
    if (gi < 144) {
        float2 uv = (float2(gi%16,gi/16)+0.5)/float2(16,9);
        float2 radius = 1.0/float2(64,36);
        float3 a0=patch(a,uv-radius), a1=patch(a,uv+float2(radius.x,-radius.y));
        float3 a2=patch(a,uv+float2(-radius.x,radius.y)), a3=patch(a,uv+radius);
        float3 b0=patch(b,uv-radius), b1=patch(b,uv+float2(radius.x,-radius.y));
        float3 b2=patch(b,uv+float2(-radius.x,radius.y)), b3=patch(b,uv+radius);
        float detailA=dot(abs(a0-a1)+abs(a0-a2)+abs(a0-a3),1.0/9.0);
        float detailB=dot(abs(b0-b1)+abs(b0-b2)+abs(b0-b3),1.0/9.0);
        active = detailA>0.003 ? 1.0 : 0.0;
        strong = detailA>0.012 ? 1.0 : 0.0;
        float3 match = matchPatch(b,uv,a0,a1,a2,a3);
        float best = match.x;
        float reverseBest = matchPatch(a,uv,b0,b1,b2,b3).x;
        // Consistent displacement among detailed patches confirms a pan even
        // when newly exposed edges lack correspondence in one direction.
        motion=float4(match.yz,dot(match.yz,match.yz),1.0)
            *(detailA>0.012 && best<0.015 ? 1.0 : 0.0);
        float reverseBrightness=max(0.04,dot((b0+b1+b2+b3)*0.25,1.0/3.0));
        reverseEvidence=float4(reverseBest,reverseBest/reverseBrightness,reverseBest>0.015?1.0:0.0,1.0)
            *(detailB>0.012?1.0:0.0);
        reverseFaint=float4(reverseBest,reverseBest/reverseBrightness,reverseBest>0.004?1.0:0.0,1.0)
            *(detailB>0.003?1.0:0.0);
        float brightness = max(0.04, dot((a0+a1+a2+a3)*0.25,1.0/3.0));
        evidence = float4(best, best/brightness, best>0.004 ? 1.0 : 0.0,1.0);
    }
    faintStructure[gi]=evidence*active;
    structure[gi]=float4(evidence.xy,evidence.x>0.015?1.0:0.0,evidence.w)*strong;
    reverseFaintStructure[gi]=reverseFaint;
    motionEvidence[gi]=motion;
    strongDetail[gi]=strong;
    reverseStructure[gi]=reverseEvidence;
    correspondence[gi]=evidence;
    sums[gi]=sum;
    GroupMemoryBarrierWithGroupSync();
    histogramDelta[gi]=0;
    if (gi<64) histogramDelta[gi]=abs(float(histogramA[gi])-float(histogramB[gi]));
    GroupMemoryBarrierWithGroupSync();
    for (uint stride=128; stride>0; stride>>=1) {
        if(gi<stride) { histogramDelta[gi]+=histogramDelta[gi+stride]; sums[gi]+=sums[gi+stride]; structure[gi]+=structure[gi+stride]; correspondence[gi]+=correspondence[gi+stride]; reverseStructure[gi]+=reverseStructure[gi+stride]; motionEvidence[gi]+=motionEvidence[gi+stride]; strongDetail[gi]+=strongDetail[gi+stride]; faintStructure[gi]+=faintStructure[gi+stride]; reverseFaintStructure[gi]+=reverseFaintStructure[gi+stride]; }
        GroupMemoryBarrierWithGroupSync();
    }
    if(gi==0) {
        float3 m=sums[0].xyz/2304.0;
        result[0]=m.x;
        // RGB catches equally bright color cuts; relative change catches dark shots.
        float3 unmatched=structure[0].xyz/max(1.0,structure[0].w);
        float3 reverseUnmatched=reverseStructure[0].xyz/max(1.0,reverseStructure[0].w);
        bool reverseCut=reverseStructure[0].w>=12 && m.x>=0.012 && reverseUnmatched.x>=0.020
            && (reverseUnmatched.y>=0.12 || reverseUnmatched.x>=0.045) && reverseUnmatched.z>=0.6;
        bool structureCut = structure[0].w>=12 && m.x>=0.012 && unmatched.x>=0.020
            && (unmatched.y>=0.12 || unmatched.x>=0.045) && unmatched.z>=0.6;
        float3 faint=faintStructure[0].xyz/max(1.0,faintStructure[0].w);
        float3 reverseFaintMean=reverseFaintStructure[0].xyz/max(1.0,reverseFaintStructure[0].w);
        bool faintCut=m.x>=0.012 && m.x<0.05
            && ((faintStructure[0].w>=12 && faint.x>=0.006 && faint.y>=0.06 && faint.z>=0.6)
                || (reverseFaintStructure[0].w>=12 && reverseFaintMean.x>=0.006
                    && reverseFaintMean.y>=0.06 && reverseFaintMean.z>=0.6));
        float3 allUnmatched=correspondence[0].xyz/144.0;
        bool noCorrespondence = allUnmatched.x>=0.020 && allUnmatched.z>=0.6;
        float2 meanMotion=motionEvidence[0].xy/max(1.0,motionEvidence[0].w);
        float variance=motionEvidence[0].z/max(1.0,motionEvidence[0].w)-dot(meanMotion,meanMotion);
        bool coherentMotion=motionEvidence[0].w>=12 && motionEvidence[0].w>=strongDetail[0]*0.35
            && variance<=4.0 && dot(meanMotion,meanMotion)>2.25;
        float colorDistance=histogramDelta[0]/4608.0; // total variation, 0..1
        bool colorCut=m.x>=0.025 && colorDistance>=0.50;
        result[1]=f0.x>0 && (colorCut || (!coherentMotion && ((m.x>=f0.x && noCorrespondence)
            || (m.x>=0.02 && m.y>=0.55 && m.z>=0.6 && noCorrespondence)
            || structureCut || reverseCut || faintCut))) ? 1.0 : 0.0;
    }
}
