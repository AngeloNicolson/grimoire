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

Plain text files with `::` as the delimiter:

```
What is DNS? :: Domain Name System - translates domain names into IP addresses.
What port does DNS use? :: 53.
```

One card per line. Empty lines and lines without `::` are ignored.

## Directory Structure

Grimoire expects decks organized in subject directories:

```
~/knowledge_vault/Study-Vault/Anki/
  networking/
    week_1/
      protocols.txt
      access_networks.txt
    week_2/
      transport_layer.txt
  physics/
    kinematics.txt
    forces.txt
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
| `y` | Correct (streak +1) |
| `n` | Wrong (stage drops, streak resets) |
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

Deck directory and data file paths are currently hardcoded at the top of `src/main.cpp`. Ollama endpoint defaults to `http://localhost:11434`.

## Feature Plan

### v0.2 - Anki Import
- [ ] `grimoire import <file.apkg>` command to convert Anki .apkg decks to txt format
- [ ] Strip HTML from Anki card fields
- [ ] Handle basic Anki note types (Basic, Basic + Reverse)
- [ ] Support cloze deletion conversion
- [ ] Place imported decks into the deck directory with sensible names

### v0.3 - Polish
- [ ] Config file (~/.config/grimoire/config.toml) for deck dir, data path, colors
- [ ] Session summary screen (cards drilled, time, stage changes)
- [ ] Progress bar showing session completion
- [ ] Deck-level stats view (stage distribution, last drilled)

### v0.4 - Advanced Drilling
- [ ] Tag-based filtering (drill only cards with specific tags)
- [ ] Worst-cards mode (drill the N cards with lowest mastery)
- [ ] Timed mode (drill for X minutes then stop)
- [ ] Undo last answer

### v0.5 - Card Management
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
