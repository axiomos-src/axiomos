/* AxiomOS - Aurora: calculator application.
 *
 * A four function calculator with a scientific keypad, a tape of previous
 * results, memory keys and a small expression evaluator.  The evaluator is a
 * precedence climbing parser over a token buffer; everything is integer, with
 * a millesimal scale so that fractional results stay exact enough for the
 * display (three decimals, truncated).
 */

#include "gui.h"

#define AXCA_MAX_TOKENS  64
#define AXCA_MAX_HISTORY 16
#define AXCA_TAPE_ROWS   8
#define AXCA_KEY_HEIGHT  44
#define AXCA_DISPLAY     72
#define AXCA_MAX_DIGITS  16

#define AXCA_C_BG      GUI_RGB(0x18, 0x1A, 0x20)
#define AXCA_C_PANEL   GUI_RGB(0x22, 0x25, 0x2C)
#define AXCA_C_KEY     GUI_RGB(0x2C, 0x30, 0x39)
#define AXCA_C_KEY_H   GUI_RGB(0x38, 0x3D, 0x48)
#define AXCA_C_OP      GUI_RGB(0x3A, 0x44, 0x56)
#define AXCA_C_EQUALS  GUI_RGB(0x59, 0xA8, 0xFF)
#define AXCA_C_TEXT    GUI_RGB(0xE8, 0xEC, 0xF4)
#define AXCA_C_DIM     GUI_RGB(0x8A, 0x90, 0x9E)
#define AXCA_C_LINE    GUI_RGB(0x3A, 0x3E, 0x48)

enum axca_op
{
    AXCA_OP_NONE = 0,
    AXCA_OP_ADD,
    AXCA_OP_SUB,
    AXCA_OP_MUL,
    AXCA_OP_DIV,
    AXCA_OP_MOD,
    AXCA_OP_POW,
    AXCA_OP_LPAREN,
    AXCA_OP_RPAREN
};

typedef struct axca_token
{
    u8  op;
    bool_t is_number;
    i64 value;
} axca_token_t;

typedef struct axca_history
{
    char expression[32];
    char result[24];
} axca_history_t;

typedef struct axca_calc
{
    char           display[24];
    u16            display_length;
    bool_t         entry_active;
    bool_t         error;
    i64            accumulator;      /* scaled by 1000 */
    u8             pending_op;
    i64            memory;
    axca_token_t   tokens[AXCA_MAX_TOKENS];
    u16            token_count;
    axca_history_t history[AXCA_MAX_HISTORY];
    u16            history_count;
    bool_t         scientific;
    u8             hover_key;
    u8             pressed_key;
    u32            ticks;
    u32            computations;
    char           expression[48];
    u16            expression_length;
} axca_calc_t;

typedef struct axca_key
{
    const char *label;
    u8          op;
    u8          kind;      /* 0 digit, 1 operator, 2 action, 3 constant */
    u8          group;
} axca_key_t;

static void axca_zero(void *ptr, u32 size);
static void axca_set_display(axca_calc_t *calc, i64 scaled);
static void axca_append_digit(axca_calc_t *calc, char digit);
static void axca_apply_operator(axca_calc_t *calc, u8 op);
static i64  axca_current_value(const axca_calc_t *calc);
static i64  axca_parse(axca_calc_t *calc, bool_t *ok);
static i64  axca_parse_expression(axca_calc_t *calc, u16 *offset, bool_t *ok);
static i64  axca_parse_term(axca_calc_t *calc, u16 *offset, bool_t *ok);
static i64  axca_parse_power(axca_calc_t *calc, u16 *offset, bool_t *ok);
static i64  axca_parse_primary(axca_calc_t *calc, u16 *offset, bool_t *ok);
static bool_t axca_tokenize(axca_calc_t *calc, const char *text);
static i64  axca_ipow(i64 base, i64 exponent);
static void axca_format(char *out, i64 scaled);
static void axca_format_expression(char *out, u64 value);
static void axca_push_history(axca_calc_t *calc, const char *expression,
                              const char *result);
static void axca_key_geometry(i32 width, i32 height, u8 index, gui_rect_t *out,
                              const axca_key_t **key);
static void axca_paint_display(gui_surface_t *surface, const axca_calc_t *calc,
                               gui_rect_t rect);
static void axca_paint_keys(gui_surface_t *surface, axca_calc_t *calc,
                            gui_rect_t area);
static void axca_paint_tape(gui_surface_t *surface, const axca_calc_t *calc,
                            gui_rect_t tape);
static void axca_paint_memory(gui_surface_t *surface, const axca_calc_t *calc,
                              gui_rect_t bar);

static const axca_key_t axca_keys[24] = {
    { "7", 0, 0, 0 }, { "8", 0, 0, 0 }, { "9", 0, 0, 0 }, { "/", AXCA_OP_DIV, 1, 0 },
    { "4", 0, 0, 0 }, { "5", 0, 0, 0 }, { "6", 0, 0, 0 }, { "*", AXCA_OP_MUL, 1, 0 },
    { "1", 0, 0, 0 }, { "2", 0, 0, 0 }, { "3", 0, 0, 0 }, { "-", AXCA_OP_SUB, 1, 0 },
    { "0", 0, 0, 0 }, { ".", 0, 2, 0 }, { "=", 0, 2, 0 }, { "+", AXCA_OP_ADD, 1, 0 },
    { "C", 0, 2, 0 }, { "AC", 0, 2, 0 }, { "M+", 0, 2, 0 }, { "M-", 0, 2, 0 },
    { "pi", 0, 3, 0 }, { "e", 0, 3, 0 }, { "x2", AXCA_OP_POW, 1, 0 }, { "mod", AXCA_OP_MOD, 1, 0 }
};

static void axca_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static i64 axca_ipow(i64 base, i64 exponent)
{
    i64 result = 1000;
    i64 factor = base;
    if (exponent < 0)
        return 0;
    while (exponent > 0)
    {
        if (exponent & 1)
            result = result * factor / 1000;
        factor = factor * factor / 1000;
        exponent >>= 1;
    }
    return result;
}

static void axca_format(char *out, i64 scaled)
{
    char digits[24];
    u32 count = 0;
    u32 i;
    i64 value = scaled < 0 ? -scaled : scaled;
    i64 fraction = value % 1000;
    i64 integer = value / 1000;
    if (scaled < 0)
        out[0] = '-';
    else
        out[0] = ' ';
    {
        u32 offset = 1;
        do
        {
            digits[count] = (char)('0' + (integer % 10));
            integer /= 10;
            count++;
        } while (integer > 0 && count < 20);
        for (i = 0; i < count; i++)
            out[offset + i] = digits[count - 1 - i];
        offset += count;
        out[offset] = '.';
        out[offset + 1] = (char)('0' + (fraction / 100) % 10);
        out[offset + 2] = (char)('0' + (fraction / 10) % 10);
        out[offset + 3] = (char)('0' + fraction % 10);
        out[offset + 4] = 0;
    }
}

static void axca_format_expression(char *out, u64 value)
{
    char digits[24];
    u32 count = 0;
    u32 i;
    do
    {
        digits[count] = (char)('0' + (value % 10));
        value /= 10;
        count++;
    } while (value > 0 && count < 20);
    for (i = 0; i < count; i++)
        out[i] = digits[count - 1 - i];
    out[count] = 0;
}

static void axca_set_display(axca_calc_t *calc, i64 scaled)
{
    axca_format(calc->display, scaled);
    calc->display_length = 0;
    while (calc->display[calc->display_length] != 0)
        calc->display_length++;
    if (calc->display[0] == ' ')
    {
        u16 i;
        for (i = 0; i < calc->display_length; i++)
            calc->display[i] = calc->display[i + 1];
        calc->display_length--;
        calc->display[calc->display_length] = 0;
    }
}

static i64 axca_current_value(const axca_calc_t *calc)
{
    i64 value = 0;
    i64 fraction = 0;
    i64 divisor = 1;
    u16 i = 0;
    bool_t after_point = false;
    bool_t negative = false;
    if (calc->display_length == 0)
        return 0;
    if (calc->display[0] == '-')
    {
        negative = true;
        i = 1;
    }
    while (i < calc->display_length)
    {
        char c = calc->display[i];
        if (c == '.')
        {
            after_point = true;
            i++;
            continue;
        }
        if (c < '0' || c > '9')
        {
            i++;
            continue;
        }
        if (after_point)
        {
            if (divisor < 1000)
            {
                fraction += (i64)(c - '0') * (1000 / (divisor * 10));
                divisor *= 10;
            }
        }
        else
            value = value * 10 + (i64)(c - '0');
        i++;
    }
    value = value * 1000 + (fraction / 100) * 100 + ((fraction / 10) % 10) * 10 + fraction % 10;
    return negative ? -value : value;
}

static void axca_append_digit(axca_calc_t *calc, char digit)
{
    if (calc->error || !calc->entry_active)
    {
        calc->display[0] = 0;
        calc->display_length = 0;
    }
    if (calc->display_length >= AXCA_MAX_DIGITS)
        return;
    calc->display[calc->display_length] = digit;
    calc->display_length++;
    calc->display[calc->display_length] = 0;
    calc->entry_active = true;
}

static void axca_apply_operator(axca_calc_t *calc, u8 op)
{
    i64 value;
    if (calc->pending_op != AXCA_OP_NONE && calc->entry_active)
    {
        value = axca_current_value(calc);
        switch (calc->pending_op)
        {
        case AXCA_OP_ADD: calc->accumulator = calc->accumulator + value; break;
        case AXCA_OP_SUB: calc->accumulator = calc->accumulator - value; break;
        case AXCA_OP_MUL: calc->accumulator = calc->accumulator * value / 1000; break;
        case AXCA_OP_DIV:
            if (value == 0)
            {
                calc->error = true;
                calc->display[0] = 'E';
                calc->display[1] = 0;
                calc->display_length = 1;
                return;
            }
            calc->accumulator = calc->accumulator * 1000 / value;
            break;
        case AXCA_OP_MOD:
            if (value == 0)
            {
                calc->error = true;
                calc->display[0] = 'E';
                calc->display[1] = 0;
                calc->display_length = 1;
                return;
            }
            calc->accumulator = (calc->accumulator / 1000 % (value / 1000)) * 1000;
            break;
        case AXCA_OP_POW:
            calc->accumulator = axca_ipow(calc->accumulator, value / 1000);
            break;
        default:
            break;
        }
        axca_set_display(calc, calc->accumulator);
    }
    else if (!calc->entry_active)
    {
        calc->accumulator = axca_current_value(calc);
    }
    else
    {
        calc->accumulator = axca_current_value(calc);
    }
    calc->pending_op = op;
    calc->entry_active = false;
    calc->computations++;
}

static bool_t axca_tokenize(axca_calc_t *calc, const char *text)
{
    u16 i = 0;
    calc->token_count = 0;
    while (text[i] != 0 && calc->token_count < AXCA_MAX_TOKENS)
    {
        char c = text[i];
        if (c == ' ')
        {
            i++;
            continue;
        }
        if (c >= '0' && c <= '9')
        {
            i64 value = 0;
            while (text[i] >= '0' && text[i] <= '9')
            {
                value = value * 10 + (text[i] - '0');
                i++;
            }
            calc->tokens[calc->token_count].is_number = true;
            calc->tokens[calc->token_count].value = value * 1000;
            calc->tokens[calc->token_count].op = AXCA_OP_NONE;
            calc->token_count++;
            continue;
        }
        calc->tokens[calc->token_count].is_number = false;
        calc->tokens[calc->token_count].value = 0;
        switch (c)
        {
        case '+': calc->tokens[calc->token_count].op = AXCA_OP_ADD; break;
        case '-': calc->tokens[calc->token_count].op = AXCA_OP_SUB; break;
        case '*': calc->tokens[calc->token_count].op = AXCA_OP_MUL; break;
        case '/': calc->tokens[calc->token_count].op = AXCA_OP_DIV; break;
        case '%': calc->tokens[calc->token_count].op = AXCA_OP_MOD; break;
        case '^': calc->tokens[calc->token_count].op = AXCA_OP_POW; break;
        case '(': calc->tokens[calc->token_count].op = AXCA_OP_LPAREN; break;
        case ')': calc->tokens[calc->token_count].op = AXCA_OP_RPAREN; break;
        default: return false;
        }
        calc->token_count++;
        i++;
    }
    return true;
}

static i64 axca_parse_primary(axca_calc_t *calc, u16 *offset, bool_t *ok)
{
    axca_token_t *token;
    if (*offset >= calc->token_count)
    {
        *ok = false;
        return 0;
    }
    token = &calc->tokens[*offset];
    if (token->is_number)
    {
        (*offset)++;
        return token->value;
    }
    if (token->op == AXCA_OP_SUB)
    {
        (*offset)++;
        return -axca_parse_primary(calc, offset, ok);
    }
    if (token->op == AXCA_OP_LPAREN)
    {
        i64 value;
        (*offset)++;
        value = axca_parse_expression(calc, offset, ok);
        if (!*ok)
            return 0;
        if (*offset >= calc->token_count ||
            calc->tokens[*offset].op != AXCA_OP_RPAREN)
        {
            *ok = false;
            return 0;
        }
        (*offset)++;
        return value;
    }
    *ok = false;
    return 0;
}

static i64 axca_parse_power(axca_calc_t *calc, u16 *offset, bool_t *ok)
{
    i64 left = axca_parse_primary(calc, offset, ok);
    if (!*ok)
        return 0;
    while (*offset < calc->token_count &&
           calc->tokens[*offset].op == AXCA_OP_POW)
    {
        (*offset)++;
        {
            i64 right = axca_parse_primary(calc, offset, ok);
            if (!*ok)
                return 0;
            left = axca_ipow(left, right / 1000);
        }
    }
    return left;
}

static i64 axca_parse_term(axca_calc_t *calc, u16 *offset, bool_t *ok)
{
    i64 left = axca_parse_power(calc, offset, ok);
    if (!*ok)
        return 0;
    while (*offset < calc->token_count &&
           (calc->tokens[*offset].op == AXCA_OP_MUL ||
            calc->tokens[*offset].op == AXCA_OP_DIV ||
            calc->tokens[*offset].op == AXCA_OP_MOD))
    {
        u8 op = calc->tokens[*offset].op;
        (*offset)++;
        {
            i64 right = axca_parse_power(calc, offset, ok);
            if (!*ok)
                return 0;
            if (op == AXCA_OP_MUL)
                left = left * right / 1000;
            else if (op == AXCA_OP_DIV)
            {
                if (right == 0)
                {
                    *ok = false;
                    return 0;
                }
                left = left * 1000 / right;
            }
            else
            {
                if (right == 0)
                {
                    *ok = false;
                    return 0;
                }
                left = (left / 1000 % (right / 1000)) * 1000;
            }
        }
    }
    return left;
}

static i64 axca_parse_expression(axca_calc_t *calc, u16 *offset, bool_t *ok)
{
    i64 left = axca_parse_term(calc, offset, ok);
    if (!*ok)
        return 0;
    while (*offset < calc->token_count &&
           (calc->tokens[*offset].op == AXCA_OP_ADD ||
            calc->tokens[*offset].op == AXCA_OP_SUB))
    {
        u8 op = calc->tokens[*offset].op;
        (*offset)++;
        {
            i64 right = axca_parse_term(calc, offset, ok);
            if (!*ok)
                return 0;
            if (op == AXCA_OP_ADD)
                left += right;
            else
                left -= right;
        }
    }
    return left;
}

static i64 axca_parse(axca_calc_t *calc, bool_t *ok)
{
    u16 offset = 0;
    i64 value;
    *ok = true;
    if (!axca_tokenize(calc, calc->expression))
    {
        *ok = false;
        return 0;
    }
    value = axca_parse_expression(calc, &offset, ok);
    if (*ok && offset != calc->token_count)
        *ok = false;
    return value;
}

static void axca_push_history(axca_calc_t *calc, const char *expression,
                              const char *result)
{
    u16 i = 0;
    axca_history_t *entry;
    if (calc->history_count >= AXCA_MAX_HISTORY)
    {
        for (i = 0; i + 1 < AXCA_MAX_HISTORY; i++)
            calc->history[i] = calc->history[i + 1];
        calc->history_count = AXCA_MAX_HISTORY - 1;
    }
    entry = &calc->history[calc->history_count];
    i = 0;
    while (expression[i] != 0 && i < 31)
    {
        entry->expression[i] = expression[i];
        i++;
    }
    entry->expression[i] = 0;
    i = 0;
    while (result[i] != 0 && i < 23)
    {
        entry->result[i] = result[i];
        i++;
    }
    entry->result[i] = 0;
    calc->history_count++;
}

/* ------------------------------------------------------------- rendering */

static void axca_paint_display(gui_surface_t *surface, const axca_calc_t *calc,
                               gui_rect_t rect)
{
    gui_fill_rect(surface, rect, AXCA_C_PANEL);
    gui_rect_outline(surface, rect, AXCA_C_LINE);
    gui_text(surface, rect.x + 14, rect.y + 10, calc->expression, AXCA_C_DIM, 1);
    gui_text(surface, rect.x + rect.w - 18 - gui_text_width(calc->display, 3),
             rect.y + 30, calc->display, calc->error ? AXCA_C_EQUALS : AXCA_C_TEXT, 3);
    if (calc->pending_op != AXCA_OP_NONE)
    {
        const char *symbol = "?";
        switch (calc->pending_op)
        {
        case AXCA_OP_ADD: symbol = "+"; break;
        case AXCA_OP_SUB: symbol = "-"; break;
        case AXCA_OP_MUL: symbol = "*"; break;
        case AXCA_OP_DIV: symbol = "/"; break;
        case AXCA_OP_MOD: symbol = "%"; break;
        case AXCA_OP_POW: symbol = "^"; break;
        default: break;
        }
        gui_text(surface, rect.x + 12, rect.y + rect.h - 22, symbol,
                 AXCA_C_EQUALS, 1);
    }
}

static void axca_key_geometry(i32 width, i32 height, u8 index, gui_rect_t *out,
                              const axca_key_t **key)
{
    i32 columns = 4;
    i32 rows = 6;
    i32 gap = 8;
    i32 key_width = (width - gap * (columns + 1)) / columns;
    i32 key_height = (height - gap * (rows + 1)) / rows;
    u8 column = (u8)(index % 4);
    u8 row = (u8)(index / 4);
    *out = gui_rect(gap + (i32)column * (key_width + gap),
                    gap + (i32)row * (key_height + gap), key_width, key_height);
    *key = &axca_keys[index];
    if (key_height < AXCA_KEY_HEIGHT)
        out->h = key_height;
}

static void axca_paint_keys(gui_surface_t *surface, axca_calc_t *calc,
                            gui_rect_t area)
{
    u8 i;
    gui_fill_rect(surface, area, AXCA_C_BG);
    for (i = 0; i < 24; i++)
    {
        gui_rect_t rect;
        const axca_key_t *key;
        u32 colour;
        axca_key_geometry(area.w, area.h, i, &rect, &key);
        rect.x += area.x;
        rect.y += area.y;
        if (key->op == AXCA_OP_ADD || key->op == AXCA_OP_SUB ||
            key->op == AXCA_OP_MUL || key->op == AXCA_OP_DIV)
            colour = AXCA_C_OP;
        else if (key->kind == 2)
            colour = (key->label[0] == '=') ? AXCA_C_EQUALS : AXCA_C_KEY;
        else
            colour = AXCA_C_KEY;
        if (i == calc->hover_key)
            colour = AXCA_C_KEY_H;
        gui_fill_round_rect(surface, rect, 6, colour);
        gui_text(surface, rect.x + rect.w / 2 - gui_text_width(key->label, 2) / 2,
                 rect.y + rect.h / 2 - 8, key->label, AXCA_C_TEXT, 2);
    }
}

static void axca_paint_tape(gui_surface_t *surface, const axca_calc_t *calc,
                            gui_rect_t tape)
{
    u16 i;
    i32 y = tape.y + 8;
    gui_fill_rect(surface, tape, AXCA_C_PANEL);
    gui_text(surface, tape.x + 12, y, "Tape", AXCA_C_DIM, 1);
    y += 22;
    for (i = 0; i < calc->history_count; i++)
    {
        if (y + 30 > tape.y + tape.h)
            break;
        gui_text(surface, tape.x + 12, y, calc->history[i].expression,
                 AXCA_C_DIM, 1);
        gui_text(surface, tape.x + tape.w - 12 -
                 gui_text_width(calc->history[i].result, 1),
                 y, calc->history[i].result, AXCA_C_TEXT, 1);
        y += 20;
    }
}

static void axca_paint_memory(gui_surface_t *surface, const axca_calc_t *calc,
                              gui_rect_t bar)
{
    char buffer[24];
    gui_fill_rect(surface, bar, AXCA_C_PANEL);
    gui_hline(surface, bar.x, bar.x + bar.w, bar.y, AXCA_C_LINE);
    gui_text(surface, bar.x + 12, bar.y + 7, "M", AXCA_C_DIM, 1);
    axca_format(buffer, calc->memory);
    gui_text(surface, bar.x + 32, bar.y + 7, buffer, AXCA_C_TEXT, 1);
    gui_text(surface, bar.x + bar.w - 120, bar.y + 7, calc->scientific ?
             "scientific" : "standard", AXCA_C_DIM, 1);
}

/* -------------------------------------------------------------- public API */

void axca_init(axca_calc_t *calc)
{
    axca_zero(calc, (u32)sizeof(*calc));
    calc->display[0] = '0';
    calc->display[1] = 0;
    calc->display_length = 1;
    calc->entry_active = true;
    calc->pending_op = AXCA_OP_NONE;
}

void axca_type(axca_calc_t *calc, char c)
{
    if (calc->error)
    {
        calc->error = false;
        calc->display_length = 0;
        calc->display[0] = 0;
        calc->entry_active = false;
    }
    if (calc->expression_length < 47 && (c == '+' || c == '-' || c == '*' ||
                                         c == '/' || c == '(' || c == ')' ||
                                         c == '^' || c == '%'))
    {
        calc->expression[calc->expression_length] = c;
        calc->expression_length++;
        calc->expression[calc->expression_length] = 0;
        return;
    }
    if (c == '=')
    {
        bool_t ok = false;
        i64 value = axca_parse(calc, &ok);
        char result[24];
        if (!ok)
        {
            calc->error = true;
            calc->display[0] = 'E';
            calc->display[1] = 0;
            calc->display_length = 1;
            return;
        }
        axca_format(result, value);
        if (result[0] == ' ')
        {
            u16 i;
            for (i = 0; result[i] != 0; i++)
                result[i] = result[i + 1];
        }
        axca_set_display(calc, value);
        axca_push_history(calc, calc->expression, calc->display);
        calc->accumulator = value;
        calc->expression_length = 0;
        calc->expression[0] = 0;
        calc->entry_active = false;
        return;
    }
    if (c >= '0' && c <= '9')
    {
        axca_append_digit(calc, c);
        if (calc->expression_length < 46)
        {
            calc->expression[calc->expression_length] = c;
            calc->expression_length++;
            calc->expression[calc->expression_length] = 0;
        }
    }
}

void axca_press(axca_calc_t *calc, u8 index)
{
    const axca_key_t *key;
    if (index >= 24)
        return;
    key = &axca_keys[index];
    if (key->kind == 0)
    {
        axca_type(calc, key->label[0]);
        return;
    }
    if (key->kind == 1)
    {
        axca_apply_operator(calc, key->op);
        if (calc->expression_length < 47)
        {
            const char *symbol = "+";
            switch (key->op)
            {
            case AXCA_OP_SUB: symbol = "-"; break;
            case AXCA_OP_MUL: symbol = "*"; break;
            case AXCA_OP_DIV: symbol = "/"; break;
            case AXCA_OP_MOD: symbol = "%"; break;
            case AXCA_OP_POW: symbol = "^"; break;
            default: break;
            }
            calc->expression[calc->expression_length] = symbol[0];
            calc->expression_length++;
            calc->expression[calc->expression_length] = 0;
        }
        return;
    }
    if (key->kind == 1 || key->kind == 2)
    {
        if (key->label[0] == '=')
        {
            axca_type(calc, '=');
            return;
        }
        if (key->label[0] == 'C' && key->label[1] == 0)
        {
            calc->display[0] = '0';
            calc->display[1] = 0;
            calc->display_length = 1;
            calc->entry_active = true;
            calc->error = false;
            return;
        }
        if (key->label[0] == 'A')
        {
            axca_init(calc);
            return;
        }
        if (key->label[0] == 'M')
        {
            i64 value = axca_current_value(calc);
            if (key->label[1] == '+')
                calc->memory += value;
            else
                calc->memory -= value;
            return;
        }
        if (key->label[0] == '.')
        {
            if (calc->display_length < AXCA_MAX_DIGITS)
            {
                u16 i;
                for (i = 0; i < calc->display_length; i++)
                {
                    if (calc->display[i] == '.')
                        return;
                }
                if (!calc->entry_active)
                {
                    calc->display[0] = '0';
                    calc->display_length = 1;
                }
                calc->display[calc->display_length] = '.';
                calc->display_length++;
                calc->display[calc->display_length] = 0;
            }
            return;
        }
    }
    if (key->kind == 3)
    {
        if (key->label[0] == 'p')
            axca_set_display(calc, 3141);
        else
            axca_set_display(calc, 2718);
        calc->entry_active = false;
    }
}

void axca_hover(axca_calc_t *calc, i32 x, i32 y, i32 width, i32 height)
{
    u8 i;
    calc->hover_key = 0xFF;
    for (i = 0; i < 24; i++)
    {
        gui_rect_t rect;
        const axca_key_t *key;
        axca_key_geometry(width, height, i, &rect, &key);
        if (gui_rect_contains(rect, x, y))
        {
            calc->hover_key = i;
            return;
        }
    }
}

void axca_paint(axca_calc_t *calc, gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t display = gui_rect(bounds.x + 12, bounds.y + 12, bounds.w - 24, AXCA_DISPLAY);
    gui_rect_t keys = gui_rect(bounds.x + 12, display.y + display.h + 12,
                               bounds.w - 24,
                               bounds.h - display.h - 12 - 24 - AXCA_TAPE_ROWS * 20 - 24);
    gui_rect_t tape = gui_rect(bounds.x + bounds.w - 180, display.y, 168,
                               bounds.h - display.h - 36);
    gui_rect_t memory = gui_rect(bounds.x, bounds.y + bounds.h - 24, bounds.w, 24);
    gui_fill_rect(surface, bounds, AXCA_C_BG);
    axca_paint_display(surface, calc, display);
    axca_paint_keys(surface, calc, keys);
    axca_paint_tape(surface, calc, tape);
    axca_paint_memory(surface, calc, memory);
}

void axca_dump(const axca_calc_t *calc)
{
    printk("axca: display='%s' acc=%lld op=%u history=%u\n", calc->display,
           (long long)calc->accumulator, (unsigned)calc->pending_op,
           (unsigned)calc->history_count);
}

bool_t axca_selftest(void)
{
    axca_calc_t calc;
    axca_init(&calc);
    axca_type(&calc, '1');
    axca_type(&calc, '2');
    if (axca_current_value(&calc) != 12000)
        return false;
    axca_type(&calc, '+');
    axca_type(&calc, '3');
    axca_type(&calc, '=');
    if (axca_current_value(&calc) != 15000)
        return false;
    axca_init(&calc);
    axca_type(&calc, '2');
    axca_type(&calc, '*');
    axca_type(&calc, '3');
    axca_type(&calc, '+');
    axca_type(&calc, '4');
    axca_type(&calc, '=');
    if (axca_current_value(&calc) != 10000)
        return false;
    axca_init(&calc);
    axca_type(&calc, '1');
    axca_type(&calc, '/');
    axca_type(&calc, '0');
    axca_type(&calc, '=');
    if (!calc.error)
        return false;
    axca_init(&calc);
    axca_press(&calc, 16);
    axca_press(&calc, 4);
    axca_press(&calc, 15);
    axca_press(&calc, 14);
    if (calc.history_count != 1)
        return false;
    return true;
}

