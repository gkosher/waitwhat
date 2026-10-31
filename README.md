# WaitWhat

A tiny Windows tray app that quizzes you at random moments. You pick what to learn, it pops a question up every so often, and you type the answer.

Written in C with [microui](https://github.com/rxi/microui) and a custom GDI renderer. One `.exe` with no dependencies.

## Use it

- Run `waitwhat.exe`. It sits in the system tray (the `?` icon).
- **Left-click** the tray icon to open the manager. **Right-click** it for *Ask me now / Pause / Quit*.
- **Ctrl+Alt+W** anywhere asks a question now, or jumps to the one that's already open.
- `waitwhat.exe --ask` does the same thing from a shortcut or script.
- A question pops up in the bottom-right corner, at a random moment between the *at least* and *at most* times set in **Settings**. It never steals focus while you're typing in something else: click it, or press Ctrl+Alt+W.
- Enter checks your answer and Esc skips. If the checker got it wrong, **I was right** overrides it.

## Decks = JSON files

Every `.json` file in `decks/` (next to the exe) is a question set. Use the checkboxes in the **Decks** tab to choose which sets are in the random pool. Any file that is added or edited (by you, an editor, or an AI) reloads automatically within about 2 seconds.

```json
{
  "name": "Spanish basics",
  "cards": [
    { "q": "Spanish for 'dog'?", "a": "perro" },
    { "q": "Spanish for 'you' (informal)?", "a": ["tú", "tu"] },
    { "q": "Spanish for 'library'?", "a": "biblioteca", "note": "Not 'librería', that's a bookshop." }
  ]
}
```

- `q`: the question. `a`: the answer, or a list of accepted answers. `note`: optional, shown after you answer.
- If a file has broken JSON, the manager shows the line number in red.

### Have an AI write a deck

Open the **AI prompt** tab, type a topic, then click **Copy prompt**. Paste it into any AI. The prompt includes the exact file format and the path to save to. You can also just ask Claude Code to "add a deck about X to the decks folder".

### Included decks

- `example-capitals.json`: a small warm-up deck.
- `os-dev-c-asm.json`: 94 cards on writing an OS in C and x86 assembly. Covers boot sectors, real/protected/long mode, the GDT/IDT, paging, the PIC/PIT, VGA, Multiboot, toolchain flags, ABIs and QEMU.

## Sharing decks

Open a deck and click **Share**. The app uploads it and copies a short code like `wk7f2qxa` to your clipboard. Anyone can paste that code into **Import a shared deck** on the Decks tab.

The server is a single Cloudflare Worker with KV (see [`server/`](server/)). It's kept small and hard to abuse:

- **Proof of work:** every upload must include a nonce where `sha256(hex(sha256(body)) + ":" + nonce)` starts with 20 zero bits. That takes about a second for the app, but gets expensive for anyone trying to flood the server.
- **Rate limit:** 3 uploads per minute per IP.
- **Strict format:** 64 KB max, at most 1000 cards, and every field length is capped. The server rewrites each deck so only `name`/`q`/`a`/`note` text is ever stored.
- **Content-addressed:** the code is a hash of the deck, so re-uploading the same deck creates no new entry. Downloads are immutable and cached at Cloudflare's edge.

### Run your own server

```bash
cd server
npm install
npx wrangler login
npx wrangler kv namespace create DECKS   # paste the printed id into wrangler.toml
npx wrangler deploy
```

Put the printed `https://waitwhat.<you>.workers.dev` URL into **Settings → Sharing server**. If you're shipping builds, set `DEFAULT_SERVER` in `src/app.h` instead. To test locally, run `npx wrangler dev`.

## How it decides what to ask

Each card has a level from 0 to 5. A right answer moves it up one level and a miss drops it back to 0. Cards are picked by **weighted random**, so repeats can happen, but not on purpose:

| level | 0 | new | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|---|
| weight | 10 | 8 | 6 | 4 | 2.5 | 1.5 | 1 |

A card you haven't seen in a while gets a boost of up to 3x. The boost builds faster on low-level cards. Level 4 and up counts as "learned".

Answer checking ignores case and punctuation ("Washington, D.C." = "washington dc"). It forgives one or two typos on longer words, but never on numbers.

## Files

| file | what |
|---|---|
| `decks/*.json` | your question sets |
| `progress.json` | level / stats per card, keyed by deck file + question text |
| `settings.json` | timing, sound, disabled decks, sharing server |

## Build

Needs MinGW-w64 gcc (e.g. MSYS2 `pacman -S mingw-w64-x86_64-gcc`):

```bat
build.bat
```

`vendor/` holds microui (rxi, MIT) and cJSON (Dave Gamble, MIT).
