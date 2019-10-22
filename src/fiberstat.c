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

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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

/* natsort */
#include <strnatcmp.h>

/******************************************************************************/

#define PROGRAM_NAME    "fiberstat"
#define PROGRAM_VERSION PACKAGE_VERSION

/******************************************************************************/

/* Define to test bar fill levels */
/* #define FORCE_TEST_LEVELS */

/* Define to multiply by this number of times the information of the currently
 * available interfaces. E.g. if the system has 4 interfaces, a multiplier value
 * of 3 will make it expose 12 instead.
 */
/* #define FORCE_TEST_MULTIPLY_IFACES 3 */

/* Define to test polling fake sysfs files, use test/test-sysfs-setup to
 * initialize the test sysfs file tree.
 */
/* #define FORCE_TEST_SYSFS */

#if defined FORCE_TEST_SYSFS
# define SYSFS_PREFIX "/tmp"
#else
# define SYSFS_PREFIX ""
#endif

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

static unsigned int  n_explicit_ifaces;
static char        **explicit_ifaces;

static int
lookup_explicit_interface (const char *iface)
{
    unsigned int i;

    for (i = 0; i < n_explicit_ifaces; i++) {
        if (strcmp (iface, explicit_ifaces[i]) == 0)
            return i;
    }
    return -1;
}

static int
track_explicit_interface (const char *iface)
{
    if (lookup_explicit_interface (iface) >= 0)
        return 0;

    n_explicit_ifaces++;
    explicit_ifaces = realloc (explicit_ifaces, sizeof (char *) * n_explicit_ifaces);
    if (!explicit_ifaces)
        return -1;
    explicit_ifaces[n_explicit_ifaces - 1] = strdup (iface);
    if (!explicit_ifaces[n_explicit_ifaces - 1])
        return -2;
    return 0;
}

static void
print_help (void)
{
    printf ("\n"
            "Usage: " PROGRAM_NAME " [OPTION...]\n"
            "\n"
            "Common options:\n"
            "  -i, --iface=[IFACE]  Monitor the specific interface.\n"
            "  -t, --timeout        How often to reload values, in ms.\n"
            "  -d, --debug          Verbose output in " DEBUG_LOG ".\n"
            "  -h, --help           Show help.\n"
            "  -v, --version        Show version.\n"
            "\n"
            "Notes:\n"
            "  * -i,--iface may be given multiple times to specify more than\n"
            "    one explicit interface to monitor.\n"
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
    { "iface",   required_argument, 0, 'i' },
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

        iarg = getopt_long (argc, argv, "i:t:dhv", longopts, &idx);
        if (iarg < 0)
            break;

        switch (iarg) {
        case 'i':
            if (track_explicit_interface (optarg) < 0) {
                fprintf (stderr, "error: couldn't track explicit interface to monitor");
                exit (EXIT_FAILURE);
            }
            break;
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

    if (timeout_ms < 0)
        timeout_ms = DEFAULT_TIMEOUT_MS;
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
    bool    left_scroll_arrow;
    bool    right_scroll_arrow;

    InterfaceInfo **ifaces;
    unsigned int    n_ifaces;
    unsigned int    first_iface_index;

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
    keypad (stdscr, TRUE);
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
    COLOR_PAIR_BOX_BACKGROUND_GREEN,
    COLOR_PAIR_BOX_BACKGROUND_YELLOW,
    COLOR_PAIR_BOX_BACKGROUND_RED,
    COLOR_PAIR_BOX_BACKGROUND_WHITE,
    COLOR_PAIR_BOX_TEXT_GREEN,
    COLOR_PAIR_BOX_TEXT_YELLOW,
    COLOR_PAIR_BOX_TEXT_RED,
    COLOR_PAIR_BOX_TEXT_WHITE,
} ColorPair;

static void
setup_windows (void)
{
    static int once;

    if (!once) {
        once = 1;
        start_color ();
        curs_set (0);
        init_pair (COLOR_PAIR_MAIN,                  COLOR_WHITE,  COLOR_BLACK);
        init_pair (COLOR_PAIR_TITLE_TEXT,            COLOR_GREEN,  COLOR_BLACK);
        init_pair (COLOR_PAIR_SHORTCUT_TEXT,         COLOR_CYAN,   COLOR_BLACK);
        init_pair (COLOR_PAIR_BOX_BACKGROUND_GREEN,  COLOR_BLACK,  COLOR_GREEN);
        init_pair (COLOR_PAIR_BOX_BACKGROUND_YELLOW, COLOR_BLACK,  COLOR_YELLOW);
        init_pair (COLOR_PAIR_BOX_BACKGROUND_RED,    COLOR_BLACK,  COLOR_RED);
        init_pair (COLOR_PAIR_BOX_BACKGROUND_WHITE,  COLOR_BLACK,  COLOR_WHITE);
        init_pair (COLOR_PAIR_BOX_TEXT_GREEN,        COLOR_GREEN,  COLOR_BLACK);
        init_pair (COLOR_PAIR_BOX_TEXT_YELLOW,       COLOR_YELLOW, COLOR_BLACK);
        init_pair (COLOR_PAIR_BOX_TEXT_RED,          COLOR_RED,    COLOR_BLACK);
        init_pair (COLOR_PAIR_BOX_TEXT_WHITE,        COLOR_WHITE,  COLOR_BLACK);
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

/* Expected TX/RX power threshold
 * The GOOD/BAD thresholds are chosen so that the 'ignored partial' values
 * computed while on high res are 0, so that we have "full blocks" printed
 * with red and yellow. */
#define POWER_MAX    0.0
#define POWER_GOOD -18.4
#define POWER_BAD  -21.7
#define POWER_MIN  -25.0
#define POWER_UNK  -40.0

static float
power_to_percentage (float power)
{
    if (power >= POWER_MAX)
        return 100.0;
    if (power <= POWER_MIN)
        return 0.0;
    return 100.0 * (power - POWER_MIN) / (POWER_MAX - POWER_MIN);
}

/******************************************************************************/
/* List of hwmon entries */

#define PHANDLE_SIZE_BYTES 4

#define HWMON_SYSFS_DIR              SYSFS_PREFIX "/sys/class/hwmon"
#define HWMON_POWER1_INPUT_FILE      "power1_input"
#define HWMON_POWER2_INPUT_FILE      "power2_input"
#define HWMON_POWER1_LABEL_FILE      "power1_label"
#define HWMON_POWER2_LABEL_FILE      "power2_label"
#define HWMON_TX_POWER_LABEL_CONTENT "TX_power"
#define HWMON_RX_POWER_LABEL_CONTENT "RX_power"
#define HWMON_PHANDLE_FILE           "of_node/phandle"

typedef struct _HwmonInfo {
    char    *name;
    char    *tx_power_path;
    char    *rx_power_path;
    uint8_t  sfp_phandle[PHANDLE_SIZE_BYTES];
} HwmonInfo;

static void
hwmon_info_free (HwmonInfo *info)
{
    free (info->tx_power_path);
    free (info->rx_power_path);
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

static HwmonInfo *
lookup_hwmon (const uint8_t *phandle)
{
    unsigned int i;

    for (i = 0; i < context.n_hwmon; i++) {
        if (memcmp (context.hwmon[i]->sfp_phandle, phandle, PHANDLE_SIZE_BYTES) == 0)
            return context.hwmon[i];
    }
    return NULL;
}

static bool
check_file_contents (const char *path,
                     const char *contents)
{
    int    fd;
    bool   result;
    int    n_read;
    char   aux[255];
    size_t contents_size;

    fd = open (path, O_RDONLY);
    if (fd < 0)
        return false;

    if (!contents) {
        result = true;
        goto out;
    }

    contents_size = strlen (contents);
    n_read = read (fd, aux, contents_size);
    result = ((n_read == contents_size) && strncmp (aux, contents, n_read) == 0);

out:
    close (fd);
    return result;
}

static bool
check_file_exists (const char *path)
{
    return check_file_contents (path, NULL);
}

static bool
load_power_input_file_paths (const char  *hwmon,
                             char       **out_tx_file_path,
                             char       **out_rx_file_path)
{
    char  path[PATH_MAX];
    char *tx_file_path = NULL;
    char *rx_file_path = NULL;

    snprintf (path, sizeof (path), HWMON_SYSFS_DIR "/%s/" HWMON_POWER1_LABEL_FILE, hwmon);
    if (!check_file_contents (path, HWMON_TX_POWER_LABEL_CONTENT)) {
        log_debug ("hwmon '%s' doesn't have expected tx power label file", hwmon);
        goto out;
    }

    snprintf (path, sizeof (path), HWMON_SYSFS_DIR "/%s/" HWMON_POWER2_LABEL_FILE, hwmon);
    if (!check_file_contents (path, HWMON_RX_POWER_LABEL_CONTENT)) {
        log_debug ("hwmon '%s' doesn't have expected rx power label file", hwmon);
        goto out;
    }

    snprintf (path, sizeof (path), HWMON_SYSFS_DIR "/%s/" HWMON_POWER1_INPUT_FILE, hwmon);
    if (!check_file_exists (path)) {
        log_debug ("hwmon '%s' doesn't have tx power input file", hwmon);
        goto out;
    }
    tx_file_path = strdup (path);

    snprintf (path, sizeof (path), HWMON_SYSFS_DIR "/%s/" HWMON_POWER2_INPUT_FILE, hwmon);
    if (!check_file_exists (path)) {
        log_debug ("hwmon '%s' doesn't have rx power input file", hwmon);
        goto out;
    }
    rx_file_path = strdup (path);

out:
    if (tx_file_path && rx_file_path) {
        *out_tx_file_path = tx_file_path;
        *out_rx_file_path = rx_file_path;
        return true;
    }

    free (tx_file_path);
    free (rx_file_path);
    return false;
}

static bool
load_hwmon_phandle (const char *hwmon,
                    uint8_t    *phandle)
{
    char path[PATH_MAX];
    int  fd;
    int  n_read;

    snprintf (path, sizeof (path), HWMON_SYSFS_DIR "/%s/" HWMON_PHANDLE_FILE, hwmon);
    fd = open (path, O_RDONLY);
    if (fd < 0) {
        log_debug ("hwmon '%s' doesn't have sfp phandle file", hwmon);
        return false;
    }

    n_read = read (fd, phandle, PHANDLE_SIZE_BYTES);
    close (fd);

    if (n_read < PHANDLE_SIZE_BYTES) {
        log_warning ("couldn't read hwmon '%s' sfp phandle file", hwmon);
        return false;
    }

    return true;
}

static int
setup_hwmon_list (void)
{
    DIR           *d;
    struct dirent *dir;

    d = opendir (HWMON_SYSFS_DIR);
    if (!d)
        return -1;

    while ((dir = readdir(d)) != NULL) {
        char      *tx_file_path = NULL;
        char      *rx_file_path = NULL;
        uint8_t    phandle[PHANDLE_SIZE_BYTES];
        HwmonInfo *info;

        if ((strcmp (dir->d_name, ".") == 0) || (strcmp (dir->d_name, "..") == 0))
            continue;

        if (!load_power_input_file_paths (dir->d_name, &tx_file_path, &rx_file_path))
            continue;

        if (!load_hwmon_phandle (dir->d_name, phandle)) {
            free (tx_file_path);
            free (rx_file_path);
            continue;
        }

        /* valid hwmon entry */

        info = calloc (sizeof (HwmonInfo), 1);
        if (info)
            info->name = strdup (dir->d_name);
        if (!info || !info->name) {
            free (tx_file_path);
            free (rx_file_path);
            return -2;
        }

        info->tx_power_path = tx_file_path;
        info->rx_power_path = rx_file_path;
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

#define NET_SYSFS_DIR      SYSFS_PREFIX "/sys/class/net"
#define NET_PHANDLE_FILE   "of_node/sfp"
#define NET_OPERSTATE_FILE "operstate"

typedef struct _InterfaceInfo {
    char      *name;
    HwmonInfo *hwmon;
    char      *operstate_path;
    int        tx_power_fd;
    int        rx_power_fd;
    int        operstate_fd;
    float      tx_power;
    float      rx_power;
    char      *operstate;
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

static bool
load_interface_phandle (const char *iface,
                        uint8_t    *phandle)
{
    char path[PATH_MAX];
    int  fd;
    int  n_read;

    snprintf (path, sizeof (path), NET_SYSFS_DIR "/%s/" NET_PHANDLE_FILE, iface);
    fd = open (path, O_RDONLY);
    if (fd < 0) {
        log_debug ("iface '%s' doesn't have sfp phandle file", iface);
        return false;
    }

    n_read = read (fd, phandle, PHANDLE_SIZE_BYTES);
    close (fd);

    if (n_read < PHANDLE_SIZE_BYTES) {
        log_warning ("couldn't read iface '%s' sfp phandle file", iface);
        return false;
    }

    return true;
}

static int
compare_interface (const void *a, const void *b)
{
    return strnatcmp ((*((InterfaceInfo **)a))->name, (*((InterfaceInfo **)b))->name);
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
        uint8_t        phandle[PHANDLE_SIZE_BYTES];

        if ((strcmp (dir->d_name, ".") == 0) || (strcmp (dir->d_name, "..") == 0))
            continue;

        if (n_explicit_ifaces && lookup_explicit_interface (dir->d_name) < 0)
            continue;

        if (!load_interface_phandle (dir->d_name, phandle))
            continue;

        hwmon = lookup_hwmon (phandle);
        if (!hwmon) {
            log_warning ("couldn't match hwmon entry for net iface '%s'", dir->d_name);
            continue;
        }

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

        iface->tx_power_fd = open (hwmon->tx_power_path, O_RDONLY);
        iface->rx_power_fd = open (hwmon->rx_power_path, O_RDONLY);
        if (iface->tx_power_fd < 0)
            log_warning ("couldn't open TX power file for interface '%s' at %s", iface->name, hwmon->tx_power_path);
        if (iface->rx_power_fd < 0)
            log_warning ("couldn't open RX power file for interface '%s' at %s", iface->name, hwmon->rx_power_path);

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

    /* error if some of the explicit interfaces were not found */
    if (n_explicit_ifaces && (n_explicit_ifaces != context.n_ifaces)) {
        unsigned int i;

        for (i = 0; i < n_explicit_ifaces; i++) {
            unsigned int j;
            bool         found = false;

            for (j = 0; !found && (j < context.n_ifaces); j++) {
                if (strcmp (explicit_ifaces[i], context.ifaces[j]->name) == 0)
                    found = true;
            }
            if (!found)
                log_error ("explicit interface requested doesn't exist: %s", explicit_ifaces[i]);
        }
        return -4;
    }

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

    /* sort array by interface name */
    qsort (context.ifaces, context.n_ifaces, sizeof (InterfaceInfo *), compare_interface);

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

/* When using UTF-8, we can use a block fill from 1/8 to 8/8 */
static const int   RESOLUTION[] = { [BOX_CHARSET_ASCII] = 1, [BOX_CHARSET_UTF8] = 8 };
static const char *BLK[] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };

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
           bool        apply_thresholds,
           const char *label)
{
    static unsigned int resolution = 0;
    static int          max_level_fill_height = 0;
    static int          max_level_fill_height_n = 0;
    static int          good_level_fill_height = 0;
    static int          good_level_fill_height_n = 0;
    static int          bad_level_fill_height = 0;
    static int          bad_level_fill_height_n = 0;
    static int          row_color_green = 0;
    static int          row_color_yellow = 0;
    static int          row_color_red = 0;
    static int          row_color_white = 0;
    char                buf[32];
    unsigned int        i;
    unsigned int        j;
    float               fill_percent;
    float               fill_scaled;
    unsigned int        fill_height;
    unsigned int        fill_height_n;
    unsigned int        fill_height_partial = 0; /* 0-7 */
    unsigned int        x_center;

    /* initialize resolution info, only the first time we run */
    if (resolution == 0) {
        resolution = RESOLUTION[current_box_charset];
        /* when using low res, we change the background color and we use a
         * space as character. */
        if (resolution == 1) {
            row_color_green  = COLOR_PAIR (COLOR_PAIR_BOX_BACKGROUND_GREEN);
            row_color_yellow = COLOR_PAIR (COLOR_PAIR_BOX_BACKGROUND_YELLOW);
            row_color_red    = COLOR_PAIR (COLOR_PAIR_BOX_BACKGROUND_RED);
            row_color_white  = COLOR_PAIR (COLOR_PAIR_BOX_BACKGROUND_WHITE);
        }
        /* when using high res, we change foreground color and we use partial
         * block characters */
        else {
            row_color_green  = COLOR_PAIR (COLOR_PAIR_BOX_TEXT_GREEN);
            row_color_yellow = COLOR_PAIR (COLOR_PAIR_BOX_TEXT_YELLOW);
            row_color_red    = COLOR_PAIR (COLOR_PAIR_BOX_TEXT_RED);
            row_color_white  = COLOR_PAIR (COLOR_PAIR_BOX_TEXT_WHITE);
        }

        /* setup max level info */
        fill_percent = power_to_percentage (POWER_MAX);
        fill_scaled = ((float) fill_percent * BOX_CONTENT_HEIGHT * resolution) / 100.0;
        fill_height = floor (fill_scaled + 0.5);
        fill_height_n = fill_height / resolution;
        fill_height_partial = fill_height % resolution;
        max_level_fill_height = fill_height;
        max_level_fill_height_n = fill_height_n;
        log_debug ("max level fill percent: %.1f, fill height: %u (res: %u, N %u, partial ignored %u), per-step power: %.2f dBm",
                   fill_percent, max_level_fill_height, resolution, max_level_fill_height_n, fill_height_partial,
                   (POWER_MAX - POWER_MIN) / fill_height);
        assert (fill_height_partial == 0);

        /* setup good level info */
        fill_percent = power_to_percentage (POWER_GOOD);
        fill_scaled = ((float) fill_percent * BOX_CONTENT_HEIGHT * resolution) / 100.0;
        fill_height = floor (fill_scaled + 0.5);
        fill_height_n = fill_height / resolution;
        fill_height_partial = fill_height % resolution;
        good_level_fill_height = fill_height;
        good_level_fill_height_n = fill_height_n;
        log_debug ("good level fill percent: %.1f, fill height: %u (res: %u, N %u, partial ignored %u), power: %.2f dBm",
                   fill_percent, good_level_fill_height, resolution, good_level_fill_height_n, fill_height_partial, POWER_GOOD);
        assert (fill_height_partial == 0);

        /* setup bad level info */
        fill_percent = power_to_percentage (POWER_BAD);
        fill_scaled = ((float) fill_percent * BOX_CONTENT_HEIGHT * resolution) / 100.0;
        fill_height = floor (fill_scaled + 0.5);
        fill_height_n = fill_height / resolution;
        fill_height_partial = fill_height % resolution;
        bad_level_fill_height = fill_height;
        bad_level_fill_height_n = fill_height_n;
        log_debug ("bad level fill percent: %.1f, fill height: %u (res: %u, N %u, partial ignored %u), power: %.2f dBm",
                   fill_percent, bad_level_fill_height, resolution, bad_level_fill_height_n, fill_height_partial, POWER_BAD);
        assert (fill_height_partial == 0);
    }

    fill_percent = power_to_percentage (power);
    fill_scaled = ((float) fill_percent * BOX_CONTENT_HEIGHT * resolution) / 100.0;
    fill_height = floor (fill_scaled + 0.5);
    fill_height_n = fill_height / resolution;
    fill_height_partial = fill_height % resolution;
    log_debug ("fill percent: %.1f, fill height: %u (res: %u, N %u, partial %u), power: %.2f dBm",
               fill_percent, fill_height, resolution, fill_height_n, fill_height_partial, power);

    /* box */
    mvwprintw (context.content_win, y, x, "%s", TL[current_box_charset]);
    for (i = 0; i < BOX_CONTENT_WIDTH; i++)
        mvwprintw (context.content_win, y, x+1+i, "%s", HRZ[current_box_charset]);
    mvwprintw (context.content_win, y, x+1+BOX_CONTENT_WIDTH, "%s", TR[current_box_charset]);
    for (i = 0; i < BOX_CONTENT_HEIGHT; i++) {
        const char  *fill;
        bool         print = false;
        unsigned int row_height;

        row_height = BOX_CONTENT_HEIGHT - 1 - i;

        /*
         * The fill_height_n value specifies how many FULL blocks need to be printed.
         * The fill_height_partial value specifies the partial height (0-7) of the top block, if any
         */
        mvwprintw (context.content_win, y+1+i, x, "%s", VRT[current_box_charset]);

        if ((row_height == fill_height_n) && (fill_height_partial > 0)) {
            /* can't have partial on low resolution */
            assert (resolution > 1);
            print = true;
            fill = BLK[fill_height_partial - 1];
        } else if (row_height < fill_height_n) {
            print = true;
            if (resolution == 1)
                fill = " ";
            else
                fill= BLK[7];
        }

        if (print) {
            int row_color;

            if (1/* apply_thresholds */) {
                if (row_height < bad_level_fill_height_n)
                    row_color = row_color_red;
                else if (row_height < good_level_fill_height_n)
                    row_color = row_color_yellow;
                else
                    row_color = row_color_green;
            } else
                row_color = row_color_white;

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

    /* lowerlayerdown is too long and messes up the UI, so limit it a bit */
    if (strcmp (operstate, "lowerlayerdown") == 0)
        snprintf (buffer, sizeof (buffer), "link lowerdown");
    else
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
        static float fill = POWER_MIN;
        float extra = (current_box_charset == BOX_CHARSET_ASCII) ? 1 : 0.2;

        tx_power = fill;
        fill += extra;
        if (fill > POWER_MAX)
            fill = POWER_MIN;

        rx_power = fill;
        fill += extra;
        if (fill > POWER_MAX)
            fill = POWER_MIN;

        log_debug ("forced test levels: TX %.2f dBm, RX %.2f dBm", tx_power, rx_power);
    }
#endif /* FORCE_TEST_LEVELS */

    /* Print TX/RX boxes and common interface info */
    print_box (x, y, tx_power, false, "TX dBm");
    print_box (x + BOX_WIDTH + BOX_SEPARATION, y, rx_power, true, "RX dBm");
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

/* The margin at left and right allows to place the scrolling
 * arrows inside the margin (in the middle, at 2) */
#define MARGIN_HORIZONTAL 5

#define INTERFACE_SEPARATION_HORIZONTAL  3
#define INTERFACE_SEPARATION_VERTICAL    3

static void
refresh_contents (void)
{
    unsigned int i, n, x, y;
    unsigned int total_width;
    unsigned int total_height;
    unsigned int x_initial;
    unsigned int n_ifaces_per_row;
    unsigned int n_ifaces_per_column;
    unsigned int n_ifaces_per_window;
    unsigned int n_rows;
    unsigned int n_columns;
    unsigned int content_max_width;
    unsigned int content_max_height;
    unsigned int last_iface_index;
    unsigned int visible_ifaces;

    content_max_width = (context.max_x - (MARGIN_HORIZONTAL * 2));
    log_debug ("width: window %u, interface %u, content max %u",
               context.max_x, INTERFACE_WIDTH, content_max_width);

    content_max_height = context.max_y;
    log_debug ("height: window %u, interface %u, content max %u",
               context.max_y, INTERFACE_HEIGHT, content_max_height);

    werase (context.content_win);

    /* compute how many interfaces we can print in the window */

    /* rows... */
    n_ifaces_per_row = 0;
    while (1) {
        unsigned int next = ((n_ifaces_per_row + 1) * INTERFACE_WIDTH) + ((n_ifaces_per_row) * INTERFACE_SEPARATION_HORIZONTAL);
        if (next >= content_max_width)
            break;
        n_ifaces_per_row++;
    }
    log_debug ("number of interfaces per row: %u", n_ifaces_per_row);

    if (n_ifaces_per_row == 0) {
        log_warning ("window doesn't allow one full interface per row: forcing it anyway");
        n_ifaces_per_row = 1;
    }

    /* columns... */
    n_ifaces_per_column = 0;
    while (1) {
        unsigned int next = ((n_ifaces_per_column + 1) * INTERFACE_HEIGHT) + ((n_ifaces_per_column) * INTERFACE_SEPARATION_VERTICAL);
        if (next >= content_max_height)
            break;
        n_ifaces_per_column++;
    }
    log_debug ("number of interfaces per column: %u", n_ifaces_per_column);

    if (n_ifaces_per_column == 0) {
        log_warning ("window doesn't allow one full interface per column: forcing it anyway");
        n_ifaces_per_column = 1;
    }

    /* totals... */
    n_ifaces_per_window = n_ifaces_per_row * n_ifaces_per_column;
    log_debug ("window allows up to %u interfaces (%u per rows and %u per column)",
               n_ifaces_per_window, n_ifaces_per_row, n_ifaces_per_column);

    /* we skip all interfaces that have been scrolled */
    visible_ifaces = (context.n_ifaces - context.first_iface_index);

    /* compute amount of rows and columns we're printing */
    n_rows = visible_ifaces / n_ifaces_per_row;
    if (n_rows > 0) {
        if (visible_ifaces % n_ifaces_per_row > 0)
            n_rows++;
        if (n_rows > n_ifaces_per_column)
            n_rows = n_ifaces_per_column;
    } else
        n_rows = 1;
    n_columns = (visible_ifaces > n_ifaces_per_row) ? n_ifaces_per_row : visible_ifaces;
    log_debug ("printing %u rows with up to %u interfaces per row",
               n_rows, n_columns);

    /* compute total width and height in order to center elements in the interface */
    total_width = (n_columns * INTERFACE_WIDTH) + ((n_columns - 1) * INTERFACE_SEPARATION_HORIZONTAL);
    total_height = (n_rows * INTERFACE_HEIGHT) + ((n_rows - 1) * INTERFACE_SEPARATION_VERTICAL);
    log_debug ("total width %u, total height %u", total_width, total_height);

    /* check if we need scrolling support */
    context.left_scroll_arrow = false;
    context.right_scroll_arrow = false;
    if ((context.first_iface_index > 0) || (visible_ifaces > n_ifaces_per_window)) {
        last_iface_index = context.first_iface_index + n_ifaces_per_window;
        if (last_iface_index >= context.n_ifaces)
            last_iface_index = context.n_ifaces;
        else
            context.right_scroll_arrow = true;
        if (context.first_iface_index > 0)
            context.left_scroll_arrow = true;
    } else
        last_iface_index = context.n_ifaces;

    /* print scrolling arrows if needed */
    if (context.left_scroll_arrow) {
        wattron(context.content_win, A_BOLD);
        mvwprintw (context.content_win,
                   total_height / 2,
                   (context.max_x / 2) - (total_width / 2) - MARGIN_HORIZONTAL + 2,
                   "<");
        wattroff(context.content_win, A_BOLD);
    }

    if (context.right_scroll_arrow) {
        wattron(context.content_win, A_BOLD);
        mvwprintw (context.content_win,
                   total_height / 2,
                   (context.max_x / 2) + (total_width / 2) + MARGIN_HORIZONTAL - 2,
                   ">");
        wattroff(context.content_win, A_BOLD);
    }

    x_initial = x = (context.max_x / 2) - (total_width / 2);
    y = 0;

    for (n = 0, i = context.first_iface_index; i < last_iface_index; i++, n++) {
        print_interface (context.ifaces[i], x, y);
        if (((n + 1) % n_ifaces_per_row) == 0) {
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

#define QUIT_SHORTCUT 'q'

static void
setup_locale (void)
{
    const char *current;

    setlocale (LC_ALL, "");

    current = setlocale (LC_CTYPE, NULL);
    if (current && strstr (current, "utf8"))
        current_box_charset = BOX_CHARSET_UTF8;
}

static int
wait_for_input (void)
{
    fd_set         input_set;
    struct timeval menu_timeout;

    menu_timeout.tv_sec = timeout_ms / 1000;
    menu_timeout.tv_usec = (timeout_ms % 1000) * 1000;

    FD_ZERO (&input_set);
    FD_SET (0, &input_set);

    if (select (1, &input_set, NULL, NULL, &menu_timeout) < 0)
        return -1;

    return getch ();
}

int main (int argc, char *const *argv)
{
    int status = 0;

    setup_context (argc, argv);
    setup_log ();
    setup_locale ();

    log_info ("-----------------------------------------------------------");
    log_info ("starting program " PROGRAM_NAME " (v" PROGRAM_VERSION ")...");

    if (setup_curses () < 0) {
        fprintf (stderr, "error: couldn't setup curses\n");
        status = -1;
        goto out_cleanup_log;
    }

    if (setup_hwmon_list () < 0) {
        fprintf (stderr, "error: couldn't setup hwmon list\n");
        status = -2;
        goto out_cleanup_curses;
    }

    if (setup_interfaces () < 0) {
        fprintf (stderr, "error: couldn't setup interfaces\n");
        status = -3;
        goto out_cleanup_hwmon;
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

        switch (wait_for_input ()) {
            case QUIT_SHORTCUT:
                context.stop = true;
                break;
            case KEY_LEFT:
#if defined FORCE_TEST_LEVELS
                context.refresh_contents = true;
#endif
                if (context.left_scroll_arrow) {
                    assert (context.first_iface_index > 0);
                    context.first_iface_index--;
                    context.refresh_contents = true;
                    log_debug ("scroll left, first interface index %u", context.first_iface_index);
                }
                break;
            case KEY_RIGHT:
#if defined FORCE_TEST_LEVELS
                context.refresh_contents = true;
#endif
                if (context.right_scroll_arrow) {
                    context.first_iface_index++;
                    context.refresh_contents = true;
                    log_debug ("scroll right, first interface index %u", context.first_iface_index);
                }
                break;
            default:
                break;
        }
    } while (!context.stop);

    teardown_interfaces ();
out_cleanup_hwmon:
    teardown_hwmon_list ();
out_cleanup_curses:
    teardown_curses ();
out_cleanup_log:
    teardown_log();
    return status;
}
