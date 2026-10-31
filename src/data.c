/* decks (json files), progress, settings, picking and answer checking */
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wctype.h>

Deck *decks;
int ndecks;
Settings settings = { 10, 30, 0, 1, 1, DEFAULT_SERVER };
wchar_t dir_app[MAX_PATH], dir_decks[MAX_PATH];

static wchar_t path_progress[MAX_PATH], path_settings[MAX_PATH], path_history[MAX_PATH];
static Prog *progs;
static int nprogs, cprogs;

static const wchar_t *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";


/* ------------------------------------------------------------- file io */

static char *read_file(const wchar_t *path) {
  FILE *fp = _wfopen(path, L"rb");
  char *buf;
  long n;
  if (!fp) { return NULL; }
  fseek(fp, 0, SEEK_END);
  n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  buf = malloc(n + 1);
  n = (long)fread(buf, 1, n, fp);
  buf[n] = 0;
  fclose(fp);
  /* skip a utf-8 BOM, editors love adding those */
  if ((unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
    memmove(buf, buf + 3, n - 2);
  }
  return buf;
}

static int write_file(const wchar_t *path, const char *data) {
  wchar_t tmp[MAX_PATH + 8];
  FILE *fp;
  swprintf(tmp, MAX_PATH + 8, L"%ls.tmp", path);
  fp = _wfopen(tmp, L"wb");
  if (!fp) { return 0; }
  fwrite(data, 1, strlen(data), fp);
  fclose(fp);
  return MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
}

static int write_json(const wchar_t *path, cJSON *root) {
  char *s = cJSON_Print(root);
  int ok = s && write_file(path, s);
  cJSON_free(s);
  return ok;
}

static void deck_path(const Deck *d, wchar_t *out) {
  swprintf(out, MAX_PATH, L"%ls\\%ls", dir_decks, d->file);
}

static const char *EXAMPLE_DECK =
  "{\n"
  "  \"name\": \"Example: World capitals\",\n"
  "  \"cards\": [\n"
  "    { \"q\": \"Capital of Japan?\", \"a\": \"Tokyo\" },\n"
  "    { \"q\": \"Capital of Australia?\", \"a\": \"Canberra\", \"note\": \"Not Sydney - Canberra was purpose-built as a compromise between Sydney and Melbourne.\" },\n"
  "    { \"q\": \"Capital of Canada?\", \"a\": \"Ottawa\" },\n"
  "    { \"q\": \"Capital of Brazil?\", \"a\": \"Brasilia\", \"note\": \"Rio de Janeiro was the capital until 1960.\" },\n"
  "    { \"q\": \"Capital of Turkey?\", \"a\": \"Ankara\" },\n"
  "    { \"q\": \"Capital of Egypt?\", \"a\": \"Cairo\" },\n"
  "    { \"q\": \"Capital of South Korea?\", \"a\": \"Seoul\" },\n"
  "    { \"q\": \"Capital of New Zealand?\", \"a\": \"Wellington\" },\n"
  "    { \"q\": \"Capital of the United States?\", \"a\": [\"Washington, D.C.\", \"Washington DC\", \"Washington\"] },\n"
  "    { \"q\": \"Capital of Switzerland?\", \"a\": [\"Bern\", \"Berne\"], \"note\": \"Officially it's the 'federal city' - Switzerland has no de jure capital.\" },\n"
  "    { \"q\": \"Capital of Morocco?\", \"a\": \"Rabat\" },\n"
  "    { \"q\": \"Capital of Vietnam?\", \"a\": [\"Hanoi\", \"Ha Noi\"] }\n"
  "  ]\n"
  "}\n";

int data_init(void) {
  wchar_t *slash;
  int first = 0;
  GetModuleFileNameW(NULL, dir_app, MAX_PATH);
  slash = wcsrchr(dir_app, L'\\');
  if (slash) { *slash = 0; }
  swprintf(dir_decks, MAX_PATH, L"%ls\\decks", dir_app);
  swprintf(path_progress, MAX_PATH, L"%ls\\progress.json", dir_app);
  swprintf(path_settings, MAX_PATH, L"%ls\\settings.json", dir_app);
  swprintf(path_history, MAX_PATH, L"%ls\\history.json", dir_app);
  /* no settings yet = first launch (decks may already ship with the app) */
  first = GetFileAttributesW(path_settings) == INVALID_FILE_ATTRIBUTES;
  if (GetFileAttributesW(dir_decks) == INVALID_FILE_ATTRIBUTES) {
    wchar_t ex[MAX_PATH];
    CreateDirectoryW(dir_decks, NULL);
    swprintf(ex, MAX_PATH, L"%ls\\example-capitals.json", dir_decks);
    write_file(ex, EXAMPLE_DECK);
  }
  return first;
}


/* ------------------------------------------------------------ settings */

/* deck file names remembered in settings.json (disabled, mastered) */
typedef struct { char **names; int n; } NameList;
static NameList disabled, mastered;

static int list_has(const NameList *l, const char *fname) {
  int i;
  for (i = 0; i < l->n; i++) { if (_stricmp(l->names[i], fname) == 0) { return 1; } }
  return 0;
}

static void list_load(NameList *l, cJSON *arr) {
  cJSON *it;
  cJSON_ArrayForEach(it, arr) {
    if (cJSON_IsString(it)) {
      l->names = realloc(l->names, sizeof(char*) * (l->n + 1));
      l->names[l->n++] = _strdup(it->valuestring);
    }
  }
}

/* rebuild a list from the decks we know about, keeping names of decks that
** are currently missing (e.g. a file being rewritten) */
static int deck_is_off(const Deck *d) { return !d->enabled; }
static int deck_is_mastered(const Deck *d) { return d->mastered; }

static void list_save(NameList *l, cJSON *root, const char *key, int (*want)(const Deck*)) {
  cJSON *arr = cJSON_AddArrayToObject(root, key);
  NameList out = { NULL, 0 };
  int i, j;
  for (i = 0; i < l->n; i++) {
    int known = 0;
    for (j = 0; j < ndecks; j++) { if (_stricmp(decks[j].fname, l->names[i]) == 0) { known = 1; } }
    if (!known) { out.names = realloc(out.names, sizeof(char*) * (out.n + 1)); out.names[out.n++] = l->names[i]; }
    else { free(l->names[i]); }
  }
  for (j = 0; j < ndecks; j++) {
    if (want(&decks[j])) {
      out.names = realloc(out.names, sizeof(char*) * (out.n + 1));
      out.names[out.n++] = _strdup(decks[j].fname);
    }
  }
  free(l->names);
  *l = out;
  for (i = 0; i < l->n; i++) { cJSON_AddItemToArray(arr, cJSON_CreateString(l->names[i])); }
}

static double num(cJSON *obj, const char *key, double def) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static int flag(cJSON *obj, const char *key, int def) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

void settings_load(void) {
  char *txt = read_file(path_settings);
  cJSON *root, *it;
  if (!txt) { return; }
  root = cJSON_Parse(txt);
  free(txt);
  if (!root) { return; }
  settings.min_minutes = (float)num(root, "min_minutes", settings.min_minutes);
  settings.max_minutes = (float)num(root, "max_minutes", settings.max_minutes);
  settings.paused = flag(root, "paused", settings.paused);
  settings.sound = flag(root, "sound", settings.sound);
  settings.skip_fullscreen = flag(root, "skip_fullscreen", settings.skip_fullscreen);
  it = cJSON_GetObjectItemCaseSensitive(root, "server");
  if (cJSON_IsString(it)) { snprintf(settings.server, sizeof(settings.server), "%s", it->valuestring); }
  if (settings.min_minutes < 1) { settings.min_minutes = 1; }
  if (settings.max_minutes < settings.min_minutes) { settings.max_minutes = settings.min_minutes; }
  list_load(&disabled, cJSON_GetObjectItemCaseSensitive(root, "disabled"));
  list_load(&mastered, cJSON_GetObjectItemCaseSensitive(root, "mastered"));
  cJSON_Delete(root);
}

void settings_save(void) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "min_minutes", settings.min_minutes);
  cJSON_AddNumberToObject(root, "max_minutes", settings.max_minutes);
  cJSON_AddBoolToObject(root, "paused", settings.paused);
  cJSON_AddBoolToObject(root, "sound", settings.sound);
  cJSON_AddBoolToObject(root, "skip_fullscreen", settings.skip_fullscreen);
  cJSON_AddStringToObject(root, "server", settings.server);
  list_save(&disabled, root, "disabled", deck_is_off);
  list_save(&mastered, root, "mastered", deck_is_mastered);
  write_json(path_settings, root);
  cJSON_Delete(root);
}

static void autostart_command(wchar_t *cmd, int cap) {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(NULL, exe, MAX_PATH);
  swprintf(cmd, cap, L"\"%ls\"", exe);
}

int autostart_get(void) {
  return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAMEW, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

/* if start-with-Windows is on but the exe has moved, point it at this copy */
void autostart_repair(void) {
  wchar_t cur[MAX_PATH + 4], want[MAX_PATH + 4];
  DWORD sz = sizeof(cur);
  if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAMEW, RRF_RT_REG_SZ, NULL, cur, &sz) != ERROR_SUCCESS) { return; }
  autostart_command(want, MAX_PATH + 4);
  if (_wcsicmp(cur, want) != 0) { autostart_set(1); }
}

void autostart_set(int on) {
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) { return; }
  if (on) {
    wchar_t cmd[MAX_PATH + 4];
    autostart_command(cmd, MAX_PATH + 4);
    RegSetValueExW(key, APP_NAMEW, 0, REG_SZ, (const BYTE*)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(key, APP_NAMEW);
  }
  RegCloseKey(key);
}


/* ------------------------------------------------------------ progress */

static uint64_t fnv1a(uint64_t h, const char *s) {
  while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
  return h;
}

static uint64_t card_key(const char *fname, const char *q) {
  uint64_t h = fnv1a(14695981039346656037ULL, fname);
  h = fnv1a(h, "\x1f");
  return fnv1a(h, q);
}

static Prog *prog_find(uint64_t key) {
  int i;
  for (i = 0; i < nprogs; i++) { if (progs[i].key == key) { return &progs[i]; } }
  return NULL;
}

static Prog *prog_ensure(uint64_t key) {
  Prog *p = prog_find(key);
  if (p) { return p; }
  if (nprogs == cprogs) {
    cprogs = cprogs ? cprogs * 2 : 256;
    progs = realloc(progs, sizeof(Prog) * cprogs);
  }
  p = &progs[nprogs++];
  memset(p, 0, sizeof(*p));
  p->key = key;
  return p;
}

Prog prog_get(uint64_t key) {
  Prog *p = prog_find(key), empty;
  if (p) { return *p; }
  memset(&empty, 0, sizeof(empty));
  empty.key = key;
  return empty;
}

void prog_set(Prog p) { *prog_ensure(p.key) = p; }

void prog_record(uint64_t key, int correct) {
  Prog *p = prog_ensure(key);
  p->seen++;
  p->last = (long long)time(NULL);
  if (correct) {
    p->right++;
    if (p->box < MAX_BOX) { p->box++; }
  } else {
    p->box = 0;
  }
}

static void progress_load(void) {
  char *txt = read_file(path_progress);
  cJSON *root, *it;
  if (!txt) { return; }
  root = cJSON_Parse(txt);
  free(txt);
  if (!root) { return; }
  cJSON_ArrayForEach(it, root) {
    Prog *p = prog_ensure(strtoull(it->string, NULL, 16));
    p->box = (int)num(it, "box", 0);
    p->seen = (int)num(it, "seen", 0);
    p->right = (int)num(it, "right", 0);
    p->last = (long long)num(it, "last", 0);
    if (p->box < 0) { p->box = 0; }
    if (p->box > MAX_BOX) { p->box = MAX_BOX; }
  }
  cJSON_Delete(root);
}

void progress_save(void) {
  cJSON *root = cJSON_CreateObject();
  int i;
  for (i = 0; i < nprogs; i++) {
    Prog *p = &progs[i];
    char key[24];
    cJSON *o;
    if (!p->seen) { continue; }
    snprintf(key, sizeof(key), "%016llx", (unsigned long long)p->key);
    o = cJSON_AddObjectToObject(root, key);
    cJSON_AddNumberToObject(o, "box", p->box);
    cJSON_AddNumberToObject(o, "seen", p->seen);
    cJSON_AddNumberToObject(o, "right", p->right);
    cJSON_AddNumberToObject(o, "last", (double)p->last);
  }
  write_json(path_progress, root);
  cJSON_Delete(root);
}

void progress_reset(void) {
  nprogs = 0;
  progress_save();
}

int deck_learned(Deck *d) {
  int i, n = 0;
  for (i = 0; i < d->ncards; i++) {
    Prog *p = prog_find(d->cards[i].key);
    if (p && p->box >= 4) { n++; }
  }
  return n;
}

void progress_stats(int *answered, int *right, int *learned, int *total) {
  int i;
  *answered = *right = *learned = *total = 0;
  for (i = 0; i < nprogs; i++) { *answered += progs[i].seen; *right += progs[i].right; }
  for (i = 0; i < ndecks; i++) { *learned += deck_learned(&decks[i]); *total += decks[i].ncards; }
}


void deck_reset_progress(int di) {
  int i, j;
  for (i = 0; i < decks[di].ncards; i++) {
    for (j = 0; j < nprogs; j++) {
      if (progs[j].key == decks[di].cards[i].key) { progs[j] = progs[--nprogs]; break; }
    }
  }
  progress_save();
}


/* ------------------------------------------------------------- history */

/* history.json: { "2026-10-31": { "deck.json": [answers, right], ... }, ... } */
typedef struct { char day[11]; char deck[MAX_PATH]; Tally t; } HistEntry;
static HistEntry *hist;
static int nhist, hist_loaded;

static void day_key(int days_ago, char out[11]) {
  time_t t = time(NULL) - (time_t)days_ago * 86400;
  struct tm *lt = localtime(&t);
  strftime(out, 11, "%Y-%m-%d", lt);
}

static void history_load(void) {
  char *txt;
  cJSON *root, *day, *deck;
  hist_loaded = 1;
  txt = read_file(path_history);
  if (!txt) { return; }
  root = cJSON_Parse(txt);
  free(txt);
  cJSON_ArrayForEach(day, root) {
    cJSON_ArrayForEach(deck, day) {
      HistEntry *e;
      if (cJSON_GetArraySize(deck) != 2) { continue; }
      hist = realloc(hist, sizeof(HistEntry) * (nhist + 1));
      e = &hist[nhist++];
      snprintf(e->day, sizeof(e->day), "%s", day->string);
      snprintf(e->deck, sizeof(e->deck), "%s", deck->string);
      e->t.n = cJSON_GetArrayItem(deck, 0)->valueint;
      e->t.right = cJSON_GetArrayItem(deck, 1)->valueint;
    }
  }
  cJSON_Delete(root);
}

static void history_save(void) {
  cJSON *root = cJSON_CreateObject();
  int i;
  for (i = 0; i < nhist; i++) {
    cJSON *day = cJSON_GetObjectItemCaseSensitive(root, hist[i].day), *pair;
    if (!day) { day = cJSON_AddObjectToObject(root, hist[i].day); }
    pair = cJSON_AddArrayToObject(day, hist[i].deck);
    cJSON_AddItemToArray(pair, cJSON_CreateNumber(hist[i].t.n));
    cJSON_AddItemToArray(pair, cJSON_CreateNumber(hist[i].t.right));
  }
  write_json(path_history, root);
  cJSON_Delete(root);
}

void history_add(const char *deck_fname, int n, int right) {
  char today[11];
  int i;
  if (!hist_loaded) { history_load(); }
  day_key(0, today);
  for (i = 0; i < nhist; i++) {
    if (strcmp(hist[i].day, today) == 0 && _stricmp(hist[i].deck, deck_fname) == 0) { break; }
  }
  if (i == nhist) {
    hist = realloc(hist, sizeof(HistEntry) * (nhist + 1));
    memset(&hist[nhist], 0, sizeof(HistEntry));
    snprintf(hist[nhist].day, sizeof(hist[nhist].day), "%s", today);
    snprintf(hist[nhist].deck, sizeof(hist[nhist].deck), "%s", deck_fname);
    nhist++;
  }
  hist[i].t.n += n;
  hist[i].t.right += right;
  history_save();
}

static Tally tally_where(const char *day, const char *deck_fname) {
  Tally t = { 0, 0 };
  int i;
  if (!hist_loaded) { history_load(); }
  for (i = 0; i < nhist; i++) {
    if (day && strcmp(hist[i].day, day) != 0) { continue; }
    if (deck_fname && _stricmp(hist[i].deck, deck_fname) != 0) { continue; }
    t.n += hist[i].t.n;
    t.right += hist[i].t.right;
  }
  return t;
}

Tally history_day(int days_ago, const char *deck_fname) {
  char day[11];
  day_key(days_ago, day);
  return tally_where(day, deck_fname);
}

Tally history_range(int days, const char *deck_fname) {
  Tally t = { 0, 0 };
  int i;
  if (days <= 0) { return tally_where(NULL, deck_fname); }
  for (i = 0; i < days; i++) {
    Tally d = history_day(i, deck_fname);
    t.n += d.n;
    t.right += d.right;
  }
  return t;
}

int history_streak(void) {
  /* today counts if you've answered; otherwise the streak is still alive from yesterday */
  int start = history_day(0, NULL).n > 0 ? 0 : 1, i;
  for (i = start; history_day(i, NULL).n > 0; i++) { }
  return i - start;
}

void history_day_label(int days_ago, char *out, int cap) {
  time_t t = time(NULL) - (time_t)days_ago * 86400;
  strftime(out, cap, "%b %d", localtime(&t));
}

/* --------------------------------------------------------------- decks */

static void decks_free(void) {
  int i;
  for (i = 0; i < ndecks; i++) {
    int j;
    for (j = 0; j < decks[i].ncards; j++) { free((void*)decks[i].cards[j].ans); }
    free(decks[i].cards);
    cJSON_Delete(decks[i].root);
  }
  free(decks);
  decks = NULL;
  ndecks = 0;
}

static int cmp_deck(const void *a, const void *b) {
  return _wcsicmp(((const Deck*)a)->file, ((const Deck*)b)->file);
}

static const char *str_field(cJSON *obj, const char *a, const char *b) {
  cJSON *v = cJSON_GetObjectItem(obj, a);
  if (!v && b) { v = cJSON_GetObjectItem(obj, b); }
  return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : NULL;
}

static cJSON *cards_array(cJSON *root) {
  return cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "cards");
}

static void deck_parse(Deck *d) {
  wchar_t path[MAX_PATH];
  char *txt;
  cJSON *cards, *it;
  int skipped = 0, cap;

  d->name = d->stem;
  deck_path(d, path);
  txt = read_file(path);
  if (!txt) { snprintf(d->err, sizeof(d->err), "Couldn't read this file."); return; }
  d->root = cJSON_Parse(txt);
  if (!d->root) {
    const char *e = cJSON_GetErrorPtr(), *p;
    int line = 1;
    for (p = txt; e && p < e && *p; p++) { if (*p == '\n') { line++; } }
    snprintf(d->err, sizeof(d->err), "Broken JSON around line %d - fix the file and it reloads by itself.", line);
    free(txt);
    return;
  }
  free(txt);

  if (cJSON_IsObject(d->root)) {
    const char *name = str_field(d->root, "name", "title");
    if (name) { d->name = name; }
  }
  cards = cards_array(d->root);
  cap = cJSON_GetArraySize(cards);
  d->cards = calloc(cap > 0 ? cap : 1, sizeof(Card));

  cJSON_ArrayForEach(it, cards) {
    Card *c = &d->cards[d->ncards];
    cJSON *a = cJSON_GetObjectItem(it, "a");
    if (!a) { a = cJSON_GetObjectItem(it, "answer"); }
    if (!a) { a = cJSON_GetObjectItem(it, "answers"); }
    c->q = str_field(it, "q", "question");
    c->note = str_field(it, "note", "explanation");
    if (cJSON_IsString(a) && a->valuestring[0]) {
      c->ans = malloc(sizeof(char*));
      c->ans[c->nans++] = a->valuestring;
    } else if (cJSON_IsArray(a)) {
      cJSON *s;
      c->ans = malloc(sizeof(char*) * (cJSON_GetArraySize(a) + 1));
      cJSON_ArrayForEach(s, a) {
        if (cJSON_IsString(s) && s->valuestring[0]) { c->ans[c->nans++] = s->valuestring; }
      }
    }
    if (!c->q || c->nans == 0) {
      free((void*)c->ans);
      memset(c, 0, sizeof(*c));
      skipped++;
      continue;
    }
    c->item = it;
    c->key = card_key(d->fname, c->q);
    d->ncards++;
  }
  if (!cards) {
    snprintf(d->err, sizeof(d->err), "No \"cards\" list found in this file.");
  } else if (skipped) {
    snprintf(d->err, sizeof(d->err), "%d card%s skipped (each needs a \"q\" and an \"a\").", skipped, skipped == 1 ? "" : "s");
  }
}

typedef struct { wchar_t file[MAX_PATH]; FILETIME mtime; } DirEntry;

static int scan_dir(DirEntry **out) {
  WIN32_FIND_DATAW fd;
  wchar_t pat[MAX_PATH];
  HANDLE h;
  int n = 0, cap = 0;
  *out = NULL;
  swprintf(pat, MAX_PATH, L"%ls\\*.json", dir_decks);
  h = FindFirstFileW(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) { return 0; }
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { continue; }
    if (n == cap) { cap = cap ? cap * 2 : 16; *out = realloc(*out, sizeof(DirEntry) * cap); }
    wcsncpy((*out)[n].file, fd.cFileName, MAX_PATH - 1);
    (*out)[n].file[MAX_PATH - 1] = 0;
    (*out)[n].mtime = fd.ftLastWriteTime;
    n++;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return n;
}

void decks_load(void) {
  DirEntry *ents;
  int n = scan_dir(&ents), i;
  static int progress_loaded;
  if (!progress_loaded) { progress_load(); progress_loaded = 1; }
  decks_free();
  decks = calloc(n > 0 ? n : 1, sizeof(Deck));
  for (i = 0; i < n; i++) {
    Deck *d = &decks[i];
    char *dot;
    wcscpy(d->file, ents[i].file);
    d->mtime = ents[i].mtime;
    wide_to_utf8(d->file, -1, d->fname, MAX_PATH);
    strcpy(d->stem, d->fname);
    dot = strrchr(d->stem, '.');
    if (dot) { *dot = 0; }
  }
  ndecks = n;
  free(ents);
  qsort(decks, ndecks, sizeof(Deck), cmp_deck);
  for (i = 0; i < ndecks; i++) {
    decks[i].enabled = !list_has(&disabled, decks[i].fname);
    decks[i].mastered = list_has(&mastered, decks[i].fname);
    deck_parse(&decks[i]);
  }
}

int decks_changed_on_disk(void) {
  DirEntry *ents;
  int n = scan_dir(&ents), i, changed = n != ndecks;
  for (i = 0; i < n && !changed; i++) {
    int j, found = 0;
    for (j = 0; j < ndecks; j++) {
      if (_wcsicmp(decks[j].file, ents[i].file) == 0) {
        found = CompareFileTime(&decks[j].mtime, &ents[i].mtime) == 0;
        break;
      }
    }
    if (!found) { changed = 1; }
  }
  free(ents);
  return changed;
}

static int deck_save(int di) {
  wchar_t path[MAX_PATH];
  int ok;
  deck_path(&decks[di], path);
  ok = write_json(path, decks[di].root);
  decks_load();
  return ok;
}

void deck_slug(const char *name, char *out, int cap) {
  int j = 0, dash = 0;
  for (; *name && j < cap - 1; name++) {
    unsigned char c = (unsigned char)*name;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { out[j++] = c; dash = 0; }
    else if (c >= 'A' && c <= 'Z') { out[j++] = c - 'A' + 'a'; dash = 0; }
    else if (j > 0 && !dash) { out[j++] = '-'; dash = 1; }
  }
  while (j > 0 && out[j - 1] == '-') { j--; }
  if (j == 0) { strcpy(out, "deck"); j = 4; }
  out[j] = 0;
}

int deck_create(const char *name) {
  char slug[96], fname[128];
  wchar_t wname[128], path[MAX_PATH];
  cJSON *root;
  int i, ok;
  deck_slug(name, slug, sizeof(slug));
  for (i = 1; ; i++) {
    if (i == 1) { snprintf(fname, sizeof(fname), "%s.json", slug); }
    else { snprintf(fname, sizeof(fname), "%s-%d.json", slug, i); }
    utf8_to_wide(fname, -1, wname, 128);
    swprintf(path, MAX_PATH, L"%ls\\%ls", dir_decks, wname);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) { break; }
  }
  root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "name", name);
  cJSON_AddArrayToObject(root, "cards");
  ok = write_json(path, root);
  cJSON_Delete(root);
  decks_load();
  for (i = 0; i < ndecks; i++) { if (_wcsicmp(decks[i].file, wname) == 0) { return i; } }
  return ok ? 0 : -1;
}

static char *trim(char *s) {
  char *e;
  while (*s == ' ') { s++; }
  e = s + strlen(s);
  while (e > s && e[-1] == ' ') { *--e = 0; }
  return s;
}

int deck_add_card(int di, const char *q, const char *a, const char *note) {
  Deck *d = &decks[di];
  cJSON *cards, *card;
  char buf[1024], *part, *next;
  int count = 0;
  if (!d->root) { return 0; }
  cards = cards_array(d->root);
  if (!cards) { cards = cJSON_AddArrayToObject(d->root, "cards"); }
  card = cJSON_CreateObject();
  cJSON_AddStringToObject(card, "q", q);

  /* "a | b | c" becomes a list of accepted answers */
  snprintf(buf, sizeof(buf), "%s", a);
  for (part = buf; *part; part++) { if (*part == '|') { count++; } }
  if (count == 0) {
    cJSON_AddStringToObject(card, "a", trim(buf));
  } else {
    cJSON *arr = cJSON_AddArrayToObject(card, "a");
    for (part = buf; part; part = next) {
      next = strchr(part, '|');
      if (next) { *next++ = 0; }
      part = trim(part);
      if (*part) { cJSON_AddItemToArray(arr, cJSON_CreateString(part)); }
    }
  }
  if (note && note[0]) { cJSON_AddStringToObject(card, "note", note); }
  cJSON_AddItemToArray(cards, card);
  return deck_save(di);
}

char *deck_export(int di) {
  Deck *d = &decks[di];
  cJSON *root = cJSON_CreateObject(), *cards = cJSON_AddArrayToObject(root, "cards");
  char *s;
  int i, j;
  cJSON_AddStringToObject(root, "name", d->name);
  for (i = 0; i < d->ncards; i++) {
    Card *c = &d->cards[i];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "q", c->q);
    if (c->nans == 1) {
      cJSON_AddStringToObject(o, "a", c->ans[0]);
    } else {
      cJSON *a = cJSON_AddArrayToObject(o, "a");
      for (j = 0; j < c->nans; j++) { cJSON_AddItemToArray(a, cJSON_CreateString(c->ans[j])); }
    }
    if (c->note) { cJSON_AddStringToObject(o, "note", c->note); }
    cJSON_AddItemToArray(cards, o);
  }
  s = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return s;
}

int deck_import(const char *json, char *err, int errcap) {
  cJSON *root = cJSON_Parse(json), *name;
  char slug[96], fname[128];
  wchar_t wname[128], path[MAX_PATH];
  int i;
  name = cJSON_GetObjectItem(root, "name");
  if (!root || !cJSON_IsString(name) || !cJSON_IsArray(cJSON_GetObjectItem(root, "cards"))) {
    snprintf(err, errcap, "The server sent something that isn't a deck.");
    cJSON_Delete(root);
    return -1;
  }
  deck_slug(name->valuestring, slug, sizeof(slug));
  for (i = 1; ; i++) {
    if (i == 1) { snprintf(fname, sizeof(fname), "%s.json", slug); }
    else { snprintf(fname, sizeof(fname), "%s-%d.json", slug, i); }
    utf8_to_wide(fname, -1, wname, 128);
    swprintf(path, MAX_PATH, L"%ls\\%ls", dir_decks, wname);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) { break; }
  }
  if (!write_json(path, root)) {
    snprintf(err, errcap, "Couldn't save the deck file.");
    cJSON_Delete(root);
    return -1;
  }
  cJSON_Delete(root);
  decks_load();
  for (i = 0; i < ndecks; i++) { if (_wcsicmp(decks[i].file, wname) == 0) { return i; } }
  return -1;
}

int deck_delete_card(int di, int ci) {
  Deck *d = &decks[di];
  if (!d->root || ci < 0 || ci >= d->ncards) { return 0; }
  cJSON_Delete(cJSON_DetachItemViaPointer(cards_array(d->root), d->cards[ci].item));
  return deck_save(di);
}


/* ------------------------------------------------------------- picking */

static uint64_t rng_state;

void rng_seed(void) {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  rng_state = (uint64_t)q.QuadPart ^ ((uint64_t)time(NULL) << 32) ^ GetCurrentProcessId();
  if (!rng_state) { rng_state = 88172645463325252ULL; }
}

double rng01(void) {
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;
  return ((rng_state * 2685821657736338717ULL) >> 11) * (1.0 / 9007199254740992.0);
}

/* Leitner-style weighting: cards you keep missing come up a lot, cards you
** know come up rarely, and anything you haven't seen in a while gets a
** gentle boost. It is still a weighted random draw, so repeats can happen. */
static double card_weight(const Deck *d, const Card *c, long long now) {
  static const double box_weight[MAX_BOX + 1] = { 10, 6, 4, 2.5, 1.5, 1 };
  Prog *p = prog_find(c->key);
  double days, w;
  if (!p || !p->seen) { w = 8; }
  else {
    days = (now - p->last) / 86400.0;
    w = box_weight[p->box] * (1.0 + fmin(2.0, days / (double)(1 << p->box)));
  }
  /* mastered decks still come up now and then, to keep them fresh */
  return d->mastered ? w * MASTERED_WEIGHT : w;
}

int pick_card(int *out_deck, int *out_card) {
  long long now = (long long)time(NULL);
  double total = 0, r;
  int i, j;
  for (i = 0; i < ndecks; i++) {
    if (!decks[i].enabled) { continue; }
    for (j = 0; j < decks[i].ncards; j++) { total += card_weight(&decks[i], &decks[i].cards[j], now); }
  }
  if (total <= 0) { return 0; }
  r = rng01() * total;
  for (i = 0; i < ndecks; i++) {
    if (!decks[i].enabled) { continue; }
    for (j = 0; j < decks[i].ncards; j++) {
      *out_deck = i;
      *out_card = j;
      r -= card_weight(&decks[i], &decks[i].cards[j], now);
      if (r < 0) { return 1; }
    }
  }
  return 1; /* float rounding: last enabled card */
}


/* ------------------------------------------------------------ checking */

#define NORM_CAP 512

/* lowercase, drop apostrophes, turn other punctuation into spaces and
** collapse whitespace, so "Washington, D.C." == "washington dc" */
static int normalize(const char *s, wchar_t *out) {
  wchar_t tmp[NORM_CAP];
  int n = utf8_to_wide(s, -1, tmp, NORM_CAP), i, j = 0, space = 0;
  CharLowerBuffW(tmp, n);
  for (i = 0; i < n; i++) {
    wchar_t c = tmp[i];
    if (c == L'\'' || c == 0x2019 || c == 0x2018 || c == L'.') { continue; }
    if (IsCharAlphaNumericW(c)) {
      if (space && j > 0) { out[j++] = L' '; }
      space = 0;
      out[j++] = c;
    } else {
      space = 1;
    }
  }
  out[j] = 0;
  return j;
}

static int levenshtein(const wchar_t *a, int la, const wchar_t *b, int lb) {
  int row[NORM_CAP + 1], i, j;
  for (j = 0; j <= lb; j++) { row[j] = j; }
  for (i = 1; i <= la; i++) {
    int prev = row[0], tmp;
    row[0] = i;
    for (j = 1; j <= lb; j++) {
      int cost = a[i - 1] == b[j - 1] ? 0 : 1;
      tmp = row[j];
      row[j] = mu_min(mu_min(row[j] + 1, row[j - 1] + 1), prev + cost);
      prev = tmp;
    }
  }
  return row[lb];
}

/* crude stemming of a normalized string, word by word, so word forms match:
** "context switching" == "context switch", "interrupts" == "interrupt" */
static int stem(const wchar_t *s, wchar_t *out) {
  int j = 0;
  while (*s) {
    int len = 0, n;
    while (s[len] && s[len] != L' ') { len++; }
    n = len;
    if (n > 5 && wcsncmp(s + n - 3, L"ing", 3) == 0) { n -= 3; }
    else if (n > 4 && wcsncmp(s + n - 2, L"ed", 2) == 0) { n -= 2; }
    else if (n > 4 && wcsncmp(s + n - 2, L"es", 2) == 0) { n -= 2; }
    else if (n > 3 && s[n - 1] == L's' && s[n - 2] != L's') { n -= 1; }
    memcpy(out + j, s, n * sizeof(wchar_t));
    j += n;
    s += len;
    if (*s) { out[j++] = L' '; s++; }
  }
  out[j] = 0;
  return j;
}

/* drop accents: "té" -> "te", "mézon" -> "mezon" (decompose, remove combining marks) */
static void strip_accents(const wchar_t *s, wchar_t *out) {
  wchar_t tmp[NORM_CAP * 2];
  int n = FoldStringW(MAP_COMPOSITE, s, -1, tmp, NORM_CAP * 2), i, j = 0;
  if (n <= 0) { wcscpy(out, s); return; }
  for (i = 0; i < n && tmp[i]; i++) {
    if (tmp[i] >= 0x300 && tmp[i] <= 0x36F) { continue; }
    if (j < NORM_CAP - 1) { out[j++] = tmp[i]; }
  }
  out[j] = 0;
}

/* is `in` exactly `ans` with one extra character inserted? ("hlt" -> "halt") */
static int one_insertion(const wchar_t *ans, int la, const wchar_t *in, int lin) {
  int i = 0, j = 0, skipped = 0;
  if (lin != la + 1) { return 0; }
  while (j < lin) {
    if (i < la && ans[i] == in[j]) { i++; j++; }
    else if (!skipped) { skipped = 1; j++; }
    else { return 0; }
  }
  return i == la;
}

int check_answer(const char *const *answers, int n, const char *input) {
  wchar_t in[NORM_CAP], ans[NORM_CAP], in_stem[NORM_CAP], ans_stem[NORM_CAP];
  wchar_t in_plain[NORM_CAP], ans_plain[NORM_CAP];
  int lin = normalize(input, in), i, best = ANSWER_WRONG;
  if (lin == 0) { return ANSWER_WRONG; }
  stem(in, in_stem);
  strip_accents(in, in_plain);
  for (i = 0; i < n; i++) {
    int la = normalize(answers[i], ans), allowed, k, digits = 0;
    if (la == lin && wcscmp(ans, in) == 0) { return ANSWER_EXACT; }
    stem(ans, ans_stem);
    if (wcscmp(ans_stem, in_stem) == 0) { return ANSWER_EXACT; }
    /* right word, accents left off (hard to type): count it, but show the spelling */
    strip_accents(ans, ans_plain);
    if (wcscmp(ans_plain, in_plain) == 0) { best = ANSWER_CLOSE; continue; }
    /* forgive small typos, but never on numbers: 1945 is not 1946 */
    for (k = 0; k < la; k++) { if (iswdigit(ans[k])) { digits = 1; } }
    allowed = digits ? 0 : (la >= 10 ? 2 : (la >= 5 ? 1 : 0));
    if (allowed && abs(la - lin) <= allowed && levenshtein(ans, la, in, lin) <= allowed) {
      best = ANSWER_CLOSE;
    }
    /* short answers (mnemonics like hlt, lgdt) only forgive one extra letter;
    ** a swapped letter could be a different answer (eax/rax, cli/sti) */
    if (!digits && la >= 3 && la < 5 && one_insertion(ans, la, in, lin)) {
      best = ANSWER_CLOSE;
    }
  }
  return best;
}
