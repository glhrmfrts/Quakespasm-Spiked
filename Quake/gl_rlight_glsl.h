#pragma once

#define DLIGHT_MAX_DLIGHTS 64

#define DLIGHT_TOSTR (x) #x

#define DLIGHT_FRAG_UNIFORMS_GLSL \
"struct dlight_single_t { vec4 color; vec4 position; float radius; };\n" \
"layout (std140) uniform dlight_data {\n" \
"	int num_lights;\n" \
"	dlight_single_t lights[64];\n" \
"};\n"

#define DLIGHT_SAMPLE_GLSL(vertNormal) \
"float lightshift = 128.0;\n" \
"if (num_lights==0) {  }\n" \
"for (int i = 0; i < num_lights; i++) {\n" \
"	vec3 light_dist = WorldCoord - lights[i].position.xyz;\n" \
"	vec3 light_dir = normalize(light_dist);\n" \
"	float light_factor = dot(light_dir, -(" vertNormal "));\n" \
"	light_factor *= 1.0 - clamp(length(light_dist) / lights[i].radius, 0.0, 1.0);\n" \
"	light_factor = clamp(light_factor, 0.0, 1.0);\n" \
"	lighting.xyz += lights[i].color.xyz * light_factor;\n" \
"}\n" \