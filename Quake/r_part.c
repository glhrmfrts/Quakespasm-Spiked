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

#include "quakedef.h"
#include "gl_fog.h"

#define MAX_PARTICLES			(100*1024)	// default max # of particles at one
											//  time
#define ABSOLUTE_MIN_PARTICLES	512			// no fewer than this no matter what's
											//  on the command line

int		ramp1[8] = {0x6f, 0x6d, 0x6b, 0x69, 0x67, 0x65, 0x63, 0x61};
int		ramp2[8] = {0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x68, 0x66};
int		ramp3[8] = {0x6d, 0x6b, 6, 5, 4, 3};

particle_t	*active_particles, *free_particles, *particles;

vec3_t			r_pright, r_pup, r_ppn;

int			r_numparticles;

gltexture_t *particletexture, *particletexture1, *particletexture2, *particletexture3, *particletexture4; //johnfitz
float texturescalefactor; //johnfitz -- compensate for apparent size of different particle textures

cvar_t	r_particles = {"r_particles","1", CVAR_ARCHIVE}; //johnfitz
cvar_t	r_quadparticles = {"r_quadparticles","1", CVAR_ARCHIVE}; //johnfitz

/*
===============
R_ParticleTextureLookup -- johnfitz -- generate nice antialiased 32x32 circle for particles
===============
*/
int R_ParticleTextureLookup (int x, int y, int sharpness)
{
	int r; //distance from point x,y to circle origin, squared
	int a; //alpha value to return

	x -= 16;
	y -= 16;
	r = x * x + y * y;
	r = r > 255 ? 255 : r;
	a = sharpness * (255 - r);
	a = q_min(a,255);
	return a;
}

/*
===============
R_InitParticleTextures -- johnfitz -- rewritten
===============
*/
void R_InitParticleTextures (void)
{
	int			x,y;
	static byte	particle1_data[64*64*4];
	static byte	particle2_data[2*2*4];
	static byte	particle3_data[64*64*4];
	byte		*dst;

	// particle texture 1 -- circle
	dst = particle1_data;
	for (x=0 ; x<64 ; x++)
		for (y=0 ; y<64 ; y++)
		{
			*dst++ = 255;
			*dst++ = 255;
			*dst++ = 255;
			*dst++ = R_ParticleTextureLookup(x, y, 8);
		}
	particletexture1 = TexMgr_LoadImage (NULL, "particle1", 64, 64, SRC_RGBA, particle1_data, "", (src_offset_t)particle1_data, TEXPREF_PERSIST | TEXPREF_ALPHA | TEXPREF_LINEAR);

	// particle texture 2 -- square
	dst = particle2_data;
	for (x=0 ; x<2 ; x++)
		for (y=0 ; y<2 ; y++)
		{
			*dst++ = 255;
			*dst++ = 255;
			*dst++ = 255;
			*dst++ = x || y ? 0 : 255;
		}
	particletexture2 = TexMgr_LoadImage (NULL, "particle2", 2, 2, SRC_RGBA, particle2_data, "", (src_offset_t)particle2_data, TEXPREF_PERSIST | TEXPREF_ALPHA | TEXPREF_NEAREST);

	// particle texture 3 -- blob
	dst = particle3_data;
	for (x=0 ; x<64 ; x++)
		for (y=0 ; y<64 ; y++)
		{
			*dst++ = 255;
			*dst++ = 255;
			*dst++ = 255;
			*dst++ = R_ParticleTextureLookup(x, y, 2);
		}
	particletexture3 = TexMgr_LoadImage (NULL, "particle3", 64, 64, SRC_RGBA, particle3_data, "", (src_offset_t)particle3_data, TEXPREF_PERSIST | TEXPREF_ALPHA | TEXPREF_LINEAR);

	//set default
	particletexture = particletexture1;
	texturescalefactor = 1.27;
}

typedef struct {
	vec3_t pos;
	float scale;
	vec4_t color;
} r_part_vertex_t;

static GLuint part_program;
static GLuint part_vbo;
static GLuint u_up;
static GLuint u_right;
static GLuint u_view_projection_matrix;
static GLuint u_particle_texture;
static GLuint fog_data_block_index;

static r_part_vertex_t part_verts[MAX_PARTICLES];

static void R_InitParticleShaders ()
{
	const char* vert_source = ""
	"#version 330 core\n"
	"\n"
	"layout (location = 0) in vec4 a_pos;\n"
	"layout (location = 1) in vec4 a_color;\n"
	"\n"
	
	"\n"
	"out VS_OUT { vec4 color; } vs_out;\n"
	"\n"
	"void main() {\n"
	"	gl_Position = a_pos;\n"
	"	vs_out.color = a_color;\n"
	"}\n";

	const char* geom_source = ""
	"#version 330 core\n"
	"\n"
	"layout (points) in;\n"
	"layout (triangle_strip, max_vertices=4) out;\n"
	"\n"
	"uniform vec3 u_up;\n"
	"uniform vec3 u_right;\n"
	"uniform mat4 u_view_projection_matrix;\n"
	"\n"
	"in VS_OUT { vec4 color; } gs_in[];\n"
	"\n"
	"out float FogFragCoord;\n"
	"out vec2 v_tex_coord;\n"
	"out vec4 v_color;\n"

	"\n"
	"void main() {\n"
	"	vec3 down = -u_up;\n"
	"	vec3 left = -u_right;\n"
	"	vec3 v0 = left+down;\n"
	"	vec3 v1 = left+u_up;\n"
	"	vec3 v2 = u_right+u_up;\n"
	"	vec3 v3 = u_right+down;\n"

	"	float scale = gl_in[0].gl_Position.w;\n"

	"	gl_Position = u_view_projection_matrix * vec4(gl_in[0].gl_Position.xyz + scale*v0, 1.0);\n"
	"	FogFragCoord = gl_Position.w;\n"
	"	v_tex_coord = vec2(0,0);\n"
	"	v_color = gs_in[0].color;\n"
	"	EmitVertex();\n"

	"	gl_Position = u_view_projection_matrix * vec4(gl_in[0].gl_Position.xyz + scale*v1, 1.0);\n"
	"	FogFragCoord = gl_Position.w;\n"
	"	v_tex_coord = vec2(0,1);\n"
	"	v_color = gs_in[0].color;\n"
	"	EmitVertex();\n"

	"	gl_Position = u_view_projection_matrix * vec4(gl_in[0].gl_Position.xyz + scale*v3, 1.0);\n"
	"	FogFragCoord = gl_Position.w;\n"
	"	v_tex_coord = vec2(1,0);\n"
	"	v_color = gs_in[0].color;\n"
	"	EmitVertex();\n"

	"	gl_Position = u_view_projection_matrix * vec4(gl_in[0].gl_Position.xyz + scale*v2, 1.0);\n"
	"	FogFragCoord = gl_Position.w;\n"
	"	v_tex_coord = vec2(1,1);\n"
	"	v_color = gs_in[0].color;\n"
	"	EmitVertex();\n"

	"	EndPrimitive();\n"
	"}\n"
	"";

	const char* frag_source = ""
	"#version 330 core\n"
	""
	"\n"
	"uniform sampler2D u_particle_texture;\n"

	FOG_FRAG_UNIFORMS_GLSL

	"\n"
	"in float FogFragCoord;\n"
	"in vec2 v_tex_coord;\n"
	"in vec4 v_color;\n"
	"\n"
	"out vec4 out_color;\n"
	"\n"
	"void main() {\n"
	"	vec4 tex_color = texture(u_particle_texture, v_tex_coord);\n"
	"	vec4 result = tex_color * v_color;\n"

		FOG_CALC_GLSL

	"	out_color = result;\n"
	"}\n";

	gl_shader_t sh = {0};
	GL_CreateShaderFromVGF (&sh, vert_source, geom_source, frag_source, 0, NULL);
	part_program = sh.program_id;
	if (part_program != 0) {
		u_view_projection_matrix = GL_GetUniformLocation (&part_program, "u_view_projection_matrix");
		u_particle_texture = GL_GetUniformLocation (&part_program, "u_particle_texture");
		u_up = GL_GetUniformLocation (&part_program, "u_up");
		u_right = GL_GetUniformLocation (&part_program, "u_right");

		fog_data_block_index = GL_GetUniformBlockIndexFunc (part_program, "fog_data");
		GL_UniformBlockBindingFunc (part_program, fog_data_block_index, FOG_UBO_BINDING_POINT);
	}
}

static void R_InitParticleVBO ()
{
	GL_GenBuffersFunc (1, &part_vbo);
	GL_BindBufferFunc (GL_ARRAY_BUFFER, part_vbo);
	GL_BufferDataFunc (GL_ARRAY_BUFFER, sizeof(part_verts), part_verts, GL_DYNAMIC_DRAW);
	GL_BindBufferFunc (GL_ARRAY_BUFFER, 0);
}

/*
===============
R_SetParticleTexture_f -- johnfitz
===============
*/
static void R_SetParticleTexture_f (cvar_t *var)
{
	switch ((int)(r_particles.value))
	{
	case 1:
		particletexture = particletexture1;
		texturescalefactor = 1.27;
		break;
	case 2:
		particletexture = particletexture2;
		texturescalefactor = 1.0;
		break;
//	case 3:
//		particletexture = particletexture3;
//		texturescalefactor = 1.5;
//		break;
	}
}

/*
===============
R_InitParticles
===============
*/
void R_InitParticles (void)
{
	int		i;

	i = COM_CheckParm ("-particles");

	if (i)
	{
		r_numparticles = (int)(Q_atoi(com_argv[i+1]));
		if (r_numparticles < ABSOLUTE_MIN_PARTICLES)
			r_numparticles = ABSOLUTE_MIN_PARTICLES;
	}
	else
	{
		r_numparticles = MAX_PARTICLES;
	}

	particles = (particle_t *)
			Hunk_AllocName (r_numparticles * sizeof(particle_t), "particles");

	Cvar_RegisterVariable (&r_particles); //johnfitz
	Cvar_SetCallback (&r_particles, R_SetParticleTexture_f);
	Cvar_RegisterVariable (&r_quadparticles); //johnfitz

	R_InitParticleTextures (); //johnfitz

	R_InitParticleShaders ();

	R_InitParticleVBO ();
}

/*
===============
R_EntityParticles
===============
*/
#define NUMVERTEXNORMALS	162
extern	float	r_avertexnormals[NUMVERTEXNORMALS][3];
vec3_t	avelocities[NUMVERTEXNORMALS];
float	beamlength = 16;
vec3_t	avelocity = {23, 7, 3};
float	partstep = 0.01;
float	timescale = 0.01;

void R_EntityParticles (entity_t *ent)
{
	int		i;
	particle_t	*p;
	float		angle;
	float		sp, sy, cp, cy;
//	float		sr, cr;
//	int		count;
	vec3_t		forward;
	float		dist;

	dist = 64;
//	count = 50;

	if (!avelocities[0][0])
	{
		for (i = 0; i < NUMVERTEXNORMALS; i++)
		{
			avelocities[i][0] = (rand() & 255) * 0.01;
			avelocities[i][1] = (rand() & 255) * 0.01;
			avelocities[i][2] = (rand() & 255) * 0.01;
		}
	}

	for (i = 0; i < NUMVERTEXNORMALS; i++)
	{
		angle = cl.time * avelocities[i][0];
		sy = sin(angle);
		cy = cos(angle);
		angle = cl.time * avelocities[i][1];
		sp = sin(angle);
		cp = cos(angle);
		angle = cl.time * avelocities[i][2];
	//	sr = sin(angle);
	//	cr = cos(angle);

		forward[0] = cp*cy;
		forward[1] = cp*sy;
		forward[2] = -sp;

		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;

		p->die = cl.time + 0.01;
		p->color = 0x6f;
		p->type = pt_explode;

		p->org[0] = ent->origin[0] + r_avertexnormals[i][0]*dist + forward[0]*beamlength;
		p->org[1] = ent->origin[1] + r_avertexnormals[i][1]*dist + forward[1]*beamlength;
		p->org[2] = ent->origin[2] + r_avertexnormals[i][2]*dist + forward[2]*beamlength;
	}
}

/*
===============
R_ClearParticles
===============
*/
void R_ClearParticles (void)
{
	int		i;

	free_particles = &particles[0];
	active_particles = NULL;

	for (i=0 ;i<r_numparticles ; i++)
		particles[i].next = &particles[i+1];
	particles[r_numparticles-1].next = NULL;
}

/*
===============
R_ReadPointFile_f
===============
*/
void R_ReadPointFile_f (void)
{
	FILE	*f;
	vec3_t	org;
	int		r;
	int		c;
	particle_t	*p;
	char	name[MAX_QPATH];

	if (cls.state != ca_connected)
		return;			// need an active map.

	q_snprintf (name, sizeof(name), "maps/%s.pts", cl.mapname);

	COM_FOpenFile (name, &f, NULL);
	if (!f)
	{
		Con_Printf ("couldn't open %s\n", name);
		return;
	}

	Con_Printf ("Reading %s...\n", name);
	c = 0;
	org[0] = org[1] = org[2] = 0; // silence pesky compiler warnings
	for ( ;; )
	{
		r = fscanf (f,"%f %f %f\n", &org[0], &org[1], &org[2]);
		if (r != 3)
			break;
		c++;

		if (!free_particles)
		{
			Con_Printf ("Not enough free particles\n");
			break;
		}
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;

		p->die = 99999;
		p->color = (-c)&15;
		p->type = pt_static;
		VectorCopy (vec3_origin, p->vel);
		VectorCopy (org, p->org);
	}

	fclose (f);
	Con_Printf ("%i points read\n", c);
}

/*
===============
R_ParseParticleEffect

Parse an effect out of the server message
===============
*/
void R_ParseParticleEffect (void)
{
	vec3_t		org, dir;
	int			i, count, msgcount, color;

	for (i=0 ; i<3 ; i++)
		org[i] = MSG_ReadCoord (cl.protocolflags);
	for (i=0 ; i<3 ; i++)
		dir[i] = MSG_ReadChar () * (1.0/16);
	msgcount = MSG_ReadByte ();
	color = MSG_ReadByte ();

	if (msgcount == 255)
	{
		if (!PScript_RunParticleEffectTypeString(org, dir, 1, "te_explosion"))
			count = 0;
		else
			count = 1024;
	}
	else
	{
		if (!PScript_RunParticleEffect(org, dir, color, msgcount))
			count = 0;
		else
			count = msgcount;
	}

	R_RunParticleEffect (org, dir, color, count);
}

/*
===============
R_ParticleExplosion
===============
*/
void R_ParticleExplosion (vec3_t org)
{
	int			i, j;
	particle_t	*p;

	for (i=0 ; i<1024 ; i++)
	{
		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;

		p->die = cl.time + 5;
		p->color = ramp1[0];
		p->ramp = rand()&3;
		if (i & 1)
		{
			p->type = pt_explode;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%1024)-512;
			}
		}
		else
		{
			p->type = pt_explode2;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%1024)-512;
			}
		}
	}
}

/*
===============
R_ParticleExplosion2
===============
*/
void R_ParticleExplosion2 (vec3_t org, int colorStart, int colorLength)
{
	int			i, j;
	particle_t	*p;
	int			colorMod = 0;

	for (i=0; i<512; i++)
	{
		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;

		p->die = cl.time + 0.3;
		p->color = colorStart + (colorMod % colorLength);
		colorMod++;

		p->type = pt_blob;
		for (j=0 ; j<3 ; j++)
		{
			p->org[j] = org[j] + ((rand()%32)-16);
			p->vel[j] = (rand()%512)-256;
		}
	}
}

/*
===============
R_BlobExplosion
===============
*/
void R_BlobExplosion (vec3_t org)
{
	int			i, j;
	particle_t	*p;

	for (i=0 ; i<1024 ; i++)
	{
		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;

		p->die = cl.time + 1 + (rand()&8)*0.05;

		if (i & 1)
		{
			p->type = pt_blob;
			p->color = 66 + rand()%6;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%512)-256;
			}
		}
		else
		{
			p->type = pt_blob2;
			p->color = 150 + rand()%6;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%512)-256;
			}
		}
	}
}

/*
===============
R_RunParticleEffect
===============
*/
void R_RunParticleEffect (vec3_t org, vec3_t dir, int color, int count)
{
	int			i, j;
	particle_t	*p;

	for (i=0 ; i<count ; i++)
	{
		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;

		if (count == 1024)
		{	// rocket explosion
			p->die = cl.time + 5;
			p->color = ramp1[0];
			p->ramp = rand()&3;
			if (i & 1)
			{
				p->type = pt_explode;
				for (j=0 ; j<3 ; j++)
				{
					p->org[j] = org[j] + ((rand()%32)-16);
					p->vel[j] = (rand()%512)-256;
				}
			}
			else
			{
				p->type = pt_explode2;
				for (j=0 ; j<3 ; j++)
				{
					p->org[j] = org[j] + ((rand()%32)-16);
					p->vel[j] = (rand()%512)-256;
				}
			}
		}
		else
		{
			p->die = cl.time + 0.1*(rand()%5);
			p->color = (color&~7) + (rand()&7);
			p->type = pt_slowgrav;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()&15)-8);
				p->vel[j] = dir[j]*15;// + (rand()%300)-150;
			}
		}
	}
}

/*
===============
R_LavaSplash
===============
*/
void R_LavaSplash (vec3_t org)
{
	int			i, j, k;
	particle_t	*p;
	float		vel;
	vec3_t		dir;

	for (i=-16 ; i<16 ; i++)
		for (j=-16 ; j<16 ; j++)
			for (k=0 ; k<1 ; k++)
			{
				if (!free_particles)
					return;
				p = free_particles;
				free_particles = p->next;
				p->next = active_particles;
				active_particles = p;

				p->die = cl.time + 2 + (rand()&31) * 0.02;
				p->color = 224 + (rand()&7);
				p->type = pt_slowgrav;

				dir[0] = j*8 + (rand()&7);
				dir[1] = i*8 + (rand()&7);
				dir[2] = 256;

				p->org[0] = org[0] + dir[0];
				p->org[1] = org[1] + dir[1];
				p->org[2] = org[2] + (rand()&63);

				VectorNormalize (dir);
				vel = 50 + (rand()&63);
				VectorScale (dir, vel, p->vel);
			}
}

/*
===============
R_TeleportSplash
===============
*/
void R_TeleportSplash (vec3_t org)
{
	int			i, j, k;
	particle_t	*p;
	float		vel;
	vec3_t		dir;

	for (i=-16 ; i<16 ; i+=4)
		for (j=-16 ; j<16 ; j+=4)
			for (k=-24 ; k<32 ; k+=4)
			{
				if (!free_particles)
					return;
				p = free_particles;
				free_particles = p->next;
				p->next = active_particles;
				active_particles = p;

				p->die = cl.time + 0.2 + (rand()&7) * 0.02;
				p->color = 7 + (rand()&7);
				p->type = pt_slowgrav;

				dir[0] = j*8;
				dir[1] = i*8;
				dir[2] = k*8;

				p->org[0] = org[0] + i + (rand()&3);
				p->org[1] = org[1] + j + (rand()&3);
				p->org[2] = org[2] + k + (rand()&3);

				VectorNormalize (dir);
				vel = 50 + (rand()&63);
				VectorScale (dir, vel, p->vel);
			}
}

/*
===============
R_RocketTrail

FIXME -- rename function and use #defined types instead of numbers
===============
*/
void R_RocketTrail (vec3_t start, vec3_t end, int type)
{
	vec3_t		vec;
	float		len;
	int			j;
	particle_t	*p;
	int			dec;
	static int	tracercount;

	VectorSubtract (end, start, vec);
	len = VectorNormalize (vec);
	if (type < 128)
		dec = 3;
	else
	{
		dec = 1;
		type -= 128;
	}

	while (len > 0)
	{
		len -= dec;

		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;

		VectorCopy (vec3_origin, p->vel);
		p->die = cl.time + 2;

		switch (type)
		{
			case 0:	// rocket trail
				p->ramp = (rand()&3);
				p->color = ramp3[(int)p->ramp];
				p->type = pt_fire;
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()%6)-3);
				break;

			case 1:	// smoke smoke
				p->ramp = (rand()&3) + 2;
				p->color = ramp3[(int)p->ramp];
				p->type = pt_fire;
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()%6)-3);
				break;

			case 2:	// blood
				p->type = pt_grav;
				p->color = 67 + (rand()&3);
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()%6)-3);
				break;

			case 3:
			case 5:	// tracer
				p->die = cl.time + 0.5;
				p->type = pt_static;
				if (type == 3)
					p->color = 52 + ((tracercount&4)<<1);
				else
					p->color = 230 + ((tracercount&4)<<1);

				tracercount++;

				VectorCopy (start, p->org);
				if (tracercount & 1)
				{
					p->vel[0] = 30*vec[1];
					p->vel[1] = 30*-vec[0];
				}
				else
				{
					p->vel[0] = 30*-vec[1];
					p->vel[1] = 30*vec[0];
				}
				break;

			case 4:	// slight blood
				p->type = pt_grav;
				p->color = 67 + (rand()&3);
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()%6)-3);
				len -= 3;
				break;

			case 6:	// voor trail
				p->color = 9*16 + 8 + (rand()&3);
				p->type = pt_static;
				p->die = cl.time + 0.3;
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()&15)-8);
				break;
		}

		VectorAdd (start, vec, start);
	}
}

static size_t frame_particles;

/*
===============
CL_RunParticles -- johnfitz -- all the particle behavior, separated from R_DrawParticles
===============
*/
void CL_RunParticles (void)
{
	particle_t		*p, *kill;
	int				i;
	float			time1, time2, time3, dvel, frametime, grav;
	extern	cvar_t	sv_gravity;

	frametime = cl.time - cl.oldtime;
	time3 = frametime * 15;
	time2 = frametime * 10;
	time1 = frametime * 5;
	grav = frametime * sv_gravity.value * 0.05;
	dvel = 4*frametime;

	for ( ;; )
	{
		kill = active_particles;
		if (kill && kill->die < cl.time)
		{
			active_particles = kill->next;
			kill->next = free_particles;
			free_particles = kill;
			continue;
		}
		break;
	}

	size_t vi = 0;
	size_t num_particles = 0;

	for (p=active_particles ; p ; p=p->next)
	{
		for ( ;; )
		{
			kill = p->next;
			if (kill && kill->die < cl.time)
			{
				p->next = kill->next;
				kill->next = free_particles;
				free_particles = kill;
				continue;
			}
			break;
		}

		p->org[0] += p->vel[0]*frametime;
		p->org[1] += p->vel[1]*frametime;
		p->org[2] += p->vel[2]*frametime;

		switch (p->type)
		{
		case pt_static:
			break;
		case pt_fire:
			p->ramp += time1;
			if (p->ramp >= 6)
				p->die = -1;
			else
				p->color = ramp3[(int)p->ramp];
			p->vel[2] += grav;
			break;

		case pt_explode:
			p->ramp += time2;
			if (p->ramp >=8)
				p->die = -1;
			else
				p->color = ramp1[(int)p->ramp];
			for (i=0 ; i<3 ; i++)
				p->vel[i] += p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_explode2:
			p->ramp += time3;
			if (p->ramp >=8)
				p->die = -1;
			else
				p->color = ramp2[(int)p->ramp];
			for (i=0 ; i<3 ; i++)
				p->vel[i] -= p->vel[i]*frametime;
			p->vel[2] -= grav;
			break;

		case pt_blob:
			for (i=0 ; i<3 ; i++)
				p->vel[i] += p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_blob2:
			for (i=0 ; i<2 ; i++)
				p->vel[i] -= p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_grav:
		case pt_slowgrav:
			p->vel[2] -= grav;
			break;
		}

		// hack a scale up to keep particles from disapearing
		float scale = (p->org[0] - r_origin[0]) * vpn[0]
			+ (p->org[1] - r_origin[1]) * vpn[1]
			+ (p->org[2] - r_origin[2]) * vpn[2];
		if (scale < 20)
			scale = 1 + 0.08; //johnfitz -- added .08 to be consistent
		else
			scale = 1 + scale * 0.004;

		scale /= 2.0; //quad is half the size of triangle

		scale *= texturescalefactor; //johnfitz -- compensate for apparent size of different particle textures

		//johnfitz -- particle transparency and fade out
		const GLubyte* c = (GLubyte*)&d_8to24table[(int)p->color];
		const float alpha = CLAMP(0, p->die + 0.5 - cl.time, 1);
		
		VectorCopy(p->org, part_verts[num_particles].pos);
		part_verts[num_particles].scale = scale;
		part_verts[num_particles].color[0] = (float)c[0]/255.0f;
		part_verts[num_particles].color[1] = (float)c[1]/255.0f;
		part_verts[num_particles].color[2] = (float)c[2]/255.0f;
		part_verts[num_particles].color[3] = alpha;
		num_particles++;
	}

	//Con_Printf("num_particles: %d\n", (int)num_particles);

	frame_particles = num_particles;
}

/*
===============
R_DrawParticles -- johnfitz -- moved all non-drawing code to CL_RunParticles
===============
*/
void R_DrawParticles (void)
{
	particle_t		*p;
	vec3_t			up, right;

	extern	cvar_t	r_particles; //johnfitz
	//float			alpha; //johnfitz -- particle transparency

	extern mat4_t r_projection_view_matrix;

	if (!r_particles.value)
		return;

	//ericw -- avoid empty glBegin(),glEnd() pair below; causes issues on AMD
	if (!active_particles)
		return;

	// gnemeth -- using vbo for particles

	VectorScale (vup, 1.25, up);
	VectorScale (vright, 1.25, right);

	glEnable (GL_BLEND);
	glDepthMask (GL_FALSE);

	GL_UseProgramFunc (part_program);

	GL_Uniform1iFunc (u_particle_texture, 0);
	GL_Uniform3fFunc (u_up, up[0], up[1], up[2]);
	GL_Uniform3fFunc (u_right, right[0], right[1], right[2]);
	GL_UniformMatrix4fvFunc (u_view_projection_matrix, 1, false, (const GLfloat*)r_projection_view_matrix);

	GL_SelectTexture (GL_TEXTURE0);
	GL_Bind (particletexture);

	GL_BindBufferFunc (GL_ARRAY_BUFFER, part_vbo);
	GL_BufferSubDataFunc (GL_ARRAY_BUFFER, 0, frame_particles * sizeof(r_part_vertex_t), part_verts);

	GL_EnableVertexAttribArrayFunc (0);
	GL_EnableVertexAttribArrayFunc (1);
	GL_VertexAttribPointerFunc (0, 4, GL_FLOAT, false, sizeof(r_part_vertex_t), NULL);
	GL_VertexAttribPointerFunc (1, 4, GL_FLOAT, false, sizeof(r_part_vertex_t), ((float*)NULL) + 4);

	glDrawArrays (GL_POINTS, 0, frame_particles);

	GL_DisableVertexAttribArrayFunc (0);
	GL_DisableVertexAttribArrayFunc (1);

	GL_BindBufferFunc (GL_ARRAY_BUFFER, 0);
	GL_UseProgramFunc (0);

	glDisable (GL_BLEND);
	glDepthMask (GL_TRUE);
}


/*
===============
R_DrawParticles_ShowTris -- johnfitz
===============
*/
void R_DrawParticles_ShowTris (void)
{
	particle_t		*p;
	float			scale;
	vec3_t			up, right, p_up, p_right, p_upright;
	extern	cvar_t	r_particles;

	if (!r_particles.value)
		return;

	VectorScale (vup, 1.5, up);
	VectorScale (vright, 1.5, right);

	if (r_quadparticles.value)
	{
		for (p=active_particles ; p ; p=p->next)
		{
			glBegin (GL_TRIANGLE_FAN);

			// hack a scale up to keep particles from disapearing
			scale = (p->org[0] - r_origin[0]) * vpn[0]
				  + (p->org[1] - r_origin[1]) * vpn[1]
				  + (p->org[2] - r_origin[2]) * vpn[2];
			if (scale < 20)
				scale = 1 + 0.08; //johnfitz -- added .08 to be consistent
			else
				scale = 1 + scale * 0.004;

			scale /= 2.0; //quad is half the size of triangle

			scale *= texturescalefactor; //compensate for apparent size of different particle textures

			glVertex3fv (p->org);

			VectorMA (p->org, scale, up, p_up);
			glVertex3fv (p_up);

			VectorMA (p_up, scale, right, p_upright);
			glVertex3fv (p_upright);

			VectorMA (p->org, scale, right, p_right);
			glVertex3fv (p_right);

			glEnd ();
		}
	}
	else
	{
		glBegin (GL_TRIANGLES);
		for (p=active_particles ; p ; p=p->next)
		{
			// hack a scale up to keep particles from disapearing
			scale = (p->org[0] - r_origin[0]) * vpn[0]
				  + (p->org[1] - r_origin[1]) * vpn[1]
				  + (p->org[2] - r_origin[2]) * vpn[2];
			if (scale < 20)
				scale = 1 + 0.08; //johnfitz -- added .08 to be consistent
			else
				scale = 1 + scale * 0.004;

			scale *= texturescalefactor; //compensate for apparent size of different particle textures

			glVertex3fv (p->org);

			VectorMA (p->org, scale, up, p_up);
			glVertex3fv (p_up);

			VectorMA (p->org, scale, right, p_right);
			glVertex3fv (p_right);
		}
		glEnd ();
	}
}

