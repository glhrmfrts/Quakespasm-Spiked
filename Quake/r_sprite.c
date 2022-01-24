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
//r_sprite.c -- sprite model rendering

#include "quakedef.h"
#include "gl_fog.h"

enum { MAX_SPRITES = 1024*8 };

static GLuint spr_program;
static GLuint spr_vbo;
static GLuint u_view_projection_matrix;
static GLuint u_texture;
static GLuint fog_data_block_index;

typedef struct {
	vec4_t pos;
	vec4_t up;
	vec4_t right;
	vec4_t color;
	vec4_t frame_bounds;
	float smax;
	float tmax;
} r_spr_vertex_t;

typedef struct {
	gltexture_t* texture;
	size_t num_sprites;
	r_spr_vertex_t spr_verts[MAX_SPRITES];
} r_spr_batch_t;

static r_spr_batch_t batch;

static void R_InitSpritesShaders ()
{
	const char* vert_source = ""
	"#version 330 core\n"
	"\n"
	"layout (location = 0) in vec4 a_pos;\n"
	"layout (location = 1) in vec4 a_up;\n"
	"layout (location = 2) in vec4 a_right;\n"
	"layout (location = 3) in vec4 a_color;\n"
	"layout (location = 4) in vec4 a_frame_bounds;\n"
	"layout (location = 5) in vec2 a_frame_texmaxs;\n"
	"\n"
	
	"\n"
	"out VS_OUT { vec3 up; vec3 right; vec4 color; vec4 frame_bounds; vec2 frame_texmaxs; } vs_out;\n"
	"\n"

	"void main() {\n"
	"	gl_Position = a_pos;\n"
	"	vs_out.up = a_up.xyz;\n"
	"	vs_out.right = a_right.xyz;\n"
	"	vs_out.color = a_color;\n"
	"	vs_out.frame_bounds = a_frame_bounds;\n"
	"	vs_out.frame_texmaxs = a_frame_texmaxs;\n"
	"}\n";

	const char* geom_source = ""
	"#version 330 core\n"
	"\n"
	"layout (points) in;\n"
	"layout (triangle_strip, max_vertices=4) out;\n"
	"\n"
	"uniform mat4 u_view_projection_matrix;\n"
	"\n"
	"in VS_OUT { vec3 up; vec3 right; vec4 color; vec4 frame_bounds; vec2 frame_texmaxs; } gs_in[];\n"
	"\n"
	"out float FogFragCoord;\n"
	"out vec2 v_tex_coord;\n"
	"out vec4 v_color;\n"

	"\n"
	"void main() {\n"
	"	float fleft = gs_in[0].frame_bounds[0];\n"
	"	float fdown = gs_in[0].frame_bounds[1];\n"
	"	float fright = gs_in[0].frame_bounds[2];\n"
	"	float fup = gs_in[0].frame_bounds[3];\n"

	"	float scale = gl_in[0].gl_Position.w;\n"

	"	vec3 v0 = gl_in[0].gl_Position.xyz + (fdown*scale)*gs_in[0].up;\n"
	"	v0 += (fleft*scale)*gs_in[0].right;\n"
	"	vec2 t0 = vec2(0, gs_in[0].frame_texmaxs[1]);\n"

	"	vec3 v1 = gl_in[0].gl_Position.xyz + (fup*scale)*gs_in[0].up;\n"
	"	v1 += (fleft*scale)*gs_in[0].right;\n"
	"	vec2 t1 = vec2(0, 0);\n"

	"	vec3 v2 = gl_in[0].gl_Position.xyz + (fup*scale)*gs_in[0].up;\n"
	"	v2 += (fright*scale)*gs_in[0].right;\n"
	"	vec2 t2 = vec2(gs_in[0].frame_texmaxs[0], 0);\n"

	"	vec3 v3 = gl_in[0].gl_Position.xyz + (fdown*scale)*gs_in[0].up;\n"
	"	v3 += (fright*scale)*gs_in[0].right;\n"
	"	vec2 t3 = vec2(gs_in[0].frame_texmaxs[0], gs_in[0].frame_texmaxs[1]);\n"

	"	gl_Position = u_view_projection_matrix * vec4(v0, 1.0);\n"
	"	FogFragCoord = gl_Position.w;\n"
	"	v_tex_coord = t0;\n"
	"	v_color = gs_in[0].color;\n"
	"	EmitVertex();\n"

	"	gl_Position = u_view_projection_matrix * vec4(v1, 1.0);\n"
	"	FogFragCoord = gl_Position.w;\n"
	"	v_tex_coord = t1;\n"
	"	v_color = gs_in[0].color;\n"
	"	EmitVertex();\n"

	"	gl_Position = u_view_projection_matrix * vec4(v3, 1.0);\n"
	"	FogFragCoord = gl_Position.w;\n"
	"	v_tex_coord = t3;\n"
	"	v_color = gs_in[0].color;\n"
	"	EmitVertex();\n"

	"	gl_Position = u_view_projection_matrix * vec4(v2, 1.0);\n"
	"	FogFragCoord = gl_Position.w;\n"
	"	v_tex_coord = t2;\n"
	"	v_color = gs_in[0].color;\n"
	"	EmitVertex();\n"

	"	EndPrimitive();\n"
	"}\n"
	"";

	const char* frag_source = ""
	"#version 330 core\n"
	""
	"\n"
	"uniform sampler2D u_texture;\n"

	FOG_FRAG_UNIFORMS_GLSL

	"\n"
	"in float FogFragCoord;\n"
	"in vec2 v_tex_coord;\n"
	"in vec4 v_color;\n"
	"\n"
	"out vec4 out_color;\n"
	"\n"
	"void main() {\n"
	"	vec4 tex_color = texture(u_texture, v_tex_coord);\n"
	"	vec4 result = tex_color * v_color;\n"

		FOG_CALC_GLSL

	"	out_color = result;\n"
	"}\n";

	gl_shader_t sh = {0};
	GL_CreateShaderFromVGF (&sh, vert_source, geom_source, frag_source, 0, NULL);
	spr_program = sh.program_id;
	if (spr_program != 0) {
		u_view_projection_matrix = GL_GetUniformLocation (&spr_program, "u_view_projection_matrix");
		u_texture = GL_GetUniformLocation (&spr_program, "u_texture");

		fog_data_block_index = GL_GetUniformBlockIndexFunc (spr_program, "fog_data");
		GL_UniformBlockBindingFunc (spr_program, fog_data_block_index, FOG_UBO_BINDING_POINT);
	}
}

static void R_InitSpritesVBO ()
{
	GL_GenBuffersFunc (1, &spr_vbo);
	GL_BindBufferFunc (GL_ARRAY_BUFFER, spr_vbo);
	GL_BufferDataFunc (GL_ARRAY_BUFFER, sizeof(batch.spr_verts), batch.spr_verts, GL_DYNAMIC_DRAW);
	GL_BindBufferFunc (GL_ARRAY_BUFFER, 0);
}

void R_InitSprites ()
{
	R_InitSpritesShaders ();
	R_InitSpritesVBO ();

	GLint value;
	glGetIntegerv (GL_MAX_VERTEX_ATTRIBS, &value);
	Con_Printf("GL_MAX_VERTEX_ATTRIBS: %d\n", value);
}

/*
================
R_GetSpriteFrame
================
*/
mspriteframe_t *R_GetSpriteFrame (entity_t *currentent)
{
	msprite_t		*psprite;
	mspritegroup_t	*pspritegroup;
	mspriteframe_t	*pspriteframe;
	int				i, numframes, frame;
	float			*pintervals, fullinterval, targettime, time;

	psprite = (msprite_t *) currentent->model->cache.data;
	frame = currentent->frame;

	if ((frame >= psprite->numframes) || (frame < 0))
	{
		Con_DPrintf ("R_DrawSprite: no such frame %d for '%s'\n", frame, currentent->model->name);
		frame = 0;
	}

	if (psprite->frames[frame].type == SPR_SINGLE)
	{
		pspriteframe = psprite->frames[frame].frameptr;
	}
	else
	{
		pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		pintervals = pspritegroup->intervals;
		numframes = pspritegroup->numframes;
		fullinterval = pintervals[numframes-1];

		time = cl.time + currentent->syncbase;

	// when loading in Mod_LoadSpriteGroup, we guaranteed all interval values
	// are positive, so we don't have to worry about division by 0
		targettime = time - ((int)(time / fullinterval)) * fullinterval;

		for (i=0 ; i<(numframes-1) ; i++)
		{
			if (pintervals[i] > targettime)
				break;
		}

		pspriteframe = pspritegroup->frames[i];
	}

	return pspriteframe;
}

static size_t frame_batches;
static size_t frame_sprites;

void R_FlushSprites ()
{
	extern mat4_t r_projection_view_matrix;

	if (batch.num_sprites == 0 || !batch.texture) {
		batch.num_sprites = 0;
		return;
	}

	glEnable (GL_BLEND);
	glDepthMask (GL_FALSE);

	GL_UseProgramFunc (spr_program);
	GL_Uniform1iFunc (u_texture, 0);
	GL_UniformMatrix4fvFunc (u_view_projection_matrix, 1, false, (const GLfloat*)r_projection_view_matrix);

	GL_SelectTexture (GL_TEXTURE0);
	GL_Bind (batch.texture);

	GL_BindBufferFunc (GL_ARRAY_BUFFER, spr_vbo);
	GL_BufferSubDataFunc (GL_ARRAY_BUFFER, 0, batch.num_sprites * sizeof(r_spr_vertex_t), batch.spr_verts);

	GL_EnableVertexAttribArrayFunc (0);
	GL_EnableVertexAttribArrayFunc (1);
	GL_EnableVertexAttribArrayFunc (2);
	GL_EnableVertexAttribArrayFunc (3);
	GL_EnableVertexAttribArrayFunc (4);
	GL_EnableVertexAttribArrayFunc (5);

	GL_VertexAttribPointerFunc (0, 4, GL_FLOAT, false, sizeof(r_spr_vertex_t), NULL);
	GL_VertexAttribPointerFunc (1, 4, GL_FLOAT, false, sizeof(r_spr_vertex_t), ((float*)NULL) + 4);
	GL_VertexAttribPointerFunc (2, 4, GL_FLOAT, false, sizeof(r_spr_vertex_t), ((float*)NULL) + 8);
	GL_VertexAttribPointerFunc (3, 4, GL_FLOAT, false, sizeof(r_spr_vertex_t), ((float*)NULL) + 12);
	GL_VertexAttribPointerFunc (4, 4, GL_FLOAT, false, sizeof(r_spr_vertex_t), ((float*)NULL) + 16);
	GL_VertexAttribPointerFunc (5, 2, GL_FLOAT, false, sizeof(r_spr_vertex_t), ((float*)NULL) + 20);

	glDrawArrays (GL_POINTS, 0, batch.num_sprites);

	GL_DisableVertexAttribArrayFunc (0);
	GL_DisableVertexAttribArrayFunc (1);
	GL_DisableVertexAttribArrayFunc (2);
	GL_DisableVertexAttribArrayFunc (3);
	GL_DisableVertexAttribArrayFunc (4);
	GL_DisableVertexAttribArrayFunc (5);

	GL_BindBufferFunc (GL_ARRAY_BUFFER, 0);
	
	GL_UseProgramFunc (0);

	glDepthMask (GL_TRUE);
	glDisable (GL_BLEND);

	frame_batches++;
	frame_sprites += batch.num_sprites;

	batch.num_sprites = 0;
}

static void R_BatchSprite (const vec3_t origin, float scale, const vec3_t s_up, const vec3_t s_right,
	const mspriteframe_t* frame, const vec3_t color)
{
	if (frame->gltexture != batch.texture || batch.num_sprites >= MAX_SPRITES) {
		R_FlushSprites ();
		batch.texture = frame->gltexture;
	}

	r_spr_vertex_t* spr = &batch.spr_verts[batch.num_sprites++];
	VectorCopy (origin, spr->pos);
	spr->pos[3] = scale;

	VectorCopy (s_up, spr->up);
	VectorCopy (s_right, spr->right);
	VectorCopy (color, spr->color);
	spr->color[3] = 1.0f;
	spr->frame_bounds[0] = frame->left;
	spr->frame_bounds[1] = frame->down;
	spr->frame_bounds[2] = frame->right;
	spr->frame_bounds[3] = frame->up;
	spr->smax = frame->smax;
	spr->tmax = frame->tmax;
}

/*
=================
R_DrawSpriteModel -- johnfitz -- rewritten: now supports all orientations
=================
*/
void R_DrawSpriteModel (entity_t *e)
{
	vec3_t			point, v_forward, v_right, v_up;
	msprite_t		*psprite;
	mspriteframe_t	*frame;
	float			*s_up, *s_right;
	float			angle, sr, cr;
	float			scale;

	//TODO: frustum cull it?

	frame = R_GetSpriteFrame (e);
	psprite = (msprite_t *) currententity->model->cache.data;

	switch(psprite->type)
	{
	case SPR_VP_PARALLEL_UPRIGHT: //faces view plane, up is towards the heavens
		v_up[0] = 0;
		v_up[1] = 0;
		v_up[2] = 1;
		s_up = v_up;
		s_right = vright;
		break;
	case SPR_FACING_UPRIGHT: //faces camera origin, up is towards the heavens
		VectorSubtract(currententity->origin, r_origin, v_forward);
		v_forward[2] = 0;
		VectorNormalizeFast(v_forward);
		v_right[0] = v_forward[1];
		v_right[1] = -v_forward[0];
		v_right[2] = 0;
		v_up[0] = 0;
		v_up[1] = 0;
		v_up[2] = 1;
		s_up = v_up;
		s_right = v_right;
		break;
	case SPR_VP_PARALLEL: //faces view plane, up is towards the top of the screen
		s_up = vup;
		s_right = vright;
		break;
	case SPR_ORIENTED: //pitch yaw roll are independent of camera
		AngleVectors (currententity->angles, v_forward, v_right, v_up);
		s_up = v_up;
		s_right = v_right;
		break;
	case SPR_VP_PARALLEL_ORIENTED: //faces view plane, but obeys roll value
		angle = currententity->angles[ROLL] * M_PI_DIV_180;
		sr = sin(angle);
		cr = cos(angle);
		v_right[0] = vright[0] * cr + vup[0] * sr;
		v_right[1] = vright[1] * cr + vup[1] * sr;
		v_right[2] = vright[2] * cr + vup[2] * sr;
		v_up[0] = vright[0] * -sr + vup[0] * cr;
		v_up[1] = vright[1] * -sr + vup[1] * cr;
		v_up[2] = vright[2] * -sr + vup[2] * cr;
		s_up = v_up;
		s_right = v_right;
		break;
	default:
		return;
	}

	//johnfitz: offset decals
	if (psprite->type == SPR_ORIENTED)
		GL_PolygonOffset (OFFSET_DECAL);

	if (e->netstate.scale != 16)
		scale = e->netstate.scale/16.0;
	else
		scale = 1;

	SDL_assert (spr_program != 0);

	vec3_t color = { e->netstate.colormod[0]/32.0,e->netstate.colormod[1]/32.0,e->netstate.colormod[2]/32.0 };
	R_BatchSprite (e->origin, scale, s_up, s_right, frame, color);
}

void R_PrintSpriteInfo ()
{
	//Con_Printf ("batches: %d, sprites: %d\n", frame_batches, frame_sprites);
	frame_batches = 0;
	frame_sprites = 0;
}