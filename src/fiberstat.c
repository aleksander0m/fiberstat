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
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <getopt.h>
#include <net/if.h>
#include <wchar.h>
#include <locale.h>
#include <math.h>

#include <ncurses.h>

/******************************************************************************/

#define PROGRAM_NAME    "fiberstat"
#define PROGRAM_VERSION PACKAGE_VERSION

/******************************************************************************/

/* Define to test bar fill levels */
#undef FORCE_TEST_LEVELS

/* Define to test polling fake sysfs files
 * E.g.:
 *   $ mkdir -p /tmp/lo
 *   $ echo 100 > /tmp/lo/power1_input
 *   $ echo 200 > /tmp/lo/power2_input
 */
#undef FORCE_TEST_SYSFS

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

#define DEFAULT_TIMEOUT_MS 1000
static int timeout_ms = -1;

static void
print_help (void)
{
    printf ("\n"
            "Usage: " PROGRAM_NAME " <option>\n"
            "\n"
            "Common options:\n"
            "  -t, --timeout    How often to reload values, in ms.\n"
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
    { "timeout", required_argument, 0, 't' },
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

        iarg = getopt_long (argc, argv, "t:dhv", longopts, &idx);

        if (iarg < 0)
            break;

        switch (iarg) {
        case 't':
            timeout_ms = atoi (optarg);
            if (timeout_ms <= 0) {
                fprintf (stderr, "error: invalid timeout: %s", optarg);
                exit (EXIT_FAILURE);
            }
            break;
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
    COLOR_PAIR_BOX_CONTENT,
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
        init_pair (COLOR_PAIR_BOX_CONTENT, COLOR_BLACK, COLOR_GREEN);
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

/* Expected TX/RX power threshold */
#define POWER_MAX   0.0
#define POWER_MIN -20.0
#define POWER_UNK -40.0

static int
power_to_percentage (float power)
{
    if (power >= POWER_MAX)
        return 100;
    if (power <= POWER_MIN)
        return 0;
    return floor ((100.0 * (power - POWER_MIN) / (POWER_MAX - POWER_MIN)) + 0.5);
}

/******************************************************************************/
/* List of interfaces */

typedef struct _InterfaceInfo {
    char *name;
    char *tx_power_path;
    char *rx_power_path;
    int   tx_power_fd;
    int   rx_power_fd;
    /* fit values in the [-20,0] range */
    float tx_power;
    float rx_power;
} InterfaceInfo;

static void
interface_info_free (InterfaceInfo *iface)
{
    if (!(iface->tx_power_fd < 0))
        close (iface->tx_power_fd);
    if (!(iface->rx_power_fd < 0))
        close (iface->rx_power_fd);
    free (iface->tx_power_path);
    free (iface->rx_power_path);
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

        iface->tx_power = POWER_MIN;
        iface->rx_power = POWER_MIN;
        iface->tx_power_fd = -1;
        iface->rx_power_fd = -1;

#if defined FORCE_TEST_SYSFS
        {
            char aux[256];

            snprintf (aux, sizeof (aux), "/tmp/%s/power1_input", iface->name);
            iface->tx_power_path = strdup (aux);
            snprintf (aux, sizeof (aux), "/tmp/%s/power2_input", iface->name);
            iface->rx_power_path = strdup (aux);
        }
#endif

        if (iface->tx_power_path) {
            iface->tx_power_fd = open (iface->tx_power_path, O_RDONLY);
            if (iface->tx_power_fd < 0)
                log_warning ("couldn't open TX power file for interface '%s' at %s", iface->name, iface->tx_power_path);
        } else
            log_warning ("TX power file for interface '%s' undefined", iface->name);

        if (iface->rx_power_path) {
            iface->rx_power_fd = open (iface->rx_power_path, O_RDONLY);
            if (iface->rx_power_fd < 0)
                log_warning ("couldn't open RX power file for interface '%s' at %s", iface->name, iface->rx_power_path);
        } else
            log_warning ("RX power file for interface '%s' undefined", iface->name);

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

const wchar_t VRT = L'\x2502';
const wchar_t HRZ = L'\x2500';
const wchar_t TL  = L'\x250C';
const wchar_t TR  = L'\x2510';
const wchar_t BL  = L'\x2514';
const wchar_t BR  = L'\x2518';

#define BOX_CONTENT_WIDTH   4
#define BOX_WIDTH           (BOX_CONTENT_WIDTH + 2)
#define BOX_CONTENT_HEIGHT  16
#define BOX_HEIGHT          (BOX_CONTENT_HEIGHT + 4)
#define BOX_SEPARATION      1

static void
print_box (int         x,
           int         y,
           float       power,
           const char *label)
{
    char          buf[32];
    unsigned int  i;
    unsigned int  j;
    float         fill_scaled;
    unsigned int  fill_height;
    char         *fill = " ";
    unsigned int  fill_percent;
    unsigned int  x_center;

    fill_percent = power_to_percentage (power);
    fill_scaled = ((float) fill_percent * BOX_CONTENT_HEIGHT) / 100.0;
    fill_height = floor (fill_scaled + 0.5);
    log_debug ("fill percent: %u, fill height: %u", fill_percent, fill_height);

    /* box */
    mvwaddnwstr (context.content_win, y, x, &TL, 1);
    for (i = 0; i < BOX_CONTENT_WIDTH; i++)
        mvwaddnwstr (context.content_win, y, x+1+i, &HRZ, 1);
    mvwaddnwstr (context.content_win, y, x+1+BOX_CONTENT_WIDTH, &TR, 1);
    for (i = 0; i < BOX_CONTENT_HEIGHT; i++) {
        mvwaddnwstr (context.content_win, y+1+i, x, &VRT, 1);
        if (i >= (BOX_CONTENT_HEIGHT - fill_height)) {
            wattron (context.content_win, COLOR_PAIR (COLOR_PAIR_BOX_CONTENT));
            for (j = 0; j < BOX_CONTENT_WIDTH; j++)
                mvwaddnstr (context.content_win, y+1+i, x+1+j, fill, 1);
            wattroff (context.content_win, COLOR_PAIR (COLOR_PAIR_BOX_CONTENT));
        }
        mvwaddnwstr (context.content_win, y+1+i, x+1+BOX_CONTENT_WIDTH, &VRT, 1);
    }
    mvwaddnwstr (context.content_win, y+1+BOX_CONTENT_HEIGHT, x, &BL, 1);
    for (i = 0; i < BOX_CONTENT_WIDTH; i++)
        mvwaddnwstr (context.content_win, y+1+BOX_CONTENT_HEIGHT, x+1+i, &HRZ, 1);
    mvwaddnwstr (context.content_win, y+1+BOX_CONTENT_HEIGHT, x+1+BOX_CONTENT_WIDTH, &BR, 1);

    x_center = x + (BOX_WIDTH / 2) - (strlen (label) / 2);
    mvwprintw (context.content_win, y+1+BOX_CONTENT_HEIGHT+2, x_center, "%s", label);

    snprintf (buf, sizeof (buf), "%.2f", power);
    x_center = x + (BOX_WIDTH / 2) - (strlen (buf) / 2);
    mvwprintw (context.content_win, y+1+BOX_CONTENT_HEIGHT+1, x_center, "%s", buf);
}

#define INTERFACE_WIDTH  (BOX_WIDTH + BOX_SEPARATION + BOX_WIDTH)
#define INTERFACE_HEIGHT (BOX_HEIGHT + 2)

static void
print_iface_info (int         x,
                  int         y,
                  const char *name,
                  float       tx_power,
                  float       rx_power)
{
    int x_center;

    x_center = x + (INTERFACE_WIDTH / 2) - (strlen (name) / 2);
    mvwprintw (context.content_win, y, x_center, "%s", name);
}

static void
print_interface (InterfaceInfo *iface, int x, int y)
{
    float tx_power;
    float rx_power;

    tx_power = iface->tx_power;
    rx_power = iface->rx_power;

#if defined FORCE_TEST_LEVELS
    {
        static float fill = -20.0;

        tx_power = fill;
        fill += 5.0;
        if (fill > POWER_MAX)
            fill = POWER_MIN;

        rx_power = fill;
        fill += 5.0;
        if (fill > POWER_MAX)
            fill = POWER_MIN;
    }
#endif /* FORCE_TEST_LEVELS */

    /* Power boxes */
    print_box (x, y, tx_power, "TX dBm");
    print_box (x + BOX_WIDTH + BOX_SEPARATION, y, rx_power, "RX dBm");
    print_iface_info (x, y + BOX_HEIGHT, iface->name, tx_power, rx_power);
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

#define MARGIN_HORIZONTAL                5
#define INTERFACE_SEPARATION_HORIZONTAL  3
#define INTERFACE_SEPARATION_VERTICAL    3

static void
refresh_contents (void)
{
    unsigned int i, x, y;
    unsigned int total_width;
    unsigned int x_initial;
    unsigned int n_ifaces_per_row;
    unsigned int content_max_width;

    content_max_width = (context.max_x - (MARGIN_HORIZONTAL * 2));
    log_debug ("window width: %u", context.max_x);
    log_debug ("interface width: %u", INTERFACE_WIDTH);
    log_debug ("content max width: %u", content_max_width);

    werase (context.content_win);

    n_ifaces_per_row = 0;
    while (1) {
        unsigned int next = ((n_ifaces_per_row + 1) * INTERFACE_WIDTH) + ((n_ifaces_per_row) * INTERFACE_SEPARATION_HORIZONTAL);
        if (next >= content_max_width)
            break;
        total_width = next;
        n_ifaces_per_row++;
    }
    log_debug ("number of interfaces per row: %u", n_ifaces_per_row);

    if (context.n_ifaces < n_ifaces_per_row)
        total_width = (context.n_ifaces * INTERFACE_WIDTH) + ((context.n_ifaces - 1) * INTERFACE_SEPARATION_HORIZONTAL);
    else
        total_width = (n_ifaces_per_row * INTERFACE_WIDTH) + ((n_ifaces_per_row - 1) * INTERFACE_SEPARATION_HORIZONTAL);
    log_debug ("total width: %u", total_width);

    x_initial = x = (context.max_x / 2) - (total_width / 2);
    y = 0;

    for (i = 0; i < context.n_ifaces; i++) {
        print_interface (context.ifaces[i], x, y);
        if (((i + 1) % n_ifaces_per_row) == 0) {
            x = x_initial;
            y += (INTERFACE_HEIGHT + INTERFACE_SEPARATION_VERTICAL);
        } else {
            x += (INTERFACE_WIDTH + INTERFACE_SEPARATION_HORIZONTAL);
        }
    }

    wrefresh (context.content_win);
}

/******************************************************************************/

static float
reload_power_from_file (int fd)
{
    float   value;
    char    buffer[255] = { 0 };
    ssize_t n_read;

    lseek (fd, 0, SEEK_SET);
    n_read = read (fd, buffer, sizeof (buffer));
    if (n_read <= 0)
        return POWER_UNK;

    value = strtof (buffer, NULL);
    if (value < 0.1)
        return POWER_UNK;

    /* power given in uW by the kernel, we use dBm instead */
    return (10 * log10 (value / 1000.0));
}

static int
update_value (int fd, float *value)
{
    float power;

    power = reload_power_from_file (fd);
    if (fabs (power - *value) < 0.001)
        return -1;

    *value = power;
    return 0;
}

static void
reload_values (void)
{
    unsigned int i;
    unsigned int n_updates = 0;

    for (i = 0; i < context.n_ifaces; i++) {
        if ((!(context.ifaces[i]->tx_power_fd < 0)) &&
            update_value (context.ifaces[i]->tx_power_fd, &context.ifaces[i]->tx_power) == 0) {
            log_debug ("'%s' interface TX power updated: %.2lf",
                       context.ifaces[i]->name, context.ifaces[i]->tx_power);
            n_updates++;
        }
        if ((!(context.ifaces[i]->rx_power_fd < 0)) &&
            update_value (context.ifaces[i]->rx_power_fd, &context.ifaces[i]->rx_power) == 0) {
            log_debug ("'%s' interface RX power updated: %.2lf",
                       context.ifaces[i]->name, context.ifaces[i]->rx_power);
            n_updates++;
        }
    }

    if (n_updates) {
        log_debug ("need to refresh contents: %u values updated", n_updates);
        context.refresh_contents = true;
    }
}

/******************************************************************************/
/* Main */

int main (int argc, char *const *argv)
{
    setup_context (argc, argv);
    setup_log ();

    setlocale (LC_ALL, "");

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
        reload_values ();

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

        usleep ((timeout_ms > 0 ? timeout_ms : DEFAULT_TIMEOUT_MS) * 1000);
    } while (!context.stop);

    teardown_interfaces ();
    teardown_curses ();
    teardown_log();
    return 0;
}
