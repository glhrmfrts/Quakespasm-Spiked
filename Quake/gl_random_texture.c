#include "gl_random_texture.h"
#include "stb_image.h"

static unsigned char random_texture_data[] = {
#include "gl_random_texture_data.h"
};

GLuint GL_GetRandomTexture() {
    static qboolean created;
    static GLuint random_texture;
    if (!created) {
        int width, height, channels;
        unsigned char* texdata = stbi_load_from_memory (random_texture_data, sizeof(random_texture_data), &width, &height, &channels, 4);

        glGenTextures (1, &random_texture);
        glBindTexture (GL_TEXTURE_2D, random_texture);

        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, texdata);

        glBindTexture (GL_TEXTURE_2D, 0);

        created = true;

        free (texdata);
    }
    return random_texture;
}