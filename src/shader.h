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

#ifndef _SHADER_H__
#define _SHADER_H__

typedef enum _Shaders               Shaders;

enum _Shaders {
    SHADER_DEINT_LINEAR = 0,
    SHADER_COPY
};

typedef struct _GstGLESShader      GstGLESShader;
typedef struct _GstGLESTexture     GstGLESTexture;

struct _GstGLESShader
{
    gint program;
    GLuint vertex_shader;
    GLuint fragment_shader;

    /* standard locations, used in most shaders */
    GLint position_loc;
    GLint texcoord_loc;
};

struct _GstGLESTexture
{
    GLuint id;
    GLint loc;
};

/* initialises the GL program with its shaders
 * and sets the program handle
 * returns 0 on succes, -1 on failure*/
gint
gl_init_shader (GstElement *sink, GstGLESShader *shader,
                Shaders process_type);
void
gl_delete_shader (GstGLESShader *shader);
#endif
