/* microui backend: Win32 windows + a software/GDI renderer into a DIB.
** Rects (incl. anti-aliased rounded ones) are filled directly into the
** pixel buffer; text and icons go through GDI for ClearType. */
#include "app.h"
#include <dwmapi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

float ui_scale = 1.0f;
UiFont font_normal, font_bold, font_big, font_small, font_icon;

const mu_Color COL_ACCENT = { 99, 102, 241, 255 };
const mu_Color COL_DIM    = { 140, 145, 160, 255 };
const mu_Color COL_GOOD   = { 74, 222, 128, 255 };
const mu_Color COL_WARN   = { 251, 191, 36, 255 };
const mu_Color COL_BAD    = { 248, 113, 113, 255 };

static HDC measure_dc;
static const wchar_t *CLASS_NORMAL = L"WaitWhatWindow";
static const wchar_t *CLASS_POPUP  = L"WaitWhatPopup";
static HICON icon_big, icon_small;

/* custom command: a rounded rect that keeps its full geometry, so corners
** stay correct when it is partially clipped */
#define CMD_ROUND (MU_COMMAND_MAX + 1)
typedef struct {
  mu_BaseCommand base;
  mu_Rect rect, clip;
  mu_Color color;
  int radius;
} RoundCmd;

static const WCHAR ICON_GLYPHS[MU_ICON_MAX] = { 0, 0xE711, 0xE73E, 0xE76C, 0xE70D };


/* ------------------------------------------------------------ text utils */

int utf8_to_wide(const char *s, int len, wchar_t *out, int cap) {
  int n;
  if (len < 0) { len = (int)strlen(s); }
  if (len == 0 || cap < 2) { if (cap > 0) { out[0] = 0; } return 0; }
  n = MultiByteToWideChar(CP_UTF8, 0, s, len, out, cap - 1);
  if (n <= 0) { n = 0; }
  out[n] = 0;
  return n;
}

int wide_to_utf8(const wchar_t *s, int len, char *out, int cap) {
  int n;
  if (len < 0) { len = (int)wcslen(s); }
  if (len == 0 || cap < 2) { if (cap > 0) { out[0] = 0; } return 0; }
  n = WideCharToMultiByte(CP_UTF8, 0, s, len, out, cap - 1, NULL, NULL);
  if (n <= 0) { n = 0; }
  out[n] = 0;
  return n;
}

static UiFont *font_of(mu_Font f) { return f ? (UiFont*)f : &font_normal; }

static int text_width(mu_Font font, const char *str, int len) {
  wchar_t buf[4096];
  SIZE sz = { 0, 0 };
  int n = utf8_to_wide(str, len, buf, 4096);
  SelectObject(measure_dc, font_of(font)->h);
  GetTextExtentPoint32W(measure_dc, buf, n, &sz);
  return sz.cx;
}

static int text_height(mu_Font font) { return font_of(font)->height; }

static void make_font(UiFont *f, const wchar_t *face, int px, int weight) {
  TEXTMETRICW tm;
  f->h = CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
  SelectObject(measure_dc, f->h);
  GetTextMetricsW(measure_dc, &tm);
  f->height = tm.tmHeight;
}

static void make_icon_font(void) {
  wchar_t face[64] = { 0 };
  make_font(&font_icon, L"Segoe Fluent Icons", 11, FW_NORMAL);
  SelectObject(measure_dc, font_icon.h);
  GetTextFaceW(measure_dc, 64, face);
  if (wcscmp(face, L"Segoe Fluent Icons") != 0) {
    DeleteObject(font_icon.h);
    make_font(&font_icon, L"Segoe MDL2 Assets", 11, FW_NORMAL);
  }
}


/* ------------------------------------------------------------- drawing */

static float round_coverage(float px, float py, mu_Rect r, float rad) {
  float l = r.x + rad, rt = r.x + r.w - rad, t = r.y + rad, b = r.y + r.h - rad;
  float dx = px < l ? l - px : (px > rt ? px - rt : 0);
  float dy = py < t ? t - py : (py > b ? py - b : 0);
  float cov;
  if (dx <= 0 || dy <= 0) { return 1.0f; }
  cov = rad - sqrtf(dx * dx + dy * dy) + 0.5f;
  return cov <= 0 ? 0 : (cov >= 1 ? 1 : cov);
}

static mu_Rect intersect(mu_Rect a, mu_Rect b) {
  int x1 = mu_max(a.x, b.x), y1 = mu_max(a.y, b.y);
  int x2 = mu_min(a.x + a.w, b.x + b.w), y2 = mu_min(a.y + a.h, b.y + b.h);
  if (x2 < x1) { x2 = x1; }
  if (y2 < y1) { y2 = y1; }
  return mu_rect(x1, y1, x2 - x1, y2 - y1);
}

static void fill(UiWindow *w, mu_Rect r, mu_Color c, int radius, mu_Rect clip) {
  mu_Rect area;
  float rad = (float)radius;
  int x, y;
  if (c.a == 0) { return; }
  area = intersect(intersect(r, clip), mu_rect(0, 0, w->w, w->h));
  if (area.w <= 0 || area.h <= 0) { return; }
  if (rad * 2 > r.w) { rad = r.w * 0.5f; }
  if (rad * 2 > r.h) { rad = r.h * 0.5f; }
  GdiFlush();
  for (y = area.y; y < area.y + area.h; y++) {
    uint32_t *row = w->pixels + (size_t)y * w->w;
    for (x = area.x; x < area.x + area.w; x++) {
      int a = c.a;
      if (rad > 0) {
        float cov = round_coverage(x + 0.5f, y + 0.5f, r, rad);
        if (cov <= 0) { continue; }
        a = (int)(a * cov);
      }
      if (a >= 255) {
        row[x] = (c.r << 16) | (c.g << 8) | c.b;
      } else {
        uint32_t d = row[x];
        int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
        dr = (c.r * a + dr * (255 - a)) / 255;
        dg = (c.g * a + dg * (255 - a)) / 255;
        db = (c.b * a + db * (255 - a)) / 255;
        row[x] = (dr << 16) | (dg << 8) | db;
      }
    }
  }
}

static void draw_wide(UiWindow *w, const wchar_t *s, int n, int x, int y, UiFont *f, mu_Color c) {
  SelectObject(w->memdc, f->h);
  SetTextColor(w->memdc, RGB(c.r, c.g, c.b));
  TextOutW(w->memdc, x, y, s, n);
}

static void render(UiWindow *w) {
  mu_Command *cmd = NULL;
  mu_Rect surface = mu_rect(0, 0, w->w, w->h);
  SetBkMode(w->memdc, TRANSPARENT);
  SelectClipRgn(w->memdc, NULL);
  while (mu_next_command(w->ctx, &cmd)) {
    switch (cmd->type) {
      case MU_COMMAND_RECT:
        fill(w, cmd->rect.rect, cmd->rect.color, 0, surface);
        break;
      case CMD_ROUND: {
        RoundCmd *rc = (RoundCmd*)cmd;
        fill(w, rc->rect, rc->color, rc->radius, rc->clip);
        break;
      }
      case MU_COMMAND_TEXT: {
        wchar_t buf[4096];
        int n = utf8_to_wide(cmd->text.str, -1, buf, 4096);
        draw_wide(w, buf, n, cmd->text.pos.x, cmd->text.pos.y, font_of(cmd->text.font), cmd->text.color);
        break;
      }
      case MU_COMMAND_ICON: {
        mu_Rect r = cmd->icon.rect;
        WCHAR g = ICON_GLYPHS[cmd->icon.id];
        SIZE sz;
        SelectObject(w->memdc, font_icon.h);
        GetTextExtentPoint32W(w->memdc, &g, 1, &sz);
        draw_wide(w, &g, 1, r.x + (r.w - sz.cx) / 2, r.y + (r.h - sz.cy) / 2, &font_icon, cmd->icon.color);
        break;
      }
      case MU_COMMAND_CLIP: {
        mu_Rect r = intersect(cmd->clip.rect, surface);
        HRGN rgn = CreateRectRgn(r.x, r.y, r.x + r.w, r.y + r.h);
        SelectClipRgn(w->memdc, rgn);
        DeleteObject(rgn);
        break;
      }
    }
  }
  GdiFlush();
}

void ui_fill_round(mu_Context *ctx, mu_Rect r, mu_Color c, int radius) {
  RoundCmd *cmd;
  if (mu_check_clip(ctx, r) == MU_CLIP_ALL) { return; }
  cmd = (RoundCmd*)mu_push_command(ctx, CMD_ROUND, sizeof(RoundCmd));
  cmd->rect = r;
  cmd->clip = mu_get_clip_rect(ctx);
  cmd->color = c;
  cmd->radius = radius;
}

static void draw_frame(mu_Context *ctx, mu_Rect rect, int colorid) {
  mu_Color c = ctx->style->colors[colorid];
  int rad = S(6);
  switch (colorid) {
    case MU_COLOR_WINDOWBG:
    case MU_COLOR_TITLEBG:
      mu_draw_rect(ctx, rect, c);
      return;
    case MU_COLOR_SCROLLBASE:
    case MU_COLOR_SCROLLTHUMB:
      ui_fill_round(ctx, rect, c, rect.w / 2);
      return;
    case MU_COLOR_BASE:
    case MU_COLOR_BASEHOVER:
    case MU_COLOR_BASEFOCUS: {
      mu_Color border = colorid == MU_COLOR_BASEFOCUS ? COL_ACCENT : ctx->style->colors[MU_COLOR_BORDER];
      ui_fill_round(ctx, rect, border, rad);
      rect = mu_rect(rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2);
      rad -= 1;
      break;
    }
  }
  ui_fill_round(ctx, rect, c, rad);
}

static void style_init(mu_Context *ctx) {
  mu_Style *s = ctx->style;
  s->font = &font_normal;
  s->padding = S(8);
  s->spacing = S(6);
  s->indent = S(16);
  s->size = mu_vec2(S(80), S(32) - s->padding * 2);
  s->title_height = S(28);
  s->scrollbar_size = S(6);
  s->thumb_size = S(24);
  s->colors[MU_COLOR_TEXT]        = mu_color(232, 234, 240, 255);
  s->colors[MU_COLOR_BORDER]      = mu_color(52, 54, 66, 255);
  s->colors[MU_COLOR_WINDOWBG]    = mu_color(22, 23, 28, 255);
  s->colors[MU_COLOR_TITLEBG]     = mu_color(16, 17, 21, 255);
  s->colors[MU_COLOR_TITLETEXT]   = mu_color(232, 234, 240, 255);
  s->colors[MU_COLOR_PANELBG]     = mu_color(27, 28, 34, 255);
  s->colors[MU_COLOR_BUTTON]      = mu_color(42, 44, 54, 255);
  s->colors[MU_COLOR_BUTTONHOVER] = mu_color(54, 57, 70, 255);
  s->colors[MU_COLOR_BUTTONFOCUS] = mu_color(68, 72, 88, 255);
  s->colors[MU_COLOR_BASE]        = mu_color(31, 32, 39, 255);
  s->colors[MU_COLOR_BASEHOVER]   = mu_color(36, 38, 46, 255);
  s->colors[MU_COLOR_BASEFOCUS]   = mu_color(36, 38, 46, 255);
  s->colors[MU_COLOR_SCROLLBASE]  = mu_color(0, 0, 0, 0);
  s->colors[MU_COLOR_SCROLLTHUMB] = mu_color(70, 73, 88, 255);
}


/* ------------------------------------------------------------- helpers */

void ui_label_color(mu_Context *ctx, const char *text, mu_Color color) {
  mu_Color old = ctx->style->colors[MU_COLOR_TEXT];
  ctx->style->colors[MU_COLOR_TEXT] = color;
  mu_label(ctx, text);
  ctx->style->colors[MU_COLOR_TEXT] = old;
}

void ui_label_font(mu_Context *ctx, const char *text, UiFont *font, mu_Color color) {
  mu_Font old = ctx->style->font;
  ctx->style->font = font;
  ui_label_color(ctx, text, color);
  ctx->style->font = old;
}

void ui_text_font(mu_Context *ctx, const char *text, UiFont *font, mu_Color color) {
  mu_Font oldf = ctx->style->font;
  mu_Color oldc = ctx->style->colors[MU_COLOR_TEXT];
  ctx->style->font = font;
  ctx->style->colors[MU_COLOR_TEXT] = color;
  mu_text(ctx, text);
  ctx->style->font = oldf;
  ctx->style->colors[MU_COLOR_TEXT] = oldc;
}

void ui_label_right(mu_Context *ctx, const char *text, mu_Color color) {
  mu_Color old = ctx->style->colors[MU_COLOR_TEXT];
  ctx->style->colors[MU_COLOR_TEXT] = color;
  mu_draw_control_text(ctx, text, mu_layout_next(ctx), MU_COLOR_TEXT, MU_OPT_ALIGNRIGHT);
  ctx->style->colors[MU_COLOR_TEXT] = old;
}

int ui_accent_button(mu_Context *ctx, const char *label) {
  mu_Color *c = ctx->style->colors;
  mu_Color b = c[MU_COLOR_BUTTON], h = c[MU_COLOR_BUTTONHOVER], f = c[MU_COLOR_BUTTONFOCUS];
  int res;
  c[MU_COLOR_BUTTON] = COL_ACCENT;
  c[MU_COLOR_BUTTONHOVER] = mu_color(118, 121, 248, 255);
  c[MU_COLOR_BUTTONFOCUS] = mu_color(82, 85, 220, 255);
  res = mu_button(ctx, label);
  c[MU_COLOR_BUTTON] = b;
  c[MU_COLOR_BUTTONHOVER] = h;
  c[MU_COLOR_BUTTONFOCUS] = f;
  return res;
}

void ui_spacer(mu_Context *ctx, int h) {
  int w = -1;
  mu_layout_row(ctx, 1, &w, h);
  mu_layout_next(ctx);
}

void ui_heading(mu_Context *ctx, const char *text) {
  int w = -1;
  ui_spacer(ctx, S(4));
  mu_layout_row(ctx, 1, &w, font_bold.height + S(4));
  ui_label_font(ctx, text, &font_bold, ctx->style->colors[MU_COLOR_TEXT]);
}

void ui_level_bar(mu_Context *ctx, int box) {
  mu_Rect r = mu_layout_next(ctx);
  int gap = S(3), seg = (r.w - gap * (MAX_BOX - 1)) / MAX_BOX, h = S(6), i;
  int y = r.y + (r.h - h) / 2;
  for (i = 0; i < MAX_BOX; i++) {
    mu_Color c = i < box ? (box >= 4 ? COL_GOOD : COL_ACCENT) : mu_color(48, 50, 62, 255);
    ui_fill_round(ctx, mu_rect(r.x + i * (seg + gap), y, seg, h), c, h / 2);
  }
}

int ui_begin_main(UiWindow *w, int opt) {
  mu_Container *c = mu_get_container(w->ctx, "main");
  c->rect = mu_rect(0, 0, w->w, w->h);
  return mu_begin_window_ex(w->ctx, "main", c->rect,
    opt | MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE);
}


/* ------------------------------------------------------------- windows */

static void run_frame(UiWindow *w) {
  mu_begin(w->ctx);
  w->frame(w);
  mu_end(w->ctx);
}

void ui_refresh(UiWindow *w) {
  int i;
  if (!w || !w->bmp || !w->frame) { return; } /* WM_SIZE arrives during CreateWindow */
  if (w->busy) { w->dirty = 1; return; }
  w->busy = 1;
  for (i = 0; i < 4; i++) {
    w->dirty = 0;
    /* second pass lets hover/focus settle after the input is consumed */
    run_frame(w);
    run_frame(w);
    if (w->post_frame) { w->post_frame(w); }
    if (!w->dirty) { break; }
  }
  render(w);
  InvalidateRect(w->hwnd, NULL, FALSE);
  w->busy = 0;
}

void ui_close(UiWindow *w) { PostMessageW(w->hwnd, WM_CLOSE, 0, 0); }

static void resize_surface(UiWindow *w, int cw, int ch) {
  BITMAPINFO bi;
  if (cw < 1) { cw = 1; }
  if (ch < 1) { ch = 1; }
  if (w->bmp && cw == w->w && ch == w->h) { return; }
  if (w->bmp) { SelectObject(w->memdc, w->oldbmp); DeleteObject(w->bmp); }
  memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = cw;
  bi.bmiHeader.biHeight = -ch;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  w->bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void**)&w->pixels, NULL, 0);
  w->oldbmp = SelectObject(w->memdc, w->bmp);
  w->w = cw;
  w->h = ch;
}

static void feed_text(UiWindow *w, const char *s) {
  /* microui's input buffer is 32 bytes: feed in chunks on utf-8 boundaries */
  char chunk[32];
  while (*s) {
    int n = 0;
    while (s[n] && n < 28) { n++; }
    while (s[n] && n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) { n--; }
    memcpy(chunk, s, n);
    chunk[n] = 0;
    mu_input_text(w->ctx, chunk);
    run_frame(w);
    s += n;
  }
}

static void paste(UiWindow *w) {
  HANDLE h;
  if (!OpenClipboard(w->hwnd)) { return; }
  h = GetClipboardData(CF_UNICODETEXT);
  if (h) {
    const wchar_t *src = GlobalLock(h);
    if (src) {
      static char buf[8192];
      char *p;
      wide_to_utf8(src, -1, buf, sizeof(buf));
      for (p = buf; *p; p++) { if (*p == '\r' || *p == '\n' || *p == '\t') { *p = ' '; } }
      GlobalUnlock(h);
      feed_text(w, buf);
    }
  }
  CloseClipboard();
}

void clipboard_set(const char *utf8) {
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n * sizeof(wchar_t));
  if (!mem) { return; }
  MultiByteToWideChar(CP_UTF8, 0, utf8, -1, GlobalLock(mem), n);
  GlobalUnlock(mem);
  if (OpenClipboard(NULL)) {
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, mem)) { GlobalFree(mem); }
    CloseClipboard();
  } else {
    GlobalFree(mem);
  }
}

static int key_map(WPARAM vk) {
  switch (vk) {
    case VK_SHIFT:   return MU_KEY_SHIFT;
    case VK_CONTROL: return MU_KEY_CTRL;
    case VK_MENU:    return MU_KEY_ALT;
    case VK_BACK:    return MU_KEY_BACKSPACE;
    case VK_RETURN:  return MU_KEY_RETURN;
  }
  return 0;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  UiWindow *w = (UiWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (msg == WM_NCCREATE) {
    w = ((CREATESTRUCTW*)lp)->lpCreateParams;
    w->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)w);
  }
  if (!w) { return DefWindowProcW(hwnd, msg, wp, lp); }

  switch (msg) {
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) {
        resize_surface(w, LOWORD(lp), HIWORD(lp));
        ui_refresh(w);
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      BitBlt(dc, 0, 0, w->w, w->h, w->memdc, 0, 0, SRCCOPY);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
      TrackMouseEvent(&t);
      mu_input_mousemove(w->ctx, (short)LOWORD(lp), (short)HIWORD(lp));
      ui_refresh(w);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (!w->ctx->mouse_down) {
        mu_input_mousemove(w->ctx, -10000, -10000);
        ui_refresh(w);
      }
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      SetCapture(hwnd);
      mu_input_mousedown(w->ctx, (short)LOWORD(lp), (short)HIWORD(lp), MU_MOUSE_LEFT);
      ui_refresh(w);
      return 0;
    case WM_LBUTTONUP:
      ReleaseCapture();
      mu_input_mouseup(w->ctx, (short)LOWORD(lp), (short)HIWORD(lp), MU_MOUSE_LEFT);
      ui_refresh(w);
      return 0;
    case WM_MOUSEWHEEL:
      mu_input_scroll(w->ctx, 0, -GET_WHEEL_DELTA_WPARAM(wp) * S(60) / WHEEL_DELTA);
      ui_refresh(w);
      return 0;
    case WM_KEYDOWN: {
      int k = key_map(wp);
      if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) { paste(w); }
      if (w->on_key) { w->on_key(w, (int)wp); }
      if (k) { mu_input_keydown(w->ctx, k); }
      ui_refresh(w);
      return 0;
    }
    case WM_KEYUP: {
      int k = key_map(wp);
      if (k) { mu_input_keyup(w->ctx, k); ui_refresh(w); }
      return 0;
    }
    case WM_CHAR: {
      static wchar_t high;
      wchar_t ch[3] = { 0 };
      char utf8[8];
      if (wp >= 0xD800 && wp <= 0xDBFF) { high = (wchar_t)wp; return 0; }
      if (wp < 32 || wp == 127) { return 0; }
      if (wp >= 0xDC00 && wp <= 0xDFFF) { ch[0] = high; ch[1] = (wchar_t)wp; }
      else { ch[0] = (wchar_t)wp; }
      wide_to_utf8(ch, -1, utf8, sizeof(utf8));
      mu_input_text(w->ctx, utf8);
      ui_refresh(w);
      return 0;
    }
    case WM_NCHITTEST:
      if (w->hittest) {
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int r;
        ScreenToClient(hwnd, &p);
        r = w->hittest(w, p.x, p.y);
        if (r) { return r; }
      }
      break;
    case WM_GETMINMAXINFO:
      if (w->min_w) {
        MINMAXINFO *mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = w->min_w;
        mm->ptMinTrackSize.y = w->min_h;
      }
      return 0;
    case WM_ACTIVATE:
      ui_refresh(w);
      break;
    case WM_DESTROY:
      if (w->on_destroy) { w->on_destroy(w); }
      return 0;
    case WM_NCDESTROY:
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      if (w->bmp) { SelectObject(w->memdc, w->oldbmp); DeleteObject(w->bmp); }
      DeleteDC(w->memdc);
      free(w->ctx);
      free(w);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

UiWindow *ui_create(const wchar_t *title, int popup, int x, int y, int cw, int ch) {
  UiWindow *w = calloc(1, sizeof(UiWindow));
  DWORD style = popup ? WS_POPUP : WS_OVERLAPPEDWINDOW;
  DWORD ex = popup ? (WS_EX_TOPMOST | WS_EX_TOOLWINDOW) : 0;
  RECT r = { 0, 0, cw, ch };
  BOOL dark = TRUE;
  int corner = 2; /* DWMWCP_ROUND */

  w->ctx = malloc(sizeof(mu_Context));
  mu_init(w->ctx);
  w->ctx->text_width = text_width;
  w->ctx->text_height = text_height;
  w->ctx->draw_frame = draw_frame;
  style_init(w->ctx);
  w->memdc = CreateCompatibleDC(NULL);

  AdjustWindowRectEx(&r, style, FALSE, ex);
  CreateWindowExW(ex, popup ? CLASS_POPUP : CLASS_NORMAL, title, style,
    x, y, r.right - r.left, r.bottom - r.top, NULL, NULL, GetModuleHandleW(NULL), w);
  DwmSetWindowAttribute(w->hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
  DwmSetWindowAttribute(w->hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
  SendMessageW(w->hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon_big);
  SendMessageW(w->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon_small);
  return w;
}


/* ---------------------------------------------------------------- icon */

HICON ui_make_icon(int size) {
  BITMAPINFO bi;
  uint32_t *px, *mask_px;
  HDC dc = CreateCompatibleDC(NULL);
  HBITMAP color, text, mask, old;
  HFONT font;
  RECT rc = { 0, 0, size, size };
  ICONINFO ii;
  HICON icon;
  mu_Rect shape = mu_rect(0, 0, size, size);
  int i;

  memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, NULL, 0);
  text = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&mask_px, NULL, 0);
  memset(mask_px, 0, size * size * 4);

  /* render the glyph white-on-black, then use it as a blend mask */
  old = SelectObject(dc, text);
  font = CreateFontW(-(size * 4 / 5), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  SelectObject(dc, font);
  SetTextColor(dc, RGB(255, 255, 255));
  SetBkMode(dc, TRANSPARENT);
  DrawTextW(dc, L"?", 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  GdiFlush();

  for (i = 0; i < size * size; i++) {
    int x = i % size, y = i / size;
    float cov = round_coverage(x + 0.5f, y + 0.5f, shape, size * 0.28f);
    int t = mask_px[i] & 0xFF;
    int r = COL_ACCENT.r + (255 - COL_ACCENT.r) * t / 255;
    int g = COL_ACCENT.g + (255 - COL_ACCENT.g) * t / 255;
    int b = COL_ACCENT.b + (255 - COL_ACCENT.b) * t / 255;
    px[i] = ((uint32_t)(cov * 255) << 24) | (r << 16) | (g << 8) | b;
  }

  mask = CreateBitmap(size, size, 1, 1, NULL);
  ii.fIcon = TRUE;
  ii.xHotspot = ii.yHotspot = 0;
  ii.hbmMask = mask;
  ii.hbmColor = color;
  icon = CreateIconIndirect(&ii);

  SelectObject(dc, old);
  DeleteObject(font);
  DeleteObject(text);
  DeleteObject(color);
  DeleteObject(mask);
  DeleteDC(dc);
  return icon;
}


/* ---------------------------------------------------------------- init */

void ui_init(void) {
  WNDCLASSEXW wc;
  HDC screen = GetDC(NULL);
  ui_scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0f;
  ReleaseDC(NULL, screen);
  measure_dc = CreateCompatibleDC(NULL);

  make_font(&font_normal, L"Segoe UI", 15, FW_NORMAL);
  make_font(&font_bold, L"Segoe UI Semibold", 15, FW_SEMIBOLD);
  make_font(&font_big, L"Segoe UI Semibold", 20, FW_SEMIBOLD);
  make_font(&font_small, L"Segoe UI", 13, FW_NORMAL);
  make_icon_font();

  icon_big = ui_make_icon(GetSystemMetrics(SM_CXICON));
  icon_small = ui_make_icon(GetSystemMetrics(SM_CXSMICON));

  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hIcon = icon_big;
  wc.hIconSm = icon_small;
  wc.lpszClassName = CLASS_NORMAL;
  RegisterClassExW(&wc);
  wc.style = CS_DROPSHADOW;
  wc.lpszClassName = CLASS_POPUP;
  RegisterClassExW(&wc);
}
