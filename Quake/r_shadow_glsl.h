//
// This file contains snippets of GLSL shader code to use shadow maps.
//

#define SHADOW_VERT_UNIFORMS_GLSL \
	"uniform mat4 ShadowMatrix;\n"

#define SHADOW_FRAG_UNIFORMS_GLSL \
	"uniform bool UseShadow;\n" \
	"uniform float SunBrighten;\n" \
	"uniform float SunDarken;\n" \
	"uniform sampler2DShadow ShadowTex;\n"

#define SHADOW_VARYING_GLSL \
	"varying vec3 ShadowCoord;\n"

#define SHADOW_GET_COORD_GLSL(vertName) \
	"	ShadowCoord = (ShadowMatrix * " vertName ").xyz;\n"

#define SHADOW_SAMPLE_GLSL \
		"	if (UseShadow) {\n" \
		"		vec2 poissonDisk[4] = vec2[](vec2( -0.94201624, -0.39906216 ),vec2( 0.94558609, -0.76890725 ),vec2( -0.094184101, -0.92938870 ),vec2( 0.34495938, 0.29387760 ));\n" \
		"		float bias = 0.005;\n" \
		"		float darken = SunDarken/4.0; float brighten=SunBrighten/4.0;\n" \
		"		float shadowVis = 1.0;\n" \
		"		for (int i=0;i<4;i++) {\n" \
		"           if (shadow2D(ShadowTex, vec3(ShadowCoord.xy+poissonDisk[i]/700.0,ShadowCoord.z)).z < 1.0) {\n" \
		"                shadowVis -= darken;\n" \
		"           } else { shadowVis += brighten;\n }\n" \
		"       }\n" \
		"		result = vec4(result.xyz * shadowVis, result.a);\n" \
		"	}\n"