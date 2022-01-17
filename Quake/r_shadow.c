/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2021-2022 Guilherme Nemeth

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
//
// This file contains the implementation for shadow mapping of a single directional light (the sun)
// and point lights.
//
// TODO:
// 		- X  Sample Shadow Map in water
//		- X? Correctly render fence textures in shadow map
//		- XX Use Stratified Poisson Sampling
//		- X Use UBO for sending data to shaders
//		- Implement Spot lights shadows
//		- Implement Point lights shadows
//		- Fix sampling

//		- Implement HDR rendering
//		- Implement MSAA support
//		- Implement Motion Blur
//		- Implement SSR for water
//

#include "quakedef.h"
#include "glquake.h"
#include "gl_random_texture.h"
#include "r_shadow_glsl.h"

enum {
	SUN_SHADOW_WIDTH = 1024*4,
	SUN_SHADOW_HEIGHT = 1024*4,

	SPOT_SHADOW_WIDTH = 1024,
	SPOT_SHADOW_HEIGHT = 1024,
};

#define SUN_SHADOW_BIAS (0.01f)

#define SPOT_SHADOW_BIAS (0.000001f)

cvar_t r_shadow_sun = {"r_shadow_sun", "1", CVAR_ARCHIVE, 1.0f};
cvar_t r_shadow_sundebug = {"r_shadow_sundebug", "0", CVAR_NONE, 0.0f};
cvar_t r_shadow_sunbrighten = {"r_shadow_sunbrighten", "0.2", CVAR_NONE, 0.2f};
cvar_t r_shadow_sundarken = {"r_shadow_sundarken", "0.4", CVAR_NONE, 0.4f};
cvar_t r_shadow_sunworldcast = {"r_shadow_sunworldcast", "1", CVAR_ARCHIVE, 1.0f};

static struct {
	gl_shader_t shader;
	int u_Tex;
	int u_UseAlphaTest;
	int u_Alpha;
	int u_Debug;
	int u_ShadowMatrix;
	int u_ModelMatrix;
} shadow_brush_glsl;

typedef struct {
	int maxbones;

	gl_shader_t shader;

	// uniforms used in vert shader
	GLuint bonesLoc;
	GLuint blendLoc;

	// uniforms used in frag shader
	GLuint texLoc;
	GLuint alphaLoc;
	GLuint debugLoc;

	// shadow uniforms
	GLuint shadowMatrixLoc;
	GLuint modelMatrixLoc;
} shadow_aliasglsl_t;

typedef struct {
	mat4_t shadow_matrix;
	vec4_t light_normal;
	vec4_t light_position;
	float brighten;
	float darken;
	float radius;
	float bias;
	float spot_cutoff;
	int light_type;
} shadow_ubo_single_t;

typedef struct {
	int use_shadow;
	int num_shadow_maps;
	int pad1;
	int pad2;
	shadow_ubo_single_t shadows[MAX_FRAME_SHADOWS];
} shadow_ubo_data_t;

static int num_shadow_alias_glsl;
static shadow_aliasglsl_t shadow_alias_glsl[ALIAS_GLSL_MODES];

static GLuint shadow_ubo;
static shadow_ubo_data_t shadow_ubo_data;
static struct { GLuint id; GLuint unit; } shadow_frame_textures[MAX_FRAME_SHADOWS];

static vec3_t current_sun_pos;
static vec3_t debug_sun_pos;
static qboolean debug_override_sun_pos;

static r_shadow_light_t* sun_light;
static r_shadow_light_t* first_light;
static r_shadow_light_t* last_light_rendered;

static int light_id_gen;

static const char* shadow_brush_vertex_shader;
static const char* shadow_brush_fragment_shader;
static const char *shadow_alias_vertex_shader;
static const char *shadow_alias_fragment_shader;

static void R_Shadow_SetAngle_f ()
{
	if (Cmd_Argc() < 4) {
		if (!sun_light) {
			Con_Printf("No active sunlight\n");
			return;
		}

		Con_Printf ("Current sun shadow angle: %5.1f %5.1f %5.1f\n",
			sun_light->light_angles[1], -sun_light->light_angles[0], sun_light->light_angles[2]);
		Con_Printf ("Usage: r_shadow_sunangle <yaw> <pitch> <roll>\n");
		return;
	}

	float yaw = Q_atof (Cmd_Argv(1));
	float pitch = Q_atof (Cmd_Argv(2));
	float roll = Q_atof (Cmd_Argv(3));

	R_Shadow_SetupSun ((const vec3_t){ yaw, pitch, roll });
}

static void R_Shadow_CreateBrushShaders ()
{
	if (!GL_CreateShaderFromVF (&shadow_brush_glsl.shader, shadow_brush_vertex_shader, shadow_brush_fragment_shader, 0, NULL)) {
		Con_DWarning ("Failed to compile shadow shader\n");
		return;
	}

	shadow_brush_glsl.u_Tex = GL_GetUniformLocationFunc (shadow_brush_glsl.shader.program_id, "Tex");
	shadow_brush_glsl.u_UseAlphaTest = GL_GetUniformLocationFunc (shadow_brush_glsl.shader.program_id, "UseAlphaTest");
	shadow_brush_glsl.u_Alpha = GL_GetUniformLocationFunc (shadow_brush_glsl.shader.program_id, "Alpha");
	shadow_brush_glsl.u_ShadowMatrix = GL_GetUniformLocationFunc (shadow_brush_glsl.shader.program_id, "ShadowMatrix");
	shadow_brush_glsl.u_ModelMatrix = GL_GetUniformLocationFunc (shadow_brush_glsl.shader.program_id, "ModelMatrix");
	shadow_brush_glsl.u_Debug = GL_GetUniformLocationFunc (shadow_brush_glsl.shader.program_id, "Debug");
}

#define pose1VertexAttrIndex 0
#define pose1NormalAttrIndex 1
#define pose2VertexAttrIndex 2
#define pose2NormalAttrIndex 3
#define texCoordsAttrIndex 4
#define vertColoursAttrIndex 5
#define boneWeightAttrIndex pose2VertexAttrIndex
#define boneIndexAttrIndex pose2NormalAttrIndex

static void R_Shadow_CreateAliasShaders ()
{
#if 1
	int i;
	shadow_aliasglsl_t *glsl;
	char processedVertSource[8192], *defines;
	const glsl_attrib_binding_t bindings[] = {
		{ "TexCoords", texCoordsAttrIndex },
		{ "Pose1Vert", pose1VertexAttrIndex },
		{ "Pose1Normal", pose1NormalAttrIndex },
		{ "Pose2Vert", pose2VertexAttrIndex },
		{ "Pose2Normal", pose2NormalAttrIndex },
		{ "VertColours", vertColoursAttrIndex }
	};

	if (!gl_glsl_alias_able)
		return;

	for (i = 0; i < ALIAS_GLSL_MODES; i++)
	{
		glsl = &shadow_alias_glsl[i];

		if (i == ALIAS_GLSL_SKELETAL)
		{
			defines = "#define SKELETAL\n#define MAXBONES 64\n";
			glsl->maxbones = 64;
		}
		else
		{
			defines = "";
			glsl->maxbones = 0;
		}
		q_snprintf(processedVertSource, sizeof(processedVertSource), shadow_alias_vertex_shader, defines);

#if 0
		GL_CreateShaderFromVF (&glsl->shader, processedVertSource, shadow_alias_fragment_shader);
		int numbindings = sizeof(bindings) / sizeof(bindings[0]);
		for (int i = 0; i < numbindings; i++) {
			GL_BindAttribLocationFunc(glsl->shader.program_id, bindings[i].attrib, bindings[i].name);
		}
#else
		glsl->shader.program_id = GL_CreateProgram (processedVertSource, shadow_alias_fragment_shader, sizeof(bindings) / sizeof(bindings[0]), bindings);
#endif 

		if (glsl->shader.program_id != 0)
		{
		// get uniform locations
			if (i == ALIAS_GLSL_SKELETAL)
			{
				glsl->bonesLoc = GL_GetUniformLocation (&glsl->shader.program_id, "BoneTable");
				glsl->blendLoc = -1;
			}
			else
			{
				glsl->bonesLoc = -1;
				glsl->blendLoc = GL_GetUniformLocation (&glsl->shader.program_id, "Blend");
			}

			glsl->texLoc = GL_GetUniformLocation (&glsl->shader.program_id, "Tex");
			glsl->alphaLoc = GL_GetUniformLocation (&glsl->shader.program_id, "Alpha");
			glsl->debugLoc = GL_GetUniformLocation (&glsl->shader.program_id, "Debug");
			glsl->shadowMatrixLoc = GL_GetUniformLocation (&glsl->shader.program_id, "ShadowMatrix");
			glsl->modelMatrixLoc = GL_GetUniformLocation (&glsl->shader.program_id, "ModelMatrix");
			num_shadow_alias_glsl++;
		}
	}
#endif
}

static void R_Shadow_CreateFramebuffer (r_shadow_light_t* light)
{
	GL_GenFramebuffersFunc (1, &light->shadow_map_fbo);
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, light->shadow_map_fbo);

	glGenTextures (1, &light->shadow_map_texture);
	glBindTexture (GL_TEXTURE_2D, light->shadow_map_texture);

	glTexImage2D (GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, light->shadow_map_width, light->shadow_map_height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, 0);

	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);

	GL_FramebufferTextureFunc (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, light->shadow_map_texture, 0);

	glDrawBuffer (GL_NONE); // No color buffer is drawn to.
	glReadBuffer (GL_NONE);

	if (GL_CheckFramebufferStatusFunc(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		Con_Warning ("Failed to create Sun Shadow Framebuffer\n");
		return;
	}

	glBindTexture (GL_TEXTURE_2D, 0);
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, 0);
}

void R_Shadow_Init ()
{
	Cvar_RegisterVariable(&r_shadow_sun);
	Cvar_RegisterVariable(&r_shadow_sundebug);
	Cvar_RegisterVariable(&r_shadow_sunbrighten);
	Cvar_RegisterVariable(&r_shadow_sundarken);
	Cvar_RegisterVariable(&r_shadow_sunworldcast);
	Cmd_AddCommand("r_shadow_sunangle", R_Shadow_SetAngle_f);

	if (!shadow_brush_glsl.shader.program_id) {
		R_Shadow_CreateBrushShaders();
	}
	if (num_shadow_alias_glsl < ALIAS_GLSL_MODES) {
		R_Shadow_CreateAliasShaders();
	}
}

static void R_Shadow_LinkLight (r_shadow_light_t* light)
{
	light->id = light_id_gen++;
	light->next = first_light;
	first_light = light;
}

//
// Takes the world bounds and returns the bounds in the sun light/shadow-map space.
//
static void R_Shadow_GetWorldProjectionBounds(const vec3_t mins, const vec3_t maxs, const vec3_t lightangles,
	vec3_t out_projmins, vec3_t out_projmaxs) {

	vec4_t world_corners[8] = {
		{ mins[0], mins[1], mins[2], 1.0f },
		{ maxs[0], mins[1], mins[2], 1.0f },
		{ maxs[0], maxs[1], mins[2], 1.0f },
		{ mins[0], maxs[1], mins[2], 1.0f },

		{ mins[0], mins[1], maxs[2], 1.0f },
		{ maxs[0], mins[1], maxs[2], 1.0f },
		{ maxs[0], maxs[1], maxs[2], 1.0f },
		{ mins[0], maxs[1], maxs[2], 1.0f },
	};

	mat4_t view_matrix;
	Matrix4_ViewMatrix (lightangles, (const vec3_t){0,0,0}, view_matrix);

	vec4_t view_world_corners[8];
	for (int i = 0; i < countof(world_corners); i++) {
		Matrix4_Transform4 (view_matrix, world_corners[i], view_world_corners[i]);
	}

	vec3_t proj_mins = {FLT_MAX, FLT_MAX, FLT_MAX};
	vec3_t proj_maxs = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
	for (int i = 0; i < countof(view_world_corners); i++) {
		for (int ax = 0; ax < 3; ax++) {
			if (view_world_corners[i][ax] < proj_mins[ax]) {
				proj_mins[ax] = view_world_corners[i][ax];
			}
			if (view_world_corners[i][ax] > proj_maxs[ax]) {
				proj_maxs[ax] = view_world_corners[i][ax];
			}
		}
	}

	VectorCopy (proj_mins, out_projmins);
	VectorCopy (proj_maxs, out_projmaxs);
}

void R_Shadow_SetupSun (vec3_t angle)
{
	if (!r_shadow_sun.value) { return; }

	if (!sun_light) {
		sun_light = calloc (1, sizeof(r_shadow_light_t));
		sun_light->enabled = true;
		sun_light->type = r_shadow_light_type_sun;
		sun_light->shadow_map_width = SUN_SHADOW_WIDTH;
		sun_light->shadow_map_height = SUN_SHADOW_HEIGHT;
		sun_light->bias = SUN_SHADOW_BIAS;
		R_Shadow_LinkLight (sun_light);

		R_Shadow_CreateFramebuffer (sun_light);
	}

	//
	// We accept angles in the form of (Yaw Pitch Roll) to maintain consistency
	// with other mapping options, but Quake internally accepts (Pitch Yaw Roll)
	// so here we need to convert from one to the other.
	//
	sun_light->light_angles[0] = -angle[1];
	sun_light->light_angles[1] = angle[0];
	sun_light->light_angles[2] = angle[2];

	vec3_t pos = {0,0,0};
	vec3_t mins;
	vec3_t maxs;
	vec3_t worldsize;
	vec3_t halfsize;

	VectorCopy (cl.worldmodel->mins, mins);
	VectorCopy (cl.worldmodel->maxs, maxs);
	VectorSubtract (maxs, mins, worldsize);
	VectorScale (worldsize, 0.5f, halfsize);

	if (debug_override_sun_pos) {
		VectorCopy (debug_sun_pos, pos);
		debug_override_sun_pos = false;
	}
	else {
		//pos[0] = mins[0];
		//pos[1] = maxs[1];
		//pos[2] = maxs[2];
	}
	VectorCopy (pos, current_sun_pos);

	vec3_t fwd, right, up;
	AngleVectors (sun_light->light_angles, fwd, right, up);
	Con_Printf ("fwd: (%f, %f, %f)\n", fwd[0], fwd[1], fwd[2]);

	VectorCopy (fwd, sun_light->light_normal);

	Con_Printf ("mins.x: %f, maxs.x: %f\n", mins[0], maxs[0]);
	Con_Printf ("mins.y: %f, maxs.y: %f\n", mins[1], maxs[1]);
	Con_Printf ("mins.z: %f, maxs.z: %f\n", mins[2], maxs[2]);
	// Con_Printf("view_mins.z: %f, view_maxs.z: %f\n", view_mins[2], view_maxs[2]);

	vec3_t proj_mins, proj_maxs;
	R_Shadow_GetWorldProjectionBounds (mins, maxs, sun_light->light_angles, proj_mins, proj_maxs);

	// Don't ask me why but to get the correct near and far Z we have to use this angles.
	vec3_t tolight_angles = { -sun_light->light_angles[0], sun_light->light_angles[1] + 180.0f, sun_light->light_angles[2] };

	vec3_t tl_proj_mins, tl_proj_maxs;
	R_Shadow_GetWorldProjectionBounds (mins, maxs, tolight_angles, tl_proj_mins, tl_proj_maxs);

	Con_Printf ("proj_mins.x: %f, proj_maxs.x: %f\n", proj_mins[0], proj_maxs[0]);
	Con_Printf ("proj_mins.y: %f, proj_maxs.y: %f\n", proj_mins[1], proj_maxs[1]);
	Con_Printf ("proj_mins.z: %f, proj_maxs.z: %f\n", proj_mins[2], proj_maxs[2]);

	Con_Printf ("tl_proj_mins.z: %f, tl_proj_maxs.z: %f\n", tl_proj_mins[2], tl_proj_maxs[2]);

	float znear = tl_proj_mins[2];
	float zfar = tl_proj_maxs[2];

	Con_Printf ("znear: %f, zfar: %f\n", znear, zfar);

	mat4_t proj_matrix;
	const float scale = 1.0f;
	Matrix4_Ortho (
		// bottom, top
		proj_mins[1]*scale,proj_maxs[1]*scale,
		//-4096.0f,4096.0f,

		// left, right
		proj_mins[0]*scale,proj_maxs[0]*scale,
		//-4096.0f, 4096.0f,

		// near,far
		znear*scale,zfar*scale,

		proj_matrix
	);

	// Matrix4_ProjectionMatrix(r_fovx, r_fovy, 0.1f, 16384, false, 0, 0, proj_matrix);

	mat4_t render_view_matrix;
	Matrix4_ViewMatrix (sun_light->light_angles, pos, render_view_matrix);

	Matrix4_Multiply (proj_matrix, render_view_matrix, sun_light->shadow_map_projview);

	mat4_t shadow_bias_matrix = {
		0.5, 0.0, 0.0, 0.0,
		0.0, 0.5, 0.0, 0.0,
		0.0, 0.0, 0.5, 0.0,
		0.5, 0.5, 0.5, 1.0
	};
	Matrix4_Multiply (shadow_bias_matrix, sun_light->shadow_map_projview, sun_light->world_to_shadow_map);

	// Ensure random texture is loaded cause we'll need it
	GL_GetRandomTexture ();
}

void R_Shadow_AddSpotLight(const vec3_t pos, const vec3_t angles, float fov, float zfar)
{
	if (!zfar) {
		zfar = 300;
	}

	r_shadow_light_t* l = calloc(1, sizeof(r_shadow_light_t));
	l->enabled = true;
	l->type = r_shadow_light_type_spot;
	l->bias = SPOT_SHADOW_BIAS;
	l->radius = zfar;
	l->shadow_map_width = SPOT_SHADOW_WIDTH;
	l->shadow_map_height = SPOT_SHADOW_HEIGHT;

	VectorCopy(pos, l->light_position);

	l->light_angles[0] = -angles[1];
	l->light_angles[1] = angles[0];
	l->light_angles[2] = angles[2];

	vec3_t fwd, right, up;
	AngleVectors(l->light_angles, fwd, right, up);
	VectorCopy(fwd, l->light_normal);

	mat4_t view_matrix, proj_matrix;
	Matrix4_ViewMatrix(l->light_angles, l->light_position, view_matrix);
	Matrix4_ProjectionMatrix(fov, fov, 1.0f, zfar, false, 0, 0, proj_matrix);
	Matrix4_Multiply(proj_matrix, view_matrix, l->shadow_map_projview);
	memcpy(l->world_to_shadow_map, l->shadow_map_projview, sizeof(l->shadow_map_projview));

	R_Shadow_CreateFramebuffer(l);

	R_Shadow_LinkLight(l);

	Con_Printf("Added shadow spotlight.\n");
}

enum shadow_entity { entity_invalid, entity_worldspawn, entity_light };

static qboolean worldsun;
static vec3_t worldsunangle;
static qboolean shadowlight;
static vec3_t shadowlightorigin;
static vec3_t shadowlightangle;
static float shadowlightconeangle;
static float shadowlightradius;
static qboolean shadowlightspot;

static void R_Shadow_HandleEntityKey (enum shadow_entity t, const char* key, size_t keylen, const char* value, size_t valuelen)
{
	if (keylen == 0) { return; }

	char* v = malloc (valuelen + 1);
	v[valuelen] = '\0';
	memcpy (v, value, valuelen);

	if (t == entity_worldspawn) {
		if (!strncmp(key, "_shadowsun", keylen)) {
			worldsun = true;
		}
		else if (!strncmp(key, "_shadowsunangle", keylen)) {
			Cmd_TokenizeString (v);
			worldsunangle[0] = Q_atof (Cmd_Argv(0));
			worldsunangle[1] = Q_atof (Cmd_Argv(1));
			worldsunangle[2] = Q_atof (Cmd_Argv(2));
		}
	}
	else if (t == entity_light) {
		if (!strncmp(key, "_shadowlight", keylen)) {
			shadowlight = true;
		}
		else if (!strncmp(key, "origin", keylen)) {
			Cmd_TokenizeString (v);
			shadowlightorigin[0] = Q_atof (Cmd_Argv(0));
			shadowlightorigin[1] = Q_atof (Cmd_Argv(1));
			shadowlightorigin[2] = Q_atof (Cmd_Argv(2));
		}
		else if (!strncmp(key, "mangle", keylen)) {
			Cmd_TokenizeString (v);
			shadowlightangle[0] = Q_atof (Cmd_Argv(0));
			shadowlightangle[1] = Q_atof (Cmd_Argv(1));
			shadowlightangle[2] = Q_atof (Cmd_Argv(2));
			shadowlightspot = true;
		}
		else if (!strncmp(key, "angle", keylen)) {
			shadowlightconeangle = Q_atof (v);
			shadowlightspot = true;
		}
		else if (!strncmp(key, "_shadowlightconeangle", keylen)) {
			shadowlightconeangle = Q_atof (v);
			shadowlightspot = true;
		}
		else if (!strncmp(key, "_shadowlightradius", keylen)) {
			shadowlightradius = Q_atof (v);
		}
	}
	free (v);
}

static void R_Shadow_EndEntity (enum shadow_entity t)
{
	if (t == entity_light && shadowlight) {
		if (shadowlightspot) {
			R_Shadow_AddSpotLight (shadowlightorigin, shadowlightangle, shadowlightconeangle, shadowlightradius);
		}
	}
	else if (t == entity_worldspawn && worldsun) {
		R_Shadow_SetupSun (worldsunangle);
	}

	memset (shadowlightorigin, 0, sizeof(shadowlightorigin));
	memset (shadowlightangle, 0, sizeof(shadowlightangle));
	shadowlightconeangle = 0.0f;
	shadowlightradius = 0.0f;
	shadowlight = false;
	worldsun = false;
}

static void R_Shadow_ParseEntities (const char* ent_text)
{
    enum {
        parse_initial,
        parse_entity1,
        parse_entity2,
        parse_field_key,
        parse_field_value,
        parse_brushes,
        parse_comment,
    } state = parse_initial;

	size_t field_begin = 0;
	size_t field_end = 0;
	const char* field_key;
	const char* field_value;
	size_t field_key_len;
	size_t field_value_len;
	size_t textsize = strlen(ent_text);
	enum shadow_entity current_entity = entity_worldspawn;
    
    for (size_t offs = 0; offs < textsize; offs++) {
        char c = ent_text[offs];
        char cn = (offs < textsize-1) ? ent_text[offs+1] : 0;

        switch (state) {
        case parse_initial: {
            if (c == '/' && cn == '/') {
                state = parse_comment;
                offs++;
            }
            else if (c == '{') {
                state = parse_entity1;
            }
            break;
        }
        case parse_entity1: {
            if (c == '"') {
                state = parse_field_key;
                field_begin = offs + 1;
            }
            else if (c == '{') {
                state = parse_brushes;
                field_begin = offs + 1;
            }
            else if (c == '}') {
                state = parse_initial;
				R_Shadow_EndEntity (current_entity);
				current_entity = entity_invalid;
            }
            break;
        }
        case parse_entity2: {
            if (c == '"') {
                state = parse_field_value;
                field_begin = offs + 1;
            }
            break;
        }
        case parse_field_key: {
            if (c == '"') {
                state = parse_entity2;
                field_key = ent_text+field_begin;
				field_key_len = offs-field_begin;
            }
            break;
        }
        case parse_field_value: {
            if (c == '"') {
                state = parse_entity1;
                field_value = ent_text+field_begin;
				field_value_len = offs-field_begin;

				if (!strncmp(field_key, "classname", field_key_len)) {
					if (!strncmp(field_value, "worldspawn", field_value_len)) {
						current_entity = entity_worldspawn;
					}
				}

				if (current_entity == entity_invalid && !strncmp(field_key, "_shadowlight", strlen("_shadowlight"))) {
					current_entity = entity_light;
				}

				R_Shadow_HandleEntityKey (current_entity, field_key, field_key_len, field_value, field_value_len);
            }
            break;
        }
        case parse_brushes: {
            if (c == '}') {
                state = parse_entity1;
            }
            break;
        }
        case parse_comment:
            if (c == '\n') {
                state = parse_initial;
            }
            break;
        }
    }
}

static void R_Shadow_ClearLights ()
{
	r_shadow_light_t* light = first_light;
	r_shadow_light_t* next = NULL;
	while (light) {
		// GL_DeleteFramebuffersFunc (1, &light->shadow_map_fbo);
		glDeleteTextures (1, &light->shadow_map_texture);

		next = light->next;
		free (light);
		light = next;
	}

	sun_light = NULL;
	first_light = NULL;
	light_id_gen = 0;
}

void R_Shadow_NewMap ()
{
	R_Shadow_ClearLights ();
	worldsun = false;
	memset (worldsunangle, 0, sizeof(worldsunangle));
	R_Shadow_ParseEntities (cl.worldmodel->entities);
}

//
// Functions for drawing stuff to the Shadow Map
//

extern GLuint gl_bmodel_vbo;

static void R_Shadow_DrawTextureChains (r_shadow_light_t* light, qmodel_t *model, entity_t *ent, texchain_t chain)
{
	if (light->type == r_shadow_light_type_sun && !r_shadow_sunworldcast.value) { return; }

	float entalpha = (ent != NULL) ? ENTALPHA_DECODE(ent->alpha) : 1.0f;

	glEnable (GL_BLEND);
	glDisable (GL_CULL_FACE);

	GL_UseProgramFunc (shadow_brush_glsl.shader.program_id);
	
// Bind the buffers
	GL_BindBuffer (GL_ARRAY_BUFFER, gl_bmodel_vbo);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0); // indices come from client memory!

	GL_EnableVertexAttribArrayFunc (0);
	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, VBO_VERTEXSIZE * sizeof(float), ((float *)0));

	GL_EnableVertexAttribArrayFunc (1);
	GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, VBO_VERTEXSIZE * sizeof(float), ((float*)0) + 3);
	
// set uniforms
	GL_Uniform1iFunc (shadow_brush_glsl.u_Tex, 0);
	GL_Uniform1iFunc (shadow_brush_glsl.u_UseAlphaTest, 0);
	GL_Uniform1fFunc (shadow_brush_glsl.u_Alpha, entalpha);
	GL_UniformMatrix4fvFunc (shadow_brush_glsl.u_ShadowMatrix, 1, false, (const GLfloat*)light->shadow_map_projview);
	GL_Uniform1iFunc (shadow_brush_glsl.u_Debug, r_shadow_sundebug.value);

	mat4_t model_matrix;
	if (ent) {
		Matrix4_InitTranslationAndRotation (ent->origin, ent->angles, model_matrix);
	}
	else {
		Matrix4_InitIdentity (model_matrix);
	}
	GL_UniformMatrix4fvFunc (shadow_brush_glsl.u_ModelMatrix, 1, false, (const GLfloat*)model_matrix);

	qboolean bound = false;
	
	for (int i=0 ; i<model->numtextures ; i++)
	{
		texture_t* t = model->textures[i];

		if (!t || !t->texturechains[chain] || t->texturechains[chain]->flags & (SURF_DRAWTURB | SURF_DRAWTILED | SURF_NOTEXTURE))
			continue;

		R_ClearBatch ();

		bound = false;
		for (msurface_t* s = t->texturechains[chain]; s; s = s->texturechain)
		{
			if (!bound) //only bind once we are sure we need this texture
			{
				GL_SelectTexture(GL_TEXTURE0);
				GL_Bind((R_TextureAnimation(t, ent != NULL ? ent->frame : 0))->gltexture);

				if (t->texturechains[chain]->flags & SURF_DRAWFENCE)
					GL_Uniform1iFunc(shadow_brush_glsl.u_UseAlphaTest, 1); // Flip alpha test back on

				bound = true;
			}

			R_BatchSurface(s);

			rs_brushpasses++;
		}

		R_FlushBatch ();

		if (bound && t->texturechains[chain]->flags & SURF_DRAWFENCE)
			GL_Uniform1iFunc (shadow_brush_glsl.u_UseAlphaTest, 0); // Flip alpha test back off
	}
	
	// clean up
	GL_DisableVertexAttribArrayFunc (0);
	
	GL_UseProgramFunc (0);
	GL_SelectTexture (GL_TEXTURE0);

	glEnable (GL_CULL_FACE);
	glDisable (GL_BLEND);
}

void R_Shadow_DrawBrushModel (r_shadow_light_t* light, entity_t* e)
{
	int			i, k;
	msurface_t	*psurf;
	float		dot;
	mplane_t	*pplane;
	qmodel_t	*clmodel;
	vec3_t		lightorg;

	//if (R_CullModelForEntity(e))
	//	return;

	currententity = e;
	clmodel = e->model;

	psurf = &clmodel->surfaces[clmodel->firstmodelsurface];

	R_ClearTextureChains (clmodel, chain_model);
	for (i=0 ; i<clmodel->nummodelsurfaces ; i++, psurf++)
	{
		pplane = psurf->plane;
		R_ChainSurface (psurf, chain_model);
		// rs_brushpolys++;
	}

	R_Shadow_DrawTextureChains (light, clmodel, e, chain_model);
}

#if 1

/*
=============
GLARB_GetXYZOffset

Returns the offset of the first vertex's meshxyz_t.xyz in the vbo for the given
model and pose.
=============
*/
static void *GLARB_GetXYZOffset_MDL (aliashdr_t *hdr, int pose)
{
	const size_t xyzoffs = offsetof (meshxyz_mdl_t, xyz);
	return currententity->model->meshvboptr+(hdr->vbovertofs + (hdr->numverts_vbo * pose * sizeof (meshxyz_mdl_t)) + xyzoffs);
}
static void *GLARB_GetXYZOffset_MDLQF (aliashdr_t *hdr, int pose)
{
	const size_t xyzoffs = offsetof (meshxyz_mdl16_t, xyz);
	return currententity->model->meshvboptr+(hdr->vbovertofs + (hdr->numverts_vbo * pose * sizeof (meshxyz_mdl16_t)) + xyzoffs);
}
static void *GLARB_GetXYZOffset_MD3 (aliashdr_t *hdr, int pose)
{
	const size_t xyzoffs = offsetof (meshxyz_md3_t, xyz);
	return currententity->model->meshvboptr+(hdr->vbovertofs + (hdr->numverts_vbo * pose * sizeof (meshxyz_md3_t)) + xyzoffs);
}

/*
=============
GLARB_GetNormalOffset

Returns the offset of the first vertex's meshxyz_t.normal in the vbo for the
given model and pose.
=============
*/
static void *GLARB_GetNormalOffset_MDL (aliashdr_t *hdr, int pose)
{
	const size_t normaloffs = offsetof (meshxyz_mdl_t, normal);
	return currententity->model->meshvboptr+(hdr->vbovertofs + (hdr->numverts_vbo * pose * sizeof (meshxyz_mdl_t)) + normaloffs);
}
static void *GLARB_GetNormalOffset_MDLQF (aliashdr_t *hdr, int pose)
{
	const size_t normaloffs = offsetof (meshxyz_mdl16_t, normal);
	return currententity->model->meshvboptr+(hdr->vbovertofs + (hdr->numverts_vbo * pose * sizeof (meshxyz_mdl16_t)) + normaloffs);
}
static void *GLARB_GetNormalOffset_MD3 (aliashdr_t *hdr, int pose)
{
	const size_t normaloffs = offsetof (meshxyz_md3_t, normal);
	return currententity->model->meshvboptr+(hdr->vbovertofs + (hdr->numverts_vbo * pose * sizeof (meshxyz_md3_t)) + normaloffs);
}


void R_Shadow_DrawAliasFrame (r_shadow_light_t* light, shadow_aliasglsl_t* glsl, aliashdr_t* paliashdr, lerpdata_t* lerpdata, entity_t* ent)
{
	float entalpha = (ent != NULL) ? ENTALPHA_DECODE(ent->alpha) : 1.0f;

	glDisable (GL_CULL_FACE);

	float blend;
	if (lerpdata->pose1 != lerpdata->pose2)
	{
		blend = lerpdata->blend;
	}
	else // poses the same means either 1. the entity has paused its animation, or 2. r_lerpmodels is disabled
	{
		blend = 0;
	}

	GL_UseProgramFunc (glsl->shader.program_id);
	
// Bind the buffers
	GL_BindBuffer (GL_ARRAY_BUFFER, ent->model->meshvbo);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, ent->model->meshindexesvbo);

	GL_EnableVertexAttribArrayFunc (texCoordsAttrIndex);
	GL_EnableVertexAttribArrayFunc (pose1VertexAttrIndex);
	GL_EnableVertexAttribArrayFunc (pose2VertexAttrIndex);
	GL_EnableVertexAttribArrayFunc (pose1NormalAttrIndex);
	GL_EnableVertexAttribArrayFunc (pose2NormalAttrIndex);

	switch (paliashdr->poseverttype)
	{
	case PV_QUAKE1:
		GL_VertexAttribPointerFunc (texCoordsAttrIndex, 2, GL_FLOAT, GL_FALSE, 0, ent->model->meshvboptr+paliashdr->vbostofs);

		GL_VertexAttribPointerFunc (pose1VertexAttrIndex, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof (meshxyz_mdl_t), GLARB_GetXYZOffset_MDL (paliashdr, lerpdata->pose1));
		GL_VertexAttribPointerFunc (pose2VertexAttrIndex, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof (meshxyz_mdl_t), GLARB_GetXYZOffset_MDL (paliashdr, lerpdata->pose2));
		// GL_TRUE to normalize the signed bytes to [-1 .. 1]
		GL_VertexAttribPointerFunc (pose1NormalAttrIndex, 4, GL_BYTE, GL_TRUE, sizeof (meshxyz_mdl_t), GLARB_GetNormalOffset_MDL (paliashdr, lerpdata->pose1));
		GL_VertexAttribPointerFunc (pose2NormalAttrIndex, 4, GL_BYTE, GL_TRUE, sizeof (meshxyz_mdl_t), GLARB_GetNormalOffset_MDL (paliashdr, lerpdata->pose2));
		break;
	case PV_QUAKEFORGE:
		GL_VertexAttribPointerFunc (texCoordsAttrIndex, 2, GL_FLOAT, GL_FALSE, 0, ent->model->meshvboptr+paliashdr->vbostofs);

		GL_VertexAttribPointerFunc (pose1VertexAttrIndex, 4, GL_UNSIGNED_SHORT, GL_FALSE, sizeof (meshxyz_mdl16_t), GLARB_GetXYZOffset_MDLQF (paliashdr, lerpdata->pose1));
		GL_VertexAttribPointerFunc (pose2VertexAttrIndex, 4, GL_UNSIGNED_SHORT, GL_FALSE, sizeof (meshxyz_mdl16_t), GLARB_GetXYZOffset_MDLQF (paliashdr, lerpdata->pose2));
		// GL_TRUE to normalize the signed bytes to [-1 .. 1]
		GL_VertexAttribPointerFunc (pose1NormalAttrIndex, 4, GL_BYTE, GL_TRUE, sizeof (meshxyz_mdl16_t), GLARB_GetNormalOffset_MDLQF (paliashdr, lerpdata->pose1));
		GL_VertexAttribPointerFunc (pose2NormalAttrIndex, 4, GL_BYTE, GL_TRUE, sizeof (meshxyz_mdl16_t), GLARB_GetNormalOffset_MDLQF (paliashdr, lerpdata->pose2));
		break;
	case PV_QUAKE3:
		GL_VertexAttribPointerFunc (texCoordsAttrIndex, 2, GL_FLOAT, GL_FALSE, 0, ent->model->meshvboptr+paliashdr->vbostofs);

		GL_VertexAttribPointerFunc (pose1VertexAttrIndex, 4, GL_SHORT, GL_FALSE, sizeof (meshxyz_md3_t), GLARB_GetXYZOffset_MD3 (paliashdr, lerpdata->pose1));
		GL_VertexAttribPointerFunc (pose2VertexAttrIndex, 4, GL_SHORT, GL_FALSE, sizeof (meshxyz_md3_t), GLARB_GetXYZOffset_MD3 (paliashdr, lerpdata->pose2));
		// GL_TRUE to normalize the signed bytes to [-1 .. 1]
		GL_VertexAttribPointerFunc (pose1NormalAttrIndex, 4, GL_BYTE, GL_TRUE, sizeof (meshxyz_md3_t), GLARB_GetNormalOffset_MD3 (paliashdr, lerpdata->pose1));
		GL_VertexAttribPointerFunc (pose2NormalAttrIndex, 4, GL_BYTE, GL_TRUE, sizeof (meshxyz_md3_t), GLARB_GetNormalOffset_MD3 (paliashdr, lerpdata->pose2));
		break;
	case PV_IQM:
		{
			const iqmvert_t *pose = (const iqmvert_t*)(ent->model->meshvboptr+paliashdr->vbovertofs + (paliashdr->numverts_vbo * 0 * sizeof (iqmvert_t)));

			GL_VertexAttribPointerFunc (pose1VertexAttrIndex, 3, GL_FLOAT, GL_FALSE, sizeof (iqmvert_t), pose->xyz);
			GL_VertexAttribPointerFunc (pose1NormalAttrIndex, 3, GL_FLOAT, GL_FALSE, sizeof (iqmvert_t), pose->norm);
			GL_VertexAttribPointerFunc (boneWeightAttrIndex, 4, GL_FLOAT, GL_FALSE, sizeof (iqmvert_t), pose->weight);
			GL_VertexAttribPointerFunc (boneIndexAttrIndex, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof (iqmvert_t), pose->idx);
			GL_VertexAttribPointerFunc (texCoordsAttrIndex, 2, GL_FLOAT, GL_FALSE, sizeof (iqmvert_t), pose->st);

			GL_EnableVertexAttribArrayFunc (vertColoursAttrIndex);
			GL_VertexAttribPointerFunc (vertColoursAttrIndex, 4, GL_FLOAT, GL_FALSE, sizeof (iqmvert_t), pose->rgba);
		}
		break;
	}
	
// set uniforms
	if (glsl->blendLoc != -1)
		GL_Uniform1fFunc (glsl->blendLoc, blend);
	if (glsl->bonesLoc != -1)
		GL_Uniform4fvFunc (glsl->bonesLoc, paliashdr->numbones*3, lerpdata->bonestate->mat);
	GL_Uniform1iFunc (glsl->texLoc, 0);
	GL_Uniform1fFunc (glsl->alphaLoc, entalpha);
	GL_UniformMatrix4fvFunc (glsl->shadowMatrixLoc, 1, false, (const GLfloat*)light->shadow_map_projview);
	GL_Uniform1iFunc (glsl->debugLoc, r_shadow_sundebug.value);

	mat4_t model_matrix;
	if (ent) {
		Matrix4_InitTranslationAndRotation (lerpdata->origin, lerpdata->angles, model_matrix);
		Matrix4_Translate (model_matrix, paliashdr->scale_origin, model_matrix);
		Matrix4_Scale (model_matrix, paliashdr->scale, model_matrix);
	}
	else {
		Matrix4_InitIdentity (model_matrix);
	}
	GL_UniformMatrix4fvFunc (glsl->modelMatrixLoc, 1, false, (const GLfloat*)model_matrix);

// draw
	glDrawElements (GL_TRIANGLES, paliashdr->numindexes, GL_UNSIGNED_SHORT, ent->model->meshindexesvboptr+paliashdr->eboofs);

// clean up
	GL_DisableVertexAttribArrayFunc (texCoordsAttrIndex);
	GL_DisableVertexAttribArrayFunc (pose1VertexAttrIndex);
	GL_DisableVertexAttribArrayFunc (pose2VertexAttrIndex);
	GL_DisableVertexAttribArrayFunc (pose1NormalAttrIndex);
	GL_DisableVertexAttribArrayFunc (pose2NormalAttrIndex);
	GL_DisableVertexAttribArrayFunc (vertColoursAttrIndex);

	GL_UseProgramFunc (0);
	GL_BindBuffer (GL_ARRAY_BUFFER, 0);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);
}

void R_Shadow_DrawAliasModel (r_shadow_light_t* light, entity_t *e)
{
	shadow_aliasglsl_t *glsl;
	aliashdr_t	*paliashdr;
	int			i, anim, skinnum;
	gltexture_t	*tx, *fb;
	lerpdata_t	lerpdata;
	qboolean	alphatest = !!(e->model->flags & MF_HOLEY);
	int surf;

	if (e->eflags & EFLAGS_VIEWMODEL) { return; }

	//
	// setup pose/lerp data -- do it first so we don't miss updates due to culling
	//
	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);
	R_SetupAliasFrame (paliashdr, e, &lerpdata);
	R_SetupEntityTransform (e, &lerpdata);

	glsl = &shadow_alias_glsl[(paliashdr->poseverttype==PV_IQM)?ALIAS_GLSL_SKELETAL:ALIAS_GLSL_BASIC];

	//
	// random stuff
	//
	if (gl_affinemodels.value)
		glHint (GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);

	for(surf=0;;surf++)
	{
		// rs_aliaspolys += paliashdr->numtris;

		//
		// draw it
		//
		R_Shadow_DrawAliasFrame (light, glsl, paliashdr, &lerpdata, e);

		if (!paliashdr->nextsurface)
			break;
		paliashdr = (aliashdr_t*)((byte*)paliashdr + paliashdr->nextsurface);
	}

cleanup:
	glHint (GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
}
#endif

void R_Shadow_DrawEntities (r_shadow_light_t* light)
{
	int		i;

	if (!r_drawentities.value)
		return;

	//johnfitz -- sprites are not a special case
	for (i=0 ; i<cl_numvisedicts ; i++)
	{
		currententity = cl_visedicts[i];

		//johnfitz -- chasecam
		if (currententity == &cl.entities[cl.viewentity])
			currententity->angles[0] *= 0.3;
		//johnfitz

		//spike -- this would be more efficient elsewhere, but its more correct here.
		if (currententity->eflags & EFLAGS_EXTERIORMODEL)
			continue;

		switch (currententity->model->type)
		{
			case mod_alias:
				R_Shadow_DrawAliasModel (light, currententity);
				break;
			case mod_brush:
				R_Shadow_DrawBrushModel (light, currententity);
				break;
			case mod_sprite:
				//R_DrawSpriteModel (currententity);
				break;
			case mod_ext_invalid:
				//nothing. could draw a blob instead.
				break;
		} 
	}
}

static void R_Shadow_PrepareToRender (r_shadow_light_t* light)
{
	R_MarkSurfacesForLightShadowMap (light);
	if (r_shadow_sundebug.value) {
		glViewport (0, 0, 1024, 1024);
		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}
	else {
		GL_BindFramebufferFunc (GL_FRAMEBUFFER, light->shadow_map_fbo);
		glViewport (0, 0, light->shadow_map_width, light->shadow_map_height);
		glClear (GL_DEPTH_BUFFER_BIT);
	}
}

static void R_Shadow_RenderSunShadowMap (r_shadow_light_t* light)
{
	if (!r_shadow_sun.value) { return; }

	light->brighten = r_shadow_sunbrighten.value;
	light->darken = r_shadow_sundarken.value;

	R_Shadow_PrepareToRender (light);
	R_Shadow_DrawTextureChains (light, cl.worldmodel, NULL, chain_world);
	R_Shadow_DrawEntities (light);
	last_light_rendered = light;
	light->rendered = true;
}

static void R_Shadow_RenderSpotShadowMap (r_shadow_light_t* light)
{
	light->brighten = r_shadow_sunbrighten.value;
	light->darken = r_shadow_sundarken.value;

	R_Shadow_PrepareToRender (light);
	R_Shadow_DrawTextureChains (light, cl.worldmodel, NULL, chain_world);
	R_Shadow_DrawEntities (light);
	last_light_rendered = light;
	light->rendered = true;
}

static void R_Shadow_AddLightToUniformBuffer (r_shadow_light_t* light)
{
	if (shadow_ubo_data.num_shadow_maps >= MAX_FRAME_SHADOWS) {
		Con_DWarning ("Shadow map limit reached, max: %d\n", MAX_FRAME_SHADOWS);
		return;
	}

	shadow_ubo_single_t* ldata = &shadow_ubo_data.shadows[shadow_ubo_data.num_shadow_maps++];
	ldata->light_type = (int)light->type;
	ldata->brighten = light->brighten;
	ldata->darken = light->darken;
	ldata->bias = light->bias;
	ldata->radius = light->radius;
	ldata->spot_cutoff = 0.3f;
	VectorCopy (light->light_position, ldata->light_position);
	VectorCopy (light->light_normal, ldata->light_normal);
	memcpy (ldata->shadow_matrix, light->world_to_shadow_map, sizeof(mat4_t));

	shadow_frame_textures[shadow_ubo_data.num_shadow_maps - 1].id = light->shadow_map_texture;
	shadow_frame_textures[shadow_ubo_data.num_shadow_maps - 1].unit = SHADOW_MAP_TEXTURE_UNIT - GL_TEXTURE0 + light->id;
}

//
// For now, just check if the light is inside a certain radius of the player.
// FIXME: Implement better/proper culling.
//
static qboolean R_Shadow_CullLight(const r_shadow_light_t* light)
{
	const float CULL_RADIUS = 1024.0f;
	vec3_t dist;
	VectorSubtract (r_refdef.vieworg, light->light_position, dist);
	return (VectorLength(dist) <= CULL_RADIUS);
}

static void R_Shadow_UpdateUniformBuffer ()
{
	if (!shadow_ubo) {
		GL_GenBuffersFunc (1, &shadow_ubo);
		GL_BindBufferFunc (GL_UNIFORM_BUFFER, shadow_ubo);
		GL_BufferDataFunc (GL_UNIFORM_BUFFER, sizeof(shadow_ubo_data_t), &shadow_ubo_data, GL_DYNAMIC_DRAW);
		GL_BindBufferFunc (GL_UNIFORM_BUFFER, 0);
		GL_BindBufferBaseFunc (GL_UNIFORM_BUFFER, SHADOW_UBO_BINDING_POINT, shadow_ubo);
	}

	shadow_ubo_data.use_shadow = (int)r_shadow_sun.value;
	shadow_ubo_data.num_shadow_maps = 0;

	for (r_shadow_light_t* light = first_light; light; light = light->next) {
		if (light->rendered) {
			R_Shadow_AddLightToUniformBuffer (light);
		}
	}

	GL_BindBufferFunc (GL_UNIFORM_BUFFER, shadow_ubo);
	GL_BufferDataFunc (GL_UNIFORM_BUFFER, sizeof(shadow_ubo_data_t), &shadow_ubo_data, GL_DYNAMIC_DRAW);
	GL_BindBufferFunc (GL_UNIFORM_BUFFER, 0);
}

GLuint R_Shadow_GetUniformBuffer ()
{
	return shadow_ubo;
}

void R_Shadow_BindTextures (const GLuint* sampler_locations)
{
	for (int i = 0; i < shadow_ubo_data.num_shadow_maps; i++) {
		GL_SelectTextureFunc (GL_TEXTURE0 + shadow_frame_textures[i].unit);
		glBindTexture (GL_TEXTURE_2D, shadow_frame_textures[i].id);
		GL_Uniform1iFunc (sampler_locations[i], shadow_frame_textures[i].unit);
	}
}

void R_Shadow_RenderShadowMap ()
{
	last_light_rendered = NULL;

	for (r_shadow_light_t* light = first_light; light; light = light->next) {
		light->rendered = false;
		switch (light->type) {
		case r_shadow_light_type_sun:
			R_Shadow_RenderSunShadowMap (light);
			break;
		case r_shadow_light_type_spot:
			if (R_Shadow_CullLight(light)) {
				R_Shadow_RenderSpotShadowMap (light);
			}
			break;
		}
	}

	if (!r_shadow_sundebug.value && last_light_rendered != NULL) {
		R_Shadow_UpdateUniformBuffer ();

		// Restore the original viewport

		GL_BindFramebufferFunc (GL_FRAMEBUFFER, 0);

		int scale;

		//johnfitz -- rewrote this section
		if (!r_refdef.drawworld)
			scale = 1;	//don't rescale. we can't handle rescaling transparent parts.
		else
			scale = CLAMP(1, (int)r_scale.value, 4); // ericw -- see R_ScaleView
		glViewport(glx + r_refdef.vrect.x,
			gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height,
			r_refdef.vrect.width / scale,
			r_refdef.vrect.height / scale);
		//johnfitz
	}

	R_MarkSurfaces (); // Mark surfaces here because we disabled in R_SetupView
}

r_shadow_light_t* R_Shadow_GetSunLight ()
{
	return sun_light;
}

static void R_Shadow_Cleanup ()
{
}

static const GLchar *shadow_brush_vertex_shader = \
	"#version 330 core\n"
	"\n"
	"layout (location=0) in vec3 Vert;\n"
	"layout (location=1) in vec2 aTexCoord;\n"
	"\n"
	"uniform mat4 ShadowMatrix;\n"
	"uniform mat4 ModelMatrix;\n"
	"\n"
	"\nsmooth out vec2 texCoord;\n"
	"\n"
	"void main()\n"
	"{\n"
	"	gl_Position = ShadowMatrix * ModelMatrix * vec4(Vert, 1.0);\n"
	"   texCoord = aTexCoord;\n"
	"}\n";

static const GLchar *shadow_brush_fragment_shader = \
	"#version 330 core\n"
	"\n"
	"uniform sampler2D Tex;\n"
	"uniform int Debug;\n"
	"\n"
	"smooth in vec2 texCoord;\n"
	"\n"
	"out vec4 ccolor;\n"
	"//out float fragmentdepth;\n"
	"\n"
	"void main()\n"
	"{\n"
	"   if (Debug == 1) { ccolor = vec4(gl_FragCoord.z); }\n"
	"   else if (Debug == 2) { ccolor = texture2D(Tex, texCoord); }\n"
	"   else { vec4 texcol = texture2D(Tex, texCoord); if (texcol.a<0.1) { discard; } else { gl_FragDepth = gl_FragCoord.z; } }\n"
	"}\n";


static const GLchar *shadow_alias_vertex_shader = \
	"#version 110\n"
	"%s"
	"\n"
	"attribute vec4 Pose1Vert;\n"
	"attribute vec4 Pose1Normal;\n"

	"#ifdef SKELETAL\n"
	"#define BoneWeight Pose2Vert\n"
	"#define BoneIndex Pose2Normal\n"
	"attribute vec4 BoneWeight;\n"
	"attribute vec4 BoneIndex;\n"
	"attribute vec4 VertColours;\n"
	"uniform vec4 BoneTable[MAXBONES*3];\n" //fixme: should probably try to use a UBO or SSBO.
	"#else\n"

	"uniform float Blend;\n"
	"attribute vec4 Pose2Vert;\n"
	"attribute vec4 Pose2Normal;\n"

	"#endif\n"

	"attribute vec2 TexCoords; // only xy are used \n"

	"uniform mat4 ShadowMatrix;\n"
	"uniform mat4 ModelMatrix;\n"

	"void main()\n"
	"{\n"
	"	gl_TexCoord[0] = vec4(TexCoords, 0.0, 1.0);\n"
	"#ifdef SKELETAL\n"
	"	mat4 wmat;"
	"	wmat[0]  = BoneTable[0+3*int(BoneIndex.x)] * BoneWeight.x;"
	"	wmat[0] += BoneTable[0+3*int(BoneIndex.y)] * BoneWeight.y;"
	"	wmat[0] += BoneTable[0+3*int(BoneIndex.z)] * BoneWeight.z;"
	"	wmat[0] += BoneTable[0+3*int(BoneIndex.w)] * BoneWeight.w;"
	"	wmat[1]  = BoneTable[1+3*int(BoneIndex.x)] * BoneWeight.x;"
	"	wmat[1] += BoneTable[1+3*int(BoneIndex.y)] * BoneWeight.y;"
	"	wmat[1] += BoneTable[1+3*int(BoneIndex.z)] * BoneWeight.z;"
	"	wmat[1] += BoneTable[1+3*int(BoneIndex.w)] * BoneWeight.w;"
	"	wmat[2]  = BoneTable[2+3*int(BoneIndex.x)] * BoneWeight.x;"
	"	wmat[2] += BoneTable[2+3*int(BoneIndex.y)] * BoneWeight.y;"
	"	wmat[2] += BoneTable[2+3*int(BoneIndex.z)] * BoneWeight.z;"
	"	wmat[2] += BoneTable[2+3*int(BoneIndex.w)] * BoneWeight.w;"
	"	wmat[3] = vec4(0.0,0.0,0.0,1.0);\n"
	"	vec4 lerpedVert = (vec4(Pose1Vert.xyz, 1.0) * wmat);\n"
	"#else\n"
	"	vec4 lerpedVert = mix(vec4(Pose1Vert.xyz, 1.0), vec4(Pose2Vert.xyz, 1.0), Blend);\n"
	"#endif\n"
	"	gl_Position = ShadowMatrix * ModelMatrix * lerpedVert;\n"
	"}\n";

static const GLchar *shadow_alias_fragment_shader = \
	"#version 330 core\n"
	"\n"
	"uniform sampler2D Tex;\n"
	"uniform int Debug;\n"
	"uniform float Alpha;\n"
	"\n"
	"smooth in vec2 texCoord;\n"
	"\n"
	"out vec4 ccolor;\n"
	"//out float fragmentdepth;\n"
	"\n"
	"void main()\n"
	"{\n"
	"   if (Alpha < 0.1) { discard; }\n"
	"   if (Debug == 1) { ccolor = vec4(gl_FragCoord.z); }\n"
	"   else if (Debug == 2) { ccolor = texture2D(Tex, texCoord); }\n"
	"   else { vec4 texcol = texture2D(Tex, texCoord); if (texcol.a<0.1) { discard; } else { gl_FragDepth = gl_FragCoord.z; } }\n"
	"}\n";