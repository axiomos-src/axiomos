/* AxiomOS - Aurora: spreadsheet application.
 *
 * Grid of 26 columns (A..Z) and 128 rows, integer cell storage, a small
 * formula language and an evaluator with a value cache.  Everything is
 * statically sized: a sheet is about 48 KiB, which fits comfortably in the
 * GUI pool used by the shell.
 *
 * Formula language (cell beginning with '='):
 *   =1+2*3              arithmetic with + - * / and parentheses
 *   =A1+B2              cell references, optionally absolute with '$'
 *   =SUM(A1:A9)         range aggregates: SUM, AVG, MIN, MAX, COUNT
 *   =-A1                unary minus
 *   =A1&" text"         left wins, text concatenation is not supported yet
 *
 * Errors are reported as a single '#ERR' marker so that dependent cells do
 * not recurse forever; the evaluator carries a depth guard and a cycle guard.
 */

#include "gui.h"

#define AXSH_MAX_COLS      26
#define AXSH_MAX_ROWS      128
#define AXSH_CELL_TEXT     48
#define AXSH_FORMULA_TEXT  96
#define AXSH_COL_WIDTH     88
#define AXSH_ROW_HEIGHT    20
#define AXSH_HEAD_HEIGHT   24
#define AXSH_FORMULA_BAR   30
#define AXSH_MAX_TOKENS    64
#define AXSH_EVAL_DEPTH    24
#define AXSH_NAME_LENGTH   24

#define AXSH_C_BG        GUI_RGB(0x1C, 0x1D, 0x22)
#define AXSH_C_HEADER    GUI_RGB(0x25, 0x27, 0x2E)
#define AXSH_C_GRID      GUI_RGB(0x33, 0x35, 0x3E)
#define AXSH_C_TEXT      GUI_RGB(0xD8, 0xDA, 0xE2)
#define AXSH_C_DIM       GUI_RGB(0x84, 0x88, 0x96)
#define AXSH_C_SEL       GUI_RGB(0x2A, 0x3A, 0x58)
#define AXSH_C_SEL_FILL  GUI_RGB(0x22, 0x2C, 0x40)
#define AXSH_C_ACCENT    GUI_RGB(0x59, 0xA8, 0xFF)
#define AXSH_C_EDIT      GUI_RGB(0x2B, 0x30, 0x40)
#define AXSH_C_ERROR     GUI_RGB(0xE0, 0x60, 0x60)
#define AXSH_C_HEADER_SEL GUI_RGB(0x3A, 0x44, 0x58)

/* A cell holds either literal text or a formula.  Values are cached after the
 * last recalculation so painting does not re-evaluate the sheet per frame. */
typedef struct axsh_cell
{
    char  text[AXSH_CELL_TEXT];
    char  formula[AXSH_FORMULA_TEXT];
    i64   value;
    bool_t has_formula;
    bool_t numeric;
    bool_t error;
    bool_t cached;
    u8    dirty;
} axsh_cell_t;

typedef struct axsh_sheet
{
    axsh_cell_t cells[AXSH_MAX_ROWS][AXSH_MAX_COLS];
    u16         column_width[AXSH_MAX_COLS];
    u16         row_height[AXSH_MAX_ROWS];
    u16         cursor_row;
    u16         cursor_column;
    u16         anchor_row;
    u16         anchor_column;
    bool_t      selecting;
    i32         scroll_x;
    i32         scroll_y;
    i32         view_width;
    i32         view_height;
    bool_t      editing;
    char        edit_buffer[AXSH_FORMULA_TEXT];
    u16         edit_length;
    char        name[AXSH_NAME_LENGTH];
    u32         recalc_stamp;
    u32         edits;
    u32         evaluations;
    u32         depth;
} axsh_sheet_t;

/* Byte code produced by the formula tokenizer. */
enum axsh_op
{
    AXSH_OP_NUMBER = 0,
    AXSH_OP_REF,
    AXSH_OP_PLUS,
    AXSH_OP_MINUS,
    AXSH_OP_MUL,
    AXSH_OP_DIV,
    AXSH_OP_LPAREN,
    AXSH_OP_RPAREN,
    AXSH_OP_FUNC,
    AXSH_OP_END
};

typedef struct axsh_token
{
    u8   kind;
    u8   func;          /* 0 SUM, 1 AVG, 2 MIN, 3 MAX, 4 COUNT */
    u16  row;
    u16  column;
    u16  row2;          /* range end for functions */
    u16  column2;
    i64  number;
    bool_t absolute;
} axsh_token_t;

typedef struct axsh_parser
{
    axsh_token_t tokens[AXSH_MAX_TOKENS];
    u16          count;
    u16          position;
    bool_t       failed;
} axsh_parser_t;

static void axsh_zero_bytes(void *ptr, u32 size);
static i32  axsh_column_x(const axsh_sheet_t *sheet, u16 column);
static i32  axsh_row_y(const axsh_sheet_t *sheet, u16 row);
static u16  axsh_column_at(const axsh_sheet_t *sheet, i32 x);
static u16  axsh_row_at(const axsh_sheet_t *sheet, i32 y);
static void axsh_column_name(u16 column, char *out);
static void axsh_reference_name(u16 row, u16 column, char *out);
static bool_t axsh_parse_reference(const char *text, u16 *length, u16 *row,
                                   u16 *column, bool_t *absolute);
static bool_t axsh_parse_number(const char *text, u16 *length, i64 *value);
static bool_t axsh_parse_formula(axsh_sheet_t *sheet, const char *formula,
                                 axsh_parser_t *parser);
static bool_t axsh_parse_range(axsh_sheet_t *sheet, const char *text,
                               u16 *length, u16 *row, u16 *column,
                               u16 *row2, u16 *column2, bool_t *absolute);
static i64  axsh_eval_expression(axsh_sheet_t *sheet, axsh_parser_t *parser,
                                 u16 *offset, bool_t *ok);
static i64  axsh_eval_term(axsh_sheet_t *sheet, axsh_parser_t *parser,
                           u16 *offset, bool_t *ok);
static i64  axsh_eval_factor(axsh_sheet_t *sheet, axsh_parser_t *parser,
                             u16 *offset, bool_t *ok);
static i64  axsh_eval_cell(axsh_sheet_t *sheet, u16 row, u16 column, bool_t *ok);
static i64  axsh_eval_function(axsh_sheet_t *sheet, const axsh_token_t *token,
                               bool_t *ok);
static i64  axsh_cell_value(const axsh_sheet_t *sheet, u16 row, u16 column);
static void axsh_set_text(axsh_sheet_t *sheet, u16 row, u16 column,
                          const char *text);
static void axsh_recalculate(axsh_sheet_t *sheet, u16 row, u16 column);
static void axsh_recalculate_all(axsh_sheet_t *sheet);
static void axsh_invalidate(axsh_sheet_t *sheet);
static void axsh_begin_edit(axsh_sheet_t *sheet);
static void axsh_commit_edit(axsh_sheet_t *sheet);
static void axsh_cancel_edit(axsh_sheet_t *sheet);
static void axsh_edit_paint(gui_surface_t *surface, const axsh_sheet_t *sheet,
                            gui_rect_t bar);
static void axsh_paint_headers(gui_surface_t *surface, const axsh_sheet_t *sheet,
                               gui_rect_t grid, i32 origin_x);
static void axsh_paint_grid(gui_surface_t *surface, axsh_sheet_t *sheet,
                            gui_rect_t grid, i32 origin_x, i32 origin_y);
static void axsh_paint_selection(gui_surface_t *surface,
                                 const axsh_sheet_t *sheet, gui_rect_t grid,
                                 i32 origin_x, i32 origin_y);
static void axsh_paint_formula_bar(gui_surface_t *surface,
                                   const axsh_sheet_t *sheet, gui_rect_t bar);
static void axsh_paint_cell_text(gui_surface_t *surface, const axsh_cell_t *cell,
                                 i32 x, i32 y, i32 width, u32 colour);
static i64  axsh_abs(i64 v);

static void axsh_zero_bytes(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static i64 axsh_abs(i64 v)
{
    return v < 0 ? -v : v;
}

static i32 axsh_column_x(const axsh_sheet_t *sheet, u16 column)
{
    i32 x = 0;
    u16 i;
    for (i = 0; i < column && i < AXSH_MAX_COLS; i++)
        x += sheet->column_width[i];
    return x;
}

static i32 axsh_row_y(const axsh_sheet_t *sheet, u16 row)
{
    i32 y = 0;
    u16 i;
    for (i = 0; i < row && i < AXSH_MAX_ROWS; i++)
        y += sheet->row_height[i];
    return y;
}

static u16 axsh_column_at(const axsh_sheet_t *sheet, i32 x)
{
    i32 cursor = 0;
    u16 i;
    for (i = 0; i < AXSH_MAX_COLS; i++)
    {
        cursor += sheet->column_width[i];
        if (x < cursor)
            return i;
    }
    return AXSH_MAX_COLS - 1;
}

static u16 axsh_row_at(const axsh_sheet_t *sheet, i32 y)
{
    i32 cursor = 0;
    u16 i;
    for (i = 0; i < AXSH_MAX_ROWS; i++)
    {
        cursor += sheet->row_height[i];
        if (y < cursor)
            return i;
    }
    return AXSH_MAX_ROWS - 1;
}

static void axsh_column_name(u16 column, char *out)
{
    if (column >= AXSH_MAX_COLS)
        column = AXSH_MAX_COLS - 1;
    out[0] = (char)('A' + column);
    out[1] = 0;
}

static void axsh_reference_name(u16 row, u16 column, char *out)
{
    i32 digits = 1;
    u32 value = row + 1;
    i32 i;
    while (value >= 10)
    {
        value /= 10;
        digits++;
    }
    out[0] = (char)('A' + (column % AXSH_MAX_COLS));
    value = row + 1;
    for (i = digits; i > 0; i--)
    {
        out[i] = (char)('0' + (value % 10));
        value /= 10;
    }
    out[digits + 1] = 0;
}

/* ------------------------------------------------------------- tokenizing */

static bool_t axsh_parse_number(const char *text, u16 *length, i64 *value)
{
    u16 i = 0;
    i64 sign = 1;
    i64 result = 0;
    bool_t digits = false;
    if (text[i] == '-')
    {
        sign = -1;
        i++;
    }
    while (text[i] >= '0' && text[i] <= '9')
    {
        result = result * 10 + (i64)(text[i] - '0');
        digits = true;
        i++;
    }
    if (!digits)
        return false;
    *value = result * sign;
    *length = i;
    return true;
}

static bool_t axsh_parse_reference(const char *text, u16 *length, u16 *row,
                                   u16 *column, bool_t *absolute)
{
    u16 i = 0;
    u16 parsed_column;
    u16 parsed_row = 0;
    bool_t abs_col = false;
    bool_t abs_row = false;
    bool_t digits = false;
    if (text[i] == '$')
    {
        abs_col = true;
        i++;
    }
    if (text[i] < 'A' || text[i] > 'Z')
        return false;
    parsed_column = (u16)(text[i] - 'A');
    i++;
    if (text[i] == '$')
    {
        abs_row = true;
        i++;
    }
    while (text[i] >= '0' && text[i] <= '9')
    {
        parsed_row = (u16)(parsed_row * 10 + (text[i] - '0'));
        digits = true;
        i++;
    }
    if (!digits)
        return false;
    if (parsed_row == 0)
        return false;
    parsed_row = (u16)(parsed_row - 1);
    if (parsed_row >= AXSH_MAX_ROWS)
        parsed_row = AXSH_MAX_ROWS - 1;
    *row = parsed_row;
    *column = parsed_column;
    *absolute = (bool_t)(abs_col || abs_row);
    *length = i;
    return true;
}

static bool_t axsh_parse_range(axsh_sheet_t *sheet, const char *text,
                               u16 *length, u16 *row, u16 *column,
                               u16 *row2, u16 *column2, bool_t *absolute)
{
    u16 consumed = 0;
    u16 sub = 0;
    bool_t abs_a = false;
    bool_t abs_b = false;
    AXIOM_UNUSED_VAR(sheet);
    if (!axsh_parse_reference(text, &consumed, row, column, &abs_a))
        return false;
    if (text[consumed] != ':')
    {
        *row2 = *row;
        *column2 = *column;
        *absolute = abs_a;
        *length = consumed;
        return true;
    }
    if (!axsh_parse_reference(&text[consumed + 1], &sub, row2, column2, &abs_b))
        return false;
    *absolute = (bool_t)(abs_a || abs_b);
    *length = (u16)(consumed + 1 + sub);
    return true;
}

static bool_t axsh_parse_function(axsh_parser_t *parser, const char *text,
                                  u16 *length, axsh_sheet_t *sheet)
{
    static const char *const names[5] = { "SUM", "AVG", "MIN", "MAX", "COUNT" };
    u16 i;
    u16 consumed = 0;
    axsh_token_t *token;
    for (i = 0; i < 5; i++)
    {
        u16 k = 0;
        u16 name_len = 0;
        while (names[i][name_len] != 0)
            name_len++;
        while (k < name_len && text[k] == names[i][k])
            k++;
        if (k != name_len)
            continue;
        if (text[k] != '(')
            continue;
        /* reserve the token slot before parsing the range */
        if (parser->count >= AXSH_MAX_TOKENS)
            return false;
        token = &parser->tokens[parser->count];
        token->kind = AXSH_OP_FUNC;
        token->func = (u8)i;
        token->number = 0;
        token->absolute = false;
        if (!axsh_parse_range(sheet, &text[k + 1], &consumed,
                              &token->row, &token->column,
                              &token->row2, &token->column2, &token->absolute))
        {
            u16 r = 0;
            u16 c = 0;
            u16 r2 = 0;
            u16 c2 = 0;
            bool_t abs_flag = false;
            i64 number = 0;
            u16 num_len = 0;
            /* COUNT(1,2,3) style literal lists are accepted as a range of the
             * first value only; keep the grammar small and predictable. */
            if (axsh_parse_number(&text[k + 1], &num_len, &number))
            {
                token->row = 0;
                token->column = 0;
                token->row2 = 0;
                token->column2 = 0;
                token->number = number;
                consumed = (u16)(num_len + 1 + 1);
                if (text[k + 1 + num_len] == ')')
                    consumed = (u16)(num_len + 2);
            }
            else
            {
                AXIOM_UNUSED_VAR(r);
                AXIOM_UNUSED_VAR(c);
                AXIOM_UNUSED_VAR(r2);
                AXIOM_UNUSED_VAR(c2);
                AXIOM_UNUSED_VAR(abs_flag);
                return false;
            }
        }
        else
        {
            consumed = (u16)(consumed + 2);
        }
        parser->count++;
        *length = (u16)(k + consumed);
        return true;
    }
    return false;
}

static bool_t axsh_parse_formula(axsh_sheet_t *sheet, const char *formula,
                                 axsh_parser_t *parser)
{
    u16 i = 0;
    parser->count = 0;
    parser->position = 0;
    parser->failed = false;
    if (formula[0] != '=')
        return false;
    i = 1;
    while (formula[i] != 0)
    {
        char c = formula[i];
        if (c == ' ' || c == '\t')
        {
            i++;
            continue;
        }
        if (parser->count >= AXSH_MAX_TOKENS)
            return false;
        if (c >= '0' && c <= '9')
        {
            u16 consumed = 0;
            i64 value = 0;
            if (!axsh_parse_number(&formula[i], &consumed, &value))
                return false;
            parser->tokens[parser->count].kind = AXSH_OP_NUMBER;
            parser->tokens[parser->count].number = value;
            parser->count++;
            i = (u16)(i + consumed);
            continue;
        }
        if ((c >= 'A' && c <= 'Z') || c == '$')
        {
            u16 consumed = 0;
            u16 row = 0;
            u16 column = 0;
            bool_t absolute = false;
            u16 alpha = 0;
            while (alpha < 8 && formula[i + alpha] >= 'A' && formula[i + alpha] <= 'Z')
                alpha++;
            if (alpha >= 2 && formula[i + alpha] == '(')
            {
                u16 func_len = 0;
                if (!axsh_parse_function(parser, &formula[i], &func_len, sheet))
                    return false;
                i = (u16)(i + func_len);
                continue;
            }
            if (!axsh_parse_reference(&formula[i], &consumed, &row, &column, &absolute))
                return false;
            parser->tokens[parser->count].kind = AXSH_OP_REF;
            parser->tokens[parser->count].row = row;
            parser->tokens[parser->count].column = column;
            parser->tokens[parser->count].absolute = absolute;
            parser->count++;
            i = (u16)(i + consumed);
            continue;
        }
        switch (c)
        {
        case '+':
            parser->tokens[parser->count].kind = AXSH_OP_PLUS;
            break;
        case '-':
            parser->tokens[parser->count].kind = AXSH_OP_MINUS;
            break;
        case '*':
            parser->tokens[parser->count].kind = AXSH_OP_MUL;
            break;
        case '/':
            parser->tokens[parser->count].kind = AXSH_OP_DIV;
            break;
        case '(':
            parser->tokens[parser->count].kind = AXSH_OP_LPAREN;
            break;
        case ')':
            parser->tokens[parser->count].kind = AXSH_OP_RPAREN;
            break;
        default:
            return false;
        }
        parser->count++;
        i++;
    }
    parser->tokens[parser->count].kind = AXSH_OP_END;
    parser->count++;
    return true;
}

/* -------------------------------------------------------------- evaluation */

static i64 axsh_cell_value(const axsh_sheet_t *sheet, u16 row, u16 column)
{
    if (row >= AXSH_MAX_ROWS || column >= AXSH_MAX_COLS)
        return 0;
    if (sheet->cells[row][column].error)
        return 0;
    return sheet->cells[row][column].value;
}

static i64 axsh_eval_cell(axsh_sheet_t *sheet, u16 row, u16 column, bool_t *ok)
{
    axsh_cell_t *cell;
    axsh_parser_t parser;
    u16 offset = 0;
    i64 value = 0;
    if (row >= AXSH_MAX_ROWS || column >= AXSH_MAX_COLS)
    {
        *ok = false;
        return 0;
    }
    cell = &sheet->cells[row][column];
    if (cell->cached)
        return cell->value;
    if (!cell->has_formula)
    {
        bool_t numeric = false;
        u16 consumed = 0;
        i64 literal = 0;
        cell->cached = true;
        if (cell->text[0] == 0)
            return 0;
        numeric = axsh_parse_number(cell->text, &consumed, &literal);
        if (numeric && cell->text[consumed] == 0)
        {
            cell->numeric = true;
            cell->value = literal;
        }
        else
        {
            cell->numeric = false;
            cell->value = 0;
        }
        return cell->value;
    }
    if (sheet->depth >= AXSH_EVAL_DEPTH)
    {
        cell->error = true;
        cell->cached = true;
        *ok = false;
        return 0;
    }
    sheet->depth++;
    sheet->evaluations++;
    if (!axsh_parse_formula(sheet, cell->formula, &parser))
    {
        sheet->depth--;
        cell->error = true;
        cell->cached = true;
        *ok = false;
        return 0;
    }
    value = axsh_eval_expression(sheet, &parser, &offset, ok);
    sheet->depth--;
    cell->value = value;
    cell->cached = true;
    cell->error = *ok ? false : true;
    return value;
}

static i64 axsh_eval_function(axsh_sheet_t *sheet, const axsh_token_t *token,
                              bool_t *ok)
{
    u16 row0 = token->row < token->row2 ? token->row : token->row2;
    u16 row1 = token->row < token->row2 ? token->row2 : token->row;
    u16 col0 = token->column < token->column2 ? token->column : token->column2;
    u16 col1 = token->column < token->column2 ? token->column2 : token->column;
    u16 row;
    u16 col;
    i64 total = 0;
    i64 count = 0;
    i64 minimum = 0;
    i64 maximum = 0;
    bool_t first = true;
    if (row1 >= AXSH_MAX_ROWS)
        row1 = AXSH_MAX_ROWS - 1;
    if (col1 >= AXSH_MAX_COLS)
        col1 = AXSH_MAX_COLS - 1;
    for (row = row0; row <= row1; row++)
    {
        for (col = col0; col <= col1; col++)
        {
            axsh_cell_t *cell = &sheet->cells[row][col];
            i64 value;
            if (cell->text[0] == 0 && !cell->has_formula)
                continue;
            value = axsh_eval_cell(sheet, row, col, ok);
            if (!*ok)
                continue;
            total += value;
            count++;
            if (first)
            {
                minimum = value;
                maximum = value;
                first = false;
            }
            else
            {
                if (value < minimum)
                    minimum = value;
                if (value > maximum)
                    maximum = value;
            }
        }
    }
    switch (token->func)
    {
    case 0:
        return total;
    case 1:
        return count > 0 ? total / count : 0;
    case 2:
        return count > 0 ? minimum : 0;
    case 3:
        return count > 0 ? maximum : 0;
    case 4:
        return count;
    default:
        break;
    }
    return 0;
}

static i64 axsh_eval_factor(axsh_sheet_t *sheet, axsh_parser_t *parser,
                            u16 *offset, bool_t *ok)
{
    axsh_token_t *token;
    i64 value = 0;
    if (*offset >= parser->count)
    {
        *ok = false;
        return 0;
    }
    token = &parser->tokens[*offset];
    if (token->kind == AXSH_OP_MINUS)
    {
        (*offset)++;
        return -axsh_eval_factor(sheet, parser, offset, ok);
    }
    if (token->kind == AXSH_OP_NUMBER)
    {
        (*offset)++;
        return token->number;
    }
    if (token->kind == AXSH_OP_REF)
    {
        (*offset)++;
        if (token->row >= AXSH_MAX_ROWS || token->column >= AXSH_MAX_COLS)
        {
            *ok = false;
            return 0;
        }
        return axsh_eval_cell(sheet, token->row, token->column, ok);
    }
    if (token->kind == AXSH_OP_FUNC)
    {
        (*offset)++;
        return axsh_eval_function(sheet, token, ok);
    }
    if (token->kind == AXSH_OP_LPAREN)
    {
        (*offset)++;
        value = axsh_eval_expression(sheet, parser, offset, ok);
        if (!*ok)
            return 0;
        if (*offset >= parser->count || parser->tokens[*offset].kind != AXSH_OP_RPAREN)
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

static i64 axsh_eval_term(axsh_sheet_t *sheet, axsh_parser_t *parser,
                          u16 *offset, bool_t *ok)
{
    i64 left = axsh_eval_factor(sheet, parser, offset, ok);
    if (!*ok)
        return 0;
    while (*offset < parser->count)
    {
        axsh_token_t *token = &parser->tokens[*offset];
        if (token->kind != AXSH_OP_MUL && token->kind != AXSH_OP_DIV)
            break;
        (*offset)++;
        {
            i64 right = axsh_eval_factor(sheet, parser, offset, ok);
            if (!*ok)
                return 0;
            if (token->kind == AXSH_OP_MUL)
                left = left * right;
            else if (right == 0)
            {
                *ok = false;
                return 0;
            }
            else
                left = left / right;
        }
    }
    return left;
}

static i64 axsh_eval_expression(axsh_sheet_t *sheet, axsh_parser_t *parser,
                                u16 *offset, bool_t *ok)
{
    i64 left = axsh_eval_term(sheet, parser, offset, ok);
    if (!*ok)
        return 0;
    while (*offset < parser->count)
    {
        axsh_token_t *token = &parser->tokens[*offset];
        if (token->kind != AXSH_OP_PLUS && token->kind != AXSH_OP_MINUS)
            break;
        (*offset)++;
        {
            i64 right = axsh_eval_term(sheet, parser, offset, ok);
            if (!*ok)
                return 0;
            if (token->kind == AXSH_OP_PLUS)
                left = left + right;
            else
                left = left - right;
        }
    }
    return left;
}

/* ---------------------------------------------------------------- mutation */

static void axsh_set_text(axsh_sheet_t *sheet, u16 row, u16 column,
                          const char *text)
{
    axsh_cell_t *cell;
    u16 i = 0;
    if (row >= AXSH_MAX_ROWS || column >= AXSH_MAX_COLS)
        return;
    cell = &sheet->cells[row][column];
    cell->text[0] = 0;
    cell->formula[0] = 0;
    cell->has_formula = false;
    cell->error = false;
    cell->cached = false;
    while (text[i] != 0 && i < AXSH_CELL_TEXT - 1)
    {
        cell->text[i] = text[i];
        i++;
    }
    cell->text[i] = 0;
    if (text[0] == '=')
    {
        u16 k = 0;
        while (text[k] != 0 && k < AXSH_FORMULA_TEXT - 1)
        {
            cell->formula[k] = text[k];
            k++;
        }
        cell->formula[k] = 0;
        cell->has_formula = true;
    }
    else
    {
        u16 consumed = 0;
        i64 literal = 0;
        bool_t numeric = axsh_parse_number(cell->text, &consumed, &literal);
        if (numeric)
            cell->numeric = (bool_t)(cell->text[consumed] == 0);
    }
    sheet->edits++;
    axsh_invalidate(sheet);
}

static void axsh_invalidate(axsh_sheet_t *sheet)
{
    u16 row;
    u16 column;
    sheet->recalc_stamp++;
    for (row = 0; row < AXSH_MAX_ROWS; row++)
    {
        for (column = 0; column < AXSH_MAX_COLS; column++)
        {
            sheet->cells[row][column].cached = false;
            sheet->cells[row][column].dirty = 0;
        }
    }
}

static void axsh_recalculate(axsh_sheet_t *sheet, u16 row, u16 column)
{
    bool_t ok = true;
    axsh_cell_t *cell = &sheet->cells[row][column];
    cell->cached = false;
    cell->error = false;
    sheet->depth = 0;
    axsh_eval_cell(sheet, row, column, &ok);
}

static void axsh_recalculate_all(axsh_sheet_t *sheet)
{
    u16 row;
    u16 column;
    axsh_invalidate(sheet);
    sheet->depth = 0;
    for (row = 0; row < AXSH_MAX_ROWS; row++)
    {
        for (column = 0; column < AXSH_MAX_COLS; column++)
        {
            if (sheet->cells[row][column].cached)
                continue;
            {
                bool_t ok = true;
                axsh_eval_cell(sheet, row, column, &ok);
            }
        }
    }
}

static void axsh_begin_edit(axsh_sheet_t *sheet)
{
    axsh_cell_t *cell = &sheet->cells[sheet->cursor_row][sheet->cursor_column];
    u16 i = 0;
    sheet->editing = true;
    const char *source = cell->has_formula ? cell->formula : cell->text;
    while (source[i] != 0 && i < AXSH_FORMULA_TEXT - 1)
    {
        sheet->edit_buffer[i] = source[i];
        i++;
    }
    sheet->edit_buffer[i] = 0;
    sheet->edit_length = i;
    sheet->selecting = false;
}

static void axsh_commit_edit(axsh_sheet_t *sheet)
{
    if (!sheet->editing)
        return;
    sheet->edit_buffer[sheet->edit_length] = 0;
    axsh_set_text(sheet, sheet->cursor_row, sheet->cursor_column,
                  sheet->edit_buffer);
    sheet->editing = false;
}

static void axsh_cancel_edit(axsh_sheet_t *sheet)
{
    sheet->editing = false;
    sheet->edit_length = 0;
    sheet->edit_buffer[0] = 0;
}

static void axsh_edit_append(axsh_sheet_t *sheet, char c)
{
    if (!sheet->editing)
        axsh_begin_edit(sheet);
    if (sheet->edit_length + 1 >= AXSH_FORMULA_TEXT)
        return;
    sheet->edit_buffer[sheet->edit_length] = c;
    sheet->edit_length++;
    sheet->edit_buffer[sheet->edit_length] = 0;
}

/* --------------------------------------------------------------- rendering */

static void axsh_paint_cell_text(gui_surface_t *surface, const axsh_cell_t *cell,
                                 i32 x, i32 y, i32 width, u32 colour)
{
    char trimmed[AXSH_CELL_TEXT];
    u16 i = 0;
    i32 limit = width - 6;
    while (cell->text[i] != 0 && i < AXSH_CELL_TEXT - 1)
    {
        trimmed[i] = cell->text[i];
        i++;
    }
    trimmed[i] = 0;
    if (i > 0)
    {
        u16 fitted = i;
        while (fitted > 1)
        {
            char measure[AXSH_CELL_TEXT];
            u16 k;
            for (k = 0; k < fitted; k++)
                measure[k] = trimmed[k];
            measure[fitted] = 0;
            if (gui_text_width(measure, 1) <= limit)
                break;
            fitted--;
            trimmed[fitted] = 0;
        }
        gui_text(surface, x, y, trimmed, colour, 1);
    }
}

static void axsh_paint_headers(gui_surface_t *surface, const axsh_sheet_t *sheet,
                               gui_rect_t grid, i32 origin_x)
{
    u16 column;
    u16 row;
    char name[4];
    gui_fill_rect(surface, gui_rect(grid.x, grid.y, grid.w, AXSH_HEAD_HEIGHT),
                  AXSH_C_HEADER);
    for (column = 0; column < AXSH_MAX_COLS; column++)
    {
        i32 x = origin_x + axsh_column_x(sheet, column);
        i32 width = sheet->column_width[column];
        u32 colour = AXSH_C_DIM;
        if (x + width < grid.x || x > grid.x + grid.w)
            continue;
        gui_vline(surface, x + width - 1, grid.y, grid.y + grid.h, AXSH_C_GRID);
        if (column == sheet->cursor_column)
        {
            gui_fill_rect(surface, gui_rect(x, grid.y, width, AXSH_HEAD_HEIGHT),
                          AXSH_C_HEADER_SEL);
            colour = AXSH_C_TEXT;
        }
        axsh_column_name(column, name);
        gui_text(surface, x + width / 2 - 4, grid.y + 5, name, colour, 1);
    }
    gui_fill_rect(surface, gui_rect(grid.x, grid.y + AXSH_HEAD_HEIGHT, grid.w, 1),
                  AXSH_C_GRID);
    for (row = 0; row < AXSH_MAX_ROWS; row++)
    {
        i32 y = grid.y + AXSH_HEAD_HEIGHT + axsh_row_y(sheet, row) - sheet->scroll_y;
        if (y < grid.y || y > grid.y + grid.h)
            continue;
    }
}

static void axsh_paint_grid(gui_surface_t *surface, axsh_sheet_t *sheet,
                            gui_rect_t grid, i32 origin_x, i32 origin_y)
{
    u16 row;
    u16 column;
    u32 stamp = sheet->recalc_stamp;
    for (row = 0; row < AXSH_MAX_ROWS; row++)
    {
        i32 y = origin_y + axsh_row_y(sheet, row);
        if (y + sheet->row_height[row] < grid.y)
            continue;
        if (y > grid.y + grid.h)
            break;
        gui_hline(surface, grid.x, grid.x + grid.w, y + sheet->row_height[row] - 1,
                  AXSH_C_GRID);
        for (column = 0; column < AXSH_MAX_COLS; column++)
        {
            i32 x = origin_x + axsh_column_x(sheet, column);
            i32 width = sheet->column_width[column];
            axsh_cell_t *cell;
            u32 colour;
            if (x + width < grid.x)
                continue;
            if (x > grid.x + grid.w)
                break;
            cell = &sheet->cells[row][column];
            if (cell->dirty != stamp)
            {
                cell->dirty = (u8)stamp;
            }
            colour = cell->error ? AXSH_C_ERROR : (cell->has_formula ? AXSH_C_ACCENT : AXSH_C_TEXT);
            if (stamp != 0 && cell->cached && cell->numeric)
                colour = cell->error ? AXSH_C_ERROR : AXSH_C_TEXT;
            gui_text(surface, x + 4, y + 2, "", colour, 1);
            axsh_paint_cell_text(surface, cell, x + 4, y + 2, width, colour);
        }
        for (column = 0; column < AXSH_MAX_COLS; column++)
        {
            i32 x = origin_x + axsh_column_x(sheet, column);
            if (x < grid.x || x > grid.x + grid.w)
                continue;
            gui_vline(surface, x, y, y + sheet->row_height[row], AXSH_C_GRID);
        }
    }
}

static void axsh_paint_selection(gui_surface_t *surface,
                                 const axsh_sheet_t *sheet, gui_rect_t grid,
                                 i32 origin_x, i32 origin_y)
{
    u16 row0 = sheet->cursor_row < sheet->anchor_row ? sheet->cursor_row : sheet->anchor_row;
    u16 row1 = sheet->cursor_row < sheet->anchor_row ? sheet->anchor_row : sheet->cursor_row;
    u16 col0 = sheet->cursor_column < sheet->anchor_column ? sheet->cursor_column : sheet->anchor_column;
    u16 col1 = sheet->cursor_column < sheet->anchor_column ? sheet->anchor_column : sheet->cursor_column;
    i32 x0 = origin_x + axsh_column_x(sheet, col0);
    i32 x1 = origin_x + axsh_column_x(sheet, col1) + sheet->column_width[col1];
    i32 y0 = origin_y + axsh_row_y(sheet, row0);
    i32 y1 = origin_y + axsh_row_y(sheet, row1) + sheet->row_height[row1];
    if (row0 != row1 || col0 != col1)
        gui_blend_rect(surface, gui_rect(x0, y0, x1 - x0, y1 - y0),
                       GUI_ARGB(0x40, 0x2A, 0x3A, 0x58));
    gui_rect_outline(surface, gui_rect(x0, y0, x1 - x0, y1 - y0), AXSH_C_ACCENT);
    gui_fill_rect(surface, gui_rect(x1 - 1, y1 - 1, 3, 3), AXSH_C_ACCENT);
}

static void axsh_paint_formula_bar(gui_surface_t *surface,
                                   const axsh_sheet_t *sheet, gui_rect_t bar)
{
    char reference[16];
    const axsh_cell_t *cell = &sheet->cells[sheet->cursor_row][sheet->cursor_column];
    const char *source = cell->has_formula ? cell->formula : cell->text;
    gui_fill_rect(surface, bar, AXSH_C_HEADER);
    gui_hline(surface, bar.x, bar.x + bar.w, bar.y + bar.h - 1, AXSH_C_GRID);
    axsh_reference_name(sheet->cursor_row, sheet->cursor_column, reference);
    gui_fill_rect(surface, gui_rect(bar.x + 8, bar.y + 4, 56, bar.h - 8), AXSH_C_BG);
    gui_rect_outline(surface, gui_rect(bar.x + 8, bar.y + 4, 56, bar.h - 8), AXSH_C_GRID);
    gui_text(surface, bar.x + 20, bar.y + bar.h / 2 - 6, reference, AXSH_C_TEXT, 1);
    gui_text(surface, bar.x + 76, bar.y + bar.h / 2 - 6, "fx", AXSH_C_ACCENT, 1);
    gui_fill_rect(surface, gui_rect(bar.x + 100, bar.y + 4, bar.w - 108, bar.h - 8), AXSH_C_BG);
    gui_text(surface, bar.x + 108, bar.y + bar.h / 2 - 6, source, AXSH_C_TEXT, 1);
}

static void axsh_edit_paint(gui_surface_t *surface, const axsh_sheet_t *sheet,
                            gui_rect_t bar)
{
    i32 x = bar.x + 4;
    i32 y = bar.y + 2;
    gui_fill_rect(surface, gui_rect(bar.x, bar.y, bar.w, bar.h), AXSH_C_EDIT);
    gui_rect_outline(surface, gui_rect(bar.x, bar.y, bar.w, bar.h), AXSH_C_ACCENT);
    if (sheet->edit_length > 0)
        gui_text(surface, x, y, sheet->edit_buffer, AXSH_C_TEXT, 1);
    x += gui_text_width(sheet->edit_buffer, 1);
    gui_fill_rect(surface, gui_rect(x, y, 2, bar.h - 6), AXSH_C_ACCENT);
}

/* -------------------------------------------------------------- public API */

void axsh_init(axsh_sheet_t *sheet, const char *name)
{
    u16 i;
    u32 j;
    u16 row;
    u16 column;
    axsh_zero_bytes(sheet, (u32)sizeof(*sheet));
    for (i = 0; i < AXSH_MAX_COLS; i++)
        sheet->column_width[i] = AXSH_COL_WIDTH;
    for (i = 0; i < AXSH_MAX_ROWS; i++)
        sheet->row_height[i] = AXSH_ROW_HEIGHT;
    for (row = 0; row < AXSH_MAX_ROWS; row++)
    {
        for (column = 0; column < AXSH_MAX_COLS; column++)
        {
            sheet->cells[row][column].value = 0;
            sheet->cells[row][column].has_formula = false;
            sheet->cells[row][column].numeric = false;
            sheet->cells[row][column].error = false;
            sheet->cells[row][column].cached = false;
        }
    }
    sheet->cursor_row = 0;
    sheet->cursor_column = 0;
    sheet->anchor_row = 0;
    sheet->anchor_column = 0;
    j = 0;
    while (name[j] != 0 && j < AXSH_NAME_LENGTH - 1)
    {
        sheet->name[j] = name[j];
        j++;
    }
    sheet->name[j] = 0;
    sheet->recalc_stamp = 0;
    axsh_recalculate_all(sheet);
}

void axsh_set_cell(axsh_sheet_t *sheet, u16 row, u16 column, const char *text)
{
    axsh_set_text(sheet, row, column, text);
    axsh_recalculate(sheet, row, column);
}

i64 axsh_value(const axsh_sheet_t *sheet, u16 row, u16 column)
{
    return axsh_cell_value(sheet, row, column);
}

const char *axsh_display(const axsh_sheet_t *sheet, u16 row, u16 column)
{
    return sheet->cells[row][column].text;
}

bool_t axsh_cell_error(const axsh_sheet_t *sheet, u16 row, u16 column)
{
    return sheet->cells[row][column].error;
}

void axsh_set_column_width(axsh_sheet_t *sheet, u16 column, u16 width)
{
    if (column >= AXSH_MAX_COLS)
        return;
    if (width < 24)
        width = 24;
    if (width > 400)
        width = 400;
    sheet->column_width[column] = width;
}

void axsh_set_row_height(axsh_sheet_t *sheet, u16 row, u16 height)
{
    if (row >= AXSH_MAX_ROWS)
        return;
    if (height < 16)
        height = 16;
    if (height > 120)
        height = 120;
    sheet->row_height[row] = height;
}

void axsh_move(axsh_sheet_t *sheet, i32 drow, i32 dcolumn, bool_t extend)
{
    i32 row = (i32)sheet->cursor_row + drow;
    i32 column = (i32)sheet->cursor_column + dcolumn;
    if (row < 0)
        row = 0;
    if (row >= AXSH_MAX_ROWS)
        row = AXSH_MAX_ROWS - 1;
    if (column < 0)
        column = 0;
    if (column >= AXSH_MAX_COLS)
        column = AXSH_MAX_COLS - 1;
    if (extend)
    {
        if (!sheet->selecting)
        {
            sheet->anchor_row = sheet->cursor_row;
            sheet->anchor_column = sheet->cursor_column;
        }
        sheet->selecting = true;
    }
    else
    {
        sheet->selecting = false;
        sheet->anchor_row = (u16)row;
        sheet->anchor_column = (u16)column;
    }
    sheet->cursor_row = (u16)row;
    sheet->cursor_column = (u16)column;
    {
        i32 cursor_y = axsh_row_y(sheet, sheet->cursor_row);
        if (cursor_y < sheet->scroll_y)
            sheet->scroll_y = cursor_y;
        if (cursor_y + AXSH_ROW_HEIGHT > sheet->scroll_y + sheet->view_height)
            sheet->scroll_y = cursor_y + AXSH_ROW_HEIGHT - sheet->view_height;
        if (sheet->scroll_y < 0)
            sheet->scroll_y = 0;
    }
}

void axsh_click(axsh_sheet_t *sheet, i32 x, i32 y, bool_t extend)
{
    u16 column = axsh_column_at(sheet, x + sheet->scroll_x);
    u16 row = axsh_row_at(sheet, y + sheet->scroll_y);
    if (sheet->editing)
        axsh_commit_edit(sheet);
    sheet->cursor_row = row;
    sheet->cursor_column = column;
    if (extend)
    {
        sheet->selecting = true;
    }
    else
    {
        sheet->selecting = false;
        sheet->anchor_row = row;
        sheet->anchor_column = column;
    }
}

void axsh_scroll(axsh_sheet_t *sheet, i32 dx, i32 dy)
{
    sheet->scroll_x += dx;
    sheet->scroll_y += dy;
    if (sheet->scroll_x < 0)
        sheet->scroll_x = 0;
    if (sheet->scroll_y < 0)
        sheet->scroll_y = 0;
    {
        i32 max_x = axsh_column_x(sheet, AXSH_MAX_COLS - 1) - sheet->view_width + 120;
        i32 max_y = axsh_row_y(sheet, AXSH_MAX_ROWS - 1) - sheet->view_height + 40;
        if (sheet->scroll_x > max_x)
            sheet->scroll_x = max_x > 0 ? max_x : 0;
        if (sheet->scroll_y > max_y)
            sheet->scroll_y = max_y > 0 ? max_y : 0;
    }
}

bool_t axsh_key(axsh_sheet_t *sheet, u16 key, bool_t shift, bool_t control)
{
    if (sheet->editing)
    {
        switch (key)
        {
        case 0x0D:
        case 0x0A:
            axsh_commit_edit(sheet);
            axsh_move(sheet, 1, 0, false);
            return true;
        case 0x1B:
            axsh_cancel_edit(sheet);
            return true;
        case 0x08:
            if (sheet->edit_length > 0)
            {
                sheet->edit_length--;
                sheet->edit_buffer[sheet->edit_length] = 0;
            }
            return true;
        default:
            break;
        }
        if (key >= 0x20 && key < 0x7F)
        {
            char text[2];
            text[0] = (char)key;
            text[1] = 0;
            {
                u16 k = 0;
                while (k < sheet->edit_length)
                    k++;
                if (sheet->edit_length + 1 < AXSH_FORMULA_TEXT)
                {
                    sheet->edit_buffer[sheet->edit_length] = (char)key;
                    sheet->edit_length++;
                    sheet->edit_buffer[sheet->edit_length] = 0;
                }
            }
            AXIOM_UNUSED_VAR(text);
            return true;
        }
        return false;
    }
    switch (key)
    {
    case 0x01:
        axsh_move(sheet, -1, 0, shift);
        return true;
    case 0x02:
        axsh_move(sheet, 1, 0, shift);
        return true;
    case 0x03:
        axsh_move(sheet, 0, -1, shift);
        return true;
    case 0x04:
        axsh_move(sheet, 0, 1, shift);
        return true;
    case 0x08:
        axsh_set_cell(sheet, sheet->cursor_row, sheet->cursor_column, "");
        return true;
    case 0x0D:
        axsh_begin_edit(sheet);
        return true;
    default:
        break;
    }
    if (key >= 0x20 && key < 0x7F)
    {
        axsh_begin_edit(sheet);
        sheet->edit_buffer[0] = 0;
        sheet->edit_length = 0;
        axsh_edit_append(sheet, (char)key);
        if (control && (key == 'F' || key == 'f'))
        {
            axsh_recalculate_all(sheet);
        }
        return true;
    }
    return false;
}

void axsh_paint(axsh_sheet_t *sheet, gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t formula = gui_rect(bounds.x, bounds.y, bounds.w, AXSH_FORMULA_BAR);
    gui_rect_t head = gui_rect(bounds.x, bounds.y + AXSH_FORMULA_BAR,
                               bounds.w, AXSH_HEAD_HEIGHT);
    gui_rect_t grid = gui_rect(bounds.x, head.y + AXSH_HEAD_HEIGHT,
                               bounds.w, bounds.h - AXSH_FORMULA_BAR - AXSH_HEAD_HEIGHT);
    i32 origin_x = grid.x - sheet->scroll_x;
    i32 origin_y = grid.y - sheet->scroll_y;
    sheet->view_width = grid.w;
    sheet->view_height = grid.h;
    gui_fill_rect(surface, bounds, AXSH_C_BG);
    axsh_paint_formula_bar(surface, sheet, formula);
    gui_fill_rect(surface, head, AXSH_C_HEADER);
    axsh_paint_headers(surface, sheet, gui_rect(grid.x, head.y, grid.w, head.h), origin_x);
    axsh_paint_grid(surface, sheet, grid, origin_x, origin_y);
    axsh_paint_selection(surface, sheet, grid, origin_x, origin_y);
    if (sheet->editing)
    {
        i32 x = origin_x + axsh_column_x(sheet, sheet->cursor_column);
        i32 y = origin_y + axsh_row_y(sheet, sheet->cursor_row);
        axsh_edit_paint(surface, sheet,
                        gui_rect(x, y, sheet->column_width[sheet->cursor_column] + 160,
                                 sheet->row_height[sheet->cursor_row] + 4));
    }
    else
    {
        char reference[16];
        axsh_reference_name(sheet->cursor_row, sheet->cursor_column, reference);
        gui_text(surface, grid.x + 8, grid.y + grid.h - 20, reference, AXSH_C_DIM, 1);
        gui_text(surface, grid.x + 88, grid.y + grid.h - 20, sheet->name, AXSH_C_DIM, 1);
    }
}

void axsh_dump(const axsh_sheet_t *sheet)
{
    printk("axsh: %s cursor %u:%u cells=%u evals=%u\n",
           sheet->name, (unsigned)sheet->cursor_row,
           (unsigned)sheet->cursor_column, (unsigned)sheet->edits,
           (unsigned)sheet->evaluations);
}

bool_t axsh_selftest(void)
{
    axsh_sheet_t sheet;
    axsh_init(&sheet, "test");
    axsh_set_cell(&sheet, 0, 0, "10");
    axsh_set_cell(&sheet, 1, 0, "20");
    axsh_set_cell(&sheet, 2, 0, "30");
    axsh_set_cell(&sheet, 3, 0, "=A1+A2+A3");
    if (axsh_value(&sheet, 3, 0) != 60)
        return false;
    axsh_set_cell(&sheet, 3, 1, "=SUM(A1:A3)");
    if (axsh_value(&sheet, 3, 1) != 60)
        return false;
    axsh_set_cell(&sheet, 4, 0, "=A3*2-A1/5");
    if (axsh_value(&sheet, 4, 0) != 58)
        return false;
    axsh_set_cell(&sheet, 5, 0, "=AVG(A1:A3)");
    if (axsh_value(&sheet, 5, 0) != 20)
        return false;
    axsh_set_cell(&sheet, 6, 0, "=MAX(A1:A3)");
    if (axsh_value(&sheet, 6, 0) != 30)
        return false;
    axsh_set_cell(&sheet, 7, 0, "=COUNT(A1:A3)");
    if (axsh_value(&sheet, 7, 0) != 3)
        return false;
    axsh_set_cell(&sheet, 8, 0, "=A1/0");
    if (!axsh_cell_error(&sheet, 8, 0))
        return false;
    axsh_set_cell(&sheet, 9, 0, "plain text");
    if (axsh_value(&sheet, 9, 0) != 0)
        return false;
    axsh_move(&sheet, 1, 2, false);
    if (sheet.cursor_row != 1 || sheet.cursor_column != 2)
        return false;
    return true;
}
