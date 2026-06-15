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

#ifndef GRIMOIRE_VERSION
#define GRIMOIRE_VERSION "0.2.0"
#endif

static const std::string DEFAULT_LIBRARY_ROOT = "~/grimoire_knowledge_vault";
static const std::string CONFIG_FILE = "~/.config/grimoire/config.json";
static const std::string DEFAULT_DATA_FILE = "~/.local/share/grimoire/progress.json";
static const std::string DEFAULT_SESSION_FILE = "~/.local/share/grimoire/session.json";

static std::string g_library_root = DEFAULT_LIBRARY_ROOT;
static std::string g_vault_root;
static std::string g_deck_dir;
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

static std::string normalize_path(const std::string& path)
{
    if (path.empty()) return "";
    try
    {
        return fs::absolute(expand_home(path)).lexically_normal().string();
    }
    catch (...)
    {
        return expand_home(path);
    }
}

static std::string iso_date(time_t timestamp = time(nullptr))
{
    char buf[32];
    std::tm* local = std::localtime(&timestamp);
    if (!local) return "";
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", local);
    return buf;
}

static int days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

static bool parse_iso_date(const std::string& value, int& year, int& month, int& day)
{
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    try
    {
        year = std::stoi(value.substr(0, 4));
        month = std::stoi(value.substr(5, 2));
        day = std::stoi(value.substr(8, 2));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

static int iso_date_to_day_number(const std::string& value)
{
    int year = 0, month = 0, day = 0;
    if (!parse_iso_date(value, year, month, day)) return 0;
    return days_from_civil(year, (unsigned)month, (unsigned)day);
}

// Inverse of days_from_civil (Howard Hinnant's algorithm): day number -> y/m/d.
static void civil_from_days(int z, int& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int yr = (int)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = yr + (m <= 2);
}

static std::string iso_date_from_day_number(int day_number)
{
    int y = 0;
    unsigned m = 0, d = 0;
    civil_from_days(day_number, y, m, d);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, (int)m, (int)d);
    return buf;
}

// SM-2-lite scheduling. grade: 0 = Again, 1 = Good, 2 = Easy.
static json sr_update(json entry, int grade, int today_day)
{
    double ease = entry.value("ease", 2.5);
    int interval = entry.value("interval", 0);
    int reps = entry.value("reps", 0);

    if (grade == 0) // Again
    {
        ease = std::max(1.3, ease - 0.2);
        reps = 0;
        interval = 1;
    }
    else if (grade == 1) // Good
    {
        if (reps == 0)
            interval = 1;
        else if (reps == 1)
            interval = 6;
        else
            interval = std::max(1, (int)(interval * ease + 0.5));
        reps += 1;
    }
    else // Easy
    {
        ease += 0.15;
        if (reps == 0)
            interval = 4;
        else
            interval = std::max(1, (int)(interval * ease * 1.3 + 0.5));
        reps += 1;
    }

    json out;
    out["ease"] = ease;
    out["interval"] = interval;
    out["reps"] = reps;
    out["due"] = iso_date_from_day_number(today_day + interval);
    out["last"] = iso_date_from_day_number(today_day);
    return out;
}

// Predicted next interval (days) for a grade without mutating state - used for UI previews.
static int sr_preview_interval(const json& entry, int grade)
{
    json copy = entry.is_null() ? json::object() : entry;
    return sr_update(copy, grade, 0).value("interval", 1);
}

struct ActivityMetrics
{
    int current_streak = 0;
    int longest_streak = 0;
    double consistency_rating = 0.0;
};

static ActivityMetrics compute_activity_metrics(std::vector<std::string> active_dates)
{
    ActivityMetrics metrics;
    std::sort(active_dates.begin(), active_dates.end());
    active_dates.erase(std::unique(active_dates.begin(), active_dates.end()), active_dates.end());

    int today_day = iso_date_to_day_number(iso_date());
    int active_last_30 = 0;
    int previous_day = 0;
    int run = 0;

    for (const auto& date : active_dates)
    {
        int day = iso_date_to_day_number(date);
        if (day == 0) continue;
        if (today_day - day >= 0 && today_day - day < 30) active_last_30++;

        if (run == 0 || day != previous_day + 1)
            run = 1;
        else
            run++;

        metrics.longest_streak = std::max(metrics.longest_streak, run);
        previous_day = day;
    }

    if (!active_dates.empty())
    {
        int last_day = iso_date_to_day_number(active_dates.back());
        if (last_day == today_day || last_day == today_day - 1)
        {
            metrics.current_streak = 1;
            int expected = last_day;
            for (int i = (int)active_dates.size() - 2; i >= 0; i--)
            {
                int day = iso_date_to_day_number(active_dates[i]);
                if (day == expected - 1)
                {
                    metrics.current_streak++;
                    expected = day;
                    continue;
                }
                break;
            }
        }
    }

    metrics.consistency_rating = active_last_30 * (100.0 / 30.0);
    return metrics;
}

struct LibraryRegistry
{
    std::string current_vault;
    std::vector<std::string> known_vaults;
};

static LibraryRegistry load_library_registry()
{
    LibraryRegistry registry;
    std::ifstream file(fs::path(normalize_path(g_library_root)) / "registry.json");
    if (!file.is_open()) return registry;
    try
    {
        json data = json::parse(file);
        registry.current_vault = data.value("current_vault", "");
        registry.known_vaults = data.value("known_vaults", std::vector<std::string>{});
    }
    catch (...)
    {
    }
    return registry;
}

struct SplashSummary
{
    int current_streak = 0;
    int sessions_completed = 0;
    int current_vault_decks = 0;
    double consistency_rating = 0.0;
    std::string improvement_deck_id;
};

static SplashSummary load_splash_summary()
{
    SplashSummary summary;
    std::ifstream file(fs::path(normalize_path(g_library_root)) / "library_metadata.json");
    if (!file.is_open()) return summary;
    try
    {
        json data = json::parse(file);
        auto vaults = data.value("vaults", json::array());
        for (const auto& vault : vaults)
        {
            if (!vault.is_object()) continue;
            if (vault.value("path", "") != g_vault_root) continue;
            summary.current_vault_decks = vault.value("deck_count", 0);
            json vault_summary = vault.value("summary", json::object());
            summary.current_streak = vault_summary.value("current_day_streak", 0);
            summary.sessions_completed = vault_summary.value("sessions_completed", 0);
            summary.consistency_rating = vault_summary.value("consistency_rating", 0.0);
            summary.improvement_deck_id = vault_summary.value("focus_deck_id", "");
            break;
        }

        if (summary.current_vault_decks == 0)
        {
            summary.current_streak = 0;
            summary.sessions_completed = 0;
            summary.consistency_rating = 0.0;
            summary.improvement_deck_id.clear();
        }
    }
    catch (...)
    {
    }
    return summary;
}

struct AppConfig
{
    std::string library_root;
    std::string current_vault;
    std::vector<std::string> known_vaults;

    std::string registry_path() const
    {
        std::string root = library_root.empty() ? normalize_path(DEFAULT_LIBRARY_ROOT) : library_root;
        return (fs::path(root) / "registry.json").string();
    }

    void apply() const
    {
        g_library_root = library_root.empty() ? normalize_path(DEFAULT_LIBRARY_ROOT) : library_root;
        g_vault_root = current_vault;
        g_deck_dir = g_vault_root + "/decks";
        g_data_file = DEFAULT_DATA_FILE;
        g_session_file = DEFAULT_SESSION_FILE;
    }

    void ensure_consistency()
    {
        if (library_root.empty()) library_root = normalize_path(DEFAULT_LIBRARY_ROOT);
        library_root = normalize_path(library_root);

        if (current_vault.empty() && !known_vaults.empty()) current_vault = known_vaults.front();
        if (current_vault.empty()) return;

        current_vault = normalize_path(current_vault);
        bool found = false;
        for (auto& vault : known_vaults)
        {
            vault = normalize_path(vault);
            if (vault == current_vault) found = true;
        }
        if (!found) known_vaults.insert(known_vaults.begin(), current_vault);

        std::sort(known_vaults.begin(), known_vaults.end());
        known_vaults.erase(std::unique(known_vaults.begin(), known_vaults.end()), known_vaults.end());
    }

    void set_current_vault(const std::string& path)
    {
        current_vault = normalize_path(path);
        ensure_consistency();
    }

    void set_library_root(const std::string& path)
    {
        library_root = normalize_path(path);
        ensure_consistency();
    }

    bool load()
    {
        std::ifstream file(expand_home(CONFIG_FILE));
        if (!file.is_open()) return false;

        json data;
        try
        {
            data = json::parse(file);
        }
        catch (...)
        {
            return false;
        }

        library_root = data.value("library_root", "");
        if (library_root.empty()) library_root = DEFAULT_LIBRARY_ROOT;
        library_root = normalize_path(library_root);

        std::ifstream registry_file(registry_path());
        if (registry_file.is_open())
        {
            try
            {
                json registry = json::parse(registry_file);
                current_vault = registry.value("current_vault", "");
                known_vaults = registry.value("known_vaults", std::vector<std::string>{});
            }
            catch (...)
            {
                return false;
            }
        }
        else
        {
            current_vault = data.value("current_vault", "");
            known_vaults = data.value("known_vaults", std::vector<std::string>{});
            if (current_vault.empty()) current_vault = data.value("vault_root", "");
        }

        ensure_consistency();
        if (current_vault.empty()) return false;
        apply();
        return true;
    }

    bool save() const
    {
        AppConfig normalized = *this;
        normalized.ensure_consistency();

        std::string path = expand_home(CONFIG_FILE);
        fs::create_directories(fs::path(path).parent_path());

        std::ofstream file(path);
        if (!file.is_open()) return false;

        json data;
        data["library_root"] = normalized.library_root;
        file << data.dump(2);

        fs::create_directories(normalized.library_root);
        std::ofstream registry_file(normalized.registry_path());
        if (!registry_file.is_open()) return false;

        json registry;
        registry["current_vault"] = normalized.current_vault;
        registry["known_vaults"] = normalized.known_vaults;
        registry_file << registry.dump(2);
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

static std::string make_note_ref_portable(const std::string& path)
{
    if (path.empty()) return "";

    fs::path absolute = fs::absolute(path);
    fs::path vault_root = fs::absolute(expand_home(g_vault_root));

    std::string abs_str = absolute.lexically_normal().string();
    std::string root_str = vault_root.lexically_normal().string();

    if (abs_str == root_str) return ".";
    if (abs_str.size() > root_str.size() && abs_str.compare(0, root_str.size(), root_str) == 0 &&
        abs_str[root_str.size()] == '/')
        return abs_str.substr(root_str.size() + 1);
    return collapse_home(abs_str);
}

static bool save_deck(const std::string& path, const Deck& deck)
{
    std::ofstream file(path);
    if (!file.is_open()) return false;

    bool has_metadata = !deck.id.empty() || !deck.title.empty();
    if (has_metadata)
    {
        file << "---\n";
        if (!deck.id.empty()) file << "deck_id: " << deck.id << "\n";
        if (!deck.title.empty()) file << "title: " << deck.title << "\n";
        file << "---\n";
    }

    if (!deck.summary.empty())
    {
        file << deck.summary << "\n";
        file << "---\n";
    }

    for (size_t i = 0; i < deck.cards.size(); i++)
    {
        const auto& card = deck.cards[i];
        file << "Q:\n" << card.question << "\n";
        file << "A:\n" << card.answer << "\n";
        if (!card.id.empty()) file << "ID:\n" << card.id << "\n";
        if (!card.note_ref.empty()) file << "NOTE:\n" << card.note_ref << "\n";
        if (i + 1 < deck.cards.size()) file << "\n";
    }

    return true;
}

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
    bool in_answer_fence = false;

    auto flush_card = [&]()
    {
        if (!in_card) return;
        if (!current_card.question.empty() && !current_card.answer.empty())
            deck.cards.push_back(current_card);
        current_card = Card{};
        in_card = false;
        block_field = BlockField::None;
        in_answer_fence = false;
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
            in_answer_fence = false;
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
            std::string answer_trim = trim(line);
            // Track fenced code: a ' :: ' inside ``` ... ``` is never a card boundary.
            if (starts_with(answer_trim, "```"))
            {
                in_answer_fence = !in_answer_fence;
                append_line(current_card.answer, line);
                continue;
            }
            // Outside code, a top-level "Question :: Answer" line starts a NEW
            // single-line card, so block cards and inline cards can interleave.
            if (!in_answer_fence && line == answer_trim)
            {
                auto inline_segments = split_card_segments(line);
                if (inline_segments.size() >= 2 &&
                    !trim(inline_segments[0]).empty() &&
                    !trim(inline_segments[1]).empty())
                {
                    flush_card();
                    current_card = Card{};
                    current_card.question = trim(inline_segments[0]);
                    current_card.answer = trim(inline_segments[1]);
                    apply_inline_card_metadata(current_card, inline_segments);
                    in_card = true;
                    block_field = BlockField::None;
                    continue;
                }
            }
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

static std::string deck_id_from_path(const std::string& path, const std::string& root);
static void write_library_metadata(const struct Progress& progress);

static std::vector<std::string> list_deck_files_recursive(const std::string& root)
{
    std::vector<std::string> files;
    if (root.empty() || !fs::exists(root)) return files;
    for (auto& entry : fs::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".txt") files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

struct Progress
{
    json drill_mastery;
    json deck_stats;
    json session_history;
    json schedule;    // spaced-repetition schedule keyed by "<deck_id>:<card_key>" (dormant)
    json card_stats;  // per-card right/wrong/last_seen tally, keyed like sched_key

    void load()
    {
        std::string path = expand_home(g_data_file);
        std::ifstream file(path);
        if (!file.is_open())
        {
            drill_mastery = json::object();
            deck_stats = json::object();
            session_history = json::array();
            schedule = json::object();
            card_stats = json::object();
            return;
        }
        try
        {
            json data = json::parse(file);
            drill_mastery = data.value("drill_mastery", json::object());
            deck_stats = data.value("deck_stats", json::object());
            session_history = data.value("session_history", json::array());
            schedule = data.value("schedule", json::object());
            card_stats = data.value("card_stats", json::object());
        }
        catch (...)
        {
            drill_mastery = json::object();
            deck_stats = json::object();
            session_history = json::array();
            schedule = json::object();
            card_stats = json::object();
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
        data["session_history"] = session_history;
        data["schedule"] = schedule;
        data["card_stats"] = card_stats;
        file << data.dump(2);
        write_library_metadata(*this);
    }

    // Schedule key mirrors the mastery keying: stable card id when present, else index.
    std::string sched_key(const std::string& deck_id, const std::string& card_key,
                          int fallback_idx) const
    {
        if (!card_key.empty()) return deck_id + ":" + card_key;
        return deck_id + ":" + std::to_string(fallback_idx);
    }

    json get_schedule(const std::string& key) const
    {
        if (schedule.is_object() && schedule.contains(key)) return schedule[key];
        return json();
    }

    void set_schedule(const std::string& key, const json& entry)
    {
        if (!schedule.is_object()) schedule = json::object();
        schedule[key] = entry;
    }

    json get_card_stat(const std::string& key) const
    {
        if (card_stats.is_object() && card_stats.contains(key)) return card_stats[key];
        return json();
    }

    // Record one answer to a card. Drives the drill-review weakness pool.
    void record_answer(const std::string& deck_id, const std::string& card_key, int fallback_idx,
                       bool correct)
    {
        if (!card_stats.is_object()) card_stats = json::object();
        std::string key = sched_key(deck_id, card_key, fallback_idx);
        json& entry = card_stats[key];
        if (!entry.is_object()) entry = json::object();
        if (correct)
            entry["right"] = entry.value("right", 0) + 1;
        else
            entry["wrong"] = entry.value("wrong", 0) + 1;
        entry["last_seen"] = iso_date();
    }

    // A deck is eligible for drill review once at least one drill session has been completed.
    bool deck_completed(const std::string& deck_id) const
    {
        if (!deck_stats.is_object() || !deck_stats.contains(deck_id)) return false;
        return deck_stats[deck_id].value("completed", 0) > 0;
    }

    // ISO date of the most recent completed drill of this deck, or "" if never completed.
    std::string last_completed_date(const std::string& deck_id) const
    {
        std::string best;
        long long best_ts = -1;
        if (!session_history.is_array()) return best;
        for (const auto& e : session_history)
        {
            if (!e.is_object()) continue;
            if (e.value("deck_id", "") != deck_id) continue;
            if (e.value("result", "") != "completed") continue;
            long long ts = e.value("timestamp", 0LL);
            if (ts > best_ts)
            {
                best_ts = ts;
                best = e.value("date", "");
            }
        }
        return best;
    }

    // Aggregate correct rate (0-100) across a deck's cards, or -1 if never answered.
    int deck_correct_rate(const std::string& deck_id, const std::vector<Card>& cards) const
    {
        int right = 0, wrong = 0;
        for (size_t i = 0; i < cards.size(); i++)
        {
            json st = get_card_stat(sched_key(deck_id, cards[i].id, (int)i));
            if (!st.is_object()) continue;
            right += st.value("right", 0);
            wrong += st.value("wrong", 0);
        }
        if (right + wrong == 0) return -1;
        return (right * 100) / (right + wrong);
    }

    int get_stage(const std::string& deck_id, int card_idx) const
    {
        std::string key = deck_id + ":" + std::to_string(card_idx);
        if (drill_mastery.contains(key)) { return drill_mastery[key].get<int>(); }
        return 0;
    }

    int get_stage(const std::string& deck_id, const std::string& card_key, int fallback_idx) const
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

    void record_session(const std::string& vault_root, const std::string& deck_id,
                        const std::string& deck_path, int card_count, int duration_seconds,
                        bool completed)
    {
        auto& stats = deck_stats[deck_id];
        if (!stats.is_object()) stats = json::object();
        if (completed)
            stats["completed"] = stats.value("completed", 0) + 1;
        else
            stats["paused"] = stats.value("paused", 0) + 1;

        json event;
        event["date"] = iso_date();
        event["timestamp"] = (long long)time(nullptr);
        event["vault_root"] = vault_root;
        event["deck_id"] = deck_id;
        event["deck_path"] = deck_path;
        event["card_count"] = card_count;
        event["duration_seconds"] = duration_seconds;
        event["result"] = completed ? "completed" : "paused";
        session_history.push_back(event);
    }
};

static json build_library_metadata(const Progress& progress)
{
    json metadata;
    metadata["generated_at"] = (long long)time(nullptr);
    metadata["generated_date"] = iso_date();
    metadata["library_root"] = g_library_root;

    LibraryRegistry registry = load_library_registry();
    metadata["current_vault"] = registry.current_vault;

    int sessions_completed = 0;
    int sessions_paused = 0;
    for (auto it = progress.deck_stats.begin(); it != progress.deck_stats.end(); ++it)
    {
        const auto& stats = it.value();
        sessions_completed += stats.value("completed", 0);
        sessions_paused += stats.value("paused", 0);
    }

    std::vector<std::string> active_dates;
    for (const auto& event : progress.session_history)
    {
        if (!event.is_object()) continue;
        std::string date = event.value("date", "");
        if (!date.empty()) active_dates.push_back(date);
    }
    ActivityMetrics library_metrics = compute_activity_metrics(active_dates);

    json improvement = json::array();
    struct WeakDeck
    {
        double mastery_rate = 1.0;
        int tracked_cards = 0;
        int strong_cards = 0;
        std::string deck_id;
        std::string vault_root;
    };
    std::vector<WeakDeck> weak_decks;
    std::map<std::string, std::vector<WeakDeck>> weak_decks_by_vault;

    json vaults = json::array();
    int total_decks = 0;
    for (const auto& vault_root : registry.known_vaults)
    {
        json vault;
        vault["path"] = vault_root;
        vault["label"] = fs::path(vault_root).filename().string();
        vault["is_current"] = vault_root == registry.current_vault;

        std::vector<std::string> deck_files = list_deck_files_recursive(vault_root + "/decks");
        vault["deck_count"] = (int)deck_files.size();
        total_decks += (int)deck_files.size();

        json decks = json::array();
        for (const auto& deck_path : deck_files)
        {
            Deck deck = parse_deck(deck_path);
            std::string deck_root = normalize_path(vault_root + "/decks");
            std::string deck_id = deck.id.empty() ? deck_id_from_path(deck_path, deck_root) : deck.id;
            int tracked = 0;
            int strong = 0;
            for (size_t i = 0; i < deck.cards.size(); i++)
            {
                int stage = progress.get_stage(deck_id, deck.cards[i].id, (int)i);
                tracked++;
                if (stage >= 2) strong++;
            }

            auto deck_stat_it = progress.deck_stats.find(deck_id);
            int completed = 0;
            int paused = 0;
            if (deck_stat_it != progress.deck_stats.end())
            {
                completed = deck_stat_it.value().value("completed", 0);
                paused = deck_stat_it.value().value("paused", 0);
            }

            json deck_entry;
            deck_entry["id"] = deck_id;
            deck_entry["title"] =
                deck.title.empty() ? fs::path(deck_path).stem().string() : deck.title;
            deck_entry["path"] = deck_path;
            deck_entry["card_count"] = (int)deck.cards.size();
            deck_entry["strong_cards"] = strong;
            deck_entry["mastery_rate"] = tracked > 0 ? (double)strong / tracked : 0.0;
            deck_entry["completed_sessions"] = completed;
            deck_entry["paused_sessions"] = paused;
            decks.push_back(deck_entry);

            if (tracked > 0)
            {
                WeakDeck weak{tracked > 0 ? (double)strong / tracked : 0.0, tracked, strong, deck_id,
                              vault_root};
                weak_decks.push_back(weak);
                weak_decks_by_vault[vault_root].push_back(weak);
            }
        }

        int vault_sessions_completed = 0;
        int vault_sessions_paused = 0;
        std::vector<std::string> vault_dates;
        for (const auto& event : progress.session_history)
        {
            if (!event.is_object()) continue;
            if (event.value("vault_root", "") != vault_root) continue;
            std::string result = event.value("result", "");
            if (result == "completed")
                vault_sessions_completed++;
            else if (result == "paused")
                vault_sessions_paused++;

            std::string date = event.value("date", "");
            if (!date.empty()) vault_dates.push_back(date);
        }
        ActivityMetrics vault_metrics = compute_activity_metrics(vault_dates);

        std::sort(weak_decks_by_vault[vault_root].begin(), weak_decks_by_vault[vault_root].end(),
                  [](const WeakDeck& a, const WeakDeck& b)
                  {
                      if (a.mastery_rate != b.mastery_rate) return a.mastery_rate < b.mastery_rate;
                      return a.tracked_cards > b.tracked_cards;
                  });

        std::string focus_deck_id;
        if (!weak_decks_by_vault[vault_root].empty())
            focus_deck_id = weak_decks_by_vault[vault_root].front().deck_id;

        vault["summary"] = {
            {"sessions_completed", vault_sessions_completed},
            {"sessions_paused", vault_sessions_paused},
            {"completion_rate",
             vault_sessions_completed + vault_sessions_paused > 0
                 ? (double)vault_sessions_completed /
                       (vault_sessions_completed + vault_sessions_paused)
                 : 0.0},
            {"current_day_streak", vault_metrics.current_streak},
            {"longest_day_streak", vault_metrics.longest_streak},
            {"consistency_rating", vault_metrics.consistency_rating},
            {"focus_deck_id", focus_deck_id}};
        vault["decks"] = decks;
        vaults.push_back(vault);
    }

    std::sort(weak_decks.begin(), weak_decks.end(),
              [](const WeakDeck& a, const WeakDeck& b)
              {
                  if (a.mastery_rate != b.mastery_rate) return a.mastery_rate < b.mastery_rate;
                  return a.tracked_cards > b.tracked_cards;
              });

    for (size_t i = 0; i < weak_decks.size() && i < 5; i++)
    {
        json item;
        item["deck_id"] = weak_decks[i].deck_id;
        item["vault_root"] = weak_decks[i].vault_root;
        item["mastery_rate"] = weak_decks[i].mastery_rate;
        item["tracked_cards"] = weak_decks[i].tracked_cards;
        item["strong_cards"] = weak_decks[i].strong_cards;
        improvement.push_back(item);
    }

    metadata["vaults"] = vaults;
    metadata["summary"] = {
        {"vault_count", (int)registry.known_vaults.size()},
        {"deck_count", total_decks},
        {"sessions_completed", sessions_completed},
        {"sessions_paused", sessions_paused},
        {"completion_rate",
         sessions_completed + sessions_paused > 0
             ? (double)sessions_completed / (sessions_completed + sessions_paused)
             : 0.0},
        {"consistency_rating", library_metrics.consistency_rating},
        {"current_day_streak", library_metrics.current_streak},
        {"longest_day_streak", library_metrics.longest_streak},
        {"areas_of_improvement", improvement}};
    return metadata;
}

static void write_library_metadata(const Progress& progress)
{
    try
    {
        fs::create_directories(g_library_root);
        std::ofstream file(fs::path(g_library_root) / "library_metadata.json");
        if (!file.is_open()) return;
        file << build_library_metadata(progress).dump(2);
    }
    catch (...)
    {
    }
}

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
    int session_right = 0; // answers graded correct this sitting
    int session_wrong = 0; // answers graded wrong this sitting

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

    int mastered_count() const
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
        progress->record_answer(deck_id, cards[idx].id, idx, true);
        session_right++;
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
        progress->record_answer(deck_id, cards[idx].id, idx, false);
        session_wrong++;
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
        json all_sessions = load_all_sessions();
        json j;
        j["vault_root"] = g_vault_root;
        j["deck_path"] = path;
        j["deck_id"] = deck_id;
        j["deck_name"] = deck_name;
        j["round"] = round;
        j["missed"] = missed;
        j["streaks"] = streaks;
        j["targets"] = targets;
        j["round_num"] = round_num;
        j["elapsed"] = elapsed;
        j["saved_at"] = (long long)time(nullptr);
        if (!all_sessions.contains(g_vault_root) || !all_sessions[g_vault_root].is_object())
            all_sessions[g_vault_root] = json::object();
        all_sessions[g_vault_root][deck_id] = j;
        std::ofstream file(fpath);
        file << all_sessions.dump(2);
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

    static void clear_session(const std::string& deck_id = "")
    {
        std::string fpath = expand_home(g_session_file);
        if (!fs::exists(fpath)) return;
        json all_sessions = load_all_sessions();
        if (deck_id.empty())
        {
            all_sessions.erase(g_vault_root);
        }
        else if (all_sessions.contains(g_vault_root) && all_sessions[g_vault_root].is_object())
        {
            all_sessions[g_vault_root].erase(deck_id);
            if (all_sessions[g_vault_root].empty()) all_sessions.erase(g_vault_root);
        }
        if (all_sessions.empty())
        {
            fs::remove(fpath);
            return;
        }
        std::ofstream file(fpath);
        file << all_sessions.dump(2);
    }

    static json load_all_sessions()
    {
        std::string fpath = expand_home(g_session_file);
        std::ifstream file(fpath);
        if (!file.is_open()) return json::object();
        try
        {
            json parsed = json::parse(file);
            if (parsed.is_object() && parsed.contains("deck_name"))
            {
                json migrated = json::object();
                std::string vault_root = parsed.value("vault_root", "");
                std::string deck_id = parsed.value("deck_id", parsed.value("deck_path", ""));
                if (!vault_root.empty() && !deck_id.empty())
                    migrated[vault_root][deck_id] = parsed;
                return migrated;
            }
            if (parsed.is_object())
            {
                json migrated = json::object();
                for (auto it = parsed.begin(); it != parsed.end(); ++it)
                {
                    if (!it.value().is_object()) continue;
                    if (it.value().contains("deck_name"))
                    {
                        std::string deck_id =
                            it.value().value("deck_id", it.value().value("deck_path", ""));
                        if (!deck_id.empty()) migrated[it.key()][deck_id] = it.value();
                    }
                    else
                    {
                        migrated[it.key()] = it.value();
                    }
                }
                return migrated;
            }
            return json::object();
        }
        catch (...)
        {
            return json::object();
        }
    }

    static json load_session(const std::string& deck_id = "")
    {
        json all_sessions = load_all_sessions();
        if (!all_sessions.is_object()) return json();
        if (!all_sessions.contains(g_vault_root)) return json();
        const json& vault_sessions = all_sessions[g_vault_root];
        if (!vault_sessions.is_object()) return json();
        if (!deck_id.empty())
        {
            if (!vault_sessions.contains(deck_id)) return json();
            return vault_sessions[deck_id];
        }
        json latest;
        long long latest_saved_at = -1;
        for (auto it = vault_sessions.begin(); it != vault_sessions.end(); ++it)
        {
            if (!it.value().is_object() || !it.value().contains("deck_name")) continue;
            long long saved_at = it.value().value("saved_at", 0LL);
            if (latest.is_null() || saved_at > latest_saved_at)
            {
                latest = it.value();
                latest_saved_at = saved_at;
            }
        }
        return latest.is_null() ? json() : latest;
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

enum class ContentBlockType
{
    Paragraph,
    Code
};

struct ContentBlock
{
    ContentBlockType type = ContentBlockType::Paragraph;
    std::string language;
    std::string text;
};

static bool is_language_char(char c)
{
    return std::isalnum((unsigned char)c) || c == '+' || c == '#' || c == '-' || c == '_';
}

static std::vector<ContentBlock> parse_card_content(const std::string& text)
{
    std::vector<ContentBlock> blocks;
    size_t pos = 0;

    while (pos < text.size())
    {
        size_t fence = text.find("```", pos);
        if (fence == std::string::npos)
        {
            if (pos < text.size())
                blocks.push_back({ContentBlockType::Paragraph, "", text.substr(pos)});
            break;
        }

        if (fence > pos)
            blocks.push_back({ContentBlockType::Paragraph, "", text.substr(pos, fence - pos)});

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
        blocks.push_back({ContentBlockType::Code, language, content.substr(code_start)});

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

static std::vector<StyledLine> wrap_styled_line(const StyledLine& line, int width)
{
    std::vector<StyledLine> wrapped;
    int safe_width = std::max(1, width);
    StyledLine current;
    int used = 0;

    auto flush = [&]()
    {
        wrapped.push_back(current);
        current.clear();
        used = 0;
    };

    for (const auto& span : line)
    {
        size_t pos = 0;
        while (pos < span.text.size())
        {
            if (used >= safe_width) flush();

            int room = safe_width - used;
            int take = std::min(room, (int)(span.text.size() - pos));
            current.push_back({span.text.substr(pos, take), span.color, span.bold});
            used += take;
            pos += take;

            if (used >= safe_width && pos < span.text.size()) flush();
        }
    }

    if (!current.empty() || wrapped.empty()) wrapped.push_back(current);
    return wrapped;
}

static std::vector<StyledLine> format_display_text(const std::string& text, int width, int text_color,
                                                   bool text_bold)
{
    std::vector<StyledLine> lines;
    int safe_width = std::max(1, width);

    for (const auto& block : parse_card_content(text))
    {
        if (block.type == ContentBlockType::Paragraph)
        {
            std::istringstream stream(block.text);
            std::string paragraph_line;
            while (std::getline(stream, paragraph_line))
            {
                if (paragraph_line.empty())
                {
                    lines.push_back({{std::string(), text_color, text_bold}});
                    continue;
                }
                auto wrapped = wrap_text(paragraph_line, safe_width);
                if (wrapped.empty()) wrapped.push_back("");
                for (const auto& line : wrapped)
                    lines.push_back({{line, text_color, text_bold}});
            }
            continue;
        }

        if (!lines.empty()) lines.push_back({{std::string(), CLR_DEFAULT, false}});

        if (!block.language.empty())
        {
            std::string label = "[" + block.language + "]";
            lines.push_back({{label, CLR_BORDER, false}});
        }

        std::istringstream stream(block.text);
        std::string code_line;
        int code_width = std::max(1, safe_width - 2);
        while (std::getline(stream, code_line))
        {
            auto spans = highlight_code_line(code_line, block.language);
            for (const auto& wrapped_line : wrap_styled_line(spans, code_width))
            {
                StyledLine styled = {{"  ", CLR_BORDER, false}};
                styled.insert(styled.end(), wrapped_line.begin(), wrapped_line.end());
                lines.push_back(styled);
            }
        }

        if (block.text.empty()) lines.push_back({{"  ", CLR_BORDER, false}});
        lines.push_back({{std::string(), CLR_DEFAULT, false}});
    }

    if (lines.empty()) lines.push_back({{std::string(), text_color, text_bold}});
    return lines;
}

static std::vector<std::string> format_note_text(const std::string& text, int width)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    int safe_width = std::max(1, width);

    while (std::getline(stream, line))
    {
        if (line.empty())
        {
            lines.push_back("");
            continue;
        }

        auto wrapped = wrap_text(line, safe_width);
        if (wrapped.empty()) wrapped.push_back("");
        lines.insert(lines.end(), wrapped.begin(), wrapped.end());
    }

    if (lines.empty()) lines.push_back("");
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

static void show_blocking_message(const std::string& title, const std::vector<std::string>& lines,
                                  const std::string& footer);
static std::string format_elapsed(time_t start);

static void show_note_reader(const std::string& path, const std::string& deck_name,
                             int round_num, time_t session_start, int queue_left,
                             int queued_count, int mastered, int total_cards, int stage,
                             int streak, int card_target)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        show_blocking_message("Note Open Failed",
                              {"Grimoire could not open this note:", collapse_home(path)},
                              "[Any key] back");
        return;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string note_text = buffer.str();

    int scroll = 0;
    int last_content_width = -1;
    std::vector<std::string> rendered_lines;

    while (true)
    {
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        clear();
        std::string elapsed = format_elapsed(session_start);
        attron(COLOR_PAIR(CLR_HEADER));
        mvprintw(0, 1, "%s", elapsed.c_str());
        mvprintw(0, (max_x - (int)deck_name.size()) / 2, "%s", deck_name.c_str());
        attroff(COLOR_PAIR(CLR_HEADER));

        char mastered_str[64];
        snprintf(mastered_str, sizeof(mastered_str), "%d/%d", mastered, total_cards);
        attron(COLOR_PAIR(CLR_HEADER));
        mvprintw(0, max_x - (int)strlen(mastered_str) - 1, "%s", mastered_str);
        attroff(COLOR_PAIR(CLR_HEADER));

        attron(COLOR_PAIR(CLR_HEADER));
        mvprintw(1, 1, "[drilling]");
        attroff(COLOR_PAIR(CLR_HEADER));

        char queue_str[128];
        snprintf(queue_str, sizeof(queue_str), "%d left | %d queued", queue_left, queued_count);
        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(1, max_x - (int)strlen(queue_str) - 1, "%s", queue_str);
        attroff(COLOR_PAIR(CLR_DIM));

        draw_hline_full(2, 0, max_x);

        char round_str[64];
        snprintf(round_str, sizeof(round_str), "Round %d", round_num);
        attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
        mvprintw(3, (max_x - (int)strlen(round_str)) / 2, "%s", round_str);
        attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);

        draw_hline_full(4, 0, max_x);

        int box_w = std::min(max_x - 6, 72);
        box_w = std::max(40, box_w);
        int box_h = std::min(max_y - 8, max_y - 6);
        box_h = std::max(12, box_h);
        int box_x = (max_x - box_w) / 2;
        int box_y = std::max(6, (max_y - box_h) / 2);
        int inner_x = box_x + 2;
        int inner_y = box_y + 3;
        int content_w = std::max(20, box_w - 4);
        int content_h = std::max(1, box_h - 7);

        if (content_w != last_content_width)
        {
            rendered_lines = format_note_text(note_text, content_w);
            scroll = std::min(scroll, std::max(0, (int)rendered_lines.size() - content_h));
            last_content_width = content_w;
        }

        int max_scroll = std::max(0, (int)rendered_lines.size() - content_h);
        scroll = std::max(0, std::min(scroll, max_scroll));

        draw_box(box_y, box_x, box_h, box_w);

        attron(COLOR_PAIR(stage_color(stage)));
        mvprintw(box_y, box_x + 2, "[%s]", stage_label(stage));
        attroff(COLOR_PAIR(stage_color(stage)));

        char streak_str[32];
        snprintf(streak_str, sizeof(streak_str), "%d/%d", streak, card_target);
        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(box_y, box_x + box_w - 2 - (int)strlen(streak_str), "%s", streak_str);
        attroff(COLOR_PAIR(CLR_DIM));

        std::string title = fs::path(path).filename().string();
        if ((int)title.size() > box_w - 6) title = title.substr(0, box_w - 6);
        attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
        mvprintw(box_y + 1, box_x + 2, "%s", title.c_str());
        attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);

        std::string path_line = collapse_home(path);
        if ((int)path_line.size() > box_w - 6) path_line = path_line.substr(0, box_w - 6);
        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(box_y + 2, box_x + 2, "%s", path_line.c_str());
        attroff(COLOR_PAIR(CLR_DIM));

        draw_hline_full(box_y + 3, box_x + 1, box_w - 2);

        int y = inner_y;
        for (int i = 0; i < content_h && (i + scroll) < (int)rendered_lines.size(); i++)
        {
            mvaddnstr(y, inner_x, rendered_lines[i + scroll].c_str(), content_w);
            y++;
        }

        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(max_y - 2, 1, "[j/k] Scroll  [g/G] Top/Bottom  [Ctrl-D/U] Half Page  [q] Back");
        attroff(COLOR_PAIR(CLR_DIM));
        if (total_cards > 0)
        {
            int n = total_cards;
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

        timeout(-1);
        int ch = getch();
        if (ch == 'q' || ch == 27) return;
        if (ch == 'j' || ch == KEY_DOWN)
        {
            if (scroll < max_scroll) scroll++;
            continue;
        }
        if (ch == 'k' || ch == KEY_UP)
        {
            if (scroll > 0) scroll--;
            continue;
        }
        if (ch == 'g')
        {
            int next = getch();
            if (next == 'g')
                scroll = 0;
            else
                ungetch(next);
            continue;
        }
        if (ch == 'G')
        {
            scroll = max_scroll;
            continue;
        }
        if (ch == 4 || ch == KEY_NPAGE)
        {
            scroll = std::min(max_scroll, scroll + std::max(1, content_h / 2));
            continue;
        }
        if (ch == 21 || ch == KEY_PPAGE)
        {
            scroll = std::max(0, scroll - std::max(1, content_h / 2));
            continue;
        }
    }
}

static void draw_centered_message(const std::string& title, const std::vector<std::string>& lines,
                                  const std::string& footer = "");
static std::string prompt_path_screen(const std::string& title, const std::vector<std::string>& lines,
                                      const std::string& default_value);
static std::string prompt_text_screen(const std::string& title, const std::vector<std::string>& lines,
                                      const std::string& label, const std::string& default_value);
static void ensure_library_dirs(const std::string& library_root);
static void ensure_vault_dirs(const std::string& vault_root);
static bool run_first_time_setup(AppConfig& config);

// --- AI ---

static const std::string OLLAMA_URL = "http://localhost:11434";
static std::string g_ai_model;

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

static std::string strip_note_anchor(const std::string& note_ref)
{
    size_t hash = note_ref.find('#');
    if (hash == std::string::npos) return note_ref;
    return note_ref.substr(0, hash);
}

static std::string resolve_note_ref_path(const std::string& note_ref)
{
    std::string base = trim(strip_note_anchor(note_ref));
    if (base.empty()) return "";

    std::string expanded = expand_home(base);
    if (fs::exists(expanded)) return expanded;

    fs::path vault_root = expand_home(g_vault_root);
    fs::path candidate = vault_root / base;
    if (fs::exists(candidate)) return candidate.string();

    candidate = vault_root / "notes" / base;
    if (fs::exists(candidate)) return candidate.string();

    return "";
}

static void show_blocking_message(const std::string& title, const std::vector<std::string>& lines,
                                  const std::string& footer = "[Any key] back")
{
    timeout(-1);
    draw_centered_message(title, lines, footer);
    getch();
}

static void open_note_ref(const Card& card, const std::string& deck_name, int round_num,
                          time_t session_start, int queue_left, int queued_count, int mastered,
                          int total_cards, int stage, int streak, int card_target)
{
    if (card.note_ref.empty())
    {
        show_blocking_message("No Note Reference",
                              {"This card does not have a linked note."});
        return;
    }

    std::string resolved = resolve_note_ref_path(card.note_ref);
    if (resolved.empty())
    {
        show_blocking_message("Note Not Found",
                              {"Grimoire could not resolve this note reference:",
                               card.note_ref});
        return;
    }

    show_note_reader(resolved, deck_name, round_num, session_start, queue_left, queued_count,
                     mastered, total_cards, stage, streak, card_target);
}

static bool assign_note_ref_for_card(const std::string& deck_path, std::vector<Card>& session_cards,
                                     int card_idx)
{
    if (card_idx < 0 || card_idx >= (int)session_cards.size()) return false;

    Card& card = session_cards[card_idx];
    std::string default_value = card.note_ref.empty() ? expand_home(g_vault_root) : card.note_ref;

    std::string input = prompt_path_screen(
        "Attach Note",
        {"Enter the note path to attach to this card.",
         "If the note is inside your vault, Grimoire will save it as a vault-relative path."},
        default_value);

    if (input.empty()) return false;

    std::string trimmed_input = trim(input);
    std::string stored_ref = trimmed_input;
    std::string maybe_path = expand_home(trimmed_input);

    if (fs::exists(maybe_path)) stored_ref = make_note_ref_portable(maybe_path);

    Deck deck = parse_deck(deck_path);
    if (card_idx >= (int)deck.cards.size())
    {
        show_blocking_message("Attach Failed",
                              {"The deck changed on disk and the current card could not be updated."});
        return false;
    }

    deck.cards[card_idx].note_ref = stored_ref;
    if (!save_deck(deck_path, deck))
    {
        show_blocking_message("Save Failed",
                              {"Grimoire could not write the updated deck file."});
        return false;
    }

    card.note_ref = stored_ref;
    show_blocking_message("Note Attached",
                          {"This card is now linked to:", stored_ref});
    return true;
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

struct TypedAnswerJudgement
{
    bool valid = false;
    bool correct = false;
    std::string feedback;
};

static std::string extract_json_object(const std::string& value)
{
    size_t start = value.find('{');
    size_t end = value.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end <= start) return "";
    return value.substr(start, end - start + 1);
}

static TypedAnswerJudgement judge_typed_answer(const std::string& model, const Card& card,
                                               const std::string& deck,
                                               const std::string& typed_answer)
{
    std::string system_prompt =
        "You grade flashcard answers. Compare the learner's typed answer to the expected answer. "
        "Be lenient for study recall. Accept short answers, paraphrases, synonyms, minor typos, "
        "and answers that give the core fact even if they omit minor supporting details. "
        "Do not require word-for-word matching. Mark incorrect only when the core answer is "
        "missing, contradicted, or too vague to show recall. "
        "Return only compact JSON with this exact shape: "
        "{\"correct\":true,\"feedback\":\"short plain text\"}. "
        "No markdown and no extra text.\n\n"
        "Deck: " +
        deck +
        "\nQuestion: " +
        card.question +
        "\nExpected answer: " +
        card.answer;

    json payload;
    payload["model"] = model;
    payload["prompt"] = "Learner answer: " + typed_answer;
    payload["system"] = system_prompt;
    payload["stream"] = false;

    std::string cmd = "curl -s -X POST " + OLLAMA_URL +
                      "/api/generate "
                      "-H 'Content-Type: application/json' "
                      "-d " +
                      shell_escape(payload.dump()) + " 2>/dev/null";

    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {false, false, "Could not run curl for AI grading."};
    std::array<char, 4096> buf;
    while (fgets(buf.data(), buf.size(), pipe))
    {
        result += buf.data();
    }
    pclose(pipe);

    if (result.empty()) return {false, false, "No response from Ollama."};

    try
    {
        auto resp = json::parse(result);
        if (resp.contains("error"))
            return {false, false, "Ollama error: " + resp["error"].get<std::string>()};
        if (!resp.contains("response"))
            return {false, false, "Could not read AI grading response."};

        std::string response = resp["response"].get<std::string>();
        std::string json_text = extract_json_object(response);
        if (json_text.empty()) json_text = response;
        auto verdict = json::parse(json_text);
        return {true, verdict.value("correct", false), verdict.value("feedback", response)};
    }
    catch (...)
    {
    }

    return {false, false, "Could not parse AI grading response."};
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

struct TextInputResult
{
    std::string value;
    bool cancelled = false;
};

static TextInputResult get_input_result(int y, int x, int max_w)
{
    std::string input;
    bool cancelled = false;
    const int max_input_len = 2000;
    curs_set(1);
    timeout(-1);
    while (true)
    {
        mvhline(y, x, ' ', max_w);
        int visible_w = std::max(1, max_w - 2);
        std::string visible = input;
        if ((int)visible.size() > visible_w)
            visible = visible.substr(visible.size() - visible_w);
        mvprintw(y, x, "> ");
        addnstr(visible.c_str(), visible_w);
        refresh();
        int ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) break;
        if (ch == 27)
        {
            cancelled = true;
            break;
        }
        if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && !input.empty()) { input.pop_back(); }
        else if (ch >= 32 && ch < 127 && (int)input.size() < max_input_len)
        {
            input += (char)ch;
        }
    }
    curs_set(0);
    return {input, cancelled};
}

static std::vector<std::string> hard_wrap_line(const std::string& text, int width)
{
    std::vector<std::string> lines;
    int safe_width = std::max(1, width);
    if (text.empty()) return {""};
    // Split on explicit newlines first, then hard-wrap each segment to the width.
    size_t start = 0;
    while (true)
    {
        size_t nl = text.find('\n', start);
        std::string chunk =
            text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (chunk.empty())
            lines.push_back("");
        else
            for (size_t i = 0; i < chunk.size(); i += safe_width)
                lines.push_back(chunk.substr(i, safe_width));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (lines.empty()) lines.push_back("");
    return lines;
}

// One wrapped display row of an editable buffer: a slice [start, start+len) of the source string
// (excluding any trailing newline). `hard` marks a row that ended on an explicit '\n'.
struct BufRow
{
    int start;
    int len;
    bool hard;
};

// Lay a buffer out into display rows: split on '\n', soft-wrap each segment at `width`.
static std::vector<BufRow> layout_buffer(const std::string& s, int width)
{
    std::vector<BufRow> rows;
    int w = std::max(1, width);
    int n = (int)s.size();
    int start = 0, col = 0;
    for (int i = 0; i < n; i++)
    {
        if (s[i] == '\n')
        {
            rows.push_back({start, i - start, true});
            start = i + 1;
            col = 0;
        }
        else if (++col == w)
        {
            rows.push_back({start, i - start + 1, false});
            start = i + 1;
            col = 0;
        }
    }
    rows.push_back({start, n - start, false}); // trailing (also the empty line after a final '\n')
    return rows;
}

// Map a cursor index to its (row, col) display position.
static void buffer_cursor_rc(const std::vector<BufRow>& rows, int cursor, int& crow, int& ccol)
{
    for (int r = 0; r < (int)rows.size(); r++)
    {
        int s = rows[r].start, l = rows[r].len, end = s + l;
        if (cursor < end)
        {
            crow = r;
            ccol = cursor - s;
            return;
        }
        if (cursor == end && (rows[r].hard || r == (int)rows.size() - 1))
        {
            crow = r;
            ccol = l;
            return;
        }
        // soft-wrap boundary (cursor == end, not hard): falls through to next row's column 0
    }
    crow = (int)rows.size() - 1;
    ccol = rows.back().len;
}

// Map a (row, col) display position back to a cursor index, clamped to the row.
static int buffer_rc_cursor(const std::vector<BufRow>& rows, int row, int col)
{
    row = std::max(0, std::min(row, (int)rows.size() - 1));
    return rows[row].start + std::max(0, std::min(col, rows[row].len));
}

// Modal (vi-style) multi-line text buffer. Starts in INSERT: type freely, Enter inserts a
// newline, Esc switches to NORMAL. In NORMAL: Enter submits, i re-enters insert, q/Esc cancels.
// Arrows (and h/j/k/l in normal mode) move the cursor; edits happen at the cursor position.
static TextInputResult get_wrapped_input_result(int y, int x, int max_w, int max_rows)
{
    std::string input;
    int cursor = 0;
    bool cancelled = false;
    bool insert_mode = true;
    const int max_input_len = 2000;
    int visible_w = std::max(1, max_w - 2);
    max_rows = std::max(1, max_rows);

    timeout(-1);
    while (true)
    {
        auto rows = layout_buffer(input, visible_w);
        int crow = 0, ccol = 0;
        buffer_cursor_rc(rows, cursor, crow, ccol);

        int start_row = crow >= max_rows ? crow - max_rows + 1 : 0;

        for (int i = 0; i < max_rows; i++)
            mvhline(y + i, x, ' ', max_w);
        for (int i = 0; i < max_rows && start_row + i < (int)rows.size(); i++)
        {
            const BufRow& r = rows[start_row + i];
            const std::string prefix = (start_row + i == 0) ? "> " : "  ";
            mvprintw(y + i, x, "%s", prefix.c_str());
            addnstr(input.c_str() + r.start, std::min(r.len, visible_w));
        }

        // Mode-aware status line, placed inside the card just below the input region.
        int status_y = y + max_rows;
        attron(COLOR_PAIR(CLR_DIM));
        mvhline(status_y, x, ' ', max_w);
        const char* status = insert_mode
                                 ? "-- INSERT --  [Enter] Newline  [Esc] Normal"
                                 : "-- NORMAL --  [Enter] Submit  [i] Insert  [q] Cancel";
        mvaddnstr(status_y, x, status, max_w);
        attroff(COLOR_PAIR(CLR_DIM));

        // Place the hardware cursor at the edit point so it shows in the box.
        curs_set(insert_mode ? 1 : 0);
        move(y + (crow - start_row), std::min(x + 2 + ccol, x + max_w - 1));
        refresh();

        int ch = getch();
        int n = (int)input.size();

        // Cursor movement, shared by both modes.
        if (ch == KEY_LEFT) { if (cursor > 0) cursor--; continue; }
        if (ch == KEY_RIGHT) { if (cursor < n) cursor++; continue; }
        if (ch == KEY_UP) { if (crow > 0) cursor = buffer_rc_cursor(rows, crow - 1, ccol); continue; }
        if (ch == KEY_DOWN)
        {
            if (crow < (int)rows.size() - 1) cursor = buffer_rc_cursor(rows, crow + 1, ccol);
            continue;
        }
        if (ch == KEY_HOME) { cursor = rows[crow].start; continue; }
        if (ch == KEY_END) { cursor = rows[crow].start + rows[crow].len; continue; }

        if (insert_mode)
        {
            if (ch == 27) { insert_mode = false; continue; } // Esc -> normal mode
            if (ch == '\n' || ch == '\r' || ch == KEY_ENTER)
            {
                if (n < max_input_len) { input.insert(cursor, 1, '\n'); cursor++; }
                continue;
            }
            if (ch == KEY_BACKSPACE || ch == 127 || ch == 8)
            {
                if (cursor > 0) { input.erase(cursor - 1, 1); cursor--; }
                continue;
            }
            if (ch == KEY_DC) // delete forward
            {
                if (cursor < n) input.erase(cursor, 1);
                continue;
            }
            if (ch >= 32 && ch < 127 && n < max_input_len)
            {
                input.insert(cursor, 1, (char)ch);
                cursor++;
            }
        }
        else // normal mode
        {
            if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) break; // submit
            if (ch == 'i') { insert_mode = true; continue; }
            if (ch == 'a') { insert_mode = true; if (cursor < n) cursor++; continue; }
            if (ch == 'A') { insert_mode = true; cursor = rows[crow].start + rows[crow].len; continue; }
            if (ch == 'q' || ch == 27) { cancelled = true; break; }
            if (ch == 'h') { if (cursor > 0) cursor--; continue; }
            if (ch == 'l') { if (cursor < n) cursor++; continue; }
            if (ch == 'k') { if (crow > 0) cursor = buffer_rc_cursor(rows, crow - 1, ccol); continue; }
            if (ch == 'j')
            {
                if (crow < (int)rows.size() - 1) cursor = buffer_rc_cursor(rows, crow + 1, ccol);
                continue;
            }
            if (ch == '0') { cursor = rows[crow].start; continue; }
            if (ch == '$') { cursor = rows[crow].start + rows[crow].len; continue; }
        }
    }
    curs_set(0);
    return {input, cancelled};
}

static std::string get_input(int y, int x, int max_w)
{
    return get_input_result(y, x, max_w).value;
}

static void draw_centered_message(const std::string& title, const std::vector<std::string>& lines,
                                  const std::string& footer)
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

    TextInputResult result = get_input_result(max_y - 3, 3, std::max(20, max_x - 6));
    if (result.cancelled) return "";
    if (result.value.empty()) return default_value;
    return result.value;
}

static std::string prompt_text_screen(const std::string& title, const std::vector<std::string>& lines,
                                      const std::string& label, const std::string& default_value)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    draw_centered_message(title, lines, "[Enter] confirm  [Esc] cancel");

    std::string prompt = label;
    if (!default_value.empty()) prompt += " [" + default_value + "]";
    mvprintw(max_y - 4, 3, "%s", prompt.c_str());

    TextInputResult result = get_input_result(max_y - 3, 3, std::max(20, max_x - 6));
    if (result.cancelled) return "";
    if (result.value.empty()) return default_value;
    return result.value;
}

static void ensure_library_dirs(const std::string& library_root)
{
    fs::create_directories(library_root);
    fs::create_directories(fs::path(library_root) / "vaults");
}

static void ensure_vault_dirs(const std::string& vault_root)
{
    fs::create_directories(fs::path(vault_root) / "decks");
    fs::create_directories(fs::path(vault_root) / "notes");
    fs::create_directories(fs::path(vault_root) / "media");
}

static std::string default_child_vault_root(const AppConfig& config)
{
    std::string library_root =
        config.library_root.empty() ? normalize_path(DEFAULT_LIBRARY_ROOT) : config.library_root;
    return (fs::path(library_root) / "vaults").string();
}

static std::string vault_display_name_for_paths(const std::string& library_root,
                                                const std::string& vault_path)
{
    std::string normalized_library = normalize_path(library_root);
    std::string normalized_vault = normalize_path(vault_path);
    if (normalized_vault.empty()) return "(none)";

    fs::path child_root = fs::path(normalized_library) / "vaults";
    fs::path vault = normalized_vault;

    std::string child_root_str = child_root.lexically_normal().string();
    std::string vault_str = vault.lexically_normal().string();

    if (vault_str == child_root_str) return "vaults";
    if (vault_str.size() > child_root_str.size() &&
        vault_str.compare(0, child_root_str.size(), child_root_str) == 0 &&
        vault_str[child_root_str.size()] == '/')
        return "vaults/" + vault.lexically_relative(child_root).string();

    return collapse_home(vault_str);
}

static std::string vault_display_name(const AppConfig& config, const std::string& vault_path)
{
    return vault_display_name_for_paths(config.library_root, vault_path);
}

static bool configure_library_root(AppConfig& config, const std::string& suggested_path)
{
    std::string path = prompt_path_screen(
        "Grimoire Library",
        {"Choose where the parent Grimoire library should live.",
         "This root stores shared metadata and a vaults/ directory for child vaults."},
        suggested_path);
    if (path.empty()) return false;

    path = normalize_path(path);
    try
    {
        ensure_library_dirs(path);
    }
    catch (...)
    {
        draw_centered_message("Create Failed",
                              {"Grimoire could not create the library at that location."},
                              "[Any key] back");
        getch();
        return false;
    }

    config.set_library_root(path);
    return true;
}

static bool configure_existing_vault(AppConfig& config, const std::string& suggested_path)
{
    std::string path = prompt_path_screen(
        "Existing Vault",
        {"Enter the path to an existing child vault.",
         "If the vault is missing decks/, Grimoire will create the standard folders there."},
        suggested_path);
    if (path.empty()) return false;

    path = normalize_path(path);
    if (!fs::exists(path) || !fs::is_directory(path))
    {
        draw_centered_message("Invalid Path",
                              {"That path does not exist or is not a directory."},
                              "[Any key] back");
        getch();
        return false;
    }

    ensure_vault_dirs(path);
    config.set_current_vault(path);
    if (!config.save())
    {
        draw_centered_message("Config Error",
                              {"Grimoire could not save its configuration file."},
                              "[Any key] back");
        getch();
        return false;
    }
    config.apply();
    return true;
}

static bool create_new_vault(AppConfig& config, const std::string& suggested_name)
{
    std::string vault_name = trim(prompt_text_screen(
        "New Vault",
        {"Enter a name for the new child vault.",
         "Grimoire will create it under the parent library's vaults/ directory."},
        "Name", suggested_name));
    if (vault_name.empty()) return false;

    fs::path path = fs::path(default_child_vault_root(config)) / vault_name;
    path = path.lexically_normal();

    try
    {
        ensure_library_dirs(config.library_root);
        ensure_vault_dirs(path.string());
    }
    catch (...)
    {
        draw_centered_message("Create Failed",
                              {"Grimoire could not create the vault at that location."},
                              "[Any key] back");
        getch();
        return false;
    }

    config.set_current_vault(path.string());
    if (!config.save())
    {
        draw_centered_message("Config Error",
                              {"Grimoire could not save its configuration file."},
                              "[Any key] back");
        getch();
        return false;
    }
    config.apply();
    return true;
}

static bool run_first_time_setup(AppConfig& config)
{
    while (true)
    {
        draw_centered_message(
            "Welcome To Grimoire",
            {"Choose where your parent Grimoire library should live.",
             "The library stores shared metadata and keeps child vaults in vaults/.",
             "Each child vault stays self-contained with decks/, notes/, and media/."},
            "[s] set library  [q] quit");

        int ch = getch();
        if (ch == 'q' || ch == 27) return false;
        if (ch != 's') continue;

        if (!configure_library_root(config, expand_home(DEFAULT_LIBRARY_ROOT))) continue;

        while (true)
        {
            draw_centered_message(
                "Choose First Vault",
                {"Create a new child vault under the library, or register an existing vault path.",
                 "Grimoire only loads one child vault at a time."},
                "[e] existing vault  [n] new vault  [q] back");

            int vault_ch = getch();
            if (vault_ch == 'q' || vault_ch == 27) break;
            if (vault_ch == 'e' &&
                configure_existing_vault(config, default_child_vault_root(config)))
                return true;
            if (vault_ch == 'n' && create_new_vault(config, "study")) return true;
        }
    }
}

static bool manage_vaults(AppConfig& config)
{
    int selected = 0;
    int scroll = 0;

    while (true)
    {
        config.ensure_consistency();
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        clear();

        attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
        std::string title = "Vaults";
        mvprintw(0, (max_x - (int)title.size()) / 2, "%s", title.c_str());
        attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);

        draw_hline_full(1, 0, max_x);

        std::vector<std::string> lines = config.known_vaults;
        int visible = std::max(1, max_y - 6);
        if (selected >= (int)lines.size()) selected = std::max(0, (int)lines.size() - 1);
        if (selected < scroll) scroll = selected;
        if (selected >= scroll + visible) scroll = selected - visible + 1;

        int y = 3;
        if (lines.empty())
        {
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(y, 3, "No configured vaults yet.");
            attroff(COLOR_PAIR(CLR_DIM));
        }
        else
        {
            for (int i = 0; i < visible && (i + scroll) < (int)lines.size(); i++)
            {
                int idx = i + scroll;
                std::string display = vault_display_name(config, lines[idx]);
                if (lines[idx] == config.current_vault) display += " [current]";

                if (idx == selected)
                {
                    attron(COLOR_PAIR(CLR_HIGHLIGHT));
                    mvprintw(y + i, 3, "%-*s", max_x - 6, display.c_str());
                    attroff(COLOR_PAIR(CLR_HIGHLIGHT));
                }
                else
                {
                    mvprintw(y + i, 3, "%s", display.c_str());
                }
            }
        }

        draw_hline_full(max_y - 2, 0, max_x);
        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(max_y - 1, 1,
                 "[j/k] navigate  [Enter] switch  [a] add existing  [n] new vault  [q] back");
        attroff(COLOR_PAIR(CLR_DIM));
        attron(COLOR_PAIR(CLR_DIM));
        std::string library_line = "Library: " + collapse_home(config.library_root);
        mvprintw(2, 3, "%s", library_line.c_str());
        attroff(COLOR_PAIR(CLR_DIM));
        refresh();

        int ch = getch();
        if (ch == 'q' || ch == 27) return false;
        if ((ch == 'j' || ch == KEY_DOWN) && selected < (int)lines.size() - 1) selected++;
        if ((ch == 'k' || ch == KEY_UP) && selected > 0) selected--;

        if (ch == '\n' || ch == KEY_ENTER)
        {
            if (!lines.empty())
            {
                config.set_current_vault(lines[selected]);
                if (config.save())
                {
                    config.apply();
                    return true;
                }
                show_blocking_message("Config Error",
                                      {"Grimoire could not save the selected vault."});
            }
        }

        if (ch == 'a')
        {
            std::string suggested = !config.current_vault.empty() ? config.current_vault
                                                                  : default_child_vault_root(config);
            if (configure_existing_vault(config, suggested)) return true;
        }

        if (ch == 'n')
        {
            if (create_new_vault(config, "study")) return true;
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

    if (!g_ai_model.empty()) return g_ai_model;

    std::string model = get_loaded_model();
    if (!model.empty())
    {
        g_ai_model = model;
        return g_ai_model;
    }

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

    g_ai_model = model;
    return g_ai_model;
}

static std::string choose_ai_model()
{
    std::string model = pick_model();
    if (model.empty()) return "";

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    clear();
    attron(COLOR_PAIR(CLR_DIM));
    char buf[128];
    snprintf(buf, sizeof(buf), "Loading %s...", model.c_str());
    mvprintw(max_y / 2, (max_x - (int)strlen(buf)) / 2, "%s", buf);
    attroff(COLOR_PAIR(CLR_DIM));
    refresh();

    load_model(model);
    g_ai_model = model;
    return g_ai_model;
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
        mvprintw(max_y - 1, 1, "[Enter] ask  [m] model  [j/k] scroll  [q/Esc] back");
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
        if (ch == 'm')
        {
            std::string next_model = choose_ai_model();
            if (!next_model.empty()) model = next_model;
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

enum class TypedAnswerAction
{
    Cancel,
    Correct,
    Wrong
};

static TypedAnswerAction prompt_and_judge_typed_answer(const std::string& model, const Card& card,
                                                       const std::string& deck, int input_y,
                                                       int input_x, int input_w, int result_rows,
                                                       int box_y)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    input_w = std::max(20, std::min(input_w, max_x - input_x - 1));
    result_rows = std::max(7, result_rows);
    input_y = std::max(1, std::min(input_y, max_y - result_rows - 1));

    attron(COLOR_PAIR(CLR_DIM));
    mvhline(input_y, input_x, ' ', input_w);
    mvprintw(input_y, input_x, "Type your answer:");
    attroff(COLOR_PAIR(CLR_DIM));
    TextInputResult typed =
        get_wrapped_input_result(input_y + 1, input_x, input_w, std::max(1, result_rows - 2));
    if (typed.cancelled || trim(typed.value).empty()) return TypedAnswerAction::Cancel;

    getmaxyx(stdscr, max_y, max_x);
    attron(COLOR_PAIR(CLR_DIM));
    for (int i = 0; i < result_rows && input_y + i < max_y - 1; i++)
        mvhline(input_y + i, input_x, ' ', input_w);
    mvprintw(input_y, input_x, "Checking answer...");
    attroff(COLOR_PAIR(CLR_DIM));
    refresh();

    TypedAnswerJudgement judgement = judge_typed_answer(model, card, deck, typed.value);
    bool suggested_correct = judgement.correct;

    getmaxyx(stdscr, max_y, max_x);
    int title_color = judgement.valid ? (suggested_correct ? CLR_CORRECT : CLR_WRONG) : CLR_WRONG;
    std::string status = judgement.valid ? (suggested_correct ? "[y] Pass" : "[n] Fail")
                                         : "[!] AI check failed";

    std::vector<StyledLine> result_lines;
    result_lines.push_back({{status, title_color, true}});
    // Render each section through format_display_text so fenced code in the answer is parsed
    // and syntax-highlighted instead of dumped as raw backticks. Body lines are indented 2.
    auto append_section = [&](const std::string& label, const std::string& text, int color)
    {
        result_lines.push_back({{label, color, false}});
        auto styled = format_display_text(text, std::max(1, input_w - 4), color, false);
        for (auto& sl : styled)
        {
            StyledLine indented;
            indented.push_back({"  ", color, false});
            for (auto& sp : sl) indented.push_back(sp);
            result_lines.push_back(indented);
        }
    };

    std::string feedback = judgement.feedback.empty() ? "No feedback returned." : judgement.feedback;
    append_section("You:", typed.value, CLR_DEFAULT);
    append_section("AI:", feedback, title_color);
    append_section("Answer:", card.answer, CLR_DIM);

    // Size the result card to its content: extend the box down to fit, and only scroll once the
    // content would run past the bottom of the screen.
    getmaxyx(stdscr, max_y, max_x);
    int box_x_full = input_x - 2;
    int box_w_full = input_w + 4;
    int avail_rows = std::max(1, (max_y - 3) - input_y);
    int visible_rows = std::min((int)result_lines.size(), avail_rows);
    int box_bottom = input_y + visible_rows; // row of the box's bottom border

    // Wipe from the separator down, then redraw the (possibly taller/shorter) box once. The
    // question and header above input_y - 1 are left intact.
    for (int yy = input_y - 1; yy < max_y; yy++)
        mvhline(yy, 0, ' ', max_x);
    draw_box(box_y, box_x_full, box_bottom - box_y + 1, box_w_full);
    attron(COLOR_PAIR(CLR_BORDER));
    mvhline(input_y - 1, box_x_full + 1, ACS_HLINE, box_w_full - 2);
    attroff(COLOR_PAIR(CLR_BORDER));

    int scroll = 0;
    int max_scroll = std::max(0, (int)result_lines.size() - visible_rows);
    timeout(-1);
    while (true)
    {
        scroll = std::max(0, std::min(scroll, max_scroll));

        for (int i = 0; i < visible_rows; i++)
            mvhline(input_y + i, input_x, ' ', input_w);

        int draw_y = input_y;
        for (int i = 0; i < visible_rows && i + scroll < (int)result_lines.size(); i++)
        {
            StyledLine line = result_lines[i + scroll];
            draw_styled_lines({line}, draw_y, input_x, input_y + visible_rows, input_w);
        }

        attron(COLOR_PAIR(CLR_DIM));
        mvhline(max_y - 2, 1, ' ', std::max(1, max_x - 2));
        std::string hint;
        if (max_scroll > 0)
        {
            int shown = std::min((int)result_lines.size(), scroll + visible_rows);
            char sc[72];
            snprintf(sc, sizeof(sc), "[Enter] Next  [j/k] Scroll  (%d/%d)", shown,
                     (int)result_lines.size());
            hint = sc;
        }
        else
            hint = judgement.valid ? "[Any key] Next" : "[Any key] Back without marking";
        mvaddnstr(max_y - 2, 1, hint.c_str(), std::max(1, max_x - 2));
        attroff(COLOR_PAIR(CLR_DIM));

        refresh();
        int ch = getch();
        if (max_scroll > 0 && (ch == 'j' || ch == KEY_DOWN))
        {
            if (scroll < max_scroll) scroll++;
            continue;
        }
        if (max_scroll > 0 && (ch == 'k' || ch == KEY_UP))
        {
            if (scroll > 0) scroll--;
            continue;
        }
        break;
    }
    if (!judgement.valid) return TypedAnswerAction::Cancel;
    return suggested_correct ? TypedAnswerAction::Correct : TypedAnswerAction::Wrong;
}

// Startup splash screen — random logo variant
// Returns 'c' to continue paused session, or anything else to browse
static int show_splash(int due_count)
{
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, LOGO_COUNT - 1);
    auto& logo = LOGOS[dist(rng)];

    auto saved = DrillSession::load_session();
    SplashSummary summary = load_splash_summary();
    bool has_session =
        saved.contains("deck_name") && saved.value("vault_root", "") == g_vault_root;

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

    std::string library_label = "Library: " + collapse_home(g_library_root);
    attron(COLOR_PAIR(CLR_DIM));
    mvprintw(hint_y, (max_x - (int)library_label.size()) / 2, "%s", library_label.c_str());
    attroff(COLOR_PAIR(CLR_DIM));
    hint_y += 2;

    std::string vault_label = "Vault: " + vault_display_name_for_paths(g_library_root, g_vault_root);
    std::string decks_line = "Decks: " + std::to_string(summary.current_vault_decks);
    std::string completed_line = "Completed: " + std::to_string(summary.sessions_completed);
    std::string streak_line = "Streak: " + std::to_string(summary.current_streak) + "d";
    std::string consistency_line =
        "Consistency: " + std::to_string((int)(summary.consistency_rating + 0.5)) + "%";
    std::string focus = summary.improvement_deck_id.empty() ? "Focus: none yet"
                                                            : "Focus: " + summary.improvement_deck_id;
    int max_inner_w = std::max(28, max_x - 10);
    if ((int)focus.size() > max_inner_w - 4) focus = focus.substr(0, max_inner_w - 7) + "...";

    int card_inner_w = std::max((int)vault_label.size(), (int)decks_line.size());
    card_inner_w = std::max(card_inner_w, (int)completed_line.size());
    card_inner_w = std::max(card_inner_w, (int)streak_line.size());
    card_inner_w = std::max(card_inner_w, (int)consistency_line.size());
    card_inner_w = std::max(card_inner_w, (int)focus.size());
    card_inner_w = std::min(card_inner_w, max_inner_w - 2);
    int card_w = card_inner_w + 4;
    int card_h = 8;
    int card_x = (max_x - card_w) / 2;
    int card_y = hint_y;

    draw_box(card_y, card_x, card_h, card_w);

    attron(COLOR_PAIR(CLR_DIM));
    mvprintw(card_y + 1, card_x + (card_w - (int)vault_label.size()) / 2, "%s", vault_label.c_str());
    mvprintw(card_y + 2, card_x + 2, "%s", decks_line.c_str());
    mvprintw(card_y + 3, card_x + 2, "%s", completed_line.c_str());
    mvprintw(card_y + 4, card_x + 2, "%s", streak_line.c_str());
    mvprintw(card_y + 5, card_x + 2, "%s", consistency_line.c_str());
    attroff(COLOR_PAIR(CLR_DIM));
    attron(COLOR_PAIR(CLR_HEADER));
    mvprintw(card_y + 6, card_x + 2, "%s", focus.c_str());
    attroff(COLOR_PAIR(CLR_HEADER));
    hint_y = card_y + card_h + 1;

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

    if (due_count > 0)
    {
        char rev[64];
        snprintf(rev, sizeof(rev), "[r] Drill Review (%d)", due_count);
        attron(COLOR_PAIR(CLR_HEADER) | A_BOLD);
        mvprintw(hint_y, (max_x - (int)strlen(rev)) / 2, "%s", rev);
        attroff(COLOR_PAIR(CLR_HEADER) | A_BOLD);
        hint_y += 2;
    }

    std::string hint = "[Enter] Browse decks  [v] Vaults  [q] Quit";
    attron(COLOR_PAIR(CLR_DIM));
    mvprintw(hint_y, (max_x - (int)hint.size()) / 2, "%s", hint.c_str());
    attroff(COLOR_PAIR(CLR_DIM));

    refresh();

    while (true)
    {
        int ch = getch();
        if (ch == 'q' || ch == 27) return 'q';
        if (ch == 'v') return 'v';
        if (ch == 'r' && due_count > 0) return 'r';
        if (ch == 'c' && has_session) return 'c';
        if (ch == '\n' || ch == ' ' || !has_session) return '\n';
    }
}

// Yazi-style 3-column file browser
// Columns: parent | current | child/preview
// Navigate with h/l to go up/down directory levels, j/k to move within a listing.

static std::string browse_decks(const std::string& root, const Progress& progress)
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
            std::string did =
                preview_deck.id.empty() ? deck_id_from_path(path, root) : preview_deck.id;
            int rate = progress.deck_correct_rate(did, preview_deck.cards);
            std::string last = progress.last_completed_date(did);

            int y = list_start;
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(y++, px + 1, "%d card%s", n, n == 1 ? "" : "s");

            char rate_s[48];
            if (rate >= 0)
                snprintf(rate_s, sizeof(rate_s), "Correct: %d%%", rate);
            else
                snprintf(rate_s, sizeof(rate_s), "Correct: --");
            attroff(COLOR_PAIR(CLR_DIM));
            attron(COLOR_PAIR(rate < 0 ? CLR_DIM : (rate >= 70 ? CLR_CORRECT : CLR_WRONG)));
            mvprintw(y++, px + 1, "%s", rate_s);
            attroff(COLOR_PAIR(rate < 0 ? CLR_DIM : (rate >= 70 ? CLR_CORRECT : CLR_WRONG)));

            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(y++, px + 1, "Last done: %s", last.empty() ? "never" : last.c_str());

            int preview_y = y + 1;
            for (int i = 0; i < std::min(n, list_h - (preview_y - list_start)); i++)
            {
                std::string q = preview_deck.cards[i].question;
                std::replace(q.begin(), q.end(), '\n', ' ');
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

// Returns 'c' to continue, 'n' to start fresh, or 'q' to go back.
static int choose_saved_deck_session(const std::string& deck_name, const json& saved)
{
    int remaining = 0;
    if (saved.contains("round") && saved["round"].is_array())
        remaining += (int)saved["round"].size();
    if (saved.contains("missed") && saved["missed"].is_array())
        remaining += (int)saved["missed"].size();

    char state[128];
    snprintf(state, sizeof(state), "Saved drill: round %d, %d cards left",
             saved.value("round_num", 1), remaining);

    std::vector<std::pair<std::string, int>> options = {
        {"Continue saved session", 'c'},
        {"Start new session", 'n'},
        {"Back to decks", 'q'},
    };
    int selected = 0;

    timeout(-1);
    while (true)
    {
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        clear();

        int box_w = std::min(std::max(44, (int)deck_name.size() + 12), max_x - 4);
        int box_h = 11;
        int box_x = (max_x - box_w) / 2;
        int box_y = (max_y - box_h) / 2;
        draw_box(box_y, box_x, box_h, box_w);

        std::string title = "Resume Deck?";
        attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
        mvprintw(box_y + 1, box_x + (box_w - (int)title.size()) / 2, "%s", title.c_str());
        attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);

        std::string deck_line = deck_name;
        if ((int)deck_line.size() > box_w - 4) deck_line = deck_line.substr(0, box_w - 7) + "...";
        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(box_y + 3, box_x + 2, "%s", deck_line.c_str());
        mvprintw(box_y + 4, box_x + 2, "%s", state);
        attroff(COLOR_PAIR(CLR_DIM));

        for (int i = 0; i < (int)options.size(); i++)
        {
            int row = box_y + 6 + i;
            if (i == selected)
            {
                attron(COLOR_PAIR(CLR_HIGHLIGHT));
                mvprintw(row, box_x + 2, "%-*s", box_w - 4, options[i].first.c_str());
                attroff(COLOR_PAIR(CLR_HIGHLIGHT));
            }
            else
            {
                mvprintw(row, box_x + 2, "%s", options[i].first.c_str());
            }
        }

        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(max_y - 1, 1, "[j/k] Navigate  [Enter] Select  [Esc] Back");
        attroff(COLOR_PAIR(CLR_DIM));
        refresh();

        int ch = getch();
        if (ch == 'j' || ch == KEY_DOWN)
        {
            if (selected < (int)options.size() - 1) selected++;
            continue;
        }
        if (ch == 'k' || ch == KEY_UP)
        {
            if (selected > 0) selected--;
            continue;
        }
        if (ch == '\n' || ch == KEY_ENTER || ch == ' ') return options[selected].second;
        if (ch == 'c' || ch == 'C') return 'c';
        if (ch == 'n' || ch == 'N') return 'n';
        if (ch == 'q' || ch == 27) return 'q';
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

static void draw_drill_header(const DrillSession& session, time_t session_start, int queue_left,
                              int queued_count)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int mastered = session.mastered_count();
    std::string elapsed = format_elapsed(session_start);
    attron(COLOR_PAIR(CLR_HEADER));
    mvprintw(0, 1, "%s", elapsed.c_str());
    mvprintw(0, (max_x - (int)session.deck_name.size()) / 2, "%s", session.deck_name.c_str());
    attroff(COLOR_PAIR(CLR_HEADER));

    char mastered_str[64];
    snprintf(mastered_str, sizeof(mastered_str), "%d/%d", mastered, (int)session.cards.size());
    attron(COLOR_PAIR(CLR_HEADER));
    mvprintw(0, max_x - (int)strlen(mastered_str) - 1, "%s", mastered_str);
    attroff(COLOR_PAIR(CLR_HEADER));

    attron(COLOR_PAIR(CLR_HEADER));
    mvprintw(1, 1, "[drilling]");
    attroff(COLOR_PAIR(CLR_HEADER));

    char queue_str[128];
    snprintf(queue_str, sizeof(queue_str), "%d left | %d queued", queue_left, queued_count);
    attron(COLOR_PAIR(CLR_DIM));
    mvprintw(1, max_x - (int)strlen(queue_str) - 1, "%s", queue_str);
    attroff(COLOR_PAIR(CLR_DIM));

    draw_hline_full(2, 0, max_x);

    char round_str[64];
    snprintf(round_str, sizeof(round_str), "Round %d", session.round_num);
    attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
    mvprintw(3, (max_x - (int)strlen(round_str)) / 2, "%s", round_str);
    attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);

    draw_hline_full(4, 0, max_x);
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
                // All mastered - session summary screen
                int elapsed = (int)difftime(time(nullptr), session_start);
                int max_y, max_x;
                getmaxyx(stdscr, max_y, max_x);
                clear();

                // Centered box
                int box_w = 46;
                int box_h = 13;
                int box_x = (max_x - box_w) / 2;
                int box_y = (max_y - box_h) / 2;
                draw_box(box_y, box_x, box_h, box_w);

                attron(COLOR_PAIR(CLR_CORRECT) | A_BOLD);
                std::string msg = "SESSION COMPLETE";
                mvprintw(box_y + 2, box_x + (box_w - (int)msg.size()) / 2, "%s", msg.c_str());
                attroff(COLOR_PAIR(CLR_CORRECT) | A_BOLD);

                int total_answers = session.session_right + session.session_wrong;
                int accuracy = total_answers > 0 ? (session.session_right * 100) / total_answers : 0;
                char l_cards[48], l_rounds[48], l_answers[48], l_time[48];
                snprintf(l_cards, sizeof(l_cards), "Cards mastered : %d", (int)session.cards.size());
                snprintf(l_rounds, sizeof(l_rounds), "Rounds         : %d", session.round_num);
                snprintf(l_answers, sizeof(l_answers), "Answers        : %d right  %d wrong (%d%%)",
                         session.session_right, session.session_wrong, accuracy);
                snprintf(l_time, sizeof(l_time), "Time           : %s",
                         format_elapsed(session_start).c_str());
                int lx = box_x + 3;
                attron(COLOR_PAIR(CLR_DIM));
                mvprintw(box_y + 4, lx, "%s", l_cards);
                mvprintw(box_y + 5, lx, "%s", l_rounds);
                mvprintw(box_y + 6, lx, "%s", l_answers);
                mvprintw(box_y + 7, lx, "%s", l_time);

                std::string hint = "[Press any key]";
                mvprintw(box_y + 9, box_x + (box_w - (int)hint.size()) / 2, "%s", hint.c_str());
                attroff(COLOR_PAIR(CLR_DIM));

                refresh();
                timeout(-1); // block for final screen
                getch();
                session.progress->record_session(g_vault_root, session.deck_id, deck_path,
                                                (int)session.cards.size(), elapsed, true);
                session.progress->save();
                DrillSession::clear_session(session.deck_id);
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
        bool card_handled = false;

        // --- Show question ---
        while (true)
        {
            int max_y, max_x;
            getmaxyx(stdscr, max_y, max_x);
            clear();
            int mastered = session.mastered_count();
            draw_drill_header(session, session_start, (int)session.round.size() + 1,
                              (int)session.missed.size());

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
            mvprintw(max_y - 2, 1,
                     "[t] Type Answer  [Space] Show Answer  [n] Note  [N] Set Note  [a] AI  [q] Quit");
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
                session.progress->record_session(g_vault_root, session.deck_id, deck_path,
                                                (int)session.cards.size(), elapsed, false);
                session.progress->save();
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
            if (ch == 't')
            {
                std::string model = ensure_ollama_ready();
                if (model.empty())
                {
                    timeout(1000);
                    continue;
                }
                int result_rows = 14;
                int max_typed_card_h = std::max(16, max_y - 6);
                int typed_card_h =
                    std::min(std::max((int)q_lines.size() + result_rows + 7, 16),
                             max_typed_card_h);
                result_rows = std::max(7, std::min(result_rows, typed_card_h - 7));
                int typed_box_y = std::max(6, (max_y - typed_card_h) / 2);
                clear();
                draw_drill_header(session, session_start, (int)session.round.size() + 1,
                                  (int)session.missed.size());
                draw_box(typed_box_y, box_x, typed_card_h, content_w);

                int typed_inner_x = box_x + 2;
                int typed_y = typed_box_y + 1;
                attron(COLOR_PAIR(stage_color(stage)));
                mvprintw(typed_y, typed_inner_x, "[%s]", stage_label(stage));
                attroff(COLOR_PAIR(stage_color(stage)));
                attron(COLOR_PAIR(CLR_DIM));
                mvprintw(typed_y, box_x + content_w - 2 - (int)strlen(streak_str), "%s",
                         streak_str);
                attroff(COLOR_PAIR(CLR_DIM));
                typed_y += 2;
                int input_y = typed_box_y + typed_card_h - result_rows - 1;
                draw_styled_lines(q_lines, typed_y, typed_inner_x, input_y - 1, content_w - 4);
                attron(COLOR_PAIR(CLR_BORDER));
                mvhline(input_y - 1, box_x + 1, ACS_HLINE, content_w - 2);
                attroff(COLOR_PAIR(CLR_BORDER));
                attron(COLOR_PAIR(CLR_DIM));
                mvhline(max_y - 2, 1, ' ', std::max(1, max_x - 2));
                attroff(COLOR_PAIR(CLR_DIM));
                int total_cards_t = (int)session.cards.size();
                if (total_cards_t > 0)
                {
                    int n = total_cards_t;
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

                TypedAnswerAction action =
                    prompt_and_judge_typed_answer(model, card, session.deck_name, input_y,
                                                  typed_inner_x, content_w - 4, result_rows,
                                                  typed_box_y);
                timeout(1000);
                if (action == TypedAnswerAction::Correct)
                {
                    session.mark_correct(card_idx);
                    card_handled = true;
                    break;
                }
                if (action == TypedAnswerAction::Wrong)
                {
                    session.mark_wrong(card_idx);
                    card_handled = true;
                    break;
                }
                continue;
            }
            if (ch == 'n')
            {
                open_note_ref(card, session.deck_name, session.round_num, session_start,
                              (int)session.round.size() + 1, (int)session.missed.size(),
                              mastered, (int)session.cards.size(), stage, streak, card_target);
                timeout(1000);
                continue;
            }
            if (ch == 'N')
            {
                assign_note_ref_for_card(deck_path, session.cards, card_idx);
                timeout(1000);
                continue;
            }
            if (ch == ' ') break;
        }
        if (card_handled) continue;

        // --- Show answer ---
        while (true)
        {
            int max_y, max_x;
            getmaxyx(stdscr, max_y, max_x);
            clear();
            int mastered = session.mastered_count();
            draw_drill_header(session, session_start, (int)session.round.size(),
                              (int)session.missed.size());

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
            mvprintw(max_y - 2, 1, "[N] Set Note  [a] Ask AI  [q] Quit");
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
                session.progress->record_session(g_vault_root, session.deck_id, deck_path,
                                                (int)session.cards.size(), elapsed, false);
                session.progress->save();
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
            if (ch == 'N')
            {
                assign_note_ref_for_card(deck_path, session.cards, card_idx);
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

// --- Spaced repetition review ---

struct ReviewItem
{
    std::string deck_id;
    std::string deck_path;
    std::string card_key; // stable card id (may be empty -> index-keyed)
    int card_idx = 0;
    Card card;
    bool is_new = false;
    int due_day = 0;
};

// Scan every deck in the active vault and collect cards that are due today (or earlier),
// plus up to new_limit never-scheduled "new" cards. Due cards come first, oldest-due first.
static std::vector<ReviewItem> build_review_queue(const Progress& progress, int new_limit = 20)
{
    std::vector<ReviewItem> due_items;
    std::vector<ReviewItem> new_items;
    std::string deck_root = normalize_path(expand_home(g_deck_dir));
    int today = iso_date_to_day_number(iso_date());

    for (const auto& deck_path : list_deck_files_recursive(deck_root))
    {
        Deck deck = parse_deck(deck_path);
        if (deck.cards.empty()) continue;
        std::string deck_id = deck.id.empty() ? deck_id_from_path(deck_path, deck_root) : deck.id;
        for (size_t i = 0; i < deck.cards.size(); i++)
        {
            ReviewItem item;
            item.deck_id = deck_id;
            item.deck_path = deck_path;
            item.card_key = deck.cards[i].id;
            item.card_idx = (int)i;
            item.card = deck.cards[i];

            std::string key = progress.sched_key(deck_id, deck.cards[i].id, (int)i);
            json entry = progress.get_schedule(key);
            if (entry.is_null())
            {
                item.is_new = true;
                item.due_day = today;
                new_items.push_back(item);
            }
            else
            {
                int dd = iso_date_to_day_number(entry.value("due", ""));
                if (dd <= today)
                {
                    item.is_new = false;
                    item.due_day = dd;
                    due_items.push_back(item);
                }
            }
        }
    }

    std::sort(due_items.begin(), due_items.end(),
              [](const ReviewItem& a, const ReviewItem& b) { return a.due_day < b.due_day; });
    if (new_limit >= 0 && (int)new_items.size() > new_limit) new_items.resize(new_limit);

    std::vector<ReviewItem> queue = std::move(due_items);
    for (auto& it : new_items) queue.push_back(it);
    return queue;
}

// Difficulty weight for the drill-review pool. Error rate (Laplace-smoothed so a fresh card
// is ~0.5, never a divide-by-zero) scaled by recency: a card answered today is damped
// (cooldown), and one you keep missing but haven't seen in a while floats to the top. As a
// card's right count climbs its error rate falls, so it decays out of the pool on its own.
static double drill_review_weight(const json& stat, int today)
{
    int right = stat.value("right", 0);
    int wrong = stat.value("wrong", 0);
    double error_rate = (wrong + 1.0) / (right + wrong + 2.0);
    const double target_gap = 3.0; // days until a card is fully "cooled down"
    double recency = 1.0;
    if (stat.contains("last_seen"))
    {
        int last = iso_date_to_day_number(stat.value("last_seen", ""));
        int gap = today - last;
        if (gap < 0) gap = 0;
        recency = std::min(1.0, (gap + 1.0) / (target_gap + 1.0));
    }
    return error_rate * recency;
}

// Build the drill-review pool: cards from decks completed at least once, ranked by difficulty
// weight (weakest first). Returns the top `limit` cards.
static std::vector<ReviewItem> build_drill_review_pool(const Progress& progress, int limit = 20)
{
    std::vector<std::pair<double, ReviewItem>> scored;
    // Match the deck_id keying used when drilling writes card_stats (non-normalized root).
    std::string deck_root = expand_home(g_deck_dir);
    int today = iso_date_to_day_number(iso_date());

    for (const auto& deck_path : list_deck_files_recursive(deck_root))
    {
        Deck deck = parse_deck(deck_path);
        if (deck.cards.empty()) continue;
        std::string deck_id = deck.id.empty() ? deck_id_from_path(deck_path, deck_root) : deck.id;
        if (!progress.deck_completed(deck_id)) continue;
        for (size_t i = 0; i < deck.cards.size(); i++)
        {
            ReviewItem item;
            item.deck_id = deck_id;
            item.deck_path = deck_path;
            item.card_key = deck.cards[i].id;
            item.card_idx = (int)i;
            item.card = deck.cards[i];

            std::string key = progress.sched_key(deck_id, deck.cards[i].id, (int)i);
            json stat = progress.get_card_stat(key);
            if (!stat.is_object()) stat = json::object();
            item.is_new = !stat.contains("right") && !stat.contains("wrong");
            scored.push_back({drill_review_weight(stat, today), item});
        }
    }

    std::sort(scored.begin(), scored.end(),
              [](const std::pair<double, ReviewItem>& a, const std::pair<double, ReviewItem>& b)
              { return a.first > b.first; });
    std::vector<ReviewItem> pool;
    for (auto& s : scored)
    {
        if ((int)pool.size() >= limit) break;
        pool.push_back(s.second);
    }
    return pool;
}

static void draw_review_header(const ReviewItem& item, int idx, int total, int reviewed,
                               int again_count, time_t session_start)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
    mvprintw(1, 2, "REVIEW");
    attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);

    std::string dname = item.deck_id;
    auto slash = dname.rfind('/');
    if (slash != std::string::npos) dname = dname.substr(slash + 1);
    attron(COLOR_PAIR(CLR_DIM));
    mvprintw(1, 12, "%s", dname.c_str());
    attroff(COLOR_PAIR(CLR_DIM));

    char stat[96];
    snprintf(stat, sizeof(stat), "card %d/%d  again %d  %s", idx + 1, total, again_count,
             format_elapsed(session_start).c_str());
    attron(COLOR_PAIR(CLR_DIM));
    mvprintw(1, max_x - (int)strlen(stat) - 2, "%s", stat);
    attroff(COLOR_PAIR(CLR_DIM));

    int filled = total > 0 ? (reviewed * max_x) / total : 0;
    move(2, 0);
    for (int i = 0; i < max_x; i++)
    {
        bool on = i < filled;
        attron(COLOR_PAIR(on ? CLR_CORRECT : CLR_DIM));
        addch(ACS_HLINE);
        attroff(COLOR_PAIR(on ? CLR_CORRECT : CLR_DIM));
    }
}

// Cross-deck spaced-repetition review. Each due card is shown once, graded Again/Good/Easy,
// rescheduled via SM-2, and persisted immediately so quitting mid-review keeps progress.
static void run_review(std::vector<ReviewItem>& items, Progress& progress)
{
    if (items.empty()) return;
    time_t session_start = time(nullptr);
    int total = (int)items.size();
    int reviewed = 0;
    int again_count = 0;
    int today = iso_date_to_day_number(iso_date());
    timeout(1000); // refresh elapsed timer

    for (int idx = 0; idx < total; idx++)
    {
        ReviewItem& item = items[idx];
        std::string key = progress.sched_key(item.deck_id, item.card_key, item.card_idx);
        json entry = progress.get_schedule(key);
        bool quit = false;
        int typed_grade = -1;

        // --- Question phase ---
        bool reveal = false;
        while (!reveal && !quit)
        {
            int max_y, max_x;
            getmaxyx(stdscr, max_y, max_x);
            clear();
            draw_review_header(item, idx, total, reviewed, again_count, session_start);

            int content_w = std::min(max_x - 6, 60);
            auto q_lines = format_display_text(item.card.question, content_w - 4, CLR_DEFAULT, false);
            int card_h = std::min((int)q_lines.size() + 6, std::max(6, max_y - 9));
            int box_x = (max_x - content_w) / 2;
            int box_y = std::max(5, (max_y - card_h) / 2);
            draw_box(box_y, box_x, card_h, content_w);

            int inner_x = box_x + 2;
            int y = box_y + 1;
            attron(COLOR_PAIR(item.is_new ? CLR_HEADER : CLR_DIM));
            mvprintw(y, inner_x, "%s", item.is_new ? "[new]" : "[review]");
            attroff(COLOR_PAIR(item.is_new ? CLR_HEADER : CLR_DIM));
            y += 2;
            draw_styled_lines(q_lines, y, inner_x, box_y + card_h - 1, content_w - 4);

            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(max_y - 2, 1, "[t] Type Answer  [Space] Show Answer  [a] Ask AI  [q] Quit");
            attroff(COLOR_PAIR(CLR_DIM));
            refresh();

            int ch = getch();
            if (ch == ERR) continue;
            if (ch == 'q' || ch == 27) { quit = true; break; }
            if (ch == 'a') { show_ai_assistant(item.card, item.deck_id); timeout(1000); continue; }
            if (ch == 't')
            {
                std::string model = ensure_ollama_ready();
                if (model.empty())
                {
                    timeout(1000);
                    continue;
                }
                int result_rows = 14;
                int max_typed_card_h = std::max(16, max_y - 6);
                int typed_card_h =
                    std::min(std::max((int)q_lines.size() + result_rows + 7, 16),
                             max_typed_card_h);
                result_rows = std::max(7, std::min(result_rows, typed_card_h - 7));
                int typed_box_y = std::max(5, (max_y - typed_card_h) / 2);
                clear();
                draw_review_header(item, idx, total, reviewed, again_count, session_start);
                draw_box(typed_box_y, box_x, typed_card_h, content_w);

                int typed_inner_x = box_x + 2;
                int typed_y = typed_box_y + 1;
                attron(COLOR_PAIR(item.is_new ? CLR_HEADER : CLR_DIM));
                mvprintw(typed_y, typed_inner_x, "%s", item.is_new ? "[new]" : "[review]");
                attroff(COLOR_PAIR(item.is_new ? CLR_HEADER : CLR_DIM));
                typed_y += 2;
                int input_y = typed_box_y + typed_card_h - result_rows - 1;
                draw_styled_lines(q_lines, typed_y, typed_inner_x, input_y - 1, content_w - 4);
                attron(COLOR_PAIR(CLR_BORDER));
                mvhline(input_y - 1, box_x + 1, ACS_HLINE, content_w - 2);
                attroff(COLOR_PAIR(CLR_BORDER));
                attron(COLOR_PAIR(CLR_DIM));
                mvhline(max_y - 2, 1, ' ', std::max(1, max_x - 2));
                attroff(COLOR_PAIR(CLR_DIM));
                refresh();

                TypedAnswerAction action =
                    prompt_and_judge_typed_answer(model, item.card, item.deck_id, input_y,
                                                  typed_inner_x, content_w - 4, result_rows,
                                                  typed_box_y);
                timeout(1000);
                if (action == TypedAnswerAction::Correct)
                {
                    typed_grade = 1;
                    reveal = true;
                    break;
                }
                if (action == TypedAnswerAction::Wrong)
                {
                    typed_grade = 0;
                    reveal = true;
                    break;
                }
                continue;
            }
            if (ch == ' ') reveal = true;
        }
        if (quit) break;

        if (typed_grade >= 0)
        {
            json updated =
                sr_update(entry.is_null() ? json::object() : entry, typed_grade, today);
            progress.set_schedule(key, updated);
            if (typed_grade == 0)
            {
                int st = progress.get_stage(item.deck_id, item.card_key, item.card_idx);
                progress.set_stage(item.deck_id, item.card_key, item.card_idx, std::max(st - 1, 0));
            }
            else
            {
                int reps = updated.value("reps", 0);
                int st = reps >= 3 ? 2 : (reps >= 1 ? 1 : 0);
                progress.set_stage(item.deck_id, item.card_key, item.card_idx, st);
            }
            progress.save();
            if (typed_grade == 0) again_count++;
            reviewed++;
            continue;
        }

        // --- Answer + grade phase ---
        bool graded = false;
        while (!graded && !quit)
        {
            int max_y, max_x;
            getmaxyx(stdscr, max_y, max_x);
            clear();
            draw_review_header(item, idx, total, reviewed, again_count, session_start);

            int content_w = std::min(max_x - 6, 60);
            auto q_lines = format_display_text(item.card.question, content_w - 4, CLR_DIM, false);
            auto a_lines = format_display_text(item.card.answer, content_w - 4, CLR_DEFAULT, true);
            int card_h = std::min((int)q_lines.size() + (int)a_lines.size() + 8,
                                  std::max(8, max_y - 9));
            int box_x = (max_x - content_w) / 2;
            int box_y = std::max(5, (max_y - card_h) / 2);
            draw_box(box_y, box_x, card_h, content_w);

            int inner_x = box_x + 2;
            int y = box_y + 1;
            attron(COLOR_PAIR(item.is_new ? CLR_HEADER : CLR_DIM));
            mvprintw(y, inner_x, "%s", item.is_new ? "[new]" : "[review]");
            attroff(COLOR_PAIR(item.is_new ? CLR_HEADER : CLR_DIM));
            y += 2;
            draw_styled_lines(q_lines, y, inner_x, box_y + card_h - 1, content_w - 4);
            y++;
            attron(COLOR_PAIR(CLR_BORDER));
            mvhline(y, box_x + 1, ACS_HLINE, content_w - 2);
            attroff(COLOR_PAIR(CLR_BORDER));
            y += 2;
            draw_styled_lines(a_lines, y, inner_x, box_y + card_h - 1, content_w - 4);

            char again_s[24], good_s[24], easy_s[24];
            snprintf(again_s, sizeof(again_s), "[1] Again %dd", sr_preview_interval(entry, 0));
            snprintf(good_s, sizeof(good_s), "[2] Good %dd", sr_preview_interval(entry, 1));
            snprintf(easy_s, sizeof(easy_s), "[3] Easy %dd", sr_preview_interval(entry, 2));
            attron(COLOR_PAIR(CLR_WRONG));
            mvprintw(max_y - 3, 1, "%s", again_s);
            attroff(COLOR_PAIR(CLR_WRONG));
            attron(COLOR_PAIR(CLR_CORRECT));
            mvprintw(max_y - 3, 18, "%s", good_s);
            attroff(COLOR_PAIR(CLR_CORRECT));
            attron(COLOR_PAIR(CLR_HEADER));
            mvprintw(max_y - 3, 34, "%s", easy_s);
            attroff(COLOR_PAIR(CLR_HEADER));
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(max_y - 2, 1, "[a] Ask AI  [q] Quit");
            attroff(COLOR_PAIR(CLR_DIM));
            refresh();

            int ch = getch();
            if (ch == ERR) continue;
            if (ch == 'q' || ch == 27) { quit = true; break; }
            if (ch == 'a') { show_ai_assistant(item.card, item.deck_id); timeout(1000); continue; }
            int grade = -1;
            if (ch == '1') grade = 0;
            else if (ch == '2' || ch == ' ') grade = 1;
            else if (ch == '3') grade = 2;
            if (grade < 0) continue;

            json updated = sr_update(entry.is_null() ? json::object() : entry, grade, today);
            progress.set_schedule(key, updated);
            // Keep the mastery stage loosely in sync so dashboards reflect review work.
            if (grade == 0)
            {
                int st = progress.get_stage(item.deck_id, item.card_key, item.card_idx);
                progress.set_stage(item.deck_id, item.card_key, item.card_idx, std::max(st - 1, 0));
            }
            else
            {
                int reps = updated.value("reps", 0);
                int st = reps >= 3 ? 2 : (reps >= 1 ? 1 : 0);
                progress.set_stage(item.deck_id, item.card_key, item.card_idx, st);
            }
            progress.save();
            if (grade == 0) again_count++;
            reviewed++;
            graded = true;
        }
        if (quit) break;
    }

    timeout(-1);
    bool completed = (reviewed >= total);
    progress.record_session(g_vault_root, "__review__", "", total,
                            (int)difftime(time(nullptr), session_start), completed);
    progress.save();

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    clear();
    int box_w = 44, box_h = 9;
    int box_x = (max_x - box_w) / 2;
    int box_y = (max_y - box_h) / 2;
    draw_box(box_y, box_x, box_h, box_w);
    attron(COLOR_PAIR(CLR_CORRECT) | A_BOLD);
    std::string msg = "REVIEW COMPLETE";
    mvprintw(box_y + 2, box_x + (box_w - (int)msg.size()) / 2, "%s", msg.c_str());
    attroff(COLOR_PAIR(CLR_CORRECT) | A_BOLD);
    char buf[80];
    snprintf(buf, sizeof(buf), "%d reviewed   %d to relearn", reviewed, again_count);
    attron(COLOR_PAIR(CLR_DIM));
    mvprintw(box_y + 4, box_x + (box_w - (int)strlen(buf)) / 2, "%s", buf);
    std::string hint = "[Press any key]";
    mvprintw(box_y + 6, box_x + (box_w - (int)hint.size()) / 2, "%s", hint.c_str());
    attroff(COLOR_PAIR(CLR_DIM));
    refresh();
    getch();
}

static void draw_drill_review_header(const ReviewItem& item, int idx, int total, int reviewed,
                                    int wrong_count, time_t session_start)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
    mvprintw(1, 2, "DRILL REVIEW");
    attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);

    std::string dname = item.deck_id;
    auto slash = dname.rfind('/');
    if (slash != std::string::npos) dname = dname.substr(slash + 1);
    attron(COLOR_PAIR(CLR_DIM));
    mvprintw(1, 17, "%s", dname.c_str());
    attroff(COLOR_PAIR(CLR_DIM));

    char stat[96];
    snprintf(stat, sizeof(stat), "card %d/%d  missed %d  %s", idx + 1, total, wrong_count,
             format_elapsed(session_start).c_str());
    attron(COLOR_PAIR(CLR_DIM));
    mvprintw(1, max_x - (int)strlen(stat) - 2, "%s", stat);
    attroff(COLOR_PAIR(CLR_DIM));

    int filled = total > 0 ? (reviewed * max_x) / total : 0;
    move(2, 0);
    for (int i = 0; i < max_x; i++)
    {
        bool on = i < filled;
        attron(COLOR_PAIR(on ? CLR_CORRECT : CLR_DIM));
        addch(ACS_HLINE);
        attroff(COLOR_PAIR(on ? CLR_CORRECT : CLR_DIM));
    }
}

// Drill Review: a weakness-pool pass over the cards you most often miss across completed decks.
// Each card is graded right/wrong, which feeds the per-card tally that ranks the pool.
static void run_drill_review(std::vector<ReviewItem>& items, Progress& progress)
{
    if (items.empty()) return;
    time_t session_start = time(nullptr);
    int total = (int)items.size();
    int reviewed = 0;
    int wrong_count = 0;
    timeout(1000); // refresh elapsed timer

    auto grade_card = [&](ReviewItem& item, bool correct)
    {
        progress.record_answer(item.deck_id, item.card_key, item.card_idx, correct);
        int st = progress.get_stage(item.deck_id, item.card_key, item.card_idx);
        progress.set_stage(item.deck_id, item.card_key, item.card_idx,
                           correct ? std::min(st + 1, 2) : std::max(st - 1, 0));
        progress.save();
        if (!correct) wrong_count++;
        reviewed++;
    };

    for (int idx = 0; idx < total; idx++)
    {
        ReviewItem& item = items[idx];
        bool quit = false;
        int typed_grade = -1; // 1 correct, 0 wrong

        // --- Question phase ---
        bool reveal = false;
        while (!reveal && !quit)
        {
            int max_y, max_x;
            getmaxyx(stdscr, max_y, max_x);
            clear();
            draw_drill_review_header(item, idx, total, reviewed, wrong_count, session_start);

            int content_w = std::min(max_x - 6, 60);
            auto q_lines = format_display_text(item.card.question, content_w - 4, CLR_DEFAULT, false);
            int card_h = std::min((int)q_lines.size() + 6, std::max(6, max_y - 9));
            int box_x = (max_x - content_w) / 2;
            int box_y = std::max(5, (max_y - card_h) / 2);
            draw_box(box_y, box_x, card_h, content_w);

            int inner_x = box_x + 2;
            int y = box_y + 1;
            attron(COLOR_PAIR(item.is_new ? CLR_HEADER : CLR_DIM));
            mvprintw(y, inner_x, "%s", item.is_new ? "[new]" : "[weak]");
            attroff(COLOR_PAIR(item.is_new ? CLR_HEADER : CLR_DIM));
            y += 2;
            draw_styled_lines(q_lines, y, inner_x, box_y + card_h - 1, content_w - 4);

            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(max_y - 2, 1, "[t] Type Answer  [Space] Show Answer  [a] Ask AI  [q] Quit");
            attroff(COLOR_PAIR(CLR_DIM));
            refresh();

            int ch = getch();
            if (ch == ERR) continue;
            if (ch == 'q' || ch == 27) { quit = true; break; }
            if (ch == 'a') { show_ai_assistant(item.card, item.deck_id); timeout(1000); continue; }
            if (ch == 't')
            {
                std::string model = ensure_ollama_ready();
                if (model.empty())
                {
                    timeout(1000);
                    continue;
                }
                int result_rows = 14;
                int max_typed_card_h = std::max(16, max_y - 6);
                int typed_card_h =
                    std::min(std::max((int)q_lines.size() + result_rows + 7, 16), max_typed_card_h);
                result_rows = std::max(7, std::min(result_rows, typed_card_h - 7));
                int typed_box_y = std::max(5, (max_y - typed_card_h) / 2);
                clear();
                draw_drill_review_header(item, idx, total, reviewed, wrong_count, session_start);
                draw_box(typed_box_y, box_x, typed_card_h, content_w);

                int typed_inner_x = box_x + 2;
                int typed_y = typed_box_y + 1;
                attron(COLOR_PAIR(item.is_new ? CLR_HEADER : CLR_DIM));
                mvprintw(typed_y, typed_inner_x, "%s", item.is_new ? "[new]" : "[weak]");
                attroff(COLOR_PAIR(item.is_new ? CLR_HEADER : CLR_DIM));
                typed_y += 2;
                int input_y = typed_box_y + typed_card_h - result_rows - 1;
                draw_styled_lines(q_lines, typed_y, typed_inner_x, input_y - 1, content_w - 4);
                attron(COLOR_PAIR(CLR_BORDER));
                mvhline(input_y - 1, box_x + 1, ACS_HLINE, content_w - 2);
                attroff(COLOR_PAIR(CLR_BORDER));
                attron(COLOR_PAIR(CLR_DIM));
                mvhline(max_y - 2, 1, ' ', std::max(1, max_x - 2));
                attroff(COLOR_PAIR(CLR_DIM));
                refresh();

                TypedAnswerAction action =
                    prompt_and_judge_typed_answer(model, item.card, item.deck_id, input_y,
                                                  typed_inner_x, content_w - 4, result_rows,
                                                  typed_box_y);
                timeout(1000);
                if (action == TypedAnswerAction::Correct) { typed_grade = 1; reveal = true; break; }
                if (action == TypedAnswerAction::Wrong) { typed_grade = 0; reveal = true; break; }
                continue;
            }
            if (ch == ' ') reveal = true;
        }
        if (quit) break;

        if (typed_grade >= 0)
        {
            grade_card(item, typed_grade == 1);
            continue;
        }

        // --- Answer + grade phase ---
        bool graded = false;
        while (!graded && !quit)
        {
            int max_y, max_x;
            getmaxyx(stdscr, max_y, max_x);
            clear();
            draw_drill_review_header(item, idx, total, reviewed, wrong_count, session_start);

            int content_w = std::min(max_x - 6, 60);
            auto q_lines = format_display_text(item.card.question, content_w - 4, CLR_DIM, false);
            auto a_lines = format_display_text(item.card.answer, content_w - 4, CLR_DEFAULT, true);
            int card_h =
                std::min((int)q_lines.size() + (int)a_lines.size() + 8, std::max(8, max_y - 9));
            int box_x = (max_x - content_w) / 2;
            int box_y = std::max(5, (max_y - card_h) / 2);
            draw_box(box_y, box_x, card_h, content_w);

            int inner_x = box_x + 2;
            int y = box_y + 1;
            attron(COLOR_PAIR(item.is_new ? CLR_HEADER : CLR_DIM));
            mvprintw(y, inner_x, "%s", item.is_new ? "[new]" : "[weak]");
            attroff(COLOR_PAIR(item.is_new ? CLR_HEADER : CLR_DIM));
            y += 2;
            draw_styled_lines(q_lines, y, inner_x, box_y + card_h - 1, content_w - 4);
            y++;
            attron(COLOR_PAIR(CLR_BORDER));
            mvhline(y, box_x + 1, ACS_HLINE, content_w - 2);
            attroff(COLOR_PAIR(CLR_BORDER));
            y += 2;
            draw_styled_lines(a_lines, y, inner_x, box_y + card_h - 1, content_w - 4);

            attron(COLOR_PAIR(CLR_CORRECT));
            mvprintw(max_y - 3, 1, "[Space/y] Got it");
            attroff(COLOR_PAIR(CLR_CORRECT));
            attron(COLOR_PAIR(CLR_WRONG));
            mvprintw(max_y - 3, 20, "[n] Missed");
            attroff(COLOR_PAIR(CLR_WRONG));
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(max_y - 2, 1, "[a] Ask AI  [q] Quit");
            attroff(COLOR_PAIR(CLR_DIM));
            refresh();

            int ch = getch();
            if (ch == ERR) continue;
            if (ch == 'q' || ch == 27) { quit = true; break; }
            if (ch == 'a') { show_ai_assistant(item.card, item.deck_id); timeout(1000); continue; }
            if (ch == ' ' || ch == 'y') { grade_card(item, true); graded = true; }
            else if (ch == 'n') { grade_card(item, false); graded = true; }
        }
        if (quit) break;
    }

    timeout(-1);
    bool completed = (reviewed >= total);
    progress.record_session(g_vault_root, "__drill_review__", "", total,
                            (int)difftime(time(nullptr), session_start), completed);
    progress.save();

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    clear();
    int box_w = 44, box_h = 9;
    int box_x = (max_x - box_w) / 2;
    int box_y = (max_y - box_h) / 2;
    draw_box(box_y, box_x, box_h, box_w);
    attron(COLOR_PAIR(CLR_CORRECT) | A_BOLD);
    std::string msg = "DRILL REVIEW COMPLETE";
    mvprintw(box_y + 2, box_x + (box_w - (int)msg.size()) / 2, "%s", msg.c_str());
    attroff(COLOR_PAIR(CLR_CORRECT) | A_BOLD);
    char buf[80];
    int right_count = reviewed - wrong_count;
    snprintf(buf, sizeof(buf), "%d reviewed   %d right   %d missed", reviewed, right_count,
             wrong_count);
    attron(COLOR_PAIR(CLR_DIM));
    mvprintw(box_y + 4, box_x + (box_w - (int)strlen(buf)) / 2, "%s", buf);
    std::string hint = "[Press any key]";
    mvprintw(box_y + 6, box_x + (box_w - (int)hint.size()) / 2, "%s", hint.c_str());
    attroff(COLOR_PAIR(CLR_DIM));
    refresh();
    getch();
}

int main(int argc, char* argv[])
{
    // Headless CLI handling (no ncurses) for scripting and package-manager tests.
    if (argc > 1)
    {
        std::string arg = argv[1];
        if (arg == "--version" || arg == "-v")
        {
            printf("grimoire %s\n", GRIMOIRE_VERSION);
            return 0;
        }
        if (arg == "--help" || arg == "-h")
        {
            printf("grimoire %s - terminal flashcard drill with spaced repetition\n\n",
                   GRIMOIRE_VERSION);
            printf("Usage: grimoire [command]\n\n");
            printf("With no command, launches the interactive TUI.\n\n");
            printf("Commands:\n");
            printf("  --due, review-count   Print the number of cards due for review\n");
            printf("  --version, -v         Print version\n");
            printf("  --help, -h            Show this help\n");
            return 0;
        }
        if (arg == "--due" || arg == "review-count")
        {
            AppConfig config;
            if (!config.load())
            {
                printf("0\n");
                return 0;
            }
            Progress progress;
            progress.load();
            auto queue = build_drill_review_pool(progress);
            printf("%d\n", (int)queue.size());
            return 0;
        }
        fprintf(stderr, "grimoire: unknown option '%s' (try --help)\n", arg.c_str());
        return 1;
    }

    // Init ncurses
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    curs_set(0);

    if (has_colors()) { init_colors(); }

    AppConfig config;
    if (!config.load())
    {
        if (!run_first_time_setup(config))
        {
            endwin();
            return 0;
        }
    }

    // Load progress
    Progress progress;
    progress.load();
    write_library_metadata(progress);

    while (true)
    {
        std::vector<ReviewItem> review_queue = build_drill_review_pool(progress);
        int splash_ch = show_splash((int)review_queue.size());
        if (splash_ch == 'q') break;
        if (splash_ch == 'v')
        {
            manage_vaults(config);
            continue;
        }
        if (splash_ch == 'r')
        {
            run_drill_review(review_queue, progress);
            continue;
        }

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
                DrillSession::clear_session(saved.value("deck_id", ""));
                continue;
            }
            DrillSession::clear_session();
            continue;
        }

        // Normal browse flow
        while (true)
        {
            std::string deck_root = expand_home(g_deck_dir);
            std::string deck_path = browse_decks(deck_root, progress);
            if (deck_path.empty()) break;

            auto deck = parse_deck(deck_path);
            if (deck.cards.empty()) continue;

            std::string deck_id = deck.id.empty() ? deck_id_from_path(deck_path, deck_root) : deck.id;
            std::string dname = deck.title.empty() ? deck_id : deck.title;
            auto slash = dname.rfind('/');
            if (slash != std::string::npos) dname = dname.substr(slash + 1);
            std::replace(dname.begin(), dname.end(), '_', ' ');
            std::replace(dname.begin(), dname.end(), '-', ' ');

            auto saved = DrillSession::load_session(deck_id);
            if (saved.contains("deck_path"))
            {
                int action = choose_saved_deck_session(dname, saved);
                if (action == 'q') continue;
                if (action == 'c')
                {
                    DrillSession session;
                    session.restore(saved, std::move(deck.cards), &progress);
                    int elapsed = saved.value("elapsed", 0);
                    run_drill(session, deck_path, elapsed);
                    continue;
                }
                DrillSession::clear_session(deck_id);
            }

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
