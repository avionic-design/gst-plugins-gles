# Note: plugindir is set in configure

##############################################################################
# TODO: change libgstglesplugin.la to something else, e.g. libmysomething.la     #
##############################################################################
plugin_LTLIBRARIES = libgstglesplugin.la

##############################################################################
# TODO: for the next set of variables, name the prefix if you named the .la, #
#  e.g. libmysomething.la => libmysomething_la_SOURCES                       #
#                            libmysomething_la_CFLAGS                        #
#                            libmysomething_la_LIBADD                        #
#                            libmysomething_la_LDFLAGS                       #
##############################################################################

# sources used to compile this plug-in
libgstglesplugin_la_SOURCES = \
    shader.c shader.h \
    gstglessink.c gstglessink.h

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstglesplugin_la_CFLAGS = $(GST_CFLAGS) $(GLES_CFLAGS) $(GIO_CFLAGS)
libgstglesplugin_la_LIBADD = $(GST_LIBS) $(GLES_LIBS) $(GIO_LIBS)
libgstglesplugin_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstglesplugin_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS = gstglessink.h shader.h
