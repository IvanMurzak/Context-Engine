# lit_pbr — a physically-based lit material with three keyword axes:
#   NORMAL_MAP {off,on}, QUALITY {low,med,high}, SHADOWS {off,on}
# => 2 x 3 x 2 = 12 shader variants. Exercises a multi-value (non-boolean) keyword.
shader lit_pbr
keyword NORMAL_MAP off on
keyword QUALITY low med high
keyword SHADOWS off on
stage vertex vs_main
#version 450
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(set = 0, binding = 0) uniform Camera { mat4 view_proj; };
layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;
void vs_main()
{
    v_normal = in_normal;
    v_uv = in_uv;
    gl_Position = view_proj * vec4(in_position, 1.0);
}
endstage
stage fragment fs_main
#version 450
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 1, binding = 0) uniform sampler2D albedo_tex;
#ifdef NORMAL_MAP
layout(set = 1, binding = 1) uniform sampler2D normal_tex;
#endif
void fs_main()
{
    vec3 n = normalize(v_normal);
#ifdef NORMAL_MAP
    n = normalize(n + texture(normal_tex, v_uv).xyz * 2.0 - 1.0);
#endif
    float ndl = max(dot(n, vec3(0.0, 1.0, 0.0)), 0.0);
#ifdef SHADOWS
    ndl *= 0.75;
#endif
#if QUALITY == high
    ndl = pow(ndl, 1.0 / 2.2);
#endif
    out_color = texture(albedo_tex, v_uv) * ndl;
}
endstage
