/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 * fiberstat.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
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
#include <locale.h>
#include <math.h>
#include <dirent.h>

#include <ncurses.h>

/******************************************************************************/

#define PROGRAM_NAME    "fiberstat"
#define PROGRAM_VERSION PACKAGE_VERSION

/******************************************************************************/

/* Define to test bar fill levels */
/* #define FORCE_TEST_LEVELS */

/* Define to test polling fake sysfs files
 * E.g.:
 *   $ mkdir -p /tmp/lo
 *   $ echo 100 > /tmp/lo/power1_input
 *   $ echo 200 > /tmp/lo/power2_input
 */
/* #define FORCE_TEST_SYSFS */

/* Define to multiply by this number of times the information of the currently
 * available interfaces. E.g. if the system has 4 interfaces, a multiplier value
 * of 3 will make it expose 12 instead.
 */
/* #define FORCE_TEST_MULTIPLY_IFACES 3 */

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
typedef struct _HwmonInfo     HwmonInfo;

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

    HwmonInfo    **hwmon;
    unsigned int   n_hwmon;
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
    COLOR_PAIR_BOX_CONTENT_GREEN,
    COLOR_PAIR_BOX_CONTENT_YELLOW,
    COLOR_PAIR_BOX_CONTENT_RED,
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
        init_pair (COLOR_PAIR_BOX_CONTENT_GREEN, COLOR_BLACK, COLOR_GREEN);
        init_pair (COLOR_PAIR_BOX_CONTENT_YELLOW, COLOR_BLACK, COLOR_YELLOW);
        init_pair (COLOR_PAIR_BOX_CONTENT_RED, COLOR_BLACK, COLOR_RED);
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
#define POWER_GOOD -10.0
#define POWER_BAD  -15.0
#define POWER_MIN  -20.0
#define POWER_UNK  -40.0

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
/* List of hwmon entries */

#define HWMON_SYSFS_DIR     "/sys/class/hwmon"
#define HWMON_TX_POWER_FILE "power1_input"
#define HWMON_RX_POWER_FILE "power2_input"
#define HWMON_PHANDLE_FILE  "of_node/phandle"

typedef struct _HwmonInfo {
    char    *name;
    char    *tx_power_path;
    char    *rx_power_path;
    uint8_t  sfp_phandle[4];
} HwmonInfo;

static void
hwmon_info_free (HwmonInfo *info)
{
    free (info->tx_power_path);
    free (info->tx_power_path);
    free (info->name);
    free (info);
}

static void
teardown_hwmon_list (void)
{
    unsigned int i;

    for (i = 0; i < context.n_hwmon; i++)
        hwmon_info_free (context.hwmon[i]);
    free (context.hwmon);
}

#if !defined FORCE_TEST_SYSFS
static HwmonInfo *
lookup_hwmon (const uint8_t *phandle)
{
    unsigned int i;

    for (i = 0; i < context.n_hwmon; i++) {
        if (memcmp (context.hwmon[i]->sfp_phandle, phandle, sizeof (context.hwmon[i]->sfp_phandle)) == 0)
            return context.hwmon[i];
    }
    return NULL;
}
#endif

static int
setup_hwmon_list (void)
{
    DIR           *d;
    struct dirent *dir;

    d = opendir (HWMON_SYSFS_DIR);
    if (!d)
        return -1;

    while ((dir = readdir(d)) != NULL) {
        char       path1[PATH_MAX];
        char       path2[PATH_MAX];
        char       path3[PATH_MAX];
        uint8_t    phandle[4];
        HwmonInfo *info;
        int        fd;
        int        n_read;

        if ((strcmp (dir->d_name, ".") == 0) || (strcmp (dir->d_name, "..") == 0))
            continue;

        snprintf (path1, sizeof (path1), HWMON_SYSFS_DIR "/%s/" HWMON_TX_POWER_FILE, dir->d_name);
        fd = open (path1, O_RDONLY);
        if (fd < 0) {
            log_debug ("hwmon '%s' doesn't have tx power file", dir->d_name);
            continue;
        }
        close (fd);

        snprintf (path2, sizeof (path2), HWMON_SYSFS_DIR "/%s/" HWMON_RX_POWER_FILE, dir->d_name);
        fd = open (path2, O_RDONLY);
        if (fd < 0) {
            log_debug ("hwmon '%s' doesn't have rx power file", dir->d_name);
            continue;
        }
        close (fd);

        snprintf (path3, sizeof (path3), HWMON_SYSFS_DIR "/%s/" HWMON_PHANDLE_FILE, dir->d_name);
        fd = open (path3, O_RDONLY);
        if (fd < 0) {
            log_debug ("hwmon '%s' doesn't have sfp phandle file", dir->d_name);
            continue;
        }

        n_read = read (fd, phandle, sizeof (phandle));
        close (fd);

        if (n_read < sizeof (phandle)) {
            log_warning ("couldn't read hwmon '%s' sfp phandle file", dir->d_name);
            continue;
        }

        /* valid hwmon entry */

        info = calloc (sizeof (HwmonInfo), 1);
        if (!info)
            return -2;

        info->name = strdup (dir->d_name);
        info->tx_power_path = strdup (path1);
        info->rx_power_path = strdup (path2);
        memcpy (info->sfp_phandle, phandle, sizeof (phandle));

        context.n_hwmon++;
        context.hwmon = realloc (context.hwmon, sizeof (HwmonInfo *) * context.n_hwmon);
        if (!context.hwmon)
            return -3;
        context.hwmon[context.n_hwmon - 1] = info;

        log_info ("hwmon '%s' is a valid monitor with sfp handle %02x:%02x:%02x:%02x",
                  dir->d_name, phandle[0], phandle[1], phandle[2], phandle[3]);
    }

    if (context.n_hwmon > 0)
        log_info ("hwmon entries found: %u", context.n_hwmon);
    else
        log_error ("no hwmon entries found");

    closedir (d);
    return 0;
}

/******************************************************************************/
/* List of interfaces */

#define NET_SYSFS_DIR      "/sys/class/net"
#define NET_PHANDLE_FILE   "of_node/sfp"
#define NET_OPERSTATE_FILE "operstate"

typedef struct _InterfaceInfo {
    char      *name;
    HwmonInfo *hwmon;
    char      *operstate_path;

#if defined FORCE_TEST_SYSFS
    char *tx_power_path;
    char *rx_power_path;
#endif

    int    tx_power_fd;
    int    rx_power_fd;
    int    operstate_fd;
    float  tx_power;
    float  rx_power;
    char  *operstate;
} InterfaceInfo;

static void
interface_info_free (InterfaceInfo *iface)
{
    if (!(iface->tx_power_fd < 0))
        close (iface->tx_power_fd);
    if (!(iface->rx_power_fd < 0))
        close (iface->rx_power_fd);
    if (!(iface->operstate_fd < 0))
        close (iface->operstate_fd);
#if defined FORCE_TEST_SYSFS
    free (iface->tx_power_path);
    free (iface->rx_power_path);
#endif
    free (iface->operstate_path);
    free (iface->operstate);
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
    DIR           *d;
    struct dirent *dir;

    d = opendir (NET_SYSFS_DIR);
    if (!d)
        return -1;

    while ((dir = readdir(d)) != NULL) {
        InterfaceInfo *iface;
        char           path[PATH_MAX];
        HwmonInfo     *hwmon = NULL;

        if ((strcmp (dir->d_name, ".") == 0) || (strcmp (dir->d_name, "..") == 0))
            continue;

#if !defined FORCE_TEST_SYSFS
        {
            uint8_t phandle[4];
            int     fd;
            int     n_read;

            snprintf (path, sizeof (path), NET_SYSFS_DIR "/%s/" NET_PHANDLE_FILE, dir->d_name);
            fd = open (path, O_RDONLY);
            if (fd < 0) {
                log_debug ("net iface '%s' doesn't have sfp phandle file", dir->d_name);
                continue;
            }

            n_read = read (fd, phandle, sizeof (phandle));
            close (fd);

            if (n_read < sizeof (phandle)) {
                log_warning ("couldn't read net iface '%s' sfp phandle file", dir->d_name);
                continue;
            }

            hwmon = lookup_hwmon (phandle);
            if (!hwmon) {
                log_warning ("couldn't match hwmon entry for net iface '%s'", dir->d_name);
                continue;
            }
        }
#endif

        iface = calloc (1, sizeof (InterfaceInfo));
        if (iface)
            iface->name = strdup (dir->d_name);
        if (!iface || !iface->name)
            return -2;

        log_info ("tracking interface '%s'...", iface->name);

        iface->hwmon = hwmon;
        iface->tx_power = POWER_MIN;
        iface->rx_power = POWER_MIN;
        iface->tx_power_fd = -1;
        iface->rx_power_fd = -1;
        iface->operstate_fd = -1;

#if !defined FORCE_TEST_SYSFS
        iface->tx_power_fd = open (hwmon->tx_power_path, O_RDONLY);
        iface->rx_power_fd = open (hwmon->rx_power_path, O_RDONLY);
        if (iface->tx_power_fd < 0)
            log_warning ("couldn't open TX power file for interface '%s' at %s", iface->name, hwmon->tx_power_path);
        if (iface->rx_power_fd < 0)
            log_warning ("couldn't open RX power file for interface '%s' at %s", iface->name, hwmon->rx_power_path);
#else
        snprintf (path, sizeof (path), "/tmp/%s/" HWMON_TX_POWER_FILE, iface->name);
        iface->tx_power_path = strdup (path);
        snprintf (path, sizeof (path), "/tmp/%s/" HWMON_RX_POWER_FILE, iface->name);
        iface->rx_power_path = strdup (path);
        iface->tx_power_fd = open (iface->tx_power_path, O_RDONLY);
        iface->rx_power_fd = open (iface->rx_power_path, O_RDONLY);
        if (iface->tx_power_fd < 0)
            log_warning ("couldn't open TX power file for interface '%s' at %s", iface->name, iface->tx_power_path);
        if (iface->rx_power_fd < 0)
            log_warning ("couldn't open RX power file for interface '%s' at %s", iface->name, iface->rx_power_path);
#endif

        snprintf (path, sizeof (path), NET_SYSFS_DIR "/%s/" NET_OPERSTATE_FILE, iface->name);
        iface->operstate_path = strdup (path);
        iface->operstate_fd = open (iface->operstate_path, O_RDONLY);
        if (iface->operstate_fd < 0)
            log_warning ("couldn't open operstate file for interface '%s' at %s", iface->name, iface->operstate_path);

        context.n_ifaces++;
        context.ifaces = realloc (context.ifaces, sizeof (InterfaceInfo *) * context.n_ifaces);
        if (!context.ifaces)
            return -3;
        context.ifaces[context.n_ifaces - 1] = iface;
    }

    closedir (d);

#if defined FORCE_TEST_MULTIPLY_IFACES
    {
        unsigned int m;
        unsigned int real_n_ifaces = context.n_ifaces;

        for (m = 0; m < (FORCE_TEST_MULTIPLY_IFACES - 1); m++) {
            unsigned int i;

            for (i = 0; i < real_n_ifaces; i++) {
                InterfaceInfo *iface;

                iface = calloc (1, sizeof (InterfaceInfo));
                if (!iface)
                    return -2;
                iface->name = strdup (context.ifaces[i]->name);
                iface->tx_power = POWER_MIN;
                iface->rx_power = POWER_MIN;
                iface->tx_power_fd = -1;
                iface->rx_power_fd = -1;
                iface->operstate_fd = -1;

                context.n_ifaces++;
                context.ifaces = realloc (context.ifaces, sizeof (InterfaceInfo *) * context.n_ifaces);
                if (!context.ifaces)
                    return -3;
                context.ifaces[context.n_ifaces - 1] = iface;
            }
        }
    }
#endif

    log_debug ("detected %u interfaces", context.n_ifaces);
    return 0;
}

/******************************************************************************/

typedef enum {
    BOX_CHARSET_ASCII,
    BOX_CHARSET_UTF8,
} BoxCharset;

static const char *VRT[] = { [BOX_CHARSET_ASCII] = "|", [BOX_CHARSET_UTF8] = "│" };
static const char *HRZ[] = { [BOX_CHARSET_ASCII] = "-", [BOX_CHARSET_UTF8] = "─" };
static const char *TL[]  = { [BOX_CHARSET_ASCII] = "-", [BOX_CHARSET_UTF8] = "┌" };
static const char *TR[]  = { [BOX_CHARSET_ASCII] = "-", [BOX_CHARSET_UTF8] = "┐" };
static const char *BL[]  = { [BOX_CHARSET_ASCII] = "-", [BOX_CHARSET_UTF8] = "└" };
static const char *BR[]  = { [BOX_CHARSET_ASCII] = "-", [BOX_CHARSET_UTF8] = "┘" };

static BoxCharset current_box_charset = BOX_CHARSET_ASCII;

/*
 * The information for one single interface is exposed as follows:
 *   ┌────┐ ┌────┐
 *   │    │ │    │
 *   │    │ │    │
 *   │    │ │    │
 *   │    │ │    │   The height of the interface info section is 21:
 *   │    │ │    │      box:        17 chars (15 content, 2 border)
 *   │    │ │    │      box info:    2 chars
 *   │    │ │    │      iface info:  2 chars
 *   │    │ │    │
 *   │    │ │    │   The minimum width of the interface info section is 13:
 *   │    │ │    │      TX box:         6 chars (4 content, 2 border)
 *   │    │ │    │      box separation: 1 char
 *   │    │ │    │      RX box:         6 chars (4 content, 2 border)
 *   │    │ │    │
 *   │    │ │    │
 *   └────┘ └────┘
 *   -20,00 -17,50     ----> TX/RX values in dBm   (box info)
 *   TX dBm RX dBm     ----> Box info              (box info)
 *        lo           ----> Interface name        (iface info)
 *   link unknown      ----> Link state            (iface info)
 *
 * The height of the bar is defined so that the whole interface takes
 * a maximum of 21 chars, because on serial terminals a window height
 * of 23 chars max is assumed as default and we don't want to take more
 * than that:
 *     1 char for app title
 *     21 chars for interface
 *     1 empty line to avoid cursor rewriting the last printed line
 */

#define BOX_CONTENT_WIDTH   4
#define BOX_BORDER_WIDTH    2
#define BOX_WIDTH           (BOX_CONTENT_WIDTH + BOX_BORDER_WIDTH)
#define BOX_CONTENT_HEIGHT  15
#define BOX_BORDER_HEIGHT   2
#define BOX_INFO_HEIGHT     2
#define BOX_HEIGHT          (BOX_CONTENT_HEIGHT + BOX_BORDER_HEIGHT + BOX_INFO_HEIGHT)
#define BOX_SEPARATION      1

#define IFACE_INFO_HEIGHT   2

#define INTERFACE_WIDTH  (BOX_WIDTH + BOX_SEPARATION + BOX_WIDTH)
#define INTERFACE_HEIGHT (BOX_HEIGHT + IFACE_INFO_HEIGHT)

static void
print_box (int         x,
           int         y,
           float       power,
           const char *label)
{
    static int    good_level_fill_height = 0;
    static int    bad_level_fill_height  = 0;
    char          buf[32];
    unsigned int  i;
    unsigned int  j;
    float         fill_scaled;
    unsigned int  fill_height;
    const char   *fill = " ";
    unsigned int  fill_percent;
    unsigned int  x_center;

    if (!good_level_fill_height) {
        fill_percent = power_to_percentage (POWER_GOOD);
        fill_scaled = ((float) fill_percent * BOX_CONTENT_HEIGHT) / 100.0;
        fill_height = floor (fill_scaled + 0.5);
        log_debug ("good level fill percent: %u, fill height: %u, power: %.2f dBm",
                   fill_percent, fill_height, POWER_GOOD);
        good_level_fill_height = fill_height;
    }

    if (!bad_level_fill_height) {
        fill_percent = power_to_percentage (POWER_BAD);
        fill_scaled = ((float) fill_percent * BOX_CONTENT_HEIGHT) / 100.0;
        fill_height = floor (fill_scaled + 0.5);
        log_debug ("bad level fill percent: %u, fill height: %u, power: %.2f dBm",
                   fill_percent, fill_height, POWER_BAD);
        bad_level_fill_height = fill_height;
    }

    fill_percent = power_to_percentage (power);
    fill_scaled = ((float) fill_percent * BOX_CONTENT_HEIGHT) / 100.0;
    fill_height = floor (fill_scaled + 0.5);
    log_debug ("fill percent: %u, fill height: %u, power: %.2f dBm",
               fill_percent, fill_height, power);

    /* box */
    mvwprintw (context.content_win, y, x, "%s", TL[current_box_charset]);
    for (i = 0; i < BOX_CONTENT_WIDTH; i++)
        mvwprintw (context.content_win, y, x+1+i, "%s", HRZ[current_box_charset]);
    mvwprintw (context.content_win, y, x+1+BOX_CONTENT_WIDTH, "%s", TR[current_box_charset]);
    for (i = 0; i < BOX_CONTENT_HEIGHT; i++) {
        mvwprintw (context.content_win, y+1+i, x, "%s", VRT[current_box_charset]);
        if (i >= (BOX_CONTENT_HEIGHT - fill_height)) {
            int row_color;

            if (i >= (BOX_CONTENT_HEIGHT - bad_level_fill_height))
                row_color = COLOR_PAIR (COLOR_PAIR_BOX_CONTENT_RED);
            else if (i >= (BOX_CONTENT_HEIGHT - good_level_fill_height))
                row_color = COLOR_PAIR (COLOR_PAIR_BOX_CONTENT_YELLOW);
            else
                row_color = COLOR_PAIR (COLOR_PAIR_BOX_CONTENT_GREEN);

            wattron (context.content_win, row_color);
            for (j = 0; j < BOX_CONTENT_WIDTH; j++)
                mvwprintw (context.content_win, y+1+i, x+1+j, fill);
            wattroff (context.content_win, row_color);
        }
        mvwprintw (context.content_win, y+1+i, x+1+BOX_CONTENT_WIDTH, "%s", VRT[current_box_charset]);
    }
    mvwprintw (context.content_win, y+1+BOX_CONTENT_HEIGHT, x, "%s", BL[current_box_charset]);
    for (i = 0; i < BOX_CONTENT_WIDTH; i++)
        mvwprintw (context.content_win, y+1+BOX_CONTENT_HEIGHT, x+1+i, "%s", HRZ[current_box_charset]);
    mvwprintw (context.content_win, y+1+BOX_CONTENT_HEIGHT, x+1+BOX_CONTENT_WIDTH, "%s", BR[current_box_charset]);

    /* box info */

    x_center = x + (BOX_WIDTH / 2) - (strlen (label) / 2);
    mvwprintw (context.content_win, y+1+BOX_CONTENT_HEIGHT+2, x_center, "%s", label);

    snprintf (buf, sizeof (buf), "%.2f", power);
    x_center = x + (BOX_WIDTH / 2) - (strlen (buf) / 2);
    mvwprintw (context.content_win, y+1+BOX_CONTENT_HEIGHT+1, x_center, "%s", buf);
}

static void
print_iface_info (int         x,
                  int         y,
                  const char *name,
                  const char *operstate)
{
    char buffer[100];
    int  x_center;

    x_center = x + (INTERFACE_WIDTH / 2) - (strlen (name) / 2);
    mvwprintw (context.content_win, y, x_center, "%s", name);

    snprintf (buffer, sizeof (buffer), "link %s", operstate);
    x_center = x + (INTERFACE_WIDTH / 2) - (strlen (buffer) / 2);
    mvwprintw (context.content_win, y + 1, x_center, "%s", buffer);
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
        fill += 2.5;
        if (fill > POWER_MAX)
            fill = POWER_MIN;

        rx_power = fill;
        fill += 2.5;
        if (fill > POWER_MAX)
            fill = POWER_MIN;

        log_debug ("forced test levels: TX %.2f dBm, RX %.2f dBm", tx_power, rx_power);
    }
#endif /* FORCE_TEST_LEVELS */

    /* Print TX/RX boxes and common interface info */
    print_box (x, y, tx_power, "TX dBm");
    print_box (x + BOX_WIDTH + BOX_SEPARATION, y, rx_power, "RX dBm");
    print_iface_info (x, y + BOX_HEIGHT, iface->name, iface->operstate);

    /* force moving cursor to next line to make app running through minicom happy */
    mvwprintw (context.content_win, y + INTERFACE_HEIGHT, 0, "");
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

static int
update_string (int fd, char **str)
{
    char    buffer[255] = { 0 };
    ssize_t n_read;

    lseek (fd, 0, SEEK_SET);
    n_read = read (fd, buffer, sizeof (buffer));
    if (n_read <= 0)
        return -1;

    if (buffer[n_read - 1] == '\n')
        buffer[n_read - 1] = '\0';

    if (*str && strcmp (*str, buffer) == 0)
        return -1;

    free (*str);
    *str = strdup (buffer);
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
        if ((!(context.ifaces[i]->operstate_fd < 0)) &&
            update_string (context.ifaces[i]->operstate_fd, &context.ifaces[i]->operstate) == 0) {
            log_debug ("'%s' interface operational state updated: %s",
                       context.ifaces[i]->name, context.ifaces[i]->operstate);
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

static void
setup_locale (void)
{
    const char *current;

    setlocale (LC_ALL, "");

    current = setlocale (LC_CTYPE, NULL);
    if (current && strstr (current, "utf8"))
        current_box_charset = BOX_CHARSET_UTF8;
}

int main (int argc, char *const *argv)
{
    setup_context (argc, argv);
    setup_log ();
    setup_locale ();

    log_info ("-----------------------------------------------------------");
    log_info ("starting program " PROGRAM_NAME " (v" PROGRAM_VERSION ")...");

    if (setup_curses () < 0) {
        fprintf (stderr, "error: couldn't setup curses\n");
        return -1;
    }

    if (setup_hwmon_list () < 0) {
        fprintf (stderr, "error: couldn't setup hwmon list\n");
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
    teardown_hwmon_list ();
    teardown_curses ();
    teardown_log();
    return 0;
}
