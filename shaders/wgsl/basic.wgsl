// Basic triangle shader (WebGPU / WGSL).
//
// Mirrors shaders/metal/basic.metal and shaders/glsl/basic.{vert,frag}. Both
// entry points live in one module (like the .metal file); the demo creates a
// vertex and a fragment shader handle from this single source.
//
// Vertex layout must match the demo's Vertex struct and pipeline:
//   @location(0) position : vec2<f32>   (NE_VERTEX_FORMAT_FLOAT2)
//   @location(1) color    : vec4<f32>   (NE_VERTEX_FORMAT_FLOAT4)
// Note: color is vec4 (not vec3 as in the .metal file) — WebGPU validates the
// vertex format against the shader strictly, so it must match FLOAT4.

struct VertexIn {
    @location(0) position : vec2<f32>,
    @location(1) color    : vec4<f32>,
};

struct VertexOut {
    @builtin(position) position : vec4<f32>,
    @location(0)       color    : vec4<f32>,
};

@vertex
fn vs_main(in : VertexIn) -> VertexOut {
    var out : VertexOut;
    out.position = vec4<f32>(in.position, 0.0, 1.0);
    out.color    = in.color;
    return out;
}

@fragment
fn fs_main(in : VertexOut) -> @location(0) vec4<f32> {
    return in.color;
}
