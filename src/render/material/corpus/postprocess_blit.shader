# postprocess_blit — a fullscreen post-process blit with an optional tonemap pass. Single keyword
# axis TONEMAP {off,on} => 2 variants. A fullscreen-triangle vertex stage (no vertex inputs).
shader postprocess_blit
keyword TONEMAP off on
stage vertex vs_fullscreen
#version 450
layout(location = 0) out vec2 v_uv;
void vs_fullscreen()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
endstage
stage fragment fs_blit
#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform sampler2D src_tex;
void fs_blit()
{
    vec3 c = texture(src_tex, v_uv).rgb;
#ifdef TONEMAP
    c = c / (c + vec3(1.0));
#endif
    out_color = vec4(c, 1.0);
}
endstage
