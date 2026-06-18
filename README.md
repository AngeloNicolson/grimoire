# Grimoire

A terminal-based flashcard drill system with persistent mastery tracking. Built with C++ and ncurses, inspired by [ncmpcpp](https://github.com/ncmpcpp/ncmpcpp).

Grimoire uses a 3-stage mastery system that adapts to your knowledge over time. Cards you know well require fewer repetitions each session, while cards you struggle with demand more. Sessions naturally get shorter as you improve.

## How It Works

### 3-Stage Mastery

Every card has a mastery stage that persists across sessions:

| Stage | Label    | Required Streak | What It Means |
|-------|----------|----------------|---------------|
| 0     | New      | 3 correct in a row | You're still learning this |
| 1     | Familiar | 2 correct in a row | You've proven you know it |
| 2     | Strong   | 1 correct | Quick confirmation |

**On success:** Stage advances (0 -> 1 -> 2, capped at 2)
**On failure:** Stage drops one level (2 -> 1 -> 0, floored at 0)

Cards are never skipped. Every card appears every session. Over time, known cards settle at Stage 2 and sessions get fast.

### Drill Rounds

Within a session, cards cycle through rounds:

1. Round 1 starts with all cards shuffled
2. Cards you get wrong (or haven't hit their streak target) go into the next round
3. Rounds repeat until every card hits its streak target
4. Session complete

### AI Assistant

Press `a` during a drill to ask an AI about the current card. Grimoire integrates with Ollama for local AI:

- Auto-starts Ollama if not running
- In-app model picker, remembered until Grimoire exits or you switch models
- Card question and answer provided as context automatically
- Ask follow-up questions or just hit Enter for "Explain this card"

## Card Format

Grimoire supports both single-line cards and block cards. You can mix both
formats in the same deck file, so compact cards can stay on one line while only
cards that need multiline answers or code use block form.

### Single-line cards

Plain text files with `::` as the delimiter:

```text
What is DNS? :: Domain Name System - translates domain names into IP addresses.
What port does DNS use? :: 53.
```

You can also add stable card ids and note references inline:

```text
What is DNS? :: Domain Name System. :: id=dns-definition :: note=Study-Vault/Programming/Computer Science/Computer Networks/DNS.md#DNS
```

### Block cards

Use block cards for multiline content, code samples, stable ids, and note references:

````text
Q:
What is DNS?
A:
Domain Name System - translates domain names into IP addresses.

```cpp
std::cout << "DNS maps names to addresses\n";
```

ID:
dns-definition
NOTE:
Study-Vault/Programming/Computer Science/Computer Networks/DNS.md#DNS
````

### Card markup

Question and answer text is parsed into display blocks before rendering:

- Plain paragraphs wrap to the card width.
- Fenced code blocks use triple backticks and preserve line breaks.
- Add an optional language after the opening fence, such as `cpp`, `python`, `bash`, `sh`, or `json`, for basic syntax highlighting.
- Long code lines wrap inside the card instead of being cut off.

Example:

````text
Q:
What does this Python loop do?
A:
It prints each name.

```python
for name in names:
    print(name)
```
````

### Deck metadata

Deck files can optionally begin with metadata:

```text
---
deck_id: networking.dns.core
title: DNS Core Concepts
---
Brief deck summary here.
---
What is DNS? :: Domain Name System.
```

If `deck_id` is present, Grimoire uses it instead of the file path for progress tracking. This lets you move deck files without losing progress. If a card has an `id`, Grimoire also tracks mastery by that stable card id instead of line order.

Empty lines and lines without `::` are ignored outside block cards.

Inside a block answer, a top-level `Question :: Answer` line starts the next
single-line card unless it appears inside a fenced code block. If you need a
literal `::` example in an answer, put it in a fenced code block or indent it.

## Directory Structure

On first launch, Grimoire asks where the parent Grimoire library should live.

The parent library stores shared metadata and a `vaults/` directory. Each child vault is loaded separately, so Grimoire only indexes the active vault instead of everything at once.

The default layout is:

```text
~/.local/share/grimoire/library/
  registry.json
  vaults/
    study/
      decks/
      notes/
      media/
    bible/
      decks/
      notes/
      media/
```

Grimoire currently drills decks from the active child vault's `decks/` directory. A typical child vault layout looks like:

```text
~/.local/share/grimoire/library/vaults/study/
  decks/
    networking/
      week_1/
        protocols.txt
        access_networks.txt
      week_2/
        transport_layer.txt
    physics/
      kinematics.txt
      forces.txt
  notes/
  media/
```

The deck browser shows subjects on the left and decks on the right.

## Controls

### Deck Browser
| Key | Action |
|-----|--------|
| `j`/`k` | Navigate up/down |
| `h`/`l` | Switch pane (subjects/decks) |
| `Enter` | Select |
| `q` | Quit |

Highlighting a deck shows a stats preview in the right column: card count, overall
correct rate (green ≥70%, red below), the date it was last drilled to completion,
and a peek at the first few questions.

If a selected deck has a saved drill, Grimoire opens a resume selector. Use
`j`/`k` to choose whether to continue the saved session, start a new session, or
go back, then press `Enter`.

### Drill
| Key | Action |
|-----|--------|
| `t` | Type an answer and let AI mark it automatically |
| `Space` | Show answer |
| `n` | Open linked note for the current card while viewing the question |
| `N` | Set or update the linked note for the current card |
| `y` | Correct (streak +1) |
| `n` | Wrong on the answer screen (stage drops, streak resets) |
| `a` | Ask AI about this card |
| `q` | Quit session |

### Typing an answer

The `t` answer field is a small modal (vi-style) editor. It opens in **insert**
mode so you can type right away:

| Mode | Key | Action |
|------|-----|--------|
| Insert | type | Insert text at the cursor |
| Insert | `Enter` | New line |
| Insert | `Backspace`/`Del` | Delete before/at the cursor |
| Insert | `Esc` | Switch to normal mode |
| Normal | `Enter` | Submit answer for AI marking |
| Normal | `i`/`a`/`A` | Insert / after cursor / end of line |
| Normal | `h`/`j`/`k`/`l`, `0`/`$` | Move cursor / line start / line end |
| Normal | `q`/`Esc` | Cancel |
| Both | Arrows, `Home`/`End` | Move the cursor |

Editing works like a normal text buffer — the cursor can move anywhere and
text is inserted or deleted at that point. For a one-line answer: type it,
then `Esc` `Enter` to submit.

### AI Assistant
| Key | Action |
|-----|--------|
| `Enter` | Type a question |
| `m` | Choose/load a different model |
| `j`/`k` | Scroll response |
| `q`/`Esc` | Back to card |

### Drill Review (Weakness Pool)
From the splash screen, `[r] Drill Review (N)` starts a cross-deck pass over the cards you most
often get wrong. It draws only from **decks you've completed at least one drill of**, ranks them
by a difficulty weight, and shows the weakest first.

| Key | Action |
|-----|--------|
| `t` | Type an answer and let AI mark it automatically |
| `Space` | Show answer |
| `Space`/`y` | Got it (on the answer screen) |
| `n` | Missed (on the answer screen) |
| `a` | Ask AI about this card |
| `q` | Quit review (graded cards are saved) |

Every answer updates a per-card `right`/`wrong`/`last_seen` tally (in both drill and drill review).
The pool weight is the card's smoothed **error rate** scaled by **recency**: a card answered today
is damped (cooldown), and a card you keep missing but haven't seen in a while floats to the top. As
your right count climbs, a card's weight decays and it drops out of the pool on its own. Up to 20
cards are shown per session.

Run `grimoire --due` to print the size of the current drill-review pool (handy for status bars).

> A separate date-based spaced-repetition mode is planned; the SM-2 scheduling code remains in
> place but is currently unused while Drill Review is the active review mode.

## Installation

### Quick install (Linux/macOS)

```sh
curl -sL https://raw.githubusercontent.com/AngeloNicolson/grimoire/main/install.sh | sh
```

### Arch Linux (AUR)

```sh
yay -S grimoire-git
```

### Homebrew (macOS)

```sh
brew tap AngeloNicolson/grimoire https://github.com/AngeloNicolson/grimoire
brew install grimoire
```

### Build from source

Requires: C++17 compiler, ncurses, CMake 3.14+

```sh
git clone https://github.com/AngeloNicolson/grimoire.git
cd grimoire
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cp build/grimoire ~/.local/bin/
```

nlohmann/json is fetched automatically via CMake.

## Data

Progress is saved to `~/.local/share/grimoire/progress.json`. This includes:

- **drill_mastery**: Per-card mastery stages (persists across sessions)
- **deck_stats**: Session completion/abandonment counts per deck
- **card_stats**: Per-card `right`/`wrong`/`last_seen` tally that drives the Drill Review pool
- **schedule**: Per-card SM-2 spaced-repetition state (`due`, `interval`, `ease`, `reps`, `last`) — retained but currently unused

## Configuration

On first run, Grimoire writes config to `~/.config/grimoire/config.json` and stores:

- `library_root`: the path to your parent Grimoire library

The parent library itself stores:

- `registry.json`: the current vault and known child vault paths
- `library_metadata.json`: a generated summary of vaults, decks, completion stats, consistency, streaks, and weak areas

The active deck directory is derived as `current_vault/decks`.

Progress and session state remain local app data:

- `~/.local/share/grimoire/progress.json`
- `~/.local/share/grimoire/session.json`

Ollama defaults to `http://localhost:11434`.

## Feature Plan

### v0.2 - Custom Math Renderer
- [ ] Custom glyph bitmaps for math symbols (fractions, summation, integrals, superscripts, subscripts)
- [ ] Layout engine to parse math notation and position glyphs
- [ ] Render to terminal via sixel graphics protocol
- [ ] Support LaTeX-style input syntax in card text (e.g. `\frac{n!}{k!(n-k)!}`)

### v0.3 - Deck Import
- [ ] `grimoire import <file.apkg>` command to convert Anki .apkg decks to txt format
- [ ] Strip HTML from Anki card fields
- [ ] Handle basic Anki note types (Basic, Basic + Reverse)
- [ ] Support cloze deletion conversion
- [ ] Place imported decks into the deck directory with sensible names

### v0.4 - Polish
- [ ] Config file (~/.config/grimoire/config.toml) for deck dir, data path, colors
- [x] Session summary screen (cards, rounds, answers, accuracy, time)
- [x] Progress bar showing session completion
- [x] Deck-level stats view (stage distribution, sessions)

### v0.5 - Advanced Drilling
- [ ] Tag-based filtering (drill only cards with specific tags)
- [ ] Worst-cards mode (drill the N cards with lowest mastery)
- [ ] Timed mode (drill for X minutes then stop)
- [ ] Undo last answer

### v0.6 - Card Management
- [ ] Edit cards in-app (open $EDITOR on the deck file)
- [ ] Add new cards from within grimoire
- [ ] Search across all decks
- [ ] Card history (when was it last drilled, success rate)

### Future
- [ ] Multiple card formats (markdown tables, CSV)
- [x] Drill Review (cross-deck weakness pool weighted by per-card error rate + recency)
- [ ] Date-based spaced repetition mode (SM-2; scheduling code already present, currently unused)
- [ ] Sync progress across machines
- [ ] Export stats

## License

MIT
