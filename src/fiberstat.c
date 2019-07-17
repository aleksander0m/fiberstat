/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * Copyright (C) 2019 Zodiac Inflight Innovations
 * Copyright (C) 2019 Aleksander Morgado <aleksander@aleksander.es>
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <malloc.h>
#include <assert.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <getopt.h>
#include <net/if.h>

#include <ncurses.h>

/******************************************************************************/

#define PROGRAM_NAME    "fiberstat"
#define PROGRAM_VERSION PACKAGE_VERSION

/******************************************************************************/
/* Debug logging */

#define DEBUG_LOG "/tmp/fiberstat.log"

static FILE *logfile;
static bool  debug;

static void
setup_log (void)
{
    if (!debug)
        return;
    logfile = fopen (DEBUG_LOG, "w+");
}

static void
teardown_log (void)
{
    if (logfile)
        fclose (logfile);
}

static void
log_message (const char *level,
             const char *fmt,
             ...)
{
    char    *message;
    va_list  args;

    if (!logfile)
        return;

    va_start (args, fmt);
    if (vasprintf (&message, fmt, args) == -1)
        return;
    va_end (args);

    fprintf (logfile, "%s %s\n", level, message);
    fflush (logfile);
    free (message);
}

#define log_error(...)   log_message ("[error]", ## __VA_ARGS__ )
#define log_warning(...) log_message ("[warn ]", ## __VA_ARGS__ )
#define log_info(...)    log_message ("[info ]", ## __VA_ARGS__ )
#define log_debug(...)   log_message ("[debug]", ## __VA_ARGS__ )

/******************************************************************************/
/* Context */

static void
print_help (void)
{
    printf ("\n"
            "Usage: " PROGRAM_NAME " <option>\n"
            "\n"
            "Common options:\n"
            "  -d, --debug      Verbose output in " DEBUG_LOG ".\n"
            "  -h, --help       Show help.\n"
            "  -v, --version    Show version.\n"
            "\n");
}

static void
print_version (void)
{
    printf ("\n"
            PROGRAM_NAME " " PROGRAM_VERSION "\n"
            "Copyright (C) 2019 Zodiac Inflight Innovations\n"
            "\n");
}

static const struct option longopts[] = {
    { "debug",   no_argument,       0, 'd' },
    { "version", no_argument,       0, 'v' },
    { "help",    no_argument,       0, 'h' },
    { 0,         0,                 0, 0   },
};

static void
setup_context (int argc, char *const *argv)
{
    /* turn off getopt error message */
    opterr = 1;
    while (1) {
        int idx = 0;
        int iarg = 0;

        iarg = getopt_long (argc, argv, "dhv", longopts, &idx);

        if (iarg < 0)
            break;

        switch (iarg) {
        case 'd':
            debug = true;
            break;
        case 'h':
            print_help ();
            exit (0);
        case 'v':
            print_version ();
            exit (0);
        }
    }
}

/******************************************************************************/
/* Application context */

typedef struct _InterfaceInfo InterfaceInfo;

typedef struct {
    bool    stop;
    bool    resize;
    bool    refresh_title;
    bool    refresh_contents;
    bool    refresh_log;
    int     max_y;
    int     max_x;
    WINDOW *header_win;
    WINDOW *content_win;
    InterfaceInfo **ifaces;
    unsigned int    n_ifaces;
} Context;

static Context context = {
    .resize = true,
};

static void
request_terminate (int signum)
{
    context.stop = true;
}

static void
request_resize (int signum)
{
    context.resize = true;
}

/******************************************************************************/
/* Curses management */

static int
setup_curses (void)
{
    struct sigaction actterm;
    struct sigaction actwinch;

    sigemptyset(&actterm.sa_mask);
    actterm.sa_flags = 0;
    actterm.sa_handler = request_terminate;
    if (sigaction (SIGTERM, &actterm, NULL) < 0) {
        fprintf (stderr, "error: unable to register SIGTERM\n");
        return -1;
    }

    sigemptyset (&actwinch.sa_mask);
    actwinch.sa_flags = 0;
    actwinch.sa_handler = request_resize;
    if (sigaction (SIGWINCH, &actwinch, NULL) < 0) {
        fprintf (stderr, "error: unable to register SIGWINCH\n");
        return -1;
    }

    initscr ();
    keypad(stdscr, TRUE);
    nodelay (stdscr, TRUE);
    noecho ();
    cbreak ();
    curs_set (0);

    return 0;
}

static void
teardown_curses (void)
{
    endwin();
}

/******************************************************************************/
/* Window management */

typedef enum {
    COLOR_PAIR_MAIN = 1,
    COLOR_PAIR_TITLE_TEXT,
    COLOR_PAIR_SHORTCUT_TEXT,
} ColorPair;

static void
setup_windows (void)
{
    static int once;

    if (!once) {
        once = 1;
        start_color ();
        curs_set (0);
        init_pair (COLOR_PAIR_MAIN, COLOR_WHITE, COLOR_BLACK);
        init_pair (COLOR_PAIR_TITLE_TEXT, COLOR_GREEN, COLOR_BLACK);
        init_pair (COLOR_PAIR_SHORTCUT_TEXT, COLOR_CYAN, COLOR_BLACK);
        bkgd (COLOR_PAIR (COLOR_PAIR_MAIN));
    }

    endwin ();
    refresh ();
    getmaxyx (stdscr, context.max_y, context.max_x);

    /* header window */
    if (context.header_win)
        delwin (context.header_win);
    context.header_win = newwin (1, context.max_x, 0, 0);
    wbkgd (context.header_win, COLOR_PAIR (COLOR_PAIR_MAIN));

    /* content window */
    if (context.content_win)
        delwin (context.content_win);
    context.content_win = newwin (context.max_y - 1, context.max_x, 1, 0);
    wbkgd (context.content_win, COLOR_PAIR (COLOR_PAIR_MAIN));

    context.refresh_title    = true;
    context.refresh_contents = true;
}

/******************************************************************************/
/* List of interfaces */

typedef struct _InterfaceInfo {
    char *name;
} InterfaceInfo;

static void
interface_info_free (InterfaceInfo *iface)
{
    free (iface->name);
    free (iface);
}

static void
teardown_interfaces (void)
{
    unsigned int i;

    for (i = 0; i < context.n_ifaces; i++)
        interface_info_free (context.ifaces[i]);
    free (context.ifaces);
}

static int
setup_interfaces (void)
{
    struct if_nameindex *if_nidxs;
    struct if_nameindex *intf;

    if_nidxs = if_nameindex ();
    if (!if_nidxs)
        return -1;

    for (intf = if_nidxs; intf->if_index || intf->if_name; intf++) {
        InterfaceInfo *iface;

        iface = calloc (1, sizeof (InterfaceInfo));
        if (iface)
            iface->name = strdup (intf->if_name);
        if (!iface || !iface->name)
            return -2;

        log_info ("tracking interface '%s'...", iface->name);

        context.n_ifaces++;
        context.ifaces = realloc (context.ifaces, sizeof (InterfaceInfo *) * context.n_ifaces);
        if (!context.ifaces)
            return -3;
        context.ifaces[context.n_ifaces - 1] = iface;
    }
    if_freenameindex(if_nidxs);

    return 0;
}

/******************************************************************************/
/* Core application logic */

static void
refresh_title (void)
{
    char title[64];

    werase (context.header_win);

    snprintf (title, sizeof (title), "%s %s", PROGRAM_NAME, PACKAGE_VERSION);
    wattron(context.header_win, A_BOLD | A_UNDERLINE | COLOR_PAIR (COLOR_PAIR_TITLE_TEXT));
    mvwprintw (context.header_win, 0, (context.max_x / 2) - (strlen (title) / 2), "%s", title);
    wattroff(context.header_win, A_BOLD | A_UNDERLINE | COLOR_PAIR (COLOR_PAIR_TITLE_TEXT));

    wrefresh (context.header_win);
}

static void
refresh_contents (void)
{
    werase (context.content_win);

    /* TODO */

    wrefresh (context.content_win);
}

/******************************************************************************/
/* Main */

int main (int argc, char *const *argv)
{
    setup_context (argc, argv);
    setup_log ();

    log_info ("-----------------------------------------------------------");
    log_info ("starting program " PROGRAM_NAME " (v" PROGRAM_VERSION ")...");

    if (setup_curses () < 0) {
        fprintf (stderr, "error: couldn't setup curses\n");
        return -1;
    }

    if (setup_interfaces () < 0) {
        fprintf (stderr, "error: couldn't setup interfaces\n");
        return -1;
    }

    do {
        if (context.resize) {
            setup_windows ();
            context.resize = false;
        }

        if (context.refresh_title) {
            refresh_title ();
            context.refresh_title = false;
        }

        if (context.refresh_contents) {
            refresh_contents ();
            context.refresh_contents = false;
        }

        sleep (1);
    } while (!context.stop);

    teardown_interfaces ();
    teardown_curses ();
    teardown_log();
    return 0;
}
