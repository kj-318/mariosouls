// Remastered geometry-pass test shaders.
// Compiled by tools/compile_test_shaders.py (d3dcompiler_47.dll, vs_5_0/ps_5_0)
// into framework/hosts/ds1remastered/test_shaders_gen.h.  Do not hand-edit the
// generated header; edit this file and rerun tools/build_remastered.bat.

struct VSOut {
    float4 pos : SV_Position;
};

// Positions arrive already in clip space (the CPU applies the captured
// view-projection for world markers).
VSOut VSMain(float4 p : POSITION) {
    VSOut o;
    o.pos = p;
    return o;
}

// Values written to the first two bound render targets.  Set per draw by the
// host from inject_mode.txt, so G-buffer channel meanings can be probed live.
cbuffer TestTargets : register(b0) {
    float4 gTarget0;
    float4 gTarget1;
};

struct PSOut {
    float4 t0 : SV_Target0;
    float4 t1 : SV_Target1;
};

PSOut PSMain(VSOut i) {
    PSOut o;
    o.t0 = gTarget0;
    o.t1 = gTarget1;
    return o;
}
