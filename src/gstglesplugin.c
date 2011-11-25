/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
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

#include <string.h>

#include <gst/gst.h>
#include <gst/video/video.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <X11/Xatom.h>

#include "gstglesplugin.h"
#include "shader.h"

GST_DEBUG_CATEGORY (gst_gles_plugin_debug);

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
static void gst_gles_plugin_finalize (GObject *gobject);

#define WxH ", width = (int) [ 16, 4096 ], height = (int) [ 16, 4096 ]"

static GstStaticPadTemplate gles_sink_factory =
        GST_STATIC_PAD_TEMPLATE ("sink",
                                 GST_PAD_SINK,
                                 GST_PAD_ALWAYS,
                                 GST_STATIC_CAPS ( GST_VIDEO_CAPS_YUV("I420") WxH));




/* OpenGL ES 2.0 implementation */
static GLuint
gl_create_texture()
{
    GLuint tex_id;

    glGenTextures (1, &tex_id);
    glBindTexture (GL_TEXTURE_2D, tex_id);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    return tex_id;
}

static void
gl_gen_framebuffer(GstGLESPlugin *sink)
{
    glGenFramebuffers (1, &sink->framebuffer);

    glGenTextures (1, &sink->copy_rgb_texture);
    if (!sink->copy_rgb_texture)
        GST_ERROR_OBJECT (sink, "Could not create RGB texture");

    glBindTexture (GL_TEXTURE_2D, sink->copy_rgb_texture);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, GST_VIDEO_SINK_WIDTH (sink),
                  GST_VIDEO_SINK_HEIGHT (sink), 0, GL_RGB,
                  GL_UNSIGNED_BYTE, NULL);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindFramebuffer (GL_FRAMEBUFFER, sink->framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, sink->copy_rgb_texture, 0);
}

static void
gl_init_textures (GstGLESPlugin *sink)
{
    //glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    sink->y_texture = gl_create_texture();
    sink->u_texture = gl_create_texture();
    sink->v_texture = gl_create_texture();
}

static void
gl_load_texture (GstGLESPlugin *sink, GstBuffer *buf)
{
    /* y component */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, sink->y_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, GST_VIDEO_SINK_WIDTH (sink),
                 GST_VIDEO_SINK_HEIGHT (sink), 0, GL_LUMINANCE,
                 GL_UNSIGNED_BYTE, GST_BUFFER_DATA (buf));
    glUniform1i (sink->y_loc, 0);

    /* u component */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture (GL_TEXTURE_2D, sink->u_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                 GST_VIDEO_SINK_WIDTH (sink)/2,
                 GST_VIDEO_SINK_HEIGHT (sink)/2, 0, GL_LUMINANCE,
                 GL_UNSIGNED_BYTE, GST_BUFFER_DATA (buf) +
                 GST_VIDEO_SINK_WIDTH (sink) * GST_VIDEO_SINK_HEIGHT (sink));
    glUniform1i (sink->u_loc, 1);

    /* v component */
    glActiveTexture(GL_TEXTURE2);
    glBindTexture (GL_TEXTURE_2D, sink->v_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                 GST_VIDEO_SINK_WIDTH (sink)/2,
                 GST_VIDEO_SINK_HEIGHT (sink)/2, 0, GL_LUMINANCE,
                 GL_UNSIGNED_BYTE, GST_BUFFER_DATA (buf) +
                 GST_VIDEO_SINK_WIDTH (sink) * GST_VIDEO_SINK_HEIGHT (sink) +
                 GST_VIDEO_SINK_WIDTH (sink)/2 *
                 GST_VIDEO_SINK_HEIGHT (sink)/2);
    glUniform1i (sink->v_loc, 2);

}

static void
gl_draw_fbo (GstGLESPlugin *sink, GstBuffer *buf)
{
    GLfloat vVertices[] =
    {
        -1.0f, -1.0f,
        0.0f, 1.0f,

        1.0f, -1.0f,
        1.0f, 1.0f,

        1.0f, 1.0f,
        1.0f, 0.0f,

        -1.0f, 1.0f,
        0.0f, 0.0f,
    };
    GLushort indices[] = { 0, 1, 2, 0, 2, 3 };

    glBindFramebuffer (GL_FRAMEBUFFER, sink->framebuffer);
    glUseProgram (sink->program);
    //glViewport (result.x, result.y, result.w, result.h);
    glViewport(0, 0, GST_VIDEO_SINK_WIDTH (sink),
               GST_VIDEO_SINK_HEIGHT (sink));

    glClear (GL_COLOR_BUFFER_BIT);

    glVertexAttribPointer (sink->position_loc, 2, GL_FLOAT,
        GL_FALSE, 4 * sizeof (GLfloat), vVertices);

    glVertexAttribPointer (sink->texcoord_loc, 2, GL_FLOAT,
        GL_FALSE, 4 * sizeof (GLfloat), &vVertices[2]);

    glEnableVertexAttribArray (sink->position_loc);
    glEnableVertexAttribArray (sink->texcoord_loc);

    gl_load_texture(sink, buf);
    GLint line_height_loc = glGetUniformLocation(sink->program,
                                                 "line_height");
    glUniform1f(line_height_loc, 1.0/sink->video_height);

    glDrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
}

void
gl_draw_onscreen (GstGLESPlugin *sink)
{
    GLfloat vVertices[] =
    {
        -1.0f, -1.0f,
        0.0f, 0.0f,

        1.0f, -1.0f,
        1.0f, 0.0f,

        1.0f, 1.0f,
        1.0f, 1.0f,

        -1.0f, 1.0f,
        0.0f, 1.0f,
    };
    GLushort indices[] = { 0, 1, 2, 0, 2, 3 };

    GstVideoRectangle src;
    GstVideoRectangle dst;
    GstVideoRectangle result;

    dst.x = 0;
    dst.y = 0;
    dst.w = sink->window_width;
    dst.h = sink->window_height;

    src.x = 0;
    src.y = 0;
    src.w = sink->video_width;
    src.h = sink->video_height;

    gst_video_sink_center_rect(src, dst, &result, TRUE);

    glUseProgram (sink->copy_program);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);

    glViewport (result.x, result.y, result.w, result.h);

    glClear (GL_COLOR_BUFFER_BIT);

    glVertexAttribPointer (sink->copy_position_loc, 2, GL_FLOAT,
        GL_FALSE, 4 * sizeof (GLfloat), vVertices);

    glVertexAttribPointer (sink->copy_texcoord_loc, 2, GL_FLOAT,
        GL_FALSE, 4 * sizeof (GLfloat), &vVertices[2]);

    glEnableVertexAttribArray (sink->copy_position_loc);
    glEnableVertexAttribArray (sink->copy_texcoord_loc);


    glActiveTexture(GL_TEXTURE3);
    glBindTexture (GL_TEXTURE_2D, sink->copy_rgb_texture);
    glUniform1i (sink->copy_rgb_loc, 3);

    glDrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
    eglSwapBuffers (sink->display, sink->surface);
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
        GST_WARNING_OBJECT(sink, "Did not get exactly one config, but %d",
                           num_configs);
    }

    GST_DEBUG_OBJECT (sink, "Create EGL window surface, "
                      "display: %p", sink->x_display);
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
    GLuint textures[] = { 0, 1, 2 };

    GST_DEBUG_OBJECT (sink,"egl close");
    if (sink->initialized) {
        glDeleteTextures (3, textures);
        glDeleteShader (sink->vertex_shader);
        glDeleteShader (sink->fragment_shader);
        glDeleteProgram (sink->program);
    }

    if (sink->surface) {
        eglDestroySurface (sink->display, sink->surface) ;
        sink->surface = NULL;
    }

    if (sink->context) {
        eglDestroyContext (sink->display, sink->context);
        sink->context = NULL;
    }

    if (sink->display) {
        eglTerminate (sink->display);
        sink->display = NULL;
    }
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
    GST_DEBUG_OBJECT(sink,"x11 close");
    if (sink->x_display) {
        XDestroyWindow(sink->x_display, sink->x_window);
        XCloseDisplay(sink->x_display);
        sink->x_display = NULL;
    }
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

  gobject_class->finalize = gst_gles_plugin_finalize;

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

  gst_base_sink_set_max_lateness (GST_BASE_SINK (sink), 20 * GST_MSECOND);
  gst_base_sink_set_qos_enabled(GST_BASE_SINK (sink), TRUE);
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

    GST_DEBUG_OBJECT (sink, "Initialized");

    return TRUE;
}

/* deinitialisation code */
static gboolean
gst_gles_plugin_stop (GstBaseSink *basesink)
{
    GstGLESPlugin *sink = GST_GLES_PLUGIN (basesink);

    GST_VIDEO_SINK_WIDTH (sink) = 0;
    GST_VIDEO_SINK_HEIGHT (sink)  = 0;

    egl_close (sink);
    x11_close (sink);

    sink->initialized = FALSE;
    GST_LOG_OBJECT (sink, "stopped");

    return TRUE;
}

/* this function handles the link with other elements */
static gboolean
gst_gles_plugin_set_caps (GstBaseSink *basesink, GstCaps *caps)
{
  GstGLESPlugin *sink = GST_GLES_PLUGIN (basesink);
  GstVideoFormat fmt;
  guint display_par_n;
  guint display_par_d;
  gint par_n;
  gint par_d;
  gint w;
  gint h;

  GST_LOG_OBJECT (sink, "caps: %" GST_PTR_FORMAT, caps);

  if (!gst_video_format_parse_caps (caps, &fmt, &w, &h)) {
      GST_WARNING_OBJECT (sink, "pase_caps failed");
      return FALSE;
  }

  /* retrieve pixel aspect ratio of encoded video */
  if (!gst_video_parse_caps_pixel_aspect_ratio (caps, &par_n, &par_d)) {
      GST_WARNING_OBJECT (sink, "no pixel aspect ratio");
      return FALSE;
  }

  g_assert ((fmt == GST_VIDEO_FORMAT_I420));
  sink->video_width = w;
  sink->video_height = h;
  GST_VIDEO_SINK_WIDTH (sink) = w;
  GST_VIDEO_SINK_HEIGHT (sink) = h;

  /* calculate actual rendering pixel aspect ratio based on video pixel
   * aspect ratio and display pixel aspect ratio */
  /* FIXME: add display pixel aspect ratio as property to the plugin */
  display_par_n = 1;
  display_par_d = 1;

  gst_video_calculate_display_ratio (&sink->par_n, &sink->par_d,
                                     GST_VIDEO_SINK_WIDTH (sink),
                                     GST_VIDEO_SINK_HEIGHT (sink),
                                     par_n, par_d,
                                     display_par_n, display_par_d);

  sink->video_width = sink->video_width * par_n / par_d;

  GST_INFO_OBJECT (sink, "format                : %d", fmt);
  GST_INFO_OBJECT (sink, "encoded width x height: %d x %d", w, h);
  GST_INFO_OBJECT (sink, "display width x height: %d x %d", sink->video_width,
                   sink->video_height);
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

    /* FIXME: this shouldn't be done here, but in _start() due to _start
     *        being in a different thread this leads to problems. has to be
     *        investigated further. */
    if(!sink->initialized) {
        gint ret;

        GST_DEBUG_OBJECT(sink, "Initialize X11");
        sink->window_width = 720;
        sink->window_height = 576;
        if (x11_init (sink, sink->window_width, sink->window_height) < 0) {
            GST_ERROR_OBJECT (sink, "X11 init failed, abort");
            return GST_FLOW_ERROR;
        }

        GST_DEBUG_OBJECT (sink, "Initialize EGL");
        if (egl_init (sink) < 0) {
            GST_ERROR_OBJECT (sink, "EGL init failed, abort");
            x11_close (sink);
            return GST_FLOW_ERROR;
        }

        GST_DEBUG_OBJECT (sink, "Initialize GLES Shaders");
        ret = gl_init_shader (sink);
        if (ret < 0) {
            GST_ERROR_OBJECT (sink, "Could not initialize shader: %d", ret);
            egl_close (sink);
            x11_close (sink);
            sink->initialized = FALSE;
            return GST_FLOW_ERROR;
        }

        GST_DEBUG_OBJECT (sink, "Initialize GLES copy Shaders");
        ret = gl_init_copy_shader (sink);
        if (ret < 0) {
            GST_ERROR_OBJECT (sink, "Could not initialize copy shader: %d",
                              ret);
            egl_close (sink);
            x11_close (sink);
            sink->initialized = FALSE;
            return GST_FLOW_ERROR;
        }

        GST_DEBUG_OBJECT (sink, "Initialize GLES Textures");
        gl_init_textures (sink);

        /* generate the framebuffer object */
        GST_DEBUG_OBJECT (sink, "Initialize FBO");
        gl_gen_framebuffer (sink);

        sink->initialized = TRUE;

        GST_DEBUG_OBJECT (sink, "Init done");
    }

    while (XPending (sink->x_display)) {// check for events from the x-server
        XEvent  xev;
        XNextEvent(sink->x_display, &xev);

        switch (xev.type) {
        case ConfigureRequest:
        case ConfigureNotify:
            GST_DEBUG_OBJECT(sink, "XConfigure* Event: wxh: %dx%d",
                             xev.xconfigure.width,
                             xev.xconfigure.height);
            sink->window_width = xev.xconfigure.width;
            sink->window_height = xev.xconfigure.height;
            break;
        default:
            break;
        }
    }

    gl_draw_fbo (sink, buf);
    gl_draw_onscreen (sink);

    return GST_FLOW_OK;
}

static void
gst_gles_plugin_finalize (GObject *gobject)
{
    GstGLESPlugin *plugin = (GstGLESPlugin *)gobject;
    egl_close(plugin);
    x11_close(plugin);
    plugin->initialized = FALSE;
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
