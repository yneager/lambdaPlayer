// D3D11 compute kernels that execute RIFE v4.x flownet graphs stored in the
// ncnn param/bin format. Tensors are float32, channel-major (C, H, W) and live
// in structured buffers; `off` is the element offset of channel 0, so channel
// slices of a tensor are plain offset views. Layer semantics follow ncnn's
// reference CPU layers (Convolution, Deconvolution, Interp, BinaryOp,
// Eltwise, PixelShuffle, ReLU, Sigmoid, Concat) and the rife.Warp custom
// layer (bilinear grid sample, border clamp, align_corners=True).

cbuffer Op : register(b0)
{
    uint4 p0;
    uint4 p1;
    uint4 p2;
    uint4 p3;
    float4 f0;
    float4 f1;
};

#define NONE 0xffffffffu

StructuredBuffer<float> src0 : register(t0);
StructuredBuffer<float> src1 : register(t1);
StructuredBuffer<float> src2 : register(t2);
RWStructuredBuffer<float> dst : register(u0);

uint at(uint off, uint w, uint h, uint c, uint y, uint x)
{
    return off + (c * h + y) * w + x;
}

// ---- Convolution 3x3 (stride 1 or 2, pad 1), fused epilogue ----------------
// p0 = in (W, H, C, off)     p1 = out (W, H, C, off)
// p2 = (weights, bias, beta, residual) offsets; bias/beta in src1 (weights),
//      residual in src2 using the output's layout; NONE = absent
// p3 = (stride, activation, 0, 0), f0.x = leaky slope
// out = act((conv + bias) * beta + residual)
#define CONV_TX 16
#define CONV_TY 8
#define CONV_OCB 16
#define CONV_ICB 4
#define CONV_TILE_W ((CONV_TX - 1) * 2 + 3)
#define CONV_TILE_H ((CONV_TY - 1) * 2 + 3)

groupshared float convTile[CONV_ICB][CONV_TILE_H][CONV_TILE_W];
// Four output channels per element: one LDS read feeds four FMAs.
groupshared float4 convWeights[CONV_ICB * 9][CONV_OCB / 4];

[numthreads(CONV_TX, CONV_TY, 1)]
void conv3x3(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    const uint inW = p0.x, inH = p0.y, inC = p0.z, inOff = p0.w;
    const uint outW = p1.x, outH = p1.y, outC = p1.z, outOff = p1.w;
    const uint stride = p3.x;
    const uint tileW = (CONV_TX - 1) * stride + 3;
    const uint tileH = (CONV_TY - 1) * stride + 3;
    const int ix0 = int(gid.x * CONV_TX * stride) - 1;
    const int iy0 = int(gid.y * CONV_TY * stride) - 1;
    const uint ocBase = gid.z * CONV_OCB;

    float4 acc[CONV_OCB / 4];
    [unroll] for (uint o = 0; o < CONV_OCB / 4; ++o) acc[o] = 0.0;

    for (uint ic0 = 0; ic0 < inC; ic0 += CONV_ICB)
    {
        const uint tileSize = tileW * tileH;
        for (uint i = gi; i < CONV_ICB * tileSize; i += CONV_TX * CONV_TY)
        {
            const uint c = i / tileSize;
            const uint r = i - c * tileSize;
            const uint ty = r / tileW;
            const uint tx = r - ty * tileW;
            const int x = ix0 + int(tx);
            const int y = iy0 + int(ty);
            const uint ic = ic0 + c;
            float v = 0.0;
            if (ic < inC && x >= 0 && y >= 0 && x < int(inW) && y < int(inH))
                v = src0[at(inOff, inW, inH, ic, uint(y), uint(x))];
            convTile[c][ty][tx] = v;
        }
        for (uint j = gi; j < CONV_ICB * 9 * (CONV_OCB / 4); j += CONV_TX * CONV_TY)
        {
            const uint o4 = j % (CONV_OCB / 4);
            const uint k = j / (CONV_OCB / 4);
            const uint c = k / 9;
            const uint tap = k - c * 9;
            const uint ic = ic0 + c;
            float4 w = 0.0;
            [unroll] for (uint e = 0; e < 4; ++e)
            {
                const uint oc = ocBase + o4 * 4 + e;
                w[e] = (oc < outC && ic < inC) ? src1[p2.x + (oc * inC + ic) * 9 + tap] : 0.0;
            }
            convWeights[k][o4] = w;
        }
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint c = 0; c < CONV_ICB; ++c)
        {
            [unroll] for (uint ky = 0; ky < 3; ++ky)
            {
                [unroll] for (uint kx = 0; kx < 3; ++kx)
                {
                    const float v = convTile[c][tid.y * stride + ky][tid.x * stride + kx];
                    const uint k = c * 9 + ky * 3 + kx;
                    [unroll] for (uint o2 = 0; o2 < CONV_OCB / 4; ++o2) acc[o2] = mad(v, convWeights[k][o2], acc[o2]);
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    const uint ox = gid.x * CONV_TX + tid.x;
    const uint oy = gid.y * CONV_TY + tid.y;
    if (ox >= outW || oy >= outH) return;
    [unroll] for (uint o3 = 0; o3 < CONV_OCB; ++o3)
    {
        const uint oc = ocBase + o3;
        if (oc >= outC) continue;
        float v = acc[o3 / 4][o3 % 4];
        if (p2.y != NONE) v += src1[p2.y + oc];
        if (p2.z != NONE) v *= src1[p2.z + oc];
        if (p2.w != NONE) v += src2[at(p2.w, outW, outH, oc, oy, ox)];
        if (p3.y != 0) v = v > 0.0 ? v : v * f0.x;
        dst[at(outOff, outW, outH, oc, oy, ox)] = v;
    }
}

// ---- Deconvolution (transposed convolution) --------------------------------
// p0 = in, p1 = out, p2 = (weights, bias, kernel, stride), p3 = (pad, 0, 0, 0)
// Weights are ncnn's (outC, inC, k, k) layout.
#define DECONV_OCB 8

[numthreads(8, 8, 1)]
void deconv(uint3 id : SV_DispatchThreadID)
{
    const uint inW = p0.x, inH = p0.y, inC = p0.z, inOff = p0.w;
    const uint outW = p1.x, outH = p1.y, outC = p1.z, outOff = p1.w;
    const uint k = p2.z, stride = p2.w;
    const int pad = int(p3.x);
    if (id.x >= outW || id.y >= outH) return;
    const uint ocBase = id.z * DECONV_OCB;
    float acc[DECONV_OCB];
    [unroll] for (uint o = 0; o < DECONV_OCB; ++o) acc[o] = 0.0;
    for (uint ky = 0; ky < k; ++ky)
    {
        const int ty = int(id.y) + pad - int(ky);
        if (ty < 0 || (ty % int(stride)) != 0) continue;
        const uint iy = uint(ty) / stride;
        if (iy >= inH) continue;
        for (uint kx = 0; kx < k; ++kx)
        {
            const int tx = int(id.x) + pad - int(kx);
            if (tx < 0 || (tx % int(stride)) != 0) continue;
            const uint ix = uint(tx) / stride;
            if (ix >= inW) continue;
            for (uint ic = 0; ic < inC; ++ic)
            {
                const float v = src0[at(inOff, inW, inH, ic, iy, ix)];
                [unroll] for (uint o2 = 0; o2 < DECONV_OCB; ++o2)
                {
                    const uint oc = min(ocBase + o2, outC - 1);
                    acc[o2] = mad(v, src1[p2.x + ((oc * inC + ic) * k + ky) * k + kx], acc[o2]);
                }
            }
        }
    }
    [unroll] for (uint o3 = 0; o3 < DECONV_OCB; ++o3)
    {
        const uint oc = ocBase + o3;
        if (oc >= outC) continue;
        float v = acc[o3];
        if (p2.y != NONE) v += src1[p2.y + oc];
        dst[at(outOff, outW, outH, oc, id.y, id.x)] = v;
    }
}

// ---- Deconvolution 4x4, stride 2, padding 1 (every RIFE v4 lastconv) -------
// One thread per input cell (i, j) produces the 2x2 output quad
// (2i + py, 2j + px). With pad 1: py = 0 uses (ky 1, row i) and (ky 3, row
// i - 1); py = 1 uses (ky 0, row i + 1) and (ky 2, row i); same for x.
#define DQ_OCB 8
#define DQ_ICB 8
groupshared float deconvWeights[DQ_ICB * 16][DQ_OCB];

[numthreads(8, 8, 1)]
void deconv4x4s2(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    const uint inW = p0.x, inH = p0.y, inC = p0.z, inOff = p0.w;
    const uint outW = p1.x, outH = p1.y, outC = p1.z, outOff = p1.w;
    const int j = int(gid.x * 8 + tid.x);
    const int i = int(gid.y * 8 + tid.y);
    const uint ocBase = gid.z * DQ_OCB;
    const bool valid = j < int(inW) && i < int(inH);
    float acc[4][DQ_OCB];
    [unroll] for (uint q = 0; q < 4; ++q)
        [unroll] for (uint o = 0; o < DQ_OCB; ++o) acc[q][o] = 0.0;

    for (uint ic0 = 0; ic0 < inC; ic0 += DQ_ICB)
    {
        for (uint n = gi; n < DQ_ICB * 16 * DQ_OCB; n += 64)
        {
            const uint o = n % DQ_OCB;
            const uint k = n / DQ_OCB;
            const uint c = k / 16;
            const uint oc = ocBase + o;
            const uint ic = ic0 + c;
            deconvWeights[k][o] = (oc < outC && ic < inC) ? src1[p2.x + (oc * inC + ic) * 16 + (k - c * 16)] : 0.0;
        }
        GroupMemoryBarrierWithGroupSync();
        if (valid)
        {
            for (uint c = 0; c < DQ_ICB; ++c)
            {
                const uint ic = ic0 + c;
                if (ic >= inC) break;
                float v[3][3];
                [unroll] for (int dy = -1; dy <= 1; ++dy)
                    [unroll] for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int y = i + dy, x = j + dx;
                        v[dy + 1][dx + 1] = (x >= 0 && y >= 0 && x < int(inW) && y < int(inH))
                            ? src0[at(inOff, inW, inH, ic, uint(y), uint(x))] : 0.0;
                    }
                // (ky, row offset) pairs per output parity.
                const int2 taps[2][2] = {{int2(1, 0), int2(3, -1)}, {int2(0, 1), int2(2, 0)}};
                [unroll] for (uint py = 0; py < 2; ++py)
                    [unroll] for (uint px = 0; px < 2; ++px)
                        [unroll] for (uint ty = 0; ty < 2; ++ty)
                            [unroll] for (uint tx = 0; tx < 2; ++tx)
                            {
                                const int2 ky = taps[py][ty];
                                const int2 kx = taps[px][tx];
                                const float value = v[ky.y + 1][kx.y + 1];
                                const uint k = c * 16 + uint(ky.x) * 4 + uint(kx.x);
                                [unroll] for (uint o = 0; o < DQ_OCB; ++o)
                                    acc[py * 2 + px][o] = mad(value, deconvWeights[k][o], acc[py * 2 + px][o]);
                            }
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (!valid) return;
    [unroll] for (uint oo = 0; oo < DQ_OCB; ++oo)
    {
        const uint oc = ocBase + oo;
        if (oc >= outC) continue;
        const float bias = p2.y != NONE ? src1[p2.y + oc] : 0.0;
        [unroll] for (uint q2 = 0; q2 < 4; ++q2)
        {
            const uint oy = uint(i) * 2 + q2 / 2;
            const uint ox = uint(j) * 2 + q2 % 2;
            if (oy < outH && ox < outW) dst[at(outOff, outW, outH, oc, oy, ox)] = acc[q2][oo] + bias;
        }
    }
}

// ---- Interp (bilinear, align_corners=False, ncnn/PyTorch coefficients) ----
void linearCoeff(uint d, uint inSize, uint outSize, out uint s0, out uint s1, out float a)
{
    if (inSize == 1)
    {
        s0 = 0; s1 = 0; a = 0.0;
        return;
    }
    const float scale = float(inSize) / float(outSize);
    float f = (float(d) + 0.5) * scale - 0.5;
    int s = int(floor(f));
    f -= float(s);
    if (s < 0) { s = 0; f = 0.0; }
    if (s >= int(inSize) - 1) { s = int(inSize) - 2; f = 1.0; }
    s0 = uint(s);
    s1 = uint(s) + 1;
    a = f;
}

[numthreads(8, 8, 1)]
void interp(uint3 id : SV_DispatchThreadID)
{
    const uint inW = p0.x, inH = p0.y, inOff = p0.w;
    const uint outW = p1.x, outH = p1.y, outC = p1.z, outOff = p1.w;
    if (id.x >= outW || id.y >= outH || id.z >= outC) return;
    uint x0, x1, y0, y1;
    float ax, ay;
    linearCoeff(id.x, inW, outW, x0, x1, ax);
    linearCoeff(id.y, inH, outH, y0, y1, ay);
    const float v00 = src0[at(inOff, inW, inH, id.z, y0, x0)];
    const float v01 = src0[at(inOff, inW, inH, id.z, y0, x1)];
    const float v10 = src0[at(inOff, inW, inH, id.z, y1, x0)];
    const float v11 = src0[at(inOff, inW, inH, id.z, y1, x1)];
    const float top = v00 * (1.0 - ax) + v01 * ax;
    const float bottom = v10 * (1.0 - ax) + v11 * ax;
    dst[at(outOff, outW, outH, id.z, id.y, id.x)] = top * (1.0 - ay) + bottom * ay;
}

// ---- BinaryOp / Eltwise ---------------------------------------------------
// p0 = a (W, H, C, off), p1 = b (W, H, C, off), p2 = out (W, H, C, off)
// p3 = (op, bScalar, 0, 0), f0.x = scalar b, f0.yz = Eltwise coefficients
// ops: 0 add, 1 sub, 2 mul, 3 div, 4 max, 5 min, 7 rsub, 8 rdiv, 100 a*c0+b*c1
float binaryOp(uint op, float a, float b)
{
    switch (op)
    {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return a / b;
    case 4: return max(a, b);
    case 5: return min(a, b);
    case 7: return b - a;
    case 8: return b / a;
    default: return a * f0.y + b * f0.z;
    }
}

[numthreads(8, 8, 1)]
void binary(uint3 id : SV_DispatchThreadID)
{
    const uint w = p2.x, h = p2.y, c = p2.z;
    if (id.x >= w || id.y >= h || id.z >= c) return;
    const float a = src0[at(p0.w, p0.x, p0.y,
                            p0.z == 1 ? 0 : id.z, p0.y == 1 ? 0 : id.y, p0.x == 1 ? 0 : id.x)];
    float b = f0.x;
    if (p3.y == 0)
        b = src1[at(p1.w, p1.x, p1.y, p1.z == 1 ? 0 : id.z, p1.y == 1 ? 0 : id.y, p1.x == 1 ? 0 : id.x)];
    dst[at(p2.w, w, h, id.z, id.y, id.x)] = binaryOp(p3.x, a, b);
}

// ---- unary activations: p3.x = 0 leaky/relu (slope f0.x), 1 sigmoid -------
[numthreads(8, 8, 1)]
void activation(uint3 id : SV_DispatchThreadID)
{
    const uint w = p1.x, h = p1.y, c = p1.z;
    if (id.x >= w || id.y >= h || id.z >= c) return;
    float v = src0[at(p0.w, w, h, id.z, id.y, id.x)];
    v = p3.x == 1 ? 1.0 / (1.0 + exp(-v)) : (v > 0.0 ? v : v * f0.x);
    dst[at(p1.w, w, h, id.z, id.y, id.x)] = v;
}

// ---- copy (Concat part, identity Interp) -----------------------------------
[numthreads(8, 8, 1)]
void copy(uint3 id : SV_DispatchThreadID)
{
    const uint w = p0.x, h = p0.y, c = p0.z;
    if (id.x >= w || id.y >= h || id.z >= c) return;
    dst[at(p1.w, w, h, id.z, id.y, id.x)] = src0[at(p0.w, w, h, id.z, id.y, id.x)];
}

// ---- PixelShuffle (PyTorch/CRD order), p3.x = upscale factor ---------------
[numthreads(8, 8, 1)]
void pixelShuffle(uint3 id : SV_DispatchThreadID)
{
    const uint outW = p1.x, outH = p1.y, outC = p1.z;
    if (id.x >= outW || id.y >= outH || id.z >= outC) return;
    const uint r = p3.x;
    const uint ic = id.z * r * r + (id.y % r) * r + (id.x % r);
    dst[at(p1.w, outW, outH, id.z, id.y, id.x)] = src0[at(p0.w, p0.x, p0.y, ic, id.y / r, id.x / r)];
}

// ---- rife.Warp: image src0 (p0), flow src1 (p1, 2 channels), out p2 --------
[numthreads(8, 8, 1)]
void warp(uint3 id : SV_DispatchThreadID)
{
    const uint w = p0.x, h = p0.y, c = p0.z;
    if (id.x >= w || id.y >= h || id.z >= c) return;
    const float fx = src1[at(p1.w, w, h, 0, id.y, id.x)];
    const float fy = src1[at(p1.w, w, h, 1, id.y, id.x)];
    const float sx = clamp(float(id.x) + fx, 0.0, float(w - 1));
    const float sy = clamp(float(id.y) + fy, 0.0, float(h - 1));
    const uint x0 = uint(floor(sx));
    const uint y0 = uint(floor(sy));
    const uint x1 = min(x0 + 1, w - 1);
    const uint y1 = min(y0 + 1, h - 1);
    const float ax = sx - float(x0);
    const float ay = sy - float(y0);
    const float v00 = src0[at(p0.w, w, h, id.z, y0, x0)];
    const float v01 = src0[at(p0.w, w, h, id.z, y0, x1)];
    const float v10 = src0[at(p0.w, w, h, id.z, y1, x0)];
    const float v11 = src0[at(p0.w, w, h, id.z, y1, x1)];
    dst[at(p2.w, w, h, id.z, id.y, id.x)] =
        (v00 * (1.0 - ax) + v01 * ax) * (1.0 - ay) + (v10 * (1.0 - ax) + v11 * ax) * ay;
}
