/*
 * AxiomOS - bundled applications.
 *
 * Each application is a window with a paint callback plus optional key and
 * mouse handlers.  State lives in a small heap allocated struct so that
 * several instances of the same app can coexist.
 */

#include <gui.h>


/* ======================================================================
 * Terminal
 *
 * The window is owned by the terminal module (gui/apps/term/app.c); this
 * entry point only asks it to open a fresh window.  Keeping the entry point here
 * means the launcher table stays unchanged.
 * ====================================================================== */

void axterm_open(void);

void gui_app_terminal_open(void)
{
    axterm_open();
}


/* ======================================================================
 * File browser
 * ====================================================================== */

typedef struct files_state {
    i32 selected;
    i32 scroll;
    char entries[16][40];
    i32  count;
} files_state_t;

static void files_collect(files_state_t *state)
{
    state->count = 0;
    /* Static demonstration entries: the RAM filesystem exposes its listing
     * through the serial log, so the browser shows a curated view here. */
    const char *items[] = {
        "/axiomos            <dir>",
        "/axiomos/kernel.elf  96 KiB",
        "/axiomos/boot.nsh     3 B",
        "/docs/architecture.md",
        "/docs/hvci.md",
        "/README.md",
    };
    for (i32 i = 0; i < 6; i++) {
        i32 j = 0;
        while (items[i][j] && j < 39) {
            state->entries[i][j] = items[i][j];
            j++;
        }
        state->entries[i][j] = 0;
        state->count++;
    }
}

static void files_paint(gui_window_t *window)
{
    files_state_t *state = (files_state_t *)window->state;
    gui_surface_t *surface = window->content;
    const gui_theme_t *theme = gui_theme();

    gui_fill(surface, theme->window_bg);
    gui_fill_rect(surface, gui_rect(0, 0, 180, surface->height),
                  theme->window_bg_alt);
    gui_text(surface, 10, 10, "Places", theme->text_dim, 1);
    const char *places[] = {"ESP partition", "RAM filesystem", "Kernel image"};
    for (i32 i = 0; i < 3; i++) {
        gui_rect_t row = gui_rect(4, 34 + i * 26, 172, 22);
        if (i == 0)
            gui_fill_round_rect(surface, row, 5, theme->selection);
        gui_text(surface, 12, 38 + i * 26, places[i], theme->text, 1);
    }

    gui_text(surface, 196, 12, "/", theme->text, 1);
    gui_hline(surface, 192, surface->width - 8, 30, theme->border);

    for (i32 i = 0; i < state->count; i++) {
        i32 y = 40 + (i - state->scroll) * 22;
        if (y < 36 || y > surface->height - 24)
            continue;
        if (i == state->selected)
            gui_fill_round_rect(surface, gui_rect(192, y - 3,
                                                  surface->width - 200, 21), 4,
                                theme->selection);
        gui_text(surface, 200, y, state->entries[i],
                 i == state->selected ? theme->text : theme->text_dim, 1);
    }

    gui_widget_scrollbar(surface,
                         gui_rect(surface->width - 10, 34, 6,
                                  surface->height - 44),
                         state->scroll, state->count, 12,
                         theme->window_bg_alt, theme->accent_dim);
}

static void files_key(gui_window_t *window, char key, u8 scancode)
{
    files_state_t *state = (files_state_t *)window->state;
    if (scancode == 0x48 && state->selected > 0)      /* up */
        state->selected--;
    else if (scancode == 0x50 && state->selected + 1 < state->count)  /* down */
        state->selected++;
    else if (scancode == 0x1C && state->count > 0) {  /* enter */
        char line[48];
        i32 i = 0;
        for (; state->entries[state->selected][i] && i < 40; i++)
            line[i] = state->entries[state->selected][i];
        line[i] = 0;
        printk("gui: open %s\n", line);
    }
    (void)key;
}

void gui_app_files_open(void)
{
    gui_window_t *window = gui_window_create("Files", 160, 120, 620, 380,
                                             files_paint);
    if (!window)
        return;
    files_state_t *state = (files_state_t *)kzalloc(sizeof(*state));
    if (!state)
        return;
    window->state = state;
    window->on_key = files_key;
    files_collect(state);
}

/* ======================================================================
 * Settings / system information
 * ====================================================================== */

typedef struct settings_state {
    bool_t hvci;
    bool_t animations;
    i32    volume;
    gui_button_t apply;
} settings_state_t;

static void settings_paint(gui_window_t *window)
{
    settings_state_t *state = (settings_state_t *)window->state;
    gui_surface_t *surface = window->content;
    const gui_theme_t *theme = gui_theme();

    gui_fill(surface, theme->window_bg);
    gui_text(surface, 20, 16, "System settings", theme->text, 2);
    gui_hline(surface, 20, surface->width - 20, 44, theme->border);

    gui_text(surface, 20, 62, "Security", theme->accent, 1);
    gui_widget_checkbox(surface, 28, 88, state->hvci,
                        "Hypervisor enforced code integrity (HVCI)", theme->text,
                        theme->accent);
    gui_widget_checkbox(surface, 28, 112, state->animations,
                        "Window animations", theme->text, theme->accent);

    gui_text(surface, 20, 152, "Display", theme->accent, 1);
    gui_text(surface, 28, 176, "Brightness", theme->text_dim, 1);
    gui_widget_slider(surface, gui_rect(120, 178, 260, 16), state->volume, 100,
                      theme->accent, theme->window_bg_alt);

    gui_text(surface, 20, 212, "Kernel", theme->accent, 1);
    char buffer[64];
    hvci_status_t status;
    hvci_get_status(&status);
    gui_text(surface, 28, 238,
             status.mode == 2 ? "integrity: VMX root mode"
                              : "integrity: software enforcement",
             theme->text_dim, 1);
    gui_text(surface, 28, 258, "build: AxiomOS 1.0.0 (C + NASM + Rust)",
             theme->text_dim, 1);

    state->apply.rect = gui_rect(surface->width - 130, surface->height - 44, 110, 30);
    state->apply.label = "Apply";
    state->apply.primary = true;
    gui_widget_button(surface, (gui_theme_t *)theme, &state->apply);
    (void)buffer;
}

static void settings_mouse(gui_window_t *window, i32 x, i32 y, u8 buttons,
                           bool_t pressed)
{
    settings_state_t *state = (settings_state_t *)window->state;
    if (pressed && (buttons & 1)) {
        if (y >= 86 && y <= 102 && x >= 26 && x <= 40)
            state->hvci = (bool_t)!state->hvci;
        else if (y >= 110 && y <= 126 && x >= 26 && x <= 40)
            state->animations = (bool_t)!state->animations;
        else if (gui_rect_contains(state->apply.rect, x, y))
            printk("gui: settings applied\n");
    }
}

void gui_app_settings_open(void)
{
    gui_window_t *window = gui_window_create("Settings", 200, 140, 560, 380,
                                             settings_paint);
    if (!window)
        return;
    settings_state_t *state = (settings_state_t *)kzalloc(sizeof(*state));
    if (!state)
        return;
    state->hvci = true;
    state->volume = 62;
    window->state = state;
    window->on_mouse = settings_mouse;
}

/* Integer sine in 12.12 fixed point, used by the about logo. */
static i32 gui_sin(i32 degrees)
{
    i32 angle = degrees % 360;
    if (angle < 0)
        angle += 360;
    i32 sign = 1;
    if (angle > 180) {
        angle -= 180;
        sign = -1;
    }
    i32 q = (angle * 4096) / 180;
    i32 value;
    if (angle <= 90)
        value = (2 * q * (4096 - q / 2)) / 4096;
    else {
        i32 x = 4096 - q;
        value = (2 * x * (4096 - x / 2)) / 4096;
    }
    return sign * value;
}

/* ======================================================================
 * About
 * ====================================================================== */

static void about_paint(gui_window_t *window)
{
    gui_surface_t *surface = window->content;
    const gui_theme_t *theme = gui_theme();
    gui_fill(surface, theme->window_bg);

    /* Logo mark: a hexagonal frame with the letter A inside. */
    i32 cx = surface->width / 2;
    i32 cy = 84;
    for (i32 i = 0; i < 6; i++) {
        i32 angle = 60 * i + 30;
        i32 x0 = cx + 46 * gui_sin(angle) / 4096;
        i32 y0 = cy + 46 * gui_sin(angle - 90) / 4096;
        i32 x1 = cx + 46 * gui_sin(angle + 60) / 4096;
        i32 y1 = cy + 46 * gui_sin(angle - 30) / 4096;
        gui_line(surface, x0, y0, x1, y1, theme->accent);
    }
    gui_text(surface, cx - 7, cy - 8, "A", theme->accent, 2);

    const char *lines[] = {
        "AxiomOS 1.0.0 \"Cascade\"",
        "an open source operating system for x86_64",
        "",
        "boot: UEFI (ABP v1)   kernel: C, NASM, Rust",
        "graphics: software compositor, 1024x768x32",
        "security: HVCI manifest + W^X",
    };
    for (i32 i = 0; i < 6; i++) {
        i32 x = cx - gui_text_width(lines[i], 1) / 2;
        gui_text(surface, x, 150 + i * 20, lines[i],
                 i == 0 ? theme->text : theme->text_dim, 1);
    }
}

void gui_app_about_open(void)
{
    gui_window_create("About AxiomOS", 300, 150, 420, 300, about_paint);
}

/* ======================================================================
 * Calculator
 * ====================================================================== */

typedef struct calculator_state {
    i64 accumulator;
    i64 current;
    char display[24];
    char pending;
    bool_t entering;
    gui_button_t keys[16];
} calculator_state_t;

static void calc_update_display(calculator_state_t *state)
{
    i64 value = state->entering ? state->current : state->accumulator;
    char digits[24];
    i32 count = 0;
    bool_t negative = (bool_t)(value < 0);
    if (negative)
        value = -value;
    if (value == 0)
        digits[count++] = '0';
    while (value > 0 && count < 20) {
        digits[count++] = (char)('0' + (value % 10));
        value /= 10;
    }
    i32 index = 0;
    if (negative)
        state->display[index++] = '-';
    while (count > 0)
        state->display[index++] = digits[--count];
    state->display[index] = 0;
}

static void calc_paint(gui_window_t *window)
{
    calculator_state_t *state = (calculator_state_t *)window->state;
    gui_surface_t *surface = window->content;
    const gui_theme_t *theme = gui_theme();

    gui_fill(surface, theme->window_bg);
    gui_fill_round_rect(surface, gui_rect(12, 12, surface->width - 24, 48), 6,
                        GUI_RGB(0x08, 0x0C, 0x18));
    i32 width = gui_text_width(state->display, 2);
    gui_text(surface, surface->width - width - 20, 24, state->display,
             theme->accent, 2);

    const char *labels[16] = {
        "7", "8", "9", "/",
        "4", "5", "6", "*",
        "1", "2", "3", "-",
        "0", ".", "=", "+",
    };
    i32 grid_x = 12;
    i32 grid_y = 72;
    i32 cell_w = (surface->width - 24) / 4;
    i32 cell_h = (surface->height - 84) / 4;
    for (i32 i = 0; i < 16; i++) {
        i32 row = i / 4;
        i32 column = i % 4;
        state->keys[i].rect = gui_rect(grid_x + column * cell_w + 3,
                                       grid_y + row * cell_h + 3,
                                       cell_w - 6, cell_h - 6);
        state->keys[i].label = labels[i];
        state->keys[i].primary = (bool_t)(i == 14);
        gui_widget_button(surface, (gui_theme_t *)theme, &state->keys[i]);
    }
}

static void calc_key(gui_window_t *window, char key, u8 scancode)
{
    calculator_state_t *state = (calculator_state_t *)window->state;
    if (key >= '0' && key <= '9') {
        if (!state->entering) {
            state->current = 0;
            state->entering = true;
        }
        state->current = state->current * 10 + (key - '0');
    } else if (key == '+' || key == '-' || key == '*' || key == '/') {
        if (state->entering) {
            state->accumulator = state->current;
            state->entering = false;
        }
        state->pending = key;
    } else if (key == '=') {
        if (state->pending) {
            switch (state->pending) {
            case '+': state->accumulator += state->current; break;
            case '-': state->accumulator -= state->current; break;
            case '*': state->accumulator *= state->current; break;
            case '/':
                if (state->current != 0)
                    state->accumulator /= state->current;
                break;
            default: break;
            }
            state->pending = 0;
        }
        state->entering = false;
    }
    calc_update_display(state);
    (void)scancode;
}

void gui_app_calculator_open(void)
{
    gui_window_t *window = gui_window_create("Calculator", 700, 220, 300, 380,
                                             calc_paint);
    if (!window)
        return;
    calculator_state_t *state = (calculator_state_t *)kzalloc(sizeof(*state));
    if (!state)
        return;
    window->state = state;
    window->on_key = calc_key;
    window->resizable = false;
    calc_update_display(state);
}

/* ======================================================================
 * System monitor
 * ====================================================================== */

static void monitor_paint(gui_window_t *window)
{
    gui_surface_t *surface = window->content;
    const gui_theme_t *theme = gui_theme();
    gui_fill(surface, theme->window_bg);

    gui_text(surface, 16, 12, "System monitor", theme->text, 1);
    gui_hline(surface, 16, surface->width - 16, 32, theme->border);

    char buffer[64];
    hvci_status_t status;
    hvci_get_status(&status);

    struct { const char *label; u64 value; } rows[] = {
        { "uptime (ms)", timer_ms() },
        { "timer ticks", timer_ticks() },
        { "threads", (u64)sched_thread_count() },
        { "PCI devices", (u64)pci_device_count() },
        { "disk sectors", ahci_disk_size() },
        { "protected pages", status.protected_pages },
        { "integrity violations", status.violations },
    };

    for (i32 i = 0; i < 7; i++) {
        i32 y = 44 + i * 22;
        gui_text(surface, 20, y, rows[i].label, theme->text_dim, 1);
        /* Render the number. */
        char digits[20];
        i32 count = 0;
        u64 value = rows[i].value;
        if (value == 0)
            digits[count++] = '0';
        while (value && count < 18) {
            digits[count++] = (char)('0' + value % 10);
            value /= 10;
        }
        i32 index = 0;
        while (count > 0 && index < 20)
            buffer[index++] = digits[--count];
        buffer[index] = 0;
        gui_text(surface, 200, y, buffer, theme->text, 1);
    }

    /* CPU load bar animated from the tick counter. */
    i32 load = (i32)((timer_ticks() / 8) % 40) + 30;
    gui_text(surface, 20, surface->height - 46, "cpu", theme->text_dim, 1);
    gui_widget_progress(surface, gui_rect(60, surface->height - 46, 220, 12),
                        load, theme->accent, theme->window_bg_alt);

    /* Memory: pages used sketch. */
    gui_text(surface, 20, surface->height - 26, "mem", theme->text_dim, 1);
    gui_widget_progress(surface, gui_rect(60, surface->height - 26, 220, 12),
                        44, theme->success, theme->window_bg_alt);
}

void gui_app_taskmanager_open(void)
{
    gui_window_create("System Monitor", 560, 300, 460, 300, monitor_paint);
}

/* ======================================================================
 * HVCI status panel
 * ====================================================================== */

static void hvci_paint(gui_window_t *window)
{
    gui_surface_t *surface = window->content;
    const gui_theme_t *theme = gui_theme();
    hvci_status_t status;
    hvci_get_status(&status);

    gui_fill(surface, theme->window_bg);
    gui_text(surface, 18, 14, "Code integrity", theme->text, 2);
    gui_text(surface, 18, 44, status.active ? "enabled" : "disabled",
             status.active ? theme->success : theme->error, 1);

    gui_widget_badge(surface, surface->width - 130, 16,
                     status.mode == 2 ? "VMX mode" : "software mode",
                     theme->accent, theme->text_inverse);

    gui_hline(surface, 18, surface->width - 18, 66, theme->border);

    gui_text(surface, 20, 80, "Protected regions", theme->accent, 1);
    gui_text(surface, 20, 100, ".text   kernel/arch/x86_64/*.asm", theme->text_dim, 1);
    gui_text(surface, 20, 118, ".text   kernel/**/*.c, gui/**/*.c", theme->text_dim, 1);
    gui_text(surface, 20, 136, ".text   rust (axi_hvci, axi_crypto)", theme->text_dim, 1);
    gui_text(surface, 20, 154, ".rodata hvci manifest, page tables", theme->text_dim, 1);

    gui_text(surface, 20, 186, "Verification", theme->accent, 1);
    i32 scanned = (i32)(status.verified_pages % 100);
    gui_widget_progress(surface, gui_rect(20, 208, surface->width - 40, 10),
                        scanned, theme->accent, theme->window_bg_alt);
    gui_text(surface, 20, 226, status.detail, theme->text_dim, 1);

    gui_text(surface, 20, 258,
             "policy: W^X - writable pages are never executable",
             theme->text_dim, 1);
    gui_text(surface, 20, 278, "manifest: SHA-256 over each executable region",
             theme->text_dim, 1);
}

void gui_app_hvci_open(void)
{
    gui_window_create("HVCI", 640, 180, 480, 330, hvci_paint);
}

/* ======================================================================
 * Text editor
 * ====================================================================== */

typedef struct editor_state {
    char text[4096];
    i32  length;
    i32  scroll;
} editor_state_t;

static void editor_paint(gui_window_t *window)
{
    editor_state_t *state = (editor_state_t *)window->state;
    gui_surface_t *surface = window->content;
    const gui_theme_t *theme = gui_theme();

    gui_fill(surface, GUI_RGB(0x0A, 0x0E, 0x1B));
    gui_fill_rect(surface, gui_rect(0, 0, 28, surface->height),
                  GUI_RGB(0x11, 0x18, 0x2C));

    i32 line = 1 + state->scroll / 18;
    for (i32 i = 0; i < 6; i++) {
        char number[8];
        i32 value = line + i;
        i32 count = 0;
        char digits[8];
        if (value == 0)
            digits[count++] = '0';
        while (value && count < 6) {
            digits[count++] = (char)('0' + value % 10);
            value /= 10;
        }
        i32 index = 0;
        while (count > 0)
            number[index++] = digits[--count];
        number[index] = 0;
        gui_text(surface, 6, 10 + i * 18, number, GUI_RGB(0x4A, 0x56, 0x78), 1);
    }

    i32 x = 36;
    i32 y = 10;
    char line_buffer[96];
    i32 count = 0;
    for (i32 i = 0; i < state->length; i++) {
        char c = state->text[i];
        if (c == '\n') {
            line_buffer[count] = 0;
            gui_text(surface, x, y, line_buffer, GUI_RGB(0xD8, 0xE2, 0xFF), 1);
            count = 0;
            y += 18;
            if (y > surface->height - 18)
                break;
        } else if (count < 90) {
            line_buffer[count++] = c;
        }
    }
    if (count > 0) {
        line_buffer[count] = 0;
        gui_text(surface, x, y, line_buffer, GUI_RGB(0xD8, 0xE2, 0xFF), 1);
    }
    (void)theme;
}

static void editor_key(gui_window_t *window, char key, u8 scancode)
{
    editor_state_t *state = (editor_state_t *)window->state;
    if (key == '\b') {
        if (state->length > 0)
            state->length--;
    } else if (key >= 32 && key < 127 && key != '\n') {
        if (state->length < 4094)
            state->text[state->length++] = key;
    } else if (key == '\n') {
        if (state->length < 4094)
            state->text[state->length++] = '\n';
    }
    (void)scancode;
}

void gui_app_editor_open(void)
{
    gui_window_t *window = gui_window_create("Editor", 240, 160, 600, 380,
                                             editor_paint);
    if (!window)
        return;
    editor_state_t *state = (editor_state_t *)kzalloc(sizeof(*state));
    if (!state)
        return;
    window->state = state;
    window->on_key = editor_key;
    const char *seed = "AxiomOS editor\n\nStart typing. Everything you write stays ";
    i32 i = 0;
    while (seed[i] && i < 4000) {
        state->text[i] = seed[i];
        i++;
    }
    state->length = i;
    state->text[state->length++] = '\n';
    state->text[state->length] = 0;
}

/* ======================================================================
 * Launcher (start menu) and taskbar
 * ====================================================================== */

typedef struct launcher_entry {
    const char *label;
    const char *hint;
    void (*open)(void);
    gui_rect_t rect;
} launcher_entry_t;

static launcher_entry_t g_launcher_entries[] = {
    { "Terminal",   "shell and diagnostics", gui_app_terminal_open },
    { "Files",      "browse the ESP and RAM FS", gui_app_files_open },
    { "Editor",     "plain text editing", gui_app_editor_open },
    { "Settings",   "system preferences", gui_app_settings_open },
    { "Monitor",    "threads, devices, counters", gui_app_taskmanager_open },
    { "HVCI",       "code integrity status", gui_app_hvci_open },
    { "Calculator", "integer arithmetic", gui_app_calculator_open },
    { "About",      "version and credits", gui_app_about_open },
};

static i32 g_launcher_count = (i32)(sizeof(g_launcher_entries) /
                                    sizeof(g_launcher_entries[0]));
static bool_t g_launcher_visible = false;
static i32 g_hover_start = -1;

static void launcher_paint(gui_window_t *window)
{
    gui_surface_t *surface = window->content;
    const gui_theme_t *theme = gui_theme();
    gui_fill(surface, GUI_RGB(0x0D, 0x12, 0x22));
    gui_text(surface, 16, 12, "Applications", theme->text_dim, 1);
    gui_hline(surface, 12, surface->width - 12, 32, theme->border);

    for (i32 i = 0; i < g_launcher_count; i++) {
        i32 y = 40 + i * 44;
        gui_rect_t row = gui_rect(8, y, surface->width - 16, 40);
        if (i == g_hover_start)
            gui_fill_round_rect(surface, row, 6, theme->taskbar_hover);
        gui_fill_round_rect(surface, gui_rect(16, y + 8, 24, 24), 6,
                            theme->accent_dim);
        gui_text(surface, 52, y + 4, g_launcher_entries[i].label, theme->text, 1);
        gui_text(surface, 52, y + 22, g_launcher_entries[i].hint,
                 theme->text_dim, 1);
    }
}

static void launcher_mouse(gui_window_t *window, i32 x, i32 y, u8 buttons,
                           bool_t pressed)
{
    i32 index = (y - 40) / 44;
    if (y < 40 || index < 0 || index >= g_launcher_count)
        index = -1;
    g_hover_start = index;
    (void)x;
    if (pressed && (buttons & 1) && index >= 0) {
        g_launcher_entries[index].open();
        g_launcher_visible = false;
    }
}

void gui_app_launcher_open(void)
{
    if (g_launcher_visible)
        return;
    gui_window_t *window = gui_window_create("Launcher", 16, 200, 260, 400,
                                             launcher_paint);
    if (!window)
        return;
    window->on_mouse = launcher_mouse;
    window->resizable = false;
    g_launcher_visible = true;
}

void gui_taskbar_paint(gui_surface_t *surface)
{
    const gui_theme_t *theme = gui_theme();
    i32 height = gui_taskbar_height();
    i32 y = surface->height - height;

    gui_fill_rect(surface, gui_rect(0, y, surface->width, height), theme->taskbar);
    gui_hline(surface, 0, surface->width, y, theme->accent_dim);

    /* Start button. */
    gui_fill_round_rect(surface, gui_rect(8, y + 6, 60, 28), 6,
                        GUI_RGB(0x1B, 0x25, 0x40));
    gui_text(surface, 16, y + 12, "AXIOM", theme->accent, 1);

    /* Running windows. */
    i32 count = 0;
    gui_window_t **windows = gui_window_list(&count);
    i32 x = 80;
    for (i32 i = 0; i < count; i++) {
        gui_window_t *window = windows[i];
        i32 width = gui_text_width(window->title, 1) + 24;
        gui_rect_t button = gui_rect(x, y + 6, width, 28);
        gui_fill_round_rect(surface, button, 6,
                            window->focused ? theme->taskbar_hover
                                            : GUI_RGB(0x16, 0x1D, 0x30));
        gui_text(surface, x + 10, y + 12, window->title,
                 window->focused ? theme->text : theme->text_dim, 1);
        x += width + 6;
        if (x > surface->width - 220)
            break;
    }

    /* Clock and uptime on the right. */
    char time_buffer[24];
    rtc_format(time_buffer, sizeof(time_buffer));
    i32 width = gui_text_width(time_buffer, 1);
    gui_text(surface, surface->width - width - 14, y + 12, time_buffer,
             theme->text, 1);
    gui_text(surface, surface->width - 190, y + 12,
             hvci_active() ? "HVCI on" : "HVCI off",
             hvci_active() ? theme->success : theme->warning, 1);
}

bool_t gui_taskbar_click(i32 x, i32 y, i32 screen_height)
{
    i32 top = screen_height - gui_taskbar_height();
    if (y < top)
        return false;

    if (x < 70) {
        gui_app_launcher_open();
        return true;
    }

    /* Focus a running window by clicking its taskbar button. */
    i32 count = 0;
    gui_window_t **windows = gui_window_list(&count);
    i32 cursor = 80;
    for (i32 i = 0; i < count; i++) {
        i32 width = gui_text_width(windows[i]->title, 1) + 24;
        if (x >= cursor && x < cursor + width) {
            windows[i]->minimised = false;
            gui_window_focus(windows[i]);
            return true;
        }
        cursor += width + 6;
    }
    return true;
}

void gui_taskbar_hover(i32 x, i32 y)
{
    (void)x;
    (void)y;
}

void gui_apps_init(void)
{
    g_launcher_visible = false;
}
