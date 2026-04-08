#include <ncurses.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// --- Config ---

static const std::string DECK_DIR = "~/knowledge_vault/Study-Vault/Anki";
static const std::string DATA_FILE = "~/.local/share/grimoire/progress.json";

static std::string expand_home(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = std::getenv("HOME");
    if (!home) return path;
    return std::string(home) + path.substr(1);
}

// --- Card / Deck ---

struct Card {
    std::string question;
    std::string answer;
};

struct DeckEntry {
    std::string name;
    std::string path;
    bool is_dir;
};

static std::vector<Card> parse_deck(const std::string& path) {
    std::vector<Card> cards;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        auto sep = line.find(" :: ");
        if (sep == std::string::npos) continue;
        std::string q = line.substr(0, sep);
        std::string a = line.substr(sep + 4);
        if (q.empty() || a.empty()) continue;
        cards.push_back({q, a});
    }
    return cards;
}

static std::vector<DeckEntry> list_dir(const std::string& dir) {
    std::vector<DeckEntry> entries;
    if (!fs::exists(dir)) return entries;
    for (auto& e : fs::directory_iterator(dir)) {
        DeckEntry d;
        d.path = e.path().string();
        d.name = e.path().filename().string();
        d.is_dir = e.is_directory();
        if (d.is_dir || e.path().extension() == ".txt") {
            entries.push_back(d);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const DeckEntry& a, const DeckEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return a.name < b.name;
    });
    return entries;
}

// --- Progress ---

struct Progress {
    json drill_mastery;
    json deck_stats;

    void load() {
        std::string path = expand_home(DATA_FILE);
        std::ifstream file(path);
        if (!file.is_open()) {
            drill_mastery = json::object();
            deck_stats = json::object();
            return;
        }
        try {
            json data = json::parse(file);
            drill_mastery = data.value("drill_mastery", json::object());
            deck_stats = data.value("deck_stats", json::object());
        } catch (...) {
            drill_mastery = json::object();
            deck_stats = json::object();
        }
    }

    void save() {
        std::string path = expand_home(DATA_FILE);
        std::string dir = fs::path(path).parent_path().string();
        fs::create_directories(dir);
        std::ofstream file(path);
        json data;
        data["drill_mastery"] = drill_mastery;
        data["deck_stats"] = deck_stats;
        file << data.dump(2);
    }

    int get_stage(const std::string& deck_id, int card_idx) {
        std::string key = deck_id + ":" + std::to_string(card_idx);
        if (drill_mastery.contains(key)) {
            return drill_mastery[key].get<int>();
        }
        return 0;
    }

    void set_stage(const std::string& deck_id, int card_idx, int stage) {
        std::string key = deck_id + ":" + std::to_string(card_idx);
        drill_mastery[key] = stage;
    }
};

// --- Drill Logic ---

static int stage_target(int stage) {
    switch (stage) {
        case 0: return 3;
        case 1: return 2;
        case 2: return 1;
        default: return 3;
    }
}

static const char* stage_label(int stage) {
    switch (stage) {
        case 0: return "New";
        case 1: return "Familiar";
        case 2: return "Strong";
        default: return "New";
    }
}

struct DrillSession {
    std::string deck_id;
    std::string deck_name; // display name
    std::vector<Card> cards;
    Progress* progress;

    std::vector<int> round;
    std::vector<int> missed;
    std::vector<int> streaks;
    std::vector<int> targets;
    int round_num = 1;

    void init(const std::string& id, std::vector<Card> c, Progress* p) {
        deck_id = id;
        cards = std::move(c);
        progress = p;
        round_num = 1;
        missed.clear();

        int n = cards.size();
        streaks.assign(n, 0);
        targets.resize(n);

        round.clear();
        for (int i = 0; i < n; i++) {
            int stage = progress->get_stage(deck_id, i);
            targets[i] = stage_target(stage);
            round.push_back(i);
        }
        shuffle_round();
    }

    void shuffle_round() {
        static std::mt19937 rng(std::random_device{}());
        std::shuffle(round.begin(), round.end(), rng);
    }

    bool next_round() {
        if (missed.empty()) return false;
        round = std::move(missed);
        missed.clear();
        round_num++;
        shuffle_round();
        return true;
    }

    int mastered_count() {
        int count = 0;
        for (int i = 0; i < (int)cards.size(); i++) {
            if (streaks[i] >= targets[i]) count++;
        }
        return count;
    }

    void mark_correct(int idx) {
        streaks[idx]++;
        if (streaks[idx] >= targets[idx]) {
            int stage = progress->get_stage(deck_id, idx);
            int new_stage = std::min(stage + 1, 2);
            progress->set_stage(deck_id, idx, new_stage);
            progress->save();
        } else {
            missed.push_back(idx);
        }
    }

    void mark_wrong(int idx) {
        streaks[idx] = 0;
        int stage = progress->get_stage(deck_id, idx);
        int new_stage = std::max(stage - 1, 0);
        progress->set_stage(deck_id, idx, new_stage);
        targets[idx] = stage_target(new_stage);
        progress->save();
        missed.push_back(idx);
    }
};

// --- TUI ---

static std::vector<std::string> wrap_text(const std::string& text, int width) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string word;
    std::string line;
    while (stream >> word) {
        if (line.empty()) {
            line = word;
        } else if ((int)(line.size() + 1 + word.size()) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line += " " + word;
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

static std::string strip_txt(const std::string& name) {
    if (name.size() > 4 && name.substr(name.size() - 4) == ".txt")
        return name.substr(0, name.size() - 4);
    return name;
}

enum Color {
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
};

static void init_colors() {
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
}

static int stage_color(int stage) {
    switch (stage) {
        case 0: return CLR_STAGE_NEW;
        case 1: return CLR_STAGE_FAMILIAR;
        case 2: return CLR_STAGE_STRONG;
        default: return CLR_STAGE_NEW;
    }
}

// Draw a vertical line
static void draw_vline(int y, int x, int h) {
    attron(COLOR_PAIR(CLR_BORDER));
    for (int i = 0; i < h; i++) {
        mvaddch(y + i, x, ACS_VLINE);
    }
    attroff(COLOR_PAIR(CLR_BORDER));
}

// Draw a full-width horizontal line
static void draw_hline_full(int y, int x, int w) {
    attron(COLOR_PAIR(CLR_BORDER));
    mvhline(y, x, ACS_HLINE, w);
    attroff(COLOR_PAIR(CLR_BORDER));
}

// Draw a box with ACS characters
static void draw_box(int y, int x, int h, int w) {
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

static std::string shell_escape(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static std::string query_ollama(const std::string& model, const Card& card,
                                 const std::string& deck, const std::string& user_question) {
    std::string system_prompt =
        "You are a helpful study assistant. The user is studying flashcards and needs help understanding the current card.\n\n"
        "Current flashcard:\n"
        "- Question: " + card.question + "\n"
        "- Answer: " + card.answer + "\n"
        "- Deck: " + deck + "\n\n"
        "Keep your responses concise and focused on helping the user understand this specific card. "
        "Explain concepts, provide mnemonics, give examples, or clarify anything about this card.\n\n"
        "IMPORTANT: Output plain text only. Do NOT use markdown formatting like **bold**, *italic*, headers (#), or bullet points (-/*). Just use plain sentences and paragraphs.";

    json payload;
    payload["model"] = model;
    payload["prompt"] = user_question;
    payload["system"] = system_prompt;
    payload["stream"] = false;

    std::string cmd = "curl -s -X POST " + OLLAMA_URL + "/api/generate "
                      "-H 'Content-Type: application/json' "
                      "-d " + shell_escape(payload.dump()) + " 2>/dev/null";

    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "Error: failed to run curl";
    std::array<char, 4096> buf;
    while (fgets(buf.data(), buf.size(), pipe)) {
        result += buf.data();
    }
    pclose(pipe);

    if (result.empty()) return "Error: empty response from Ollama. Is the model running?";

    try {
        auto resp = json::parse(result);
        if (resp.contains("response")) return resp["response"].get<std::string>();
        if (resp.contains("error")) return "Ollama error: " + resp["error"].get<std::string>();
    } catch (...) {}
    return "Error: could not parse Ollama response";
}

static std::string get_loaded_model() {
    FILE* pipe = popen("ollama ps 2>/dev/null | tail -n +2 | head -n 1 | awk '{print $1}'", "r");
    if (!pipe) return "";
    char buf[256];
    std::string model;
    if (fgets(buf, sizeof(buf), pipe)) {
        model = buf;
        while (!model.empty() && (model.back() == '\n' || model.back() == ' '))
            model.pop_back();
    }
    pclose(pipe);
    return model;
}

static std::string get_input(int y, int x, int max_w) {
    std::string input;
    curs_set(1);
    timeout(-1);
    while (true) {
        mvhline(y, x, ' ', max_w);
        mvprintw(y, x, "> %s", input.c_str());
        refresh();
        int ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) break;
        if (ch == 27) { input.clear(); break; }
        if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && !input.empty()) {
            input.pop_back();
        } else if (ch >= 32 && ch < 127 && (int)input.size() < max_w - 4) {
            input += (char)ch;
        }
    }
    curs_set(0);
    return input;
}

// Check if ollama service is running
static bool ollama_is_running() {
    return system("systemctl is-active --quiet ollama") == 0;
}

// Start ollama service
static void start_ollama() {
    system("systemctl start ollama >/dev/null 2>&1");
}

// Get list of available models
static std::vector<std::string> get_available_models() {
    std::vector<std::string> models;
    FILE* pipe = popen("ollama list 2>/dev/null | tail -n +2 | awk '{print $1}'", "r");
    if (!pipe) return models;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string m = buf;
        while (!m.empty() && (m.back() == '\n' || m.back() == ' '))
            m.pop_back();
        if (!m.empty()) models.push_back(m);
    }
    pclose(pipe);
    return models;
}

// Load a model by sending a minimal request
static void load_model(const std::string& model) {
    json payload;
    payload["model"] = model;
    payload["prompt"] = "hi";
    payload["stream"] = false;
    std::string cmd = "curl -s -X POST " + OLLAMA_URL + "/api/generate "
                      "-H 'Content-Type: application/json' "
                      "-d " + shell_escape(payload.dump()) + " >/dev/null 2>&1";
    system(cmd.c_str());
}

// In-app model picker, returns selected model or empty string
static std::string pick_model() {
    auto models = get_available_models();
    if (models.empty()) return "";

    int selected = 0;
    int scroll = 0;

    while (true) {
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

        for (int i = 0; i < visible && (i + scroll) < (int)models.size(); i++) {
            int idx = i + scroll;
            int y = list_y + i;
            if (idx == selected) {
                attron(COLOR_PAIR(CLR_HIGHLIGHT));
                mvprintw(y, 3, "%-*s", max_x - 6, models[idx].c_str());
                attroff(COLOR_PAIR(CLR_HIGHLIGHT));
            } else {
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
        if (ch == 'j' || ch == KEY_DOWN) { if (selected < (int)models.size() - 1) selected++; }
        if (ch == 'k' || ch == KEY_UP) { if (selected > 0) selected--; }
        if (ch == '\n' || ch == KEY_ENTER) return models[selected];
    }
}

// Ensure ollama is ready with a loaded model
static std::string ensure_ollama_ready() {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    if (!ollama_is_running()) {
        clear();
        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(max_y / 2, (max_x - 22) / 2, "Starting Ollama...");
        attroff(COLOR_PAIR(CLR_DIM));
        refresh();
        start_ollama();
        // Wait for it to be ready
        for (int i = 0; i < 10; i++) {
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

static void show_ai_assistant(const Card& card, const std::string& deck) {
    std::string model = ensure_ollama_ready();
    if (model.empty()) return;

    int scroll = 0;
    std::vector<std::string> response_lines;
    std::string last_question;

    while (true) {
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
        for (auto& l : q_ctx) { mvprintw(y++, left, "%s", l.c_str()); }
        auto a_ctx = wrap_text("A: " + card.answer, content_w);
        for (auto& l : a_ctx) { mvprintw(y++, left, "%s", l.c_str()); }
        attroff(COLOR_PAIR(CLR_DIM));

        draw_hline_full(y, 0, max_x);
        y++;

        int resp_h = max_y - y - 4;
        if (resp_h < 1) resp_h = 1;

        if (response_lines.empty() && last_question.empty()) {
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(y + resp_h / 2, (max_x - 30) / 2, "Press [Enter] to ask a question");
            attroff(COLOR_PAIR(CLR_DIM));
        } else {
            int total = (int)response_lines.size();
            if (scroll > total - resp_h) scroll = std::max(0, total - resp_h);
            if (scroll < 0) scroll = 0;

            for (int i = 0; i < resp_h && (i + scroll) < total; i++) {
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
        if (ch == 'q' || ch == 27) {
            timeout(1000);
            return;
        }
        if (ch == 'j' || ch == KEY_DOWN) { scroll++; continue; }
        if (ch == 'k' || ch == KEY_UP) { if (scroll > 0) scroll--; continue; }
        if (ch == '\n' || ch == KEY_ENTER) {
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
            for (auto& l : q_wrapped) response_lines.push_back(l);
            response_lines.push_back("");

            std::istringstream rstream(response);
            std::string para;
            while (std::getline(rstream, para)) {
                if (para.empty()) {
                    response_lines.push_back("");
                } else {
                    auto wrapped = wrap_text(para, content_w);
                    for (auto& l : wrapped) response_lines.push_back(l);
                }
            }
            scroll = 0;
        }
    }
}

// Collect all subjects (top-level dirs) and their decks
struct Subject {
    std::string name;
    std::string path;
    std::vector<DeckEntry> decks; // files and subdirs within
};

static std::vector<Subject> collect_subjects(const std::string& root) {
    std::vector<Subject> subjects;
    if (!fs::exists(root)) return subjects;
    for (auto& e : fs::directory_iterator(root)) {
        if (!e.is_directory()) continue;
        Subject s;
        s.name = e.path().filename().string();
        s.path = e.path().string();
        // Recursively collect all .txt files under this subject
        for (auto& f : fs::recursive_directory_iterator(e.path())) {
            if (f.is_regular_file() && f.path().extension() == ".txt") {
                DeckEntry d;
                d.path = f.path().string();
                // Show path relative to subject dir
                std::string rel = f.path().string().substr(e.path().string().size() + 1);
                d.name = strip_txt(rel);
                d.is_dir = false;
                s.decks.push_back(d);
            }
        }
        std::sort(s.decks.begin(), s.decks.end(), [](const DeckEntry& a, const DeckEntry& b) {
            return a.name < b.name;
        });
        if (!s.decks.empty()) {
            subjects.push_back(s);
        }
    }
    std::sort(subjects.begin(), subjects.end(), [](const Subject& a, const Subject& b) {
        return a.name < b.name;
    });
    return subjects;
}

// Two-pane deck browser (ncmpcpp style)
static std::string browse_decks(const std::string& root) {
    auto subjects = collect_subjects(root);
    if (subjects.empty()) return "";

    int pane = 0; // 0 = subjects (left), 1 = decks (right)
    int subj_sel = 0, subj_scroll = 0;
    int deck_sel = 0, deck_scroll = 0;

    while (true) {
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        clear();

        // Title bar
        attron(COLOR_PAIR(CLR_TITLE) | A_BOLD);
        std::string title = "Grimoire";
        mvprintw(0, (max_x - (int)title.size()) / 2, "%s", title.c_str());
        attroff(COLOR_PAIR(CLR_TITLE) | A_BOLD);

        // Layout: header line, column headers, divider, list area, footer
        int header_y = 1;
        draw_hline_full(header_y, 0, max_x);

        int col_header_y = 2;
        int list_start = 3;
        int footer_y = max_y - 1;
        int list_h = footer_y - list_start;
        if (list_h < 1) list_h = 1;

        // Column widths
        int left_w = max_x / 3;
        int right_w = max_x - left_w - 1; // -1 for divider

        // Column headers
        attron(COLOR_PAIR(CLR_COLHEAD) | A_BOLD);
        mvprintw(col_header_y, 1, "Subjects");
        mvprintw(col_header_y, left_w + 2, "Decks");
        attroff(COLOR_PAIR(CLR_COLHEAD) | A_BOLD);

        // Vertical divider
        draw_vline(col_header_y, left_w, list_h + 1);

        // Horizontal line under column headers
        draw_hline_full(list_start - 1, 0, max_x);

        // --- Left pane: subjects ---
        int subj_visible = list_h;
        if (subj_sel < subj_scroll) subj_scroll = subj_sel;
        if (subj_sel >= subj_scroll + subj_visible) subj_scroll = subj_sel - subj_visible + 1;

        for (int i = 0; i < subj_visible && (i + subj_scroll) < (int)subjects.size(); i++) {
            int idx = i + subj_scroll;
            int y = list_start + i;
            std::string display = subjects[idx].name;
            if ((int)display.size() > left_w - 2) display = display.substr(0, left_w - 2);

            if (idx == subj_sel) {
                if (pane == 0) {
                    attron(COLOR_PAIR(CLR_HIGHLIGHT));
                    mvprintw(y, 1, "%-*s", left_w - 2, display.c_str());
                    attroff(COLOR_PAIR(CLR_HIGHLIGHT));
                } else {
                    attron(COLOR_PAIR(CLR_DIR) | A_BOLD);
                    mvprintw(y, 1, "%s", display.c_str());
                    attroff(COLOR_PAIR(CLR_DIR) | A_BOLD);
                }
            } else {
                mvprintw(y, 1, "%s", display.c_str());
            }
        }

        // --- Right pane: decks for selected subject ---
        auto& decks = subjects[subj_sel].decks;
        int deck_visible = list_h;
        if (deck_sel < deck_scroll) deck_scroll = deck_sel;
        if (deck_sel >= deck_scroll + deck_visible) deck_scroll = deck_sel - deck_visible + 1;

        for (int i = 0; i < deck_visible && (i + deck_scroll) < (int)decks.size(); i++) {
            int idx = i + deck_scroll;
            int y = list_start + i;
            std::string display = decks[idx].name;
            if ((int)display.size() > right_w - 2) display = display.substr(0, right_w - 2);

            if (pane == 1 && idx == deck_sel) {
                attron(COLOR_PAIR(CLR_HIGHLIGHT));
                mvprintw(y, left_w + 2, "%-*s", right_w - 2, display.c_str());
                attroff(COLOR_PAIR(CLR_HIGHLIGHT));
            } else {
                mvprintw(y, left_w + 2, "%s", display.c_str());
            }
        }

        // Footer
        draw_hline_full(footer_y - 1, 0, max_x);
        attron(COLOR_PAIR(CLR_DIM));
        mvprintw(footer_y, 1, "[j/k] navigate  [h/l] switch pane  [Enter] select  [q] quit");
        attroff(COLOR_PAIR(CLR_DIM));

        refresh();

        int ch = getch();
        if (ch == 'q' || ch == 27) return "";

        if (ch == 'j' || ch == KEY_DOWN) {
            if (pane == 0) {
                if (subj_sel < (int)subjects.size() - 1) {
                    subj_sel++;
                    deck_sel = 0;
                    deck_scroll = 0;
                }
            } else {
                if (deck_sel < (int)decks.size() - 1) deck_sel++;
            }
        }
        if (ch == 'k' || ch == KEY_UP) {
            if (pane == 0) {
                if (subj_sel > 0) {
                    subj_sel--;
                    deck_sel = 0;
                    deck_scroll = 0;
                }
            } else {
                if (deck_sel > 0) deck_sel--;
            }
        }
        if (ch == 'l' || ch == KEY_RIGHT || ch == '\t') {
            if (pane == 0 && !decks.empty()) pane = 1;
        }
        if (ch == 'h' || ch == KEY_LEFT) {
            if (pane == 1) pane = 0;
        }
        if (ch == '\n' || ch == KEY_ENTER) {
            if (pane == 0 && !decks.empty()) {
                pane = 1;
            } else if (pane == 1 && !decks.empty()) {
                return decks[deck_sel].path;
            }
        }
    }
}

// Get a deck ID from a file path (relative to deck root)
static std::string deck_id_from_path(const std::string& path, const std::string& root) {
    std::string rel = path.substr(root.size() + 1);
    if (rel.size() > 4 && rel.substr(rel.size() - 4) == ".txt") {
        rel = rel.substr(0, rel.size() - 4);
    }
    return rel;
}

// Drill TUI - centered card with box
static std::string format_elapsed(time_t start) {
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

static void run_drill(DrillSession& session) {
    time_t session_start = time(nullptr);
    timeout(1000); // getch returns ERR after 1s so timer updates
    while (true) {
        // Check if round is done
        if (session.round.empty()) {
            if (!session.next_round()) {
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
                return;
            }
        }

        // Pop next card
        int card_idx = session.round.back();
        session.round.pop_back();
        auto& card = session.cards[card_idx];
        int card_target = session.targets[card_idx];
        int streak = session.streaks[card_idx];
        int stage = session.progress->get_stage(session.deck_id, card_idx);

        // --- Show question ---
        while (true) {
            int max_y, max_x;
            getmaxyx(stdscr, max_y, max_x);
            clear();

            // Line 0: timer, deck name centered, mastered right
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
            auto q_lines = wrap_text(card.question, content_w - 4);
            int card_h = (int)q_lines.size() + 8; // padding + label + streak
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
            for (auto& l : q_lines) {
                mvprintw(y, inner_x, "%s", l.c_str());
                y++;
            }

            // Footer
            draw_hline_full(max_y - 2, 0, max_x);
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(max_y - 1, 1, "[Space] Show Answer  [a] Ask AI  [q] Quit");
            attroff(COLOR_PAIR(CLR_DIM));

            refresh();

            int ch = getch();
            if (ch == ERR) continue; // timeout - redraw for timer
            if (ch == 'q' || ch == 27) { timeout(-1); return; }
            if (ch == 'a') { show_ai_assistant(card, session.deck_name); timeout(1000); continue; }
            if (ch == ' ') break;
        }

        // --- Show answer ---
        while (true) {
            int max_y, max_x;
            getmaxyx(stdscr, max_y, max_x);
            clear();

            // Top status bar
            int mastered = session.mastered_count();
            attron(COLOR_PAIR(CLR_DIM));
            char status_left[128];
            snprintf(status_left, sizeof(status_left), "%d/%d mastered",
                     mastered, (int)session.cards.size());
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
            auto q_wrapped = wrap_text(card.question, content_w - 4);
            auto a_wrapped = wrap_text(card.answer, content_w - 4);
            int card_h = (int)q_wrapped.size() + (int)a_wrapped.size() + 10;
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
            attron(COLOR_PAIR(CLR_DIM));
            for (auto& l : q_wrapped) {
                mvprintw(y, inner_x, "%s", l.c_str());
                y++;
            }
            attroff(COLOR_PAIR(CLR_DIM));
            y++;

            // Separator inside box
            attron(COLOR_PAIR(CLR_BORDER));
            mvhline(y, box_x + 1, ACS_HLINE, content_w - 2);
            attroff(COLOR_PAIR(CLR_BORDER));
            y += 2;

            // Answer
            attron(A_BOLD);
            for (auto& l : a_wrapped) {
                mvprintw(y, inner_x, "%s", l.c_str());
                y++;
            }
            attroff(A_BOLD);

            // Footer
            draw_hline_full(max_y - 3, 0, max_x);
            attron(COLOR_PAIR(CLR_CORRECT));
            mvprintw(max_y - 2, 1, "[y] Yes (%d/%d)", streak + 1, card_target);
            attroff(COLOR_PAIR(CLR_CORRECT));
            attron(COLOR_PAIR(CLR_WRONG));
            mvprintw(max_y - 2, 20, "[n] No (drop)");
            attroff(COLOR_PAIR(CLR_WRONG));
            attron(COLOR_PAIR(CLR_DIM));
            mvprintw(max_y - 1, 1, "[a] Ask AI  [q] Quit");
            attroff(COLOR_PAIR(CLR_DIM));

            refresh();

            int ch = getch();
            if (ch == ERR) continue; // timeout - redraw for timer
            if (ch == 'q' || ch == 27) { timeout(-1); return; }
            if (ch == 'a') { show_ai_assistant(card, session.deck_name); timeout(1000); continue; }
            if (ch == 'y') {
                session.mark_correct(card_idx);
                break;
            }
            if (ch == 'n') {
                session.mark_wrong(card_idx);
                break;
            }
        }
    }
}

int main(int argc, char* argv[]) {
    std::string deck_root = expand_home(DECK_DIR);

    // Init ncurses
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        init_colors();
    }

    // Browse and select deck
    std::string deck_path = browse_decks(deck_root);
    if (deck_path.empty()) {
        endwin();
        return 0;
    }

    // Parse deck
    auto cards = parse_deck(deck_path);
    if (cards.empty()) {
        endwin();
        fprintf(stderr, "No cards found in %s\n", deck_path.c_str());
        return 1;
    }

    // Load progress
    Progress progress;
    progress.load();

    // Build deck ID
    std::string deck_id = deck_id_from_path(deck_path, deck_root);

    // Run drill
    DrillSession session;
    session.init(deck_id, std::move(cards), &progress);
    // Display name: last component of deck_id, replace _ and - with spaces
    std::string dname = deck_id;
    auto slash = dname.rfind('/');
    if (slash != std::string::npos) dname = dname.substr(slash + 1);
    std::replace(dname.begin(), dname.end(), '_', ' ');
    std::replace(dname.begin(), dname.end(), '-', ' ');
    session.deck_name = dname;
    run_drill(session);

    endwin();
    return 0;
}
