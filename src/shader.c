/*
 * GStreamer
 * Copyright (C) 2011 Julian Scheel <julian@jusst.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <GLES2/gl2.h>

#include "shader.h"

/* FIXME: Should be part of the GLES headers */
#define GL_NVIDIA_PLATFORM_BINARY_NV                            0x890B


static const gchar* shader_basenames[] = {
    "deint_linear" /* SHADER_DEINT_LINEAR */
};

#ifndef DATA_DIR
//#define DATA_DIR "/usr/share/gstreamer-0.10/shaders"
#define DATA_DIR "/root"
#endif

#define SHADER_EXT_BINARY ".glsh"
#define SHADER_EXT_SOURCE ".glsl"

#define VERTEX_SHADER_BASENAME "vertex"

static GLuint
gl_load_binary_shader(GstGLESPlugin *sink,
                      const char *filename, GLenum type)
{
    GCancellable *cancellable;
    GLbyte *binary;
    GLsizei length;
    GLuint shader;
    GFile *file;
    GLint err;

    cancellable = g_cancellable_new();
    file = g_file_new_for_path(filename);

    /* create a shader object */
    GST_DEBUG_OBJECT(sink, "Create shader");
    shader = glCreateShader (type);
    if (shader == 0) {
        GST_ERROR_OBJECT(sink, "Could not create shader object");
        return -ENOMEM;
    }

    if (!g_file_load_contents(file, cancellable, (char**)&binary,
                             (gsize*)&length, NULL, NULL)) {
        GST_ERROR_OBJECT(sink, "Could not read binary shader from %s",
                         filename);
        goto cleanup;
    }

    GST_DEBUG_OBJECT(sink, "Load shader with %d bytes binary code", length);
    glShaderBinary(1, &shader, GL_NVIDIA_PLATFORM_BINARY_NV,
                   binary, length);

    err = glGetError();
    if (err != GL_NO_ERROR) {
        GST_ERROR_OBJECT (sink, "Error loading binary shader: %d\n", err);
        goto cleanup;
    }

    return shader;

cleanup:
    glDeleteShader (shader);
    return -EINVAL;
}

/* load and compile a shader src into a
 * shader program */
static GLuint
gl_load_source_shader (GstGLESPlugin *sink,
             const char *shader_src, GLenum type)
{
    GLuint shader;
    GLint compiled;
    GLint src_len;

    /* create a shader object */
    GST_DEBUG_OBJECT(sink, "Create shader");
    shader = glCreateShader (type);
    if (shader == 0) {
        GST_ERROR_OBJECT(sink, "Could not create shader object");
        return 0;
    }

    /* load source into shader object */
    src_len = (GLint) strlen(shader_src);
    GST_DEBUG_OBJECT(sink, "Load shader source on shader %d."
                     "Source length is %d. Source is:\n%s",
                     shader, src_len, shader_src);
    glShaderSource (shader, 1, &shader_src, &src_len);

    /* compile the shader */
    GST_DEBUG_OBJECT(sink, "Compile shader");
    glCompileShader (shader);

    /* check compiler status */
    GST_DEBUG_OBJECT(sink, "Check compiler state");
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GST_DEBUG_OBJECT(sink, "Compilation failure");
        GLint info_len = 0;

        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);

        if(info_len > 1) {
            char *info_log = malloc(sizeof(char) * info_len);
            glGetShaderInfoLog(shader, info_len, NULL, info_log);

            GST_ERROR_OBJECT(sink, "Failed to compile shader: %s", info_log);
            free(info_log);
        }

        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

/*
 * Loads a shader from either precompiled binary file when possible.
 * If no binary is found the source file is taken and compiled at
 * runtime. */
static GLuint
gl_load_shader (GstGLESPlugin *sink, const gchar *basename, const GLenum type)
{
    GLuint shader;
    gchar *filename;

    filename = g_strdup_printf ("%s/%s%s", DATA_DIR, basename,
                                SHADER_EXT_BINARY);

    shader = gl_load_binary_shader (sink, filename, type);
    if (!shader) {
        g_free (filename);
        filename = g_strdup_printf ("%s/%s%s", DATA_DIR,
                                    basename,
                                    SHADER_EXT_SOURCE);

        shader = gl_load_source_shader(sink, filename, type);
    }

    g_free (filename);
    return shader;
}

/*
 * Load vertex and fragment Shaders.
 * Vertex shader is a predefined default, fragment shader can be configured
 * through process_type */
static gint
gl_load_shaders (GstGLESPlugin *sink, Shaders process_type)
{
    sink->vertex_shader = gl_load_shader(sink, VERTEX_SHADER_BASENAME,
                                         GL_VERTEX_SHADER);
    if (!sink->vertex_shader)
        return -EINVAL;

    sink->fragment_shader = gl_load_shader(sink,
                                           shader_basenames[process_type],
                                           GL_FRAGMENT_SHADER);
    if (!sink->fragment_shader)
        return -EINVAL;

    return 0;
}

gint
gl_init_shader (GstGLESPlugin *sink)
{
    GLuint program;
    GLint linked;
    gint ret;

    /* load the shaders */
    GST_DEBUG_OBJECT(sink, "Load vertex and fragment shader");
    ret = gl_load_shaders(sink, SHADER_DEINT_LINEAR);
    if(ret < 0) {
        GST_ERROR_OBJECT(sink, "Could not create GL shaders: %d", ret);
        return ret;
    }

    GST_DEBUG_OBJECT(sink, "Create Program");
    program = glCreateProgram();
    if(!program) {
        GST_ERROR_OBJECT(sink, "Could not create GL program");
        return -ENOMEM;
    }

    GST_DEBUG_OBJECT(sink, "Attach vertex shader");
    glAttachShader(program, sink->vertex_shader);
    GST_DEBUG_OBJECT(sink, "Attach fragment shader");
    glAttachShader(program, sink->fragment_shader);
    GST_DEBUG_OBJECT(sink, "Bind vPosition");
    glBindAttribLocation(program, 0, "vPosition");

    GST_DEBUG_OBJECT(sink, "Link Program");
    glLinkProgram(program);

    /* check linker status */
    GST_DEBUG_OBJECT(sink, "Get Linker Result");
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if(!linked) {
        GST_ERROR_OBJECT(sink, "Linker failure");
        GLint info_len = 0;

        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &info_len);

        if(info_len > 1) {
            char *info_log = malloc(sizeof(char) * info_len);
            glGetProgramInfoLog(program, info_len, NULL, info_log);

            GST_ERROR_OBJECT(sink, "Failed to link GL program: %s", info_log);
            free(info_log);
        }

        glDeleteProgram(program);
        return -EINVAL;
    }

    sink->program = program;
    glUseProgram(sink->program);

    sink->position_loc = glGetAttribLocation(sink->program, "vPosition");
    sink->texcoord_loc = glGetAttribLocation(sink->program, "aTexcoord");

    sink->y_loc = glGetUniformLocation(sink->program, "s_ytex");
    sink->u_loc = glGetUniformLocation(sink->program, "s_utex");
    sink->v_loc = glGetUniformLocation(sink->program, "s_vtex");

    glClearColor(0.0, 0.0, 0.0, 1.0);

    GST_DEBUG_OBJECT(sink, "GLES init done");
    return 0;
}
