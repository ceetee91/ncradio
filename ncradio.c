#include <curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <sys/time.h>
#include <errno.h>
#include <pthread.h>

#include "radio.h"
#include "config.h"

#define VERSION "0.1"

/* Fixed layout rows */
#define ROW_TITLE  0
#define ROW_SEP1   1
#define ROW_FREQ   2
#define ROW_VOL    3
#define ROW_INFO   4
#define ROW_SEP2   5
#define ROW_LIST   6

/* Color pairs */
#define CP_TITLE   1
#define CP_SEL     2
#define CP_SIG_LO  3
#define CP_SIG_MED 4
#define CP_SIG_HI  5
#define CP_VOL     6
#define CP_SCAN    7
#define CP_CUR     8

typedef enum { M_NORMAL, M_TUNING, M_SCANNING, M_EDITING, M_SETTINGS } Mode;

static Radio  radio;
static Config config;
static volatile int running = 1;
static Mode mode = M_NORMAL;

static int preset_sel  = 0;
static int list_offset = 0;
static int signal_pct  = 0;

/* tuning input */
static char tune_buf[16];
static int  tune_len = 0;

/* preset name edit */
static char edit_buf[NAME_MAX_LEN + 1];
static int  edit_len = 0;

/* settings panel */
static int settings_sel = 0;
#define SETTING_SCAN_STEP  0
#define SETTING_THRESHOLD  1
#define SETTING_RDS_NAMES  2
#define SETTING_COUNT      3

/* cycle list for scan step (Hz) */
static const uint32_t scan_steps[] = { 25000, 50000, 100000, 200000 };
#define SCAN_STEP_COUNT 4

/* timed status message */
static char   status_msg[128];
static time_t status_msg_exp = 0;

/* ── helpers ─────────────────────────────────────────────────────────── */

static void set_msg(const char *msg)
{
    strncpy(status_msg, msg, sizeof(status_msg) - 1);
    status_msg[sizeof(status_msg) - 1] = '\0';
    status_msg_exp = time(NULL) + 3;
}

static void draw_bar(int y, int x, int w, int pct, int cp)
{
    int filled = (pct * w) / 100;
    for (int i = 0; i < w; i++) {
        if (i < filled) {
            attron(COLOR_PAIR(cp) | A_BOLD);
            mvaddch(y, x + i, '|');
            attroff(COLOR_PAIR(cp) | A_BOLD);
        } else {
            attron(A_DIM);
            mvaddch(y, x + i, '.');
            attroff(A_DIM);
        }
    }
}

static int scan_step_idx(void)
{
    for (int i = 0; i < SCAN_STEP_COUNT; i++)
        if (config.scan_step_hz == scan_steps[i]) return i;
    return 2;  /* default: 0.10 MHz */
}

/* ── drawing ─────────────────────────────────────────────────────────── */

static void draw_title(void)
{
    attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvprintw(ROW_TITLE, (COLS - 14) / 2, " ncradio v" VERSION " ");
    attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);
    move(ROW_SEP1, 0); hline(ACS_HLINE, COLS);
}

static void draw_freq_signal(void)
{
    attron(A_BOLD);
    mvprintw(ROW_FREQ, 2, "Freq: %6.2f MHz", radio.freq_hz / 1000000.0);
    attroff(A_BOLD);

    if (radio.stereo) {
        attron(COLOR_PAIR(CP_SIG_HI) | A_BOLD);
        mvprintw(ROW_FREQ, 22, "[ST]");
        attroff(COLOR_PAIR(CP_SIG_HI) | A_BOLD);
    } else {
        attron(A_DIM);
        mvprintw(ROW_FREQ, 22, "[MO]");
        attroff(A_DIM);
    }

    int sc = (COLS >= 60) ? 30 : 27;
    int bw = (COLS >= 72) ? 14 : 10;
    int cp = (signal_pct >= 60) ? CP_SIG_HI :
             (signal_pct >= 30) ? CP_SIG_MED : CP_SIG_LO;
    mvprintw(ROW_FREQ, sc, "Signal:[");
    draw_bar(ROW_FREQ, sc + 8, bw, signal_pct, cp);
    mvprintw(ROW_FREQ, sc + 8 + bw, "]%3d%%", signal_pct);
}

static void draw_volume(void)
{
    mvprintw(ROW_VOL, 2, "Vol:  [");
    draw_bar(ROW_VOL, 9, 14, radio.volume, CP_VOL);
    mvprintw(ROW_VOL, 24, "] %3d%%", radio.volume);

    if (radio.muted) {
        attron(COLOR_PAIR(CP_SIG_LO) | A_BOLD | A_REVERSE);
        mvprintw(ROW_VOL, 32, " MUTED ");
        attroff(COLOR_PAIR(CP_SIG_LO) | A_BOLD | A_REVERSE);
    }

    /* RDS station name — right-aligned */
    if (radio.rds_capable && radio.rds.ps_ready && radio.rds.ps[0]) {
        int len = (int)strlen(radio.rds.ps);
        int col = COLS - len - 2;
        if (col > 42) {
            attron(COLOR_PAIR(CP_CUR) | A_BOLD);
            mvprintw(ROW_VOL, col, "%s", radio.rds.ps);
            attroff(COLOR_PAIR(CP_CUR) | A_BOLD);
        }
    }
}

static void draw_info(void)
{
    if (mode == M_TUNING) {
        attron(A_BOLD);
        mvprintw(ROW_INFO, 2, "Tune (MHz): %s_", tune_buf);
        attroff(A_BOLD);
    } else if (mode == M_EDITING) {
        attron(A_BOLD);
        mvprintw(ROW_INFO, 2, "Name: %.*s_", NAME_MAX_LEN, edit_buf);
        attroff(A_BOLD);
    } else if (mode == M_SCANNING) {
        pthread_mutex_lock(&radio.mutex);
        uint32_t pos   = radio.scan_pos_hz;
        int      found = radio.found_count;
        pthread_mutex_unlock(&radio.mutex);

        int pct = (FREQ_MAX_HZ == FREQ_MIN_HZ) ? 0 :
                  (int)((double)(pos - FREQ_MIN_HZ) * 100.0 /
                        (FREQ_MAX_HZ - FREQ_MIN_HZ));

        attron(COLOR_PAIR(CP_SCAN) | A_BOLD);
        mvprintw(ROW_INFO, 2,
                 "Scanning %6.2f MHz  Found: %d  [",
                 pos / 1000000.0, found);
        attroff(COLOR_PAIR(CP_SCAN) | A_BOLD);

        int x0 = getcurx(stdscr);
        int bw = COLS - x0 - 6;
        if (bw > 2) {
            draw_bar(ROW_INFO, x0, bw, pct, CP_SCAN);
            mvprintw(ROW_INFO, x0 + bw, "]%3d%%", pct);
        }
    } else {
        /* M_NORMAL / M_SETTINGS / M_EDITING — timed msg then RDS RT */
        if (status_msg[0] && time(NULL) < status_msg_exp) {
            attron(A_BOLD);
            mvprintw(ROW_INFO, 2, "%.*s", COLS - 4, status_msg);
            attroff(A_BOLD);
        } else if (radio.rds_capable && radio.rds.rt_ready && radio.rds.rt[0]) {
            attron(A_DIM);
            mvprintw(ROW_INFO, 2, "%.*s", COLS - 4, radio.rds.rt);
            attroff(A_DIM);
        }
    }

    move(ROW_SEP2, 0); hline(ACS_HLINE, COLS);
}

static int list_rows(void) { return LINES - ROW_LIST - 3; }

/* ── settings panel ──────────────────────────────────────────────────── */

static void draw_settings(void)
{
    int top = ROW_LIST;

    attron(A_BOLD);
    mvprintw(top, 2, "Settings:");
    attroff(A_BOLD);

    static const char *labels[SETTING_COUNT] = {
        "Scan step:",
        "Signal threshold:",
        "Save RDS names:",
    };
    static const char *hints[SETTING_COUNT] = {
        "<- -> to cycle",
        "<- -> to adjust (5% steps)",
        "<- -> or Enter to toggle",
    };

    for (int i = 0; i < SETTING_COUNT; i++) {
        char valstr[32];
        switch (i) {
        case SETTING_SCAN_STEP:
            snprintf(valstr, sizeof(valstr), "%.3g MHz",
                     config.scan_step_hz / 1000000.0);
            break;
        case SETTING_THRESHOLD:
            snprintf(valstr, sizeof(valstr), "%d%%",
                     config.signal_threshold_pct);
            break;
        case SETTING_RDS_NAMES:
            snprintf(valstr, sizeof(valstr), "%s",
                     config.rds_names ? "Yes" : "No");
            break;
        }

        int row = top + 2 + i;
        if (i == settings_sel) attron(COLOR_PAIR(CP_SEL) | A_BOLD);

        mvprintw(row, 2, "%c %-20s  %-12s",
                 i == settings_sel ? '>' : ' ',
                 labels[i], valstr);

        if (i == settings_sel) attroff(COLOR_PAIR(CP_SEL) | A_BOLD);

        /* hint text (dim), leave gap after value */
        attron(A_DIM);
        mvprintw(row, 38, "%s", hints[i]);
        attroff(A_DIM);
    }
}

/* ── preset list ─────────────────────────────────────────────────────── */

static void draw_presets(void)
{
    if (mode == M_SETTINGS) { draw_settings(); return; }

    int top  = ROW_LIST;
    int rows = list_rows();
    if (rows < 2) return;

    int name_w = COLS - 24;
    if (name_w < 0)            name_w = 0;
    if (name_w > NAME_MAX_LEN) name_w = NAME_MAX_LEN;

    if (mode == M_SCANNING) {
        pthread_mutex_lock(&radio.mutex);
        int      found = radio.found_count;
        uint32_t freqs[MAX_PRESETS];
        char     names[MAX_PRESETS][NAME_MAX_LEN + 1];
        if (found > 0) {
            memcpy(freqs, radio.found_freqs,
                   (size_t)found * sizeof(uint32_t));
            memcpy(names, radio.found_names,
                   (size_t)found * sizeof(names[0]));
        }
        pthread_mutex_unlock(&radio.mutex);

        attron(A_BOLD);
        mvprintw(top, 2, "Stations found so far:");
        attroff(A_BOLD);
        for (int i = 0; i < found && i < rows - 1; i++) {
            if (names[i][0])
                mvprintw(top + 1 + i, 4, "%2d.  %6.2f MHz  %s",
                         i + 1, freqs[i] / 1000000.0, names[i]);
            else
                mvprintw(top + 1 + i, 4, "%2d.  %6.2f MHz",
                         i + 1, freqs[i] / 1000000.0);
        }
        return;
    }

    int count = config.count;
    attron(A_BOLD);
    mvprintw(top, 2, "Presets (%d):", count);
    attroff(A_BOLD);

    if (count == 0) {
        attron(A_DIM);
        mvprintw(top + 1, 4,
                 "(none — press 's' to scan or 'a' to add current frequency)");
        attroff(A_DIM);
        return;
    }

    if (preset_sel >= count) preset_sel = count - 1;
    if (preset_sel < 0)      preset_sel = 0;

    int vis = rows - 1;
    if (preset_sel < list_offset)        list_offset = preset_sel;
    if (preset_sel >= list_offset + vis) list_offset = preset_sel - vis + 1;

    for (int i = 0; i < vis && list_offset + i < count; i++) {
        int idx    = list_offset + i;
        int is_sel = (idx == preset_sel);
        int is_cur = (config.freqs[idx] == radio.freq_hz);

        if (is_sel)      attron(COLOR_PAIR(CP_SEL) | A_BOLD);
        else if (is_cur) attron(COLOR_PAIR(CP_CUR));

        mvprintw(top + 1 + i, 2, "%c %2d.  %6.2f MHz  %-*.*s%s",
                 is_sel ? '>' : ' ',
                 idx + 1,
                 config.freqs[idx] / 1000000.0,
                 name_w, name_w,
                 config.names[idx],
                 is_cur ? " <" : "  ");
        clrtoeol();

        if (is_sel)      attroff(COLOR_PAIR(CP_SEL) | A_BOLD);
        else if (is_cur) attroff(COLOR_PAIR(CP_CUR));
    }
}

static void draw_help(void)
{
    int y = LINES - 3;
    move(y, 0); hline(ACS_HLINE, COLS);
    attron(A_DIM);
    switch (mode) {
    case M_SCANNING:
        mvprintw(y + 1, 2, "s/Esc:stop scan & save   q:stop & quit");
        break;
    case M_TUNING:
        mvprintw(y + 1, 2, "0-9 .:enter MHz   Enter:tune   Esc:cancel");
        break;
    case M_EDITING:
        mvprintw(y + 1, 2,
                 "Type name (max %d chars)   Enter:save   Esc:cancel   Bksp:delete",
                 NAME_MAX_LEN);
        break;
    case M_SETTINGS:
        mvprintw(y + 1, 2, "Up/Dn:select   Left/Right:adjust   Enter:toggle (RDS names)   Esc/o:done");
        break;
    case M_NORMAL:
        mvprintw(y + 1, 2,
                 "s:scan  ,:seek<  .:seek>  t:tune  m:mute  +/-:vol  a:add  d:del  e:rename  o:settings");
        mvprintw(y + 2, 2,
                 "Up/Dn:select preset   Enter:tune to preset   q:quit");
        break;
    }
    attroff(A_DIM);
}

static void draw_all(void)
{
    erase();
    draw_title();
    draw_freq_signal();
    draw_volume();
    draw_info();
    draw_presets();
    draw_help();
    refresh();
}

/* ── scan completion ─────────────────────────────────────────────────── */

static void finish_scan(int quit_after)
{
    radio_stop_scan(&radio);

    pthread_mutex_lock(&radio.mutex);
    int found = radio.found_count;
    config.count = 0;
    for (int i = 0; i < found && i < MAX_PRESETS; i++) {
        config.freqs[config.count] = radio.found_freqs[i];
        strncpy(config.names[config.count], radio.found_names[i], NAME_MAX_LEN);
        config.names[config.count][NAME_MAX_LEN] = '\0';
        config.count++;
    }
    pthread_mutex_unlock(&radio.mutex);

    config_save(&config);
    radio_set_freq(&radio, radio.freq_hz);
    mode = M_NORMAL;
    preset_sel = 0;

    char msg[96];
    snprintf(msg, sizeof(msg),
             "Scan done — found %d station%s, saved to ~/.ncradio.conf",
             config.count, config.count == 1 ? "" : "s");
    set_msg(msg);

    if (quit_after) running = 0;
}

/* ── input ───────────────────────────────────────────────────────────── */

static void handle_settings_key(int ch)
{
    switch (ch) {
    case KEY_UP:
        if (settings_sel > 0) settings_sel--;
        break;
    case KEY_DOWN:
        if (settings_sel < SETTING_COUNT - 1) settings_sel++;
        break;

    case KEY_LEFT:
    case KEY_RIGHT: {
        int right = (ch == KEY_RIGHT);
        if (settings_sel == SETTING_SCAN_STEP) {
            int idx = scan_step_idx();
            idx = right ? (idx + 1) % SCAN_STEP_COUNT
                        : (idx + SCAN_STEP_COUNT - 1) % SCAN_STEP_COUNT;
            config.scan_step_hz = scan_steps[idx];
        } else if (settings_sel == SETTING_THRESHOLD) {
            int p = config.signal_threshold_pct + (right ? 5 : -5);
            if (p < 5)  p = 5;
            if (p > 95) p = 95;
            config.signal_threshold_pct = p;
        } else if (settings_sel == SETTING_RDS_NAMES) {
            config.rds_names = !config.rds_names;
        }
        config_save(&config);
        break;
    }

    case '\n': case KEY_ENTER:
        if (settings_sel == SETTING_RDS_NAMES) {
            config.rds_names = !config.rds_names;
            config_save(&config);
        }
        break;

    case 27:   /* Esc */
    case 'o':
        mode = M_NORMAL;
        break;
    }
}

static void handle_key(int ch)
{
    switch (mode) {

    case M_SETTINGS:
        handle_settings_key(ch);
        break;

    case M_TUNING:
        if (ch == '\n' || ch == KEY_ENTER) {
            double mhz = 0;
            if (tune_len > 0 && sscanf(tune_buf, "%lf", &mhz) == 1 &&
                mhz >= 87.5 && mhz <= 108.0) {
                radio_set_freq(&radio, (uint32_t)(mhz * 1000000.0 + 0.5));
                signal_pct = radio_get_signal(&radio);
                set_msg("Tuned.");
            } else if (tune_len > 0) {
                set_msg("Invalid frequency — enter 87.50 to 108.00");
            }
            mode = M_NORMAL;
            tune_len = 0; tune_buf[0] = '\0';
        } else if (ch == 27) {
            mode = M_NORMAL;
            tune_len = 0; tune_buf[0] = '\0';
        } else if ((ch == KEY_BACKSPACE || ch == 127 || ch == '\b') && tune_len > 0) {
            tune_buf[--tune_len] = '\0';
        } else if ((isdigit(ch) || ch == '.') && tune_len < 6) {
            tune_buf[tune_len++] = (char)ch;
            tune_buf[tune_len]   = '\0';
        }
        break;

    case M_EDITING:
        if (ch == '\n' || ch == KEY_ENTER) {
            if (preset_sel >= 0 && preset_sel < config.count) {
                memcpy(config.names[preset_sel], edit_buf, NAME_MAX_LEN + 1);
                config_save(&config);
                set_msg("Name saved.");
            }
            mode = M_NORMAL;
        } else if (ch == 27) {
            mode = M_NORMAL;
        } else if ((ch == KEY_BACKSPACE || ch == 127 || ch == '\b') && edit_len > 0) {
            edit_buf[--edit_len] = '\0';
        } else if (isprint(ch) && edit_len < NAME_MAX_LEN) {
            edit_buf[edit_len++] = (char)ch;
            edit_buf[edit_len]   = '\0';
        }
        break;

    case M_SCANNING:
        if (ch == 's' || ch == 27) finish_scan(0);
        else if (ch == 'q')        finish_scan(1);
        break;

    case M_NORMAL:
        switch (ch) {
        case 'q': case 'Q':
            running = 0;
            break;

        case 's':
            /* Pass current settings into radio before starting scan */
            radio.scan_step_hz   = config.scan_step_hz;
            radio.scan_threshold = (config.signal_threshold_pct * 65535) / 100;
            radio.scan_rds_names = config.rds_names;
            radio_start_scan(&radio);
            mode = M_SCANNING;
            break;

        case ',': case KEY_LEFT:
            radio_seek(&radio, 0);
            signal_pct = radio_get_signal(&radio);
            break;

        case '.': case KEY_RIGHT:
            radio_seek(&radio, 1);
            signal_pct = radio_get_signal(&radio);
            break;

        case 't':
            mode = M_TUNING;
            tune_len = 0; tune_buf[0] = '\0';
            break;

        case 'o':
            mode = M_SETTINGS;
            settings_sel = 0;
            break;

        case '+': case '=':
            radio_set_volume(&radio, radio.volume + 5);
            break;
        case '-': case '_':
            radio_set_volume(&radio, radio.volume - 5);
            break;

        case 'm':
            radio_mute(&radio, !radio.muted);
            break;

        case 'a': {
            int prev = config.count;
            config_add(&config, radio.freq_hz, "");
            if (config.count > prev) {
                config_save(&config);
                for (int i = 0; i < config.count; i++)
                    if (config.freqs[i] == radio.freq_hz) preset_sel = i;
                set_msg("Added to presets.");
            } else {
                set_msg("Already in presets.");
            }
            break;
        }

        case 'd':
            if (config.count > 0 && preset_sel < config.count) {
                config_del(&config, preset_sel);
                config_save(&config);
                if (preset_sel >= config.count) preset_sel = config.count - 1;
                if (preset_sel < 0)             preset_sel = 0;
                set_msg("Preset deleted.");
            }
            break;

        case 'e':
            if (preset_sel >= 0 && preset_sel < config.count) {
                strncpy(edit_buf, config.names[preset_sel], NAME_MAX_LEN);
                edit_buf[NAME_MAX_LEN] = '\0';
                edit_len = (int)strlen(edit_buf);
                mode = M_EDITING;
            }
            break;

        case KEY_UP:
            if (preset_sel > 0) preset_sel--;
            break;
        case KEY_DOWN:
            if (preset_sel < config.count - 1) preset_sel++;
            break;

        case '\n': case KEY_ENTER:
            if (preset_sel >= 0 && preset_sel < config.count) {
                radio_set_freq(&radio, config.freqs[preset_sel]);
                signal_pct = radio_get_signal(&radio);
            }
            break;

        case KEY_PPAGE:
            preset_sel -= list_rows() - 2;
            if (preset_sel < 0) preset_sel = 0;
            break;
        case KEY_NPAGE:
            preset_sel += list_rows() - 2;
            if (preset_sel >= config.count) preset_sel = config.count - 1;
            break;
        }
        break;
    }
}

/* ── signal handler ──────────────────────────────────────────────────── */

static void on_signal(int sig) { (void)sig; running = 0; }

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *device = "/dev/radio0";
    if (argc > 1) device = argv[1];

    if (radio_open(&radio, device) < 0) {
        fprintf(stderr, "ncradio: cannot open %s: %s\n", device, strerror(errno));
        fprintf(stderr, "Usage: %s [/dev/radioN]\n", argv[0]);
        return 1;
    }

    config_load(&config);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(250);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(CP_TITLE,   COLOR_CYAN,   -1);
        init_pair(CP_SEL,     COLOR_BLACK,  COLOR_CYAN);
        init_pair(CP_SIG_LO,  COLOR_RED,    -1);
        init_pair(CP_SIG_MED, COLOR_YELLOW, -1);
        init_pair(CP_SIG_HI,  COLOR_GREEN,  -1);
        init_pair(CP_VOL,     COLOR_CYAN,   -1);
        init_pair(CP_SCAN,    COLOR_YELLOW, -1);
        init_pair(CP_CUR,     COLOR_GREEN,  -1);
    }

    signal_pct = radio_get_signal(&radio);

    struct timeval last_sig = {0, 0};

    while (running) {
        if (mode != M_SCANNING) {
            struct timeval now;
            gettimeofday(&now, NULL);
            long ms = (now.tv_sec  - last_sig.tv_sec)  * 1000
                    + (now.tv_usec - last_sig.tv_usec) / 1000;
            if (ms >= 500) {
                signal_pct = radio_get_signal(&radio);
                radio_read_rds(&radio);
                last_sig = now;
            }
        }

        if (mode == M_SCANNING && !radio.scanning && radio.scan_started)
            finish_scan(0);

        draw_all();

        int ch = getch();
        if (ch != ERR) handle_key(ch);
    }

    if (mode == M_SCANNING) finish_scan(0);

    endwin();
    radio_close(&radio);
    return 0;
}
