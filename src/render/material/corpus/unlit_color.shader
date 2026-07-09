# unlit_color — a minimal unlit material: a solid base color, with optional vertex fog and GPU
# instancing as keyword axes. FOG x INSTANCED = 4 variants. Keywords are authored alphabetically so
# the authored order already matches serialize()'s canonical (sorted) order.
shader unlit_color
keyword FOG off on
keyword INSTANCED off on
stage vertex vs_main
#version 450
layout(location = 0) in vec3 in_position;
#ifdef INSTANCED
layout(location = 1) in mat4 in_model;
#endif
layout(set = 0, binding = 0) uniform Camera { mat4 view_proj; };
void vs_main()
{
#ifdef INSTANCED
    gl_Position = view_proj * in_model * vec4(in_position, 1.0);
#else
    gl_Position = view_proj * vec4(in_position, 1.0);
#endif
}
endstage
stage fragment fs_main
#version 450
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 1) uniform Material { vec4 base_color; };
void fs_main()
{
    out_color = base_color;
#ifdef FOG
    out_color.rgb = mix(out_color.rgb, vec3(0.5), 0.25);
#endif
}
endstage
