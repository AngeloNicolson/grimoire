#include "logos.h"
#include <ncurses.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <locale.h>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// --- Config ---

static const std::string DEFAULT_VAULT_ROOT = "~/grimoire_knowledge_vault";
static const std::string CONFIG_FILE = "~/.config/grimoire/config.json";
static const std::string DEFAULT_DATA_FILE = "~/.local/share/grimoire/progress.json";
static const std::string DEFAULT_SESSION_FILE = "~/.local/share/grimoire/session.json";

static std::string g_vault_root = DEFAULT_VAULT_ROOT;
static std::string g_deck_dir = DEFAULT_VAULT_ROOT + "/decks";
static std::string g_data_file = DEFAULT_DATA_FILE;
static std::string g_session_file = DEFAULT_SESSION_FILE;

static std::string expand_home(const std::string& path)
{
    if (path.empty() || path[0] != '~') return path;
    const char* home = std::getenv("HOME");
    if (!home) return path;
    return std::string(home) + path.substr(1);
}

static std::string collapse_home(const std::string& path)
{
    const char* home = std::getenv("HOME");
    if (!home) return path;
    std::string home_str = home;
    if (path == home_str) return "~";
    if (path.size() > home_str.size() && path.compare(0, home_str.size(), home_str) == 0 &&
        path[home_str.size()] == '/')
        return "~" + path.substr(home_str.size());
    return path;
}

struct AppConfig
{
    std::string vault_root;

    void apply() const
    {
        g_vault_root = vault_root.empty() ? DEFAULT_VAULT_ROOT : vault_root;
        g_deck_dir = g_vault_root + "/decks";
        g_data_file = DEFAULT_DATA_FILE;
        g_session_file = DEFAULT_SESSION_FILE;
    }

    bool load()
    {
        std::ifstream file(expand_home(CONFIG_FILE));
        if (!file.is_open()) return false;

        try
        {
            json data = json::parse(file);
            vault_root = data.value("vault_root", "");
        }
        catch (...)
        {
            return false;
        }

        if (vault_root.empty()) return false;
        apply();
        return true;
    }

    bool save() const
    {
        std::string path = expand_home(CONFIG_FILE);
        fs::create_directories(fs::path(path).parent_path());

        std::ofstream file(path);
        if (!file.is_open()) return false;

        json data;
        data["vault_root"] = vault_root;
        file << data.dump(2);
        return true;
    }
};

// --- Card / Deck ---

struct Card
{
    std::string id;
    std::string note_ref;
    std::string question;
    std::string answer;
};

struct DeckEntry
{
    std::string name;
    std::string path;
    bool is_dir;
};

struct Deck
{
    std::string id;
    std::string title;
    std::string summary;
    std::vector<Card> cards;
};

static std::string trim(const std::string& value)
{
    size_t start = 0;
    while (start < value.size() && std::isspace((unsigned char)value[start]))
        start++;

    size_t end = value.size();
    while (end > start && std::isspace((unsigned char)value[end - 1]))
        end--;

    return value.substr(start, end - start);
}

static bool starts_with(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

static bool parse_metadata_assignment(const std::string& line, std::string& key, std::string& value)
{
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    key = trim(line.substr(0, colon));
    value = trim(line.substr(colon + 1));
    return !key.empty();
}

static std::vector<std::string> split_card_segments(const std::string& line)
{
    std::vector<std::string> segments;
    size_t start = 0;

    while (start <= line.size())
    {
        size_t sep = line.find(" :: ", start);
        if (sep == std::string::npos)
        {
            segments.push_back(line.substr(start));
            break;
        }
        segments.push_back(line.substr(start, sep - start));
        start = sep + 4;
    }

    return segments;
}

static void apply_inline_card_metadata(Card& card, const std::vector<std::string>& segments)
{
    for (size_t i = 2; i < segments.size(); i++)
    {
        std::string segment = trim(segments[i]);
        size_t eq = segment.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(segment.substr(0, eq));
        std::string value = trim(segment.substr(eq + 1));

        if (key == "id")
            card.id = value;
        else if (key == "note" || key == "note_ref")
            card.note_ref = value;
    }
}

static Deck parse_deck(const std::string& path)
{
    Deck deck;
    std::ifstream file(path);
    std::string line;
    bool past_separator = false;
    bool metadata_checked = false;
    bool in_frontmatter = false;
    std::string pending_list_key;
    Card current_card;
    bool in_card = false;
    enum class BlockField
    {
        None,
        Question,
        Answer,
        NoteRef,
        CardId
    };
    BlockField block_field = BlockField::None;

    auto flush_card = [&]()
    {
        if (!in_card) return;
        if (!current_card.question.empty() && !current_card.answer.empty())
            deck.cards.push_back(current_card);
        current_card = Card{};
        in_card = false;
        block_field = BlockField::None;
    };

    auto append_line = [](std::string& target, const std::string& value)
    {
        if (!target.empty()) target += "\n";
        target += value;
    };

    while (std::getline(file, line))
    {
        std::string trimmed = trim(line);

        if (!metadata_checked && trimmed.empty()) continue;

        if (!metadata_checked)
        {
            metadata_checked = true;
            if (trimmed == "---")
            {
                in_frontmatter = true;
                continue;
            }
        }

        if (in_frontmatter)
        {
            if (trimmed == "---")
            {
                in_frontmatter = false;
                pending_list_key.clear();
                continue;
            }

            if (starts_with(trimmed, "- ") && pending_list_key == "source_notes")
            {
                continue;
            }

            pending_list_key.clear();
            std::string key;
            std::string value;
            if (!parse_metadata_assignment(trimmed, key, value)) continue;

            if (key == "deck_id")
                deck.id = value;
            else if (key == "title")
                deck.title = value;
            else if (key == "source_notes" && value.empty())
                pending_list_key = key;

            continue;
        }

        // Check for --- separator (summary delimiter)
        if (!past_separator)
        {
            if (trimmed == "---")
            {
                past_separator = true;
                continue;
            }
        }

        if (line == "Q:")
        {
            flush_card();
            past_separator = true;
            in_card = true;
            block_field = BlockField::Question;
            continue;
        }

        if (line == "A:" && in_card)
        {
            block_field = BlockField::Answer;
            continue;
        }

        if (line == "NOTE:" && in_card)
        {
            block_field = BlockField::NoteRef;
            continue;
        }

        if (line == "ID:" && in_card)
        {
            block_field = BlockField::CardId;
            continue;
        }

        if (block_field == BlockField::Question)
        {
            append_line(current_card.question, line);
            continue;
        }

        if (block_field == BlockField::Answer)
        {
            append_line(current_card.answer, line);
            continue;
        }

        if (block_field == BlockField::NoteRef)
        {
            append_line(current_card.note_ref, line);
            continue;
        }

        if (block_field == BlockField::CardId)
        {
            append_line(current_card.id, line);
            continue;
        }

        auto segments = split_card_segments(line);
        if (segments.size() < 2)
        {
            if (in_card)
            {
                if (line.empty())
                    flush_card();
                else
                    current_card.answer += "\n" + line;
                continue;
            }

            // Before any cards and no separator yet — accumulate as summary
            if (!past_separator && deck.cards.empty() && !line.empty())
            {
                if (!deck.summary.empty()) deck.summary += "\n";
                deck.summary += line;
            }
            continue;
        }
        flush_card();
        past_separator = true; // once we see a card, summary is done
        std::string q = trim(segments[0]);
        std::string a = trim(segments[1]);
        if (q.empty() || a.empty()) continue;
        current_card = Card{};
        current_card.question = q;
        current_card.answer = a;
        apply_inline_card_metadata(current_card, segments);
        in_card = true;
    }
    flush_card();
    return deck;
}

static std::vector<DeckEntry> list_dir(const std::string& dir)
{
    std::vector<DeckEntry> entries;
    if (!fs::exists(dir)) return entries;
    for (auto& e : fs::directory_iterator(dir))
    {
        DeckEntry d;
        d.path = e.path().string();
        d.name = e.path().filename().string();
        d.is_dir = e.is_directory();
        if (d.is_dir || e.path().extension() == ".txt") { entries.push_back(d); }
    }
    std::sort(entries.begin(), entries.end(),
              [](const DeckEntry& a, const DeckEntry& b)
              {
                  if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
                  return a.name < b.name;
              });
    return entries;
}

// --- Progress ---

struct Progress
{
    json drill_mastery;
    json deck_stats;

    void load()
    {
        std::string path = expand_home(g_data_file);
        std::ifstream file(path);
        if (!file.is_open())
        {
            drill_mastery = json::object();
            deck_stats = json::object();
            return;
        }
        try
        {
            json data = json::parse(file);
            drill_mastery = data.value("drill_mastery", json::object());
            deck_stats = data.value("deck_stats", json::object());
        }
        catch (...)
        {
            drill_mastery = json::object();
            deck_stats = json::object();
        }
    }

    void save()
    {
        std::string path = expand_home(g_data_file);
        std::string dir = fs::path(path).parent_path().string();
        fs::create_directories(dir);
        std::ofstream file(path);
        json data;
        data["drill_mastery"] = drill_mastery;
        data["deck_stats"] = deck_stats;
        file << data.dump(2);
    }

    int get_stage(const std::string& deck_id, int card_idx)
    {
        std::string key = deck_id + ":" + std::to_string(card_idx);
        if (drill_mastery.contains(key)) { return drill_mastery[key].get<int>(); }
        return 0;
    }

    int get_stage(const std::string& deck_id, const std::string& card_key, int fallback_idx)
    {
        if (!card_key.empty())
        {
            std::string stable_key = deck_id + ":" + card_key;
            if (drill_mastery.contains(stable_key)) { return drill_mastery[stable_key].get<int>(); }
        }
        return get_stage(deck_id, fallback_idx);
    }

    void set_stage(const std::string& deck_id, int card_idx, int stage)
    {
        std::string key = deck_id + ":" + std::to_string(card_idx);
        drill_mastery[key] = stage;
    }

    void set_stage(const std::string& deck_id, const std::string& card_key, int fallback_idx, int stage)
    {
        if (!card_key.empty())
        {
            std::string stable_key = deck_id + ":" + card_key;
            drill_mastery[stable_key] = stage;
            return;
        }
        set_stage(deck_id, fallback_idx, stage);
    }
};

// --- Drill Logic ---

static int stage_target(int stage)
{
    switch (stage)
    {
        case 0: return 3;
        case 1: return 2;
        case 2: return 1;
        default: return 3;
    }
}

static const char* stage_label(int stage)
{
    switch (stage)
    {
        case 0: return "New";
        case 1: return "Familiar";
        case 2: return "Strong";
        default: return "New";
    }
}

struct DrillSession
{
    std::string deck_id;
    std::string deck_name; // display name
    std::vector<Card> cards;
    Progress* progress;

    std::vector<int> round;
    std::vector<int> missed;
    std::vector<int> streaks;
    std::vector<int> targets;
    int round_num = 1;

    void init(const std::string& id, std::vector<Card> c, Progress* p)
    {
        deck_id = id;
        cards = std::move(c);
        progress = p;
        round_num = 1;
        missed.clear();

        int n = cards.size();
        streaks.assign(n, 0);
        targets.resize(n);

        round.clear();
        for (int i = 0; i < n; i++)
        {
            int stage = progress->get_stage(deck_id, cards[i].id, i);
            targets[i] = stage_target(stage);
            round.push_back(i);
        }
        shuffle_round();
    }

    void shuffle_round()
    {
        static std::mt19937 rng(std::random_device{}());
        std::shuffle(round.begin(), round.end(), rng);
    }

    bool next_round()
    {
        if (missed.empty()) return false;
        round = std::move(missed);
        missed.clear();
        round_num++;
        shuffle_round();
        return true;
    }

    int mastered_count()
    {
        int count = 0;
        for (int i = 0; i < (int)cards.size(); i++)
        {
            if (streaks[i] >= targets[i]) count++;
        }
        return count;
    }

    void mark_correct(int idx)
    {
        streaks[idx]++;
        if (streaks[idx] >= targets[idx])
        {
            int stage = progress->get_stage(deck_id, cards[idx].id, idx);
            int new_stage = std::min(stage + 1, 2);
            progress->set_stage(deck_id, cards[idx].id, idx, new_stage);
            progress->save();
        }
        else
        {
            missed.push_back(idx);
        }
    }

    void mark_wrong(int idx)
    {
        streaks[idx] = 0;
        int stage = progress->get_stage(deck_id, cards[idx].id, idx);
        int new_stage = std::max(stage - 1, 0);
        progress->set_stage(deck_id, cards[idx].id, idx, new_stage);
        targets[idx] = stage_target(new_stage);
        progress->save();
        missed.push_back(idx);
    }

    // Save paused session to disk
    void save_session(const std::string& path, int elapsed)
    {
        std::string fpath = expand_home(g_session_file);
        std::string dir = fs::path(fpath).parent_path().string();
        fs::create_directories(dir);
        json j;
        j["deck_path"] = path;
        j["deck_id"] = deck_id;
        j["deck_name"] = deck_name;
        j["round"] = round;
        j["missed"] = missed;
        j["streaks"] = streaks;
        j["targets"] = targets;
        j["round_num"] = round_num;
        j["elapsed"] = elapsed;
        std::ofstream file(fpath);
        file << j.dump(2);
    }

    // Restore session state from saved data
    void restore(const json& j, std::vector<Card> c, Progress* p)
    {
        deck_id = j["deck_id"].get<std::string>();
        deck_name = j["deck_name"].get<std::string>();
        cards = std::move(c);
        progress = p;
        round = j["round"].get<std::vector<int>>();
        missed = j["missed"].get<std::vector<int>>();
        streaks = j["streaks"].get<std::vector<int>>();
        targets = j["targets"].get<std::vector<int>>();
        round_num = j["round_num"].get<int>();
    }

    static void clear_session()
    {
        std::string fpath = expand_home(g_session_file);
        if (fs::exists(fpath)) fs::remove(fpath);
    }

    static json load_session()
    {
        std::string fpath = expand_home(g_session_file);
        std::ifstream file(fpath);
        if (!file.is_open()) return json();
        try
        {
            return json::parse(file);
        }
        catch (...)
        {
            return json();
        }
    }
};

// --- TUI ---

static std::vector<std::string> wrap_text(const std::string& text, int width)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string word;
    std::string line;
    while (stream >> word)
    {
        if (line.empty()) { line = word; }
        else if ((int)(line.size() + 1 + word.size()) > width)
        {
            lines.push_back(line);
            line = word;
        }
        else
        {
            line += " " + word;
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

static std::string strip_txt(const std::string& name)
{
    if (name.size() > 4 && name.substr(name.size() - 4) == ".txt")
        return name.substr(0, name.size() - 4);
    return name;
}

enum Color
{
    CLR_DEFAULT = 1,
    CLR_HEADER,
    CLR_STAGE_NEW,
    CLR_STAGE_FAMILIAR,
    CLR_STAGE_STRONG,
    CLR_DIM,
    CLR_HIGHLIGHT,
    CLR_CORRECT,
    CLR_WRONG,
    CLR_DIR,
    CLR_BORDER,
    CLR_TITLE,
    CLR_COLHEAD,
    CLR_CODE_KEYWORD,
    CLR_CODE_STRING,
    CLR_CODE_COMMENT,
    CLR_CODE_NUMBER,
};

static void init_colors()
{
    start_color();
    use_default_colors();
    init_pair(CLR_DEFAULT, -1, -1);
    init_pair(CLR_HEADER, COLOR_YELLOW, -1);
    init_pair(CLR_STAGE_NEW, COLOR_RED, -1);
    init_pair(CLR_STAGE_FAMILIAR, COLOR_YELLOW, -1);
    init_pair(CLR_STAGE_STRONG, COLOR_GREEN, -1);
    init_pair(CLR_DIM, COLOR_WHITE, -1);
    init_pair(CLR_HIGHLIGHT, COLOR_BLACK, COLOR_YELLOW);
    init_pair(CLR_CORRECT, COLOR_GREEN, -1);
    init_pair(CLR_WRONG, COLOR_RED, -1);
    init_pair(CLR_DIR, COLOR_CYAN, -1);
    init_pair(CLR_BORDER, 8, -1); // dark grey (bright black)
    init_pair(CLR_TITLE, COLOR_GREEN, -1);
    init_pair(CLR_COLHEAD, COLOR_YELLOW, -1);
    init_pair(CLR_CODE_KEYWORD, COLOR_CYAN, -1);
    init_pair(CLR_CODE_STRING, COLOR_GREEN, -1);
    init_pair(CLR_CODE_COMMENT, 8, -1);
    init_pair(CLR_CODE_NUMBER, COLOR_MAGENTA, -1);
}

static int stage_color(int stage)
{
    switch (stage)
    {
        case 0: return CLR_STAGE_NEW;
        case 1: return CLR_STAGE_FAMILIAR;
        case 2: return CLR_STAGE_STRONG;
        default: return CLR_STAGE_NEW;
    }
}

struct StyledSpan
{
    std::string text;
    int color = CLR_DEFAULT;
    bool bold = false;
};

using StyledLine = std::vector<StyledSpan>;

struct DisplayBlock
{
    bool is_code = false;
    std::string language;
    std::string text;
};

static bool is_language_char(char c)
{
    return std::isalnum((unsigned char)c) || c == '+' || c == '#' || c == '-' || c == '_';
}

static std::vector<DisplayBlock> parse_display_blocks(const std::string& text)
{
    std::vector<DisplayBlock> blocks;
    size_t pos = 0;

    while (pos < text.size())
    {
        size_t fence = text.find("```", pos);
        if (fence == std::string::npos)
        {
            if (pos < text.size()) blocks.push_back({false, "", text.substr(pos)});
            break;
        }

        if (fence > pos) blocks.push_back({false, "", text.substr(pos, fence - pos)});

        size_t content_start = fence + 3;
        size_t close = text.find("```", content_start);
        std::string content = close == std::string::npos
                                  ? text.substr(content_start)
                                  : text.substr(content_start, close - content_start);

        std::string language;
        size_t code_start = 0;
        while (code_start < content.size() && content[code_start] == ' ') code_start++;
        size_t lang_end = code_start;
        while (lang_end < content.size() && is_language_char(content[lang_end])) lang_end++;
        if (lang_end > code_start)
        {
            language = content.substr(code_start, lang_end - code_start);
            code_start = lang_end;
            while (code_start < content.size() &&
                   (content[code_start] == ' ' || content[code_start] == '\t' ||
                    content[code_start] == '\n' || content[code_start] == '\r'))
                code_start++;
        }
        blocks.push_back({true, language, content.substr(code_start)});

        if (close == std::string::npos) break;
        pos = close + 3;
    }

    return blocks;
}

static bool is_code_keyword(const std::string& language, const std::string& token)
{
    static const std::vector<std::string> cpp = {
        "auto",   "bool",      "break",  "case",   "catch",    "char",   "class",
        "const",  "continue",  "default","delete", "do",       "double", "else",
        "enum",   "false",     "float",  "for",    "if",       "int",    "long",
        "namespace", "new",    "nullptr","private","protected","public", "return",
        "short",  "sizeof",    "static", "struct", "switch",   "template", "this",
        "throw",  "true",      "try",    "typedef","typename", "using",  "void",
        "while"};
    static const std::vector<std::string> python = {
        "and", "as", "assert", "break", "class", "continue", "def", "elif", "else",
        "except", "False", "finally", "for", "from", "if", "import", "in", "is",
        "lambda", "None", "not", "or", "pass", "raise", "return", "True", "try",
        "while", "with", "yield"};
    static const std::vector<std::string> shell = {
        "case", "do", "done", "elif", "else", "esac", "fi", "for", "function", "if",
        "in", "then", "until", "while"};
    static const std::vector<std::string> json_words = {"false", "null", "true"};

    const std::vector<std::string>* words = nullptr;
    if (language == "c" || language == "cc" || language == "cpp" || language == "c++" ||
        language == "h" || language == "hpp")
        words = &cpp;
    else if (language == "py" || language == "python")
        words = &python;
    else if (language == "sh" || language == "bash" || language == "zsh")
        words = &shell;
    else if (language == "json")
        words = &json_words;

    if (!words) return false;
    return std::find(words->begin(), words->end(), token) != words->end();
}

static StyledLine highlight_code_line(const std::string& line, const std::string& language)
{
    StyledLine out;
    size_t i = 0;

    while (i < line.size())
    {
        char c = line[i];
        if ((language == "sh" || language == "bash" || language == "zsh") && c == '#')
        {
            out.push_back({line.substr(i), CLR_CODE_COMMENT, false});
            break;
        }
        if (i + 1 < line.size() && c == '/' && line[i + 1] == '/')
        {
            out.push_back({line.substr(i), CLR_CODE_COMMENT, false});
            break;
        }
        if (c == '"' || c == '\'')
        {
            char quote = c;
            size_t start = i++;
            bool escaped = false;
            while (i < line.size())
            {
                char q = line[i++];
                if (q == quote && !escaped) break;
                escaped = q == '\\' && !escaped;
                if (q != '\\') escaped = false;
            }
            out.push_back({line.substr(start, i - start), CLR_CODE_STRING, false});
            continue;
        }
        if (std::isdigit((unsigned char)c))
        {
            size_t start = i++;
            while (i < line.size() &&
                   (std::isalnum((unsigned char)line[i]) || line[i] == '.' || line[i] == '_'))
                i++;
            out.push_back({line.substr(start, i - start), CLR_CODE_NUMBER, false});
            continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_')
        {
            size_t start = i++;
            while (i < line.size() &&
                   (std::isalnum((unsigned char)line[i]) || line[i] == '_'))
                i++;
            std::string token = line.substr(start, i - start);
            bool keyword = is_code_keyword(language, token);
            out.push_back({token, keyword ? CLR_CODE_KEYWORD : CLR_DEFAULT, keyword});
            continue;
        }

        out.push_back({std::string(1, c), CLR_DEFAULT, false});
        i++;
    }

    return out;
}

static std::vector<StyledLine> format_display_text(const std::string& text, int width, int text_color,
                                                   bool text_bold)
{
    std::vector<StyledLine> lines;
    int safe_width = std::max(1, width);

    for (const auto& block : parse_display_blocks(text))
    {
        if (!block.is_code)
        {
            auto wrapped = wrap_text(block.text, safe_width);
            for (const auto& line : wrapped)
                lines.push_back({{line, text_color, text_bold}});
            continue;
        }

        if (!lines.empty()) lines.push_back({{std::string(), CLR_DEFAULT, false}});

        std::istringstream stream(block.text);
        std::string code_line;
        int code_width = std::max(1, safe_width - 2);
        while (std::getline(stream, code_line))
        {
            if ((int)code_line.size() > code_width) code_line = code_line.substr(0, code_width);
            StyledLine styled = {{"  ", CLR_BORDER, false}};
            auto spans = highlight_code_line(code_line, block.language);
            styled.insert(styled.end(), spans.begin(), spans.end());
            lines.push_back(styled);
        }

        if (block.text.empty()) lines.push_back({{"  ", CLR_BORDER, false}});
        lines.push_back({{std::string(), CLR_DEFAULT, false}});
    }

    if (lines.empty()) lines.push_back({{std::string(), text_color, text_bold}});
    return lines;
}

static void draw_styled_lines(const std::vector<StyledLine>& lines, int& y, int x, int max_y,
                              int width)
{
    for (const auto& line : lines)
    {
        if (y >= max_y) return;
        int used = 0;
        move(y, x);
        for (const auto& span : line)
        {
            if (used >= width) break;
            int remaining = width - used;
            std::string text = span.text;
            if ((int)text.size() > remaining) text = text.substr(0, remaining);
            attron(COLOR_PAIR(span.color));
            if (span.bold) attron(A_BOLD);
            addnstr(text.c_str(), remaining);
            if (span.bold) attroff(A_BOLD);
            attroff(COLOR_PAIR(span.color));
            used += (int)text.size();
        }
        y++;
    }
}

// Draw a vertical line
static void draw_vline(int y, int x, int h)
{
    attron(COLOR_PAIR(CLR_BORDER));
    for (int i = 0; i < h; i++)
    {
        mvaddch(y + i, x, ACS_VLINE);
    }
    attroff(COLOR_PAIR(CLR_BORDER));
}

// Draw a full-width horizontal line
static void draw_hline_full(int y, int x, int w)
{
    attron(COLOR_PAIR(CLR_BORDER));
    mvhline(y, x, ACS_HLINE, w);
    attroff(COLOR_PAIR(CLR_BORDER));
}

// Draw a box with ACS characters
static void draw_box(int y, int x, int h, int w)
{
    attron(COLOR_PAIR(CLR_BORDER));
    mvhline(y, x + 1, ACS_HLINE, w - 2);
    mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
    mvvline(y + 1, x, ACS_VLINE, h - 2);
    mvvline(y + 1, x + w - 1, ACS_VLINE, h - 2);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    attroff(COLOR_PAIR(CLR_BORDER));
}

// --- AI ---

static const std::string OLLAMA_URL = "http://localhost:11434";

static std::string shell_escape(const std::string& s)
{
    std::string out = "'";
    for (char c : s)
    {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

static std::string query_ollama(const std::string& model, const Card& card, const std::string& deck,
                                const std::string& user_question)
{
    std::string system_prompt =
        "You are a helpful study assistant. The user is studying flashcards and needs help "
        "understanding the current card.\n\n"
        "Current flashcard:\n"
        "- Question: " +
        card.question +
        "\n"
        "- Answer: " +
        card.answer +
        "\n"
        "- Deck: " +
        deck +
        "\n\n"
        "Keep your responses concise and focused on helping the user understand this specific "
        "card. "
        "Explain concepts, provide mnemonics, give examples, or clarify anything about this "
        "card.\n\n"
        "IMPORTANT: Output plain text only. Do NOT use markdown formatting like **bold**, "
        "*italic*, headers (#), or bullet points (-/*). Just use plain sentences and paragraphs.";

    json payload;
    payload["model"] = model;
    payload["prompt"] = user_question;
    payload["system"] = system_prompt;
    payload["stream"] = false;

    std::string cmd = "curl -s -X POST " + OLLAMA_URL +
                      "/api/generate "
                      "-H 'Content-Type: application/json' "
                      "-d " +
                      shell_escape(payload.dump()) + " 2>/dev/null";

    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "Error: failed to run curl";
    std::array<char, 4096> buf;
    while (fgets(buf.data(), buf.size(), pipe))
    {
        result += buf.data();
    }
    pclose(pipe);

    if (result.empty()) return "Error: empty response from Ollama. Is the model running?";

    try
    {
        auto resp = json::parse(result);
        if (resp.contains("response")) return resp["response"].get<std::string>();
        if (resp.contains("error")) return "Ollama error: " + resp["error"].get<std::string>();
    }
    catch (...)
    {
    }
    return "Error: could not parse Ollama response";
}

static std::string get_loaded_model()
{
    FILE* pipe = popen("ollama ps 2>/dev/null | tail -n +2 | head -n 1 | awk '{print $1}'", "r");
    if (!pipe) return "";
    char buf[256];
    std::string model;
    if (fgets(buf, sizeof(buf), pipe))
    {
        model = buf;
        while (!model.empty() && (model.back() == '\n' || model.back() == ' '))
            model.pop_back();
    }
    pclose(pipe);
    return model;
}

static std::string get_input(int y, int x, int max_w)
{
    std::string input;
    curs_set(1);
    timeout(-1);
    while (true)
    {
        mvhline(y, x, ' ', max_w);
        mvprintw(y, x, "> %s", input.c_str());
        refresh();
        int ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) break;
        if (ch == 27)
        {
            input.clear();
            break;
        }
        if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && !input.empty()) { input.pop_back(); }
        else if (ch >= 32 && ch < 127 && (int)input.size() < max_w - 4) { input += (char)ch; }
    }
    curs_set(0);
    return input;
}

static void draw_centered_message(const std::string& title, const std::vector<std::string>& lines,
                                  const std::string& footer = "")
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    clear();

    attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
    mvprintw(1, (max_x - (int)title.size()) / 2, "%s", title.c_str());
    attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);

    draw_hline_full(2, 0, max_x);

    int y = 4;
    for (const auto& line : lines)
    {
        auto wrapped = wrap_text(line, std::max(20, max_x - 6));
        for (const auto& part : wrapped)
            mvprintw(y++, 3, "%s", part.c_str());
        y++;
    }

    if (!footer.empty())
    {
        draw_hline_full(max_y - 2, 0, max_x);
        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(max_y - 1, 1, "%s", footer.c_str());
        attroff(COLOR_PAIR(CLR_DIM));
    }

    refresh();
}

static std::string prompt_path_screen(const std::string& title, const std::vector<std::string>& lines,
                                      const std::string& default_value)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    draw_centered_message(title, lines, "[Enter] confirm  [Esc] cancel");

    std::string prompt = "Path";
    if (!default_value.empty()) prompt += " [" + collapse_home(default_value) + "]";
    mvprintw(max_y - 4, 3, "%s", prompt.c_str());

    std::string input = get_input(max_y - 3, 3, std::max(20, max_x - 6));
    if (input.empty()) return default_value;
    return input;
}

static void ensure_vault_dirs(const std::string& vault_root)
{
    fs::create_directories(fs::path(vault_root) / "decks");
    fs::create_directories(fs::path(vault_root) / "notes");
    fs::create_directories(fs::path(vault_root) / "media");
}

static bool run_first_time_setup()
{
    while (true)
    {
        draw_centered_message(
            "Welcome To Grimoire",
            {"Choose where your Grimoire knowledge vault should live.",
             "An existing vault can be selected by path, or Grimoire can create a new one for you.",
             "A new vault uses a simple structure: decks/, notes/, and media/."},
            "[e] existing vault  [n] new vault  [q] quit");

        int ch = getch();
        if (ch == 'q' || ch == 27) return false;

        if (ch == 'e')
        {
            std::string path = prompt_path_screen(
                "Existing Vault",
                {"Enter the path to your existing Grimoire vault.",
                 "If the vault is missing decks/, Grimoire will create the standard folders there."},
                expand_home(DEFAULT_VAULT_ROOT));
            if (path.empty()) continue;

            path = expand_home(path);
            if (!fs::exists(path) || !fs::is_directory(path))
            {
                draw_centered_message("Invalid Path",
                                      {"That path does not exist or is not a directory."},
                                      "[Any key] back");
                getch();
                continue;
            }

            ensure_vault_dirs(path);

            AppConfig config;
            config.vault_root = path;
            if (!config.save())
            {
                draw_centered_message("Config Error",
                                      {"Grimoire could not save its configuration file."},
                                      "[Any key] back");
                getch();
                continue;
            }
            config.apply();
            return true;
        }

        if (ch == 'n')
        {
            std::string path = prompt_path_screen(
                "New Vault",
                {"Enter where the new Grimoire vault should be created."},
                expand_home(DEFAULT_VAULT_ROOT));
            if (path.empty()) continue;

            path = expand_home(path);
            try
            {
                ensure_vault_dirs(path);
            }
            catch (...)
            {
                draw_centered_message("Create Failed",
                                      {"Grimoire could not create the vault at that location."},
                                      "[Any key] back");
                getch();
                continue;
            }

            AppConfig config;
            config.vault_root = path;
            if (!config.save())
            {
                draw_centered_message("Config Error",
                                      {"Grimoire could not save its configuration file."},
                                      "[Any key] back");
                getch();
                continue;
            }
            config.apply();
            return true;
        }
    }
}

// Check if ollama service is running
static bool ollama_is_running() { return system("systemctl is-active --quiet ollama") == 0; }

// Start ollama service
static void start_ollama() { system("systemctl start ollama >/dev/null 2>&1"); }

// Get list of available models
static std::vector<std::string> get_available_models()
{
    std::vector<std::string> models;
    FILE* pipe = popen("ollama list 2>/dev/null | tail -n +2 | awk '{print $1}'", "r");
    if (!pipe) return models;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe))
    {
        std::string m = buf;
        while (!m.empty() && (m.back() == '\n' || m.back() == ' '))
            m.pop_back();
        if (!m.empty()) models.push_back(m);
    }
    pclose(pipe);
    return models;
}

// Load a model by sending a minimal request
static void load_model(const std::string& model)
{
    json payload;
    payload["model"] = model;
    payload["prompt"] = "hi";
    payload["stream"] = false;
    std::string cmd = "curl -s -X POST " + OLLAMA_URL +
                      "/api/generate "
                      "-H 'Content-Type: application/json' "
                      "-d " +
                      shell_escape(payload.dump()) + " >/dev/null 2>&1";
    system(cmd.c_str());
}

// In-app model picker, returns selected model or empty string
static std::string pick_model()
{
    auto models = get_available_models();
    if (models.empty()) return "";

    int selected = 0;
    int scroll = 0;

    while (true)
    {
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        clear();

        attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
        std::string title = "Select AI Model";
        mvprintw(0, (max_x - (int)title.size()) / 2, "%s", title.c_str());
        attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);

        draw_hline_full(1, 0, max_x);

        int list_y = 3;
        int visible = max_y - list_y - 3;
        if (visible < 1) visible = 1;

        if (selected < scroll) scroll = selected;
        if (selected >= scroll + visible) scroll = selected - visible + 1;

        for (int i = 0; i < visible && (i + scroll) < (int)models.size(); i++)
        {
            int idx = i + scroll;
            int y = list_y + i;
            if (idx == selected)
            {
                attron(COLOR_PAIR(CLR_HIGHLIGHT));
                mvprintw(y, 3, "%-*s", max_x - 6, models[idx].c_str());
                attroff(COLOR_PAIR(CLR_HIGHLIGHT));
            }
            else
            {
                mvprintw(y, 3, "%s", models[idx].c_str());
            }
        }

        draw_hline_full(max_y - 2, 0, max_x);
        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(max_y - 1, 1, "[j/k] navigate  [Enter] select  [q] cancel");
        attroff(COLOR_PAIR(CLR_DIM));

        refresh();

        timeout(-1);
        int ch = getch();
        if (ch == 'q' || ch == 27) return "";
        if (ch == 'j' || ch == KEY_DOWN)
        {
            if (selected < (int)models.size() - 1) selected++;
        }
        if (ch == 'k' || ch == KEY_UP)
        {
            if (selected > 0) selected--;
        }
        if (ch == '\n' || ch == KEY_ENTER) return models[selected];
    }
}

// Ensure ollama is ready with a loaded model
static std::string ensure_ollama_ready()
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    if (!ollama_is_running())
    {
        clear();
        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(max_y / 2, (max_x - 22) / 2, "Starting Ollama...");
        attroff(COLOR_PAIR(CLR_DIM));
        refresh();
        start_ollama();
        // Wait for it to be ready
        for (int i = 0; i < 10; i++)
        {
            usleep(500000);
            if (ollama_is_running()) break;
        }
    }

    std::string model = get_loaded_model();
    if (!model.empty()) return model;

    // No model loaded, show picker
    model = pick_model();
    if (model.empty()) return "";

    // Load it
    clear();
    attron(COLOR_PAIR(CLR_DIM));
    char buf[128];
    snprintf(buf, sizeof(buf), "Loading %s...", model.c_str());
    mvprintw(max_y / 2, (max_x - (int)strlen(buf)) / 2, "%s", buf);
    attroff(COLOR_PAIR(CLR_DIM));
    refresh();
    load_model(model);

    return model;
}

static void show_ai_assistant(const Card& card, const std::string& deck)
{
    std::string model = ensure_ollama_ready();
    if (model.empty()) return;

    int scroll = 0;
    std::vector<std::string> response_lines;
    std::string last_question;

    while (true)
    {
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        clear();

        int content_w = std::min(max_x - 4, 70);
        int left = (max_x - content_w) / 2;

        attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
        std::string title = "AI Assistant";
        mvprintw(0, (max_x - (int)title.size()) / 2, "%s", title.c_str());
        attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);

        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(1, left, "Model: %s", model.c_str());
        attroff(COLOR_PAIR(CLR_DIM));

        draw_hline_full(2, 0, max_x);

        int y = 3;
        attron(COLOR_PAIR(CLR_DIM));
        auto q_ctx = wrap_text("Q: " + card.question, content_w);
        for (auto& l : q_ctx)
        {
            mvprintw(y++, left, "%s", l.c_str());
        }
        auto a_ctx = wrap_text("A: " + card.answer, content_w);
        for (auto& l : a_ctx)
        {
            mvprintw(y++, left, "%s", l.c_str());
        }
        attroff(COLOR_PAIR(CLR_DIM));

        draw_hline_full(y, 0, max_x);
        y++;

        int resp_h = max_y - y - 4;
        if (resp_h < 1) resp_h = 1;

        if (response_lines.empty() && last_question.empty())
        {
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(y + resp_h / 2, (max_x - 30) / 2, "Press [Enter] to ask a question");
            attroff(COLOR_PAIR(CLR_DIM));
        }
        else
        {
            int total = (int)response_lines.size();
            if (scroll > total - resp_h) scroll = std::max(0, total - resp_h);
            if (scroll < 0) scroll = 0;

            for (int i = 0; i < resp_h && (i + scroll) < total; i++)
            {
                mvprintw(y + i, left, "%s", response_lines[i + scroll].c_str());
            }
        }

        draw_hline_full(max_y - 3, 0, max_x);
        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(max_y - 1, 1, "[Enter] ask  [j/k] scroll  [q/Esc] back");
        attroff(COLOR_PAIR(CLR_DIM));

        mvprintw(max_y - 2, left, "> ");

        refresh();

        timeout(-1);
        int ch = getch();
        if (ch == 'q' || ch == 27)
        {
            timeout(1000);
            return;
        }
        if (ch == 'j' || ch == KEY_DOWN)
        {
            scroll++;
            continue;
        }
        if (ch == 'k' || ch == KEY_UP)
        {
            if (scroll > 0) scroll--;
            continue;
        }
        if (ch == '\n' || ch == KEY_ENTER)
        {
            std::string question = get_input(max_y - 2, left, content_w);
            if (question.empty()) question = "Explain this card to me";

            clear();
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(max_y / 2, (max_x - 14) / 2, "Thinking...");
            attroff(COLOR_PAIR(CLR_DIM));
            refresh();

            last_question = question;
            std::string response = query_ollama(model, card, deck, question);

            response_lines.clear();
            response_lines.push_back("");
            auto q_display = "> " + question;
            auto q_wrapped = wrap_text(q_display, content_w);
            for (auto& l : q_wrapped)
                response_lines.push_back(l);
            response_lines.push_back("");

            std::istringstream rstream(response);
            std::string para;
            while (std::getline(rstream, para))
            {
                if (para.empty()) { response_lines.push_back(""); }
                else
                {
                    auto wrapped = wrap_text(para, content_w);
                    for (auto& l : wrapped)
                        response_lines.push_back(l);
                }
            }
            scroll = 0;
        }
    }
}

// Startup splash screen — random logo variant
// Returns 'c' to continue paused session, or anything else to browse
static int show_splash()
{
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, LOGO_COUNT - 1);
    auto& logo = LOGOS[dist(rng)];

    auto saved = DrillSession::load_session();
    bool has_session = saved.contains("deck_name");

    timeout(-1);
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    clear();

    int start_y = (max_y - logo.height) / 2 - 2;
    int start_x = (max_x - logo.width) / 2;
    if (start_x < 0) start_x = 0;
    if (start_y < 0) start_y = 0;

    attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
    for (int i = 0; i < logo.height; i++)
    {
        mvprintw(start_y + i, start_x, "%s", logo.lines[i]);
    }
    attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);

    int hint_y = start_y + logo.height + 3;

    if (has_session)
    {
        std::string dname = saved["deck_name"].get<std::string>();
        int remaining = (int)saved["round"].size() + (int)saved["missed"].size();
        char cont_str[128];
        snprintf(cont_str, sizeof(cont_str), "[c] Continue: %s (%d cards left)", dname.c_str(),
                 remaining);
        attron(COLOR_PAIR(CLR_HEADER) | A_BOLD);
        mvprintw(hint_y, (max_x - (int)strlen(cont_str)) / 2, "%s", cont_str);
        attroff(COLOR_PAIR(CLR_HEADER) | A_BOLD);
        hint_y += 2;
    }

    std::string hint = "[Enter] Browse decks  [q] Quit";
    attron(COLOR_PAIR(CLR_DIM));
    mvprintw(hint_y, (max_x - (int)hint.size()) / 2, "%s", hint.c_str());
    attroff(COLOR_PAIR(CLR_DIM));

    refresh();

    while (true)
    {
        int ch = getch();
        if (ch == 'q' || ch == 27) return 'q';
        if (ch == 'c' && has_session) return 'c';
        if (ch == '\n' || ch == ' ' || !has_session) return '\n';
    }
}

// Yazi-style 3-column file browser
// Columns: parent | current | child/preview
// Navigate with h/l to go up/down directory levels, j/k to move within a listing.

static std::string browse_decks(const std::string& root)
{
    // Navigation state: current directory path + selection index per directory
    std::string cwd = root;
    std::map<std::string, int> selections; // dir path -> selected index
    std::map<std::string, int> scrolls;    // dir path -> scroll offset

    auto get_sel = [&](const std::string& dir) -> int
    { return selections.count(dir) ? selections[dir] : 0; };
    auto get_scroll = [&](const std::string& dir) -> int
    { return scrolls.count(dir) ? scrolls[dir] : 0; };

    while (true)
    {
        auto entries = list_dir(cwd);
        if (entries.empty() && cwd != root)
        {
            // Empty dir — go back
            cwd = fs::path(cwd).parent_path().string();
            continue;
        }

        int sel = get_sel(cwd);
        if (sel >= (int)entries.size()) sel = std::max(0, (int)entries.size() - 1);
        selections[cwd] = sel;

        // Parent entries (empty if at root)
        std::vector<DeckEntry> parent_entries;
        std::string parent_path;
        int parent_sel = 0;
        if (cwd != root)
        {
            parent_path = fs::path(cwd).parent_path().string();
            parent_entries = list_dir(parent_path);
            parent_sel = get_sel(parent_path);
        }

        // Child entries (preview of selected item)
        std::vector<DeckEntry> child_entries;
        if (!entries.empty() && entries[sel].is_dir)
        {
            child_entries = list_dir(entries[sel].path);
        }

        // Draw
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        clear();

        // Title bar — show breadcrumb path
        attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
        std::string title = "Grimoire";
        mvprintw(0, (max_x - (int)title.size()) / 2, "%s", title.c_str());
        attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);

        // Breadcrumb
        std::string crumb;
        if (cwd.size() > root.size())
        {
            crumb = cwd.substr(root.size() + 1);
            std::replace(crumb.begin(), crumb.end(), '_', ' ');
        }
        if (!crumb.empty())
        {
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(0, 1, "%s", crumb.c_str());
            attroff(COLOR_PAIR(CLR_DIM));
        }

        int header_y = 1;
        draw_hline_full(header_y, 0, max_x);

        int list_start = 2;
        int footer_y = max_y - 1;
        int list_h = footer_y - list_start;
        if (list_h < 1) list_h = 1;

        // Always 3 columns: col0 | col1 | col2
        // At root: active=col0, preview=col1, col2 = deeper preview
        // Depth 1: parent=col0, active=col1, preview=col2
        // Depth 2+: grandparent shifts out, parent=col0, active=col1, preview=col2
        int depth = 0;
        if (cwd.size() > root.size())
        {
            std::string rel = cwd.substr(root.size() + 1);
            for (char c : rel)
                if (c == '/') depth++;
            depth++;
        }

        // Column geometry — always 3
        int col_w = max_x / 3;
        int col0_x = 0, col0_w = col_w;
        int col1_x = col_w + 1, col1_w = col_w;
        int col2_x = col_w * 2 + 2, col2_w = max_x - col2_x;

        // Vertical dividers — always 2
        draw_vline(list_start, col_w, list_h);
        draw_vline(list_start, col_w * 2 + 1, list_h);

        // Assign columns based on depth
        int active_x, active_w, preview_x, preview_w;
        int parent_col_x = -1, parent_col_w = 0;

        if (depth == 0)
        {
            // Root: active in col0, preview in col1, col2 for deeper preview
            active_x = col0_x;
            active_w = col0_w;
            preview_x = col1_x;
            preview_w = col1_w;
        }
        else
        {
            // Depth 1+: parent in col0, active in col1, preview in col2
            parent_col_x = col0_x;
            parent_col_w = col0_w;
            active_x = col1_x;
            active_w = col1_w;
            preview_x = col2_x;
            preview_w = col2_w;
        }

        // Helper to draw a column of entries
        auto draw_column = [&](const std::vector<DeckEntry>& items, int col_x, int w, int selected,
                               int& scroll, bool is_active)
        {
            int visible = list_h;
            if (selected >= 0)
            {
                if (selected < scroll) scroll = selected;
                if (selected >= scroll + visible) scroll = selected - visible + 1;
            }

            for (int i = 0; i < visible && (i + scroll) < (int)items.size(); i++)
            {
                int idx = i + scroll;
                int y = list_start + i;
                std::string display = strip_txt(items[idx].name);
                std::replace(display.begin(), display.end(), '_', ' ');
                std::replace(display.begin(), display.end(), '-', ' ');
                if ((int)display.size() > w - 2) display = display.substr(0, w - 2);

                if (idx == selected && is_active)
                {
                    attron(COLOR_PAIR(CLR_HIGHLIGHT));
                    mvprintw(y, col_x + 1, "%-*s", w - 2, display.c_str());
                    attroff(COLOR_PAIR(CLR_HIGHLIGHT));
                }
                else if (idx == selected && !is_active)
                {
                    attron(COLOR_PAIR(CLR_HIGHLIGHT) | A_DIM);
                    mvprintw(y, col_x + 1, "%-*s", w - 2, display.c_str());
                    attroff(COLOR_PAIR(CLR_HIGHLIGHT) | A_DIM);
                }
                else if (items[idx].is_dir)
                {
                    attron(COLOR_PAIR(CLR_DIR));
                    mvprintw(y, col_x + 1, "%s", display.c_str());
                    attroff(COLOR_PAIR(CLR_DIR));
                }
                else
                {
                    mvprintw(y, col_x + 1, "%s", display.c_str());
                }
            }
        };

        // Draw parent column (only when depth > 0)
        if (parent_col_x >= 0 && !parent_entries.empty())
        {
            int ps = get_scroll(parent_path);
            draw_column(parent_entries, parent_col_x, parent_col_w, parent_sel, ps, false);
            scrolls[parent_path] = ps;
        }

        // Draw active column
        {
            int cs = get_scroll(cwd);
            draw_column(entries, active_x, active_w, sel, cs, true);
            scrolls[cwd] = cs;
        }

        // Draw preview column
        auto draw_file_preview = [&](const std::string& path, int px, int pw)
        {
            auto preview_deck = parse_deck(path);
            int n = (int)preview_deck.cards.size();
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(list_start, px + 1, "%d card%s", n, n == 1 ? "" : "s");
            int preview_y = list_start + 2;
            for (int i = 0; i < std::min(n, list_h - 2); i++)
            {
                std::string q = preview_deck.cards[i].question;
                if ((int)q.size() > pw - 2) q = q.substr(0, pw - 5) + "...";
                mvprintw(preview_y + i, px + 1, "%s", q.c_str());
            }
            attroff(COLOR_PAIR(CLR_DIM));
        };

        if (!child_entries.empty())
        {
            int dummy_scroll = 0;
            draw_column(child_entries, preview_x, preview_w, -1, dummy_scroll, false);

            // At root: also show col2 as deeper preview of first child item
            if (depth == 0 && !child_entries.empty())
            {
                // Find first item in child to preview in col2
                auto& first_child = child_entries[0];
                if (first_child.is_dir)
                {
                    auto grandchild = list_dir(first_child.path);
                    if (!grandchild.empty())
                    {
                        int gs = 0;
                        draw_column(grandchild, col2_x, col2_w, -1, gs, false);
                    }
                }
                else
                {
                    draw_file_preview(first_child.path, col2_x, col2_w);
                }
            }
        }
        else if (!entries.empty() && !entries[sel].is_dir)
        {
            draw_file_preview(entries[sel].path, preview_x, preview_w);
        }

        // Footer
        draw_hline_full(footer_y - 1, 0, max_x);
        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(footer_y, 1, "[j/k] navigate  [h] back  [l/Enter] open  [q] quit");
        attroff(COLOR_PAIR(CLR_DIM));

        refresh();

        int ch = getch();
        if (ch == 'q' || ch == 27) return "";

        if (ch == 'j' || ch == KEY_DOWN)
        {
            if (sel < (int)entries.size() - 1) { selections[cwd] = sel + 1; }
        }
        if (ch == 'k' || ch == KEY_UP)
        {
            if (sel > 0) { selections[cwd] = sel - 1; }
        }
        // Enter directory or select deck
        if (ch == 'l' || ch == KEY_RIGHT || ch == '\n' || ch == KEY_ENTER)
        {
            if (!entries.empty())
            {
                if (entries[sel].is_dir) { cwd = entries[sel].path; }
                else
                {
                    return entries[sel].path;
                }
            }
        }
        // Go up
        if (ch == 'h' || ch == KEY_LEFT)
        {
            if (cwd != root) { cwd = fs::path(cwd).parent_path().string(); }
        }
    }
}

// Get a deck ID from a file path (relative to deck root)
static std::string deck_id_from_path(const std::string& path, const std::string& root)
{
    std::string rel = path.substr(root.size() + 1);
    if (rel.size() > 4 && rel.substr(rel.size() - 4) == ".txt")
    {
        rel = rel.substr(0, rel.size() - 4);
    }
    return rel;
}

// Pre-drill summary screen — shows deck info and stats, waits for keypress to begin
// Returns true to start drill, false to go back to browser
static bool show_deck_summary(const std::string& deck_name, const std::string& summary,
                              const std::string& deck_id, const std::vector<Card>& cards,
                              Progress& progress)
{
    timeout(-1); // block until keypress
    while (true)
    {
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        clear();

        int content_w = std::min(max_x - 4, 60);
        int cx = (max_x - content_w) / 2;
        int y = 1;

        // Title
        attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
        mvprintw(y, (max_x - (int)deck_name.size()) / 2, "%s", deck_name.c_str());
        attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);
        y += 2;

        draw_hline_full(y, 0, max_x);
        y += 2;

        // Summary text
        if (!summary.empty())
        {
            auto lines = wrap_text(summary, content_w);
            attron(COLOR_PAIR(CLR_DEFAULT));
            for (auto& l : lines)
            {
                mvprintw(y, cx, "%s", l.c_str());
                y++;
            }
            attroff(COLOR_PAIR(CLR_DEFAULT));
            y += 1;
            draw_hline_full(y, 0, max_x);
            y += 2;
        }

        // Card stage distribution
        int card_count = (int)cards.size();
        int new_count = 0, familiar_count = 0, strong_count = 0;
        for (int i = 0; i < card_count; i++)
        {
            int stage = progress.get_stage(deck_id, cards[i].id, i);
            if (stage == 0)
                new_count++;
            else if (stage == 1)
                familiar_count++;
            else
                strong_count++;
        }

        char total_str[64];
        snprintf(total_str, sizeof(total_str), "Cards: %d", card_count);
        attron(COLOR_PAIR(CLR_HEADER) | A_BOLD);
        mvprintw(y, cx, "%s", total_str);
        attroff(COLOR_PAIR(CLR_HEADER) | A_BOLD);
        y += 2;

        // Stage bars
        int bar_w = content_w - 16;
        auto draw_bar = [&](const char* label, int count, int color)
        {
            attron(COLOR_PAIR(color));
            mvprintw(y, cx, "%-10s %3d  ", label, count);
            attroff(COLOR_PAIR(color));
            if (card_count > 0 && bar_w > 0)
            {
                int filled = (count * bar_w) / card_count;
                attron(COLOR_PAIR(color));
                for (int i = 0; i < filled; i++)
                    addch(ACS_BLOCK);
                attroff(COLOR_PAIR(color));
                attron(COLOR_PAIR(CLR_DIM));
                for (int i = filled; i < bar_w; i++)
                    addch(ACS_BULLET);
                attroff(COLOR_PAIR(CLR_DIM));
            }
            y++;
        };

        draw_bar("New", new_count, CLR_STAGE_NEW);
        draw_bar("Familiar", familiar_count, CLR_STAGE_FAMILIAR);
        draw_bar("Strong", strong_count, CLR_STAGE_STRONG);

        // Deck stats (completions/abandonments)
        if (progress.deck_stats.contains(deck_id))
        {
            auto& ds = progress.deck_stats[deck_id];
            y += 2;
            draw_hline_full(y, 0, max_x);
            y += 2;
            int completed = ds.value("completed", 0);
            int abandoned = ds.value("abandoned", 0);
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(y, cx, "Sessions completed: %d", completed);
            y++;
            mvprintw(y, cx, "Sessions abandoned: %d", abandoned);
            attroff(COLOR_PAIR(CLR_DIM));
        }

        // Start prompt
        y = max_y - 2;
        std::string hint = "[Enter] Start  [q] Back";
        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(y, (max_x - (int)hint.size()) / 2, "%s", hint.c_str());
        attroff(COLOR_PAIR(CLR_DIM));

        refresh();

        int ch = getch();
        if (ch == '\n' || ch == ' ') return true;
        if (ch == 'q' || ch == 27) return false;
        if (ch == KEY_RESIZE) continue;
    }
}

// Drill TUI - centered card with box
static std::string format_elapsed(time_t start)
{
    int elapsed = (int)difftime(time(nullptr), start);
    int h = elapsed / 3600;
    int m = (elapsed % 3600) / 60;
    int s = elapsed % 60;
    char buf[16];
    if (h > 0)
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else
        snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

// Returns true if completed, false if paused/quit
static bool run_drill(DrillSession& session, const std::string& deck_path, int elapsed_offset = 0)
{
    time_t session_start = time(nullptr) - elapsed_offset;
    timeout(1000); // getch returns ERR after 1s so timer updates
    while (true)
    {
        // Check if round is done
        if (session.round.empty())
        {
            if (!session.next_round())
            {
                // All mastered - completion screen
                int max_y, max_x;
                getmaxyx(stdscr, max_y, max_x);
                clear();

                // Centered box
                int box_w = 40;
                int box_h = 9;
                int box_x = (max_x - box_w) / 2;
                int box_y = (max_y - box_h) / 2;
                draw_box(box_y, box_x, box_h, box_w);

                attron(COLOR_PAIR(CLR_CORRECT) | A_BOLD);
                std::string msg = "SESSION COMPLETE";
                mvprintw(box_y + 2, box_x + (box_w - (int)msg.size()) / 2, "%s", msg.c_str());
                attroff(COLOR_PAIR(CLR_CORRECT) | A_BOLD);

                char buf[64];
                snprintf(buf, sizeof(buf), "All %d cards mastered", (int)session.cards.size());
                attron(COLOR_PAIR(CLR_DIM));
                mvprintw(box_y + 4, box_x + (box_w - (int)strlen(buf)) / 2, "%s", buf);
                attroff(COLOR_PAIR(CLR_DIM));

                std::string hint = "[Press any key]";
                attron(COLOR_PAIR(CLR_DIM));
                mvprintw(box_y + 6, box_x + (box_w - (int)hint.size()) / 2, "%s", hint.c_str());
                attroff(COLOR_PAIR(CLR_DIM));

                refresh();
                timeout(-1); // block for final screen
                getch();
                DrillSession::clear_session();
                return true;
            }
        }

        // Pop next card
        int card_idx = session.round.back();
        session.round.pop_back();
        auto& card = session.cards[card_idx];
        int card_target = session.targets[card_idx];
        int streak = session.streaks[card_idx];
        int stage = session.progress->get_stage(session.deck_id, card.id, card_idx);

        // --- Show question ---
        while (true)
        {
            int max_y, max_x;
            getmaxyx(stdscr, max_y, max_x);
            clear();

            // Line 0: timer, deck name centered, mastered right
            int mastered = session.mastered_count();
            std::string elapsed = format_elapsed(session_start);
            attron(COLOR_PAIR(CLR_HEADER));
            mvprintw(0, 1, "%s", elapsed.c_str());
            mvprintw(0, (max_x - (int)session.deck_name.size()) / 2, "%s",
                     session.deck_name.c_str());
            attroff(COLOR_PAIR(CLR_HEADER));

            char mastered_str[64];
            snprintf(mastered_str, sizeof(mastered_str), "%d/%d", mastered,
                     (int)session.cards.size());
            attron(COLOR_PAIR(CLR_HEADER));
            mvprintw(0, max_x - (int)strlen(mastered_str) - 1, "%s", mastered_str);
            attroff(COLOR_PAIR(CLR_HEADER));

            // Line 1: [drilling] left, queue right
            attron(COLOR_PAIR(CLR_HEADER));
            mvprintw(1, 1, "[drilling]");
            attroff(COLOR_PAIR(CLR_HEADER));

            char queue_str[128];
            snprintf(queue_str, sizeof(queue_str), "%d left | %d queued",
                     (int)session.round.size() + 1, (int)session.missed.size());
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(1, max_x - (int)strlen(queue_str) - 1, "%s", queue_str);
            attroff(COLOR_PAIR(CLR_DIM));

            // Line 2: separator
            draw_hline_full(2, 0, max_x);

            // Line 3: Round centered
            char round_str[64];
            snprintf(round_str, sizeof(round_str), "Round %d", session.round_num);
            attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
            mvprintw(3, (max_x - (int)strlen(round_str)) / 2, "%s", round_str);
            attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);

            // Line 4: separator
            draw_hline_full(4, 0, max_x);

            // Card box - centered
            int content_w = std::min(max_x - 6, 60);
            auto q_lines = format_display_text(card.question, content_w - 4, CLR_DEFAULT, false);
            int card_h = (int)q_lines.size() + 8; // padding + label + streak
            card_h = std::min(card_h, std::max(6, max_y - 9));
            int box_x = (max_x - content_w) / 2;
            int box_y = std::max(6, (max_y - card_h) / 2);

            draw_box(box_y, box_x, card_h, content_w);

            // Stage + streak inside box top
            int inner_x = box_x + 2;
            int y = box_y + 1;

            attron(COLOR_PAIR(stage_color(stage)));
            mvprintw(y, inner_x, "[%s]", stage_label(stage));
            attroff(COLOR_PAIR(stage_color(stage)));

            char streak_str[32];
            snprintf(streak_str, sizeof(streak_str), "%d/%d", streak, card_target);
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(y, box_x + content_w - 2 - (int)strlen(streak_str), "%s", streak_str);
            attroff(COLOR_PAIR(CLR_DIM));
            y += 2;

            // Question text
            draw_styled_lines(q_lines, y, inner_x, box_y + card_h - 1, content_w - 4);

            // Hints then progress bar at very bottom
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(max_y - 2, 1, "[Space] Show Answer  [a] Ask AI  [q] Quit");
            attroff(COLOR_PAIR(CLR_DIM));
            int total_cards_q = (int)session.cards.size();
            if (total_cards_q > 0)
            {
                int n = total_cards_q;
                int gaps = n - 1;
                int usable = max_x - gaps;
                int base_w = usable / n;
                int extra = usable % n;
                if (base_w < 1)
                {
                    base_w = 1;
                    extra = 0;
                }
                move(max_y - 1, 0);
                for (int i = 0; i < n; i++)
                {
                    int start_extra = (n - extra) / 2;
                    int w = base_w + (i >= start_extra && i < start_extra + extra ? 1 : 0);
                    if (i < mastered)
                        attron(COLOR_PAIR(CLR_CORRECT));
                    else
                        attron(COLOR_PAIR(CLR_DIM));
                    for (int j = 0; j < w; j++)
                        addch(ACS_HLINE);
                    if (i < mastered)
                        attroff(COLOR_PAIR(CLR_CORRECT));
                    else
                        attroff(COLOR_PAIR(CLR_DIM));
                    if (i < n - 1) addch(' ');
                }
            }

            refresh();

            int ch = getch();
            if (ch == ERR) continue; // timeout - redraw for timer
            if (ch == 'q' || ch == 27)
            {
                session.round.push_back(card_idx); // put card back
                int elapsed = (int)difftime(time(nullptr), session_start);
                session.save_session(deck_path, elapsed);
                timeout(-1);
                return false;
            }
            if (ch == 'a')
            {
                show_ai_assistant(card, session.deck_name);
                timeout(1000);
                continue;
            }
            if (ch == ' ') break;
        }

        // --- Show answer ---
        while (true)
        {
            int max_y, max_x;
            getmaxyx(stdscr, max_y, max_x);
            clear();

            // Top status bar
            int mastered = session.mastered_count();
            attron(COLOR_PAIR(CLR_DIM));
            char status_left[128];
            snprintf(status_left, sizeof(status_left), "%d/%d mastered", mastered,
                     (int)session.cards.size());
            mvprintw(0, 1, "%s", status_left);

            char status_mid[64];
            snprintf(status_mid, sizeof(status_mid), "Round %d", session.round_num);
            attron(COLOR_PAIR(CLR_HEADER) | A_BOLD);
            mvprintw(0, (max_x - (int)strlen(status_mid)) / 2, "%s", status_mid);
            attroff(COLOR_PAIR(CLR_HEADER) | A_BOLD);

            attron(COLOR_PAIR(CLR_DIM));
            char status_right[128];
            snprintf(status_right, sizeof(status_right), "%d left | %d queued",
                     (int)session.round.size(), (int)session.missed.size());
            mvprintw(0, max_x - (int)strlen(status_right) - 1, "%s", status_right);
            attroff(COLOR_PAIR(CLR_DIM));

            draw_hline_full(1, 0, max_x);

            // Card box - centered, with question and answer
            int content_w = std::min(max_x - 6, 60);
            auto q_wrapped = format_display_text(card.question, content_w - 4, CLR_DIM, false);
            auto a_wrapped = format_display_text(card.answer, content_w - 4, CLR_DEFAULT, true);
            int card_h = (int)q_wrapped.size() + (int)a_wrapped.size() + 10;
            card_h = std::min(card_h, std::max(8, max_y - 9));
            int box_x = (max_x - content_w) / 2;
            int box_y = std::max(6, (max_y - card_h) / 2);

            draw_box(box_y, box_x, card_h, content_w);

            int inner_x = box_x + 2;
            int y = box_y + 1;

            // Stage + streak
            attron(COLOR_PAIR(stage_color(stage)));
            mvprintw(y, inner_x, "[%s]", stage_label(stage));
            attroff(COLOR_PAIR(stage_color(stage)));

            char streak_str[32];
            snprintf(streak_str, sizeof(streak_str), "%d/%d", streak, card_target);
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(y, box_x + content_w - 2 - (int)strlen(streak_str), "%s", streak_str);
            attroff(COLOR_PAIR(CLR_DIM));
            y += 2;

            // Question
            draw_styled_lines(q_wrapped, y, inner_x, box_y + card_h - 1, content_w - 4);
            y++;

            // Separator inside box
            attron(COLOR_PAIR(CLR_BORDER));
            mvhline(y, box_x + 1, ACS_HLINE, content_w - 2);
            attroff(COLOR_PAIR(CLR_BORDER));
            y += 2;

            // Answer
            draw_styled_lines(a_wrapped, y, inner_x, box_y + card_h - 1, content_w - 4);

            // Hints inline then progress bar at very bottom
            attron(COLOR_PAIR(CLR_CORRECT));
            mvprintw(max_y - 3, 1, "[y] Yes (%d/%d)", streak + 1, card_target);
            attroff(COLOR_PAIR(CLR_CORRECT));
            attron(COLOR_PAIR(CLR_WRONG));
            mvprintw(max_y - 3, 20, "[n] No (drop)");
            attroff(COLOR_PAIR(CLR_WRONG));
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(max_y - 2, 1, "[a] Ask AI  [q] Quit");
            attroff(COLOR_PAIR(CLR_DIM));
            int total_cards_a = (int)session.cards.size();
            if (total_cards_a > 0)
            {
                move(max_y - 1, 0);
                for (int i = 0; i < total_cards_a; i++)
                {
                    int seg_start = (i * max_x) / total_cards_a;
                    int seg_end = ((i + 1) * max_x) / total_cards_a;
                    int seg_w = seg_end - seg_start;
                    if (i < mastered)
                        attron(COLOR_PAIR(CLR_CORRECT));
                    else
                        attron(COLOR_PAIR(CLR_DIM));
                    for (int j = 0; j < seg_w; j++)
                        addch(ACS_HLINE);
                    if (i < mastered)
                        attroff(COLOR_PAIR(CLR_CORRECT));
                    else
                        attroff(COLOR_PAIR(CLR_DIM));
                }
            }

            refresh();

            int ch = getch();
            if (ch == ERR) continue; // timeout - redraw for timer
            if (ch == 'q' || ch == 27)
            {
                session.round.push_back(card_idx); // put card back
                int elapsed = (int)difftime(time(nullptr), session_start);
                session.save_session(deck_path, elapsed);
                timeout(-1);
                return false;
            }
            if (ch == 'a')
            {
                show_ai_assistant(card, session.deck_name);
                timeout(1000);
                continue;
            }
            if (ch == 'y')
            {
                session.mark_correct(card_idx);
                break;
            }
            if (ch == 'n')
            {
                session.mark_wrong(card_idx);
                break;
            }
        }
    }
}

int main(int argc, char* argv[])
{
    // Init ncurses
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) { init_colors(); }

    AppConfig config;
    if (!config.load())
    {
        if (!run_first_time_setup())
        {
            endwin();
            return 0;
        }
    }

    std::string deck_root = expand_home(g_deck_dir);

    // Load progress
    Progress progress;
    progress.load();

    while (true)
    {
        int splash_ch = show_splash();
        if (splash_ch == 'q') break;

        // Resume paused session
        if (splash_ch == 'c')
        {
            auto saved = DrillSession::load_session();
            if (saved.contains("deck_path"))
            {
                std::string deck_path = saved["deck_path"].get<std::string>();
                auto deck = parse_deck(deck_path);
                if (!deck.cards.empty())
                {
                    DrillSession session;
                    session.restore(saved, std::move(deck.cards), &progress);
                    int elapsed = saved.value("elapsed", 0);
                    run_drill(session, deck_path, elapsed);
                    continue;
                }
            }
            DrillSession::clear_session();
            continue;
        }

        // Normal browse flow
        while (true)
        {
            std::string deck_path = browse_decks(deck_root);
            if (deck_path.empty()) break;

            auto deck = parse_deck(deck_path);
            if (deck.cards.empty()) continue;

            std::string deck_id = deck.id.empty() ? deck_id_from_path(deck_path, deck_root) : deck.id;
            std::string dname = deck.title.empty() ? deck_id : deck.title;
            auto slash = dname.rfind('/');
            if (slash != std::string::npos) dname = dname.substr(slash + 1);
            std::replace(dname.begin(), dname.end(), '_', ' ');
            std::replace(dname.begin(), dname.end(), '-', ' ');

            if (!show_deck_summary(dname, deck.summary, deck_id, deck.cards, progress))
            {
                continue;
            }

            DrillSession session;
            session.init(deck_id, std::move(deck.cards), &progress);
            session.deck_name = dname;
            run_drill(session, deck_path);
        }
    }

    endwin();
    return 0;
}
