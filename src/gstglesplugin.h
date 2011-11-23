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

#ifndef _GST_GLES_PLUGIN_H__
#define _GST_GLES_PLUGIN_H__

#include <GLES2/gl2.h>
#include <EGL/egl.h>

#include <X11/Xlib.h>

#include <gst/gst.h>
#include <gst/video/gstvideosink.h>

GST_DEBUG_CATEGORY_EXTERN (gst_gles_plugin_debug);
#define GST_CAT_DEFAULT gst_gles_plugin_debug

G_BEGIN_DECLS

/* #defines don't like whitespacey bits */
#define GST_TYPE_GLES_PLUGIN \
  (gst_gles_plugin_get_type())
#define GST_GLES_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_GLES_PLUGIN,GstGLESPlugin))
#define GST_GLES_PLUGIN_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_GLES_PLUGIN,GstGLESPluginClass))
#define GST_IS_GLES_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_GLES_PLUGIN))
#define GST_IS_GLES_PLUGIN_TEMPLATE(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_GLES_PLUGIN))

typedef struct _GstGLESPlugin      GstGLESPlugin;
typedef struct _GstGLESPluginClass GstGLESPluginClass;

struct _GstGLESPlugin
{
  GstVideoSink basesink;

  gint par_n;
  gint par_d;

  gint video_width;
  gint video_height;

  /* x11 context */
  Display *x_display;
  Window x_window;

  /* gl context */
  gint program;
  GLuint vertex_shader;
  GLuint fragment_shader;

  EGLDisplay display;
  EGLSurface surface;
  EGLContext context;

  GLuint y_texture;
  GLuint u_texture;
  GLuint v_texture;

  gboolean initialized;

  GLint position_loc;
  GLint texcoord_loc;

  GLint y_loc;
  GLint u_loc;
  GLint v_loc;

  /* properties */
  gboolean silent;
};

struct _GstGLESPluginClass
{
  GstVideoSinkClass basesinkclass;
};

GType gst_gles_plugin_get_type (void);

G_END_DECLS

#endif /* _GST_GLES_PLUGIN_H__ */
