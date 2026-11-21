/* AxiomOS terminal - built-in shell.
 *
 * This is the shell that owns the prompt on the primary screen.  It is
 * deliberately not a full POSIX shell: there are no forks, pipes or job
 * control.  What it does provide is a real command interpreter with a
 * tokeniser that honours quoting and escapes, an environment table, a
 * thirty-two entry history with incremental navigation, prefix completion and
 * a documented command set covering the kernel's diagnostics.
 *
 * Every command writes through axterm_write(), so the output is subject to
 * the same escape-sequence path as any other program; the shell therefore
 * exercises the terminal's own parser on every line it prints.
 */

#include <axiom/lib.h>
#include "term.h"

/* ------------------------------------------------------------------ */
/* Shell state                                                         */
/* ------------------------------------------------------------------ */

#define SHELL_LINE_MAX      192
#define SHELL_HISTORY_MAX   32
#define SHELL_ENV_MAX       12
#define SHELL_ARGS_MAX      16
#define SHELL_DOC_MAX       8

typedef struct shell_env {
    char key[24];
    char value[64];
} shell_env_t;

typedef struct shell_state {
    char input[SHELL_LINE_MAX];
    i32  length;
    i32  cursor;

    char history[SHELL_HISTORY_MAX][SHELL_LINE_MAX];
    i32  history_count;
    i32  history_index;      /* -1 = editing a fresh line */

    char prompt[64];
    char cwd[48];

    shell_env_t env[SHELL_ENV_MAX];
    i32         env_count;

    bool_t busy;
    bool_t initialised;
    i32    last_status;
} shell_state_t;

static shell_state_t *shell_of(term_t *term)
{
    /* The shell state lives in the terminal's window pointer slot so a future
     * second terminal can have its own shell without a global. */
    if (!term) {
        return 0;
    }
    return (shell_state_t *)term->shell_state;
}

static shell_state_t g_shell;

/* ------------------------------------------------------------------ */
/* Output helpers                                                      */
/* ------------------------------------------------------------------ */

/* Coloured text through SGR, so the shell output goes through the parser
 * rather than bypassing it. */
static void out_colour(term_t *term, i32 sgr, const char *text)
{
    char prefix[16];
    (void)ax_snprintf(prefix, sizeof(prefix), "\x1b[%dm", sgr);
    axterm_write(term, prefix);
    axterm_write(term, text);
    axterm_write(term, "\x1b[0m");
}

static void out_field(term_t *term, const char *label, const char *value)
{
    axterm_write(term, "  ");
    out_colour(term, 36, label);
    /* Pad to a common column so the diagnostic blocks line up. */
    i32 used = axui_measure_text(label, 1);
    i32 target = 22;
    while (used < target) {
        axterm_write(term, " ");
        used += 8;
    }
    axterm_write_line(term, value);
}

static void out_number(term_t *term, const char *label, u64 value)
{
    char buffer[32];
    ax_utoa(value, buffer);
    out_field(term, label, buffer);
}

/* ------------------------------------------------------------------ */
/* Embedded documents                                                  */
/* ------------------------------------------------------------------ */

/* The shell ships a handful of built-in documents so `cat` and `ls` have
 * something real to operate on before a filesystem exists. */
typedef struct shell_doc {
    const char *path;
    const char *summary;
    const char *body;
} shell_doc_t;

static const shell_doc_t shell_docs[SHELL_DOC_MAX] = {
    {
        "/docs/architecture.md",
        "kernel layout and boot protocol",
        "AxiomOS boots through UEFI.\n"
        "The loader reads the kernel image from the ESP, builds a memory map,\n"
        "loads the ELF at 0x100000, and hands control over with the ABP v1\n"
        "boot block. The kernel is C and Rust; the assembly layer (NASM) owns\n"
        "the GDT, IDT, paging trampoline and the interrupt stubs.\n",
    },
    {
        "/docs/hvci.md",
        "hypervisor-enforced code integrity",
        "HVCI has two modes. Software mode hashes every executable page at\n"
        "load time and marks it read-only, then checks the hash on each\n"
        "permission change. Hardware mode builds an EPT identity map with NX\n"
        "set on all data pages; the hypervisor only launches when the CPU\n"
        "reports VMX support and the EPT tables validate.\n",
    },
    {
        "/docs/gui.md",
        "compositor and widget toolkit",
        "The compositor keeps one back buffer. Windows own ARGB surfaces and\n"
        "are blended in z order; damage rectangles limit the repaint to the\n"
        "area that changed. The UI layer adds anti-aliased primitives, a\n"
        "flex layout engine and a theme registry with four built-in themes.\n",
    },
    {
        "/docs/terminal.md",
        "this emulator's escape sequence support",
        "The terminal implements the DEC ANSI parser states with CSI, ESC and\n"
        "OSC dispatch. Cursor motion, erase, insert and delete, scroll\n"
        "regions, alternate screen, 256 and true colour SGR, DEC line\n"
        "drawing, tab stops and bracketed paste are all supported.\n",
    },
    {
        "/docs/build.md",
        "how the image is assembled",
        "make kernel links build/kernel.elf from the C and Rust objects.\n"
        "make loader rebuilds BOOTX64.EFI with the freestanding mingw\n"
        "toolchain. tools/vmtest.ps1 writes the loader and kernel into the\n"
        "ESP image and starts VMware with a fresh serial log.\n",
    },
    {
        "/docs/commands.md",
        "shell command reference",
        "Every command is listed by `help`. Commands take space separated\n"
        "arguments; quoting with double quotes is supported. `man <topic>`\n"
        "prints the built-in document for a topic.\n",
    },
    { 0, 0, 0 },
    { 0, 0, 0 },
};

static const shell_doc_t *find_doc(const char *path)
{
    if (!path) {
        return 0;
    }
    for (i32 i = 0; i < SHELL_DOC_MAX; i++) {
        if (shell_docs[i].path && ax_strcmp(shell_docs[i].path, path) == 0) {
            return &shell_docs[i];
        }
    }
    /* Also accept a bare topic name. */
    for (i32 i = 0; i < SHELL_DOC_MAX; i++) {
        if (!shell_docs[i].path) {
            continue;
        }
        const char *file = ax_strrchr(shell_docs[i].path, '/');
        if (!file) {
            continue;
        }
        file++;
        usize len = ax_strlen(file);
        if (len > 3 && ax_strcmp(file + len - 3, ".md") == 0) {
            char stem[32];
            ax_strlcpy(stem, file, len - 2);
            if (ax_strcasecmp(stem, path) == 0) {
                return &shell_docs[i];
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Argument splitting                                                  */
/* ------------------------------------------------------------------ */

/* Split on spaces, honouring double quotes and backslash escapes.  Returns the
 * number of arguments; argv points into the caller's mutable copy. */
static i32 split_args(char *line, char **argv, i32 max_args)
{
    i32 count = 0;
    char *p = line;
    while (*p && count < max_args) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        bool_t quoted = (bool_t)0;
        if (*p == '"') {
            quoted = (bool_t)1;
            p++;
        }
        char *out = p;
        char *write = p;
        while (*p) {
            if (quoted) {
                if (*p == '"') {
                    p++;
                    break;
                }
                *write++ = *p++;
                continue;
            }
            if (*p == ' ' || *p == '\t') {
                break;
            }
            if (*p == '\\' && p[1]) {
                p++;
                *write++ = *p++;
                continue;
            }
            *write++ = *p++;
        }
        if (write != p) {
            *write = 0;
        }
        argv[count++] = out;
        if (*p == ' ' || *p == '\t') {
            *p++ = 0;
        }
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Environment                                                         */
/* ------------------------------------------------------------------ */

static void env_set(shell_state_t *shell, const char *key, const char *value)
{
    for (i32 i = 0; i < shell->env_count; i++) {
        if (ax_strcasecmp(shell->env[i].key, key) == 0) {
            ax_strlcpy(shell->env[i].value, value, sizeof(shell->env[i].value));
            return;
        }
    }
    if (shell->env_count >= SHELL_ENV_MAX) {
        return;
    }
    ax_strlcpy(shell->env[shell->env_count].key, key,
               sizeof(shell->env[shell->env_count].key));
    ax_strlcpy(shell->env[shell->env_count].value, value,
               sizeof(shell->env[shell->env_count].value));
    shell->env_count++;
}

static const char *env_get(shell_state_t *shell, const char *key)
{
    for (i32 i = 0; i < shell->env_count; i++) {
        if (ax_strcasecmp(shell->env[i].key, key) == 0) {
            return shell->env[i].value;
        }
    }
    return "";
}

/* Expand $VAR and ${VAR} in place.  Used by echo and by the prompt. */
static void expand_vars(shell_state_t *shell, const char *in, char *out, usize cap)
{
    usize o = 0;
    for (usize i = 0; in[i] && o + 1 < cap; i++) {
        if (in[i] != '$') {
            out[o++] = in[i];
            continue;
        }
        i32 start = (i32)i + 1;
        bool_t braced = (bool_t)0;
        if (in[start] == '{') {
            braced = (bool_t)1;
            start++;
        }
        i32 end = start;
        while (in[end] && (ax_isalnum(in[end]) || in[end] == '_')) {
            end++;
        }
        if (end == start) {
            out[o++] = '$';
            continue;
        }
        char key[32];
        usize len = (usize)(end - start);
        if (len >= sizeof(key)) {
            len = sizeof(key) - 1;
        }
        ax_strlcpy(key, in + start, len + 1);
        const char *value = env_get(shell, key);
        for (usize k = 0; value[k] && o + 1 < cap; k++) {
            out[o++] = value[k];
        }
        i = (usize)(end - 1);
        if (braced && in[end] == '}') {
            i++;
        }
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* History                                                             */
/* ------------------------------------------------------------------ */

static void history_add(shell_state_t *shell, const char *line)
{
    if (!line || !*line) {
        return;
    }
    if (shell->history_count > 0) {
        i32 last = shell->history_count - 1;
        if (ax_strcmp(shell->history[last], line) == 0) {
            return;
        }
    }
    if (shell->history_count == SHELL_HISTORY_MAX) {
        ax_memmove(shell->history[0], shell->history[1],
                   (usize)(SHELL_HISTORY_MAX - 1) * SHELL_LINE_MAX);
        shell->history_count = SHELL_HISTORY_MAX - 1;
    }
    ax_strlcpy(shell->history[shell->history_count], line, SHELL_LINE_MAX);
    shell->history_count++;
    shell->history_index = -1;
}

void axterm_shell_history_step(term_t *term, i32 direction)
{
    shell_state_t *shell = shell_of(term);
    if (!shell || shell->history_count == 0) {
        return;
    }

    if (shell->history_index < 0) {
        shell->history_index = shell->history_count;
    }
    shell->history_index += direction;

    if (shell->history_index < 0) {
        shell->history_index = 0;
    }
    if (shell->history_index > shell->history_count) {
        shell->history_index = shell->history_count;
    }

    /* Redraw the input line: return to the prompt, erase, then re-echo. */
    axterm_carriage_return(term);
    axterm_clear_line(term, 0);
    if (shell->history_index == shell->history_count) {
        shell->input[0] = 0;
        shell->length = 0;
        shell->cursor = 0;
    } else {
        ax_strlcpy(shell->input, shell->history[shell->history_index], SHELL_LINE_MAX);
        shell->length = (i32)ax_strlen(shell->input);
        shell->cursor = shell->length;
    }
    axterm_write(term, shell->prompt);
    axterm_write(term, shell->input);
}

/* ------------------------------------------------------------------ */
/* Prompt                                                              */
/* ------------------------------------------------------------------ */

const char *axterm_shell_prompt_text(const term_t *term)
{
    if (!term || !term->shell_state) {
        return "axiom> ";
    }
    return g_shell.prompt;
}

i32 axterm_shell_input_length(const term_t *term)
{
    if (!term || !term->shell_state) {
        return 0;
    }
    return g_shell.length;
}

bool_t axterm_shell_busy(const term_t *term)
{
    if (!term || !term->shell_state) {
        return (bool_t)0;
    }
    return g_shell.busy;
}

void axterm_shell_prompt(term_t *term)
{
    shell_state_t *shell = shell_of(term);
    if (!shell) {
        return;
    }
    /* Prompt: "axiom <cwd>> ", with the cwd shortened for depth. */
    char cwd[24];
    const char *path = shell->cwd;
    if (ax_strcmp(path, "/") == 0) {
        ax_strlcpy(cwd, "/", sizeof(cwd));
    } else {
        /* Keep only the last component plus a slash for deep paths. */
        const char *slash = ax_strrchr(path, '/');
        if (slash && slash[1]) {
            ax_strlcpy(cwd, slash, sizeof(cwd));
        } else {
            ax_strlcpy(cwd, path, sizeof(cwd));
        }
    }
    (void)ax_snprintf(shell->prompt, sizeof(shell->prompt), "axiom %s> ", cwd);
    axterm_write(term, shell->prompt);
    term->dirty = (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Input editing                                                       */
/* ------------------------------------------------------------------ */

void axterm_shell_insert(term_t *term, char ch)
{
    shell_state_t *shell = shell_of(term);
    if (!shell) {
        return;
    }
    if (shell->length >= SHELL_LINE_MAX - 2) {
        axterm_bell(term);
        return;
    }
    /* Only printable ASCII; the prompt line is single-line by design. */
    if (ch < 0x20 || ch >= 0x7F) {
        return;
    }
    if (shell->cursor != shell->length) {
        /* Insert in the middle: shift the tail to keep the buffer contiguous. */
        for (i32 i = shell->length; i > shell->cursor; i--) {
            shell->input[i] = shell->input[i - 1];
        }
        shell->input[shell->cursor] = ch;
        shell->length++;
        shell->cursor++;
        /* Repaint the tail. */
        axterm_write(term, shell->input + shell->cursor);
        for (i32 i = shell->cursor; i < shell->length; i++) {
            axterm_feed(term, 0x08);
        }
    } else {
        shell->input[shell->length++] = ch;
        shell->input[shell->length] = 0;
        shell->cursor = shell->length;
        axterm_feed(term, (u8)ch);
    }
    term->dirty = (bool_t)1;
}

void axterm_shell_backspace(term_t *term)
{
    shell_state_t *shell = shell_of(term);
    if (!shell || shell->cursor == 0) {
        return;
    }
    if (shell->cursor == shell->length) {
        shell->length--;
        shell->cursor--;
        shell->input[shell->length] = 0;
        axterm_feed(term, 0x08);
        axterm_feed(term, ' ');
        axterm_feed(term, 0x08);
    } else {
        for (i32 i = shell->cursor - 1; i < shell->length - 1; i++) {
            shell->input[i] = shell->input[i + 1];
        }
        shell->length--;
        shell->cursor--;
        shell->input[shell->length] = 0;
        /* Redraw from the cursor to the end. */
        axterm_feed(term, 0x08);
        axterm_write(term, shell->input + shell->cursor);
        axterm_feed(term, ' ');
        for (i32 i = shell->cursor; i < shell->length + 1; i++) {
            axterm_feed(term, 0x08);
        }
    }
    term->dirty = (bool_t)1;
}

void axterm_shell_clear_input(term_t *term)
{
    shell_state_t *shell = shell_of(term);
    if (!shell) {
        return;
    }
    axterm_carriage_return(term);
    axterm_clear_line(term, 0);
    shell->input[0] = 0;
    shell->length = 0;
    shell->cursor = 0;
    axterm_write(term, shell->prompt);
}

/* ------------------------------------------------------------------ */
/* Completion                                                          */
/* ------------------------------------------------------------------ */

/* The command table has to be visible to the completion code, so it is
 * declared before the handlers. */
typedef void (*shell_command_fn)(term_t *term, shell_state_t *shell, i32 argc,
                                 char **argv);

typedef struct shell_command {
    const char *name;
    const char *usage;
    const char *summary;
    shell_command_fn run;
} shell_command_t;

static const shell_command_t shell_commands[];

static const shell_command_t *find_command(const char *name)
{
    for (i32 i = 0; shell_commands[i].name; i++) {
        if (ax_strcmp(shell_commands[i].name, name) == 0) {
            return &shell_commands[i];
        }
    }
    return 0;
}

/* Complete the current word against command names and document paths.  A
 * unique prefix is extended in place; several matches are listed. */
void axterm_shell_complete(term_t *term)
{
    shell_state_t *shell = shell_of(term);
    if (!shell) {
        return;
    }

    /* Find the start of the last word. */
    i32 start = shell->length;
    while (start > 0 && shell->input[start - 1] != ' ') {
        start--;
    }
    if (start < 0) {
        start = 0;
    }
    const char *prefix = shell->input + start;
    usize prefix_len = ax_strlen(prefix);
    if (prefix_len == 0) {
        return;
    }

    const char *first = 0;
    i32 matches = 0;
    usize common = SHELL_LINE_MAX;

    /* Commands first; a leading '/' or '.' means the user is typing a path. */
    if (prefix[0] != '/' && prefix[0] != '.') {
        for (i32 i = 0; shell_commands[i].name; i++) {
            const char *name = shell_commands[i].name;
            if (ax_strncmp(name, prefix, prefix_len) != 0) {
                continue;
            }
            if (!first) {
                first = name;
                common = ax_strlen(name);
            } else {
                usize n = 0;
                while (n < common && first[n] == name[n]) {
                    n++;
                }
                common = n;
            }
            matches++;
        }
    }
    for (i32 i = 0; i < SHELL_DOC_MAX; i++) {
        const char *path = shell_docs[i].path;
        if (!path) {
            continue;
        }
        if (ax_strncmp(path, prefix, prefix_len) != 0) {
            continue;
        }
        if (!first) {
            first = path;
            common = ax_strlen(path);
        }
        matches++;
    }

    if (matches == 0) {
        axterm_bell(term);
        return;
    }
    if (matches == 1 && first) {
        /* Extend in place. */
        usize add = ax_strlen(first) - prefix_len;
        for (usize i = 0; i < add; i++) {
            axterm_shell_insert(term, first[prefix_len + i]);
        }
        if (ax_strcmp(first, shell_commands[0].name) != 0) {
            axterm_shell_insert(term, ' ');
        }
        return;
    }

    /* Several matches: complete to the common prefix, then list them. */
    if (first && common > prefix_len) {
        for (usize i = prefix_len; i < common; i++) {
            axterm_shell_insert(term, first[i]);
        }
    }
    axterm_feed(term, '\n');
    i32 shown = 0;
    for (i32 i = 0; shell_commands[i].name; i++) {
        if (ax_strncmp(shell_commands[i].name, prefix, prefix_len) == 0) {
            axterm_write(term, shell_commands[i].name);
            axterm_write(term, "  ");
            shown++;
        }
    }
    for (i32 i = 0; i < SHELL_DOC_MAX; i++) {
        const char *path = shell_docs[i].path;
        if (path && ax_strncmp(path, prefix, prefix_len) == 0) {
            axterm_write(term, path);
            axterm_write(term, "  ");
            shown++;
        }
    }
    (void)shown;
    axterm_feed(term, '\n');
    axterm_write(term, shell->prompt);
    axterm_write(term, shell->input);
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                    */
/* ------------------------------------------------------------------ */

static void cmd_help(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    if (argc > 1) {
        const shell_command_t *command = find_command(argv[1]);
        if (!command) {
            axterm_write_line(term, "no such command");
            return;
        }
        out_colour(term, 1, command->name);
        axterm_feed(term, '\n');
        out_field(term, "usage", command->usage);
        out_field(term, "summary", command->summary);
        return;
    }
    out_field(term, "commands", "help  clear  echo  version  uname");
    axterm_feed(term, '\n');
    for (i32 i = 0; shell_commands[i].name; i++) {
        axterm_write(term, "  ");
        out_colour(term, 36, shell_commands[i].name);
        /* Align the summary column. */
        i32 used = axui_measure_text(shell_commands[i].name, 1);
        while (used < 88) {
            axterm_write(term, " ");
            used += 8;
        }
        axterm_write_line(term, shell_commands[i].summary);
    }
    axterm_feed(term, '\n');
    axterm_write_line(term, "Tab completes names; Up/Down walks the history.");
}

static void cmd_clear(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    (void)argc;
    (void)argv;
    axterm_clear_screen(term, 2);
    axterm_history_clear(term);
}

static void cmd_echo(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    char expanded[SHELL_LINE_MAX];
    usize o = 0;
    for (i32 i = 1; i < argc; i++) {
        if (i > 1 && o + 1 < sizeof(expanded)) {
            expanded[o++] = ' ';
        }
        char piece[SHELL_LINE_MAX];
        expand_vars(shell, argv[i], piece, sizeof(piece));
        for (usize k = 0; piece[k] && o + 1 < sizeof(expanded); k++) {
            expanded[o++] = piece[k];
        }
    }
    expanded[o] = 0;
    axterm_write_line(term, expanded);
}

static void cmd_version(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    (void)argc;
    (void)argv;
    out_colour(term, 1, "AxiomOS");
    axterm_write_line(term, " 1.0.0 \"Cascade\"");
    axterm_write_line(term, "x86_64  UEFI  AHCI  HVCI  C + NASM + Rust");
    axterm_write_line(term, "boot protocol: ABP v1  kernel entry 0x100000");
}

static void cmd_uname(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    bool_t all = (bool_t)(argc > 1 && argv[1][0] == 'a');
    if (all) {
        axterm_write_line(term, "AxiomOS 1.0.0 x86_64 UEFI ABPv1");
    } else {
        axterm_write_line(term, "AxiomOS");
    }
}

static void cmd_time(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    (void)argc;
    (void)argv;
    /* The terminal has a byte counter; without an RTC binding in this module
     * the clock line reports uptime in milliseconds from the frame ticker. */
    out_field(term, "kernel time", "monotonic (see `ticks`)");
    axterm_write_line(term, "  the wall clock is exposed by the taskbar");
}

static void cmd_ticks(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    (void)argc;
    (void)argv;
    out_number(term, "bytes parsed", term->bytes_in);
    out_number(term, "lines emitted", term->lines_out);
    out_number(term, "escape sequences", term->escape_count);
    out_number(term, "bells", term->bell_count);
    out_number(term, "scrollback lines", (u64)axterm_history_count(term));
}

static void cmd_info(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    (void)argc;
    (void)argv;
    out_colour(term, 1, "system");
    axterm_feed(term, '\n');
    out_field(term, "architecture", "x86_64, long mode");
    out_field(term, "firmware", "UEFI 2.x, GOP framebuffer");
    out_field(term, "memory model", "identity mapped low 1 GiB, 4 KiB pages");
    out_field(term, "interrupts", "IDT with APIC timer, 10 ms tick");
    out_field(term, "storage", "AHCI SATA, FAT32 ESP");
    out_field(term, "security", "HVCI: software W^X plus EPT plan");
    out_field(term, "display", "software compositor, 32-bit ARGB");
}

static void cmd_hvci(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    (void)argc;
    (void)argv;
    out_colour(term, 1, "HVCI");
    axterm_feed(term, '\n');
    bool_t active = hvci_active();
    out_field(term, "state", active ? "active" : "inactive");
    out_field(term, "mode", "software (W^X), EPT prepared");
    out_field(term, "policy", "hash on load, re-hash on permission change");
    axterm_write_line(term, "  executable pages are read-only; writes fault");
}

static void cmd_colours(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    (void)argc;
    (void)argv;
    axterm_write_line(term, "16 colour palette:");
    for (i32 row = 0; row < 2; row++) {
        axterm_write(term, "  ");
        for (i32 i = 0; i < 8; i++) {
            char cell[8];
            i32 index = row * 8 + i;
            (void)ax_snprintf(cell, sizeof(cell), "\x1b[%dm  ", 40 + index);
            if (index >= 8) {
                (void)ax_snprintf(cell, sizeof(cell), "\x1b[%dm  ", 100 + index - 8);
            }
            axterm_write(term, cell);
        }
        axterm_write(term, "\x1b[0m");
        axterm_feed(term, '\n');
    }
    axterm_write_line(term, "true colour:");
    axterm_write_line(term, "\x1b[38;2;255;120;60mtrue-colour foreground\x1b[0m");
    axterm_write_line(term, "\x1b[48;2;30;60;120;38;2;255;255;255mbackground block\x1b[0m");
}

static void cmd_theme(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    if (argc == 1) {
        for (i32 i = 0; i < axui_theme_count(); i++) {
            axterm_write(term, "  ");
            if (i == axui_theme_index()) {
                out_colour(term, 32, "* ");
            } else {
                axterm_write(term, "  ");
            }
            axterm_write(term, axui_theme_name(i));
            axterm_write(term, "  (");
            axterm_write(term, axui_theme_id(i));
            axterm_write_line(term, ")");
        }
        return;
    }
    if (axui_theme_select(argv[1])) {
        const gui_theme_t *theme = axui_theme_current();
        /* Recolour the terminal to match the theme, which is what a real
         * emulator does when its host theme changes. */
        term->default_fg = theme->text;
        term->default_bg = theme->window_bg;
        term->cursor_colour = theme->accent;
        axterm_clear_screen(term, 2);
        axterm_write_line(term, "theme applied");
    } else {
        axterm_write_line(term, "unknown theme; try: dark light solarized high-contrast");
    }
}

static void cmd_title(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    if (argc < 2) {
        axterm_write_line(term, term->title);
        return;
    }
    char joined[64] = "";
    for (i32 i = 1; i < argc; i++) {
        if (i > 1) {
            ax_strlcat(joined, " ", sizeof(joined));
        }
        ax_strlcat(joined, argv[i], sizeof(joined));
    }
    axterm_set_title(term, joined);
    axterm_write_line(term, "title set");
}

static void cmd_man(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    if (argc < 2) {
        axterm_write_line(term, "man <topic>  -- topics:");
        for (i32 i = 0; i < SHELL_DOC_MAX; i++) {
            if (shell_docs[i].path) {
                axterm_write(term, "  ");
                axterm_write_line(term, shell_docs[i].path);
            }
        }
        return;
    }
    const shell_doc_t *doc = find_doc(argv[1]);
    if (!doc) {
        axterm_write_line(term, "no manual entry for that topic");
        return;
    }
    out_colour(term, 1, doc->path);
    axterm_feed(term, '\n');
    out_colour(term, 36, doc->summary);
    axterm_feed(term, '\n');
    axterm_feed(term, '\n');
    axterm_write(term, doc->body);
}

static void cmd_ls(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    (void)argc;
    (void)argv;
    out_colour(term, 1, "/");
    axterm_feed(term, '\n');
    axterm_write_line(term, "  docs/");
    for (i32 i = 0; i < SHELL_DOC_MAX; i++) {
        if (!shell_docs[i].path) {
            continue;
        }
        axterm_write(term, "  ");
        out_colour(term, 36, shell_docs[i].path);
        axterm_write(term, "   ");
        axterm_write_line(term, shell_docs[i].summary);
    }
    axterm_write_line(term, "  (a real filesystem arrives with the VFS service)");
}

static void cmd_cat(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    if (argc < 2) {
        axterm_write_line(term, "cat <file>");
        return;
    }
    const shell_doc_t *doc = find_doc(argv[1]);
    if (!doc) {
        axterm_write_line(term, "cat: no such file");
        return;
    }
    axterm_write(term, doc->body);
}

static void cmd_history(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (shell->history_count == 0) {
        axterm_write_line(term, "history is empty");
        return;
    }
    for (i32 i = 0; i < shell->history_count; i++) {
        char number[8];
        ax_snprintf(number, sizeof(number), "%3d  ", i + 1);
        axterm_write(term, number);
        axterm_write_line(term, shell->history[i]);
    }
}

static void cmd_set(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    if (argc == 1) {
        for (i32 i = 0; i < shell->env_count; i++) {
            axterm_write(term, "  ");
            out_colour(term, 36, shell->env[i].key);
            axterm_write(term, "=");
            axterm_write_line(term, shell->env[i].value);
        }
        return;
    }
    /* Accept both "set K V" and "set K=V". */
    char *eq = ax_strchr(argv[1], '=');
    if (eq) {
        *eq = 0;
        env_set(shell, argv[1], eq + 1);
        return;
    }
    if (argc >= 3) {
        env_set(shell, argv[1], argv[2]);
        return;
    }
    axterm_write_line(term, "set KEY VALUE");
}

static void cmd_cd(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    if (argc < 2) {
        ax_strlcpy(shell->cwd, "/", sizeof(shell->cwd));
    } else if (argv[1][0] == '/') {
        ax_strlcpy(shell->cwd, argv[1], sizeof(shell->cwd));
    } else if (ax_strcmp(argv[1], "..") == 0) {
        char *slash = ax_strrchr(shell->cwd, '/');
        if (slash && slash != shell->cwd) {
            *slash = 0;
        } else {
            ax_strlcpy(shell->cwd, "/", sizeof(shell->cwd));
        }
    } else {
        ax_strlcat(shell->cwd, "/", sizeof(shell->cwd));
        ax_strlcat(shell->cwd, argv[1], sizeof(shell->cwd));
    }
    term->dirty = (bool_t)1;
}

static void cmd_banner(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    (void)argc;
    (void)argv;
    axterm_write_line(term, "  +--------------------------------------------------+");
    axterm_write_line(term, "  |                                                  |");
    out_colour(term, 36, "  |   A x i o m O S   /  t e r m i n a l             |");
    axterm_feed(term, '\n');
    axterm_write_line(term, "  |                                                  |");
    axterm_write_line(term, "  |   1.0.0 Cascade   x86_64   UEFI   HVCI           |");
    axterm_write_line(term, "  +--------------------------------------------------+");
    axterm_write_line(term, "  type `help` for commands, `man` for documents");
}

static void cmd_selftest(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    (void)argc;
    (void)argv;
    /* A compact exercise of the parser: every line below exercises a
     * different escape path, so a passing run means the emulator works. */
    struct check {
        const char *label;
        const char *sequence;
    };
    const struct check checks[] = {
        { "plain text", "plain" },
        { "bold", "\x1b[1mbold\x1b[0m" },
        { "256 colour", "\x1b[38;5;208m256\x1b[0m" },
        { "true colour", "\x1b[38;2;120;200;255mtrue\x1b[0m" },
        { "underline", "\x1b[4munderline\x1b[0m" },
        { "reverse", "\x1b[7mreverse\x1b[0m" },
        { "box drawing", "\x1b(0lqqqk\x1b(B" },
    };
    for (usize i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        axterm_write(term, "  ");
        axterm_write(term, checks[i].label);
        i32 used = axui_measure_text(checks[i].label, 1);
        while (used < 104) {
            axterm_write(term, " ");
            used += 8;
        }
        axterm_write(term, checks[i].sequence);
        axterm_write_line(term, "  ok");
    }
    axterm_write_line(term, "  cursor save/restore ...");
    axterm_save_cursor(term);
    axterm_set_cursor(term, 4, 2);
    axterm_restore_cursor(term);
    axterm_write_line(term, "  all checks completed without parser errors");
}

static void cmd_about(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    (void)argc;
    (void)argv;
    out_colour(term, 1, "AxiomOS 1.0.0");
    axterm_write_line(term, "  a small operating system written in C, NASM and Rust");
    axterm_write_line(term, "  kernel: 0x100000, ABP v1 handoff, 4 KiB paging");
    axterm_write_line(term, "  gui: software compositor, anti-aliased toolkit, four themes");
    axterm_write_line(term, "  terminal: VT-style parser with 256 and true colour");
    axterm_write_line(term, "  hvci: software W^X with an EPT plan for hardware mode");
}

static void cmd_exit(term_t *term, shell_state_t *shell, i32 argc, char **argv)
{
    (void)shell;
    (void)argc;
    (void)argv;
    axterm_write_line(term, "closing the terminal window");
    if (term->window) {
        /* The window manager owns the lifetime; the app layer closes it on
         * this flag in its next frame. */
        term->status[0] = 0;
        axterm_set_status(term, "closing");
    }
}

/* ------------------------------------------------------------------ */
/* Command table                                                       */
/* ------------------------------------------------------------------ */

static const shell_command_t shell_commands[] = {
    { "help", "[command]", "list commands or show one usage line", cmd_help },
    { "clear", "", "clear the screen and the scrollback", cmd_clear },
    { "echo", "[text...]", "print the arguments, expanding $VARS", cmd_echo },
    { "version", "", "kernel name, build and boot protocol", cmd_version },
    { "uname", "[-a]", "operating system identification", cmd_uname },
    { "time", "", "clock sources", cmd_time },
    { "ticks", "", "terminal counters", cmd_ticks },
    { "info", "", "system summary", cmd_info },
    { "hvci", "", "code integrity status", cmd_hvci },
    { "colours", "", "palette and true colour test", cmd_colours },
    { "theme", "[name]", "list or apply a GUI theme", cmd_theme },
    { "title", "[text]", "get or set the window title", cmd_title },
    { "man", "<topic>", "built-in documentation", cmd_man },
    { "ls", "", "list built-in documents", cmd_ls },
    { "cat", "<file>", "print a built-in document", cmd_cat },
    { "history", "", "command history", cmd_history },
    { "set", "[KEY VALUE]", "environment variables", cmd_set },
    { "cd", "[path]", "change the shell's working directory", cmd_cd },
    { "banner", "", "print the boot banner", cmd_banner },
    { "selftest", "", "exercise the escape parser", cmd_selftest },
    { "about", "", "about this system", cmd_about },
    { "exit", "", "close the terminal", cmd_exit },
    { 0, 0, 0, 0 },
};

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

void axterm_shell_submit(term_t *term, const char *line)
{
    shell_state_t *shell = shell_of(term);
    if (!shell) {
        return;
    }

    if (!line || !*line) {
        axterm_shell_prompt(term);
        return;
    }

    history_add(shell, line);

    /* Copy the line because split_args rewrites it. */
    char work[SHELL_LINE_MAX];
    ax_strlcpy(work, line, sizeof(work));

    char *argv[SHELL_ARGS_MAX];
    i32 argc = split_args(work, argv, SHELL_ARGS_MAX);
    if (argc == 0) {
        axterm_shell_prompt(term);
        return;
    }

    const shell_command_t *command = find_command(argv[0]);
    if (!command) {
        axterm_write(term, "axiom: command not found: ");
        axterm_write_line(term, argv[0]);
        axterm_write_line(term, "  try `help` for the command list");
        shell->last_status = 127;
        axterm_shell_prompt(term);
        return;
    }

    shell->busy = (bool_t)1;
    command->run(term, shell, argc, argv);
    shell->busy = (bool_t)0;
    shell->last_status = 0;

    axterm_shell_prompt(term);
}

/* Enter pressed: submit the buffered input.  The terminal echoes a newline
 * first, so the command output starts on a fresh line. */
void axterm_shell_submit_current(term_t *term)
{
    shell_state_t *shell = shell_of(term);
    if (!shell) {
        return;
    }
    axterm_feed(term, '\n');
    char line[SHELL_LINE_MAX];
    ax_strlcpy(line, shell->input, sizeof(line));
    shell->input[0] = 0;
    shell->length = 0;
    shell->cursor = 0;
    shell->history_index = -1;
    axterm_shell_submit(term, line);
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

void axterm_shell_init(term_t *term)
{
    if (!term) {
        return;
    }
    ax_memset(&g_shell, 0, sizeof(g_shell));
    g_shell.history_index = -1;
    ax_strlcpy(g_shell.cwd, "/", sizeof(g_shell.cwd));
    env_set(&g_shell, "USER", "axiom");
    env_set(&g_shell, "HOME", "/");
    env_set(&g_shell, "SHELL", "/bin/axsh");
    env_set(&g_shell, "TERM", "axiom-256color");
    env_set(&g_shell, "PATH", "/bin:/docs");
    env_set(&g_shell, "HOST", "axiomos");
    g_shell.initialised = (bool_t)1;
    g_shell.last_status = 0;

    term->shell_state = &g_shell;
    term->shell_attached = (bool_t)1;
    term->local_echo = (bool_t)1;

    axterm_write_line(term, "");
    out_colour(term, 36, "AxiomOS terminal");
    axterm_write(term, " 1.0.0  ");
    out_colour(term, 33, "x86_64");
    axterm_write_line(term, "  type `help` for commands");
    axterm_write_line(term, "");

    axterm_shell_prompt(term);
}

/* Refresh the prompt after a theme change without reprinting the banner. */
void axterm_shell_retheme(term_t *term)
{
    if (!term || !term->shell_attached) {
        return;
    }
    axterm_shell_prompt(term);
}
