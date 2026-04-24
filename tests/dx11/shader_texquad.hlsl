struct VS_Input  { float2 pos : POS; float2 uv : TEX; };
struct VS_Output { float4 pos : SV_POSITION; float2 uv : TEX; };

Texture2D    tex  : register(t0);
SamplerState samp : register(s0);

VS_Output vs_main(VS_Input i)
{
    VS_Output o;
    o.pos = float4(i.pos, 0.0f, 1.0f);
    o.uv  = i.uv;
    return o;
}

float4 ps_main(VS_Output i) : SV_TARGET
{
    return tex.Sample(samp, i.uv);
}
