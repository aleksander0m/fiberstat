
bin_PROGRAMS = fiberstat

fiberstat_SOURCES = \
	fiberstat.c \
	$(NULL)

fiberstat_CPPFLAGS = \
	-I$(top_srcdir) \
	-I$(top_builddir) \
	-I$(top_srcdir)/src \
	-I$(top_builddir)/src \
	$(NCURSESW_CFLAGS) \
	$(NULL)

fiberstat_LDFLAGS = \
	$(NCURSESW_LIBS) \
	-lpthread \
	$(NULL)
