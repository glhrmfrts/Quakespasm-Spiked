#include "quakedef.h"
#include "glquake.h"
#include <assert.h>

static qboolean compile_shader_stage(GLuint Shader, const char *Source)
{
	GL_ShaderSourceFunc(Shader, 1, &Source, NULL);
	GL_CompileShaderFunc(Shader);

	int Status = 0;
	GL_GetShaderivFunc(Shader, GL_COMPILE_STATUS, &Status);
	if (Status == GL_FALSE) {
		int logLength = 0;
		GL_GetShaderivFunc(Shader, GL_INFO_LOG_LENGTH, &logLength);

		char *infoLog = malloc(logLength + 1);
		GL_GetShaderInfoLogFunc(Shader, logLength, NULL, infoLog);
		infoLog[logLength] = '\0';

		COM_WriteFile ("failedshader.glsl", Source, strlen(Source));

		Con_SafePrintf("failed to compile:\n%s\n", Source);
		//exit(EXIT_FAILURE);
		Con_SafePrintf("Shader error: %s", infoLog);
		return false;
	}
	return true;
}

static qboolean link_shader_program(
	GLuint progID, GLuint vertexShader, GLuint fragmentShader, GLuint geometryShader,
	int numbindings, const glsl_attrib_binding_t *bindings
)
{
	GL_AttachShaderFunc(progID, vertexShader);
	GL_AttachShaderFunc(progID, fragmentShader);
	if (geometryShader) {
		GL_AttachShaderFunc(progID, geometryShader);
	}
	for (int i = 0; i < numbindings; i++) {
		GL_BindAttribLocationFunc (progID, bindings[i].attrib, bindings[i].name);
	}
	GL_LinkProgramFunc(progID);

	int Status;
	GL_GetProgramivFunc(progID, GL_LINK_STATUS, &Status);
	if (Status == GL_FALSE) {
		int LogLength = 0;
		GL_GetProgramivFunc(progID, GL_INFO_LOG_LENGTH, &LogLength);

		char *infoLog = malloc(LogLength + 1);
		GL_GetProgramInfoLogFunc(progID, LogLength, NULL, infoLog);
		infoLog[LogLength] = '\0';

		Con_SafePrintf("failed to link program: %s\n", infoLog);
		Con_SafePrintf("Program error: %s", infoLog);
		// exit(EXIT_FAILURE);
	}

	GL_DeleteShaderFunc (vertexShader);
	GL_DeleteShaderFunc (fragmentShader);
	GL_DeleteShaderFunc (geometryShader);
	return (Status == GL_TRUE);
}

static qboolean compile_shader(gl_shader_t* sh, const char* vert_source, const char* geom_source, const char* frag_source, int numbindings, const glsl_attrib_binding_t *bindings) {
	if (!sh->program_id) {
		sh->program_id = GL_CreateProgramFunc();
		sh->vertex_shader = GL_CreateShaderFunc(GL_VERTEX_SHADER);
		sh->fragment_shader = GL_CreateShaderFunc(GL_FRAGMENT_SHADER);
		if (geom_source) {
			sh->geometry_shader = GL_CreateShaderFunc(GL_GEOMETRY_SHADER);
		}
	}

	qboolean compiled = compile_shader_stage(sh->vertex_shader, vert_source);
	compiled = compile_shader_stage(sh->fragment_shader, frag_source);

	if (geom_source) {
		compiled = compile_shader_stage(sh->geometry_shader, geom_source);
	}

	if (compiled) {
		link_shader_program(
			sh->program_id, sh->vertex_shader, sh->fragment_shader, sh->geometry_shader, numbindings, bindings
		);
	}

	return compiled;
}

qboolean GL_CreateShaderFromVF(
	gl_shader_t* sh, const char* vert_source, const char* frag_source,
	int numbindings, const glsl_attrib_binding_t *bindings
) {
	return compile_shader (sh, vert_source, NULL, frag_source, numbindings, bindings);
}

qboolean GL_CreateShaderFromVGF(
	gl_shader_t* sh, const char* vert_source, const char* geom_source, const char* frag_source,
	int numbindings, const glsl_attrib_binding_t *bindings
) {
	return compile_shader (sh, vert_source, geom_source, frag_source, numbindings, bindings);
}

void GL_DestroyShader(gl_shader_t* sh) {
	GL_DeleteProgramFunc (1, &sh->program_id);
}