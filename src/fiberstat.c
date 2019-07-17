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

#include <ncurses.h>

/******************************************************************************/

#define PROGRAM_NAME    "fiberstat"
#define PROGRAM_VERSION PACKAGE_VERSION

/******************************************************************************/
/* Context */

static void
print_help (void)
{
    printf ("\n"
            "Usage: " PROGRAM_NAME " <option>\n"
            "\n"
            "Common options:\n"
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

        iarg = getopt_long (argc, argv, "hv", longopts, &idx);

        if (iarg < 0)
            break;

        switch (iarg) {
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
    if (setup_curses () < 0) {
        fprintf (stderr, "error: couldn't setup curses\n");
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

    teardown_curses ();
    return 0;
}
