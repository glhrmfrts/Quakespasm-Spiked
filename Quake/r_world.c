/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

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
// r_world.c: world model rendering

#include "quakedef.h"
#include "r_shadow_glsl.h"
#include "gl_random_texture.h"
#include "gl_fog.h"
#include "gl_rlight_glsl.h"

extern cvar_t gl_fullbrights, r_drawflat, gl_overbright, r_oldskyleaf, r_showtris; //johnfitz

extern GLuint gl_bmodel_vbo;

extern cvar_t r_shadow_sunbrighten;
extern cvar_t r_shadow_sundarken;

extern mat4_t r_projection_view_matrix;

byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel);

//==============================================================================
//
// SETUP CHAINS
//
//==============================================================================

/*
================
R_ClearTextureChains -- ericw 

clears texture chains for all textures used by the given model, and also
clears the lightmap chains
================
*/
void R_ClearTextureChains (qmodel_t *mod, texchain_t chain)
{
	int i;

	// set all chains to null
	for (i=0 ; i<mod->numtextures ; i++)
		if (mod->textures[i])
			mod->textures[i]->texturechains[chain] = NULL;

	// clear lightmap chains
	for (i=0 ; i<lightmap_count ; i++)
		lightmaps[i].polys = NULL;
}

/*
================
R_ChainSurface -- ericw -- adds the given surface to its texture chain
================
*/
void R_ChainSurface (msurface_t *surf, texchain_t chain)
{
	surf->texturechain = surf->texinfo->texture->texturechains[chain];
	surf->texinfo->texture->texturechains[chain] = surf;
}

/*
================
R_BackFaceCull -- johnfitz -- returns true if the surface is facing away from vieworg
================
*/
qboolean R_BackFaceCull (msurface_t *surf)
{
	double dot;

	if (surf->plane->type < 3)
		dot = r_refdef.vieworg[surf->plane->type] - surf->plane->dist;
	else
		dot = DotProduct (r_refdef.vieworg, surf->plane->normal) - surf->plane->dist;

	if ((dot < 0) ^ !!(surf->flags & SURF_PLANEBACK))
		return true;

	return false;
}

void R_MarkSurfacesForLightShadowMap (r_shadow_light_t* light)
{
	mleaf_t		*leaf, *viewleaf;
	msurface_t	*surf, **mark;
	int			i, j;
	byte		*vis;

	switch (light->type) {
	case r_shadow_light_type_sun:
		vis = Mod_NoVisPVS (cl.worldmodel);
		break;
	case r_shadow_light_type_spot:
	case r_shadow_light_type_point:
		viewleaf = Mod_PointInLeaf (light->light_position, cl.worldmodel);
		vis = Mod_LeafPVS (viewleaf, cl.worldmodel);
		break;
	}

	// set all chains to null
	for (i=0 ; i<cl.worldmodel->numtextures ; i++)
		if (cl.worldmodel->textures[i])
			cl.worldmodel->textures[i]->texturechains[chain_world] = NULL;

	r_visframecount++;

	// iterate through leaves, marking surfaces
	leaf = &cl.worldmodel->leafs[1];
	for (i=0 ; i<cl.worldmodel->numleafs ; i++, leaf++)
	{
		if (vis[i>>3] & (1<<(i&7)))
		{
			for (j=0, mark = leaf->firstmarksurface; j<leaf->nummarksurfaces; j++, mark++)
			{
				surf = *mark;
				if (surf->visframe != r_visframecount)
				{
					surf->visframe = r_visframecount;
					R_ChainSurface (surf, chain_world);
				}
			}

			// add static models
			if (leaf->efrags)
				R_StoreEfrags (&leaf->efrags);
		}
	}
}

/*
===============
R_MarkSurfaces -- johnfitz -- mark surfaces based on PVS and rebuild texture chains
===============
*/
void R_MarkSurfaces (void)
{
	byte		*vis;
	mleaf_t		*leaf;
	msurface_t	*surf, **mark;
	int			i, j;
	qboolean	nearwaterportal;

	// clear lightmap chains
	for (i=0 ; i<lightmap_count ; i++)
		lightmaps[i].polys = NULL;

	// check this leaf for water portals
	// TODO: loop through all water surfs and use distance to leaf cullbox
	nearwaterportal = false;
	for (i=0, mark = r_viewleaf->firstmarksurface; i < r_viewleaf->nummarksurfaces; i++, mark++)
		if ((*mark)->flags & SURF_DRAWTURB)
			nearwaterportal = true;

	// choose vis data
	if (r_novis.value || r_viewleaf->contents == CONTENTS_SOLID || r_viewleaf->contents == CONTENTS_SKY)
		vis = Mod_NoVisPVS (cl.worldmodel);
	else if (nearwaterportal)
		vis = SV_FatPVS (r_origin, cl.worldmodel);
	else
		vis = Mod_LeafPVS (r_viewleaf, cl.worldmodel);

	r_visframecount++;

	// set all chains to null
	for (i=0 ; i<cl.worldmodel->numtextures ; i++)
		if (cl.worldmodel->textures[i])
			cl.worldmodel->textures[i]->texturechains[chain_world] = NULL;

	// iterate through leaves, marking surfaces
	leaf = &cl.worldmodel->leafs[1];
	for (i=0 ; i<cl.worldmodel->numleafs ; i++, leaf++)
	{
		if (vis[i>>3] & (1<<(i&7)))
		{
			if (R_CullBox(leaf->minmaxs, leaf->minmaxs + 3))
				continue;

			if (r_oldskyleaf.value || leaf->contents != CONTENTS_SKY)
				for (j=0, mark = leaf->firstmarksurface; j<leaf->nummarksurfaces; j++, mark++)
				{
					surf = *mark;
					if (surf->visframe != r_visframecount)
					{
						surf->visframe = r_visframecount;
						if (!R_CullBox(surf->mins, surf->maxs) && !R_BackFaceCull (surf))
						{
							rs_brushpolys++; //count wpolys here
							R_ChainSurface(surf, chain_world);
							R_RenderDynamicLightmaps(cl.worldmodel, surf);
						}
					}
				}

			// add static models
			if (leaf->efrags)
				R_StoreEfrags (&leaf->efrags);
		}
	}
}

//==============================================================================
//
// DRAW CHAINS
//
//==============================================================================

/*
=============
R_BeginTransparentDrawing -- ericw
=============
*/
static void R_BeginTransparentDrawing (float entalpha)
{
	if (entalpha < 1.0f)
	{
		glDepthMask (GL_FALSE);
		glEnable (GL_BLEND);
		glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glColor4f (1,1,1,entalpha);
	}
}

/*
=============
R_EndTransparentDrawing -- ericw
=============
*/
static void R_EndTransparentDrawing (float entalpha)
{
	if (entalpha < 1.0f)
	{
		glDepthMask (GL_TRUE);
		glDisable (GL_BLEND);
		glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glColor3f (1, 1, 1);
	}
}

/*
================
R_DrawTextureChains_ShowTris -- johnfitz
================
*/
void R_DrawTextureChains_ShowTris (qmodel_t *model, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;
	glpoly_t	*p;

	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];
		if (!t)
			continue;

		if (!gl_glsl_water_able && t->texturechains[chain] && (t->texturechains[chain]->flags & SURF_DRAWTURB))
		{
			for (s = t->texturechains[chain]; s; s = s->texturechain)
				for (p = s->polys->next; p; p = p->next)
				{
					DrawGLTriangleFan (p);
				}
		}
		else
		{
			for (s = t->texturechains[chain]; s; s = s->texturechain)
			{
				DrawGLTriangleFan (s->polys);
			}
		}
	}
}

/*
================
R_DrawTextureChains_Drawflat -- johnfitz
================
*/
void R_DrawTextureChains_Drawflat (qmodel_t *model, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;
	glpoly_t	*p;

	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];
		if (!t)
			continue;

		if (!gl_glsl_water_able  && t->texturechains[chain] && (t->texturechains[chain]->flags & SURF_DRAWTURB))
		{
			for (s = t->texturechains[chain]; s; s = s->texturechain)
				for (p = s->polys->next; p; p = p->next)
				{
					srand((unsigned int) (uintptr_t) p);
					glColor3f (rand()%256/255.0, rand()%256/255.0, rand()%256/255.0);
					DrawGLPoly (p);
					rs_brushpasses++;
				}
		}
		else
		{
			for (s = t->texturechains[chain]; s; s = s->texturechain)
			{
				srand((unsigned int) (uintptr_t) s->polys);
				glColor3f (rand()%256/255.0, rand()%256/255.0, rand()%256/255.0);
				DrawGLPoly (s->polys);
				rs_brushpasses++;
			}
		}
	}
	glColor3f (1,1,1);
	srand ((int) (cl.time * 1000));
}

/*
================
R_DrawTextureChains_Glow -- johnfitz
================
*/
void R_DrawTextureChains_Glow (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;
	gltexture_t	*glt;
	qboolean	bound;

	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];

		if (!t || !t->texturechains[chain] || !(glt = R_TextureAnimation(t, ent != NULL ? ent->frame : 0)->fullbright))
			continue;

		bound = false;

		for (s = t->texturechains[chain]; s; s = s->texturechain)
		{
			if (!bound) //only bind once we are sure we need this texture
			{
				GL_Bind (glt);
				bound = true;
			}
			DrawGLPoly (s->polys);
			rs_brushpasses++;
		}
	}
}

//==============================================================================
//
// VBO SUPPORT
//
//==============================================================================

static unsigned int R_NumTriangleIndicesForSurf (msurface_t *s)
{
	return 3 * (s->numedges - 2);
}

/*
================
R_TriangleIndicesForSurf

Writes out the triangle indices needed to draw s as a triangle list.
The number of indices it will write is given by R_NumTriangleIndicesForSurf.
================
*/
static void R_TriangleIndicesForSurf (msurface_t *s, unsigned int *dest)
{
	int i;
	for (i=2; i<s->numedges; i++)
	{
		*dest++ = s->vbo_firstvert;
		*dest++ = s->vbo_firstvert + i - 1;
		*dest++ = s->vbo_firstvert + i;
	}
}

#define MAX_BATCH_SIZE 4096

static unsigned int vbo_indices[MAX_BATCH_SIZE];
static unsigned int num_vbo_indices;

/*
================
R_ClearBatch
================
*/
void R_ClearBatch ()
{
	num_vbo_indices = 0;
}

/*
================
R_FlushBatch

Draw the current batch if non-empty and clears it, ready for more R_BatchSurface calls.
================
*/
void R_FlushBatch ()
{
	if (num_vbo_indices > 0)
	{
		glDrawElements (GL_TRIANGLES, num_vbo_indices, GL_UNSIGNED_INT, vbo_indices);
		num_vbo_indices = 0;
	}
}

/*
================
R_BatchSurface

Add the surface to the current batch, or just draw it immediately if we're not
using VBOs.
================
*/
void R_BatchSurface (msurface_t *s)
{
	int num_surf_indices;

	num_surf_indices = R_NumTriangleIndicesForSurf (s);

	if (num_vbo_indices + num_surf_indices > MAX_BATCH_SIZE)
		R_FlushBatch();

	R_TriangleIndicesForSurf (s, &vbo_indices[num_vbo_indices]);
	num_vbo_indices += num_surf_indices;
}

/*
================
R_DrawTextureChains_Multitexture -- johnfitz
================
*/
void R_DrawTextureChains_Multitexture (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	int			i, j;
	msurface_t	*s;
	texture_t	*t;
	float		*v;
	qboolean	bound;

	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];

		if (!t || !t->texturechains[chain] || t->texturechains[chain]->flags & (SURF_DRAWTURB | SURF_DRAWTILED | SURF_NOTEXTURE))
			continue;

		bound = false;
		for (s = t->texturechains[chain]; s; s = s->texturechain)
		{
			if (!bound) //only bind once we are sure we need this texture
			{
				GL_Bind ((R_TextureAnimation(t, ent != NULL ? ent->frame : 0))->gltexture);
					
				if (t->texturechains[chain]->flags & SURF_DRAWFENCE)
					glEnable (GL_ALPHA_TEST); // Flip alpha test back on
					
				GL_EnableMultitexture(); // selects TEXTURE1
				bound = true;
			}
			GL_Bind (lightmaps[s->lightmaptexturenum].texture);
			glBegin(GL_POLYGON);
			v = s->polys->verts[0];
			for (j=0 ; j<s->polys->numverts ; j++, v+= VERTEXSIZE)
			{
				GL_MTexCoord2fFunc (GL_TEXTURE0_ARB, v[3], v[4]);
				GL_MTexCoord2fFunc (GL_TEXTURE1_ARB, v[5], v[6]);
				glVertex3fv (v);
			}
			glEnd ();
			rs_brushpasses++;
		}
		GL_DisableMultitexture(); // selects TEXTURE0

		if (bound && t->texturechains[chain]->flags & SURF_DRAWFENCE)
			glDisable (GL_ALPHA_TEST); // Flip alpha test back off
	}
}

/*
================
R_DrawTextureChains_NoTexture -- johnfitz

draws surfs whose textures were missing from the BSP
================
*/
void R_DrawTextureChains_NoTexture (qmodel_t *model, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;
	qboolean	bound;

	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];

		if (!t || !t->texturechains[chain] || !(t->texturechains[chain]->flags & SURF_NOTEXTURE))
			continue;

		bound = false;

		for (s = t->texturechains[chain]; s; s = s->texturechain)
		{
			if (!bound) //only bind once we are sure we need this texture
			{
				GL_Bind (t->gltexture);
				bound = true;
			}
			DrawGLPoly (s->polys);
			rs_brushpasses++;
		}
	}
}

/*
================
R_DrawTextureChains_TextureOnly -- johnfitz
================
*/
void R_DrawTextureChains_TextureOnly (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;
	qboolean	bound;

	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];

		if (!t || !t->texturechains[chain] || t->texturechains[chain]->flags & (SURF_DRAWTURB | SURF_DRAWSKY))
			continue;

		bound = false;

		for (s = t->texturechains[chain]; s; s = s->texturechain)
		{
			if (!bound) //only bind once we are sure we need this texture
			{
				GL_Bind ((R_TextureAnimation(t, ent != NULL ? ent->frame : 0))->gltexture);
					
				if (t->texturechains[chain]->flags & SURF_DRAWFENCE)
					glEnable (GL_ALPHA_TEST); // Flip alpha test back on
					
				bound = true;
			}
			DrawGLPoly (s->polys);
			rs_brushpasses++;
		}

		if (bound && t->texturechains[chain]->flags & SURF_DRAWFENCE)
			glDisable (GL_ALPHA_TEST); // Flip alpha test back off
	}
}

/*
================
GL_WaterAlphaForEntitySurface -- ericw
 
Returns the water alpha to use for the entity and surface combination.
================
*/
float GL_WaterAlphaForEntitySurface (entity_t *ent, msurface_t *s)
{
	float entalpha;
	if (ent == NULL || ent->alpha == ENTALPHA_DEFAULT)
		entalpha = GL_WaterAlphaForSurface(s);
	else
		entalpha = ENTALPHA_DECODE(ent->alpha);
	return entalpha;
}


static struct
{
	GLuint program;

	GLuint light_scale;
	GLuint alpha_scale;
	GLuint time;
	GLuint view_projection_matrix;

	GLuint dlight_data_block_index;
	GLuint fog_data_block_index;
	GLuint shadow_data_block_index;
	GLuint shadow_map_samplers_loc[MAX_FRAME_SHADOWS];
	GLuint shadow_map_cube_samplers_loc[MAX_FRAME_SHADOWS];
} r_water[2];

#define vertAttrIndex 0
#define texCoordsAttrIndex 1
#define LMCoordsAttrIndex 2
#define vertNormalIndex 3

/*
=============
GLWorld_CreateShaders
=============
*/
static void GLWater_CreateShaders (void)
{
	const char *modedefines[countof(r_water)] = {
		"",
		"#define LIT\n"
	};
	const glsl_attrib_binding_t bindings[] = {
		{ "Vert", vertAttrIndex },
		{ "TexCoords", texCoordsAttrIndex },
		{ "LMCoords", LMCoordsAttrIndex },
		{ "Normal", vertNormalIndex },
	};

	// Driver bug workarounds:
	// - "Intel(R) UHD Graphics 600" version "4.6.0 - Build 26.20.100.7263"
	//    crashing on glUseProgram with `vec3 Vert` and
	//    `gl_ModelViewProjectionMatrix * vec4(Vert, 1.0);`. Work around with
	//    making Vert a vec4. (https://sourceforge.net/p/quakespasm/bugs/39/)
	const GLchar *vertSource = \
		"#version 150\n"
		"%s"
		"\n"
		"in vec4 Vert;\n"
		"in vec2 TexCoords;\n"
"#ifdef LIT\n"
		"in vec2 LMCoords;\n"
		"varying vec2 tc_lm;\n"
"#endif\n"

		"in vec3 Normal;\n"

		"uniform mat4 ViewProjectionMatrix;\n"

		SHADOW_VERT_UNIFORMS_GLSL

		"\n"

		"out float FogFragCoord;\n"
		"out vec2 tc_tex;\n"
		"out vec3 v_Normal;\n"

		SHADOW_VERT_OUTPUT_GLSL

		"\n"
		"void main()\n"
		"{\n"
		"	tc_tex = TexCoords;\n"
"#ifdef LIT\n"
		"	tc_lm = LMCoords;\n"
"#endif\n"
		"	gl_Position = ViewProjectionMatrix * Vert;\n"
		"	FogFragCoord = gl_Position.w;\n"

		SHADOW_GET_COORD_GLSL("Vert")

		"   v_Normal = Normal;\n"

		"}\n";

	const GLchar *fragSource = \
		"#version 150\n"
		"%s"
		"\n"
		"uniform sampler2D Tex;\n"
"#ifdef LIT\n"
		"uniform sampler2D LMTex;\n"
		"uniform float LightScale;\n"
		"in vec2 tc_lm;\n"
"#endif\n"
		"uniform float Alpha;\n"
		"uniform float WarpTime;\n"

		DLIGHT_FRAG_UNIFORMS_GLSL

		SHADOW_FRAG_UNIFORMS_GLSL

		FOG_FRAG_UNIFORMS_GLSL

		"\n"
		"in float FogFragCoord;\n"
		"in vec2 tc_tex;\n"
		"in vec3 v_Normal;\n"

		SHADOW_FRAG_INPUT_GLSL

		"out vec4 outColor;\n"

		"\n"
		"void main()\n"
		"{\n"
		"	vec2 ntc = tc_tex;\n"
		//CYCLE 128
		//AMP 8*0x10000
		//SPEED 20
		//	sintable[i] = AMP + sin(i*3.14159*2/CYCLE)*AMP;
		//
		//  r_turb_turb = sintable + ((int)(cl.time*SPEED)&(CYCLE-1));
		//
		//	sturb = ((r_turb_s + r_turb_turb[(r_turb_t>>16)&(CYCLE-1)])>>16)&63;
        //	tturb = ((r_turb_t + r_turb_turb[(r_turb_s>>16)&(CYCLE-1)])>>16)&63;
        //The following 4 lines SHOULD match the software renderer, except normalised coords rather than snapped texels
        "#define M_PI 3.14159\n"
		"#define TIMEBIAS (((WarpTime*20.0)*M_PI*2.0)/128.0)\n"
		"	ntc.s += 0.125 + sin(tc_tex.t*M_PI + TIMEBIAS)*0.125;\n"
		"	ntc.t += 0.125 + sin(tc_tex.s*M_PI + TIMEBIAS)*0.125;\n"
		"	vec4 result = texture2D(Tex, ntc.st);\n"
		"	vec4 lighting = vec4(1.0);\n"
"#ifdef LIT\n"
		"	lighting = texture2D(LMTex, tc_lm.xy);\n"
		"	lighting.rgb *= LightScale;\n"
"#endif\n"

		SHADOW_SAMPLE_GLSL("v_Normal")

		"\n"

		DLIGHT_SAMPLE_WATER_GLSL

		"\n"

		"	lighting = clamp(lighting, 0.0, 1.0);\n"

		"	result.a *= Alpha;\n"
		"	result = clamp(result*lighting, 0.0, 1.0);\n"

		FOG_CALC_GLSL

		"	outColor = result;\n"
		"}\n";

	size_t i;
	char vtext[1024*8];
	char ftext[1024*8];
	gl_glsl_water_able = false;

	if (!gl_glsl_able)
		return;

	for (i = 0; i < countof(r_water); i++)
	{
		snprintf(vtext, sizeof(vtext), vertSource, modedefines[i]);
		snprintf(ftext, sizeof(ftext), fragSource, modedefines[i]);
		gl_shader_t sh = {0};
		qboolean compiled = GL_CreateShaderFromVF (&sh, vtext, ftext, sizeof(bindings)/sizeof(bindings[0]), bindings);
		if (compiled)
		{
			r_water[i].program = sh.program_id;

			// get uniform locations
			GLuint texLoc					  = GL_GetUniformLocation (&r_water[i].program, "Tex");
			GLuint LMTexLoc					  = (i?GL_GetUniformLocation (&r_water[i].program, "LMTex"):-1);
			r_water[i].light_scale			  = (i?GL_GetUniformLocation (&r_water[i].program, "LightScale"):-1);
			r_water[i].alpha_scale			  = GL_GetUniformLocation (&r_water[i].program, "Alpha");
			r_water[i].time					  = GL_GetUniformLocation (&r_water[i].program, "WarpTime");
			r_water[i].view_projection_matrix = GL_GetUniformLocation (&r_water[i].program, "ViewProjectionMatrix");

			for (int si = 0; si < MAX_FRAME_SHADOWS; si++) {
				static char uniform_name[] = "shadow_map_samplers[#]";
				static char cube_uniform_name[] = "shadow_map_cube_samplers[#]";
				uniform_name[strlen(uniform_name) - 2] = '0' + si;
				cube_uniform_name[strlen(cube_uniform_name) - 2] = '0' + si;
				r_water[i].shadow_map_samplers_loc[si] = GL_GetUniformLocation (&r_water[i].program, uniform_name);
				r_water[i].shadow_map_cube_samplers_loc[si] = GL_GetUniformLocation (&r_water[i].program, cube_uniform_name);
			}

			r_water[i].dlight_data_block_index = GL_GetUniformBlockIndexFunc (r_water[i].program, "dlight_data");
			GL_UniformBlockBindingFunc (r_water[i].program, r_water[i].dlight_data_block_index, DLIGHT_UBO_BINDING_POINT);

			r_water[i].fog_data_block_index = GL_GetUniformBlockIndexFunc (r_water[i].program, "fog_data");
			GL_UniformBlockBindingFunc (r_water[i].program, r_water[i].fog_data_block_index, FOG_UBO_BINDING_POINT);

			r_water[i].shadow_data_block_index = GL_GetUniformBlockIndexFunc (r_water[i].program, "shadow_data");
			GL_UniformBlockBindingFunc (r_water[i].program, r_water[i].shadow_data_block_index, SHADOW_UBO_BINDING_POINT);

			if (!r_water[i].program)
				return;

			//bake constants here.
			GL_UseProgramFunc (r_water[i].program);
			GL_Uniform1iFunc (texLoc, 0);
			if (LMTexLoc != -1)
				GL_Uniform1iFunc (LMTexLoc, 1);
			GL_UseProgramFunc (0);
		}
		else
			return;	//erk?
	}
	gl_glsl_water_able = true;
}

/*
================
R_DrawTextureChains_Water -- johnfitz
================
*/
void R_DrawTextureChains_Water (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;
	glpoly_t	*p;
	qboolean	bound;
	float entalpha;

	if (r_drawflat_cheatsafe || r_lightmap_cheatsafe) // ericw -- !r_drawworld_cheatsafe check moved to R_DrawWorld_Water ()
		return;

	if (gl_glsl_water_able)
	{
		extern GLuint gl_bmodel_vbo;
		int lastlightmap = -2;
		int mode = -1;
		for (i=0 ; i<model->numtextures ; i++)
		{
			t = model->textures[i];
			if (!t || !t->texturechains[chain] || !(t->texturechains[chain]->flags & SURF_DRAWTURB))
				continue;
			s = t->texturechains[chain];

			entalpha = GL_WaterAlphaForEntitySurface (ent, s);
			if (entalpha < 1.0f)
			{
				glDepthMask (GL_FALSE);
				glEnable (GL_BLEND);
			}

// Bind the buffers
			GL_BindBuffer (GL_ARRAY_BUFFER, gl_bmodel_vbo);
			GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0); // indices come from client memory!
			GL_VertexAttribPointerFunc (vertAttrIndex,      3, GL_FLOAT, GL_FALSE, VBO_VERTEXSIZE * sizeof(float), ((float *)0));
			GL_VertexAttribPointerFunc (texCoordsAttrIndex, 2, GL_FLOAT, GL_FALSE, VBO_VERTEXSIZE * sizeof(float), ((float *)0) + 3);
			GL_VertexAttribPointerFunc (LMCoordsAttrIndex,  2, GL_FLOAT, GL_FALSE, VBO_VERTEXSIZE * sizeof(float), ((float *)0) + 5);
			GL_VertexAttribPointerFunc (vertNormalIndex,    3, GL_FLOAT, GL_FALSE, VBO_VERTEXSIZE * sizeof(float), ((float *)0) + 7);

			//actually use the buffers...
			GL_EnableVertexAttribArrayFunc (vertAttrIndex);
			GL_EnableVertexAttribArrayFunc (texCoordsAttrIndex);
			GL_EnableVertexAttribArrayFunc (vertNormalIndex);

			GL_SelectTexture (GL_TEXTURE0);
			GL_Bind (t->gltexture);

			for (; s; s = s->texturechain)
			{
				if (s->lightmaptexturenum != lastlightmap)
				{
					R_FlushBatch ();

					mode = s->lightmaptexturenum>=0 && !r_fullbright_cheatsafe;
					if (mode)
					{
						GL_EnableVertexAttribArrayFunc (LMCoordsAttrIndex);
						GL_SelectTexture(GL_TEXTURE1);
						GL_Bind (lightmaps[s->lightmaptexturenum].texture);
					}
					else
						GL_DisableVertexAttribArrayFunc (LMCoordsAttrIndex);

					GL_UseProgramFunc (r_water[mode].program);
					GL_Uniform1fFunc (r_water[mode].time, cl.time);
					if (r_water[mode].light_scale != -1)
						GL_Uniform1fFunc (r_water[mode].light_scale, gl_overbright.value?2:1);
					GL_Uniform1fFunc (r_water[mode].alpha_scale, entalpha);
					GL_UniformMatrix4fvFunc (r_water[mode].view_projection_matrix,
						1, false, (const GLfloat*)r_projection_view_matrix);

					if (r_shadow_sun.value) {
						R_Shadow_BindTextures (r_water[mode].shadow_map_samplers_loc, r_water[mode].shadow_map_cube_samplers_loc);
					}

					lastlightmap = s->lightmaptexturenum;
				}
				R_BatchSurface (s);

				rs_brushpasses++;
			}

			R_FlushBatch ();
			GL_UseProgramFunc (0);
			GL_DisableVertexAttribArrayFunc (vertAttrIndex);
			GL_DisableVertexAttribArrayFunc (texCoordsAttrIndex);
			GL_DisableVertexAttribArrayFunc (LMCoordsAttrIndex);
			GL_SelectTexture (GL_TEXTURE0);
			lastlightmap = -2;

			if (entalpha < 1.0f)
			{
				glDepthMask (GL_TRUE);
				glDisable (GL_BLEND);
			}
		}
	}
	else
	{
		for (i=0 ; i<model->numtextures ; i++)
		{
			t = model->textures[i];
			if (!t || !t->texturechains[chain] || !(t->texturechains[chain]->flags & SURF_DRAWTURB))
				continue;
			bound = false;
			entalpha = 1.0f;
			for (s = t->texturechains[chain]; s; s = s->texturechain)
			{
				if (!bound) //only bind once we are sure we need this texture
				{
					entalpha = GL_WaterAlphaForEntitySurface (ent, s);
					R_BeginTransparentDrawing (entalpha);
					GL_Bind (t->gltexture);
					bound = true;
				}
				for (p = s->polys->next; p; p = p->next)
				{
					DrawWaterPoly (p);
					rs_brushpasses++;
				}
			}
			R_EndTransparentDrawing (entalpha);
		}
	}
}

/*
================
R_DrawTextureChains_White -- johnfitz -- draw sky and water as white polys when r_lightmap is 1
================
*/
void R_DrawTextureChains_White (qmodel_t *model, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;

	glDisable (GL_TEXTURE_2D);
	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];

		if (!t || !t->texturechains[chain] || !(t->texturechains[chain]->flags & SURF_DRAWTILED))
			continue;

		for (s = t->texturechains[chain]; s; s = s->texturechain)
		{
			DrawGLPoly (s->polys);
			rs_brushpasses++;
		}
	}
	glEnable (GL_TEXTURE_2D);
}

/*
================
R_DrawLightmapChains -- johnfitz -- R_BlendLightmaps stripped down to almost nothing
================
*/
void R_DrawLightmapChains (void)
{
	int			i, j;
	glpoly_t	*p;
	float		*v;

	for (i=0 ; i<lightmap_count ; i++)
	{
		if (!lightmaps[i].polys)
			continue;

		GL_Bind (lightmaps[i].texture);
		for (p = lightmaps[i].polys; p; p=p->chain)
		{
			glBegin (GL_POLYGON);
			v = p->verts[0];
			for (j=0 ; j<p->numverts ; j++, v+= VERTEXSIZE)
			{
				glTexCoord2f (v[5], v[6]);
				glVertex3fv (v);
			}
			glEnd ();
			rs_brushpasses++;
		}
	}
}

static GLuint r_world_program;

// uniforms used in vert shader

// uniforms used in frag shader
static GLuint texLoc;
static GLuint LMTexLoc;
static GLuint fullbrightTexLoc;
static GLuint useFullbrightTexLoc;
static GLuint useOverbrightLoc;
static GLuint useAlphaTestLoc;
static GLuint alphaLoc;
static GLuint modelMatrixLoc;
static GLuint viewProjectionMatrixLoc;

static GLuint dlight_data_block_index;
static GLuint fog_data_block_index;

static GLuint shadow_data_block_index;
static GLuint shadow_map_samplers_loc[MAX_FRAME_SHADOWS];
static GLuint shadow_map_cube_samplers_loc[MAX_FRAME_SHADOWS];

#define vertAttrIndex 0
#define texCoordsAttrIndex 1
#define LMCoordsAttrIndex 2

/*
=============
GLWorld_CreateShaders
=============
*/
void GLWorld_CreateShaders (void)
{
	const glsl_attrib_binding_t bindings[] = {
		{ "Vert", vertAttrIndex },
		{ "TexCoords", texCoordsAttrIndex },
		{ "LMCoords", LMCoordsAttrIndex },
		{ "Normal", vertNormalIndex },
	};

	// Driver bug workarounds:
	// - "Intel(R) UHD Graphics 600" version "4.6.0 - Build 26.20.100.7263"
	//    crashing on glUseProgram with `vec3 Vert` and
	//    `gl_ModelViewProjectionMatrix * vec4(Vert, 1.0);`. Work around with
	//    making Vert a vec4. (https://sourceforge.net/p/quakespasm/bugs/39/)
	const GLchar *vertSource = \
		"#version 150\n"
		"\n"
		"in vec4 Vert;\n"
		"in vec2 TexCoords;\n"
		"in vec2 LMCoords;\n"
		"in vec3 Normal;\n"
		"\n"

		SHADOW_VERT_UNIFORMS_GLSL

		"uniform mat4 ViewProjectionMatrix;\n"
		"uniform mat4 ModelMatrix;\n"

		"out float FogFragCoord;\n"
		"out vec2 tc_tex;\n"
		"out vec2 tc_lm;\n"
		"out vec3 v_Normal;\n"

		SHADOW_VERT_OUTPUT_GLSL

		"\n"
		"void main()\n"
		"{\n"
		"	tc_tex = TexCoords;\n"
		"	tc_lm = LMCoords;\n"
		"	gl_Position = ViewProjectionMatrix * ModelMatrix * Vert;\n"
		"	FogFragCoord = gl_Position.w;\n"
		"	v_Normal = Normal;\n"
		"   vec4 modelVert = ModelMatrix * Vert;\n"
		
		SHADOW_GET_COORD_GLSL("modelVert")

		"}\n";
	
	const GLchar *fragSource = \
		"#version 150\n"
		"\n"
		"uniform sampler2D Tex;\n"
		"uniform sampler2D LMTex;\n"
		"uniform sampler2D FullbrightTex;\n"
		"uniform bool UseFullbrightTex;\n"
		"uniform bool UseOverbright;\n"
		"uniform bool UseAlphaTest;\n"
		"uniform float Alpha;\n"

		SHADOW_FRAG_UNIFORMS_GLSL

		FOG_FRAG_UNIFORMS_GLSL

		DLIGHT_FRAG_UNIFORMS_GLSL

		"\n"
		"in float FogFragCoord;\n"
		"in vec2 tc_tex;\n"
		"in vec2 tc_lm;\n"
		"in vec3 v_Normal;\n"

		SHADOW_FRAG_INPUT_GLSL

		"out vec4 outColor;\n"

		"\n"
		"void main()\n"
		"{\n"
		"	vec4 result = texture2D(Tex, tc_tex.xy);\n"
		"	if (UseAlphaTest && (result.a < 0.666))\n"
		"		discard;\n"
		"	vec4 lightmap_color = texture2D(LMTex, tc_lm.xy);\n"
		"   vec4 lighting = lightmap_color;\n"
		"\n"

		SHADOW_SAMPLE_GLSL("v_Normal")

		"\n"

		DLIGHT_SAMPLE_GLSL("v_Normal")

		"\n"

		"	lighting = clamp(lighting, 0.0, 1.0);\n"
		"	result *= lighting;\n"

		"	if (UseOverbright)\n"
		"		result.rgb *= 2.0;\n"
		"	if (UseFullbrightTex)\n"
		"		result += texture2D(FullbrightTex, tc_tex.xy);\n"

		"	result = clamp(result, 0.0, 1.0);\n"

		FOG_CALC_GLSL
		
		"	result.a = Alpha;\n" // FIXME: This will make almost transparent things cut holes though heavy fog
		"\n"
		"	outColor = result;\n"
		"}\n";
	
	if (!gl_glsl_alias_able)
		return;

	gl_shader_t sh = { 0 };
	GL_CreateShaderFromVF (&sh, vertSource, fragSource, countof(bindings), bindings);
	r_world_program = sh.program_id;
	
	if (r_world_program != 0)
	{
		// get uniform locations
		texLoc = GL_GetUniformLocation (&r_world_program, "Tex");
		LMTexLoc = GL_GetUniformLocation (&r_world_program, "LMTex");
		fullbrightTexLoc = GL_GetUniformLocation (&r_world_program, "FullbrightTex");
		useFullbrightTexLoc = GL_GetUniformLocation (&r_world_program, "UseFullbrightTex");
		useOverbrightLoc = GL_GetUniformLocation (&r_world_program, "UseOverbright");
		useAlphaTestLoc = GL_GetUniformLocation (&r_world_program, "UseAlphaTest");
		alphaLoc = GL_GetUniformLocation (&r_world_program, "Alpha");
		modelMatrixLoc = GL_GetUniformLocation (&r_world_program, "ModelMatrix");
		viewProjectionMatrixLoc = GL_GetUniformLocation (&r_world_program, "ViewProjectionMatrix");

		for (int si = 0; si < MAX_FRAME_SHADOWS; si++) {
			static char uniform_name[] = "shadow_map_samplers[#]";
			static char cube_uniform_name[] = "shadow_map_cube_samplers[#]";
			uniform_name[strlen(uniform_name) - 2] = '0' + si;
			cube_uniform_name[strlen(cube_uniform_name) - 2] = '0' + si;
			shadow_map_samplers_loc[si] = GL_GetUniformLocation (&r_world_program, uniform_name);
			shadow_map_cube_samplers_loc[si] = GL_GetUniformLocation (&r_world_program, cube_uniform_name);
		}

		dlight_data_block_index = GL_GetUniformBlockIndexFunc (r_world_program, "dlight_data");
		GL_UniformBlockBindingFunc (r_world_program, dlight_data_block_index, DLIGHT_UBO_BINDING_POINT);

		fog_data_block_index = GL_GetUniformBlockIndexFunc (r_world_program, "fog_data");
		GL_UniformBlockBindingFunc (r_world_program, fog_data_block_index, FOG_UBO_BINDING_POINT);

		shadow_data_block_index = GL_GetUniformBlockIndexFunc (r_world_program, "shadow_data");
		GL_UniformBlockBindingFunc (r_world_program, shadow_data_block_index, SHADOW_UBO_BINDING_POINT);
	}

	GLWater_CreateShaders();
}

/*
================
R_DrawTextureChains_GLSL -- ericw

Draw lightmapped surfaces with fulbrights in one pass, using VBO.
Requires 3 TMUs, OpenGL 2.0
================
*/
void R_DrawTextureChains_GLSL (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;
	qboolean	bound;
	int		lastlightmap;
	gltexture_t	*fullbright = NULL;
	float		entalpha;
	
	entalpha = (ent != NULL) ? ENTALPHA_DECODE(ent->alpha) : 1.0f;

// enable blending / disable depth writes
	if (entalpha < 1)
	{
		glDepthMask (GL_FALSE);
		glEnable (GL_BLEND);
	}
	
	GL_UseProgramFunc (r_world_program);
	
// Bind the buffers
	GL_BindBuffer (GL_ARRAY_BUFFER, gl_bmodel_vbo);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0); // indices come from client memory!

	GL_EnableVertexAttribArrayFunc (vertAttrIndex);
	GL_EnableVertexAttribArrayFunc (texCoordsAttrIndex);
	GL_EnableVertexAttribArrayFunc (LMCoordsAttrIndex);
	GL_EnableVertexAttribArrayFunc (vertNormalIndex);
	
	GL_VertexAttribPointerFunc (vertAttrIndex,      3, GL_FLOAT, GL_FALSE, VBO_VERTEXSIZE * sizeof(float), ((float *)0));
	GL_VertexAttribPointerFunc (texCoordsAttrIndex, 2, GL_FLOAT, GL_FALSE, VBO_VERTEXSIZE * sizeof(float), ((float *)0) + 3);
	GL_VertexAttribPointerFunc (LMCoordsAttrIndex,  2, GL_FLOAT, GL_FALSE, VBO_VERTEXSIZE * sizeof(float), ((float *)0) + 5);
	GL_VertexAttribPointerFunc (vertNormalIndex,    3, GL_FLOAT, GL_FALSE, VBO_VERTEXSIZE * sizeof(float), ((float *)0) + 7);
	
// set uniforms
	GL_Uniform1iFunc (texLoc, 0);
	GL_Uniform1iFunc (LMTexLoc, 1);
	GL_Uniform1iFunc (fullbrightTexLoc, 2);
	GL_Uniform1iFunc (useFullbrightTexLoc, 0);
	GL_Uniform1iFunc (useOverbrightLoc, (int)gl_overbright.value);
	GL_Uniform1iFunc (useAlphaTestLoc, 0);
	GL_Uniform1fFunc (alphaLoc, entalpha);
	GL_UniformMatrix4fvFunc (viewProjectionMatrixLoc, 1, false, (const GLfloat*)r_projection_view_matrix);

	mat4_t model_matrix;
	if (ent) {
		Matrix4_InitTranslationAndRotation(ent->origin, ent->angles, model_matrix);
	}
	else {
		Matrix4_InitIdentity(model_matrix);
	}
	GL_UniformMatrix4fvFunc(modelMatrixLoc, 1, false, model_matrix);

// gnemeth - get the shadow data
	if (r_shadow_sun.value) {
		// GL_SelectTexture (RANDOM_TEXTURE_UNIT);
		// glBindTexture (GL_TEXTURE_2D, GL_GetRandomTexture());

		R_Shadow_BindTextures (shadow_map_samplers_loc, shadow_map_cube_samplers_loc);
	}
	
	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];

		if (!t || !t->texturechains[chain] || t->texturechains[chain]->flags & (SURF_DRAWTILED | SURF_NOTEXTURE | SURF_DRAWTURB))
			continue;

	// Enable/disable TMU 2 (fullbrights)
	// FIXME: Move below to where we bind GL_TEXTURE0
		if (gl_fullbrights.value && (fullbright = R_TextureAnimation(t, ent != NULL ? ent->frame : 0)->fullbright))
		{
			GL_SelectTexture (GL_TEXTURE2);
			GL_Bind (fullbright);
			GL_Uniform1iFunc (useFullbrightTexLoc, 1);
		}
		else
			GL_Uniform1iFunc (useFullbrightTexLoc, 0);

		R_ClearBatch ();

		bound = false;
		lastlightmap = 0; // avoid compiler warning
		for (s = t->texturechains[chain]; s; s = s->texturechain)
		{
			if (!bound) //only bind once we are sure we need this texture
			{
				GL_SelectTexture(GL_TEXTURE0);
				GL_Bind((R_TextureAnimation(t, ent != NULL ? ent->frame : 0))->gltexture);

				if (t->texturechains[chain]->flags & SURF_DRAWFENCE)
					GL_Uniform1iFunc(useAlphaTestLoc, 1); // Flip alpha test back on

				bound = true;
				lastlightmap = s->lightmaptexturenum;
			}

			if (s->lightmaptexturenum != lastlightmap)
				R_FlushBatch();

			GL_SelectTexture(GL_TEXTURE1);
			GL_Bind(lightmaps[s->lightmaptexturenum].texture);
			lastlightmap = s->lightmaptexturenum;
			R_BatchSurface(s);

			rs_brushpasses++;
		}

		R_FlushBatch ();

		if (bound && t->texturechains[chain]->flags & SURF_DRAWFENCE)
			GL_Uniform1iFunc (useAlphaTestLoc, 0); // Flip alpha test back off
	}
	
	// clean up
	GL_DisableVertexAttribArrayFunc (vertAttrIndex);
	GL_DisableVertexAttribArrayFunc (texCoordsAttrIndex);
	GL_DisableVertexAttribArrayFunc (LMCoordsAttrIndex);
	
	GL_UseProgramFunc (0);
	GL_SelectTexture (GL_TEXTURE0);
	
	if (entalpha < 1)
	{
		glDepthMask (GL_TRUE);
		glDisable (GL_BLEND);
	}
}

/*
=============
R_DrawWorld -- johnfitz -- rewritten
=============
*/
void R_DrawTextureChains (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	float entalpha;
	
	if (ent != NULL)
		entalpha = ENTALPHA_DECODE(ent->alpha);
	else
		entalpha = 1;

	R_UploadLightmaps ();

	if (r_drawflat_cheatsafe)
	{
		glDisable (GL_TEXTURE_2D);
		R_DrawTextureChains_Drawflat (model, chain);
		glEnable (GL_TEXTURE_2D);
		return;
	}

	if (r_fullbright_cheatsafe)
	{
		R_BeginTransparentDrawing (entalpha);
		R_DrawTextureChains_TextureOnly (model, ent, chain);
		R_EndTransparentDrawing (entalpha);
		goto fullbrights;
	}

	if (r_lightmap_cheatsafe)
	{
		if (!gl_overbright.value)
		{
			glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
			glColor3f(0.5, 0.5, 0.5);
		}
		R_DrawLightmapChains ();
		if (!gl_overbright.value)
		{
			glColor3f(1,1,1);
			glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		}
		R_DrawTextureChains_White (model, chain);
		return;
	}

	R_BeginTransparentDrawing (entalpha);

	R_DrawTextureChains_NoTexture (model, chain);

	// OpenGL 2 fast path
	if (r_world_program != 0)
	{
		R_EndTransparentDrawing (entalpha);
		
		R_DrawTextureChains_GLSL (model, ent, chain);
		return;
	}

	if (gl_overbright.value)
	{
		if (gl_texture_env_combine && gl_mtexable) //case 1: texture and lightmap in one pass, overbright using texture combiners
		{
			GL_EnableMultitexture ();
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
			glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_MODULATE);
			glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_PREVIOUS_EXT);
			glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_TEXTURE);
			glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, 2.0f);
			GL_DisableMultitexture ();
			R_DrawTextureChains_Multitexture (model, ent, chain);
			GL_EnableMultitexture ();
			glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, 1.0f);
			glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
			GL_DisableMultitexture ();
			glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		}
		else if (entalpha < 1) //case 2: can't do multipass if entity has alpha, so just draw the texture
		{
			R_DrawTextureChains_TextureOnly (model, ent, chain);
		}
		else //case 3: texture in one pass, lightmap in second pass using 2x modulation blend func, fog in third pass
		{
			//to make fog work with multipass lightmapping, need to do one pass
			//with no fog, one modulate pass with black fog, and one additive
			//pass with black geometry and normal fog
			Fog_DisableGFog ();
			R_DrawTextureChains_TextureOnly (model, ent, chain);
			Fog_EnableGFog ();
			glDepthMask (GL_FALSE);
			glEnable (GL_BLEND);
			glBlendFunc (GL_DST_COLOR, GL_SRC_COLOR); //2x modulate
			Fog_StartAdditive ();
			R_DrawLightmapChains ();
			Fog_StopAdditive ();
			if (Fog_GetDensity() > 0)
			{
				glBlendFunc(GL_ONE, GL_ONE); //add
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
				glColor3f(0,0,0);
				R_DrawTextureChains_TextureOnly (model, ent, chain);
				glColor3f(1,1,1);
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
			}
			glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDisable (GL_BLEND);
			glDepthMask (GL_TRUE);
		}
	}
	else
	{
		if (gl_mtexable) //case 4: texture and lightmap in one pass, regular modulation
		{
			GL_EnableMultitexture ();
			glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
			GL_DisableMultitexture ();
			R_DrawTextureChains_Multitexture (model, ent, chain);
			glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		}
		else if (entalpha < 1) //case 5: can't do multipass if entity has alpha, so just draw the texture
		{
			R_DrawTextureChains_TextureOnly (model, ent, chain);
		}
		else //case 6: texture in one pass, lightmap in a second pass, fog in third pass
		{
			//to make fog work with multipass lightmapping, need to do one pass
			//with no fog, one modulate pass with black fog, and one additive
			//pass with black geometry and normal fog
			Fog_DisableGFog ();
			R_DrawTextureChains_TextureOnly (model, ent, chain);
			Fog_EnableGFog ();
			glDepthMask (GL_FALSE);
			glEnable (GL_BLEND);
			glBlendFunc(GL_ZERO, GL_SRC_COLOR); //modulate
			Fog_StartAdditive ();
			R_DrawLightmapChains ();
			Fog_StopAdditive ();
			if (Fog_GetDensity() > 0)
			{
				glBlendFunc(GL_ONE, GL_ONE); //add
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
				glColor3f(0,0,0);
				R_DrawTextureChains_TextureOnly (model, ent, chain);
				glColor3f(1,1,1);
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
			}
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDisable (GL_BLEND);
			glDepthMask (GL_TRUE);
		}
	}

	R_EndTransparentDrawing (entalpha);

fullbrights:
	if (gl_fullbrights.value)
	{
		glDepthMask (GL_FALSE);
		glEnable (GL_BLEND);
		glBlendFunc (GL_ONE, GL_ONE);
		glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glColor3f (entalpha, entalpha, entalpha);
		Fog_StartAdditive ();
		R_DrawTextureChains_Glow (model, ent, chain);
		Fog_StopAdditive ();
		glColor3f (1, 1, 1);
		glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDisable (GL_BLEND);
		glDepthMask (GL_TRUE);
	}
}

/*
=============
R_DrawWorld -- ericw -- moved from R_DrawTextureChains, which is no longer specific to the world.
=============
*/
void R_DrawWorld (void)
{
	if (!r_drawworld_cheatsafe)
		return;

	R_DrawTextureChains (cl.worldmodel, NULL, chain_world);
}

/*
=============
R_DrawWorld_Water -- ericw -- moved from R_DrawTextureChains_Water, which is no longer specific to the world.
=============
*/
void R_DrawWorld_Water (void)
{
	if (!r_drawworld_cheatsafe)
		return;

	R_DrawTextureChains_Water (cl.worldmodel, NULL, chain_world);
}

/*
=============
R_DrawWorld_ShowTris -- ericw -- moved from R_DrawTextureChains_ShowTris, which is no longer specific to the world.
=============
*/
void R_DrawWorld_ShowTris (void)
{
	if (!r_drawworld_cheatsafe)
		return;

	R_DrawTextureChains_ShowTris (cl.worldmodel, chain_world);
}
