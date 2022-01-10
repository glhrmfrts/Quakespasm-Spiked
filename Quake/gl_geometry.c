#include "quakedef.h"
#include "glquake.h"
#include <assert.h>

gl_vertex_attribute_t GL_CreatePositionVertexAttribute() {
    gl_vertex_attribute_t va = { 0 };
    va.data_type = GL_FLOAT;
    va.size = 3;
    va.type = gl_vertex_attribute_type_position;
    return va;
}

gl_vertex_attribute_t GL_CreateTexCoordVertexAttribute() {
    gl_vertex_attribute_t va = { 0 };
    va.data_type = GL_FLOAT;
    va.size = 2;
    va.type = gl_vertex_attribute_type_texcoord;
    return va;
}

gl_vertex_attribute_t GL_CreateNormalVertexAttribute() {
    gl_vertex_attribute_t va = { 0 };
    va.data_type = GL_FLOAT;
    va.size = 3;
    va.type = gl_vertex_attribute_type_normal;
    return va;
}

static GLenum ConvertGLUsage(gl_geometry_type_t type) {
    switch (type) {
    case gl_geometry_type_static: return GL_STATIC_DRAW;
    case gl_geometry_type_stream: return GL_STREAM_DRAW;
    case gl_geometry_type_dynamic: return GL_DYNAMIC_DRAW;
    default: assert(!"unreachable"); return 0;
    }
}

static size_t GLTypeSize(GLenum data_type) {
    return sizeof(float);
}

static void EnableVertexAttribute(const gl_vertex_attribute_t* attr) {
    GL_EnableVertexAttribArrayFunc(attr->location);
    GL_VertexAttribPointerFunc(attr->location, attr->size, attr->data_type, false, attr->stride, (void*)(attr->offset));
}

qboolean GL_CreateGeometry(gl_geometry_t* g, gl_geometry_type_t type, const gl_vertex_attribute_t attrs[]) {
    g->vertex_size = 0;
    size_t stride = 0;
    size_t offset = 0;
    unsigned int i = 0;

    const gl_vertex_attribute_t* attr = attrs;
    while (attr->type) {
        g->vertex_size += attr->size;
        g->attributes[g->num_attributes++] = *attr;
        attr++;
    }

    for (size_t i = 0; i < g->num_attributes; i++) {
        gl_vertex_attribute_t* attr = g->attributes + i;
        attr->offset = stride;
        stride += attr->size * GLTypeSize(attr->data_type);
        attr->location = i;
    }

    GL_GenVertexArraysFunc(1, &g->vertex_array_id);
    GL_GenBuffersFunc(1, &g->vertex_buffer_id);
    GL_GenBuffersFunc(1, &g->index_buffer_id);

    GL_BindVertexArrayFunc(g->vertex_array_id);
    GL_BindBufferFunc(GL_ARRAY_BUFFER, g->vertex_buffer_id);

    for (size_t i = 0; i < g->num_attributes; i++) {
        gl_vertex_attribute_t* attr = g->attributes + i;
        attr->stride = stride;
        EnableVertexAttribute(attr);
    }

    GL_BindBufferFunc (GL_ARRAY_BUFFER, 0);
    GL_BindVertexArrayFunc (0);

    return true;
}

qboolean GL_AllocateQuads(gl_geometry_t* g, size_t num_quads) {
    enum { VERTS_PER_QUAD = 4, INDS_PER_QUAD = 6 };

    if (g->vertex_data) {
        free (g->vertex_data);
    }
    if (g->index_data) {
        free (g->index_data);
    }

    g->vertex_data_size = num_quads * VERTS_PER_QUAD * g->vertex_size;
    g->index_data_size = num_quads * INDS_PER_QUAD;
    g->vertex_data = malloc(g->vertex_data_size * sizeof(float));
    g->index_data = malloc(g->index_data_size * sizeof(unsigned int));

    return g->vertex_data != NULL && g->index_data != NULL;
}

qboolean GL_SendGeometry(gl_geometry_t* g) {
    GL_BindBufferFunc(GL_ARRAY_BUFFER, g->vertex_buffer_id);
    GL_BufferDataFunc(GL_ARRAY_BUFFER,
        g->vertex_data_size * sizeof(float),
        (void*)g->vertex_data,
        ConvertGLUsage(g->type));

    GL_BindBufferFunc(GL_ELEMENT_ARRAY_BUFFER, g->index_buffer_id);
    GL_BufferDataFunc(GL_ELEMENT_ARRAY_BUFFER,
        g->index_data_size * sizeof(unsigned int),
        (void*)g->index_data,
        ConvertGLUsage(g->type));

    return true;
}

void GL_DestroyGeometry(gl_geometry_t* g) {
    GL_DeleteBuffersFunc(1, &g->vertex_buffer_id);
    GL_DeleteBuffersFunc(1, &g->index_buffer_id);
    GL_DeleteVertexArraysFunc(1, &g->vertex_array_id);
    free (g->vertex_data);
    free (g->index_data);
}