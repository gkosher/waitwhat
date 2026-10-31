#ifndef APP_H
#define APP_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdint.h>
#include <time.h>
#include "microui.h"
#include "cJSON.h"

#define APP_NAME  "WaitWhat"
#define APP_NAMEW L"WaitWhat"
#define MAX_BOX   5

/* ---------------------------------------------------------------- ui.c */

typedef struct { HFONT h; int height; } UiFont;
extern UiFont font_normal, font_bold, font_big, font_small, font_icon;
extern float ui_scale;
#define S(x) ((int)((x) * ui_scale + 0.5f))

extern const mu_Color COL_ACCENT, COL_DIM, COL_GOOD, COL_WARN, COL_BAD;

typedef struct UiWindow UiWindow;
struct UiWindow {
  HWND hwnd;
  mu_Context *ctx;
  HDC memdc;
  HBITMAP bmp, oldbmp;
  uint32_t *pixels;
  int w, h;
  int min_w, min_h;
  int busy, dirty;
  void (*frame)(UiWindow *w);                 /* build the microui frame */
  void (*post_frame)(UiWindow *w);            /* after frames, safe to act */
  int  (*hittest)(UiWindow *w, int x, int y); /* return HT* or 0 */
  void (*on_key)(UiWindow *w, int vk);
  void (*on_destroy)(UiWindow *w);
};

void ui_init(void);
UiWindow *ui_create(const wchar_t *title, int popup, int x, int y, int w, int h);
void ui_refresh(UiWindow *w);
void ui_close(UiWindow *w); /* deferred, safe to call from inside a frame */
int  ui_begin_main(UiWindow *w, int opt);
HICON ui_make_icon(int size);

/* widget helpers */
void ui_label_color(mu_Context *ctx, const char *text, mu_Color color);
void ui_label_font(mu_Context *ctx, const char *text, UiFont *font, mu_Color color);
void ui_text_font(mu_Context *ctx, const char *text, UiFont *font, mu_Color color);
void ui_label_right(mu_Context *ctx, const char *text, mu_Color color);
int  ui_accent_button(mu_Context *ctx, const char *label);
void ui_spacer(mu_Context *ctx, int h);
void ui_heading(mu_Context *ctx, const char *text);
void ui_level_bar(mu_Context *ctx, int box);
void ui_fill_round(mu_Context *ctx, mu_Rect r, mu_Color c, int radius);

/* -------------------------------------------------------------- data.c */

typedef struct {
  uint64_t key;
  int box, seen, right;
  long long last;
} Prog;

typedef struct {
  cJSON *item;
  const char *q;
  const char *note;
  const char **ans;
  int nans;
  uint64_t key;
} Card;

typedef struct {
  wchar_t file[MAX_PATH];  /* file name only */
  char fname[MAX_PATH];    /* utf-8 file name */
  char stem[MAX_PATH];     /* utf-8 file name without .json */
  const char *name;
  FILETIME mtime;
  cJSON *root;
  Card *cards;
  int ncards;
  int enabled;
  char err[160];
} Deck;

/* deck sharing server; can be changed in Settings */
#define DEFAULT_SERVER ""

typedef struct {
  float min_minutes, max_minutes;
  int paused, sound, skip_fullscreen;
  char server[256];
} Settings;

extern Deck *decks;
extern int ndecks;
extern Settings settings;
extern wchar_t dir_app[MAX_PATH], dir_decks[MAX_PATH];

int  data_init(void); /* returns 1 on first run */
void settings_load(void);
void settings_save(void);
void decks_load(void);
int  decks_changed_on_disk(void);
int  deck_create(const char *name);
int  deck_add_card(int di, const char *q, const char *a, const char *note);
int  deck_delete_card(int di, int ci);
void deck_slug(const char *name, char *out, int cap);
char *deck_export(int di);              /* compact {name, cards} json, free() it */
int  deck_import(const char *json, char *err, int errcap); /* returns deck index or -1 */

Prog prog_get(uint64_t key);
void prog_set(Prog p);
void prog_record(uint64_t key, int correct);
void progress_save(void);
void progress_reset(void);
void progress_stats(int *answered, int *right, int *learned, int *total);
int  deck_learned(Deck *d);

int  pick_card(int *deck, int *card);
int  check_answer(const char *const *answers, int n, const char *input);
enum { ANSWER_WRONG, ANSWER_EXACT, ANSWER_CLOSE };

void   rng_seed(void);
double rng01(void);

int  autostart_get(void);
void autostart_set(int on);
void autostart_repair(void);

/* --------------------------------------------------------------- net.c */

enum { NET_SHARE = 1, NET_IMPORT };
typedef struct {
  volatile int running;
  int kind, ok;
  char *body;    /* share: deck json to upload */
  char *result;  /* import: downloaded deck json */
  char code[16];
  char msg[256]; /* error text */
} NetJob;
extern NetJob net_job;

void net_set_notify(HWND hwnd, UINT msg);
int  net_start(int kind, const char *arg); /* 0 if it couldn't start (see net_job.msg) */

/* ---------------------------------------------------------------- misc */

int  utf8_to_wide(const char *s, int len, wchar_t *out, int cap);
int  wide_to_utf8(const wchar_t *s, int len, char *out, int cap);
void clipboard_set(const char *utf8);

#endif
