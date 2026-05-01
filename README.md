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
- In-app model picker if no model is loaded
- Card question and answer provided as context automatically
- Ask follow-up questions or just hit Enter for "Explain this card"

## Card Format

Grimoire supports both single-line cards and block cards.

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

```text
Q:
What is DNS?
A:
Domain Name System - translates domain names into IP addresses.
ID:
dns-definition
NOTE:
Study-Vault/Programming/Computer Science/Computer Networks/DNS.md#DNS
```

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

## Directory Structure

On first launch, Grimoire asks where the parent Grimoire library should live.

The parent library stores shared metadata and a `vaults/` directory. Each child vault is loaded separately, so Grimoire only indexes the active vault instead of everything at once.

The default layout is:

```text
~/grimoire_knowledge_vault/
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
~/grimoire_knowledge_vault/vaults/study/
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

### Drill
| Key | Action |
|-----|--------|
| `Space` | Show answer |
| `n` | Open linked note for the current card while viewing the question |
| `N` | Set or update the linked note for the current card |
| `y` | Correct (streak +1) |
| `n` | Wrong on the answer screen (stage drops, streak resets) |
| `a` | Ask AI about this card |
| `q` | Quit session |

### AI Assistant
| Key | Action |
|-----|--------|
| `Enter` | Type a question |
| `j`/`k` | Scroll response |
| `q`/`Esc` | Back to card |

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
- [ ] Session summary screen (cards drilled, time, stage changes)
- [ ] Progress bar showing session completion
- [ ] Deck-level stats view (stage distribution, last drilled)

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
- [ ] Spaced repetition scheduling (optional alternative to drill mode)
- [ ] Sync progress across machines
- [ ] Export stats

## License

MIT
