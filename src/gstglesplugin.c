/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) YEAR AUTHOR_NAME AUTHOR_EMAIL
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
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

/**
 * SECTION:element-glesplugin
 *
 *
 * <refsect2>
 * <title>OpenGL ES2.0 videosink plugin</title>
 * |[
 * gst-launch -v -m videotestsrc ! glessink
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>
#include <gst/video/video.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include  <X11/Xatom.h>

#include "gstglesplugin.h"

GST_DEBUG_CATEGORY_STATIC (gst_gles_plugin_debug);
#define GST_CAT_DEFAULT gst_gles_plugin_debug

enum
{
  PROP_0,
  PROP_SILENT
};

GST_BOILERPLATE (GstGLESPlugin, gst_gles_plugin, GstVideoSink,
    GST_TYPE_VIDEO_SINK);

static void gst_gles_plugin_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_gles_plugin_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_gles_plugin_start (GstBaseSink * basesink);
static gboolean gst_gles_plugin_stop (GstBaseSink * basesink);
static gboolean gst_gles_plugin_set_caps (GstBaseSink * basesink,
                                          GstCaps * caps);
static GstFlowReturn gst_gles_plugin_render (GstBaseSink * basesink,
                                             GstBuffer * buf);
static GstFlowReturn gst_gles_plugin_preroll (GstBaseSink * basesink,
                                              GstBuffer * buf);

#define WxH ", width = (int) [ 16, 4096 ], height = (int) [ 16, 4096 ]"

static GstStaticPadTemplate gles_sink_factory =
        GST_STATIC_PAD_TEMPLATE ("sink",
                                 GST_PAD_SINK,
                                 GST_PAD_ALWAYS,
                                 GST_STATIC_CAPS ( GST_VIDEO_CAPS_YUV("I420") WxH));


/* OpenGL ES 2.0 implementation */

/* load and compile a shader src into a
 * shader program */
static GLuint
gl_load_shader (GstGLESPlugin *sink,
             const char *shader_src, GLenum type)
{
    GLuint shader;
    GLint compiled;

    /* create a shader object */
    shader = glCreateShader (type);
    if (shader == 0) {
        GST_ERROR_OBJECT(sink, "Could not create shader object");
        return 0;
    }

    /* load source into shader object */
    glShaderSource (shader, 1, &shader_src, NULL);

    /* compile the shader */
    glCompileShader (shader);

    /* check compiler status */
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

    if (!compiled) {
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

/* initialises the GL program with its shaders
 * and sets the program handle
 * returns 0 on succes, -1 on failure*/
static gint
gl_init_shader (GstGLESPlugin *sink)
{
    const char *vertex_shader_src =
            "attribute vec4 vPosition;                      \n"
            "attribute vec2 aTexcoord;                      \n"
            "varying vec2 vTexcoord;                        \n"
            ""
            "void main()                                    \n"
            "{                                              \n"
            "   gl_Position = vPosition;                    \n"
            "   vTexcoord = aTexcoord;                      \n"
            "}                                              \n";

    const char *fragment_shader_src =
            "precision mediump float;                       \n"
            "varying vec2 vTexcoord;                        \n"
            "uniform sampler2D s_texture;                   \n"
            ""
            "void main()                                    \n"
            "{                                              \n"
            "   gl_FragColor = texture2D(s_texture, vTexcoord);\n"
            "}                                              \n";

    GLuint vertex_shader;
    GLuint fragment_shader;
    GLuint program;
    GLint linked;

    /* load the shaders */
    vertex_shader = gl_load_shader(sink, vertex_shader_src, GL_VERTEX_SHADER);
    fragment_shader = gl_load_shader(sink, fragment_shader_src, GL_FRAGMENT_SHADER);

    program = glCreateProgram();

    if(!program) {
        GST_ERROR_OBJECT(sink, "Could not create GL program");
        return -1;
    }

    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glBindAttribLocation(program, 0, "vPosition");

    glLinkProgram(program);

    /* check linker status */
    glGetProgramiv(program, GL_LINK_STATUS, &linked);

    if(!linked) {
        GLint info_len = 0;

        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &info_len);

        if(info_len > 1) {
            char *info_log = malloc(sizeof(char) * info_len);
            glGetProgramInfoLog(program, info_len, NULL, info_log);

            GST_ERROR_OBJECT(sink, "Failed to link GL program: %s", info_log);
            free(info_log);
        }

        glDeleteProgram(program);
        return -1;
    }

    sink->program = program;
    glUseProgram(sink->program);

    sink->position_loc = glGetAttribLocation(sink->program, "vPosition");
    sink->sampler_loc = glGetAttribLocation(sink->program, "aTexcoord");

    glClearColor(0.0, 0.0, 0.0, 1.0);
    return 0;
}

static void
gl_draw (GstGLESPlugin *sink)
{
    GLfloat vVertices[] =
    {
        0.0f, 0.5f, 0.0f,
        -0.5f, -0.5f, 0.0f,
        0.5f, -0.5f, 0.0f
    };

    glViewport (0, 0, GST_VIDEO_SINK_WIDTH (sink),
               GST_VIDEO_SINK_HEIGHT (sink));

    glClear (GL_COLOR_BUFFER_BIT);

    glUniform1i(sink->sampler_loc, 0);
    glVertexAttribPointer (sink->position_loc, 3, GL_FLOAT, GL_FALSE, 0, vVertices);
    glEnableVertexAttribArray (sink->position_loc);

    glDrawArrays (GL_TRIANGLES, 0, 3);

    eglSwapBuffers (sink->display, sink->surface);
}

static void
gl_init_textures (GstGLESPlugin *sink)
{
    //glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &sink->texture);
    glBindTexture(GL_TEXTURE_2D, sink->texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

static void
gl_load_texture (GstGLESPlugin *sink, GstBuffer *buf)
{
    glActiveTexture(sink->texture);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, sink->video_width,
                 sink->video_height, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, GST_BUFFER_DATA (buf));
}

/* EGL implementation */

static gint
egl_init (GstGLESPlugin *sink)
{
    const EGLint configAttribs[] =
    {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };

    const EGLint contextAttribs[] =
    {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_configs;
    EGLint major;
    EGLint minor;

    GST_DEBUG_OBJECT (sink, "Get EGL display");
    sink->display = eglGetDisplay((EGLNativeDisplayType) sink->x_display);
    if (sink->display == EGL_NO_DISPLAY) {
        GST_ERROR_OBJECT(sink, "Could not get EGL display");
        return -1;
    }

    GST_DEBUG_OBJECT (sink, "Initialize EGL");
    if (!eglInitialize(sink->display, &major, &minor)) {
        GST_ERROR_OBJECT(sink, "Could not initialize EGL context");
        return -1;
    }
    GST_DEBUG_OBJECT (sink, "Have EGL version: %d.%d", major, minor);

    GST_DEBUG_OBJECT (sink, "Choose EGL config");
    if (!eglChooseConfig(sink->display, configAttribs, &config, 1,
                        &num_configs)) {
        GST_ERROR_OBJECT(sink, "Could not choose EGL config");
        return -1;
    }

    if (num_configs != 1) {
        GST_WARNING_OBJECT(sink, "Did not get exactly one config, but %d", num_configs);
    }

    GST_DEBUG_OBJECT (sink, "Create EGL window surface");
    sink->surface = eglCreateWindowSurface(sink->display, config,
                                     sink->x_window, NULL);
    if (sink->surface == EGL_NO_SURFACE) {
        GST_ERROR_OBJECT (sink, "Could not create EGL surface");
        return -1;
    }

    GST_DEBUG_OBJECT (sink, "Create EGL context");
    sink->context = eglCreateContext(sink->display, config,
                                     EGL_NO_CONTEXT, contextAttribs);
    if (sink->context == EGL_NO_CONTEXT) {
        GST_ERROR_OBJECT(sink, "Could not create EGL context");
        return -1;
    }

    GST_DEBUG_OBJECT (sink, "Switch EGL context to current");
    if (!eglMakeCurrent(sink->display, sink->surface, sink->surface,
                        sink->context)) {
        GST_ERROR_OBJECT(sink, "Could not set EGL context to current");
        return -1;
    }

    return 0;
}

static void
egl_close(GstGLESPlugin *sink)
{
    eglDestroyContext (sink->display, sink->context);
    eglDestroySurface (sink->display, sink->surface);
    eglTerminate (sink->display);
}

static gint
x11_init (GstGLESPlugin *sink, gint width, gint height)
{
    Window root;
    XSetWindowAttributes swa;
    XWMHints hints;

    sink->x_display = XOpenDisplay (NULL);
    if(!sink->x_display) {
        GST_ERROR_OBJECT(sink, "Could not create X display");
        return -1;
    }

    root = DefaultRootWindow (sink->x_display);
    swa.event_mask =
            StructureNotifyMask | ExposureMask | VisibilityChangeMask;

    sink->x_window = XCreateWindow (
                sink->x_display, root,
                0, 0, width, height, 0,
                CopyFromParent, InputOutput,
                CopyFromParent, CWEventMask,
                &swa);

    XSetWindowBackgroundPixmap (sink->x_display, sink->x_window, None);

    hints.input = True;
    hints.flags = InputHint;
    XSetWMHints(sink->x_display, sink->x_window, &hints);

    XMapWindow (sink->x_display, sink->x_window);
    XStoreName (sink->x_display, sink->x_window, "GLESSink");

    return 0;
}

static void
x11_close (GstGLESPlugin *sink)
{
    XDestroyWindow(sink->x_display, sink->x_window);
    XCloseDisplay(sink->x_display);
}


/* GObject vmethod implementations */

static void
gst_gles_plugin_base_init (gpointer gclass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (gclass);

  gst_element_class_set_details_simple(element_class,
    "GLESPlugin sink",
    "Sink/Video",
    "Output video using Open GL ES 2.0",
    "Julian Scheel <julian jusst de>");

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gles_sink_factory));
}

/* initialize the plugin's class */
static void
gst_gles_plugin_class_init (GstGLESPluginClass * klass)
{
  GstBaseSinkClass *basesink_class = GST_BASE_SINK_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->set_property = gst_gles_plugin_set_property;
  gobject_class->get_property = gst_gles_plugin_get_property;

  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Produce verbose output ?",
          FALSE, G_PARAM_READWRITE));

  /* initialise virtual methods */
  basesink_class->start = GST_DEBUG_FUNCPTR (gst_gles_plugin_start);
  basesink_class->stop = GST_DEBUG_FUNCPTR (gst_gles_plugin_stop);
  basesink_class->render = GST_DEBUG_FUNCPTR (gst_gles_plugin_render);
  basesink_class->preroll = GST_DEBUG_FUNCPTR (gst_gles_plugin_preroll);
  basesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_gles_plugin_set_caps);
}

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static void
gst_gles_plugin_init (GstGLESPlugin * sink,
    GstGLESPluginClass * gclass)
{
  sink->silent = FALSE;
  sink->initialized = FALSE;

  gst_base_sink_set_max_lateness (GST_BASE_SINK (sink), -1);
  gst_base_sink_set_qos_enabled(GST_BASE_SINK (sink), FALSE);
}

static void
gst_gles_plugin_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstGLESPlugin *filter = GST_GLES_PLUGIN (object);

  switch (prop_id) {
    case PROP_SILENT:
      filter->silent = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_gles_plugin_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstGLESPlugin *filter = GST_GLES_PLUGIN (object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, filter->silent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GstElement vmethod implementations */

/* initialisation code */
static gboolean
gst_gles_plugin_start (GstBaseSink *basesink)
{
    GstGLESPlugin *sink = GST_GLES_PLUGIN (basesink);

    GST_LOG_OBJECT (basesink, "start in thread %p", g_thread_self());

    GST_DEBUG_OBJECT(sink, "Initialized");

    return TRUE;
}

/* deinitialisation code */
static gboolean
gst_gles_plugin_stop (GstBaseSink *basesink)
{
    GstGLESPlugin *sink = GST_GLES_PLUGIN (basesink);

    GST_VIDEO_SINK_WIDTH (sink) = 0;
    GST_VIDEO_SINK_HEIGHT (sink)  = 0;

    egl_close(sink);
    x11_close (sink);
    GST_LOG_OBJECT (sink, "stop");

    return TRUE;
}

/* this function handles the link with other elements */
static gboolean
gst_gles_plugin_set_caps (GstBaseSink *basesink, GstCaps *caps)
{
  GstGLESPlugin *sink = GST_GLES_PLUGIN (basesink);
  GstVideoFormat fmt;
  gint w, h, par_n, par_d;

  GST_LOG_OBJECT (sink, "caps: %" GST_PTR_FORMAT, caps);

  if (!gst_video_format_parse_caps (caps, &fmt, &w, &h)) {
      GST_WARNING_OBJECT (sink, "pase_caps failed");
      return FALSE;
  }

  if (!gst_video_parse_caps_pixel_aspect_ratio (caps, &par_n, &par_d)) {
      GST_WARNING_OBJECT (sink, "no pixel aspect ratio");
      return FALSE;
  }

  g_assert ((fmt == GST_VIDEO_FORMAT_I420));
  sink->video_width = w;
  sink->video_height = h;
  GST_VIDEO_SINK_WIDTH (sink) = w;
  GST_VIDEO_SINK_HEIGHT (sink) = h;

  sink->par_n = par_n;
  sink->par_d = par_d;

  GST_INFO_OBJECT (sink, "format                : %d", fmt);
  GST_INFO_OBJECT (sink, "width x height        : %d x %d", w, h);
  GST_INFO_OBJECT (sink, "pixel-aspect-ratio    : %d/%d", par_d, par_n);

  return TRUE;
}

static GstFlowReturn
gst_gles_plugin_preroll (GstBaseSink * basesink, GstBuffer * buf)
{
    GST_LOG_OBJECT (basesink, "preroll");
    return GST_FLOW_OK;
}

static GstFlowReturn
gst_gles_plugin_render (GstBaseSink *basesink, GstBuffer *buf)
{
    GstGLESPlugin *sink = GST_GLES_PLUGIN (basesink);

    if(!sink->initialized) {

        GST_DEBUG_OBJECT(sink, "Initialize X11");
        if(x11_init(sink, 720, 576) < 0) {
            GST_ERROR_OBJECT(sink, "X11 init failed, abort");
            return GST_FLOW_ERROR;
        }

        GST_DEBUG_OBJECT(sink, "Initialize EGL");
        if(egl_init(sink) < 0) {
            GST_ERROR_OBJECT(sink, "EGL init failed, abort");
            x11_close(sink);
            return GST_FLOW_ERROR;
        }

        GST_DEBUG_OBJECT(sink, "Initialize GLES Shaders");
        if(gl_init_shader(sink) < 0) {
            GST_ERROR_OBJECT(sink, "Could not initialize shader");
            egl_close(sink);
            x11_close(sink);
            return GST_FLOW_ERROR;
        }

        GST_DEBUG_OBJECT(sink, "Initialize GLES Textures");
        gl_init_textures(sink);
        sink->initialized = TRUE;

        GST_DEBUG_OBJECT(sink, "Init done");
    }

    /*
    while (XPending (sink->x_display)) {   // check for events from the x-server
        XEvent  xev;
        XNextEvent(sink->x_display, &xev);
    }*/

    gl_load_texture(sink, buf);
    gl_draw(sink);

    return GST_FLOW_OK;
}

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
plugin_init (GstPlugin * plugin)
{
  /* debug category for fltering log messages
   *
   * exchange the string 'Template plugin' with your description
   */
  GST_DEBUG_CATEGORY_INIT (gst_gles_plugin_debug, "glesplugin",
      0, "OpenGL ES 2.0 plugin");

  return gst_element_register (plugin, "glesplugin", GST_RANK_NONE,
      GST_TYPE_GLES_PLUGIN);
}

/* PACKAGE: this is usually set by autotools depending on some _INIT macro
 * in configure.ac and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use autotools to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "glesplugin"
#endif

/* gstreamer looks for this structure to register plugins
 *
 * exchange the string 'Template plugin' with your plugin description
 */
GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "glesplugin",
    "Open GL ES 2.0 plugin",
    plugin_init,
    VERSION,
    "LGPL",
    "Avionic Design",
    "http://avionic-design.de/"
)
