#pragma once

#define FOG_FRAG_UNIFORMS_GLSL \
    "layout (std140) uniform fog_data {\n" \
    "   vec4 fog_color;\n" \
    "   float fog_density;\n" \
    "};\n"

#define FOG_CALC_GLSL \
    "	float fog = exp(-fog_density * fog_density * FogFragCoord * FogFragCoord);\n" \
    "	fog = clamp(fog, 0.0, 1.0);\n" \
    "	result.rgb = mix(fog_color.rgb, result.rgb, fog);\n"