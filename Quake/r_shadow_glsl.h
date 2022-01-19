//
// This file contains snippets of GLSL shader code to use shadow maps.
//

#include "gl_random_texture.h"

#define SHADOW_MAP_TEXTURE_UNIT GL_TEXTURE4

#define SHADOW_POISSON_DISK_GLSL \
"vec2 poissonDisk[16] = vec2[](\n " \
   "vec2( -0.94201624, -0.39906216 ),\n" \
   "vec2( 0.94558609, -0.76890725 ), \n"\
   "vec2( -0.094184101, -0.92938870 ),\n "\
   "vec2( 0.34495938, 0.29387760 ), \n"\
   "vec2( -0.91588581, 0.45771432 ), \n"\
   "vec2( -0.81544232, -0.87912464 ), \n"\
   "vec2( -0.38277543, 0.27676845 ), \n"\
   "vec2( 0.97484398, 0.75648379 ), \n"\
   "vec2( 0.44323325, -0.97511554 ), \n"\
   "vec2( 0.53742981, -0.47373420 ), \n"\
   "vec2( -0.26496911, -0.41893023 ),\n "\
   "vec2( 0.79197514, 0.19090188 ), \n"\
   "vec2( -0.24188840, 0.99706507 ), \n"\
   "vec2( -0.81409955, 0.91437590 ),\n "\
   "vec2( 0.19984126, 0.78641367 ),\n "\
   "vec2( 0.14383161, -0.14100790 )\n"\
   ");"

#define SHADOW_VERT_UNIFORMS_GLSL

#define SHADOW_FRAG_UNIFORMS_GLSL \
"struct shadow_single_t {\n" \
"	mat4 shadow_matrix;\n" \
"	vec4 light_normal;\n" \
"	vec4 light_position;\n" \
"	float brighten;\n" \
"	float darken;\n" \
"	float radius;\n" \
"	float bias;\n" \
"	float spot_cutoff;\n" \
"	int light_type;\n" \
"};\n" \
"layout (std140) uniform shadow_data {\n" \
"	bool use_shadow;\n" \
"	int num_shadows;\n" \
"	shadow_single_t shadows[10];\n" \
"};\n" \
"uniform sampler2DShadow shadow_map_samplers[10];\n" \
"\n" \
SHADOW_POISSON_DISK_GLSL \
"float CalcSunShadow(int idx, vec3 world_coord, vec3 world_normal) {\n" \
"	vec3 shadow_coord = (shadows[idx].shadow_matrix * vec4(world_coord, 1.0)).xyz;\n" \
"	float light_factor = dot(world_normal, shadows[idx].light_normal.xyz);\n" \
"	float bias = shadows[idx].bias*(-light_factor);\n" \
"	float darken = shadows[idx].darken/6.0;\n" \
"	float result = 0.0f;\n" \
"	for (int j=0;j<6;j++) {\n" \
"		int index = j;     //int(floor(16.0*texture2D(random_tex, (WorldCoord.xy+WorldCoord.z)*j).r));\n" \
"       if (texture(shadow_map_samplers[idx], vec3(shadow_coord.xy+poissonDisk[index]/800.0,shadow_coord.z-bias)) < 1.0) {\n" \
"           result += darken*(-light_factor);\n" \
"       }\n" \
"   }\n" \
"	return result;\n" \
"}\n" \
"\n" \
"float CalcSpotShadow(int idx, vec3 world_coord, vec3 world_normal) {\n" \
"	vec4 shadow_coord_v4 = (shadows[idx].shadow_matrix * vec4(world_coord, 1.0));\n" \
"	vec3 shadow_coord = 0.5*(shadow_coord_v4.xyz/shadow_coord_v4.w)+0.5;\n" \
"	float light_factor = dot(normalize(world_coord - shadows[idx].light_position.xyz), shadows[idx].light_normal.xyz);\n" \
"	if (light_factor <= shadows[idx].spot_cutoff) { return 0.0f; }\n" \
"	float bias = shadows[idx].bias*(light_factor);\n" \
"	float darken = shadows[idx].darken/6.0;\n" \
"	float result = 0.0f;\n" \
"	for (int j=0;j<6;j++) {\n" \
"		int index = j;     //int(floor(16.0*texture2D(random_tex, (WorldCoord.xy+WorldCoord.z)*j).r));\n" \
"       if (texture(shadow_map_samplers[idx], vec3(shadow_coord.xy+poissonDisk[index]/800.0,shadow_coord.z-bias)) < 1.0) {\n" \
"           result += darken*(light_factor);\n" \
"       }\n" \
"   }\n" \
"	return result;\n" \
"}\n"

#define SHADOW_VERT_OUTPUT_GLSL \
	"out vec3 WorldCoord;\n"

#define SHADOW_FRAG_INPUT_GLSL \
	"in vec3 WorldCoord;\n"

#define SHADOW_GET_COORD_GLSL(vertName) \
	"	WorldCoord = (" vertName ").xyz;\n"

#define SHADOW_SAMPLE_GLSL(vertNormal) \
"if (use_shadow) for (int i = 0; i < num_shadows; i++) {\n" \
"	float shadow_factor;" \
"	if (shadows[i].light_type==0) { shadow_factor = CalcSunShadow(i, WorldCoord, " vertNormal "); }\n" \
"	else if (shadows[i].light_type==1) { shadow_factor = CalcSpotShadow(i, WorldCoord, " vertNormal "); }\n" \
"	//result = vec4(result.xyz * (1.0f - shadow_factor), result.a);\n" \
"  lighting *= (1.0 - shadow_factor);\n" \
"}\n"