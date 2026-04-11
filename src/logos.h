#pragma once

// Logo variant 1: Block style
static const char* LOGO_BLOCK[] = {
    "██████╗ ██████╗ ██╗███╗   ███╗ ██████╗ ██╗██████╗ ███████╗",
    "██╔════╝ ██╔══██╗██║████╗ ████║██╔═══██╗██║██╔══██╗██╔════╝",
    "██║  ███╗██████╔╝██║██╔████╔██║██║   ██║██║██████╔╝█████╗  ",
    "██║   ██║██╔══██╗██║██║╚██╔╝██║██║   ██║██║██╔══██╗██╔══╝  ",
    "╚██████╔╝██║  ██║██║██║ ╚═╝ ██║╚██████╔╝██║██║  ██║███████╗",
    " ╚═════╝ ╚═╝  ╚═╝╚═╝╚═╝     ╚═╝ ╚═════╝ ╚═╝╚═╝  ╚═╝╚══════╝",
    nullptr
};
static const int LOGO_BLOCK_W = 58;

// Logo variant 2: Shadow style
static const char* LOGO_SHADOW[] = {
    "  ▄████  ██▀███   ██▓ ███▄ ▄███▓ ▒█████   ██▓ ██▀███  ▓█████ ",
    " ██▒ ▀█▒▓██ ▒ ██▒▓██▒▓██▒▀█▀ ██▒▒██▒  ██▒▓██▒▓██ ▒ ██▒▓█   ▀ ",
    "▒██░▄▄▄░▓██ ░▄█ ▒▒██▒▓██    ▓██░▒██░  ██▒▒██▒▓██ ░▄█ ▒▒███   ",
    "░▓█  ██▓▒██▀▀█▄  ░██░▒██    ▒██ ▒██   ██░░██░▒██▀▀█▄  ▒▓█  ▄ ",
    "░▒▓███▀▒░██▓ ▒██▒░██░▒██▒   ░██▒░ ████▓▒░░██░░██▓ ▒██▒░▒████▒",
    " ░▒   ▒ ░ ▒▓ ░▒▓░░▓  ░ ▒░   ░  ░░ ▒░▒░▒░ ░▓  ░ ▒▓ ░▒▓░░░ ▒░ ░",
    "  ░   ░   ░▒ ░ ▒░ ▒ ░░  ░      ░  ░ ▒ ▒░  ▒ ░  ░▒ ░ ▒░ ░ ░  ░",
    "░ ░   ░   ░░   ░  ▒ ░░      ░   ░ ░ ░ ▒   ▒ ░  ░░   ░    ░   ",
    "      ░    ░      ░         ░       ░ ░   ░     ░        ░  ░",
    nullptr
};
static const int LOGO_SHADOW_W = 63;

// Logo variant 3: Outline style
static const char* LOGO_OUTLINE[] = {
    " ___      _                 _          ",
    "/ _ \\_ __(_)_ __ ___   ___ (_)_ __ ___ ",
    "/ /_\\/ '__| | '_ ` _ \\ / _ \\| | '__/ _ \\",
    "/ /_\\\\| |  | | | | | | | (_) | | | |  __/",
    "\\____/|_|  |_|_| |_| |_|\\___/|_|_|  \\___|",
    nullptr
};
static const int LOGO_OUTLINE_W = 43;

struct LogoVariant {
    const char** lines;
    int width;
    int height;
};

static const LogoVariant LOGOS[] = {
    { LOGO_BLOCK,   LOGO_BLOCK_W,   6 },
    { LOGO_SHADOW,  LOGO_SHADOW_W,  9 },
    { LOGO_OUTLINE, LOGO_OUTLINE_W, 5 },
};
static const int LOGO_COUNT = 3;
