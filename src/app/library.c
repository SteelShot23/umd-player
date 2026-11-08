#include "library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/atrac3p/at3p.h"
#include "../core/canvas.h"
#include "../core/disc_image.h"
#include "../core/demux.h"
#include "../core/h264.h"
#include "../core/h264dec.h"
#include "../core/icons.h"
#include "../core/iso9660.h"
#include "../core/png.h"
#include "../core/sfo.h"
#include "../core/umdv.h"
#include "../core/vtiming.h"
#include "../core/yuv.h"
#include "../platform/platform.h"
#include "backdrop.h"
#include "infosheet.h"
#include "osd.h"
#include "resume.h"
#include "screen.h"
#include "settings.h"
#include "version.h"

#define UI_W           960
#define UI_H           544
#define ICON_W         144             /* ICON0.PNG and ICON1.PMF size on every disc */
#define ICON_H         80
/* The selected disc's icon, drawn at twice the size where the PSP draws it, so it covers the
   place PIC1 backgrounds leave for it (144x80 at 0,106 on the PSP's 480x272 screen); the
   panel goes beside it. */
#define BIG_X          2
#define BIG_Y          212
#define BIG_R          16              /* corner radius of the selected icon */
#define PANEL_GAP      24
#define PANEL_X        (BIG_X + 2 * ICON_W + PANEL_GAP)
#define PANEL_W        504
#define SMALL_X        (BIG_X + ICON_W / 2)   /* the other discs, centred under the selected one */
#define CACHE_N        6               /* discs whose art and sound are kept in memory */
#define FRAMES_AHEAD   3
#define SETTLE_US      150000          /* a selection shows its media once it rests this long */
#define STICK_DEAD     40              /* left stick travel (of 127) that does nothing */
#define STICK_SLOW     3.0f            /* discs a second just past the dead zone */
#define STICK_FAST     20.0f           /* and with the stick pushed all the way: a disc every third frame */
#define HOLD_DELAY     350000          /* Up or Down held this long starts repeating */
#define HOLD_SLOW      6.0f            /* discs a second as it starts */
#define HOLD_RAMP      2.0f            /* seconds to reach STICK_FAST */
#define LETTER_REPEAT  250000          /* L or R held: another letter this often, after HOLD_DELAY */
#define LETTER_SHOW    700000          /* how long a jump's letter stays on screen */
#define STRIP_W        44              /* the alphabet strip on the right edge: width, */
#define STRIP_TOP      70              /* top and bottom (above the hints) */
#define STRIP_BOTTOM   (UI_H - 96)
#define STRIP_SHOW     1500000         /* how long it stays after the last jump or scroll */
#define AU_MAX         4096
#define PMF_MAX        (2 * 1024 * 1024)
#define SND_MAX        (4 * 1024 * 1024)
#define PIC_MAX_W      960
#define PIC_MAX_H      544

#define DRAG_STEP      90              /* finger travel per disc when dragging the list: its spacing */
#define TAP_MOVE       24              /* at most this much movement for a tap */
#define TEXT_DIM  RGBA(180, 190, 210, 255)

/* The list view: the selected disc's icon stays at the left, the titles go beside it. */
#define LIST_X         322
#define LIST_Y         62
#define LIST_W         574             /* stops short of the alphabet strip */
#define LIST_ROW       28
#define LIST_ROWS      14
#define LIST_MARGIN    3               /* rows kept above and below the selection as the list moves */
/* The tabs over the list: Movies and TV Shows. */
#define TAB_Y          24
#define TAB_H          28
#define TITLE_Y        (BIG_Y + 2 * ICON_H + 4)   /* the selected title in the poster view */
#define NEIGHBOUR_X    (SMALL_X + ICON_W + 16)    /* the other discs' titles, beside their icons */
#define NEIGHBOUR_W    236
/* The shelf view: the selected disc's poster at the left, the next discs in a row beside it. */
#define SHELF_BIG_X    20
#define SHELF_BIG_Y    100
#define SHELF_BIG_W    205
#define SHELF_BIG_H    352
#define SHELF_X        245
#define SHELF_Y        170
#define SHELF_W        125
#define SHELF_H        215
#define SHELF_STEP     135
#define SHELF_TILES    5
#define SHELF_DRAG     110             /* finger travel per disc when swiping the row */
#define SHELF_SPEED    16.0f           /* how fast the row catches up with the selection, per second */
#define SHELF_LAG      2.0f            /* and how many discs it may fall behind */
/* A poster is ux0:data/umdplayer/posters/<the image's name>.png. Every disc keeps one in memory
   at the row's size; the worker makes the big one when a disc is selected or near it. Each is
   exactly the size it is drawn at, as the Vita draws sprites with the nearest texel. */
#define POSTER_W       SHELF_W
#define POSTER_H       SHELF_H
#define POSTER_FILE_MAX (3 * 1024 * 1024)
/* Details are ux0:data/umdplayer/info/<the image's name>.txt, "key value" lines from the poster
   script. Circle shows them on a page beside the poster. */
#define DETAILS_FILE_MAX 8192
#define DETAILS_TEXT_END 392           /* the description stops here, above the credits */

typedef struct lib_details {
    char name[96];                   /* the title on TMDb */
    char facts[96];                  /* year, length, rating and genres */
    char dates[80];
    char tagline[128];
    char credit1[160];
    char credit2[200];
    char about[1200];
} lib_details;

typedef struct lib_disc {
    char path[512];
    char title[96];
    uint8_t *icon0;                  /* RGBA, or NULL */
    int icon_w, icon_h;
    uint8_t *poster;                 /* RGBA, POSTER_W x POSTER_H, or NULL */
    uint32_t poster_id;              /* poster's id for the GPU (plat_sprite), 0 until drawn */
    lib_details *details;            /* or NULL */
    int has_resume;                 /* a saved position past the start, or a saved disc state */
    int resume_disc;                 /* played through its menus: has_resume is the disc state */
    int interactive;                 /* UMD-PG: an interactive movie, played through its menus */
    int incomplete;                  /* the image ends before the disc's files do */
    int no_movie;                    /* no UMD_VIDEO/PLAYLIST.UMD: nothing to play */
    uint64_t resume_pos, resume_start, resume_end;   /* 90 kHz; start and end are 0 for 0.1.0 entries */
    uint32_t icon_id;                /* icon0's id for the GPU (plat_sprite), 0 until drawn */
} lib_disc;

typedef struct icon_au {
    uint32_t off, size;
    uint64_t pts;
} icon_au;

/* ICON1.PMF, demuxed and timed by the worker so a switch only starts the decoder. */
typedef struct icon_video icon_video;
struct icon_video {
    uint8_t *es;
    icon_au *aus;
    int au_count;
    uint64_t first_pts, loop_len;
    h264_sps sps;
    uint8_t sps_nal[H264_MAX_PARAM_SET], pps_nal[H264_MAX_PARAM_SET];
    uint32_t sps_size, pps_size;
};

typedef struct decoded {
    plat_frame *frame;
    uint64_t pts;
} decoded;

/* A disc's media as the worker prepared it: the finished background, the timed icon video,
   and the sound file. The worker never replaces the entry of the disc on screen. */
enum { MEDIA_EMPTY, MEDIA_LOADING, MEDIA_READY };
typedef struct media {
    int disc, state;
    uint64_t used;
    uint8_t *bg;                     /* UI_W x UI_H RGBA, darkened and opaque, or NULL */
    uint8_t *pic0;                   /* PIC0.PNG, RGBA pic0_w x pic0_h, or NULL */
    int pic0_w, pic0_h;
    icon_video *icon;                /* or NULL */
    uint32_t bg_id, pic0_id;         /* the pictures' ids for the GPU while this entry lives */
    uint8_t *big_poster;             /* RGBA, SHELF_BIG_W x SHELF_BIG_H, or NULL */
    uint32_t big_poster_id;
    uint8_t *snd;
    uint32_t snd_size;
} media;

struct library {
    char dirs[512];                  /* the folders to list, separated by ';' */
    lib_disc *discs;                 /* every disc found; the first count are read and shown */
    int count, sel;
    /* Two lists in discs: tab 0 is discs 0 to split - 1, tab 1 the rest. The saved tab comes first,
       so the scan fills the list on screen first. */
    int split, tab, tab_sel[2], tab_tap;
    int list_top;                    /* the list view's top row */
    uint64_t front_draw_us;          /* the last redraw of the front layer, for the log */
    float shelf_pos;                 /* the disc the shelf's row is at, gliding towards sel */
    uint64_t shelf_us;
    int bg_pin;                      /* disc whose background stays on screen while scrolling, or -1 */
    /* The scan thread reads the discs in list order; scan_read of scan_total are done (under
       cache_lock), and library_update shows them as they come. */
    plat_thread *scanner;
    int scanning, scan_total, scan_read, scan_quit;
    int scan_force;                  /* read every disc, not its saved scan */
    int rescan;                      /* Settings, Rescan: tell the app to open the library again */
    uint64_t scan_start_us, last_loading_us;
    int loaded;                      /* disc whose media is in use, or -1 */

    /* Cache filled by the worker thread, guarded by cache_lock. */
    media cache[CACHE_N];
    plat_mutex *cache_lock;
    plat_cond *cache_cond;
    plat_thread *worker;
    int worker_quit, worker_paused, want_sel;
    uint64_t use_clock;

    /* The info sheet thread writes the missing sheets once the scan is done, while the setting
       is on: sheet_done of sheet_total, the current one sheet_permille through its checksums.
       sheet_stop (under cache_lock) is set while a movie plays and to quit; a sheet stopped
       half-way starts over. */
    plat_thread *sheeter;
    int sheet_quit, sheet_ended, sheet_total, sheet_done, sheet_finished, sheet_shown;
    volatile int sheet_stop, sheet_permille;

    const uint8_t *bg;
    const uint8_t *big_poster;       /* the selected disc's big poster, once the worker has it */
    uint32_t big_poster_id;
    const uint8_t *pic0;             /* the shown disc's title image, and its size */
    int pic0_w, pic0_h;
    const icon_video *icon;

    uint64_t moved_us;               /* when the selection last moved */
    int stick_on;                    /* the left stick is pushed past its dead zone */
    uint32_t hold_dir;               /* PLAT_BTN_UP or PLAT_BTN_DOWN held, or 0 */
    uint64_t hold_since, hold_us;
    float hold_acc;
    float stick_acc;                 /* discs owed to the stick, a fraction of one */
    uint64_t stick_us;
    uint32_t letter_dir;             /* PLAT_BTN_L or PLAT_BTN_R held, or 0 */
    uint64_t letter_next_us;         /* when a held L or R jumps again */
    uint64_t letter_us;              /* when the last jump by letter showed its letter */
    int letter_shown;                /* the jump's letter is on screen */
    uint64_t strip_us;               /* when the alphabet strip was last wanted */
    int strip_shown;                 /* it is on screen */
    int strip_touch;                 /* a finger is on it */
    int strip_target;                /* the disc the finger points at, or -1 */
    /* Where the GPU draws (plat_draws_sprites): the pictures as sprites, the background first
       and under the icon video, and the text, panels and icon corners on the transparent ui
       layer, redrawn only when what it shows changes (front_key). */
    plat_sprite sprites[10];
    uint32_t bg_id, pic0_id;
    unsigned char front_key[2048];
    uint8_t *front_prev;             /* the front layer as last handed over, for its changed rows */
    int front_valid;                 /* front_prev is what the platform was last given */
    /* Where a browsing frame's time goes, logged every 5 s (see library_update). */
    struct {
        uint64_t since, draw, media, icon, present;
        int frames, draws;
    } perf;

    /* The icon decoder stays open while browsing; a switch only flushes it. */
    plat_video *video;
    int video_w, video_h;
    int au_next;
    uint64_t loop_us, last_t, clk_raw, clk_us, snd_start;
    decoded frames[8];
    int frame_count;
    plat_frame *shown;

    /* The sound port stays open while browsing; a switch only changes what it decodes. */
    plat_mutex *lock;
    const uint8_t *snd;
    uint32_t snd_data, snd_frames, snd_block, snd_pos;
    int snd_channels, snd_rate, audio_open, audio_rate;
    at3p_decoder *at3p;
    float left[AT3P_FRAME_SAMPLES], right[AT3P_FRAME_SAMPLES];
    int16_t pcm[AT3P_FRAME_SAMPLES * 2];
    int pcm_len, pcm_pos;

    canvas ui;
    plat_present_info info;
    int ui_dirty;
    int show_info;                   /* SELECT toggles the title and resume panel */
    int details_scroll, details_more;   /* the details page: lines scrolled, and whether more follow */
    char notice[64];                 /* a message above the hints, until notice_until */
    uint64_t notice_until;
    unsigned notice_seq;
    int touch_down, touch_x0, touch_y0, touch_step_x, touch_step_y, touch_moved;   /* a finger on the screen */
    int scroll;                      /* mouse wheel steps not yet taken, positive down the list */
    uint64_t scroll_us;              /* when the wheel was last read */
    int settings_open, settings_row; /* △: the defaults for new discs */
    int keys_shown;                  /* the hints name keyboard keys (plat_keyboard_ui) */
    int marks_stale;                 /* reread the resume points: a movie was played */
};

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ---- reading discs ---- */

typedef struct disc_file {
    umd_source src;
    iso_volume vol;
} disc_file;

static int disc_open(disc_file *d, const char *path)
{
    if (plat_iso_open(path, &d->src) < 0)
        return -1;
    if (iso_open(&d->vol, &d->src) < 0) {
        plat_iso_close(&d->src);
        return -1;
    }
    return 0;
}

static uint8_t *disc_read(disc_file *d, const char *path, uint32_t *size, uint32_t max)
{
    iso_entry e;
    uint8_t *buf;

    if (iso_find(&d->vol, path, &e) < 0 || e.is_dir || e.size == 0 || e.size > max || !(buf = malloc(e.size)))
        return NULL;
    if (iso_read(&d->vol, &e, 0, buf, e.size) != (int64_t)e.size) {
        free(buf);
        return NULL;
    }
    *size = e.size;
    return buf;
}

static uint8_t *decode_png(const uint8_t *png, uint32_t size, int *w, int *h, int max_w, int max_h)
{
    uint8_t *rgba, *work;

    if (png_info(png, size, w, h) < 0 || *w > max_w || *h > max_h)
        return NULL;
    rgba = malloc((size_t)*w * *h * 4);
    work = malloc((size_t)(*w * 8 + 1) * *h);
    if (!rgba || !work || png_decode(png, size, rgba, *w * 4, work, (uint32_t)(*w * 8 + 1) * *h) < 0) {
        free(rgba);
        rgba = NULL;
    }
    free(work);
    return rgba;
}

typedef struct list_ctx {
    lib_disc *discs;
    int count, cap;
    const char *dir;
    int depth;                       /* subfolders below the listed folder */
} list_ctx;

/* Movies sorted in folders (by genre, by country, then by decade) are found down to three
   levels below each library folder. macOS app bundles are folders too; they hold no movies. */
#define LIB_DEPTH 3

static int is_bundle(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".app") == 0;
}

static const char *file_name(const char *path)
{
    /* after the last '/', or '\\' on Windows */
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back > slash)
        slash = back;
    return slash ? slash + 1 : path;
}

static void on_entry(void *user, const char *name, int is_dir)
{
    list_ctx *c = (list_ctx *)user;
    size_t n = strlen(name);

    if (is_dir) {
        char sub[512];
        const char *dir = c->dir;
        if (c->depth >= LIB_DEPTH || is_bundle(name) || snprintf(sub, sizeof(sub), "%s/%s", dir, name) >= (int)sizeof(sub))
            return;
        c->dir = sub;
        c->depth++;
        plat_list_dir(sub, on_entry, c);
        c->depth--;
        c->dir = dir;
        return;
    }
    if (n <= 4 || !image_name_supported(name)) {
        plat_log("library: skipped %s/%s\n", c->dir, name);
        return;
    }
    /* The same file in two folders shows once; resume data is keyed by the file name. */
    for (int i = 0; i < c->count; i++)
        if (strcmp(file_name(c->discs[i].path), name) == 0)
            return;
    if (c->count == c->cap) {
        int cap = c->cap ? c->cap * 2 : 64;
        lib_disc *more = realloc(c->discs, (size_t)cap * sizeof(*more));
        if (!more) {
            plat_log("library: no memory for %s/%s\n", c->dir, name);
            return;
        }
        c->discs = more;
        c->cap = cap;
    }
    memset(&c->discs[c->count], 0, sizeof(c->discs[0]));
    snprintf(c->discs[c->count++].path, sizeof(c->discs[0].path), "%s/%s", c->dir, name);
}

/* The name a disc sorts by: its file name without leading spaces and punctuation ("...As
   If") and without a leading "The ", "A " or "An ", so "The Matrix" sorts with "Matrix
   Revolutions, The". */
static const char *sort_name(const char *name)
{
    static const char *const articles[] = {"the ", "a ", "an "};

    while (*name && !((*name >= '0' && *name <= '9') || ((*name | 0x20) >= 'a' && (*name | 0x20) <= 'z')) &&
           (unsigned char)*name < 0x80)
        name++;
    for (unsigned i = 0; i < sizeof(articles) / sizeof(articles[0]); i++) {
        size_t n = strlen(articles[i]), k = 0;
        while (k < n && (name[k] | 0x20) == articles[i][k])
            k++;
        if (k == n && name[n])
            return name + n;
    }
    return name;
}

/* The letter a disc is listed under: A to Z, or # for a digit or anything else. */
static char disc_letter(const lib_disc *d)
{
    char c = (char)(*sort_name(file_name(d->path)) | 0x20);
    return c >= 'a' && c <= 'z' ? (char)(c - 0x20) : '#';
}

static int compare_discs(const void *a, const void *b)
{
    const lib_disc *da = a, *db = b;
    char la = disc_letter(da), lb = disc_letter(db);
    const char *sa = sort_name(file_name(da->path)), *sb = sort_name(file_name(db->path));
    const char *ea = strrchr(sa, '.'), *eb = strrchr(sb, '.');   /* the extension does not count */

    if (la != lb)
        return la == '#' ? -1 : lb == '#' ? 1 : la - lb;
    if (!ea)
        ea = sa + strlen(sa);
    if (!eb)
        eb = sb + strlen(sb);
    for (; sa < ea && sb < eb; sa++, sb++) {
        int ca = *sa >= 'A' && *sa <= 'Z' ? *sa | 0x20 : (unsigned char)*sa;
        int cb = *sb >= 'A' && *sb <= 'Z' ? *sb | 0x20 : (unsigned char)*sb;
        if (ca != cb)
            return ca - cb;
    }
    if ((sa < ea) != (sb < eb))
        return sa < ea ? 1 : -1;
    return strcmp(file_name(da->path), file_name(db->path));
}

/* Reads the title and ICON0.PNG of every disc; the rest loads when a disc is selected. */
/* UMD-PG discs (interactive movies, like Summer Days) carry no flag of their own. They are
   UMD Video with menus whose scripts pick the next clip: many unnamed titles, none of which
   plays much of the disc's video (5 to 21% on the test discs, against 61 to 99% for the
   longest title of a movie disc, Jackass included). Such a disc only makes sense played
   through its menus. */
static int is_interactive(disc_file *d)
{
    iso_entry dir;
    umdv_playlist *pl;
    uint8_t *buf;
    uint32_t size;
    uint64_t longest = 0, total = 0;
    int named = 0, result = 0;

    if (iso_find(&d->vol, "UMD_VIDEO/RESOURCE", &dir) < 0 || !dir.is_dir)
        return 0;
    if ((buf = disc_read(d, "UMD_VIDEO/PLAYLIST.UMD", &size, 1024 * 1024)) == NULL)
        return 0;
    if ((pl = malloc(sizeof(umdv_playlist))) != NULL && umdv_playlist_parse(buf, size, pl) == 0) {
        for (int i = 0; i < pl->title_count; i++) {
            uint32_t len = umdv_title_length(pl, &pl->titles[i]);   /* Jackass plays 96 clips as one */
            if (len > longest)
                longest = len;
            named += pl->titles[i].name[0] != 0;
        }
        /* The disc's video: each clip once, by the longest part of it a title plays. */
        for (int i = 0; i < pl->item_count; i++) {
            uint32_t len = pl->items[i].end_pts - pl->items[i].start_pts;
            int seen = 0;
            for (int j = 0; j < pl->item_count && !seen; j++)
                seen = j != i && strcmp(pl->items[j].clip, pl->items[i].clip) == 0 &&
                       (pl->items[j].end_pts - pl->items[j].start_pts > len || (pl->items[j].end_pts - pl->items[j].start_pts == len && j < i));
            if (!seen)
                total += len;
        }
        result = pl->title_count >= 20 && named == 0 && longest * 3 < total;
    }
    free(pl);
    free(buf);
    return result;
}

static icon_video *parse_icon(const uint8_t *pmf, uint32_t size);
static void free_icon(icon_video *v);
static uint8_t *first_icon_frame(const uint8_t *pmf, uint32_t size, int *w, int *h);

static int file_end(void *user, const char *path, const iso_entry *e)
{
    uint32_t *end = user, last = e->lba + (e->size + 2047) / 2048;
    (void)path;
    if (!e->is_dir && last > *end)
        *end = last;
    return 0;
}

/* The disc's poster at dw x dh, or NULL if the data folder has none. Each pixel is the average of
   the pixels of the file it covers, which keeps the small print on the case readable. */
static uint8_t *load_poster_size(const lib_disc *disc, int dw, int dh)
{
    char name[240], *dot;
    uint8_t *file, *rgba, *out;
    int n, w = 0, h = 0;

    snprintf(name, sizeof(name), "posters/%.220s", file_name(disc->path));
    if ((dot = strrchr(name, '.')) != NULL && dot > name + 8)
        *dot = 0;
    strcat(name, ".png");
    if ((file = malloc(POSTER_FILE_MAX)) == NULL)
        return NULL;
    n = plat_data_load(name, (char *)file, POSTER_FILE_MAX);
    rgba = n > 0 ? decode_png(file, (uint32_t)n, &w, &h, 1600, 1600) : NULL;
    free(file);
    if (!rgba)
        return NULL;
    if ((out = malloc((size_t)dw * dh * 4)) != NULL) {
        uint8_t *to = out;
        for (int y = 0; y < dh; y++) {
            int y0 = y * h / dh, y1 = (y + 1) * h / dh;
            if (y1 <= y0)
                y1 = y0 + 1;
            for (int x = 0; x < dw; x++) {
                int x0 = x * w / dw, x1 = (x + 1) * w / dw;
                uint32_t sum[4] = {0, 0, 0, 0}, count;
                if (x1 <= x0)
                    x1 = x0 + 1;
                count = (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0);
                for (int sy = y0; sy < y1; sy++) {
                    const uint8_t *from = rgba + ((size_t)sy * w + x0) * 4;
                    for (int sx = x0; sx < x1; sx++, from += 4) {
                        sum[0] += from[0];
                        sum[1] += from[1];
                        sum[2] += from[2];
                        sum[3] += from[3];
                    }
                }
                for (int k = 0; k < 4; k++)
                    *to++ = (uint8_t)(sum[k] / count);
            }
        }
    }
    free(rgba);
    return out;
}

static void load_poster(lib_disc *disc)
{
    disc->poster = load_poster_size(disc, POSTER_W, POSTER_H);
}

/* The disc's details, if the data folder has them. */
static void load_details(lib_disc *disc)
{
    char name[240], *dot, *buf, *line;
    lib_details *info;
    int n;

    snprintf(name, sizeof(name), "info/%.220s", file_name(disc->path));
    if ((dot = strrchr(name, '.')) != NULL && dot > name + 5)
        *dot = 0;
    strcat(name, ".txt");
    if ((buf = malloc(DETAILS_FILE_MAX)) == NULL)
        return;
    n = plat_data_load(name, buf, DETAILS_FILE_MAX - 1);
    if (n <= 0 || (info = calloc(1, sizeof(*info))) == NULL) {
        free(buf);
        return;
    }
    buf[n] = 0;
    for (line = buf; line && *line;) {
        char *end = strchr(line, '\n'), *value = strchr(line, ' ');
        if (end) {
            *end = 0;
            if (end > line && end[-1] == '\r')
                end[-1] = 0;
        }
        if (value && (!end || value < end)) {
            *value++ = 0;
#define DETAIL(field)                                                       \
            if (strcmp(line, #field) == 0)                                  \
                snprintf(info->field, sizeof(info->field), "%s", value)
            DETAIL(name);
            DETAIL(facts);
            DETAIL(dates);
            DETAIL(tagline);
            DETAIL(credit1);
            DETAIL(credit2);
            DETAIL(about);
#undef DETAIL
        }
        line = end ? end + 1 : NULL;
    }
    free(buf);
    disc->details = info;
}

/* A TV show, going by the details the poster script saved. Anything else is listed as a movie. */
static int disc_is_tv(const lib_disc *d)
{
    const char *facts = d->details ? d->details->facts : "";

    return strstr(facts, " season") != NULL || strstr(facts, "TV series") != NULL;
}

static int tv_sorted_first;

static int compare_sections(const void *a, const void *b)
{
    int ta = disc_is_tv(a), tb = disc_is_tv(b);

    if (ta != tb)
        return tv_sorted_first ? tb - ta : ta - tb;
    return compare_discs(a, b);
}

static void read_disc_info(lib_disc *disc)
{
    disc_file d;
    uint8_t *buf;
    uint32_t size;
    snprintf(disc->title, sizeof(disc->title), "%.95s", file_name(disc->path));
    if (strlen(disc->title) > 4 && disc->title[strlen(disc->title) - 4] == '.')
        disc->title[strlen(disc->title) - 4] = 0;   /* the file name, without .iso */
    load_poster(disc);
    if (disc_open(&d, disc->path) < 0)
        return;
    if ((buf = disc_read(&d, "UMD_VIDEO/PARAM.SFO", &size, 64 * 1024)) != NULL) {
        char title[96];
        if (sfo_get_string(buf, size, "TITLE", title, sizeof(title)) == 0 && title[0])
            snprintf(disc->title, sizeof(disc->title), "%s", title);
        free(buf);
    }
    if ((buf = disc_read(&d, "UMD_VIDEO/ICON0.PNG", &size, 256 * 1024)) != NULL) {
        disc->icon0 = decode_png(buf, size, &disc->icon_w, &disc->icon_h, 512, 512);
        free(buf);
    }
    /* Some discs have only the animated icon: its first frame stands in for the still one. */
    if (!disc->icon0 && (buf = disc_read(&d, "UMD_VIDEO/ICON1.PMF", &size, PMF_MAX)) != NULL) {
        disc->icon0 = first_icon_frame(buf, size, &disc->icon_w, &disc->icon_h);
        free(buf);
    }
    {
        /* A cut-short image (a bad dump or download) plays until the missing part, then
           waits for pictures that never come: say so in the library. */
        uint32_t end = 0;
        iso_walk(&d.vol, "UMD_VIDEO", file_end, &end);
        if (end > d.src.sector_count) {
            disc->incomplete = 1;
            plat_log("library: %s is incomplete: its files need %u sectors, the image has %u\n", disc->title,
                     end, d.src.sector_count);
        }
    }
    {
        iso_entry e;
        if (iso_find(&d.vol, "UMD_VIDEO/PLAYLIST.UMD", &e) < 0) {
            disc->no_movie = 1;
            plat_log("library: %s has no UMD_VIDEO/PLAYLIST.UMD: no movie to play\n", disc->title);
        }
    }
    disc->interactive = is_interactive(&d);
    if (disc->interactive)
        plat_log("library: %s is an interactive movie (UMD-PG)\n", disc->title);
    plat_iso_close(&d.src);
}

/* ---- ICON1.PMF: a PSMF header, then an MPEG program stream with H.264 ---- */

typedef struct mem_reader {
    const uint8_t *data;
    uint32_t size;
} mem_reader;

static int64_t read_mem(void *user, uint64_t off, void *dst, uint32_t len)
{
    mem_reader *m = (mem_reader *)user;
    if (off >= m->size)
        return 0;
    if (len > m->size - off)
        len = (uint32_t)(m->size - off);
    memcpy(dst, m->data + off, len);
    return len;
}

static void free_icon(icon_video *v)
{
    if (!v)
        return;
    free(v->es);
    free(v->aus);
    free(v);
}

/* The demuxer, parser, and timing state of one parse: too big for a thread's stack, and the
   worker and the scan thread both parse icons. */
typedef struct icon_parse {
    demux dmx;
    h264_parser parser;
    vtiming vt;
} icon_parse;

static icon_video *parse_icon_with(icon_parse *st, const uint8_t *pmf, uint32_t size)
{
    icon_video *v;
    mem_reader m;
    demux_unit u;
    uint32_t es_len = 0, offset;
    int resolved = 0, started = 0;
    uint64_t last = 0;

    if (size < 16 || memcmp(pmf, "PSMF", 4) != 0 || (offset = be32(pmf + 8)) >= size)
        return NULL;
    if (!(v = calloc(1, sizeof(*v))) || !(v->es = malloc(size)) || !(v->aus = malloc(AU_MAX * sizeof(icon_au)))) {
        free_icon(v);
        return NULL;
    }
    m.data = pmf + offset;
    m.size = size - offset;
    demux_init(&st->dmx, read_mem, &m);
    demux_select_mask(&st->dmx, 0, 0);
    h264_parser_init(&st->parser);
    while (demux_next(&st->dmx, &u) > 0 && v->au_count < AU_MAX) {
        h264_au_info info;
        if (u.type != DEMUX_VIDEO || h264_parse_au(&st->parser, u.data, u.size, &info) < 0)
            continue;
        if (!started) {
            if (!st->parser.sps.valid || !st->parser.pps_size)
                continue;
            vtiming_reset(&st->vt, &st->parser.sps);
            started = 1;
        }
        memcpy(v->es + es_len, u.data, u.size);
        v->aus[v->au_count].off = es_len;
        v->aus[v->au_count].size = u.size;
        v->aus[v->au_count].pts = PS_NO_TS;
        es_len += u.size;
        vtiming_push(&st->vt, &info, u.pts);
        v->au_count++;
        /* Collect times while their entries are still in the timing window. */
        for (uint64_t pts; resolved < v->au_count && vtiming_get(&st->vt, (uint32_t)resolved, &pts) != 0; resolved++)
            if (vtiming_get(&st->vt, (uint32_t)resolved, &pts) > 0)
                v->aus[resolved].pts = pts;
    }
    demux_free(&st->dmx);
    if (!started || v->au_count == 0) {
        free_icon(v);
        return NULL;
    }
    /* The last few never settle: continue them one frame apart. */
    for (int i = 0; i < v->au_count; i++) {
        if (v->aus[i].pts == PS_NO_TS)
            v->aus[i].pts = last + 3003;
        last = v->aus[i].pts;
    }
    v->first_pts = UINT64_MAX;
    for (int i = 0; i < v->au_count; i++)
        if (v->aus[i].pts < v->first_pts)
            v->first_pts = v->aus[i].pts;
    for (int i = 0; i < v->au_count; i++)
        if (v->aus[i].pts - v->first_pts + 3003 > v->loop_len)
            v->loop_len = v->aus[i].pts - v->first_pts + 3003;
    v->sps = st->parser.sps;
    memcpy(v->sps_nal, st->parser.sps_nal, st->parser.sps_size);
    memcpy(v->pps_nal, st->parser.pps_nal, st->parser.pps_size);
    v->sps_size = st->parser.sps_size;
    v->pps_size = st->parser.pps_size;
    return v;
}

static icon_video *parse_icon(const uint8_t *pmf, uint32_t size)
{
    icon_parse *st = malloc(sizeof(*st));
    icon_video *v = st ? parse_icon_with(st, pmf, size) : NULL;
    free(st);
    return v;
}

#define ICON_FRAMES_MAX 60           /* frames looked at for a still icon: 2 s */
#define ICON_LUMA_MIN   48           /* mean Y of a frame that is not a fade from black */

static int mean_luma(const h264dec_frame *f)
{
    uint64_t sum = 0;
    for (int y = 0; y < f->height; y++)
        for (int x = 0; x < f->width; x++)
            sum += f->plane[0][(size_t)y * (size_t)f->stride[0] + (size_t)x];
    return (int)(sum / ((uint64_t)f->width * (uint64_t)f->height));
}

/* Keeps the frame as RGBA if it is brighter than the one kept so far. */
static void keep_brighter(const h264dec_frame *f, uint8_t **rgba, int *best, int *w, int *h)
{
    int luma = mean_luma(f);
    uint8_t *out;

    if (luma <= *best || f->width <= 0 || f->height <= 0 ||
        (out = malloc((size_t)f->width * (size_t)f->height * 4)) == NULL)
        return;
    yuv420_to_bgra(f->plane, f->stride, f->width, f->height, out, f->width * 4);
    for (size_t k = 0; k < (size_t)f->width * (size_t)f->height * 4; k += 4) {   /* BGRA to RGBA */
        uint8_t b = out[k];
        out[k] = out[k + 2];
        out[k + 2] = b;
    }
    free(*rgba);
    *rgba = out;
    *best = luma;
    *w = f->width;
    *h = f->height;
}

/* A still picture from ICON1.PMF, decoded in software (the platform's decoder may be busy with
   the selected disc's icon): the first frame that is not dark, as many icons fade in from
   black, or the brightest of the first two seconds. */
static uint8_t *first_icon_frame(const uint8_t *pmf, uint32_t size, int *w, int *h)
{
    icon_video *v = parse_icon(pmf, size);
    h264dec *dec = v ? h264dec_open() : NULL;
    h264dec_frame f;
    uint8_t *rgba = NULL;
    int best = -1, seen = 0;

    for (int i = 0; dec && i < v->au_count && seen < ICON_FRAMES_MAX && best < ICON_LUMA_MIN; i++) {
        if (h264dec_decode(dec, v->es + v->aus[i].off, v->aus[i].size, (uint64_t)i) < 0)
            break;
        while (best < ICON_LUMA_MIN && h264dec_output(dec, &f)) {
            keep_brighter(&f, &rgba, &best, w, h);
            seen++;
        }
    }
    if (dec && best < ICON_LUMA_MIN) {
        h264dec_flush(dec);
        while (seen++ < ICON_FRAMES_MAX && h264dec_output(dec, &f))
            keep_brighter(&f, &rgba, &best, w, h);
    }
    if (dec)
        h264dec_close(dec);
    if (v)
        free_icon(v);
    return rgba;
}

/* PIC1.PNG scaled to the screen, darkened a little, opaque. */
static uint8_t *make_background(const uint8_t *png, uint32_t size)
{
    int w, h;
    uint8_t *pic = decode_png(png, size, &w, &h, PIC_MAX_W, PIC_MAX_H), *bg;
    canvas c;

    if (!pic)
        return NULL;
    if ((bg = malloc(UI_W * UI_H * 4)) != NULL) {
        c.rgba = bg;
        c.w = UI_W;
        c.h = UI_H;
        c.stride = UI_W * 4;
        canvas_rect(&c, 0, 0, UI_W, UI_H, RGBA(0, 0, 0, 255));
        canvas_blit(&c, pic, w, h, w * 4, 0, 0, UI_W, UI_H);
        for (int i = 0; i < UI_W * UI_H; i++)
            bg[i * 4 + 3] = 255;
        canvas_darken(&c, 0, 0, UI_W, UI_H, 215);
    }
    free(pic);
    return bg;
}

/* ---- SND0.AT3: RIFF WAVE with ATRAC3plus frames of block_align bytes ---- */

/* Points the sound at at3 (lock held), or at silence when at3 is NULL or unusable. */
static void set_sound(library *l, const uint8_t *at3, uint32_t size)
{
    uint32_t o = 12, block = 0, data = 0, frames = 0;
    int channels = 0, rate = 0;

    l->snd = NULL;
    if (!at3 || size < 12 || memcmp(at3, "RIFF", 4) != 0 || memcmp(at3 + 8, "WAVE", 4) != 0)
        return;
    while (o + 8 <= size) {
        uint32_t len = le32(at3 + o + 4);
        if (o + 8 + len > size)
            len = size - o - 8;
        if (memcmp(at3 + o, "fmt ", 4) == 0 && len >= 16) {
            channels = (int)le16(at3 + o + 10);
            rate = (int)le32(at3 + o + 12);
            block = le16(at3 + o + 20);
        } else if (memcmp(at3 + o, "data", 4) == 0 && block) {
            data = o + 8;
            frames = len / block;
        }
        o += 8 + len + (len & 1);
    }
    if (!frames || (channels != 1 && channels != 2) || at3p_init(l->at3p, channels) < 0)
        return;
    l->snd = at3;
    l->snd_data = data;
    l->snd_frames = frames;
    l->snd_block = block;
    l->snd_channels = channels;
    l->snd_rate = rate;
    l->snd_pos = 0;
    l->pcm_len = l->pcm_pos = 0;
}

static void sound_fill(void *user, int16_t *out, int frames)
{
    library *l = (library *)user;
    float *planes[2] = {l->left, l->right};

    plat_mutex_lock(l->lock);
    while (frames > 0) {
        int n;
        if (!l->snd) {
            memset(out, 0, (size_t)frames * 4);
            break;
        }
        if (l->pcm_pos == l->pcm_len) {
            const uint8_t *f = l->snd + l->snd_data + (size_t)l->snd_pos * l->snd_block;
            if (at3p_decode(l->at3p, f, l->snd_block, planes) < 0) {
                memset(l->left, 0, sizeof(l->left));
                memset(l->right, 0, sizeof(l->right));
            }
            if (l->snd_channels == 1)
                memcpy(l->right, l->left, sizeof(l->left));
            at3p_to_s16(planes, 2, l->pcm);
            l->pcm_len = AT3P_FRAME_SAMPLES;
            l->pcm_pos = 0;
            l->snd_pos = (l->snd_pos + 1) % l->snd_frames;   /* loop */
        }
        n = l->pcm_len - l->pcm_pos < frames ? l->pcm_len - l->pcm_pos : frames;
        memcpy(out, l->pcm + l->pcm_pos * 2, (size_t)n * 4);
        l->pcm_pos += n;
        out += n * 2;
        frames -= n;
    }
    plat_mutex_unlock(l->lock);
}

/* ---- what a scan learned, kept between runs ---- */

/* cache/<the image's name>.bin in the data folder: this, the poster if any, then the icon. Reading
   it takes a fraction of the time of opening the disc and decoding its pictures. New discs have
   none, so they are read as they appear; Settings, Rescan reads them all again. */
#define DISC_CACHE_MAGIC 0x31434D55u
#define DISC_CACHE_ICON_MAX (512 * 512 * 4)
enum { CACHED_POSTER = 1, CACHED_INCOMPLETE = 2, CACHED_NO_MOVIE = 4, CACHED_INTERACTIVE = 8 };

typedef struct disc_cache_head {
    uint32_t magic;
    int32_t icon_w, icon_h, flags;
    char title[96];
} disc_cache_head;

static void disc_cache_name(const lib_disc *disc, char *name, size_t size)
{
    char *dot;

    snprintf(name, size, "cache/%.220s", file_name(disc->path));
    if ((dot = strrchr(name, '.')) != NULL && dot > name + 6)
        *dot = 0;
    strcat(name, ".bin");
}

/* Fills in the disc from its cache file. Returns 1, or 0 if it has no good one. */
static int load_disc_cache(lib_disc *disc)
{
    char name[240], *file;
    size_t poster_size = POSTER_W * POSTER_H * 4, max = sizeof(disc_cache_head) + poster_size + DISC_CACHE_ICON_MAX;
    size_t icon_size, want;
    disc_cache_head h;
    uint8_t *poster = NULL, *icon = NULL;
    int n, ok = 0;

    disc_cache_name(disc, name, sizeof(name));
    if ((file = malloc(max)) == NULL)
        return 0;
    n = plat_data_load(name, file, (int)max);
    if (n < (int)sizeof(h))
        goto done;
    memcpy(&h, file, sizeof(h));
    if (h.magic != DISC_CACHE_MAGIC || h.icon_w < 0 || h.icon_h < 0 || h.icon_w > 512 || h.icon_h > 512)
        goto done;
    icon_size = (size_t)h.icon_w * (size_t)h.icon_h * 4;
    want = sizeof(h) + ((h.flags & CACHED_POSTER) ? poster_size : 0) + icon_size;
    if ((size_t)n != want)
        goto done;
    if ((h.flags & CACHED_POSTER) && (poster = malloc(poster_size)) == NULL)
        goto done;
    if (icon_size && (icon = malloc(icon_size)) == NULL) {
        free(poster);
        goto done;
    }
    if (poster)
        memcpy(poster, file + sizeof(h), poster_size);
    if (icon)
        memcpy(icon, file + sizeof(h) + (poster ? poster_size : 0), icon_size);
    h.title[sizeof(h.title) - 1] = 0;
    snprintf(disc->title, sizeof(disc->title), "%s", h.title);
    disc->poster = poster;
    disc->icon0 = icon;
    disc->icon_w = h.icon_w;
    disc->icon_h = h.icon_h;
    disc->incomplete = (h.flags & CACHED_INCOMPLETE) != 0;
    disc->no_movie = (h.flags & CACHED_NO_MOVIE) != 0;
    disc->interactive = (h.flags & CACHED_INTERACTIVE) != 0;
    ok = 1;
done:
    free(file);
    return ok;
}

static void save_disc_cache(const lib_disc *disc)
{
    char name[240], *file;
    disc_cache_head h;
    size_t poster_size = disc->poster ? POSTER_W * POSTER_H * 4 : 0;
    size_t icon_size = disc->icon0 ? (size_t)disc->icon_w * (size_t)disc->icon_h * 4 : 0;

    if (icon_size > DISC_CACHE_ICON_MAX || (file = malloc(sizeof(h) + poster_size + icon_size)) == NULL)
        return;
    memset(&h, 0, sizeof(h));
    h.magic = DISC_CACHE_MAGIC;
    h.icon_w = icon_size ? disc->icon_w : 0;
    h.icon_h = icon_size ? disc->icon_h : 0;
    h.flags = (disc->poster ? CACHED_POSTER : 0) | (disc->incomplete ? CACHED_INCOMPLETE : 0) |
              (disc->no_movie ? CACHED_NO_MOVIE : 0) | (disc->interactive ? CACHED_INTERACTIVE : 0);
    snprintf(h.title, sizeof(h.title), "%s", disc->title);
    memcpy(file, &h, sizeof(h));
    if (poster_size)
        memcpy(file + sizeof(h), disc->poster, poster_size);
    if (icon_size)
        memcpy(file + sizeof(h) + poster_size, disc->icon0, icon_size);
    disc_cache_name(disc, name, sizeof(name));
    if (plat_data_save(name, file, (int)(sizeof(h) + poster_size + icon_size)) < 0)
        plat_log("library: cannot save %s\n", name);
    free(file);
}

/* ---- the media cache ---- */

static media *find_media(library *l, int disc)
{
    for (int i = 0; i < CACHE_N; i++)
        if (l->cache[i].disc == disc && l->cache[i].state != MEDIA_EMPTY)
            return &l->cache[i];
    return NULL;
}

static uint32_t lib_id(void);

static void free_media(media *m)
{
    free(m->bg);
    free(m->pic0);
    free(m->big_poster);
    free_icon(m->icon);
    free(m->snd);
    memset(m, 0, sizeof(*m));
    m->disc = -1;
}

/* Reads and prepares one disc's media (no lock held: the disc has its own file handle). */
static void read_media(const lib_disc *disc, media *m)
{
    disc_file d;
    uint8_t *buf;
    uint32_t size;

    if (disc->poster)
        m->big_poster = load_poster_size(disc, SHELF_BIG_W, SHELF_BIG_H);
    if (disc_open(&d, disc->path) < 0)
        return;
    if ((buf = disc_read(&d, "UMD_VIDEO/PIC1.PNG", &size, 2 * 1024 * 1024)) != NULL) {
        m->bg = make_background(buf, size);
        free(buf);
    }
    /* The title image the PSP draws over PIC1 beside the selected disc: Bobbito's Basics to
       Boogie and Jamie Foxx keep their titles out of PIC1 and put them here. */
    if ((buf = disc_read(&d, "UMD_VIDEO/PIC0.PNG", &size, 512 * 1024)) != NULL) {
        m->pic0 = decode_png(buf, size, &m->pic0_w, &m->pic0_h, 512, 512);
        free(buf);
    }
    if ((buf = disc_read(&d, "UMD_VIDEO/ICON1.PMF", &size, PMF_MAX)) != NULL) {
        m->icon = parse_icon(buf, size);
        free(buf);
    }
    m->snd = disc_read(&d, "UMD_VIDEO/SND0.AT3", &m->snd_size, SND_MAX);
    plat_iso_close(&d.src);
}

/* Prepares the selected disc first, then its neighbours, into free or least recently used slots. */
static void worker_main(void *user)
{
    static const int order[5] = {0, 1, -1, 2, -2};
    library *l = (library *)user;

    plat_mutex_lock(l->cache_lock);
    while (!l->worker_quit) {
        int target = -1, slot = -1;
        media ready;

        if (!l->worker_paused)
            for (int k = 0; k < 5 && target < 0; k++) {
                int d = l->want_sel + order[k];
                if (d >= 0 && d < l->count && !find_media(l, d))
                    target = d;
            }
        if (target >= 0) {
            for (int i = 0; i < CACHE_N; i++) {
                media *m = &l->cache[i];
                int near = m->disc >= l->want_sel - 2 && m->disc <= l->want_sel + 2;
                if (m->state == MEDIA_EMPTY) {
                    slot = i;
                    break;
                }
                if (m->state == MEDIA_READY && m->disc != l->loaded && m->disc != l->bg_pin && !near &&
                    (slot < 0 || m->used < l->cache[slot].used))
                    slot = i;
            }
        }
        if (slot < 0) {
            plat_cond_wait(l->cache_cond, l->cache_lock);
            continue;
        }
        free_media(&l->cache[slot]);
        l->cache[slot].disc = target;
        l->cache[slot].state = MEDIA_LOADING;
        plat_mutex_unlock(l->cache_lock);

        memset(&ready, 0, sizeof(ready));
        read_media(&l->discs[target], &ready);

        plat_mutex_lock(l->cache_lock);
        ready.disc = target;
        ready.state = MEDIA_READY;
        ready.used = ++l->use_clock;
        l->cache[slot] = ready;
    }
    plat_mutex_unlock(l->cache_lock);
}

static void tell_worker(library *l, int paused)
{
    plat_mutex_lock(l->cache_lock);
    l->want_sel = l->sel;
    l->worker_paused = paused;
    l->sheet_stop = paused || l->sheet_quit;
    plat_cond_broadcast(l->cache_cond);
    plat_mutex_unlock(l->cache_lock);
}

/* Reads each disc's resume point for the progress mark under its icon. */
static void load_resume_mark(lib_disc *d)
{
    resume_entry e;
    static char state[4096];

    d->resume_disc = settings_get()->start_disc || d->interactive;
    if (d->resume_disc) {
        d->has_resume = disc_state_load(file_name(d->path), state, sizeof(state)) == 0;
        return;
    }
    d->has_resume = resume_load(file_name(d->path), &e) == 0 && e.pos > e.start && e.pos > 0;
    d->resume_pos = e.pos;
    d->resume_start = e.start;
    d->resume_end = e.end;
}

static void load_resume_marks(library *l)
{
    for (int i = 0; i < l->count; i++)
        load_resume_mark(&l->discs[i]);
    l->marks_stale = 0;
}

/* Circle's details page. It lays out like the shelf, with the poster at the left, in any view. */
static int details_open;

static int list_on(void)
{
    return settings_get()->lib_view == LIB_LIST && !details_open;
}

static int shelf_on(void)
{
    return settings_get()->lib_view == LIB_SHELF || details_open;
}

/* The discs of the tab on screen that have been read: tab_lo to tab_hi - 1. */
static int tab_lo(const library *l)
{
    return l->tab ? l->split : 0;
}

static int tab_hi(const library *l)
{
    return l->tab || l->count < l->split ? l->count : l->split;
}

/* The disc in the list view's top row. The list stays put while the selection moves inside it,
   so only two rows change, and moves when the selection nears its top or bottom. */
static int list_first(library *l)
{
    int lo = tab_lo(l), hi = tab_hi(l), top = l->list_top;

    if (l->sel < top + LIST_MARGIN)
        top = l->sel - LIST_MARGIN;
    if (l->sel >= top + LIST_ROWS - LIST_MARGIN)
        top = l->sel - LIST_ROWS + LIST_MARGIN + 1;
    if (top > hi - LIST_ROWS)
        top = hi - LIST_ROWS;
    if (top < lo)
        top = lo;
    l->list_top = top;
    return top;
}

/* 1 while the shelf's row is still gliding to the selection. */
static int shelf_moving(const library *l)
{
    float d = (float)l->sel - l->shelf_pos;

    return shelf_on() && !details_open && (d > 0.02f || d < -0.02f);
}

/* A tab's name and place over the list. Returns 0 if it has no discs. */
static int tab_label(const library *l, int t, char *out, size_t size, int *x, int *w)
{
    int n = t ? l->scan_total - l->split : l->split;

    snprintf(out, size, "%s  %d", (t == 0) == tv_sorted_first ? "TV Shows" : "Movies", n);
    *w = canvas_text_width(out, 2) + 24;
    *x = shelf_on() ? SHELF_X - 6 : LIST_X - 6;
    if (t) {
        char first[32];
        int fx, fw;
        if (tab_label(l, 0, first, sizeof(first), &fx, &fw))
            *x = fx + fw + 8;
    }
    return n > 0;
}

/* As much of s as fits in max_w pixels, ending with "..." when it was cut. out holds at least
   size bytes, and size is more than 3. */
static void fit_title(char *out, int size, const char *s, int scale, int max_w)
{
    canvas_text_fit(out, size, s, scale, max_w);
    if (!s[strlen(out)])
        return;
    canvas_text_fit(out, size - 3, s, scale, max_w - canvas_text_width("...", scale));
    strcat(out, "...");
}

static void format_time(char *out, size_t size, uint64_t ticks)
{
    unsigned s = (unsigned)(ticks / 90000);
    snprintf(out, size, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
}

/* The title on up to two lines of max_w pixels, broken at a space; a second line that is still
   too long ends with "...". Returns the number of lines. */
static int wrap_title(const char *s, char *l1, char *l2, int size, int max_w)
{
    const char *rest;
    int n;

    canvas_text_fit(l1, size, s, 2, max_w);
    l2[0] = 0;
    n = (int)strlen(l1);
    if (!s[n])
        return 1;
    if (s[n] != ' ') {                       /* back up to the last space, if the line keeps a third */
        char *sp = strrchr(l1, ' ');
        if (sp && canvas_text_width(sp, 2) * 3 <= canvas_text_width(l1, 2) * 2) {
            *sp = 0;
            n = (int)(sp - l1);
        }
    }
    l1[n] = 0;
    while (n > 0 && l1[n - 1] == ' ')
        l1[--n] = 0;
    for (rest = s + n; *rest == ' '; rest++)
        ;
    canvas_text_fit(l2, size, rest, 2, max_w);
    if (rest[strlen(l2)]) {
        canvas_text_fit(l2, size - 3, rest, 2, max_w - canvas_text_width("...", 2));
        n = (int)strlen(l2);
        while (n > 0 && l2[n - 1] == ' ')
            l2[--n] = 0;
        strcat(l2, "...");
    }
    return 2;
}

/* Right of the selected icon, while △ has it on: the disc's title and, for a disc with a saved
   position, how far it was played with × to resume and □ to start over. */
static void draw_info(library *l)
{
    const lib_disc *d = &l->discs[l->sel];
    canvas *c = &l->ui;
    int x = PANEL_X + 16, y = BIG_Y, tx = x + 2 * ICON_SIZE + 8, w = 300;
    char title[96], title2[96], at[16], total[16], line[48];
    int bw = osd_button_width(ICON_BUTTON_CROSS, 2), lines;
    /* A disc with no icon and no PIC0 (Monster Hunter Portable Rookie's Guide) would show only
       the backdrop: its title stands beside the icon, as the Select panel shows it. */
    int bare = !d->icon0 && !l->icon && !l->pic0 && !l->settings_open;

    if (list_on() || shelf_on() || (!l->show_info && !bare))
        return;
    if (!l->show_info) {
        lines = wrap_title(d->title, title, title2, sizeof(title), PANEL_W - 32);
        canvas_round_darken(c, PANEL_X, BIG_Y, PANEL_W, 2 * ICON_H, BIG_R, 90);
        canvas_text(c, x, y + ICON_H - 8 - (lines - 1) * 11, 2, RGBA(255, 255, 255, 255), title);
        if (lines > 1)
            canvas_text(c, x, y + ICON_H - 8 - (lines - 1) * 11 + 22, 2, RGBA(255, 255, 255, 255), title2);
        return;
    }
    if (d->no_movie)
        canvas_text(&l->ui, x, y + 2 * ICON_H - 16, 1, RGBA(255, 120, 100, 255),
                    "No movie in this image: PLAYLIST.UMD is missing");
    else if (d->incomplete)
        canvas_text(&l->ui, x, y + 2 * ICON_H - 16, 1, RGBA(255, 120, 100, 255),
                    "Incomplete image: some videos are missing");
    if (osd_button_width(ICON_BUTTON_SQUARE, 2) > bw)
        bw = osd_button_width(ICON_BUTTON_SQUARE, 2);
    tx = x + bw + 8;
    lines = wrap_title(d->title, title, title2, sizeof(title), PANEL_W - 32);
    canvas_round_darken(c, PANEL_X, BIG_Y, PANEL_W, 2 * ICON_H, BIG_R, 90);   /* as tall as the icon */
    if (!d->has_resume) {
        /* centred: lines 22 pixels apart, a little more before the UMD-PG line */
        int ty = y + ICON_H - 8 - (lines - 1) * 11 - (d->interactive ? 14 : 0);
        canvas_text(c, x, ty, 2, RGBA(255, 255, 255, 255), title);
        if (lines > 1)
            canvas_text(c, x, ty + 22, 2, RGBA(255, 255, 255, 255), title2);
        if (d->interactive)
            canvas_text(c, x, ty + lines * 22 + 6, 2, TEXT_DIM, "Interactive movie (UMD-PG)");
        return;
    }
    format_time(at, sizeof(at), d->resume_pos - d->resume_start);
    canvas_text(c, x, lines > 1 ? y + 4 : y + 18, 2, RGBA(255, 255, 255, 255), title);
    if (lines > 1)
        canvas_text(c, x, y + 26, 2, RGBA(255, 255, 255, 255), title2);
    osd_button(c, x, y + 46, 2, ICON_BUTTON_CROSS);
    if (d->resume_disc) {
        snprintf(line, sizeof(line), "Continue where you left off");
    } else if (d->resume_end > d->resume_start) {
        uint64_t len = d->resume_end - d->resume_start, done = d->resume_pos - d->resume_start;
        int fill = done >= len ? w : (int)(done * (uint64_t)w / len);
        format_time(total, sizeof(total), len);
        snprintf(line, sizeof(line), "Resume at %s of %s", at, total);
        canvas_rect(c, tx, y + 84, w, 6, RGBA(70, 80, 100, 255));
        canvas_rect(c, tx, y + 84, fill, 6, RGBA(255, 255, 255, 230));
    } else {
        snprintf(line, sizeof(line), "Resume at %s", at);
    }
    /* A wide key name ("Space") leaves less room: the short form, the bar shows the rest. */
    if (canvas_text_width(line, 2) > PANEL_X + PANEL_W - 16 - tx) {
        if (d->resume_disc)
            snprintf(line, sizeof(line), "Continue");
        else
            snprintf(line, sizeof(line), "Resume at %s", at);
    }
    canvas_text(c, tx, y + 54, 2, RGBA(255, 255, 255, 255), line);
    osd_button(c, x, y + 106, 2, ICON_BUTTON_SQUARE);
    canvas_text(c, tx, y + 114, 2, TEXT_DIM, "Play from Beginning");
}

/* ---- the disc on screen ---- */

static void drop_frames(library *l)
{
    for (int i = 0; i < l->frame_count; i++)
        plat_frame_release(l->frames[i].frame);
    l->frame_count = 0;
    plat_frame_release(l->shown);
    l->shown = NULL;
}

/* Shows the selected disc's big poster if the worker has made it. Unlike the rest of the media
   this needs no settling, so stepping to a neighbour is sharp at once. The worker keeps the entries
   near its want_sel, so call this after tell_worker. */
static void use_big_poster(library *l)
{
    media *m;

    plat_mutex_lock(l->cache_lock);
    m = find_media(l, l->sel);
    if (m && m->state == MEDIA_READY && m->big_poster) {
        if (!m->big_poster_id)
            m->big_poster_id = lib_id();
        l->big_poster = m->big_poster;
        l->big_poster_id = m->big_poster_id;
        l->ui_dirty = 1;
    }
    plat_mutex_unlock(l->cache_lock);
}

/* Stops showing the current disc's media; the decoder and sound port stay open. */
static void clear_media(library *l)
{
    drop_frames(l);
    l->icon = NULL;
    l->pic0 = NULL;
    l->big_poster = NULL;
    plat_mutex_lock(l->lock);
    l->snd = NULL;
    plat_mutex_unlock(l->lock);
    /* The background stays until the next disc's is ready, instead of flashing to the backdrop
       on every step: the worker keeps this entry meanwhile. */
    plat_mutex_lock(l->cache_lock);
    if (l->bg && l->loaded >= 0)
        l->bg_pin = l->loaded;
    l->loaded = -1;
    plat_mutex_unlock(l->cache_lock);
}

/* Closes the decoder and the sound port: before a movie plays or on close. */
static void release_devices(library *l)
{
    clear_media(l);
    l->bg = NULL;
    plat_mutex_lock(l->cache_lock);
    l->bg_pin = -1;
    plat_mutex_unlock(l->cache_lock);
    if (l->video) {
        plat_video_close(l->video);
        l->video = NULL;
    }
    if (l->audio_open) {
        plat_audio_close();
        l->audio_open = 0;
    }
}

/* Starts the selected disc's media if the worker has it ready. All the heavy work is done,
   so this is quick: point at the background, flush the decoder, and switch the sound. */
static int load_media(library *l)
{
    media *m;
    const icon_video *icon;
    uint64_t t0 = plat_time_us();

    plat_mutex_lock(l->cache_lock);
    m = find_media(l, l->sel);
    if (!m || m->state != MEDIA_READY) {
        plat_mutex_unlock(l->cache_lock);
        return 0;
    }
    l->loaded = l->sel;              /* from now on the worker keeps this entry */
    l->bg_pin = -1;
    m->used = ++l->use_clock;
    plat_mutex_unlock(l->cache_lock);

    l->ui_dirty = 1;
    use_big_poster(l);
    l->bg = m->bg;
    l->pic0 = m->pic0;
    /* The GPU's copies stay with the cache entry: back on a disc, nothing is copied again. */
    if (m->bg && !m->bg_id)
        m->bg_id = lib_id();
    if (m->pic0 && !m->pic0_id)
        m->pic0_id = lib_id();
    l->bg_id = m->bg_id;
    l->pic0_id = m->pic0_id;
    l->pic0_w = m->pic0_w;
    l->pic0_h = m->pic0_h;
    icon = m->icon;
    if (icon && l->video && (l->video_w != icon->sps.width || l->video_h != icon->sps.height)) {
        plat_video_close(l->video);
        l->video = NULL;
    }
    if (icon && !l->video) {
        plat_video_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.width = icon->sps.width;
        cfg.height = icon->sps.height;
        cfg.sps = icon->sps_nal;
        cfg.sps_size = icon->sps_size;
        cfg.pps = icon->pps_nal;
        cfg.pps_size = icon->pps_size;
        l->video = plat_video_open(&cfg);
        l->video_w = icon->sps.width;
        l->video_h = icon->sps.height;
    } else if (icon) {
        plat_video_flush(l->video);
    }
    l->icon = l->video ? icon : NULL;
    l->au_next = 0;
    l->loop_us = plat_time_us();
    l->last_t = 0;
    l->clk_raw = 0;

    plat_mutex_lock(l->lock);
    set_sound(l, m->snd, m->snd_size);
    plat_mutex_unlock(l->lock);
    if (l->snd && l->audio_open && l->audio_rate != l->snd_rate) {
        plat_audio_close();
        l->audio_open = 0;
    }
    if (l->snd && !l->audio_open && plat_audio_open(l->snd_rate, sound_fill, l) == 0) {
        l->audio_open = 1;
        l->audio_rate = l->snd_rate;
    }
    l->snd_start = l->audio_open ? plat_audio_played() : 0;
    plat_log("library: disc %d shown in %.1f ms\n", l->sel, (plat_time_us() - t0) / 1000.0);
    return 1;
}

/* ---- drawing ---- */

/* The disc's PIC1, or the app's backdrop for discs without one. */
static void draw_background(library *l)
{
    const uint8_t *bg = l->bg ? l->bg : backdrop_rgba();

    if (bg) {
        memcpy(l->ui.rgba, bg, UI_W * UI_H * 4);
        return;
    }
    for (int y = 0; y < UI_H; y++)
        canvas_rect(&l->ui, 0, y, UI_W, 1, RGBA(12 + y * 20 / UI_H, 18 + y * 30 / UI_H, 40 + y * 60 / UI_H, 255));
}

/* Fills the area outside the selected icon's rounded corners. PIC1 backgrounds often have a
   placeholder box exactly where the icon goes, so the corners take the background mirrored
   from just above or below the icon rather than what is behind them. */
static void fill_icon_corners(library *l)
{
    const uint8_t *bg = l->bg ? l->bg : backdrop_rgba();
    int h = 2 * ICON_H;

    if (!bg)
        return;
    for (int yy = 0; yy < h; yy++) {
        int in = canvas_round_inset(BIG_R, h, yy);
        int src = yy < h / 2 ? BIG_Y - 1 - yy : BIG_Y + h + (h - 1 - yy);
        if (!in || src < 0 || src >= UI_H)
            continue;
        for (int side = 0; side < 2; side++) {
            int x = side ? BIG_X + 2 * ICON_W - in : BIG_X;
            memcpy(l->ui.rgba + ((size_t)(BIG_Y + yy) * UI_W + x) * 4, bg + ((size_t)src * UI_W + x) * 4, (size_t)in * 4);
        }
    }
}

/* ---- the settings panel (△) ---- */

enum { SET_START, SET_SCREEN, SET_AUDIO, SET_SUBS, SET_SHEETS, SET_LOGS, SET_SKIP, SET_RESCAN, SET_ROWS };

static int lang_index(const char *code)
{
    for (int i = 0; i < LANG_CHOICES; i++)
        if (strcmp(lang_choices[i], code) == 0)
            return i;
    return -1;
}

static void settings_input(library *l, uint32_t pressed)
{
    app_settings *s = settings_get();
    int dir = (pressed & PLAT_BTN_RIGHT) ? 1 : (pressed & PLAT_BTN_LEFT) ? -1 : 0;

    if (pressed & (PLAT_BTN_CIRCLE | PLAT_BTN_TRIANGLE)) {
        l->settings_open = 0;
        l->ui_dirty = 1;
        l->marks_stale = 1;           /* "Start with" decides which resume point a disc shows */
        settings_save();
        return;
    }
    if (pressed & PLAT_BTN_UP)
        l->settings_row = (l->settings_row + SET_ROWS - 1) % SET_ROWS;
    if (pressed & PLAT_BTN_DOWN)
        l->settings_row = (l->settings_row + 1) % SET_ROWS;
    if ((pressed & PLAT_BTN_CROSS) && l->settings_row == SET_RESCAN) {
        l->settings_open = 0;
        settings_save();
        l->rescan = 1;
        return;
    }
    if (dir && l->settings_row == SET_START) {
        s->start_disc = !s->start_disc;
    } else if (dir && l->settings_row == SET_SCREEN) {
        s->screen = (s->screen + dir + SCREEN_MODES) % SCREEN_MODES;
    } else if (dir && l->settings_row == SET_AUDIO) {
        int i = lang_index(s->audio);
        i = i < 0 ? 0 : (i + dir + LANG_CHOICES) % LANG_CHOICES;
        snprintf(s->audio, sizeof(s->audio), "%s", lang_choices[i]);
    } else if (dir && l->settings_row == SET_SUBS) {
        /* Off, System, then each language. */
        int n = LANG_CHOICES + 2, i = strcmp(s->subs, SUBS_OFF) == 0 ? 0 : strcmp(s->subs, SUBS_SYSTEM) == 0 ? 1
                                                                       : 2 + lang_index(s->subs);
        if (i < 0 || i >= n)
            i = 1;
        i = (i + dir + n) % n;
        snprintf(s->subs, sizeof(s->subs), "%s", i == 0 ? SUBS_OFF : i == 1 ? SUBS_SYSTEM : lang_choices[i - 2]);
    } else if (dir && l->settings_row == SET_SHEETS) {
        s->info_sheets = !s->info_sheets;
        l->sheet_finished = 0;        /* on again: look for discs added since */
    } else if (dir && l->settings_row == SET_LOGS) {
        s->logs = !s->logs;
        plat_set_log_file(s->logs);
    } else if (dir && l->settings_row == SET_SKIP) {
        s->skip_clips = !s->skip_clips;
    }
    if (pressed)
        l->ui_dirty = 1;
}

static void draw_settings(library *l)
{
    const app_settings *s = settings_get();
    osd_view v;
    char sys[3];

    if (!l->settings_open)
        return;
    memset(&v, 0, sizeof(v));
    v.menu_open = 1;
    v.row_count = SET_ROWS;
    v.menu_row = l->settings_row;
    plat_language(sys);
    snprintf(v.header, sizeof(v.header), "SETTINGS");
    snprintf(v.subheader, sizeof(v.subheader), "For discs not played yet. System: %s", lang_name(sys));
    v.rows[SET_START].icon = ICON_PLAY;
    snprintf(v.rows[SET_START].label, sizeof(v.rows[0].label), "Start with");
    snprintf(v.rows[SET_START].value, sizeof(v.rows[0].value), "%s", s->start_disc ? "Disc" : "Movie");
    v.rows[SET_SCREEN].icon = ICON_SCREEN;
    snprintf(v.rows[SET_SCREEN].label, sizeof(v.rows[0].label), "Screen");
    snprintf(v.rows[SET_SCREEN].value, sizeof(v.rows[0].value), "%s", screen_mode_name((screen_mode)s->screen));
    v.rows[SET_AUDIO].icon = ICON_AUDIO;
    snprintf(v.rows[SET_AUDIO].label, sizeof(v.rows[0].label), "Audio");
    snprintf(v.rows[SET_AUDIO].value, sizeof(v.rows[0].value), "%s", lang_name(s->audio));
    v.rows[SET_SUBS].icon = ICON_SUBTITLES;
    snprintf(v.rows[SET_SUBS].label, sizeof(v.rows[0].label), "Subtitles");
    snprintf(v.rows[SET_SUBS].value, sizeof(v.rows[0].value), "%s",
             strcmp(s->subs, SUBS_OFF) == 0 ? "Off" : strcmp(s->subs, SUBS_SYSTEM) == 0 ? "System" : lang_name(s->subs));
    v.rows[SET_SHEETS].icon = ICON_CHAPTERS;
    snprintf(v.rows[SET_SHEETS].label, sizeof(v.rows[0].label), "Info sheets");
    snprintf(v.rows[SET_SHEETS].value, sizeof(v.rows[0].value), "%s", s->info_sheets ? "On" : "Off");
    v.rows[SET_LOGS].icon = ICON_CHAPTERS;
    snprintf(v.rows[SET_LOGS].label, sizeof(v.rows[0].label), "Logs");
    snprintf(v.rows[SET_LOGS].value, sizeof(v.rows[0].value), "%s", s->logs ? "On" : "Off");
    v.rows[SET_SKIP].icon = ICON_FORWARD;
    snprintf(v.rows[SET_SKIP].label, sizeof(v.rows[0].label), "Skip clips");
    snprintf(v.rows[SET_SKIP].value, sizeof(v.rows[0].value), "%s", s->skip_clips ? "On" : "Off");
    v.rows[SET_RESCAN].icon = ICON_FORWARD;
    snprintf(v.rows[SET_RESCAN].label, sizeof(v.rows[0].label), "Library");
    snprintf(v.rows[SET_RESCAN].value, sizeof(v.rows[0].value), "Rescan all discs");
    if (l->settings_row == SET_RESCAN)
        snprintf(v.cross_hint, sizeof(v.cross_hint), "Rescan");
    osd_draw_menu(&v, &l->ui);
}

/* "UMD Player" at the top left, with "by xesf - v0.6.8" below it in small text. */
static void draw_app_name(canvas *c, int y)
{
    canvas_text(c, 20, y, 2, RGBA(255, 255, 255, 220), "UMD Player+");
    canvas_text(c, 20, y + 22, 1, TEXT_DIM, "by xesf - v" UMDPLAYER_VERSION);
}

/* At the bottom right: the buttons that open the settings and the manual. */
static void draw_hints(canvas *c, int y)
{
    const char *key = plat_keyboard_ui() ? "F1" : "START", *lr = plat_keyboard_ui() ? "Q/E" : "L/R";
    const char *sel = plat_keyboard_ui() ? "Tab" : "SELECT";
    int w = osd_button_width(ICON_BUTTON_TRIANGLE, 1) + 6 + canvas_text_width("Settings", 1);
    int mw = osd_key_width(key) + 6 + canvas_text_width("Manual", 1);
    int lw = osd_key_width(lr) + 6 + canvas_text_width("Letter", 1);
    int vw = osd_key_width(sel) + 6 + canvas_text_width("View", 1);
    const char *more = details_open ? "Back" : "Details";
    int dw = osd_button_width(ICON_BUTTON_CIRCLE, 1) + 6 + canvas_text_width(more, 1);
    int x = UI_W - 20 - w;

    osd_draw_hint(c, x, y - 4, ICON_BUTTON_TRIANGLE, "Settings");
    osd_draw_key_hint(c, x - 24 - mw, y - 4, key, "Manual");
    osd_draw_key_hint(c, x - 48 - mw - lw, y - 4, lr, "Letter");
    osd_draw_key_hint(c, x - 72 - mw - lw - vw, y - 4, sel, "View");
    osd_draw_hint(c, x - 96 - mw - lw - vw - dw, y - 4, ICON_BUTTON_CIRCLE, more);
}

/* The poster view's titles: the selected disc's under the picture on the right, unless the
   info panel already shows it, and each other disc's beside its icon. */
static void draw_titles(library *l)
{
    canvas *c = &l->ui;
    const lib_disc *d = &l->discs[l->sel];
    char t[100];
    int bare = !d->icon0 && !l->icon && !l->pic0;

    if (!l->show_info && !bare) {
        int w;
        fit_title(t, sizeof(t), d->title, 2, PANEL_W - 24);
        w = canvas_text_width(t, 2);
        canvas_round_darken(c, PANEL_X, TITLE_Y, w + 24, 30, 8, 110);
        canvas_text(c, PANEL_X + 12, TITLE_Y + 7, 2, RGBA(255, 255, 255, 255), t);
    }
    for (int k = -1; k <= 2; k++) {
        int i = l->sel + k, y = k < 0 ? BIG_Y - 100 : BIG_Y + 2 * ICON_H + 20 + (k - 1) * 90, w;
        if (k == 0 || i < tab_lo(l) || i >= tab_hi(l))
            continue;
        fit_title(t, sizeof(t), l->discs[i].title, 1, NEIGHBOUR_W);
        w = canvas_text_width(t, 1);
        canvas_round_darken(c, NEIGHBOUR_X - 6, y + ICON_H / 2 - 10, w + 12, 20, 6, 110);
        canvas_text(c, NEIGHBOUR_X, y + ICON_H / 2 - 4, 1, RGBA(255, 255, 255, 255), t);
    }
}

/* One line about the disc: where it would resume, or what is wrong with it. Empty if nothing. */
static void disc_status(const lib_disc *d, char *line, size_t size)
{
    char at[16], total[16];

    line[0] = 0;
    if (d->no_movie) {
        snprintf(line, size, "No movie in this image");
    } else if (d->has_resume && d->resume_disc) {
        snprintf(line, size, "Continue where you left off");
    } else if (d->has_resume) {
        format_time(at, sizeof(at), d->resume_pos - d->resume_start);
        if (d->resume_end > d->resume_start) {
            format_time(total, sizeof(total), d->resume_end - d->resume_start);
            snprintf(line, size, "Resume at %s of %s", at, total);
        } else {
            snprintf(line, size, "Resume at %s", at);
        }
    } else if (d->interactive) {
        snprintf(line, size, "Interactive movie (UMD-PG)");
    } else if (d->incomplete) {
        snprintf(line, size, "Incomplete image");
    }
}

/* s on lines of max_w pixels broken at spaces, step pixels apart, leaving out the first skip
   lines and drawing at most lines of them. Returns 1 if some of the text did not fit. */
static int draw_wrapped(canvas *c, int x, int y, int scale, uint32_t color, const char *s, int max_w,
                        int skip, int lines, int step)
{
    char line[160];

    while (*s == ' ')
        s++;
    while (*s && lines > 0) {
        int n;
        canvas_text_fit(line, sizeof(line), s, scale, max_w);
        n = (int)strlen(line);
        if (n == 0)
            break;
        if (s[n] && s[n] != ' ') {
            char *sp = strrchr(line, ' ');
            if (sp && sp > line) {
                *sp = 0;
                n = (int)(sp - line);
            }
        }
        if (skip > 0) {
            skip--;
        } else {
            canvas_text(c, x, y, scale, color, line);
            y += step;
            lines--;
        }
        s += n;
        while (*s == ' ')
            s++;
    }
    return *s != 0;
}

/* The details page, right of the poster: everything TMDb said about the disc. Up and Down scroll
   a description too long for the page; a disc with no details says how to get them. */
static void draw_details(library *l)
{
    canvas *c = &l->ui;
    const lib_disc *d = &l->discs[l->sel];
    const lib_details *info = d->details;
    int x = SHELF_X + 4, w = UI_W - 24 - x, y = SHELF_BIG_Y;
    char l1[96], l2[96], line[64];
    int lines;

    canvas_round_darken(c, SHELF_X - 6, SHELF_BIG_Y - 8, UI_W - SHELF_X - 8, 370, 12, 110);
    lines = wrap_title(info && info->name[0] ? info->name : d->title, l1, l2, sizeof(l1), w);
    canvas_text(c, x, y, 2, RGBA(255, 255, 255, 255), l1);
    y += 22;
    if (lines > 1) {
        canvas_text(c, x, y, 2, RGBA(255, 255, 255, 255), l2);
        y += 22;
    }
    y += 2;
    l->details_more = 0;
    if (!info) {
        canvas_text(c, x, y + 8, 1, TEXT_DIM, "No details for this disc yet.");
        canvas_text(c, x, y + 24, 1, TEXT_DIM, "Run Get Posters on your PC with a TMDB key to add them.");
        return;
    }
    if (strcmp(info->name, d->title) != 0) {
        fit_title(l1, sizeof(l1), d->title, 1, w);
        canvas_text(c, x, y, 1, TEXT_DIM, l1);
        y += 13;
    }
    if (info->facts[0]) {
        fit_title(l1, sizeof(l1), info->facts, 1, w);
        canvas_text(c, x, y, 1, RGBA(255, 255, 255, 255), l1);
        y += 13;
    }
    if (info->dates[0]) {
        canvas_text(c, x, y, 1, TEXT_DIM, info->dates);
        y += 13;
    }
    if (info->tagline[0]) {
        fit_title(l1, sizeof(l1), info->tagline, 1, w);
        canvas_text(c, x, y, 1, RGBA(120, 190, 255, 255), l1);
        y += 13;
    }
    y += 6;
    l->details_more = draw_wrapped(c, x, y, 2, RGBA(255, 255, 255, 255), info->about, w, l->details_scroll,
                                   (DETAILS_TEXT_END - y) / 22, 22);
    if (l->details_more || l->details_scroll > 0) {
        snprintf(line, sizeof(line), "Up/Down: more");
        canvas_text(c, UI_W - 24 - canvas_text_width(line, 1), DETAILS_TEXT_END + 2, 1, RGBA(120, 190, 255, 255), line);
    }
    y = DETAILS_TEXT_END + 14;
    if (info->credit1[0]) {
        fit_title(l1, sizeof(l1), info->credit1, 1, w);
        canvas_text(c, x, y, 1, TEXT_DIM, l1);
        y += 13;
    }
    if (info->credit2[0])
        draw_wrapped(c, x, y, 1, TEXT_DIM, info->credit2, w, 0, 2, 12);
    canvas_text(c, x, SHELF_BIG_Y + 348, 1, RGBA(120, 130, 150, 255),
                "This product uses the TMDB API but is not endorsed or certified by TMDB.");
}

/* The tabs over the list, and how to change them. */
static void draw_tabs(library *l)
{
    canvas *c = &l->ui;
    char s[32];
    int x = 0, w = 0, shown = 0;

    for (int t = 0; t < 2; t++) {
        if (!tab_label(l, t, s, sizeof(s), &x, &w))
            continue;
        if (t == l->tab)
            canvas_round_rect(c, x, TAB_Y, w, TAB_H, 8, RGBA(55, 95, 180, 255));
        else
            canvas_round_darken(c, x, TAB_Y, w, TAB_H, 8, 120);
        canvas_text(c, x + 12, TAB_Y + 6, 2, t == l->tab ? RGBA(255, 255, 255, 255) : TEXT_DIM, s);
        shown++;
    }
    if (shown > 1)
        canvas_text(c, x + w + 12, TAB_Y + 10, 1, TEXT_DIM, shelf_on() ? "Up/Down: switch" : "Left/Right: switch");
}

/* The shelf view's text: the selected disc's title, facts and status over the row, a caption under
   each disc in the row with a dot on a disc with a saved position, and the start of the
   description below. The pictures are sprites. */
static void draw_shelf(library *l)
{
    canvas *c = &l->ui;
    const lib_disc *d = &l->discs[l->sel];
    char t[100], l1[96], l2[96], line[64];
    int lines = wrap_title(d->title, l1, l2, sizeof(l1), SHELF_TILES * SHELF_STEP - 24);
    int top = SHELF_BIG_Y - 20;

    canvas_round_darken(c, SHELF_X - 6, top - 8, SHELF_TILES * SHELF_STEP, 84, 10, 90);
    canvas_text(c, SHELF_X + 4, top, 2, RGBA(255, 255, 255, 255), l1);
    if (lines > 1)
        canvas_text(c, SHELF_X + 4, top + 22, 2, RGBA(255, 255, 255, 255), l2);
    if (d->details && d->details->facts[0]) {
        fit_title(t, sizeof(t), d->details->facts, 1, SHELF_TILES * SHELF_STEP - 24);
        canvas_text(c, SHELF_X + 4, top + 45, 1, RGBA(255, 255, 255, 255), t);
    }
    disc_status(d, line, sizeof(line));
    if (line[0])
        canvas_text(c, SHELF_X + 4, top + 60, 1, d->no_movie || d->incomplete ? RGBA(255, 120, 100, 255) : TEXT_DIM, line);
    if (shelf_moving(l))                     /* captions wait until the row rests */
        return;
    if (d->details && d->details->about[0]) {
        canvas_round_darken(c, SHELF_X - 6, SHELF_Y + SHELF_H + 23, SHELF_TILES * SHELF_STEP, 46, 8, 90);
        draw_wrapped(c, SHELF_X + 4, SHELF_Y + SHELF_H + 29, 1, RGBA(255, 255, 255, 255), d->details->about,
                     SHELF_TILES * SHELF_STEP - 24, 0, 3, 12);
    }
    for (int k = 1; k <= SHELF_TILES && l->sel + k < tab_hi(l); k++) {
        const lib_disc *e = &l->discs[l->sel + k];
        int x = SHELF_X + (k - 1) * SHELF_STEP, w;
        fit_title(t, sizeof(t), e->title, 1, SHELF_W);
        w = canvas_text_width(t, 1);
        canvas_round_darken(c, x - 2, SHELF_Y + SHELF_H + 4, w + 6, 16, 4, 110);
        canvas_text(c, x, SHELF_Y + SHELF_H + 8, 1, RGBA(255, 255, 255, 255), t);
        if (e->has_resume)
            canvas_round_rect(c, x + SHELF_W - 16, SHELF_Y + 6, 10, 10, 5, RGBA(120, 190, 255, 255));
    }
}

/* The list view: a page of titles beside the selected disc's icon, with its full title and
   resume point under the icon. A dot marks a disc with a saved position. */
static void draw_list(library *l)
{
    canvas *c = &l->ui;
    const lib_disc *d = &l->discs[l->sel];
    char t[100], l1[96], l2[96], line[64];
    int first = list_first(l), lines, y = BIG_Y + 2 * ICON_H + 8;

    canvas_round_darken(c, LIST_X - 6, LIST_Y - 6, LIST_W + 12, LIST_ROWS * LIST_ROW + 12, 12, 90);
    for (int r = 0; r < LIST_ROWS && first + r < tab_hi(l); r++) {
        int i = first + r, ry = LIST_Y + r * LIST_ROW;
        const lib_disc *e = &l->discs[i];
        fit_title(t, sizeof(t), e->title, 2, LIST_W - 40);
        if (i == l->sel)
            canvas_round_rect(c, LIST_X, ry, LIST_W, LIST_ROW, 6, RGBA(55, 95, 180, 255));
        canvas_text(c, LIST_X + 8, ry + 6, 2, e->no_movie ? RGBA(255, 120, 100, 255) : i == l->sel ? RGBA(255, 255, 255, 255) : TEXT_DIM, t);
        if (e->has_resume)
            canvas_round_rect(c, LIST_X + LIST_W - 20, ry + 9, 10, 10, 5, RGBA(120, 190, 255, 255));
    }

    lines = wrap_title(d->title, l1, l2, sizeof(l1), 2 * ICON_W - 16);
    canvas_round_darken(c, BIG_X, y, 2 * ICON_W, 84, 10, 90);
    canvas_text(c, BIG_X + 8, y + 6, 2, RGBA(255, 255, 255, 255), l1);
    if (lines > 1)
        canvas_text(c, BIG_X + 8, y + 28, 2, RGBA(255, 255, 255, 255), l2);
    disc_status(d, line, sizeof(line));
    if (line[0])
        canvas_text(c, BIG_X + 8, y + 50, 1, d->no_movie || d->incomplete ? RGBA(255, 120, 100, 255) : TEXT_DIM, line);
    if (d->has_resume && !d->no_movie) {
        canvas_text(c, BIG_X + 8, y + 66, 1, TEXT_DIM, "Square: from the beginning");
    } else if (d->details && d->details->facts[0]) {
        fit_title(t, sizeof(t), d->details->facts, 1, 2 * ICON_W - 16);
        canvas_text(c, BIG_X + 8, line[0] ? y + 66 : y + 50, 1, TEXT_DIM, t);
    }
}

/* A disc without ICON0 or ICON1 (Monster Hunter Portable Rookie's Guide, which shows none on a
   PSP either): the app's backdrop as its icon, or a plain box if that cannot be decoded. */
static void draw_no_icon(canvas *c, int x, int y, int w, int h)
{
    const uint8_t *bg = backdrop_rgba();

    if (bg)
        canvas_blit(c, bg, BACKDROP_W, BACKDROP_H, BACKDROP_W * 4, x, y, w, h);
    else
        canvas_rect(c, x, y, w, h, RGBA(60, 70, 90, 255));
}

/* Everything over the pictures: the selected icon's corners, the panels, the name, the hints
   and the progress. */
static void draw_letter(library *l);
static void draw_strip(library *l);
static int strip_disc(const library *l, int y);

static void draw_front(library *l)
{
    canvas *c = &l->ui;

    if (!shelf_on())
        fill_icon_corners(l);
    draw_info(l);
    draw_settings(l);
    if (!l->settings_open) {
        if (!details_open)
            draw_tabs(l);
        if (details_open)
            draw_details(l);
        else if (list_on())
            draw_list(l);
        else if (shelf_on())
            draw_shelf(l);
        else
            draw_titles(l);
        if (!details_open) {
            draw_letter(l);
            draw_strip(l);
        }
    }
    /* On the backdrop (a disc without PIC1), keep clear of its film strips. */
    draw_app_name(c, 72);
    if (!l->settings_open)
        draw_hints(c, UI_H - 72);
    if (l->notice[0] && !l->settings_open) {
        int y = UI_H - 72 - 30 - (l->scanning || l->sheet_shown >= 0 ? 34 : 0);
        int w = canvas_text_width(l->notice, 1);
        canvas_round_darken(c, UI_W - 28 - w, y - 6, w + 16, 22, 6, 160);
        canvas_text(c, UI_W - 20 - w, y, 1, RGBA(255, 120, 100, 255), l->notice);
    }
    if (l->scanning || l->sheet_shown >= 0) {
        /* above the hints: discs still being read or sheets being written, with a bar under
           the count */
        char line[48];
        int y = UI_H - 72 - 30, w, fill;
        if (l->scanning) {
            snprintf(line, sizeof(line), "Reading movies %d of %d", l->count, l->scan_total);
            fill = l->scan_total ? l->count * 1000 / l->scan_total : 0;
        } else {
            snprintf(line, sizeof(line), "Writing info sheets %d of %d", l->sheet_done + 1, l->sheet_total);
            fill = l->sheet_shown;
        }
        w = canvas_text_width(line, 1);
        canvas_round_darken(c, UI_W - 28 - w, y - 6, w + 16, 26, 6, 140);
        canvas_text(c, UI_W - 20 - w, y, 1, TEXT_DIM, line);
        canvas_rect(c, UI_W - 20 - w, y + 12, w, 3, RGBA(70, 80, 100, 255));
        canvas_rect(c, UI_W - 20 - w, y + 12, w * fill / 1000, 3, RGBA(255, 255, 255, 230));
    }
}

/* Ids of the library's pictures for the GPU, above those of the disc menus. */
static uint32_t lib_id(void)
{
    static uint32_t next = 0x80000000u;
    return ++next;
}

static void add_sprite(library *l, const uint8_t *rgba, int w, int h, uint32_t id, int x, int y, int dw, int dh)
{
    plat_sprite *s = &l->sprites[l->info.ui_sprite_count++];

    s->rgba = rgba;
    s->w = w;
    s->h = h;
    s->id = id;
    s->x[0] = s->x[2] = (float)x;
    s->x[1] = s->x[3] = (float)(x + dw);
    s->y[0] = s->y[1] = (float)y;
    s->y[2] = s->y[3] = (float)(y + dh);
    s->u0 = s->v0 = 0;
    s->u1 = s->v1 = 1;
    for (int k = 0; k < 4; k++)
        s->color[k] = 1;
}

static void add_icon(library *l, lib_disc *d, int x, int y, int w, int h)
{
    static uint32_t backdrop_id;

    if (d && d->icon0) {
        if (!d->icon_id)
            d->icon_id = lib_id();
        add_sprite(l, d->icon0, d->icon_w, d->icon_h, d->icon_id, x, y, w, h);
    } else if (backdrop_rgba()) {
        if (!backdrop_id)
            backdrop_id = lib_id();
        add_sprite(l, backdrop_rgba(), BACKDROP_W, BACKDROP_H, backdrop_id, x, y, w, h);
    }
}

/* A disc's picture in a shelf slot: its poster, else its wide icon centred in the slot. */
static void add_art(library *l, lib_disc *d, int x, int y, int w, int h)
{
    if (d->poster) {
        if (!d->poster_id)
            d->poster_id = lib_id();
        add_sprite(l, d->poster, POSTER_W, POSTER_H, d->poster_id, x, y, w, h);
    } else {
        int ih = w * ICON_H / ICON_W;
        add_icon(l, d, x, y + (h - ih) / 2, w, ih);
    }
}

/* The same for the platforms where the app draws everything itself. */
static void blit_art(library *l, const lib_disc *d, int x, int y, int w, int h)
{
    canvas *c = &l->ui;
    int ih = w * ICON_H / ICON_W;

    if (d->poster)
        canvas_blit(c, d->poster, POSTER_W, POSTER_H, POSTER_W * 4, x, y, w, h);
    else if (d->icon0)
        canvas_blit(c, d->icon0, d->icon_w, d->icon_h, d->icon_w * 4, x, y + (h - ih) / 2, w, ih);
    else
        draw_no_icon(c, x, y + (h - ih) / 2, w, ih);
}

/* The shelf's row at shelf_pos, on whole pixels so that each poster stays sharp. A poster sliding
   towards the big one is cut at the row's left edge. */
static void add_shelf_row(library *l)
{
    int hi = tab_hi(l), lo = tab_lo(l);

    for (int i = (int)l->shelf_pos; i < hi; i++) {
        float place = (float)SHELF_X + ((float)i - 1.0f - l->shelf_pos) * SHELF_STEP;
        int x = (int)(place + 0.5f), before = l->info.ui_sprite_count;

        if (x >= UI_W || before >= (int)(sizeof(l->sprites) / sizeof(l->sprites[0])))
            break;
        if (i < lo || x + SHELF_W <= SHELF_X)
            continue;
        add_art(l, &l->discs[i], x, SHELF_Y, SHELF_W, SHELF_H);
        if (x < SHELF_X && l->info.ui_sprite_count > before) {
            plat_sprite *s = &l->sprites[before];
            s->x[0] = s->x[2] = (float)SHELF_X;
            s->u0 = (float)(SHELF_X - x) / SHELF_W;
        }
    }
}

/* What draw_front shows depends on: a copy of each, compared to redraw only on a change. */
static void front_key(library *l, unsigned char *key, size_t size)
{
    struct {
        const void *bg;
        int show_info, bare, settings_open, settings_row, keys, scanning, count, scan_total;
        int sheet_shown, sheet_done, sheet_total, letter_shown, strip_shown, sel;
        int details_open, details_scroll, tab, moving;
        unsigned notice_seq;
        app_settings settings;
        const lib_disc *disc;
        lib_disc shown;
    } k;
    const lib_disc *d = &l->discs[l->sel];
    typedef char fits[sizeof(k) <= sizeof(l->front_key) ? 1 : -1];

    (void)sizeof(fits);
    memset(&k, 0, sizeof(k));
    k.bg = shelf_on() ? NULL : l->bg;   /* the shelf's text does not depend on the background */
    k.show_info = l->show_info;
    k.bare = !d->icon0 && !l->icon && !l->pic0 && !l->settings_open && !shelf_on();
    k.settings_open = l->settings_open;
    k.settings_row = l->settings_row;
    k.keys = plat_keyboard_ui();
    k.scanning = l->scanning;
    k.count = l->count;
    k.scan_total = l->scan_total;
    k.sheet_shown = l->sheet_shown;
    k.letter_shown = l->letter_shown;
    k.strip_shown = l->strip_shown;
    k.sheet_done = l->sheet_done;
    k.sheet_total = l->sheet_total;
    k.notice_seq = l->notice[0] ? l->notice_seq : 0;
    k.settings = *settings_get();
    k.sel = l->sel;                   /* the titles on screen follow the selection in every view */
    k.details_open = details_open;
    k.details_scroll = l->details_scroll;
    k.tab = l->tab;
    k.moving = shelf_moving(l);
    if (l->count > 0) {
        k.disc = d;
        k.shown = *d;
    }
    memset(key, 0, size);
    memcpy(key, &k, sizeof(k) < size ? sizeof(k) : size);
}

/* The library on the GPU: pictures as sprites, and the front layer only when it changed.
   Drawing and uploading the whole screen took about 40 ms a step on the Vita. */
static void draw_ui_sprites(library *l)
{
    static uint32_t backdrop_bg_id;
    unsigned char key[sizeof(l->front_key)];

    l->info.ui_sprites = l->sprites;
    l->info.ui_sprite_count = 0;
    if (l->bg) {
        if (!l->bg_id)
            l->bg_id = lib_id();
        add_sprite(l, l->bg, UI_W, UI_H, l->bg_id, 0, 0, UI_W, UI_H);
    } else if (backdrop_rgba()) {
        if (!backdrop_bg_id)
            backdrop_bg_id = lib_id();
        add_sprite(l, backdrop_rgba(), BACKDROP_W, BACKDROP_H, backdrop_bg_id, 0, 0, UI_W, UI_H);
    }
    l->info.ui_sprites_below = l->info.ui_sprite_count;
    if (l->pic0 && !l->show_info && !l->settings_open && !list_on() && !shelf_on()) {
        if (!l->pic0_id)
            l->pic0_id = lib_id();
        add_sprite(l, l->pic0, l->pic0_w, l->pic0_h, l->pic0_id, UI_W - 2 * l->pic0_w - 10, (UI_H - 2 * l->pic0_h) / 2,
                   2 * l->pic0_w, 2 * l->pic0_h);
    }
    for (int k = -1; k <= 2; k++) {
        int i = l->sel + k, y = k < 0 ? BIG_Y - 100 : BIG_Y + 2 * ICON_H + 20 + (k - 1) * 90;
        if (k != 0 && i >= tab_lo(l) && i < tab_hi(l) && !list_on() && !shelf_on())
            add_icon(l, &l->discs[i], SMALL_X, y, ICON_W, ICON_H);
    }
    if (shelf_on()) {
        if (l->big_poster)
            add_sprite(l, l->big_poster, SHELF_BIG_W, SHELF_BIG_H, l->big_poster_id, SHELF_BIG_X, SHELF_BIG_Y, SHELF_BIG_W, SHELF_BIG_H);
        else
            add_art(l, &l->discs[l->sel], SHELF_BIG_X, SHELF_BIG_Y, SHELF_BIG_W, SHELF_BIG_H);
        if (!details_open)
            add_shelf_row(l);

    } else if (!l->icon || !l->shown) {
        add_icon(l, &l->discs[l->sel], BIG_X, BIG_Y, 2 * ICON_W, 2 * ICON_H);
    }

    front_key(l, key, sizeof(key));
    if (memcmp(key, l->front_key, sizeof(key)) != 0 || !l->info.ui_rgba) {
        size_t row = (size_t)l->ui.stride;
        int from = 0, to = 0;
        uint64_t front_t0 = plat_time_us();
        memcpy(l->front_key, key, sizeof(key));
        memset(l->ui.rgba, 0, row * l->ui.h);
        draw_front(l);
        l->front_draw_us = plat_time_us() - front_t0;
        /* the rows that changed since the last version handed over */
        if (!l->front_prev)
            l->front_prev = calloc(row, (size_t)l->ui.h);
        if (l->front_prev && l->front_valid) {
            from = l->ui.h;
            for (int y = 0; y < l->ui.h; y++)
                if (memcmp(l->ui.rgba + y * row, l->front_prev + y * row, row) != 0) {
                    if (from > y)
                        from = y;
                    to = y + 1;
                }
        } else {
            to = l->ui.h;
        }
        if (to > from) {
            if (l->front_prev)
                memcpy(l->front_prev + from * row, l->ui.rgba + from * row, (size_t)(to - from) * row);
            l->info.ui_prev_id = l->front_valid ? l->info.ui_id : 0;
            l->front_valid = l->front_prev != NULL;
            l->info.ui_rows_from = from;
            l->info.ui_rows_to = to;
            l->info.ui_rgba = l->ui.rgba;
            l->info.ui_w = l->ui.w;
            l->info.ui_h = l->ui.h;
            l->info.ui_stride = l->ui.stride;
            l->info.ui_id++;
        }
    }
    l->ui_dirty = 0;
}

static void draw_ui(library *l)
{
    canvas *c = &l->ui;

    if (plat_draws_sprites()) {
        draw_ui_sprites(l);
        return;
    }
    l->info.ui_sprites = NULL;
    l->info.ui_prev_id = 0;
    draw_background(l);
    /* PIC0 at twice its size (the PSP's 480x272 screen doubled), right of the icon; the info
       panel and the settings take its place when open. */
    if (l->pic0 && !l->show_info && !l->settings_open && !list_on() && !shelf_on())
        canvas_blit(c, l->pic0, l->pic0_w, l->pic0_h, l->pic0_w * 4, UI_W - 2 * l->pic0_w - 10,
                    (UI_H - 2 * l->pic0_h) / 2, 2 * l->pic0_w, 2 * l->pic0_h);

    /* Neighbours: icons only, one above and two below. */
    for (int k = -1; k <= 2; k++) {
        int i = l->sel + k, y = k < 0 ? BIG_Y - 100 : BIG_Y + 2 * ICON_H + 20 + (k - 1) * 90;
        const lib_disc *d;
        if (k == 0 || i < tab_lo(l) || i >= tab_hi(l) || list_on() || shelf_on())
            continue;
        d = &l->discs[i];
        if (d->icon0)
            canvas_blit(c, d->icon0, d->icon_w, d->icon_h, d->icon_w * 4, SMALL_X, y, ICON_W, ICON_H);
        else
            draw_no_icon(c, SMALL_X, y, ICON_W, ICON_H);
    }

    /* The selected disc: its ICON1 animation shows through a transparent hole, else ICON0, with
       rounded corners. */
    if (shelf_on()) {
        if (l->big_poster)
            canvas_blit(c, l->big_poster, SHELF_BIG_W, SHELF_BIG_H, SHELF_BIG_W * 4, SHELF_BIG_X, SHELF_BIG_Y, SHELF_BIG_W, SHELF_BIG_H);
        else
            blit_art(l, &l->discs[l->sel], SHELF_BIG_X, SHELF_BIG_Y, SHELF_BIG_W, SHELF_BIG_H);
        for (int k = 1; !details_open && k <= SHELF_TILES && l->sel + k < tab_hi(l); k++)
            blit_art(l, &l->discs[l->sel + k], SHELF_X + (k - 1) * SHELF_STEP, SHELF_Y, SHELF_W, SHELF_H);
    } else if (l->icon) {
        canvas_round_rect(c, BIG_X, BIG_Y, 2 * ICON_W, 2 * ICON_H, BIG_R, RGBA(0, 0, 0, 0));
    } else if (l->discs[l->sel].icon0) {
        const lib_disc *d = &l->discs[l->sel];
        canvas_blit(c, d->icon0, d->icon_w, d->icon_h, d->icon_w * 4, BIG_X, BIG_Y, 2 * ICON_W, 2 * ICON_H);
    } else {
        draw_no_icon(c, BIG_X, BIG_Y, 2 * ICON_W, 2 * ICON_H);
    }
    draw_front(l);

    l->info.ui_rgba = c->rgba;
    l->info.ui_w = c->w;
    l->info.ui_h = c->h;
    l->info.ui_stride = c->stride;
    l->info.ui_id++;
    l->ui_dirty = 0;
}

/* The icon's time in 90 kHz. With sound, it follows the audio actually played since this
   disc's sound started, within the sound's loop, so picture and sound restart together (as on
   the PSP). Without sound, it runs on the system clock and loops at its own length. */
static uint64_t icon_time(library *l)
{
    if (l->audio_open && l->snd) {
        uint64_t loop = (uint64_t)l->snd_frames * AT3P_FRAME_SAMPLES, now = plat_time_us();
        uint64_t played = plat_audio_played(), lead;
        played = played > l->snd_start ? played - l->snd_start : 0;
        /* Played audio moves in buffer-sized steps; run on the system clock in between,
           at most two buffers ahead, like the player's clock. */
        if (played != l->clk_raw) {
            l->clk_raw = played;
            l->clk_us = now;
        }
        lead = (now - l->clk_us) * (uint64_t)l->snd_rate / 1000000;
        if (lead > 2048)
            lead = 2048;
        return (played + lead) % loop * 90000 / (uint64_t)l->snd_rate;
    }
    return (plat_time_us() - l->loop_us) * 9 / 100;
}

static void restart_icon(library *l)
{
    drop_frames(l);
    plat_video_flush(l->video);
    l->au_next = 0;
    l->loop_us = plat_time_us();
}

/* Decodes the icon animation ahead and returns 1 if a new frame is due now. */
static int advance_icon(library *l)
{
    static uint64_t window_us;
    static int shown_in_window;
    const icon_video *v = l->icon;
    uint64_t t = icon_time(l);
    int best = -1, changed = 0;

    if (t + v->loop_len / 2 < l->last_t || (l->snd && t + 45000 < l->last_t))
        restart_icon(l);                     /* the sound looped (small backward steps are jitter) */
    else if (t < l->last_t)
        t = l->last_t;
    else if (!l->snd && l->au_next == v->au_count && l->frame_count == 0 && t >= v->loop_len) {
        restart_icon(l);                     /* no sound: loop on the icon's own length */
        t = 0;
    }
    l->last_t = t;
    /* Two access units an update (a field-coded icon needs two a picture), the first picture
       too: decoding up to it at once took 50 ms on the Vita. Until it comes, the library shows
       the still icon. */
    for (int budget = 2; budget > 0 && l->frame_count < FRAMES_AHEAD && l->au_next < v->au_count;
         budget--) {
        const icon_au *a = &v->aus[l->au_next++];
        plat_frame *f;
        uint64_t pts;
        plat_video_decode(l->video, v->es + a->off, a->size, a->pts, 0);
        while (plat_video_frame(l->video, &f, &pts)) {
            int i = l->frame_count;
            if (pts == PLAT_PTS_HIDDEN || i == 8) {
                plat_frame_release(f);
                continue;
            }
            for (; i > 0 && l->frames[i - 1].pts > pts; i--)
                l->frames[i] = l->frames[i - 1];
            l->frames[i].frame = f;
            l->frames[i].pts = pts;
            l->frame_count++;
        }
    }
    for (int i = 0; i < l->frame_count && l->frames[i].pts - v->first_pts <= t; i++)
        best = i;
    if (best >= 0) {
        for (int i = 0; i < best; i++)
            plat_frame_release(l->frames[i].frame);
        plat_frame_release(l->shown);
        if (!l->shown)
            l->ui_dirty = 1;           /* the still icon makes way for the video */
        l->shown = l->frames[best].frame;
        memmove(&l->frames[0], &l->frames[best + 1], (size_t)(l->frame_count - best - 1) * sizeof(l->frames[0]));
        l->frame_count -= best + 1;
        changed = 1;
        shown_in_window++;
    }
    if (plat_time_us() - window_us > 4000000) {
        if (window_us)
            plat_log("icon: %.1f frames/s, at %.2f s of %.2f s (%s clock)\n",
                     shown_in_window * 1e6 / (double)(plat_time_us() - window_us), t / 90000.0, v->loop_len / 90000.0,
                     l->snd ? "sound" : "system");
        window_us = plat_time_us();
        shown_in_window = 0;
    }
    return changed;
}

/* ---- public ---- */

/* The scan thread: reads every disc in list order and publishes each as it is done. It waits
   while a movie plays, as the worker does, so the movie has the memory card to itself. */
static void scan_main(void *user)
{
    library *l = user;

    for (int i = 0; i < l->scan_total; i++) {
        plat_mutex_lock(l->cache_lock);
        while (l->worker_paused && !l->scan_quit)
            plat_cond_wait(l->cache_cond, l->cache_lock);
        plat_mutex_unlock(l->cache_lock);
        if (l->scan_quit)
            return;
        /* not shown yet: only this thread touches it */
        if (l->scan_force || !load_disc_cache(&l->discs[i])) {
            read_disc_info(&l->discs[i]);
            save_disc_cache(&l->discs[i]);
        }
        plat_mutex_lock(l->cache_lock);
        l->scan_read = i + 1;
        plat_mutex_unlock(l->cache_lock);
    }
}

/* The info sheet thread: counts the discs without a sheet, then writes theirs in list order.
   Runs only after the scan, so the list does not change under it. */
static void write_sheets(library *l)
{
    char sheet[1024];
    int total = 0;

    for (int i = 0; i < l->count; i++) {
        infosheet_path(l->discs[i].path, sheet, sizeof(sheet));
        total += !plat_file_exists(sheet);
    }
    plat_mutex_lock(l->cache_lock);
    l->sheet_total = total;
    l->sheet_done = 0;
    plat_mutex_unlock(l->cache_lock);
    for (int i = 0; i < l->count && total; i++) {
        infosheet_path(l->discs[i].path, sheet, sizeof(sheet));
        if (plat_file_exists(sheet))
            continue;
        for (;;) {
            plat_mutex_lock(l->cache_lock);
            while (l->worker_paused && !l->sheet_quit)
                plat_cond_wait(l->cache_cond, l->cache_lock);
            l->sheet_stop = l->sheet_quit;
            plat_mutex_unlock(l->cache_lock);
            if (l->sheet_quit)
                return;
            if (infosheet_write(l->discs[i].path, &l->sheet_stop, &l->sheet_permille) == 0 || !l->sheet_stop)
                break;                    /* written, or a disc that cannot be read: skip it */
        }
        plat_mutex_lock(l->cache_lock);
        l->sheet_done++;
        plat_mutex_unlock(l->cache_lock);
    }
}

static void sheet_main(void *user)
{
    library *l = user;

    write_sheets(l);
    plat_mutex_lock(l->cache_lock);
    l->sheet_ended = 1;
    plat_mutex_unlock(l->cache_lock);
}

static void stop_sheets(library *l)
{
    if (!l->sheeter)
        return;
    plat_mutex_lock(l->cache_lock);
    l->sheet_quit = 1;
    l->sheet_stop = 1;
    plat_cond_broadcast(l->cache_cond);
    plat_mutex_unlock(l->cache_lock);
    plat_thread_join(l->sheeter);
    l->sheeter = NULL;
    l->ui_dirty = 1;
}

/* Starts the info sheet thread when the setting turns on after the scan, stops it when the
   setting turns off, and moves its progress bar. */
static void sheets_update(library *l)
{
    int on = settings_get()->info_sheets, ended, shown;

    if (!on || l->scanning) {
        stop_sheets(l);
        return;
    }
    if (!l->sheeter && !l->sheet_finished && l->count) {
        l->sheet_quit = l->sheet_ended = 0;
        l->sheet_stop = 0;
        l->sheet_total = l->sheet_done = 0;
        l->sheet_permille = 0;
        l->sheeter = plat_thread_start(sheet_main, l, "sheets");
        l->sheet_finished = 1;        /* once per library, or until turned on again */
        if (!l->sheeter)
            plat_log("library: cannot start the info sheet thread\n");
        return;
    }
    if (!l->sheeter)
        return;
    plat_mutex_lock(l->cache_lock);
    ended = l->sheet_ended;
    shown = l->sheet_done < l->sheet_total ? l->sheet_permille / 10 * 10 : -1;   /* redraw each percent */
    plat_mutex_unlock(l->cache_lock);
    if (ended) {
        plat_thread_join(l->sheeter);
        l->sheeter = NULL;
        shown = -1;
    }
    if (shown != l->sheet_shown) {
        l->sheet_shown = shown;
        l->ui_dirty = 1;
    }
}

static int rescan_next;             /* the library opened next reads every disc again */

/* Lists the disc images in every folder and starts the scan thread on them. Only called
   while the library is empty, so the worker has nothing to read. */
static void start_scan(library *l)
{
    list_ctx ctx;
    int tv;

    memset(&ctx, 0, sizeof(ctx));
    for (const char *p = l->dirs; *p;) {
        char one[256];
        size_t n = strcspn(p, ";");
        snprintf(one, sizeof(one), "%.*s", (int)(n < sizeof(one) ? n : sizeof(one) - 1), p);
        ctx.dir = one;
        if (n) {
            int before = ctx.count;
            if (plat_list_dir(one, on_entry, &ctx) < 0)
                plat_log("library: cannot open %s\n", one);
            else
                plat_log("library: %d discs in %s\n", ctx.count - before, one);
        }
        p += n + (p[n] == ';');
    }
    /* Movies and TV shows are told apart by their details, so those are read before the sort. */
    tv = 0;
    for (int i = 0; i < ctx.count; i++) {
        load_details(&ctx.discs[i]);
        tv += disc_is_tv(&ctx.discs[i]);
    }
    tv_sorted_first = tv > 0 && (settings_get()->lib_tab == 1 || tv == ctx.count);
    if (ctx.count)
        qsort(ctx.discs, (size_t)ctx.count, sizeof(ctx.discs[0]), compare_sections);
    plat_mutex_lock(l->cache_lock);
    free(l->discs);
    l->discs = ctx.discs;
    l->count = 0;
    l->sel = 0;
    l->split = tv_sorted_first ? tv : ctx.count - tv;
    l->tab = 0;
    l->tab_sel[0] = 0;
    l->tab_sel[1] = l->split;
    l->list_top = 0;
    l->shelf_pos = 0;
    l->scan_total = ctx.count;
    l->scan_read = 0;
    l->scan_quit = 0;
    l->scan_force = rescan_next;
    rescan_next = 0;
    plat_mutex_unlock(l->cache_lock);
    l->scanning = 1;
    l->scan_start_us = plat_time_us();
    l->scanner = plat_thread_start(scan_main, l, "scan");
    if (!l->scanner)
        scan_main(l);
}

/* Shows the discs the scan thread has read since the last frame; at the end, joins it. */
static void scan_update(library *l)
{
    int read, old = l->count;

    plat_mutex_lock(l->cache_lock);
    read = l->scan_read;
    if (read > old)
        l->count = read;
    plat_mutex_unlock(l->cache_lock);
    for (int i = old; i < read; i++)
        load_resume_mark(&l->discs[i]);
    if (read > old)
        l->ui_dirty = 1;
    if (old == 0 && read > 0) {
        l->loaded = -1;
        tell_worker(l, 0);
    }
    if (read < l->scan_total)
        return;
    plat_thread_join(l->scanner);
    l->scanner = NULL;
    l->scanning = 0;
    l->ui_dirty = 1;
    plat_log("library: %d discs in total, read in %.1f s\n", l->count, (plat_time_us() - l->scan_start_us) / 1e6);
}

library *library_open(const char *dir)
{
    library *l = calloc(1, sizeof(*l));

    if (!l)
        return NULL;
    snprintf(l->dirs, sizeof(l->dirs), "%s", dir);
    l->strip_target = -1;
    l->tab_tap = -1;
    l->bg_pin = -1;
    l->lock = plat_mutex_create();
    l->at3p = malloc(at3p_decoder_size());
    l->ui.w = UI_W;
    l->ui.h = UI_H;
    l->ui.stride = UI_W * 4;
    l->ui.rgba = malloc(UI_W * UI_H * 4);
    l->cache_lock = plat_mutex_create();
    l->cache_cond = plat_cond_create();
    for (int i = 0; i < CACHE_N; i++)
        l->cache[i].disc = -1;
    if (!l->lock || !l->at3p || !l->ui.rgba || !l->cache_lock || !l->cache_cond) {
        library_close(l);
        return NULL;
    }
    at3p_init_tables();
    l->loaded = -1;
    l->sheet_shown = -1;
    l->ui_dirty = 1;
    l->show_info = settings_get()->lib_view == LIB_DETAILS;
    details_open = 0;
    l->info.video_w = ICON_W;
    l->info.video_h = ICON_H;
    l->info.dst = (plat_rect){(float)BIG_X / UI_W, (float)BIG_Y / UI_H, 2.0f * ICON_W / UI_W, 2.0f * ICON_H / UI_H};
    l->worker = plat_thread_start(worker_main, l, "library");
    start_scan(l);
    return l;
}

/* Before the first disc is read: the backdrop, how many are done, and the one being read. */
static void draw_loading(library *l)
{
    canvas *c = &l->ui;
    const uint8_t *bg = backdrop_rgba();
    const char *title = "Reading your movies";
    char line[64], name[128];
    int bw = 400, bx = (UI_W - bw) / 2, y = 220;

    if (bg)
        memcpy(c->rgba, bg, UI_W * UI_H * 4);
    else
        canvas_rect(c, 0, 0, UI_W, UI_H, RGBA(12, 22, 52, 255));
    draw_app_name(c, 72);   /* below the film strip */
    canvas_text(c, (UI_W - canvas_text_width(title, 3)) / 2, y, 3, RGBA(255, 255, 255, 255), title);
    y += 50;
    snprintf(line, sizeof(line), "%d of %d", l->count + 1, l->scan_total);
    canvas_text(c, (UI_W - canvas_text_width(line, 2)) / 2, y, 2, TEXT_DIM, line);
    y += 34;
    canvas_rect(c, bx, y, bw, 6, RGBA(70, 80, 100, 255));
    y += 24;
    canvas_text_fit(name, sizeof(name), file_name(l->discs[l->count].path), 1, UI_W - 80);
    canvas_text(c, (UI_W - canvas_text_width(name, 1)) / 2, y, 1, TEXT_DIM, name);
    l->info.ui_rgba = c->rgba;
    l->info.ui_w = c->w;
    l->info.ui_h = c->h;
    l->info.ui_stride = c->stride;
    l->info.ui_id++;
    l->info.ui_sprites = NULL;                /* the whole screen, drawn here */
    l->info.ui_prev_id = 0;
    l->front_valid = 0;
    memset(l->front_key, 0xFF, sizeof(l->front_key));
    plat_present(NULL, &l->info);
}

/* No discs: say where to put them. × lists the folders again. */
static void draw_empty(library *l)
{
    canvas *c = &l->ui;
    const char *title = "No movies found";
    const uint8_t *bg = backdrop_rgba();
    int y = 150;

    if (bg)
        memcpy(c->rgba, bg, UI_W * UI_H * 4);
    else
        canvas_rect(c, 0, 0, UI_W, UI_H, RGBA(12, 22, 52, 255));
    draw_app_name(c, 72);   /* below the film strip */
    canvas_text(c, (UI_W - canvas_text_width(title, 3)) / 2, y, 3, RGBA(255, 255, 255, 255), title);
    y += 60;
    canvas_text(c, (UI_W - canvas_text_width("Copy .iso, .cso or .zso UMD Video images to:", 2)) / 2, y, 2, TEXT_DIM,
                "Copy .iso, .cso or .zso UMD Video images to:");
    y += 40;
    for (const char *p = l->dirs; *p;) {
        char one[256];
        size_t n = strcspn(p, ";");
        snprintf(one, sizeof(one), "%.*s/", (int)(n < sizeof(one) - 1 ? n : sizeof(one) - 2), p);
        if (n) {
            canvas_text(c, (UI_W - canvas_text_width(one, 2)) / 2, y, 2, RGBA(255, 255, 255, 255), one);
            y += 28;
        }
        p += n + (p[n] == ';');
    }
    y += 40;
    {
        const char *again = "Look again";
        int bw = osd_button_width(ICON_BUTTON_CROSS, 2);
        int x = (UI_W - (bw + 10 + canvas_text_width(again, 2))) / 2;
        osd_button(c, x, y - 8, 2, ICON_BUTTON_CROSS);
        canvas_text(c, x + bw + 10, y, 2, TEXT_DIM, again);
    }
    l->info.ui_rgba = c->rgba;
    l->info.ui_w = c->w;
    l->info.ui_h = c->h;
    l->info.ui_stride = c->stride;
    l->info.ui_id++;
    l->info.ui_sprites = NULL;                /* the whole screen, drawn here */
    l->info.ui_prev_id = 0;
    l->front_valid = 0;
    memset(l->front_key, 0xFF, sizeof(l->front_key));
    l->ui_dirty = 0;
}

/* The touch screen, as buttons. Dragging up or down moves through the list, a disc per
   DRAG_STEP; a tap on the selected disc plays it, and a tap on one above or below selects it. */
static uint32_t touch_buttons(library *l)
{
    int x, y, down = plat_touch(&x, &y);
    int drag = list_on() ? LIST_ROW : DRAG_STEP;
    uint32_t b = 0;

    if (down && !l->touch_down) {
        l->touch_down = 1;
        l->touch_x0 = x;
        l->touch_y0 = l->touch_step_y = y;
        l->touch_step_x = x;
        l->touch_moved = 0;
        l->strip_touch = !l->settings_open && !details_open && x >= UI_W - STRIP_W - 16 && y >= STRIP_TOP - 20 && y < STRIP_BOTTOM + 20;
    }
    if (down && l->strip_touch) {                /* on the alphabet strip: the letter under the finger */
        l->strip_target = strip_disc(l, y);
        return 0;
    }
    if (!down && l->strip_touch) {
        l->strip_touch = 0;
        l->touch_down = 0;
        return 0;
    }
    if (down) {
        if (abs(x - l->touch_x0) > TAP_MOVE || abs(y - l->touch_y0) > TAP_MOVE)
            l->touch_moved = 1;
        if (shelf_on()) {                            /* the row follows the finger left: next disc */
            if (x - l->touch_step_x <= -SHELF_DRAG) {
                l->touch_step_x -= SHELF_DRAG;
                b |= PLAT_BTN_DOWN;
            } else if (x - l->touch_step_x >= SHELF_DRAG) {
                l->touch_step_x += SHELF_DRAG;
                b |= PLAT_BTN_UP;
            }
            return b;
        }
        if (y - l->touch_step_y <= -drag) {          /* the list follows the finger up: next disc */
            l->touch_step_y -= drag;
            b |= PLAT_BTN_DOWN;
        } else if (y - l->touch_step_y >= drag) {
            l->touch_step_y += drag;
            b |= PLAT_BTN_UP;
        }
        return b;
    }
    if (!l->touch_down)
        return 0;
    l->touch_down = 0;
    if (l->touch_moved || l->settings_open)
        return 0;
    x = l->touch_x0;
    y = l->touch_y0;
    if (!details_open && y >= TAB_Y && y < TAB_Y + TAB_H) {   /* a tab: library_update changes to it */
        for (int t = 0; t < 2; t++) {
            char s[32];
            int tx, tw;
            if (tab_label(l, t, s, sizeof(s), &tx, &tw) && x >= tx && x < tx + tw)
                l->tab_tap = t;
        }
        return 0;
    }
    if (details_open) {                          /* the poster plays; a tap elsewhere goes back */
        if (x >= SHELF_BIG_X && x < SHELF_BIG_X + SHELF_BIG_W && y >= SHELF_BIG_Y && y < SHELF_BIG_Y + SHELF_BIG_H)
            return PLAT_BTN_CROSS;
        return PLAT_BTN_CIRCLE;
    }
    if (shelf_on()) {                            /* the big poster plays; a tile selects its disc */
        if (x >= SHELF_BIG_X && x < SHELF_BIG_X + SHELF_BIG_W && y >= SHELF_BIG_Y && y < SHELF_BIG_Y + SHELF_BIG_H)
            return PLAT_BTN_CROSS;
        if (y < SHELF_Y || y >= SHELF_Y + SHELF_H)
            return 0;
        for (int k = 1; k <= SHELF_TILES; k++) {
            int left = SHELF_X + (k - 1) * SHELF_STEP;
            if (x >= left && x < left + SHELF_W && l->sel + k < tab_hi(l)) {
                l->sel += k - 1;                 /* the Down below makes it k */
                return PLAT_BTN_DOWN;
            }
        }
        return 0;
    }
    if (x >= BIG_X && x < BIG_X + 2 * ICON_W && y >= BIG_Y && y < BIG_Y + 2 * ICON_H)
        return PLAT_BTN_CROSS;
    if (list_on()) {                             /* a row: select it, or play it when selected */
        int i;
        if (x < LIST_X || x >= LIST_X + LIST_W || y < LIST_Y || y >= LIST_Y + LIST_ROWS * LIST_ROW)
            return 0;
        i = list_first(l) + (y - LIST_Y) / LIST_ROW;
        if (i >= tab_hi(l))
            return 0;
        if (i == l->sel)
            return PLAT_BTN_CROSS;
        l->sel = i > l->sel ? i - 1 : i + 1;     /* the Up or Down below makes it i */
        return i > l->sel ? PLAT_BTN_DOWN : PLAT_BTN_UP;
    }
    if (x < SMALL_X - 16 || x >= SMALL_X + ICON_W + 16)
        return 0;
    if (y >= BIG_Y - 100 && y < BIG_Y - 100 + ICON_H)
        return PLAT_BTN_UP;
    for (int k = 1; k <= 2; k++) {               /* the discs below, as draw_ui places them */
        int top = BIG_Y + 2 * ICON_H + 20 + (k - 1) * 90;
        if (y >= top && y < top + ICON_H && l->sel + k < tab_hi(l)) {
            l->sel += k - 1;                     /* the Down below makes it k */
            return PLAT_BTN_DOWN;
        }
    }
    return 0;
}

/* The mouse wheel, as Up and Down: a disc per step, one step a frame. Steps from while the
   library was not on screen (a movie playing) are dropped. */
static uint32_t scroll_buttons(library *l)
{
    uint64_t now = plat_time_us();
    int steps = plat_scroll();

    if (now - l->scroll_us > 500000)
        l->scroll = 0;
    else
        l->scroll += steps;
    l->scroll_us = now;
    if (l->scroll > 8) l->scroll = 8;
    if (l->scroll < -8) l->scroll = -8;
    if (l->scroll > 0) {
        l->scroll--;
        return PLAT_BTN_DOWN;
    }
    if (l->scroll < 0) {
        l->scroll++;
        return PLAT_BTN_UP;
    }
    return 0;
}

/* Discs to move for the left stick this frame: one as it is pushed, then a rate that grows
   with how far, from STICK_SLOW to STICK_FAST a second. Negative is up. */
static int stick_steps(library *l)
{
    int y = plat_stick_y(), a = y < 0 ? -y : y, n;
    uint64_t now = plat_time_us();
    float f, rate;

    if (a <= STICK_DEAD) {
        l->stick_on = 0;
        return 0;
    }
    if (!l->stick_on) {
        l->stick_on = 1;
        l->stick_acc = 0;
        l->stick_us = now;
        return y < 0 ? -1 : 1;
    }
    f = (float)(a - STICK_DEAD) / (127 - STICK_DEAD);
    rate = STICK_SLOW + (STICK_FAST - STICK_SLOW) * f * f;
    l->stick_acc += rate * (float)(now - l->stick_us) / 1e6f;
    l->stick_us = now;
    n = (int)l->stick_acc;
    l->stick_acc -= (float)n;
    return y < 0 ? -n : n;
}

/* Discs to move for Up or Down held (the keyboard's arrows, the D-pad): after HOLD_DELAY, a
   rate from HOLD_SLOW up to STICK_FAST over HOLD_RAMP seconds, as the stick scrolls. The press
   itself moves one, as before. Negative is up. */
static int hold_steps(library *l, uint32_t pressed)
{
    uint32_t back = shelf_on() ? PLAT_BTN_LEFT : PLAT_BTN_UP, on = shelf_on() ? PLAT_BTN_RIGHT : PLAT_BTN_DOWN;
    uint32_t held = plat_buttons() & (back | on);
    uint64_t now = plat_time_us();
    float t, rate;
    int n;

    if (held != back && held != on) {
        l->hold_dir = 0;
        return 0;
    }
    if (held != l->hold_dir || (pressed & held)) {
        l->hold_dir = held;
        l->hold_since = l->hold_us = now;
        l->hold_acc = 0;
        return 0;
    }
    if (now - l->hold_since < HOLD_DELAY) {
        l->hold_us = now;
        return 0;
    }
    t = (float)(now - l->hold_since - HOLD_DELAY) / 1e6f;
    rate = t >= HOLD_RAMP ? STICK_FAST : HOLD_SLOW + (STICK_FAST - HOLD_SLOW) * t / HOLD_RAMP;
    l->hold_acc += rate * (float)(now - l->hold_us) / 1e6f;
    l->hold_us = now;
    n = (int)l->hold_acc;
    l->hold_acc -= (float)n;
    return held == back ? -n : n;
}

/* L and R jump a letter: R to the first disc of the next letter, L to the first disc of this
   letter, or of the one before when already there. Held, they go on after HOLD_DELAY, a letter
   every LETTER_REPEAT. */
static int letter_jump(library *l, uint32_t pressed)
{
    uint32_t held = plat_buttons() & (PLAT_BTN_L | PLAT_BTN_R);
    uint64_t now = plat_time_us();
    uint32_t dir = 0;
    int i = l->sel, lo = tab_lo(l), hi = tab_hi(l);

    if (pressed & (PLAT_BTN_L | PLAT_BTN_R)) {
        dir = pressed & PLAT_BTN_R ? PLAT_BTN_R : PLAT_BTN_L;
        l->letter_dir = dir;
        l->letter_next_us = now + HOLD_DELAY;
    } else if (held && held == l->letter_dir && now >= l->letter_next_us) {
        dir = held;
        l->letter_next_us = now + LETTER_REPEAT;
    } else if (held != l->letter_dir) {
        l->letter_dir = 0;
    }
    if (!dir || l->count == 0)
        return l->sel;
    if (dir == PLAT_BTN_R) {
        char at = disc_letter(&l->discs[i]);
        while (i < hi - 1 && disc_letter(&l->discs[i]) == at)
            i++;
    } else {
        if (i > lo && disc_letter(&l->discs[i - 1]) == disc_letter(&l->discs[i])) {
            char at = disc_letter(&l->discs[i]);
            while (i > lo && disc_letter(&l->discs[i - 1]) == at)
                i--;
        } else if (i > lo) {
            char at = disc_letter(&l->discs[i - 1]);
            i--;
            while (i > lo && disc_letter(&l->discs[i - 1]) == at)
                i--;
        }
    }
    l->letter_us = now;
    l->letter_shown = 1;
    l->ui_dirty = 1;
    return i;
}

/* The letter of the selected disc, large over its icon, while a jump by letter is recent. */
static void draw_letter(library *l)
{
    canvas *c = &l->ui;
    char s[2] = {disc_letter(&l->discs[l->sel]), 0};
    int scale = 4, w = canvas_text_width(s, scale), x = BIG_X + ICON_W, y = BIG_Y + ICON_H;

    if (!l->letter_shown)
        return;
    canvas_round_darken(c, x - 40, y - 40, 80, 80, 16, 160);
    canvas_text(c, x - w / 2, y - 4 * scale, scale, RGBA(255, 255, 255, 255), s);
}

/* The letters the discs are listed under, in order, with the first disc of each. */
static int disc_letters(const library *l, char *letters, int *first)
{
    int n = 0;

    for (int i = tab_lo(l); i < tab_hi(l); i++) {
        char c = disc_letter(&l->discs[i]);
        if (n == 0 || letters[n - 1] != c) {
            letters[n] = c;
            first[n++] = i;
        }
    }
    return n;
}

/* The alphabet strip: the letters that have discs, down the right edge, the selected disc's
   letter brighter. A finger on it (or the mouse) jumps to the letter under it. */
static void draw_strip(library *l)
{
    canvas *c = &l->ui;
    char letters[27];
    int first[27], n = disc_letters(l, letters, first), row, scale;
    char at = disc_letter(&l->discs[l->sel]);

    if (!l->strip_shown || n == 0)
        return;
    row = (STRIP_BOTTOM - STRIP_TOP) / n;
    scale = row >= 18 ? 2 : 1;
    canvas_round_darken(c, UI_W - STRIP_W, STRIP_TOP - 6, STRIP_W - 6, row * n + 12, 10, 150);
    for (int k = 0; k < n; k++) {
        char s[2] = {letters[k], 0};
        int w = canvas_text_width(s, scale);
        canvas_text(c, UI_W - STRIP_W / 2 - 3 - w / 2, STRIP_TOP + k * row + (row - 8 * scale) / 2, scale,
                    letters[k] == at ? RGBA(255, 255, 255, 255) : TEXT_DIM, s);
    }
}

/* The disc whose letter is at height y on the strip. */
static int strip_disc(const library *l, int y)
{
    char letters[27];
    int first[27], n = disc_letters(l, letters, first), k;

    if (n == 0)
        return -1;
    k = (y - STRIP_TOP) / ((STRIP_BOTTOM - STRIP_TOP) / n);
    k = k < 0 ? 0 : k >= n ? n - 1 : k;
    return first[k];
}

void library_notice(library *l, const char *text)
{
    snprintf(l->notice, sizeof(l->notice), "%s", text);
    l->notice_until = plat_time_us() + 5000000;
    l->notice_seq++;
    l->ui_dirty = 1;
}

/* Changes between Movies and TV Shows, back to where that list was left. */
static void switch_tab(library *l)
{
    int other = !l->tab, lo = other ? l->split : 0, hi = other || l->count < l->split ? l->count : l->split;
    app_settings *s = settings_get();

    if (hi <= lo) {
        library_notice(l, l->scanning ? "Still reading those discs" : "No discs in that list");
        return;
    }
    l->tab_sel[l->tab] = l->sel;
    l->tab = other;
    l->sel = l->tab_sel[other] >= hi ? hi - 1 : l->tab_sel[other] < lo ? lo : l->tab_sel[other];
    l->list_top = l->sel - LIST_ROWS / 2;
    l->shelf_pos = (float)l->sel;
    clear_media(l);
    l->moved_us = plat_time_us();
    l->ui_dirty = 1;
    tell_worker(l, 0);
    s->lib_tab = (other == 0) == tv_sorted_first;
    settings_save();
}

int library_update(library *l, uint32_t pressed)
{
    int present = 0;

    if (l->notice[0] && plat_time_us() >= l->notice_until) {
        l->notice[0] = 0;
        l->ui_dirty = 1;
    }

    if (plat_keyboard_ui() != l->keys_shown) {   /* keys or a gamepad: redraw the hints */
        l->keys_shown = plat_keyboard_ui();
        l->ui_dirty = 1;
    }

    if (l->scanning) {
        scan_update(l);
        if (l->scanning && l->count == 0) {
            if (plat_time_us() - l->last_loading_us > 100000) {   /* ten times a second is enough */
                l->last_loading_us = plat_time_us();
                draw_loading(l);
            }
            return -1;
        }
    }
    if (l->count == 0) {
        if (pressed & PLAT_BTN_CROSS) {
            start_scan(l);
            return -1;
        }
        if (l->count == 0) {
            if (l->ui_dirty) {
                draw_empty(l);
                plat_present(NULL, &l->info);
            }
            return -1;
        }
    }
    sheets_update(l);
    if (l->marks_stale) {
        load_resume_marks(l);
        l->ui_dirty = 1;
    }
    {
        /* Up and Down on the shelf, which scrolls with Left and Right; Left and Right elsewhere. */
        uint32_t dpad = pressed, tab_keys = shelf_on() ? PLAT_BTN_UP | PLAT_BTN_DOWN : PLAT_BTN_LEFT | PLAT_BTN_RIGHT;
        pressed |= touch_buttons(l) | scroll_buttons(l);
        if (l->tab_tap >= 0) {
            if (l->tab_tap != l->tab && !l->settings_open)
                switch_tab(l);
            l->tab_tap = -1;
        }
        if ((dpad & tab_keys) && !l->settings_open && !details_open && !l->stick_on) {
            switch_tab(l);
            pressed &= ~tab_keys;
        }
    }
    if (l->settings_open) {
        settings_input(l, pressed);
        pressed = 0;
    }
    if (l->rescan) {
        rescan_next = 1;
        return -3;
    }
    if (pressed & PLAT_BTN_CIRCLE) {
        details_open = !details_open;
        l->details_scroll = 0;
        l->ui_dirty = 1;
    }
    if (details_open) {                          /* Up and Down scroll; Left and Right change disc */
        if ((pressed & PLAT_BTN_DOWN) && l->details_more) {
            l->details_scroll++;
            l->ui_dirty = 1;
        } else if ((pressed & PLAT_BTN_UP) && l->details_scroll > 0) {
            l->details_scroll--;
            l->ui_dirty = 1;
        }
        pressed &= ~(uint32_t)(PLAT_BTN_UP | PLAT_BTN_DOWN);
        if (pressed & (PLAT_BTN_TRIANGLE | PLAT_BTN_SELECT | PLAT_BTN_CROSS)) {
            details_open = 0;
            l->ui_dirty = 1;
            pressed &= ~(uint32_t)PLAT_BTN_SELECT;
        } else if ((pressed & PLAT_BTN_SQUARE) && l->discs[l->sel].has_resume) {
            details_open = 0;
        }
    }
    if (pressed & PLAT_BTN_TRIANGLE) {
        l->settings_open = 1;
        l->settings_row = 0;
        l->ui_dirty = 1;
        pressed = 0;
    }
    if ((pressed & PLAT_BTN_CROSS) && l->discs[l->sel].no_movie) {
        library_notice(l, "No movie in this image: UMD_VIDEO/PLAYLIST.UMD is missing");
        pressed &= ~PLAT_BTN_CROSS;
    }
    if (pressed & PLAT_BTN_CROSS)
        return l->sel;
    if (pressed & PLAT_BTN_SELECT) {             /* list, shelf, icons, icons with the info panel */
        static const int next[LIB_VIEWS] = {[LIB_LIST] = LIB_SHELF, [LIB_SHELF] = LIB_POSTERS,
                                            [LIB_POSTERS] = LIB_DETAILS, [LIB_DETAILS] = LIB_LIST};
        app_settings *s = settings_get();
        s->lib_view = next[s->lib_view];
        l->show_info = s->lib_view == LIB_DETAILS;
        settings_save();
        l->ui_dirty = 1;
    }
    if ((pressed & PLAT_BTN_SQUARE) && l->discs[l->sel].has_resume) {
        resume_forget_position(file_name(l->discs[l->sel].path));
        disc_state_save(file_name(l->discs[l->sel].path), NULL);
        l->discs[l->sel].has_resume = 0;   /* forgotten, so the disc plays from the start */
        l->marks_stale = 1;
        l->ui_dirty = 1;
        return l->sel;
    }
    {
        /* Up and Down a disc each, and the left stick as many as stick_steps says; on Windows
           and Linux the stick is the D-pad too, so its presses are left out. */
        int to = l->settings_open ? l->sel : letter_jump(l, pressed);
        if (l->strip_target >= 0 && l->strip_target != to) {
            to = l->strip_target;
            l->letter_us = plat_time_us();
            l->letter_shown = 1;
        }
        l->strip_target = -1;
        to += l->settings_open ? 0 : stick_steps(l) + hold_steps(l, pressed);
        if (l->stick_on)
            pressed &= ~(uint32_t)(PLAT_BTN_UP | PLAT_BTN_DOWN);
        if ((pressed & PLAT_BTN_UP) || (shelf_on() && (pressed & PLAT_BTN_LEFT)))
            to--;
        else if ((pressed & PLAT_BTN_DOWN) || (shelf_on() && (pressed & PLAT_BTN_RIGHT)))
            to++;
        if (to > tab_hi(l) - 1)
            to = tab_hi(l) - 1;
        if (to < tab_lo(l))
            to = tab_lo(l);
        pressed = to != l->sel;
        l->sel = to;
    }
    if (pressed) {
        clear_media(l);
        l->ui_dirty = 1;
        l->moved_us = plat_time_us();
        l->details_scroll = 0;
    }
    /* The worker rests during fast scrolling: reading each disc passed took time from the frames. */
    {
        int fast = l->stick_on || (l->hold_dir && plat_time_us() - l->hold_since > HOLD_DELAY);
        if (l->worker_paused || (!fast && l->want_sel != l->sel))
            tell_worker(l, 0);
    }
    if ((pressed || !l->big_poster) && l->want_sel == l->sel)
        use_big_poster(l);
    {
        /* The shelf's row glides to the selection; only the pictures move, so this costs little. */
        uint64_t now = plat_time_us();
        float d = (float)l->sel - l->shelf_pos, a = (float)(now - l->shelf_us) / 1e6f * SHELF_SPEED;
        l->shelf_us = now;
        if (!shelf_on() || details_open) {
            l->shelf_pos = (float)l->sel;
        } else if (d != 0) {
            if (d > SHELF_LAG)
                d = SHELF_LAG;
            else if (d < -SHELF_LAG)
                d = -SHELF_LAG;
            d -= d * (a > 1 ? 1 : a);
            l->shelf_pos = d > -0.004f && d < 0.004f ? (float)l->sel : (float)l->sel - d;
            l->ui_dirty = 1;
        }
    }
    if (l->letter_shown && plat_time_us() - l->letter_us >= LETTER_SHOW && !l->strip_touch) {
        l->letter_shown = 0;
        l->ui_dirty = 1;
    }
    /* The strip shows with a jump by letter, a finger on it, and fast scrolling (a held
       direction or the stick), and stays STRIP_SHOW after. */
    if (l->letter_shown || l->strip_touch || l->stick_on || (l->hold_dir && plat_time_us() - l->hold_since > HOLD_DELAY))
        l->strip_us = plat_time_us();
    {
        int shown = l->count > 0 && !l->settings_open && plat_time_us() - l->strip_us < STRIP_SHOW;
        if (shown != l->strip_shown) {
            l->strip_shown = shown;
            l->ui_dirty = 1;
        }
    }
    uint64_t t0 = plat_time_us(), t1, t2, t3, t4;
    if (!l->perf.since)
        l->perf.since = t0;
    /* Scrolling through the list, each disc's background, icon video and sound wait until the
       selection rests: starting them took a frame or more each, on the Vita. */
    if (l->loaded != l->sel && t0 - l->moved_us >= SETTLE_US)
        load_media(l);
    t1 = plat_time_us();
    if (l->icon) {
        present |= advance_icon(l);
        l->info.src_w = l->icon->sps.width;
        l->info.src_h = l->icon->sps.height;
        l->info.video_w = l->icon->sps.width;
        l->info.video_h = l->icon->sps.height;
    }
    t2 = plat_time_us();
    if (l->ui_dirty) {
        draw_ui(l);
        present = 1;
        l->perf.draws++;
    }
    t3 = plat_time_us();
    if (present)
        plat_present(l->icon && !shelf_on() ? l->shown : NULL, &l->info);
    t4 = plat_time_us();
    l->perf.media += t1 - t0;
    l->perf.icon += t2 - t1;
    l->perf.draw += t3 - t2;
    l->perf.present += t4 - t3;
    if (t4 - t0 > 40000)
        plat_log("perf: slow library frame %.0f ms: %.0f ms media, %.0f ms icon, %.0f ms drawing (front layer %.0f ms), %.0f ms present\n",
                 (t4 - t0) / 1000.0, (t1 - t0) / 1000.0, (t2 - t1) / 1000.0, (t3 - t2) / 1000.0, l->front_draw_us / 1000.0, (t4 - t3) / 1000.0);
    if (++l->perf.frames, t4 - l->perf.since >= 5000000) {
        double n = l->perf.frames;
        plat_log("perf: library %.1f frames/s, %d redraws; per frame %.1f ms media, %.1f ms icon, %.1f ms drawing, "
                 "%.1f ms present\n", n / ((t4 - l->perf.since) / 1e6), l->perf.draws, l->perf.media / n / 1000,
                 l->perf.icon / n / 1000, l->perf.draw / n / 1000, l->perf.present / n / 1000);
        memset(&l->perf, 0, sizeof(l->perf));
    }
    return -1;
}

int library_interactive(const library *l, int index)
{
    return index >= 0 && index < l->count && l->discs[index].interactive;
}

const char *library_path(const library *l, int index)
{
    return index >= 0 && index < l->count ? l->discs[index].path : NULL;
}

void library_suspend(library *l)
{
    release_devices(l);
    tell_worker(l, 1);   /* no memory card reads while a movie plays */
    l->ui_dirty = 1;
    l->marks_stale = 1;
}

void library_close(library *l)
{
    if (!l)
        return;
    if (l->scanner) {
        plat_mutex_lock(l->cache_lock);
        l->scan_quit = 1;
        plat_cond_broadcast(l->cache_cond);
        plat_mutex_unlock(l->cache_lock);
        plat_thread_join(l->scanner);
    }
    stop_sheets(l);
    if (l->worker) {
        plat_mutex_lock(l->cache_lock);
        l->worker_quit = 1;
        plat_cond_broadcast(l->cache_cond);
        plat_mutex_unlock(l->cache_lock);
        plat_thread_join(l->worker);
    }
    if (l->lock && l->cache_lock)
        release_devices(l);
    for (int i = 0; i < CACHE_N; i++)
        free_media(&l->cache[i]);
    for (int i = 0; i < l->scan_total; i++) {
        free(l->discs[i].poster);
        free(l->discs[i].details);
        free(l->discs[i].icon0);
    }
    free(l->discs);
    plat_cond_destroy(l->cache_cond);
    plat_mutex_destroy(l->cache_lock);
    plat_mutex_destroy(l->lock);
    free(l->at3p);
    free(l->ui.rgba);
    free(l->front_prev);
    free(l);
}

typedef struct count_ctx {
    const char *dir;
    int depth, found;
} count_ctx;

static void count_disc(void *user, const char *name, int is_dir)
{
    count_ctx *c = user;
    char sub[512];
    const char *dir = c->dir;

    if (c->found)
        return;
    if (!is_dir) {
        c->found = image_name_supported(name);
        return;
    }
    if (c->depth >= LIB_DEPTH || is_bundle(name) || snprintf(sub, sizeof(sub), "%s/%s", dir, name) >= (int)sizeof(sub))
        return;
    c->dir = sub;
    c->depth++;
    plat_list_dir(sub, count_disc, c);
    c->depth--;
    c->dir = dir;
}

/* Whether the folder or one of its subfolders (LIB_DEPTH deep) holds a disc image. */
int library_has_discs(const char *dir)
{
    count_ctx c = {dir, 0, 0};
    return plat_list_dir(dir, count_disc, &c) == 0 && c.found;
}
