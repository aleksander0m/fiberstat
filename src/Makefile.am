
bin_PROGRAMS = fiberstat

fiberstat_SOURCES = \
	fiberstat.c \
	$(NULL)

fiberstat_CPPFLAGS = \
	-I$(top_srcdir) \
	-I$(top_builddir) \
	-I$(top_srcdir)/src \
	-I$(top_builddir)/src \
	$(NCURSES_CFLAGS) \
	$(NULL)

fiberstat_LDFLAGS = \
	$(NCURSES_LIBS) \
	-lpthread \
	$(NULL)
