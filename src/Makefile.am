
################################################################################

noinst_LTLIBRARIES = libnatsort.la

libnatsort_la_SOURCES = \
	natsort/strnatcmp.h \
	natsort/strnatcmp.c \
	$(NULL)

################################################################################

bin_PROGRAMS = fiberstat

fiberstat_SOURCES = \
	fiberstat.c \
	$(NULL)

fiberstat_CPPFLAGS = \
	-I$(top_srcdir) \
	-I$(top_builddir) \
	-I$(top_srcdir)/src \
	-I$(top_builddir)/src \
	-I$(top_srcdir)/src/natsort \
	$(NCURSES_CFLAGS) \
	$(NULL)

fiberstat_LDADD = \
	$(builddir)/libnatsort.la \
	$(NULL)

fiberstat_LDFLAGS = \
	$(NCURSES_LIBS) \
	-lpthread -lm \
	$(NULL)
