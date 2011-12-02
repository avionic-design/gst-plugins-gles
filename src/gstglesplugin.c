/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2011 Julian Scheel <julian@jusst.de>
 * Copyright (C) 2011 Soeren Grunewald <soeren.grunewald@avionic-design.de>
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
#include <gst/interfaces/xoverlay.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <X11/Xatom.h>

#include <unistd.h>

#include "gstglesplugin.h"
#include "shader.h"

GST_DEBUG_CATEGORY (gst_gles_plugin_debug);


typedef enum _GstGLESPluginProperties  GstGLESPluginProperties;

enum _GstGLESPluginProperties
{
  PROP_0,
  PROP_SILENT
};

GST_BOILERPLATE_WITH_INTERFACE (GstGLESPlugin, gst_gles_plugin, GstVideoSink,
    GST_TYPE_VIDEO_SINK, GstXOverlay, GST_TYPE_X_OVERLAY, gst_gles_xoverlay)

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
static GstStateChangeReturn gst_gles_plugin_change_state (GstElement *element,
                                                          GstStateChange transition);
static gint setup_gl_context (GstGLESPlugin *sink);
static gpointer gl_thread_proc (gpointer data);
static gpointer x11_thread_proc (gpointer data);
static void stop_x11_thread (GstGLESPlugin *sink);

#define WxH ", width = (int) [ 16, 4096 ], height = (int) [ 16, 4096 ]"

static GstStaticPadTemplate gles_sink_factory =
        GST_STATIC_PAD_TEMPLATE ("sink",
                                 GST_PAD_SINK,
                                 GST_PAD_ALWAYS,
                                 GST_STATIC_CAPS ( GST_VIDEO_CAPS_YUV("I420")
                                                   WxH) );

/* OpenGL ES 2.0 implementation */
static GLuint
gl_create_texture(GLuint tex_filter)
{
    GLuint tex_id = 0;

    glGenTextures (1, &tex_id);
    glBindTexture (GL_TEXTURE_2D, tex_id);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, tex_filter);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, tex_filter);

    return tex_id;
}

static void
gl_gen_framebuffer(GstGLESPlugin *sink)
{
    GstGLESContext *gles = &sink->gl_thread.gles;
    glGenFramebuffers (1, &gles->framebuffer);

    gles->rgb_tex.id = gl_create_texture(GL_LINEAR);
    if (!gles->rgb_tex.id)
        GST_ERROR_OBJECT (sink, "Could not create RGB texture");

    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, GST_VIDEO_SINK_WIDTH (sink),
                  GST_VIDEO_SINK_HEIGHT (sink), 0, GL_RGB,
                  GL_UNSIGNED_BYTE, NULL);

    glBindFramebuffer (GL_FRAMEBUFFER, gles->framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, gles->rgb_tex.id, 0);
}

static void
gl_init_textures (GstGLESPlugin *sink)
{
    sink->gl_thread.gles.y_tex.id = gl_create_texture(GL_NEAREST);
    sink->gl_thread.gles.u_tex.id = gl_create_texture(GL_NEAREST);
    sink->gl_thread.gles.v_tex.id = gl_create_texture(GL_NEAREST);
}

static void
gl_load_texture (GstGLESPlugin *sink, GstBuffer *buf)
{
    GstGLESContext *gles = &sink->gl_thread.gles;
    /* y component */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, gles->y_tex.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, GST_VIDEO_SINK_WIDTH (sink),
                 GST_VIDEO_SINK_HEIGHT (sink), 0, GL_LUMINANCE,
                 GL_UNSIGNED_BYTE, GST_BUFFER_DATA (buf));
    glUniform1i (gles->y_tex.loc, 0);

    /* u component */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture (GL_TEXTURE_2D, gles->u_tex.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                 GST_VIDEO_SINK_WIDTH (sink)/2,
                 GST_VIDEO_SINK_HEIGHT (sink)/2, 0, GL_LUMINANCE,
                 GL_UNSIGNED_BYTE, GST_BUFFER_DATA (buf) +
                 GST_VIDEO_SINK_WIDTH (sink) * GST_VIDEO_SINK_HEIGHT (sink));
    glUniform1i (gles->u_tex.loc, 1);

    /* v component */
    glActiveTexture(GL_TEXTURE2);
    glBindTexture (GL_TEXTURE_2D, gles->v_tex.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                 GST_VIDEO_SINK_WIDTH (sink)/2,
                 GST_VIDEO_SINK_HEIGHT (sink)/2, 0, GL_LUMINANCE,
                 GL_UNSIGNED_BYTE, GST_BUFFER_DATA (buf) +
                 GST_VIDEO_SINK_WIDTH (sink) * GST_VIDEO_SINK_HEIGHT (sink) +
                 GST_VIDEO_SINK_WIDTH (sink)/2 *
                 GST_VIDEO_SINK_HEIGHT (sink)/2);
    glUniform1i (gles->v_tex.loc, 2);
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
    GstGLESContext *gles = &sink->gl_thread.gles;

    glBindFramebuffer (GL_FRAMEBUFFER, gles->framebuffer);
    glUseProgram (gles->deinterlace.program);

    glViewport(0, 0, GST_VIDEO_SINK_WIDTH (sink),
               GST_VIDEO_SINK_HEIGHT (sink));

    glClear (GL_COLOR_BUFFER_BIT);

    glVertexAttribPointer (gles->deinterlace.position_loc, 2,
                           GL_FLOAT, GL_FALSE, 4 * sizeof (GLfloat),
                           vVertices);

    glVertexAttribPointer (gles->deinterlace.texcoord_loc, 2,
                           GL_FLOAT, GL_FALSE, 4 * sizeof (GLfloat),
                           &vVertices[2]);

    glEnableVertexAttribArray (gles->deinterlace.position_loc);
    glEnableVertexAttribArray (gles->deinterlace.texcoord_loc);

    gl_load_texture(sink, buf);
    GLint line_height_loc =
            glGetUniformLocation(gles->deinterlace.program,
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

    GstGLESContext *gles = &sink->gl_thread.gles;

    dst.x = 0;
    dst.y = 0;
    dst.w = sink->x11.width;
    dst.h = sink->x11.height;

    src.x = 0;
    src.y = 0;
    src.w = sink->video_width;
    src.h = sink->video_height;

    gst_video_sink_center_rect(src, dst, &result, TRUE);

    glUseProgram (gles->scale.program);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);

    glViewport (result.x, result.y, result.w, result.h);

    glClear (GL_COLOR_BUFFER_BIT);

    glVertexAttribPointer (gles->scale.position_loc, 2, GL_FLOAT,
        GL_FALSE, 4 * sizeof (GLfloat), vVertices);

    glVertexAttribPointer (gles->scale.texcoord_loc, 2, GL_FLOAT,
        GL_FALSE, 4 * sizeof (GLfloat), &vVertices[2]);

    glEnableVertexAttribArray (gles->scale.position_loc);
    glEnableVertexAttribArray (gles->scale.texcoord_loc);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture (GL_TEXTURE_2D, gles->rgb_tex.id);
    glUniform1i (gles->rgb_tex.loc, 3);

    glDrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
    eglSwapBuffers (gles->display, gles->surface);
}

/* EGL implementation */

static void
init_gl_thread (GstGLESPlugin *sink)
{
    GstGLESThread *thread = &sink->gl_thread;
    GError *err = NULL;

    if (!g_thread_get_initialized())
        g_thread_init (NULL);

    thread->handle = g_thread_create(gl_thread_proc,
                                     sink, TRUE, &err);
    if (err || !sink->gl_thread.handle) {
        GST_ERROR_OBJECT(sink, "Could not create gl thread");
    }
}

static void
init_x11_thread (GstGLESPlugin *sink)
{
    GError *err = NULL;

    if (!g_thread_get_initialized())
        g_thread_init (NULL);

    sink->x11.thread = g_thread_create(x11_thread_proc,
                                       sink, TRUE, &err);
    if (err || !sink->x11.thread) {
        GST_ERROR_OBJECT(sink, "Could not create x11 thread");
    }
}

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

    GstGLESContext *gles = &sink->gl_thread.gles;

    GST_DEBUG_OBJECT (sink, "egl get display");
    gles->display = eglGetDisplay((EGLNativeDisplayType)
                                          sink->x11.display);
    if (gles->display == EGL_NO_DISPLAY) {
        GST_ERROR_OBJECT(sink, "Could not get EGL display");
        return -1;
    }

    GST_DEBUG_OBJECT (sink, "egl initialize");
    if (!eglInitialize(gles->display, &major, &minor)) {
        GST_ERROR_OBJECT(sink, "Could not initialize EGL context");
        return -1;
    }
    GST_DEBUG_OBJECT (sink, "Have EGL version: %d.%d", major, minor);

    GST_DEBUG_OBJECT (sink, "choose config");
    if (!eglChooseConfig(gles->display, configAttribs, &config, 1,
                        &num_configs)) {
        GST_ERROR_OBJECT(sink, "Could not choose EGL config");
        return -1;
    }

    if (num_configs != 1) {
        GST_WARNING_OBJECT(sink, "Did not get exactly one config, but %d",
                           num_configs);
    }

    GST_DEBUG_OBJECT (sink, "create window surface");
    gles->surface = eglCreateWindowSurface(gles->display, config,
                                     sink->x11.window, NULL);
    if (gles->surface == EGL_NO_SURFACE) {
        GST_ERROR_OBJECT (sink, "Could not create EGL surface");
        return -1;
    }

    GST_DEBUG_OBJECT (sink, "egl create context");
    gles->context = eglCreateContext(gles->display, config,
                                     EGL_NO_CONTEXT, contextAttribs);
    if (gles->context == EGL_NO_CONTEXT) {
        GST_ERROR_OBJECT(sink, "Could not create EGL context");
        return -1;
    }

    GST_DEBUG_OBJECT (sink, "egl make context current");
    if (!eglMakeCurrent(gles->display, gles->surface,
                        gles->surface, gles->context)) {
        GST_ERROR_OBJECT(sink, "Could not set EGL context to current");
        return -1;
    }

    GST_DEBUG_OBJECT (sink, "egl init done");
    gles->initialized = TRUE;

    return 0;
}

static void
egl_close(GstGLESContext *context)
{
    const GLuint textures[] = {
        context->y_tex.id,
        context->u_tex.id,
        context->v_tex.id,
        context->rgb_tex.id
    };

    if (context->initialized) {
        glDeleteTextures (G_N_ELEMENTS(textures), textures);
        gl_delete_shader (&context->scale);
        gl_delete_shader (&context->deinterlace);
    }

    if (context->surface) {
        eglDestroySurface (context->display, context->surface) ;
        context->surface = NULL;
    }

    if (context->context) {
        eglDestroyContext (context->display, context->context);
        context->context = NULL;
    }

    if (context->display) {
        eglTerminate (context->display);
        context->display = NULL;
    }

    context->initialized = FALSE;
}

static gint
x11_init (GstGLESPlugin *sink, gint width, gint height)
{
    Window root;
    XSetWindowAttributes swa;
    XWMHints hints;

    sink->x11.display = XOpenDisplay (NULL);
    if(!sink->x11.display) {
        GST_ERROR_OBJECT(sink, "Could not create X display");
        return -1;
    }

    XLockDisplay (sink->x11.display);
    root = DefaultRootWindow (sink->x11.display);
    swa.event_mask =
            StructureNotifyMask | ExposureMask | VisibilityChangeMask;

    sink->x11.window = XCreateWindow (
                sink->x11.display, root,
                0, 0, width, height, 0,
                CopyFromParent, InputOutput,
                CopyFromParent, CWEventMask,
                &swa);

    XSetWindowBackgroundPixmap (sink->x11.display, sink->x11.window,
                                None);

    hints.input = True;
    hints.flags = InputHint;
    XSetWMHints(sink->x11.display, sink->x11.window, &hints);

    XMapWindow (sink->x11.display, sink->x11.window);
    XStoreName (sink->x11.display, sink->x11.window, "GLESSink");
    XUnlockDisplay (sink->x11.display);

    return 0;
}

static void
x11_close (GstGLESPlugin *sink)
{
    if (sink->x11.display) {
        XLockDisplay (sink->x11.display);
        XDestroyWindow(sink->x11.display, sink->x11.window);
        XUnlockDisplay (sink->x11.display);
        XCloseDisplay(sink->x11.display);
        sink->x11.display = NULL;
    }
}

/* x11 thread mian function */
static gpointer
x11_thread_proc (gpointer data)
{
    GstGLESPlugin *sink = GST_GLES_PLUGIN (data);
    GstGLESWindow *x11 = &sink->x11;

    x11->running = TRUE;
    while (x11->running && sink->x11.display) {
        /* check for events from the x-server */
        XLockDisplay (sink->x11.display);
        while (XPending (sink->x11.display)) {
            XEvent  xev;
            XNextEvent(sink->x11.display, &xev);

            switch (xev.type) {
            case ConfigureRequest:
            case ConfigureNotify:
                GST_DEBUG_OBJECT(sink, "XConfigure* Event: wxh: %dx%d",
                                 xev.xconfigure.width,
                                 xev.xconfigure.height);
                sink->x11.width = xev.xconfigure.width;
                sink->x11.height = xev.xconfigure.height;
                break;
            default:
                break;
            }
        }
        XUnlockDisplay (sink->x11.display);
        usleep (100000);
    }

    return 0;
}

/* gl thread main function */
static gpointer
gl_thread_proc (gpointer data)
{
    GstGLESPlugin *sink = GST_GLES_PLUGIN (data);
    GstGLESThread *thread = &sink->gl_thread;
    GstGLESContext *gles = &thread->gles;

    thread->running = setup_gl_context (sink) == 0;
    init_x11_thread (sink);

    g_mutex_lock (thread->data_lock);
    while (thread->running) {
        /* wait till gst_gles_plugin_render has some data for us */
        g_cond_wait (thread->data_signal, thread->data_lock);
        if (thread->buf) {
            XLockDisplay (sink->x11.display);
            gl_draw_fbo (sink, thread->buf);
            gl_draw_onscreen (sink);
            XUnlockDisplay (sink->x11.display);
        }

        /* signal gst_gles_plugin_render that we are done */
        g_mutex_lock (thread->render_lock);
        g_cond_signal (thread->render_signal);
        g_mutex_unlock (thread->render_lock);
    }
    g_mutex_unlock (thread->data_lock);

    stop_x11_thread (sink);
    egl_close(gles);
    x11_close(sink);
    return 0;
}

static gint
setup_gl_context (GstGLESPlugin *sink)
{
    GstGLESContext *gles = &sink->gl_thread.gles;
    gint ret;

    sink->x11.width = 720;
    sink->x11.height = 576;
    if (x11_init (sink, sink->x11.width, sink->x11.height) < 0) {
        GST_ERROR_OBJECT (sink, "X11 init failed, abort");
        return -ENOMEM;
    }

    if (egl_init (sink) < 0) {
        GST_ERROR_OBJECT (sink, "EGL init failed, abort");
        x11_close (sink);
        return -ENOMEM;
    }

    ret = gl_init_shader (GST_ELEMENT (sink), &gles->deinterlace,
                          SHADER_DEINT_LINEAR);
    if (ret < 0) {
        GST_ERROR_OBJECT (sink, "Could not initialize shader: %d", ret);
        egl_close (&sink->gl_thread.gles);
        x11_close (sink);
        return -ENOMEM;
    }
    gles->y_tex.loc = glGetUniformLocation(gles->deinterlace.program,
                                           "s_ytex");
    gles->u_tex.loc = glGetUniformLocation(gles->deinterlace.program,
                                           "s_utex");
    gles->v_tex.loc = glGetUniformLocation(gles->deinterlace.program,
                                           "s_vtex");

    ret = gl_init_shader (GST_ELEMENT (sink), &gles->scale, SHADER_COPY);
    if (ret < 0) {
        GST_ERROR_OBJECT (sink, "Could not initialize shader: %d", ret);
        egl_close (gles);
        x11_close (sink);
        return -ENOMEM;
    }
    gles->rgb_tex.loc = glGetUniformLocation(gles->scale.program, "s_tex");
    gl_init_textures (sink);

    /* generate the framebuffer object */
    gl_gen_framebuffer (sink);
    gles->initialized = TRUE;

    /* finally announce the window handle to controling app */
    gst_x_overlay_got_window_handle (GST_X_OVERLAY (sink), sink->x11.window);
    return 0;
}

static void
stop_gl_thread (GstGLESPlugin *sink)
{
    if (sink->gl_thread.running) {
        sink->gl_thread.running = FALSE;
        sink->gl_thread.buf = NULL;

        g_mutex_lock (sink->gl_thread.data_lock);
        g_cond_signal (sink->gl_thread.data_signal);
        g_mutex_unlock (sink->gl_thread.data_lock);

        g_thread_join(sink->gl_thread.handle);
    }
}

static void
stop_x11_thread (GstGLESPlugin *sink)
{
    if (sink->x11.running) {
        sink->x11.running = FALSE;
        g_thread_join(sink->x11.thread);
    }
}

/* GObject vmethod implementations */

static void
gst_gles_plugin_base_init (gpointer gclass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (gclass);

  element_class->change_state = gst_gles_plugin_change_state;

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
    GstGLESThread *thread = &sink->gl_thread;
    Status ret;

    sink->silent = FALSE;
    sink->gl_thread.gles.initialized = FALSE;

    thread->data_lock = g_mutex_new();
    thread->render_lock = g_mutex_new();
    thread->data_signal = g_cond_new();
    thread->render_signal = g_cond_new();

    ret = XInitThreads();
    if (ret == 0) {
        GST_ERROR_OBJECT(sink, "XInitThreads failed");
    }

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
    return TRUE;
}

/* deinitialisation code */
static gboolean
gst_gles_plugin_stop (GstBaseSink *basesink)
{
    GstGLESPlugin *sink = GST_GLES_PLUGIN (basesink);

    stop_gl_thread (sink);

    GST_VIDEO_SINK_WIDTH (sink) = 0;
    GST_VIDEO_SINK_HEIGHT (sink)  = 0;

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

  gst_video_calculate_display_ratio ((guint*)&sink->par_n,
                                     (guint*)&sink->par_d,
                                     GST_VIDEO_SINK_WIDTH (sink),
                                     GST_VIDEO_SINK_HEIGHT (sink),
                                     (guint) par_n, (guint) par_d,
                                     display_par_n, display_par_d);

  sink->video_width = sink->video_width * par_n / par_d;

  return TRUE;
}

static GstStateChangeReturn
gst_gles_plugin_change_state (GstElement *element, GstStateChange transition)
{
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
    ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
    return ret;
}

static GstFlowReturn
gst_gles_plugin_preroll (GstBaseSink * basesink, GstBuffer * buf)
{
    GstGLESPlugin *sink = GST_GLES_PLUGIN (basesink);
    if (!sink->gl_thread.running) {
        init_gl_thread(sink);
    }
    return GST_FLOW_OK;
}

static GstFlowReturn
gst_gles_plugin_render (GstBaseSink *basesink, GstBuffer *buf)
{
    GstGLESPlugin *sink = GST_GLES_PLUGIN (basesink);
    GstGLESThread *thread = &sink->gl_thread;
    GTimeVal timeout;
    gboolean ret = TRUE;

    g_mutex_lock (thread->data_lock);
    thread->buf = buf;
    g_cond_signal (thread->data_signal);
    g_mutex_unlock (thread->data_lock);

    g_mutex_lock (thread->render_lock);

#if 1
    g_cond_wait (thread->render_signal, thread->render_lock);
#else
    g_get_current_time (&timeout);
    g_time_val_add (&timeout, 500);
    /* FIXME: timed_wait always fails. */
    ret = g_cond_timed_wait (thread->render_signal, thread->render_lock,
                             &timeout);
    GST_DEBUG_OBJECT (basesink, "Render %s", ret ? "done" : "with timeout");
#endif
    g_mutex_unlock (thread->render_lock);

    return ret ? GST_FLOW_OK : GST_FLOW_ERROR;
}

static void
gst_gles_plugin_finalize (GObject *gobject)
{
    GstGLESPlugin *plugin = (GstGLESPlugin *)gobject;
    GstGLESThread *thread = &plugin->gl_thread;

    stop_gl_thread (plugin);

    if (thread->data_lock) {
        g_mutex_free(thread->data_lock);
        thread->data_lock = NULL;
    }
    if (thread->render_lock) {
        g_mutex_free(thread->render_lock);
        thread->render_lock = NULL;
    }
    if (thread->data_signal) {
        g_cond_free(thread->data_signal);
        thread->data_signal = NULL;
    }
    if (thread->render_signal) {
        g_cond_free(thread->render_signal);
        thread->render_signal = NULL;
    }
}

/* GstXOverlay Interface implementation */
static void
gst_gles_xoverlay_set_window_handle (GstXOverlay *overlay, guintptr handle)
{
    GstGLESPlugin *sink = GST_GLES_PLUGIN (overlay);

    // destroy egl surface and x11 window
    GST_ERROR_OBJECT (sink, "Setting window handle is not yet supported.");
}

static void
gst_gles_xoverlay_interface_init (GstXOverlayClass *overlay_klass)
{
    overlay_klass->set_window_handle = gst_gles_xoverlay_set_window_handle;
}

static gboolean
gst_gles_xoverlay_supported (GstGLESPlugin *sink,
                             GType iface_type)
{
    GST_DEBUG_OBJECT(sink, "Interface XOverlay supprted");
    g_return_val_if_fail (iface_type == GST_TYPE_X_OVERLAY, FALSE);

    return TRUE;
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
