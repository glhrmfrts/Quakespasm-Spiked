//
// This file contains snippets of GLSL shader code to use shadow maps.
//

#include "gl_random_texture.h"

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

#define SHADOW_MAP_TEXTURE_UNIT GL_TEXTURE4

#define SHADOW_VERT_UNIFORMS_GLSL \
	"uniform mat4 ShadowMatrix;\n"

#define SHADOW_FRAG_UNIFORMS_GLSL \
	"uniform bool UseShadow;\n" \
	"uniform float SunBrighten;\n" \
	"uniform float SunDarken;\n" \
	"uniform sampler2DShadow ShadowTex;\n" \
	"uniform sampler2D RandomTex;\n" \
	"uniform vec3 SunLightNormal;\n"

#define SHADOW_VARYING_GLSL \
	"varying vec3 ShadowCoord;\n" \
	"varying vec3 WorldCoord;\n"

#define SHADOW_GET_COORD_GLSL(vertName) \
	"	ShadowCoord = (ShadowMatrix * " vertName ").xyz;\n" \
	"	WorldCoord = (" vertName ").xyz;\n"

#define SHADOW_SAMPLE_GLSL(vertNormal) \
		"	if (UseShadow) {\n" \
			SHADOW_POISSON_DISK_GLSL \
		"		float bias = 0.010;\n" \
		"		float darken = SunDarken/6.0; float brighten=SunBrighten/6.0;\n" \
		"		float shadowVis = 1.0;\n" \
		"		float lightFactor = -dot(" vertNormal ", SunLightNormal);\n" \
		"		for (int i=0;i<6;i++) {\n" \
		"			int index = i;//int(floor(16.0*texture2D(RandomTex, (WorldCoord.xy+WorldCoord.z)*i).r));\n" \
		"           if (shadow2D(ShadowTex, vec3(ShadowCoord.xy+poissonDisk[index]/800.0,ShadowCoord.z-(bias*lightFactor))).z < 1.0) {\n" \
		"                shadowVis -= darken;\n" \
		"           } else { shadowVis += brighten * lightFactor;\n }\n" \
		"       }\n" \
		"		result = vec4(result.xyz * shadowVis, result.a);\n" \
		"	}\n"