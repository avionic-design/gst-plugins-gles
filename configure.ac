dnl required version of autoconf
AC_PREREQ([2.53])

dnl TODO: fill in your package name and package version here
AC_INIT([gst-plugins-gles],[0.10.3],[bugs@avionic-design.de])

AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([src])
AC_CONFIG_HEADERS([config.h])

dnl required version of automake
AM_INIT_AUTOMAKE([1.10])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

dnl enable mainainer mode by default
AM_MAINTAINER_MODE([enable])

dnl check for tools (compiler etc.)
AC_PROG_CC

dnl required version of libtool
LT_PREREQ([2.2.6])
LT_INIT

dnl give error and exit if we don't have pkgconfig
AC_CHECK_PROG(HAVE_PKGCONFIG, pkg-config, [ ], [
  AC_MSG_ERROR([You need to have pkg-config installed!])
])

dnl Check for the required version of GStreamer core (and gst-plugins-base)
dnl This will export GST_CFLAGS and GST_LIBS variables for use in Makefile.am
AC_MSG_CHECKING([the GStreamer version to use])
AC_ARG_WITH([gstreamer],
	[AS_HELP_STRING([--with-gstreamer=0.10|1.0], [the GStreamer version to use (default: 0.10)])],
	[case "$with_gstreamer"
		in 0.10|1.0) ;;
		*) AC_MSG_ERROR([invalid GStreamer version specified]) ;;
	esac], [with_gstreamer=0.10])
AC_MSG_RESULT([$with_gstreamer])

GSTREAMER_0_10_REQUIRED_VERSION=0.10
GSTREAMER_0_10_PLUGINS_BASE_REQUIRED_VERSION=0.10.30
GSTREAMER_1_0_REQUIRED_VERSION=1.0
GSTREAMER_1_0_PLUGINS_BASE_REQUIRED_VERSION=1.0

case "$with_gstreamer" in
	0.10)	GSTREAMER_REQUIRED_VERSION=$GSTREAMER_0_10_REQUIRED_VERSION
		GSTREAMER_PLUGINS_BASE_REQUIRED_VERSION=$GSTREAMER_0_10_PLUGINS_BASE_REQUIRED_VERSION
		GST_API_VERSION=0.10
		;;
	1.0)	GSTREAMER_REQUIRED_VERSION=$GSTREAMER_1_0_REQUIRED_VERSION
		GSTREAMER_PLUGINS_BASE_REQUIRED_VERSION=$GSTREAMER_1_0_PLUGINS_BASE_REQUIRED_VERSION
		GST_API_VERSION=1.0
		;;
esac

AC_SUBST([GST_API_VERSION])
if test "$GST_API_VERSION" = "1.0"; then
	AC_DEFINE([GST_USE_UNSTABLE_API], [1], [Using unstable GStreamer API])
	AC_DEFINE([GST_API_VERSION_1],[1], [Using GStreamer 1.0])
fi

gstreamer_modules="gstreamer-$GST_API_VERSION
	gstreamer-base-$GST_API_VERSION
	gstreamer-controller-$GST_API_VERSION
	gstreamer-video-$GST_API_VERSION ";

if test "$GST_API_VERSION" = "0.10"; then
  gstreamer_modules+="gstreamer-interfaces-$GST_API_VERSION "
fi

PKG_CHECK_MODULES(GST, [$gstreamer_modules],
  [AC_SUBST(GST_CFLAGS) AC_SUBST(GST_LIBS)],
  [AC_MSG_ERROR([
      You need to install or upgrade the GStreamer development
      packages on your system. On debian-based systems these are
      libgstreamer$GST_API_VERSION-dev and libgstreamer-plugins-base$GST_API_VERSION-dev.
      on RPM-based systems gstreamer$GST_API_VERSION-devel, libgstreamer$GST_API_VERSION-devel
      or similar. The minimum version required is $GST_REQUIRED.
  ])
])

PKG_CHECK_MODULES(GLES, [glesv2 egl],
  [AC_SUBST(GLES_CFLAGS) AC_SUBST(GLES_LIBS)],
  [AC_MSG_ERROR([
      You need to install or upgrade the OpenGLES development
      packages on your system. On debian-based systems these are
      most likely mesa-dev. On RPM-based systems mesa-devel or
      similar.
  ])
])

PKG_CHECK_MODULES(GIO, [gio-2.0],
  [AC_SUBST(GIO_CFLAGS) AC_SUBST(GIO_LIBS)],
  [AC_MSG_ERROR([
      You need to install or upgrade the glib development
      packages on your system. On debian-based systems these are
      most likely glib-dev. On RPM-based systems glib-devel or
      similar.
  ])
])

dnl check if compiler understands -Wall (if yes, add -Wall to GST_CFLAGS)
AC_MSG_CHECKING([to see if compiler understands -Wall])
save_CFLAGS="$CFLAGS"
CFLAGS="$CFLAGS -Wall"
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([ ], [ ])], [
  GST_CFLAGS="$GST_CFLAGS -Wall"
  AC_MSG_RESULT([yes])
], [
  AC_MSG_RESULT([no])
])

dnl set the plugindir where plugins should be installed (for src/Makefile.am)
if test "x${prefix}" = "x$HOME"; then
  plugindir="$HOME/.gstreamer-$GST_API_VERSION/plugins"
else
  plugindir="\$(libdir)/gstreamer-$GST_API_VERSION"
fi
AC_SUBST(plugindir)

dnl set proper LDFLAGS for plugins
GST_PLUGIN_LDFLAGS='-module -avoid-version -export-symbols-regex [_]*\(gst_\|Gst\|GST_\).*'
AC_SUBST(GST_PLUGIN_LDFLAGS)

AC_CONFIG_FILES([Makefile src/Makefile data/Makefile])
AC_OUTPUT

