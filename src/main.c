/* WaitWhat: tray app that pops a random flashcard at random intervals */
#include "app.h"
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#define WM_TRAY      (WM_APP + 1)
#define WM_SHOW_MAIN (WM_APP + 2)
#define WM_NET_DONE  (WM_APP + 3)
#define TRAY_CLASS   L"WaitWhatTray"
#define HOTKEY_ID    1

enum { MENU_ASK = 1, MENU_OPEN, MENU_PAUSE, MENU_STARTUP, MENU_QUIT };

static HWND tray_hwnd;
static NOTIFYICONDATAW nid;
static UINT msg_taskbar_created;
static UiWindow *popup, *manager;
static time_t next_due;
static int settings_dirty;
static int autostart_on; /* mirrors the registry Run entry */

static void open_manager(void);
static void ask_question(int from_user);


/* ----------------------------------------------------------- scheduler */

static void schedule_next(void) {
  double lo = settings.min_minutes * 60.0, hi = settings.max_minutes * 60.0;
  if (hi < lo) { double t = lo; lo = hi; hi = t; }
  next_due = time(NULL) + (time_t)(lo + rng01() * (hi - lo));
}

static int user_busy(void) {
  QUERY_USER_NOTIFICATION_STATE st;
  if (SHQueryUserNotificationState(&st) != S_OK) { return 0; }
  return st == QUNS_BUSY || st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE;
}

static void status_text(char *buf, int cap) {
  long long left = (long long)(next_due - time(NULL));
  if (popup) { snprintf(buf, cap, "Question waiting for you"); }
  else if (settings.paused) { snprintf(buf, cap, "Paused"); }
  else if (left < 0) { snprintf(buf, cap, "Next question any moment"); }
  else if (left < 60) { snprintf(buf, cap, "Next question in %llds", left); }
  else { snprintf(buf, cap, "Next question in %lldm %02llds", left / 60, left % 60); }
}


/* ---------------------------------------------------------------- tray */

static void tray_update_tip(void) {
  char status[96], tip[128];
  long long left = (long long)(next_due - time(NULL));
  if (popup) { snprintf(status, sizeof(status), "question waiting"); }
  else if (settings.paused) { snprintf(status, sizeof(status), "paused"); }
  else { snprintf(status, sizeof(status), "next question in ~%lld min", left > 0 ? (left + 59) / 60 : 0); }
  snprintf(tip, sizeof(tip), APP_NAME " - %s", status);
  utf8_to_wide(tip, -1, nid.szTip, 128);
  nid.uFlags = NIF_TIP;
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void tray_add(void) {
  memset(&nid, 0, sizeof(nid));
  nid.cbSize = sizeof(nid);
  nid.hWnd = tray_hwnd;
  nid.uID = 1;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid.uCallbackMessage = WM_TRAY;
  nid.hIcon = ui_make_icon(GetSystemMetrics(SM_CXSMICON));
  wcscpy(nid.szTip, APP_NAMEW);
  Shell_NotifyIconW(NIM_ADD, &nid);
}

static void balloon(const char *title, const char *text) {
  utf8_to_wide(title, -1, nid.szInfoTitle, 64);
  utf8_to_wide(text, -1, nid.szInfo, 256);
  nid.dwInfoFlags = NIIF_NONE;
  nid.uFlags = NIF_INFO;
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void tray_menu(void) {
  HMENU m = CreatePopupMenu();
  POINT p;
  char status[96];
  wchar_t wstatus[96];
  int cmd;
  status_text(status, sizeof(status));
  utf8_to_wide(status, -1, wstatus, 96);
  AppendMenuW(m, MF_STRING | MF_GRAYED, 0, wstatus);
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  AppendMenuW(m, MF_STRING, MENU_ASK, L"Ask me now\tCtrl+Alt+W");
  AppendMenuW(m, MF_STRING, MENU_OPEN, L"Open WaitWhat...");
  AppendMenuW(m, MF_STRING | (settings.paused ? MF_CHECKED : 0), MENU_PAUSE, L"Pause");
  AppendMenuW(m, MF_STRING | (autostart_get() ? MF_CHECKED : 0), MENU_STARTUP, L"Start with Windows");
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  AppendMenuW(m, MF_STRING, MENU_QUIT, L"Quit");
  SetMenuDefaultItem(m, MENU_OPEN, FALSE);
  GetCursorPos(&p);
  SetForegroundWindow(tray_hwnd);
  cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, tray_hwnd, NULL);
  PostMessageW(tray_hwnd, WM_NULL, 0, 0);
  DestroyMenu(m);
  switch (cmd) {
    case MENU_ASK: ask_question(1); break;
    case MENU_OPEN: open_manager(); break;
    case MENU_PAUSE:
      settings.paused = !settings.paused;
      if (!settings.paused) { schedule_next(); }
      settings_save();
      if (manager) { ui_refresh(manager); }
      break;
    case MENU_STARTUP:
      autostart_on = !autostart_get();
      autostart_set(autostart_on);
      if (manager) { ui_refresh(manager); }
      break;
    case MENU_QUIT: DestroyWindow(tray_hwnd); break;
  }
  tray_update_tip();
}


/* --------------------------------------------------------------- popup */

enum { RESULT_GAVE_UP = 3 };

static struct {
  int answered;
  char deck[256];
  char deck_file[MAX_PATH];
  char q[2048];
  char note[2048];
  char ans[16][512];
  const char *ans_ptr[16];
  int nans;
  char answer_line[2048];
  uint64_t key;
  char input[512];
  int result, overridden;
  Prog before;
} pq;

static int counted_right(void) {
  int right = pq.result == ANSWER_EXACT || pq.result == ANSWER_CLOSE;
  return right ^ pq.overridden;
}

static void popup_submit(int gave_up) {
  pq.result = (gave_up || !pq.input[0]) ? RESULT_GAVE_UP : check_answer(pq.ans_ptr, pq.nans, pq.input);
  pq.overridden = 0;
  pq.before = prog_get(pq.key);
  prog_record(pq.key, counted_right());
  progress_save();
  history_add(pq.deck_file, 1, counted_right());
  pq.answered = 1;
}

static void popup_override(void) {
  pq.overridden = !pq.overridden;
  prog_set(pq.before);
  prog_record(pq.key, counted_right());
  progress_save();
  history_add(pq.deck_file, 0, counted_right() ? 1 : -1);
}

static void popup_frame(UiWindow *w) {
  mu_Context *ctx = w->ctx;
  int full = -1;
  if (!ui_begin_main(w, 0)) { return; }

  /* header: deck name + close */
  mu_layout_row(ctx, 2, (int[]){ -S(30), -1 }, S(24));
  ui_label_font(ctx, pq.deck, &font_small, COL_DIM);
  if (mu_button_ex(ctx, NULL, MU_ICON_CLOSE, MU_OPT_ALIGNCENTER)) { ui_close(w); }

  mu_layout_row(ctx, 1, &full, 0);
  ui_text_font(ctx, pq.q, &font_big, ctx->style->colors[MU_COLOR_TEXT]);
  ui_spacer(ctx, S(2));

  if (!pq.answered) {
    int res, submit = 0, gave_up = 0;
    mu_Id box;
    mu_layout_row(ctx, 1, &full, S(36));
    res = mu_textbox_ex(ctx, pq.input, sizeof(pq.input), 0);
    box = ctx->last_id;
    if ((res & MU_RES_SUBMIT) || (ctx->key_pressed & MU_KEY_RETURN)) { submit = 1; }
    mu_layout_row(ctx, 4, (int[]){ S(90), S(124), -S(76), -1 }, S(32));
    if (ui_accent_button(ctx, "Check")) { submit = 1; }
    if (mu_button(ctx, "Show answer")) { submit = gave_up = 1; }
    mu_layout_next(ctx);
    if (mu_button(ctx, "Skip")) { ui_close(w); }
    mu_layout_row(ctx, 1, &full, font_small.height);
    ui_label_font(ctx, "Enter to check  \xC2\xB7  Esc to skip  \xC2\xB7  Ctrl+Alt+W to jump here", &font_small, COL_DIM);
    /* keep typing focus in the answer box unless a button is being pressed */
    if (ctx->focus == 0) { mu_set_focus(ctx, box); }
    if (submit) { popup_submit(gave_up); }
  } else {
    int right = counted_right();
    const char *title;
    mu_Color color;
    char buf[2600];
    Prog p = prog_get(pq.key);

    if (pq.overridden) { title = right ? "Marked as correct" : "Marked as wrong"; color = right ? COL_GOOD : COL_BAD; }
    else if (pq.result == ANSWER_EXACT) { title = "Correct!"; color = COL_GOOD; }
    else if (pq.result == ANSWER_CLOSE) { title = "Close enough - check the spelling"; color = COL_WARN; }
    else if (pq.result == RESULT_GAVE_UP) { title = "Here's the answer"; color = COL_ACCENT; }
    else { title = "Not quite"; color = COL_BAD; }

    mu_layout_row(ctx, 1, &full, font_bold.height + S(4));
    ui_label_font(ctx, title, &font_bold, color);
    mu_layout_row(ctx, 1, &full, 0);
    snprintf(buf, sizeof(buf), "Answer: %s", pq.answer_line);
    mu_text(ctx, buf);
    if (pq.input[0] && pq.result != ANSWER_EXACT) {
      snprintf(buf, sizeof(buf), "You wrote: %s", pq.input);
      ui_text_font(ctx, buf, &font_normal, COL_DIM);
    }
    if (pq.note[0]) {
      ui_spacer(ctx, S(2));
      mu_layout_row(ctx, 1, &full, 0);
      ui_text_font(ctx, pq.note, &font_small, COL_DIM);
    }
    ui_spacer(ctx, S(2));
    mu_layout_row(ctx, 2, (int[]){ S(70), S(130) }, S(18));
    snprintf(buf, sizeof(buf), "Level %d/%d", p.box, MAX_BOX);
    ui_label_font(ctx, buf, &font_small, COL_DIM);
    ui_level_bar(ctx, p.box);

    mu_layout_row(ctx, 3, (int[]){ S(90), -S(150), -1 }, S(32));
    if (ui_accent_button(ctx, "Done") || (ctx->key_pressed & MU_KEY_RETURN)) { ui_close(w); }
    mu_layout_next(ctx);
    if (pq.result != RESULT_GAVE_UP || pq.overridden) {
      if (mu_button(ctx, right ? "Count it wrong" : "I was right")) { popup_override(); }
    }
  }
  mu_end_window(ctx);
}

static void popup_post(UiWindow *w) {
  mu_Container *c = mu_get_container(w->ctx, "main");
  RECT wa, wr;
  int want;
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  want = c->content_size.y + w->ctx->style->padding * 2;
  want = mu_clamp(want, S(120), (wa.bottom - wa.top) * 4 / 5);
  if (abs(want - w->h) > 1) {
    /* grow/shrink upwards, keeping the bottom edge anchored */
    GetWindowRect(w->hwnd, &wr);
    SetWindowPos(w->hwnd, NULL, wr.left, wr.bottom - want, wr.right - wr.left, want,
      SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

static int popup_hittest(UiWindow *w, int x, int y) {
  /* drag the popup by its header */
  return (y < S(36) && x < w->w - S(40)) ? HTCAPTION : 0;
}

static void popup_key(UiWindow *w, int vk) {
  if (vk == VK_ESCAPE) { ui_close(w); }
}

static void popup_destroyed(UiWindow *w) {
  (void)w;
  popup = NULL;
  schedule_next();
  tray_update_tip();
  if (manager) { ui_refresh(manager); }
}

static void ask_question(int from_user) {
  int di, ci, i;
  Card *c;
  RECT wa;
  int width = S(460), height = S(220);

  if (popup) {
    ShowWindow(popup->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(popup->hwnd);
    return;
  }
  if (!pick_card(&di, &ci)) {
    balloon("No cards to ask yet", "Open WaitWhat to add cards or turn a deck on.");
    schedule_next();
    return;
  }

  c = &decks[di].cards[ci];
  memset(&pq, 0, sizeof(pq));
  snprintf(pq.deck, sizeof(pq.deck), "%s%s", decks[di].name, decks[di].mastered ? "  \xC2\xB7  review" : "");
  snprintf(pq.deck_file, sizeof(pq.deck_file), "%s", decks[di].fname);
  snprintf(pq.q, sizeof(pq.q), "%s", c->q);
  snprintf(pq.note, sizeof(pq.note), "%s", c->note ? c->note : "");
  pq.nans = mu_min(c->nans, 16);
  for (i = 0; i < pq.nans; i++) {
    snprintf(pq.ans[i], sizeof(pq.ans[i]), "%s", c->ans[i]);
    pq.ans_ptr[i] = pq.ans[i];
    if (i > 0) { strncat(pq.answer_line, "  /  ", sizeof(pq.answer_line) - strlen(pq.answer_line) - 1); }
    strncat(pq.answer_line, pq.ans[i], sizeof(pq.answer_line) - strlen(pq.answer_line) - 1);
  }
  pq.key = c->key;

  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  popup = ui_create(APP_NAMEW, 1, wa.right - width - S(16), wa.bottom - height - S(16), width, height);
  popup->frame = popup_frame;
  popup->post_frame = popup_post;
  popup->hittest = popup_hittest;
  popup->on_key = popup_key;
  popup->on_destroy = popup_destroyed;
  ui_refresh(popup);

  /* don't steal focus from whatever you're typing in, unless you asked */
  if (from_user) {
    ShowWindow(popup->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(popup->hwnd);
  } else {
    ShowWindow(popup->hwnd, SW_SHOWNOACTIVATE);
    FLASHWINFO fi = { sizeof(fi), popup->hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0 };
    FlashWindowEx(&fi);
  }
  if (settings.sound) { MessageBeep(MB_ICONASTERISK); }
  tray_update_tip();
  if (manager) { ui_refresh(manager); }
}


/* ------------------------------------------------------------- manager */

enum { TAB_DECKS, TAB_STATS, TAB_AI, TAB_SETTINGS };
enum { ACT_NONE, ACT_CREATE, ACT_ADD, ACT_DELETE, ACT_RELOAD, ACT_FOLDER, ACT_RESET, ACT_COPY, ACT_ASK,
       ACT_SHARE, ACT_IMPORT, ACT_RESET_DECK, ACT_MASTERED };

static struct {
  int tab;
  int sel;              /* open deck, -1 = deck list */
  int confirm_del;      /* card index waiting for a second click */
  int confirm_reset;
  int confirm_deck_reset;
  int stats_deck;       /* deck shown in the Stats tab, -1 = all */
  char new_deck[128];
  char add_q[512], add_a[512], add_note[512];
  char ai_topic[256];
  float ai_count;
  DWORD copied_at;
  int action, act_deck, act_card;
  int reschedule;
  char import_code[32];
  char net_msg[300];    /* share/import status line */
  int net_good;
} mg = { .tab = TAB_DECKS, .sel = -1, .confirm_del = -1, .stats_deck = -1, .ai_count = 30 };

static void net_status(mu_Context *ctx) {
  int full = -1;
  if (net_job.running) {
    mu_layout_row(ctx, 1, &full, 0);
    ui_text_font(ctx, net_job.kind == NET_SHARE
      ? "Sharing... (doing a second of anti-spam work)" : "Downloading deck...", &font_small, COL_DIM);
  } else if (mg.net_msg[0]) {
    mu_layout_row(ctx, 1, &full, 0);
    ui_text_font(ctx, mg.net_msg, &font_small, mg.net_good ? COL_GOOD : COL_BAD);
  }
}

static void net_done(void) {
  char err[160];
  mg.net_good = net_job.ok;
  if (!net_job.ok) {
    snprintf(mg.net_msg, sizeof(mg.net_msg), "%s", net_job.msg);
  } else if (net_job.kind == NET_SHARE) {
    clipboard_set(net_job.code);
    snprintf(mg.net_msg, sizeof(mg.net_msg),
      "Share code: %s  (copied) - anyone can import it from the Decks tab.", net_job.code);
  } else {
    int i = deck_import(net_job.result, err, sizeof(err));
    if (i >= 0) {
      snprintf(mg.net_msg, sizeof(mg.net_msg), "Imported \"%s\".", decks[i].name);
      mg.sel = i;
      mg.import_code[0] = 0;
    } else {
      snprintf(mg.net_msg, sizeof(mg.net_msg), "%s", err);
      mg.net_good = 0;
    }
  }
  if (manager) { ui_refresh(manager); }
}

static void build_prompt(char *out, int cap) {
  char slug[96], dir[MAX_PATH * 3];
  const char *topic = mg.ai_topic[0] ? mg.ai_topic : "<TOPIC>";
  deck_slug(topic, slug, sizeof(slug));
  wide_to_utf8(dir_decks, -1, dir, sizeof(dir));
  snprintf(out, cap,
    "Create a flashcard deck for my WaitWhat learning app.\n"
    "\n"
    "Topic: %s\n"
    "Number of cards: %d\n"
    "\n"
    "Save it as JSON to this file:\n"
    "%s\\%s.json\n"
    "\n"
    "Use exactly this format:\n"
    "{\n"
    "  \"name\": \"%s\",\n"
    "  \"cards\": [\n"
    "    { \"q\": \"question text\", \"a\": \"answer\" },\n"
    "    { \"q\": \"question with several accepted answers\", \"a\": [\"answer\", \"other accepted answer\"] },\n"
    "    { \"q\": \"question\", \"a\": \"answer\", \"note\": \"optional explanation shown after answering\" }\n"
    "  ]\n"
    "}\n"
    "\n"
    "Rules:\n"
    "- I type each answer from memory and it is checked automatically (case and punctuation are ignored, "
    "small typos forgiven), so keep answers short: a word, a name, a number or a short phrase.\n"
    "- Put every reasonable alternative answer in the \"a\" list.\n"
    "- Every question must make sense on its own. No multiple choice, no \"which of these\".\n"
    "- Go from the fundamentals towards more advanced facts.\n"
    "- Add a short \"note\" where extra context helps it stick.\n"
    "- If the file already exists, keep its cards and add new ones without duplicates.\n",
    topic, (int)mg.ai_count, dir, slug, topic);
}

static void tab_button(mu_Context *ctx, const char *label, int tab) {
  if (mg.tab == tab) {
    if (ui_accent_button(ctx, label)) { mg.tab = tab; }
  } else if (mu_button(ctx, label)) {
    mg.tab = tab;
    mg.confirm_reset = 0;
  }
}

static void deck_list(mu_Context *ctx) {
  int full = -1, i;
  char buf[1024], dir[MAX_PATH * 3];

  ui_heading(ctx, "Your decks");
  if (ndecks == 0) {
    mu_layout_row(ctx, 1, &full, 0);
    ui_text_font(ctx, "No decks yet. Create one below, or have an AI write one for you (see the AI prompt tab).", &font_normal, COL_DIM);
  }
  for (i = 0; i < ndecks; i++) {
    Deck *d = &decks[i];
    mu_push_id(ctx, &i, sizeof(i));
    mu_layout_row(ctx, 4, (int[]){ S(30), -S(280), S(190), -1 }, S(32));
    if (mu_checkbox(ctx, "", &d->enabled) & MU_RES_CHANGE) { settings_dirty = 1; }
    ui_label_font(ctx, d->name, &font_bold, d->enabled ? ctx->style->colors[MU_COLOR_TEXT] : COL_DIM);
    if (d->mastered) {
      snprintf(buf, sizeof(buf), "Mastered  \xC2\xB7  %d card%s", d->ncards, d->ncards == 1 ? "" : "s");
      ui_label_color(ctx, buf, COL_GOOD);
    } else {
      snprintf(buf, sizeof(buf), "%d card%s  \xC2\xB7  %d learned", d->ncards, d->ncards == 1 ? "" : "s", deck_learned(d));
      ui_label_color(ctx, buf, COL_DIM);
    }
    if (mu_button(ctx, "Open")) { mg.sel = i; mg.confirm_del = -1; mg.confirm_deck_reset = 0; }
    if (d->err[0]) {
      mu_layout_row(ctx, 2, (int[]){ S(30), -1 }, 0);
      mu_layout_next(ctx);
      ui_text_font(ctx, d->err, &font_small, COL_BAD);
    }
    mu_pop_id(ctx);
  }

  ui_heading(ctx, "New deck");
  mu_layout_row(ctx, 2, (int[]){ -S(130), -1 }, S(32));
  if (mu_textbox(ctx, mg.new_deck, sizeof(mg.new_deck)) & MU_RES_SUBMIT) { mg.action = ACT_CREATE; }
  if (ui_accent_button(ctx, "Create deck")) { mg.action = ACT_CREATE; }

  ui_heading(ctx, "Import a shared deck");
  mu_layout_row(ctx, 2, (int[]){ -S(130), -1 }, S(32));
  if (mu_textbox(ctx, mg.import_code, sizeof(mg.import_code)) & MU_RES_SUBMIT) { mg.action = ACT_IMPORT; }
  if (mu_button(ctx, "Import code")) { mg.action = ACT_IMPORT; }
  net_status(ctx);

  ui_spacer(ctx, S(4));
  mu_layout_row(ctx, 2, (int[]){ S(150), S(170) }, S(32));
  if (mu_button(ctx, "Reload decks")) { mg.action = ACT_RELOAD; }
  if (mu_button(ctx, "Open decks folder")) { mg.action = ACT_FOLDER; }
  wide_to_utf8(dir_decks, -1, dir, sizeof(dir));
  snprintf(buf, sizeof(buf), "Decks are plain JSON files in %s - edits made there show up here automatically.", dir);
  mu_layout_row(ctx, 1, &full, 0);
  ui_text_font(ctx, buf, &font_small, COL_DIM);
}

/* learned bar, recent accuracy, mastered + reset buttons */
static void deck_progress(mu_Context *ctx, Deck *d) {
  int full = -1, learned = deck_learned(d), pct = d->ncards ? learned * 100 / d->ncards : 0;
  Tally week = history_range(7, d->fname);
  mu_Rect r;
  char buf[256];

  ui_spacer(ctx, S(2));
  mu_layout_row(ctx, 1, &full, S(6));
  r = mu_layout_next(ctx);
  ui_fill_round(ctx, r, mu_color(48, 50, 62, 255), r.h / 2);
  if (pct > 0) { ui_fill_round(ctx, mu_rect(r.x, r.y, mu_max(r.h, r.w * pct / 100), r.h), d->mastered || pct == 100 ? COL_GOOD : COL_ACCENT, r.h / 2); }

  if (week.n) {
    snprintf(buf, sizeof(buf), "%d of %d learned (%d%%)  \xC2\xB7  last 7 days: %d answers, %d%% right",
      learned, d->ncards, pct, week.n, week.right * 100 / week.n);
  } else {
    snprintf(buf, sizeof(buf), "%d of %d learned (%d%%)  \xC2\xB7  not practiced in the last 7 days", learned, d->ncards, pct);
  }
  mu_layout_row(ctx, 1, &full, S(22));
  ui_label_font(ctx, buf, &font_small, COL_DIM);
  if (!d->mastered && d->ncards && learned == d->ncards) {
    mu_layout_row(ctx, 1, &full, 0);
    ui_text_font(ctx, "You've learned every card in this deck. Mark it as mastered to only see it now and then for review.", &font_small, COL_GOOD);
  }

  mu_layout_row(ctx, 2, (int[]){ S(170), S(170) }, S(32));
  if (d->mastered ? mu_button(ctx, "Unmark mastered") : ui_accent_button(ctx, "Mark as mastered")) {
    mg.action = ACT_MASTERED;
    mg.act_deck = mg.sel;
  }
  if (mu_button(ctx, mg.confirm_deck_reset ? "Click again to reset" : "Reset progress")) {
    if (mg.confirm_deck_reset) { mg.action = ACT_RESET_DECK; mg.act_deck = mg.sel; mg.confirm_deck_reset = 0; }
    else { mg.confirm_deck_reset = 1; }
  }
  if (d->mastered) {
    mu_layout_row(ctx, 1, &full, 0);
    ui_text_font(ctx, "Mastered: questions from this deck only come up now and then, for review. Untick the deck to stop them completely.", &font_small, COL_DIM);
  }
}

static void deck_view(mu_Context *ctx) {
  Deck *d = &decks[mg.sel];
  int full = -1, i, add = 0, cw;
  char buf[256];

  mu_layout_row(ctx, 3, (int[]){ S(80), -S(90), -1 }, S(32));
  if (mu_button(ctx, "Back")) { mg.sel = -1; mg.net_msg[0] = 0; mg.confirm_deck_reset = 0; return; }
  snprintf(buf, sizeof(buf), "%s  (%d cards, %d learned)", d->name, d->ncards, deck_learned(d));
  ui_label_font(ctx, buf, &font_bold, ctx->style->colors[MU_COLOR_TEXT]);
  if (mu_button(ctx, net_job.running ? "..." : "Share")) { mg.action = ACT_SHARE; mg.act_deck = mg.sel; }
  net_status(ctx);
  if (d->err[0]) {
    mu_layout_row(ctx, 1, &full, 0);
    ui_text_font(ctx, d->err, &font_small, COL_BAD);
  }
  deck_progress(ctx, d);

  ui_heading(ctx, "Add a card");
  mu_layout_row(ctx, 2, (int[]){ S(80), -1 }, S(32));
  mu_label(ctx, "Question");
  if (mu_textbox(ctx, mg.add_q, sizeof(mg.add_q)) & MU_RES_SUBMIT) { add = 1; }
  mu_label(ctx, "Answer");
  if (mu_textbox(ctx, mg.add_a, sizeof(mg.add_a)) & MU_RES_SUBMIT) { add = 1; }
  mu_label(ctx, "Note");
  if (mu_textbox(ctx, mg.add_note, sizeof(mg.add_note)) & MU_RES_SUBMIT) { add = 1; }
  mu_layout_row(ctx, 3, (int[]){ S(80), S(110), -1 }, S(32));
  mu_layout_next(ctx);
  if (ui_accent_button(ctx, "Add card")) { add = 1; }
  ui_label_font(ctx, "Separate accepted answers with  |   \xC2\xB7  note is optional", &font_small, COL_DIM);
  if (add && mg.add_q[0] && mg.add_a[0]) { mg.action = ACT_ADD; mg.act_deck = mg.sel; }

  snprintf(buf, sizeof(buf), "Cards (%d)", d->ncards);
  ui_heading(ctx, buf);
  cw = (mu_get_current_container(ctx)->body.w - S(60) - S(84)) / 2;
  for (i = 0; i < d->ncards; i++) {
    Card *c = &d->cards[i];
    Prog p = prog_get(c->key);
    char ans[512] = "";
    int j;
    for (j = 0; j < c->nans; j++) {
      if (j) { strncat(ans, " / ", sizeof(ans) - strlen(ans) - 1); }
      strncat(ans, c->ans[j], sizeof(ans) - strlen(ans) - 1);
    }
    mu_push_id(ctx, &i, sizeof(i));
    mu_layout_row(ctx, 4, (int[]){ S(54), cw, -S(84), -1 }, S(30));
    ui_level_bar(ctx, p.box);
    mu_label(ctx, c->q);
    ui_label_color(ctx, ans, COL_DIM);
    if (mg.confirm_del == i) {
      if (mu_button(ctx, "Sure?")) { mg.action = ACT_DELETE; mg.act_deck = mg.sel; mg.act_card = i; }
    } else if (mu_button(ctx, "Delete")) {
      mg.confirm_del = i;
    }
    mu_pop_id(ctx);
  }
}

static void ai_tab(mu_Context *ctx) {
  static char prompt[4096];
  int full = -1;
  ui_heading(ctx, "Make a deck with AI");
  mu_layout_row(ctx, 1, &full, 0);
  ui_text_font(ctx,
    "Type what you want to learn, copy the prompt and give it to any AI that can write files "
    "(or paste its answer into the file yourself). The new deck appears here as soon as the file is saved.",
    &font_normal, COL_DIM);
  ui_spacer(ctx, S(4));
  mu_layout_row(ctx, 2, (int[]){ S(80), -1 }, S(32));
  mu_label(ctx, "Topic");
  mu_textbox(ctx, mg.ai_topic, sizeof(mg.ai_topic));
  mu_label(ctx, "Cards");
  mu_slider_ex(ctx, &mg.ai_count, 5, 200, 5, "%.0f cards", MU_OPT_ALIGNCENTER);
  mu_layout_row(ctx, 3, (int[]){ S(80), S(140), -1 }, S(32));
  mu_layout_next(ctx);
  if (ui_accent_button(ctx, "Copy prompt")) { mg.action = ACT_COPY; }
  if (mg.copied_at && GetTickCount() - mg.copied_at < 2500) { ui_label_color(ctx, "Copied to clipboard", COL_GOOD); }

  ui_heading(ctx, "Prompt preview");
  build_prompt(prompt, sizeof(prompt));
  mu_layout_row(ctx, 1, &full, 0);
  ui_text_font(ctx, prompt, &font_small, COL_DIM);
}

static mu_Color accuracy_color(int pct) {
  return pct >= 80 ? COL_GOOD : (pct >= 50 ? COL_WARN : COL_BAD);
}

static void percent(Tally t, char *out, int cap) {
  if (t.n) { snprintf(out, cap, "%d%%", t.right * 100 / t.n); }
  else { snprintf(out, cap, "-"); }
}

static void stat_tile(mu_Context *ctx, const char *value, const char *label, mu_Color color) {
  mu_Rect r = mu_layout_next(ctx);
  ui_fill_round(ctx, r, mu_color(30, 31, 38, 255), S(8));
  mu_draw_text(ctx, &font_big, value, -1, mu_vec2(r.x + S(12), r.y + S(10)), color);
  mu_draw_text(ctx, &font_small, label, -1, mu_vec2(r.x + S(12), r.y + S(14) + font_big.height), COL_DIM);
}

static void accuracy_chart(mu_Context *ctx, const char *fname) {
  const int days = 30;
  int full = -1, i, hovered = -1;
  mu_Rect r, plot;
  char buf[128], day[32];

  mu_layout_row(ctx, 1, &full, S(160));
  r = mu_layout_next(ctx);
  ui_fill_round(ctx, r, mu_color(27, 28, 34, 255), S(8));
  plot = mu_rect(r.x + S(44), r.y + S(14), r.w - S(58), r.h - S(28));

  /* gridlines at 0 / 50 / 100% */
  for (i = 0; i <= 100; i += 50) {
    int y = plot.y + plot.h - plot.h * i / 100;
    snprintf(buf, sizeof(buf), "%d%%", i);
    mu_draw_rect(ctx, mu_rect(plot.x, y, plot.w, 1), mu_color(44, 46, 56, 255));
    mu_draw_text(ctx, &font_small, buf, -1, mu_vec2(r.x + S(10), y - font_small.height / 2), COL_DIM);
  }

  for (i = 0; i < days; i++) {
    int ago = days - 1 - i;
    int x0 = plot.x + plot.w * i / days, x1 = plot.x + plot.w * (i + 1) / days;
    int bw = mu_max(S(2), (x1 - x0) - S(4)), bx = x0 + ((x1 - x0) - bw) / 2;
    Tally t = history_day(ago, fname);
    if (mu_mouse_over(ctx, mu_rect(x0, r.y, x1 - x0, r.h))) {
      hovered = ago;
      ui_fill_round(ctx, mu_rect(x0, plot.y, x1 - x0, plot.h), mu_color(255, 255, 255, 10), S(3));
    }
    if (t.n) {
      int pct = t.right * 100 / t.n, h = mu_max(S(3), plot.h * pct / 100);
      ui_fill_round(ctx, mu_rect(bx, plot.y + plot.h - h, bw, h), accuracy_color(pct), mu_min(S(3), bw / 2));
    } else {
      ui_fill_round(ctx, mu_rect(bx, plot.y + plot.h - S(2), bw, S(2)), mu_color(58, 61, 74, 255), 1);
    }
  }

  /* date labels every 7 days, counting back from today */
  mu_layout_row(ctx, 1, &full, font_small.height + S(2));
  r = mu_layout_next(ctx);
  for (i = days - 1; i >= 0; i -= 7) {
    int ago = days - 1 - i, cx = plot.x + plot.w * i / days + plot.w / days / 2, w;
    if (ago == 0) { snprintf(day, sizeof(day), "today"); }
    else { history_day_label(ago, day, sizeof(day)); }
    w = ctx->text_width(&font_small, day, -1);
    mu_draw_text(ctx, &font_small, day, -1, mu_vec2(cx - w / 2, r.y), COL_DIM);
  }

  mu_layout_row(ctx, 1, &full, S(22));
  if (hovered >= 0) {
    Tally t = history_day(hovered, fname);
    history_day_label(hovered, day, sizeof(day));
    if (t.n) {
      snprintf(buf, sizeof(buf), "%s: %d of %d right (%d%%)", day, t.right, t.n, t.right * 100 / t.n);
      ui_label_color(ctx, buf, accuracy_color(t.right * 100 / t.n));
    } else {
      snprintf(buf, sizeof(buf), "%s: no answers", day);
      ui_label_color(ctx, buf, COL_DIM);
    }
  } else {
    ui_label_font(ctx, "Bar height is the % you got right that day. Hover a bar for details.", &font_small, COL_DIM);
  }
}

static void stats_tab(mu_Context *ctx) {
  int i, tw, streak;
  const char *fname;
  char value[32], label[96];
  mu_Font old;
  Tally t;

  if (mg.stats_deck >= ndecks) { mg.stats_deck = -1; }
  fname = mg.stats_deck >= 0 ? decks[mg.stats_deck].fname : NULL;

  /* which deck to show */
  mu_layout_row(ctx, 3, (int[]){ S(36), -S(42), -1 }, S(32));
  if (mu_button(ctx, "<")) { mg.stats_deck = mg.stats_deck < 0 ? ndecks - 1 : mg.stats_deck - 1; }
  old = ctx->style->font;
  ctx->style->font = &font_bold;
  mu_draw_control_text(ctx, fname ? decks[mg.stats_deck].name : "All decks", mu_layout_next(ctx), MU_COLOR_TEXT, MU_OPT_ALIGNCENTER);
  ctx->style->font = old;
  if (mu_button(ctx, ">")) { mg.stats_deck = mg.stats_deck >= ndecks - 1 ? -1 : mg.stats_deck + 1; }

  ui_spacer(ctx, S(2));
  tw = (mu_get_current_container(ctx)->body.w - ctx->style->padding * 2 - ctx->style->spacing * 3) / 4;
  mu_layout_row(ctx, 4, (int[]){ tw, tw, tw, -1 }, S(68));
  t = history_day(0, fname);
  percent(t, value, sizeof(value));
  snprintf(label, sizeof(label), "today  \xC2\xB7  %d answered", t.n);
  stat_tile(ctx, value, label, t.n ? accuracy_color(t.right * 100 / t.n) : COL_DIM);
  t = history_range(7, fname);
  percent(t, value, sizeof(value));
  snprintf(label, sizeof(label), "last 7 days  \xC2\xB7  %d", t.n);
  stat_tile(ctx, value, label, t.n ? accuracy_color(t.right * 100 / t.n) : COL_DIM);
  t = history_range(0, fname);
  percent(t, value, sizeof(value));
  snprintf(label, sizeof(label), "all time  \xC2\xB7  %d", t.n);
  stat_tile(ctx, value, label, t.n ? accuracy_color(t.right * 100 / t.n) : COL_DIM);
  streak = history_streak();
  snprintf(value, sizeof(value), "%d", streak);
  stat_tile(ctx, value, "day streak", streak ? COL_ACCENT : COL_DIM);

  ui_heading(ctx, "Accuracy, last 30 days");
  accuracy_chart(ctx, fname);

  ui_heading(ctx, "By deck");
  mu_layout_row(ctx, 4, (int[]){ -S(300), S(100), S(100), -1 }, S(22));
  ui_label_font(ctx, "Deck", &font_small, COL_DIM);
  ui_label_font(ctx, "Learned", &font_small, COL_DIM);
  ui_label_font(ctx, "Last 7 days", &font_small, COL_DIM);
  ui_label_font(ctx, "All time", &font_small, COL_DIM);
  for (i = 0; i < ndecks; i++) {
    Deck *d = &decks[i];
    Tally week = history_range(7, d->fname), all = history_range(0, d->fname);
    mu_push_id(ctx, &i, sizeof(i));
    mu_layout_row(ctx, 4, (int[]){ -S(300), S(100), S(100), -1 }, S(28));
    if (d->mastered) {
      snprintf(label, sizeof(label), "%s  \xC2\xB7  mastered", d->name);
      ui_label_color(ctx, label, COL_GOOD);
    } else {
      ui_label_color(ctx, d->name, d->enabled ? ctx->style->colors[MU_COLOR_TEXT] : COL_DIM);
    }
    snprintf(value, sizeof(value), "%d%%", d->ncards ? deck_learned(d) * 100 / d->ncards : 0);
    mu_label(ctx, value);
    percent(week, value, sizeof(value));
    ui_label_color(ctx, value, week.n ? accuracy_color(week.right * 100 / week.n) : COL_DIM);
    percent(all, value, sizeof(value));
    ui_label_color(ctx, value, all.n ? accuracy_color(all.right * 100 / all.n) : COL_DIM);
    mu_pop_id(ctx);
  }
}

static void settings_tab(mu_Context *ctx) {
  int full = -1, answered, right, learned, total;
  char buf[256];

  ui_heading(ctx, "Timing");
  mu_layout_row(ctx, 1, &full, 0);
  ui_text_font(ctx, "A question pops up at a random moment between these two times after the last one.", &font_normal, COL_DIM);
  mu_layout_row(ctx, 2, (int[]){ S(90), -1 }, S(32));
  mu_label(ctx, "At least");
  if (mu_slider_ex(ctx, &settings.min_minutes, 1, 240, 1, "%.0f min", MU_OPT_ALIGNCENTER) & MU_RES_CHANGE) {
    if (settings.max_minutes < settings.min_minutes) { settings.max_minutes = settings.min_minutes; }
    settings_dirty = mg.reschedule = 1;
  }
  mu_label(ctx, "At most");
  if (mu_slider_ex(ctx, &settings.max_minutes, 1, 240, 1, "%.0f min", MU_OPT_ALIGNCENTER) & MU_RES_CHANGE) {
    if (settings.min_minutes > settings.max_minutes) { settings.min_minutes = settings.max_minutes; }
    settings_dirty = mg.reschedule = 1;
  }
  mu_layout_row(ctx, 1, &full, S(28));
  if (mu_checkbox(ctx, "Paused", &settings.paused) & MU_RES_CHANGE) {
    settings_dirty = 1;
    if (!settings.paused) { mg.reschedule = 1; }
  }

  ui_heading(ctx, "Behaviour");
  mu_layout_row(ctx, 1, &full, S(28));
  if (mu_checkbox(ctx, "Play a sound when a question pops up", &settings.sound) & MU_RES_CHANGE) { settings_dirty = 1; }
  if (mu_checkbox(ctx, "Wait while a fullscreen game or presentation is running", &settings.skip_fullscreen) & MU_RES_CHANGE) { settings_dirty = 1; }
  if (mu_checkbox(ctx, "Start WaitWhat when Windows starts", &autostart_on) & MU_RES_CHANGE) { autostart_set(autostart_on); }
  mu_layout_row(ctx, 1, &full, 0);
  ui_text_font(ctx, "Ctrl+Alt+W anywhere: ask a question now, or jump to the one that's open.", &font_small, COL_DIM);

  ui_heading(ctx, "Sharing server");
  mu_layout_row(ctx, 1, &full, S(32));
  if (mu_textbox(ctx, settings.server, sizeof(settings.server)) & MU_RES_CHANGE) { settings_dirty = 1; }
  mu_layout_row(ctx, 1, &full, 0);
  ui_text_font(ctx, "Where Share and Import send decks, e.g. https://waitwhat.yourname.workers.dev", &font_small, COL_DIM);

  ui_heading(ctx, "Progress");
  progress_stats(&answered, &right, &learned, &total);
  snprintf(buf, sizeof(buf), "%d answers  \xC2\xB7  %d%% correct  \xC2\xB7  %d of %d cards learned",
    answered, answered ? right * 100 / answered : 0, learned, total);
  mu_layout_row(ctx, 1, &full, S(24));
  mu_label(ctx, buf);
  mu_layout_row(ctx, 1, &full, 0);
  ui_text_font(ctx,
    "Each card has a level from 0 to 5. Right answers move it up, a miss drops it back to 0. "
    "Low-level cards are picked far more often, and cards you haven't seen for a while get a nudge, "
    "so your time goes to what you don't know yet. Level 4+ counts as learned.",
    &font_small, COL_DIM);
  mu_layout_row(ctx, 2, (int[]){ S(170), -1 }, S(32));
  if (mu_button(ctx, mg.confirm_reset ? "Click again to reset" : "Reset all progress")) {
    if (mg.confirm_reset) { mg.action = ACT_RESET; mg.confirm_reset = 0; }
    else { mg.confirm_reset = 1; }
  }
}

static void manager_frame(UiWindow *w) {
  mu_Context *ctx = w->ctx;
  int full = -1;
  char status[96];
  if (!ui_begin_main(w, MU_OPT_NOSCROLL)) { return; }

  mu_layout_row(ctx, 6, (int[]){ S(80), S(76), S(100), S(90), -S(110), -1 }, S(32));
  tab_button(ctx, "Decks", TAB_DECKS);
  tab_button(ctx, "Stats", TAB_STATS);
  tab_button(ctx, "AI prompt", TAB_AI);
  tab_button(ctx, "Settings", TAB_SETTINGS);
  status_text(status, sizeof(status));
  ui_label_right(ctx, status, COL_DIM);
  if (mu_button(ctx, "Ask now")) { mg.action = ACT_ASK; }

  mu_layout_row(ctx, 1, &full, -1);
  if (mg.tab == TAB_DECKS) {
    if (mg.sel >= ndecks) { mg.sel = -1; }
    mu_begin_panel_ex(ctx, mg.sel < 0 ? "decks" : "cards", MU_OPT_NOFRAME);
    if (mg.sel < 0) { deck_list(ctx); } else { deck_view(ctx); }
    mu_end_panel(ctx);
  } else if (mg.tab == TAB_STATS) {
    mu_begin_panel_ex(ctx, "stats", MU_OPT_NOFRAME);
    stats_tab(ctx);
    mu_end_panel(ctx);
  } else if (mg.tab == TAB_AI) {
    mu_begin_panel_ex(ctx, "ai", MU_OPT_NOFRAME);
    ai_tab(ctx);
    mu_end_panel(ctx);
  } else {
    mu_begin_panel_ex(ctx, "settings", MU_OPT_NOFRAME);
    settings_tab(ctx);
    mu_end_panel(ctx);
  }
  mu_end_window(ctx);
}

/* actions that reload decks or open windows run here, outside the frame */
static void manager_post(UiWindow *w) {
  int act = mg.action;
  mg.action = ACT_NONE;
  if (mg.reschedule) { mg.reschedule = 0; schedule_next(); }
  switch (act) {
    case ACT_CREATE:
      if (mg.new_deck[0]) {
        int i = deck_create(mg.new_deck);
        mg.new_deck[0] = 0;
        if (i >= 0) { mg.sel = i; }
      }
      break;
    case ACT_ADD:
      if (deck_add_card(mg.act_deck, mg.add_q, mg.add_a, mg.add_note)) {
        mg.add_q[0] = mg.add_a[0] = mg.add_note[0] = 0;
      }
      break;
    case ACT_DELETE:
      deck_delete_card(mg.act_deck, mg.act_card);
      mg.confirm_del = -1;
      break;
    case ACT_RELOAD: decks_load(); break;
    case ACT_FOLDER: ShellExecuteW(NULL, L"open", dir_decks, NULL, NULL, SW_SHOWNORMAL); break;
    case ACT_RESET: progress_reset(); break;
    case ACT_COPY: {
      static char prompt[4096];
      build_prompt(prompt, sizeof(prompt));
      clipboard_set(prompt);
      mg.copied_at = GetTickCount();
      break;
    }
    case ACT_ASK: ask_question(1); break;
    case ACT_MASTERED:
      decks[mg.act_deck].mastered = !decks[mg.act_deck].mastered;
      settings_save();
      break;
    case ACT_RESET_DECK: deck_reset_progress(mg.act_deck); break;
    case ACT_SHARE:
    case ACT_IMPORT:
      if (!net_job.running) {
        char *body = act == ACT_SHARE ? deck_export(mg.act_deck) : NULL;
        mg.net_msg[0] = 0;
        if (act == ACT_SHARE && decks[mg.act_deck].ncards == 0) {
          snprintf(mg.net_msg, sizeof(mg.net_msg), "Add some cards before sharing.");
          mg.net_good = 0;
        } else if (!net_start(act == ACT_SHARE ? NET_SHARE : NET_IMPORT, body ? body : mg.import_code)) {
          snprintf(mg.net_msg, sizeof(mg.net_msg), "%s", net_job.msg);
          mg.net_good = 0;
        }
        cJSON_free(body);
      }
      break;
  }
  if (act != ACT_NONE && act != ACT_ASK) { w->dirty = 1; }
  tray_update_tip();
}

static void manager_destroyed(UiWindow *w) {
  (void)w;
  manager = NULL;
  if (settings_dirty) { settings_save(); settings_dirty = 0; }
}

static void open_manager(void) {
  RECT wa;
  int width = S(780), height = S(600);
  if (manager) {
    ShowWindow(manager->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(manager->hwnd);
    return;
  }
  autostart_on = autostart_get();
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  manager = ui_create(APP_NAMEW, 0, wa.left + (wa.right - wa.left - width) / 2,
    wa.top + (wa.bottom - wa.top - height) / 2, width, height);
  manager->min_w = S(560);
  manager->min_h = S(420);
  manager->frame = manager_frame;
  manager->post_frame = manager_post;
  manager->on_destroy = manager_destroyed;
  ui_refresh(manager);
  ShowWindow(manager->hwnd, SW_SHOWNORMAL);
  SetForegroundWindow(manager->hwnd);
}


/* ---------------------------------------------------------------- main */

static void tick(void) {
  static int n;
  time_t now = time(NULL);
  n++;
  if (settings_dirty) { settings_save(); settings_dirty = 0; }
  if (n % 2 == 0 && decks_changed_on_disk()) {
    decks_load();
    if (manager) { ui_refresh(manager); }
  }
  if (!settings.paused && !popup) {
    if (now - next_due > 120) {
      schedule_next(); /* the PC was asleep: start a fresh wait instead of firing at once */
    } else if (now >= next_due) {
      if (settings.skip_fullscreen && user_busy()) { next_due = now + 60; }
      else { ask_question(0); }
    }
  }
  if (n % 10 == 0) { tray_update_tip(); }
  if (manager && !IsIconic(manager->hwnd)) { ui_refresh(manager); }
}

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_TRAY:
      switch (LOWORD(lp)) {
        case WM_LBUTTONUP: open_manager(); break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: tray_menu(); break;
        case NIN_BALLOONUSERCLICK: open_manager(); break;
      }
      return 0;
    case WM_SHOW_MAIN: open_manager(); return 0;
    case WM_NET_DONE: net_done(); return 0;
    case WM_HOTKEY: ask_question(1); return 0;
    case WM_TIMER: tick(); return 0;
    case WM_DESTROY:
      if (settings_dirty) { settings_save(); }
      Shell_NotifyIconW(NIM_DELETE, &nid);
      PostQuitMessage(0);
      return 0;
  }
  if (msg == msg_taskbar_created && msg_taskbar_created) { tray_add(); return 0; }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show) {
  WNDCLASSW wc;
  MSG msg;
  int first;
  int ask = strstr(cmdline, "--ask") != NULL;
  (void)prev; (void)show;

  CreateMutexW(NULL, TRUE, L"WaitWhat.SingleInstance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    /* already running: "waitwhat --ask" asks now, plain launch opens the window */
    HWND other = FindWindowW(TRAY_CLASS, NULL);
    if (other) {
      AllowSetForegroundWindow(ASFW_ANY);
      PostMessageW(other, ask ? WM_HOTKEY : WM_SHOW_MAIN, 0, 0);
    }
    return 0;
  }

  SetProcessDPIAware();
  rng_seed();
  first = data_init();
  ui_init();
  settings_load();
  if (first) {
    autostart_set(1); /* on by default; untick it in the tray menu or Settings */
    settings_save();
  }
  autostart_repair();
  decks_load();

  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = tray_proc;
  wc.hInstance = inst;
  wc.lpszClassName = TRAY_CLASS;
  RegisterClassW(&wc);
  tray_hwnd = CreateWindowW(TRAY_CLASS, APP_NAMEW, 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
  msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  net_set_notify(tray_hwnd, WM_NET_DONE);
  tray_add();

  if (!RegisterHotKey(tray_hwnd, HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'W')) {
    balloon(APP_NAME, "Ctrl+Alt+W is taken by another app, so the hotkey is off.");
  }
  schedule_next();
  tray_update_tip();
  SetTimer(tray_hwnd, 1, 1000, NULL);

  if (ask) { ask_question(1); }
  if (first) {
    open_manager();
    balloon("WaitWhat is running", "It lives here in the tray, quizzes you every now and then, and starts with Windows (right-click to change).");
  }

  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}
