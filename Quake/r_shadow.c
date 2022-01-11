//
// This file contains the implementation for shadow mapping of a single directional light (the sun)
//

#include "quakedef.h"
#include "glquake.h"

enum {
    SHADOW_WIDTH = 1024*4,
    SHADOW_HEIGHT = 1024*4,
};

cvar_t r_shadow_sun = {"r_shadow_sun", "1", CVAR_ARCHIVE, 1.0f};
cvar_t r_shadow_sundebug = {"r_shadow_sundebug", "0", CVAR_NONE, 0.0f};
cvar_t r_shadow_sunbrighten = {"r_shadow_sunbrighten", "0.2", CVAR_NONE, 0.2f};
cvar_t r_shadow_sundarken = {"r_shadow_sundarken", "0.4", CVAR_NONE, 0.4f};

static struct {
    vec3_t sun_glangle;
    gl_shader_t shadow_alias_shader;
    mat4_t shadow_pv_matrix;
    GLuint shadow_fbo;
    GLuint shadow_depth_tex;
    qboolean ok;
} state;

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

static int num_shadow_alias_glsl;
static shadow_aliasglsl_t shadow_alias_glsl[ALIAS_GLSL_MODES];

static const char* shadow_brush_vertex_shader;
static const char* shadow_brush_fragment_shader;
static const char *shadow_alias_vertex_shader;
static const char *shadow_alias_fragment_shader;

static vec3_t current_sun_pos;
static vec3_t debug_sun_pos;
static qboolean debug_override_sun_pos;

static void R_Shadow_SetAngle_f ()
{
    if (Cmd_Argc() < 4) {
        Con_Printf ("Current sun shadow angle: %5.1f %5.1f %5.1f\n", state.sun_glangle[1], -state.sun_glangle[0], state.sun_glangle[2]);
        Con_Printf ("Usage: r_shadow_sunangle <yaw> <pitch> <roll>\n");
        return;
    }

    float yaw = Q_atof (Cmd_Argv(1));
    float pitch = Q_atof (Cmd_Argv(2));
    float roll = Q_atof (Cmd_Argv(3));

    R_Shadow_SetupSun ((const vec3_t){ yaw, pitch, roll });
}

static void R_Shadow_SetPos_f()
{
	if (Cmd_Argc() < 4) {
        Con_Printf ("Current sun shadow pos: %5.1f %5.1f %5.1f\n", current_sun_pos[0], current_sun_pos[1], current_sun_pos[2]);
        Con_Printf ("Usage: r_shadow_sunangle <yaw> <pitch> <roll>\n");
        return;
    }

    debug_sun_pos[0] = Q_atof (Cmd_Argv(1));
    debug_sun_pos[1] = Q_atof (Cmd_Argv(2));
    debug_sun_pos[2] = Q_atof (Cmd_Argv(3));

	debug_override_sun_pos = true;

    R_Shadow_SetupSun ((const vec3_t){ state.sun_glangle[1], -state.sun_glangle[0], state.sun_glangle[2] });
}

void R_Shadow_Init ()
{
	Cvar_RegisterVariable (&r_shadow_sun);
	Cvar_RegisterVariable (&r_shadow_sundebug);
	Cvar_RegisterVariable (&r_shadow_sunbrighten);
	Cvar_RegisterVariable (&r_shadow_sundarken);
	Cmd_AddCommand ("r_shadow_sunangle", R_Shadow_SetAngle_f);
	Cmd_AddCommand ("r_shadow_sunpos", R_Shadow_SetPos_f);
}

static void R_Shadow_CreateBrushShaders ()
{
    if (!GL_CreateShaderFromVF (&shadow_brush_glsl.shader, shadow_brush_vertex_shader, shadow_brush_fragment_shader)) {
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
		glsl->shader.program_id = GL_CreateProgram(processedVertSource, shadow_alias_fragment_shader, sizeof(bindings) / sizeof(bindings[0]), bindings);
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

static void R_Shadow_CreateFramebuffer()
{
    GL_GenFramebuffersFunc (1, &state.shadow_fbo);
    GL_BindFramebufferFunc (GL_FRAMEBUFFER, state.shadow_fbo);

    glGenTextures (1, &state.shadow_depth_tex);
    glBindTexture (GL_TEXTURE_2D, state.shadow_depth_tex);

    glTexImage2D (GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, SHADOW_WIDTH, SHADOW_HEIGHT, 0, GL_DEPTH_COMPONENT, GL_FLOAT, 0);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);

    GL_FramebufferTextureFunc (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, state.shadow_depth_tex, 0);

    glDrawBuffer (GL_NONE); // No color buffer is drawn to.

    if (GL_CheckFramebufferStatusFunc(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        Con_Warning ("Failed to create Sun Shadow Framebuffer\n");
        return;
    }

    GL_BindFramebufferFunc (GL_FRAMEBUFFER, 0);
}

void R_Shadow_SetupSun (vec3_t angle)
{
    if (!r_shadow_sun.value) { return; }

    if (!shadow_brush_glsl.shader.program_id) {
        R_Shadow_CreateBrushShaders ();
    }
    if (num_shadow_alias_glsl < ALIAS_GLSL_MODES) {
        R_Shadow_CreateAliasShaders ();
    }
    if (!state.shadow_depth_tex) {
        R_Shadow_CreateFramebuffer ();
    }

    //
    // We accept angles in the form of (Yaw Pitch Roll) to maintain consistency
    // with other mapping options, but Quake internally accepts (Pitch Yaw Roll)
    // so here we need to convert from one to the other.
    //
    state.sun_glangle[0] = -angle[1];
    state.sun_glangle[1] = -angle[0];
    state.sun_glangle[2] = angle[2];

    vec3_t pos = {0,0,0};
    vec3_t mins;
    vec3_t maxs;
    vec3_t worldsize;

    VectorCopy (cl.worldmodel->mins, mins);
    VectorCopy (cl.worldmodel->maxs, maxs);
    VectorSubtract (maxs, mins, worldsize);

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

	vec3_t fwd, right, up;
	AngleVectors (state.sun_glangle, fwd, right, up);

	Con_Printf ("fwd: (%f, %f, %f)\n", fwd[0], fwd[1], fwd[2]);

	vec3_t view_angle = { -state.sun_glangle[0], -state.sun_glangle[1], state.sun_glangle[2] };
	mat4_t view_matrix;
	// Matrix4_LookAt(pos, fwd, vec3_z, view_matrix);
	Matrix4_ViewMatrix (view_angle, pos, view_matrix);

	vec4_t view_world_corners[8];
	for (int i = 0; i < countof(world_corners); i++) {
		Matrix4_Transform4 (view_matrix, world_corners[i], view_world_corners[i]);
	}

	vec4_t view_mins;
	vec4_t view_maxs;
	Matrix4_Transform4 (view_matrix, (const vec4_t){mins[0], mins[1], mins[2], 1.0f}, view_mins);
	Matrix4_Transform4 (view_matrix, (const vec4_t){maxs[0], maxs[1], maxs[2], 1.0f}, view_maxs);

	Con_Printf("mins.x: %f, maxs.x: %f\n", mins[0], maxs[0]);
	Con_Printf("mins.y: %f, maxs.y: %f\n", mins[1], maxs[1]);
	Con_Printf("mins.z: %f, maxs.z: %f\n", mins[2], maxs[2]);
	// Con_Printf("view_mins.z: %f, view_maxs.z: %f\n", view_mins[2], view_maxs[2]);

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

	Con_Printf ("proj_mins.x: %f, proj_maxs.x: %f\n", proj_mins[0], proj_maxs[0]);
	Con_Printf ("proj_mins.y: %f, proj_maxs.y: %f\n", proj_mins[1], proj_maxs[1]);
	Con_Printf ("proj_mins.z: %f, proj_maxs.z: %f\n", proj_mins[2], proj_maxs[2]);

	const float depth = (proj_mins[2] + proj_maxs[2]) * 0.5f;

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
		proj_mins[2]*scale,proj_maxs[2]*scale,

		proj_matrix
	);

    // Matrix4_ProjectionMatrix(r_fovx, r_fovy, 0.1f, 16384, false, 0, 0, proj_matrix);

	mat4_t render_view_matrix;
	// Matrix4_LookAt(pos, fwd, vec3_z, render_view_matrix);
	Matrix4_ViewMatrix (state.sun_glangle, pos, render_view_matrix);

    Matrix4_Multiply (proj_matrix, render_view_matrix, state.shadow_pv_matrix);
}

//
// Functions for drawing stuff to the Shadow Map
//

extern GLuint gl_bmodel_vbo;
extern qboolean r_drawingsunshadow;

qboolean r_drawingsunshadow = false;

static void R_Shadow_DrawTextureChains (qmodel_t *model, entity_t *ent, texchain_t chain)
{
    float entalpha = (ent != NULL) ? ENTALPHA_DECODE(ent->alpha) : 1.0f;

    glDisable (GL_CULL_FACE);

	GL_UseProgramFunc (shadow_brush_glsl.shader.program_id);
	
// Bind the buffers
	GL_BindBuffer (GL_ARRAY_BUFFER, gl_bmodel_vbo);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0); // indices come from client memory!

	GL_EnableVertexAttribArrayFunc (0);
	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), ((float *)0));

    GL_EnableVertexAttribArrayFunc (1);
    GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), ((float*)0) + 3);
	
// set uniforms
	GL_Uniform1iFunc (shadow_brush_glsl.u_Tex, 0);
    GL_Uniform1iFunc (shadow_brush_glsl.u_UseAlphaTest, 0);
	GL_Uniform1fFunc (shadow_brush_glsl.u_Alpha, entalpha);
    GL_UniformMatrix4fvFunc (shadow_brush_glsl.u_ShadowMatrix, 1, false, (const GLfloat*)state.shadow_pv_matrix);
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

			GL_SelectTexture(GL_TEXTURE1);
			GL_Bind(lightmaps[s->lightmaptexturenum].texture);
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
}

void R_Shadow_DrawBrushModel (entity_t* e)
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

	R_Shadow_DrawTextureChains (clmodel, e, chain_model);
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


void R_Shadow_DrawAliasFrame (shadow_aliasglsl_t* glsl, aliashdr_t* paliashdr, lerpdata_t* lerpdata, entity_t* ent)
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
    GL_UniformMatrix4fvFunc (glsl->shadowMatrixLoc, 1, false, (const GLfloat*)state.shadow_pv_matrix);
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

void R_Shadow_DrawAliasModel (entity_t *e)
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
		R_Shadow_DrawAliasFrame (glsl, paliashdr, &lerpdata, e);

		if (!paliashdr->nextsurface)
			break;
		paliashdr = (aliashdr_t*)((byte*)paliashdr + paliashdr->nextsurface);
	}

cleanup:
	glHint (GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
}
#endif

void R_Shadow_DrawEntities ()
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
                R_Shadow_DrawAliasModel (currententity);
				break;
			case mod_brush:
				R_Shadow_DrawBrushModel (currententity);
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

void R_Shadow_RenderShadowMap ()
{
    if (!r_shadow_sun.value) { return; }

    r_drawingsunshadow = true;

	R_MarkSurfacesForSunShadowMap ();

    if (r_shadow_sundebug.value) {
        glViewport (0, 0, 1024, 1024);
        glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    else {
        GL_BindFramebufferFunc (GL_FRAMEBUFFER, state.shadow_fbo);
        glViewport (0, 0, SHADOW_WIDTH, SHADOW_HEIGHT);
        glClear (GL_DEPTH_BUFFER_BIT);
    }

    R_Shadow_DrawTextureChains (cl.worldmodel, NULL, chain_world);

    R_Shadow_DrawEntities ();

    r_drawingsunshadow = false;

    if (!r_shadow_sundebug.value) {
        GL_BindFramebufferFunc (GL_FRAMEBUFFER, 0);

		// Restore the original viewport
        
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

        R_MarkSurfaces (); // Mark surfaces here because we disabled in R_SetupView
    }
}

void R_Shadow_GetDepthTextureAndMatrix (GLuint* out_texture, mat4_t out_matrix)
{
    *out_texture = state.shadow_depth_tex;

    mat4_t shadow_bias_matrix = {
        0.5, 0.0, 0.0, 0.0,
        0.0, 0.5, 0.0, 0.0,
        0.0, 0.0, 0.5, 0.0,
        0.5, 0.5, 0.5, 1.0
    };
    Matrix4_Multiply (shadow_bias_matrix, state.shadow_pv_matrix, out_matrix);
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
    "out float fragmentdepth;\n"
    "\n"
    "void main()\n"
    "{\n"
    "   if (Debug == 1) { ccolor = vec4(gl_FragCoord.z); }\n"
    "   else if (Debug == 2) { ccolor = texture2D(Tex, texCoord); }\n"
    "   else { fragmentdepth = gl_FragCoord.z; }\n"
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
    "out float fragmentdepth;\n"
    "\n"
    "void main()\n"
    "{\n"
    "   if (Alpha < 0.1) { discard; }\n"
    "   if (Debug == 1) { ccolor = vec4(gl_FragCoord.z); }\n"
    "   else if (Debug == 2) { ccolor = texture2D(Tex, texCoord); }\n"
    "   else { fragmentdepth = gl_FragCoord.z; }\n"
    "}\n";