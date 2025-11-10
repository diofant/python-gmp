#include "mpz.h"

#include <locale.h>

#if !defined(PYPY_VERSION) && !defined(GRAALVM_PYTHON)

static void
unknown_presentation_type(Py_UCS4 presentation_type,
                          const char* type_name)
{
    /* %c might be out-of-range, hence the two cases. */
    if (presentation_type > 32 && presentation_type < 128) {
        PyErr_Format(PyExc_ValueError,
                     "Unknown format code '%c' "
                     "for object of type '%.200s'",
                     (char)presentation_type,
                     type_name);
    }
    else {
        PyErr_Format(PyExc_ValueError,
                     "Unknown format code '\\x%x' "
                     "for object of type '%.200s'",
                     (unsigned int)presentation_type,
                     type_name);
    }
}

static void
invalid_thousands_separator_type(int specifier, Py_UCS4 presentation_type)
{
    assert(specifier == ',' || specifier == '_');
    if (presentation_type > 32 && presentation_type < 128) {
        PyErr_Format(PyExc_ValueError,
                     "Cannot specify '%c' with '%c'.",
                     specifier, (char)presentation_type);
    }
    else {
        PyErr_Format(PyExc_ValueError,
                     "Cannot specify '%c' with '\\x%x'.",
                     specifier, (unsigned int)presentation_type);
    }
}

static void
invalid_comma_and_underscore(void)
{
    PyErr_Format(PyExc_ValueError, "Cannot specify both ',' and '_'.");
}

/*
    get_integer consumes 0 or more decimal digit characters from an
    input string, updates *result with the corresponding positive
    integer, and returns the number of digits consumed.

    returns -1 on error.
*/
static int
get_integer(PyObject *str, Py_ssize_t *ppos, Py_ssize_t end,
                  Py_ssize_t *result)
{
    Py_ssize_t accumulator = 0, digitval, pos = *ppos;
    int numdigits = 0;
    int kind = PyUnicode_KIND(str);
    const void *data = PyUnicode_DATA(str);

    for (; pos < end; pos++, numdigits++) {
        digitval = Py_UNICODE_TODECIMAL(PyUnicode_READ(kind, data, pos));
        if (digitval < 0) {
            break;
        }
        /*
           Detect possible overflow before it happens:

              accumulator * 10 + digitval > PY_SSIZE_T_MAX if and only if
              accumulator > (PY_SSIZE_T_MAX - digitval) / 10.
        */
        if (accumulator > (PY_SSIZE_T_MAX - digitval) / 10) {
            PyErr_Format(PyExc_ValueError,
                         "Too many decimal digits in format string");
            *ppos = pos;
            return -1;
        }
        accumulator = accumulator * 10 + digitval;
    }
    *ppos = pos;
    *result = accumulator;
    return numdigits;
}

/************************************************************************/
/*********** standard format specifier parsing **************************/
/************************************************************************/

/* returns true if this character is a specifier alignment token */
Py_LOCAL_INLINE(int)
is_alignment_token(Py_UCS4 c)
{
    switch (c) {
    case '<': case '>': case '=': case '^':
        return 1;
    default:
        return 0;
    }
}

/* returns true if this character is a sign element */
Py_LOCAL_INLINE(int)
is_sign_element(Py_UCS4 c)
{
    switch (c) {
    case ' ': case '+': case '-':
        return 1;
    default:
        return 0;
    }
}

/* Locale type codes. LT_NO_LOCALE must be zero. */
enum LocaleType {
    LT_NO_LOCALE = 0,
    LT_DEFAULT_LOCALE = ',',
    LT_UNDERSCORE_LOCALE = '_',
    LT_UNDER_FOUR_LOCALE,
    LT_CURRENT_LOCALE
};

typedef struct {
    Py_UCS4 fill_char;
    Py_UCS4 align;
    int alternate;
    int no_neg_0;
    Py_UCS4 sign;
    Py_ssize_t width;
    enum LocaleType thousands_separators;
    Py_ssize_t precision;
    enum LocaleType frac_thousands_separator;
    Py_UCS4 type;
} InternalFormatSpec;

/*
  ptr points to the start of the format_spec, end points just past its end.
  fills in format with the parsed information.
  returns 1 on success, 0 on failure.
  if failure, sets the exception
*/
static int
parse_internal_render_format_spec(PyObject *obj,
                                  PyObject *format_spec,
                                  Py_ssize_t start, Py_ssize_t end,
                                  InternalFormatSpec *format,
                                  Py_UCS4 default_type,
                                  Py_UCS4 default_align)
{
    Py_ssize_t pos = start;
    int kind = PyUnicode_KIND(format_spec);
    const void *data = PyUnicode_DATA(format_spec);
    /* end-pos is used throughout this code to specify the length of
       the input string */
#define READ_spec(index) PyUnicode_READ(kind, data, index)

    Py_ssize_t consumed;
    int align_specified = 0;
    int fill_char_specified = 0;

    format->fill_char = ' ';
    format->align = default_align;
    format->alternate = 0;
    format->no_neg_0 = 0;
    format->sign = '\0';
    format->width = -1;
    format->thousands_separators = LT_NO_LOCALE;
    format->frac_thousands_separator = LT_NO_LOCALE;
    format->precision = -1;
    format->type = default_type;

    /* If the second char is an alignment token,
       then parse the fill char */
    if (end-pos >= 2 && is_alignment_token(READ_spec(pos+1))) {
        format->align = READ_spec(pos+1);
        format->fill_char = READ_spec(pos);
        fill_char_specified = 1;
        align_specified = 1;
        pos += 2;
    }
    else if (end-pos >= 1 && is_alignment_token(READ_spec(pos))) {
        format->align = READ_spec(pos);
        align_specified = 1;
        ++pos;
    }
    /* Parse the various sign options */
    if (end-pos >= 1 && is_sign_element(READ_spec(pos))) {
        format->sign = READ_spec(pos);
        ++pos;
    }
    /* If the next character is z, request coercion of negative 0.
       Applies only to floats. */
    if (end-pos >= 1 && READ_spec(pos) == 'z') {
        format->no_neg_0 = 1;
        ++pos;
    }
    /* If the next character is #, we're in alternate mode.  This only
       applies to integers. */
    if (end-pos >= 1 && READ_spec(pos) == '#') {
        format->alternate = 1;
        ++pos;
    }
    /* The special case for 0-padding (backwards compat) */
    if (!fill_char_specified && end-pos >= 1 && READ_spec(pos) == '0') {
        format->fill_char = '0';
        if (!align_specified && default_align == '>') {
            format->align = '=';
        }
        ++pos;
    }
    consumed = get_integer(format_spec, &pos, end, &format->width);
    if (consumed == -1) {
        /* Overflow error. Exception already set. */
        return 0;
    }
    /* If consumed is 0, we didn't consume any characters for the
       width. In that case, reset the width to -1, because
       get_integer() will have set it to zero. -1 is how we record
       that the width wasn't specified. */
    if (consumed == 0) {
        format->width = -1;
    }
    /* Comma signifies add thousands separators */
    if (end-pos && READ_spec(pos) == ',') {
        format->thousands_separators = LT_DEFAULT_LOCALE;
        ++pos;
    }
    /* Underscore signifies add thousands separators */
    if (end-pos && READ_spec(pos) == '_') {
        if (format->thousands_separators != LT_NO_LOCALE) {
            invalid_comma_and_underscore();
            return 0;
        }
        format->thousands_separators = LT_UNDERSCORE_LOCALE;
        ++pos;
    }
    if (end-pos && READ_spec(pos) == ',') {
        if (format->thousands_separators == LT_UNDERSCORE_LOCALE) {
            invalid_comma_and_underscore();
            return 0;
        }
    }
    /* Parse field precision */
    if (end-pos && READ_spec(pos) == '.') {
        ++pos;

        consumed = get_integer(format_spec, &pos, end, &format->precision);
        if (consumed == -1) {
            /* Overflow error. Exception already set. */
            return 0;
        }

        if (end-pos && READ_spec(pos) == ',') {
            if (consumed == 0) {
                format->precision = -1;
            }
            format->frac_thousands_separator = LT_DEFAULT_LOCALE;
            ++pos;
            ++consumed;
        }
        if (end-pos && READ_spec(pos) == '_') {
            if (format->frac_thousands_separator != LT_NO_LOCALE) {
                invalid_comma_and_underscore();
                return 0;
            }
            if (consumed == 0) {
                format->precision = -1;
            }
            format->frac_thousands_separator = LT_UNDERSCORE_LOCALE;
            ++pos;
            ++consumed;
        }
        if (end-pos && READ_spec(pos) == ',') {
            if (format->frac_thousands_separator == LT_UNDERSCORE_LOCALE) {
                invalid_comma_and_underscore();
                return 0;
            }
        }
        /* Not having a precision or underscore/comma after a dot
           is an error. */
        if (consumed == 0) {
            PyErr_Format(PyExc_ValueError,
                         "Format specifier missing precision");
            return 0;
        }

    }
    /* Finally, parse the type field. */
    if (end-pos > 1) {
        /* More than one char remains, so this is an invalid format
           specifier. */
        /* Create a temporary object that contains the format spec we're
           operating on.  It's format_spec[start:end] (in Python syntax). */
        PyObject* actual_format_spec = PyUnicode_FromKindAndData(kind,
            (char*)data + kind*start, end-start);
        if (actual_format_spec != NULL) {
            PyErr_Format(PyExc_ValueError,
                         ("Invalid format specifier '%U' for object "
                          "of type '%.200s'"), actual_format_spec,
                         Py_TYPE(obj)->tp_name);
            Py_DECREF(actual_format_spec);
        }
        return 0;
    }
    if (end-pos == 1) {
        format->type = READ_spec(pos);
        ++pos;
    }

    /* Do as much validating as we can, just by looking at the format
       specifier.  Do not take into account what type of formatting
       we're doing (int, float, string). */

    if (format->thousands_separators) {
        switch (format->type) {
        case 'd':
        case 'e':
        case 'f':
        case 'g':
        case 'E':
        case 'G':
        case '%':
        case 'F':
        case '\0':
            /* These are allowed. See PEP 378.*/
            break;
        case 'b':
        case 'o':
        case 'x':
        case 'X':
            /* Underscores are allowed in bin/oct/hex. See PEP 515. */
            if (format->thousands_separators == LT_UNDERSCORE_LOCALE) {
                /* Every four digits, not every three, in bin/oct/hex. */
                format->thousands_separators = LT_UNDER_FOUR_LOCALE;
                break;
            }
        default:
            invalid_thousands_separator_type((int)format->thousands_separators,
                                             format->type);
            return 0;
        }
    }

    if (format->type == 'n'
        && format->frac_thousands_separator != LT_NO_LOCALE)
    {
        invalid_thousands_separator_type((int)format->frac_thousands_separator,
                                         format->type);
        return 0;
    }

    assert (format->align <= 127);
    assert (format->sign <= 127);
    return 1;
}

/* Locale info needed for formatting integers and the part of floats
   before and including the decimal. Note that locales only support
   8-bit chars, not unicode. */
typedef struct {
    PyObject *decimal_point;
    PyObject *thousands_sep;
    PyObject *frac_thousands_sep;
    const char *grouping;
    char *grouping_buffer;
} LocaleInfo;

#define LocaleInfo_STATIC_INIT {0, 0, 0, 0}

/* describes the layout for an integer, see the comment in
   calc_number_widths() for details */
typedef struct {
    Py_ssize_t n_lpadding;
    Py_ssize_t n_prefix;
    Py_ssize_t n_spadding;
    Py_ssize_t n_rpadding;
    char sign;
    Py_ssize_t n_sign;      /* number of digits needed for sign (0/1) */
    Py_ssize_t n_grouped_digits; /* Space taken up by the digits, including
                                    any grouping chars. */
    Py_ssize_t n_decimal;   /* 0 if only an integer */
    Py_ssize_t n_remainder; /* Digits in decimal and/or exponent part,
                               excluding the decimal itself, if
                               present. */
    Py_ssize_t n_frac;
    Py_ssize_t n_grouped_frac_digits;

    /* These 2 are not the widths of fields, but are needed by
       STRINGLIB_GROUPING. */
    Py_ssize_t n_digits;    /* The number of digits before a decimal
                               or exponent. */
    Py_ssize_t n_min_width; /* The min_width we used when we computed
                               the n_grouped_digits width. */
} NumberFieldWidths;

#if PY_VERSION_HEX > 0x030D00A0
/* _PyUnicode_InsertThousandsGrouping() helper functions */

typedef struct {
    const char *grouping;
    char previous;
    Py_ssize_t i; /* Where we're currently pointing in grouping. */
} GroupGenerator;

static void
GroupGenerator_init(GroupGenerator *self, const char *grouping)
{
    self->grouping = grouping;
    self->i = 0;
    self->previous = 0;
}

/* Returns the next grouping, or 0 to signify end. */
static Py_ssize_t
GroupGenerator_next(GroupGenerator *self)
{
    /* Note that we don't really do much error checking here. If a
       grouping string contains just CHAR_MAX, for example, then just
       terminate the generator. That shouldn't happen, but at least we
       fail gracefully. */
    switch (self->grouping[self->i]) {
    case 0:
        return self->previous;
    case CHAR_MAX:
        /* Stop the generator. */
        return 0;
    default:
        {
            char ch = self->grouping[self->i];
            self->previous = ch;
            self->i++;
            return (Py_ssize_t)ch;
        }
    }
}

/* Fill in some digits, leading zeros, and thousands separator. All
   are optional, depending on when we're called. */
static void
InsertThousandsGrouping_fill(_PyUnicodeWriter *writer, Py_ssize_t *buffer_pos,
                             PyObject *digits, Py_ssize_t *digits_pos,
                             Py_ssize_t n_chars, Py_ssize_t n_zeros,
                             PyObject *thousands_sep, Py_ssize_t thousands_sep_len,
                             Py_UCS4 *maxchar)
{
    if (!writer) {
        /* if maxchar > 127, maxchar is already set */
        if (*maxchar == 127 && thousands_sep) {
            Py_UCS4 maxchar2 = PyUnicode_MAX_CHAR_VALUE(thousands_sep);
            *maxchar = Py_MAX(*maxchar, maxchar2);
        }
        return;
    }
    if (thousands_sep) {
        *buffer_pos -= thousands_sep_len;
        /* Copy the thousands_sep chars into the buffer. */
        PyUnicode_CopyCharacters(writer->buffer, *buffer_pos,
                                      thousands_sep, 0,
                                      thousands_sep_len);
    }
    *buffer_pos -= n_chars;
    *digits_pos -= n_chars;
    PyUnicode_CopyCharacters(writer->buffer, *buffer_pos,
                                  digits, *digits_pos,
                                  n_chars);
    if (n_zeros) {
        *buffer_pos -= n_zeros;
        PyUnicode_Fill(writer->buffer, *buffer_pos, n_zeros, '0');
    }
}

static Py_ssize_t
_PyUnicode_InsertThousandsGrouping(_PyUnicodeWriter *writer,
                                   Py_ssize_t n_buffer, PyObject *digits,
                                   Py_ssize_t d_pos, Py_ssize_t n_digits,
                                   Py_ssize_t min_width, const char *grouping,
                                   PyObject *thousands_sep, Py_UCS4 *maxchar)
{
    min_width = Py_MAX(0, min_width);
    if (writer) {
        assert(digits != NULL);
        assert(maxchar == NULL);
    }
    else {
        assert(digits == NULL);
        assert(maxchar != NULL);
    }
    assert(0 <= d_pos);
    assert(0 <= n_digits);
    assert(grouping != NULL);

    Py_ssize_t count = 0;
    Py_ssize_t n_zeros;
    int loop_broken = 0;
    int use_separator = 0; /* First time through, don't append the
                              separator. They only go between
                              groups. */
    Py_ssize_t buffer_pos;
    Py_ssize_t digits_pos;
    Py_ssize_t len;
    Py_ssize_t n_chars;
    Py_ssize_t remaining = n_digits; /* Number of chars remaining to
                                        be looked at */
    /* A generator that returns all of the grouping widths, until it
       returns 0. */
    GroupGenerator groupgen;
    GroupGenerator_init(&groupgen, grouping);
    const Py_ssize_t thousands_sep_len = PyUnicode_GET_LENGTH(thousands_sep);

    /* if digits are not grouped, thousands separator
       should be an empty string */
    assert(!(grouping[0] == CHAR_MAX && thousands_sep_len != 0));

    digits_pos = d_pos + n_digits;
    if (writer) {
        buffer_pos = writer->pos + n_buffer;
        assert(buffer_pos <= PyUnicode_GET_LENGTH(writer->buffer));
        assert(digits_pos <= PyUnicode_GET_LENGTH(digits));
    }
    else {
        buffer_pos = n_buffer;
    }
    if (!writer) {
        *maxchar = 127;
    }
    while ((len = GroupGenerator_next(&groupgen)) > 0) {
        len = Py_MIN(len, Py_MAX(Py_MAX(remaining, min_width), 1));
        n_zeros = Py_MAX(0, len - remaining);
        n_chars = Py_MAX(0, Py_MIN(remaining, len));
        /* Use n_zero zero's and n_chars chars
           Count only, don't do anything. */
        count += (use_separator ? thousands_sep_len : 0) + n_zeros + n_chars;
        /* Copy into the writer. */
        InsertThousandsGrouping_fill(writer, &buffer_pos,
                                     digits, &digits_pos,
                                     n_chars, n_zeros,
                                     use_separator ? thousands_sep : NULL,
                                     thousands_sep_len, maxchar);
        /* Use a separator next time. */
        use_separator = 1;
        remaining -= n_chars;
        min_width -= len;
        if (remaining <= 0 && min_width <= 0) {
            loop_broken = 1;
            break;
        }
        min_width -= thousands_sep_len;
    }
    if (!loop_broken) {
        /* We left the loop without using a break statement. */
        len = Py_MAX(Py_MAX(remaining, min_width), 1);
        n_zeros = Py_MAX(0, len - remaining);
        n_chars = Py_MAX(0, Py_MIN(remaining, len));
        /* Use n_zero zero's and n_chars chars */
        count += (use_separator ? thousands_sep_len : 0) + n_zeros + n_chars;
        /* Copy into the writer. */
        InsertThousandsGrouping_fill(writer, &buffer_pos,
                                     digits, &digits_pos,
                                     n_chars, n_zeros,
                                     use_separator ? thousands_sep : NULL,
                                     thousands_sep_len, maxchar);
    }
    return count;
}
#endif

/* not all fields of format are used.  for example, precision is
   unused.  should this take discrete params in order to be more clear
   about what it does?  or is passing a single format parameter easier
   and more efficient enough to justify a little obfuscation?
   Return -1 on error. */
static Py_ssize_t
calc_number_widths(NumberFieldWidths *spec, Py_ssize_t n_prefix,
                   Py_UCS4 sign_char, Py_ssize_t n_start,
                   Py_ssize_t n_end, Py_ssize_t n_remainder, Py_ssize_t n_frac,
                   int has_decimal, const LocaleInfo *locale,
                   const InternalFormatSpec *format, Py_UCS4 *maxchar)
{
    Py_ssize_t n_non_digit_non_padding;
    Py_ssize_t n_padding;

    spec->n_digits = n_end - n_start - n_frac - n_remainder - (has_decimal?1:0);
    spec->n_lpadding = 0;
    spec->n_prefix = n_prefix;
    spec->n_decimal = has_decimal ? PyUnicode_GET_LENGTH(locale->decimal_point) : 0;
    spec->n_remainder = n_remainder;
    spec->n_frac = n_frac;
    spec->n_spadding = 0;
    spec->n_rpadding = 0;
    spec->sign = '\0';
    spec->n_sign = 0;

    /* the output will look like:
       |                                                                                         |
       | <lpadding> <sign> <prefix> <spadding> <grouped_digits> <decimal> <remainder> <rpadding> |
       |                                                                                         |

       sign is computed from format->sign and the actual
       sign of the number

       prefix is given (it's for the '0x' prefix)

       digits is already known

       the total width is either given, or computed from the
       actual digits

       only one of lpadding, spadding, and rpadding can be non-zero,
       and it's calculated from the width and other fields
    */

    /* compute the various parts we're going to write */
    switch (format->sign) {
    case '+':
        /* always put a + or - */
        spec->n_sign = 1;
        spec->sign = (sign_char == '-' ? '-' : '+');
        break;
    case ' ':
        spec->n_sign = 1;
        spec->sign = (sign_char == '-' ? '-' : ' ');
        break;
    default:
        /* Not specified, or the default (-) */
        if (sign_char == '-') {
            spec->n_sign = 1;
            spec->sign = '-';
        }
    }
    spec->n_grouped_frac_digits = 0;
    /* The number of chars used for non-digits and non-padding. */
    n_non_digit_non_padding = spec->n_sign + spec->n_prefix + spec->n_decimal +
        + spec->n_frac + spec->n_remainder;
    /* min_width can go negative, that's okay. format->width == -1 means
       we don't care. */
    if (format->fill_char == '0' && format->align == '=') {
        spec->n_min_width = (format->width - n_non_digit_non_padding
                             + spec->n_frac - spec->n_grouped_frac_digits);
    }
    else {
        spec->n_min_width = 0;
    }
    if (spec->n_digits == 0)
        /* This case only occurs when using 'c' formatting, we need
           to special case it because the grouping code always wants
           to have at least one character. */
        spec->n_grouped_digits = 0;
    else {
        Py_UCS4 grouping_maxchar;
        spec->n_grouped_digits = _PyUnicode_InsertThousandsGrouping(
            NULL, 0,
            NULL, 0, spec->n_digits,
            spec->n_min_width,
            locale->grouping, locale->thousands_sep, &grouping_maxchar);
        if (spec->n_grouped_digits == -1) {
            return -1; /* LCOV_EXCL_LINE */
        }
        *maxchar = Py_MAX(*maxchar, grouping_maxchar);
    }
    /* Given the desired width and the total of digit and non-digit
       space we consume, see if we need any padding. format->width can
       be negative (meaning no padding), but this code still works in
       that case. */
    n_padding = format->width - (n_non_digit_non_padding
                                 + spec->n_grouped_digits
                                 + spec->n_grouped_frac_digits
                                 - spec->n_frac);
    if (n_padding > 0) {
        /* Some padding is needed. Determine if it's left, space, or right. */
        switch (format->align) {
        case '<':
            spec->n_rpadding = n_padding;
            break;
        case '^':
            spec->n_lpadding = n_padding / 2;
            spec->n_rpadding = n_padding - spec->n_lpadding;
            break;
        case '=':
            spec->n_spadding = n_padding;
            break;
        case '>':
            spec->n_lpadding = n_padding;
            break;
        /* LCOV_EXCL_START */
        default:
            Py_UNREACHABLE();
        /* LCOV_EXCL_STOP */
        }
    }
    if (spec->n_lpadding || spec->n_spadding || spec->n_rpadding) {
        *maxchar = Py_MAX(*maxchar, format->fill_char);
    }

    return (spec->n_lpadding + spec->n_sign + spec->n_prefix
            + spec->n_spadding + spec->n_grouped_digits + spec->n_decimal
            + spec->n_grouped_frac_digits + spec->n_remainder
            + spec->n_rpadding);
}

/* Fill in the digit parts of a number's string representation,
   as determined in calc_number_widths().
   Return -1 on error, or 0 on success. */
static int
fill_number(PyUnicodeWriter *writer, const NumberFieldWidths *spec,
            PyObject *digits, Py_ssize_t d_start, PyObject *prefix,
            Py_ssize_t p_start, Py_UCS4 fill_char, LocaleInfo *locale)
{
    /* Used to keep track of digits, decimal, and remainder. */
    Py_ssize_t d_pos = d_start;
    Py_ssize_t r;
    _PyUnicodeWriter *_writer = (_PyUnicodeWriter *)writer;

    if (spec->n_lpadding) {
        for (Py_ssize_t i = 0; i < spec->n_lpadding; i++) {
            PyUnicodeWriter_WriteChar(writer, fill_char);
        }
    }
    if (spec->n_sign == 1) {
        PyUnicodeWriter_WriteChar(writer, (Py_UCS4)spec->sign);
    }
    if (spec->n_prefix) {
        PyUnicodeWriter_WriteSubstring(writer, prefix, p_start, spec->n_prefix + p_start);
    }
    if (spec->n_spadding) {
        for (Py_ssize_t i = 0; i < spec->n_spadding; i++) {
            PyUnicodeWriter_WriteChar(writer, fill_char);
        }
    }
    /* Only for type 'c' special case, it has no digits. */
    if (spec->n_digits != 0) {
        /* Fill the digits with InsertThousandsGrouping. */
        r = _PyUnicode_InsertThousandsGrouping(_writer, spec->n_grouped_digits,
                                               digits, d_pos, spec->n_digits,
                                               spec->n_min_width,
                                               locale->grouping,
                                               locale->thousands_sep, NULL);
        if (r == -1) {
            return -1; /* LCOV_EXCL_LINE */
        }
        assert(r == spec->n_grouped_digits);
        d_pos += spec->n_digits;
    }
    ((_PyUnicodeWriter *)writer)->pos += spec->n_grouped_digits;
    if (spec->n_remainder) {
        PyUnicodeWriter_WriteSubstring(writer, digits, d_pos, spec->n_remainder + d_pos);
    }
    if (spec->n_rpadding) {
        for (Py_ssize_t i = 0; i < spec->n_rpadding; i++) {
            PyUnicodeWriter_WriteChar(writer, fill_char);
        }
    }
    return 0;
}

#if PY_VERSION_HEX > 0x030D00A0
static char *
_PyMem_Strdup(const char *str)
{
    assert(str != NULL);
    size_t size = strlen(str) + 1;
    char *copy = PyMem_Malloc(size);
    if (copy == NULL) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    memcpy(copy, str, size);
    return copy;
}
#endif

static int
_Py_GetLocaleconvNumeric(struct lconv *lc,
                         PyObject **decimal_point, PyObject **thousands_sep)
{
    assert(decimal_point != NULL);
    assert(thousands_sep != NULL);
#ifndef MS_WINDOWS
    int change_locale = 0;
    if (strlen(lc->decimal_point) > 1
        || ((unsigned char)lc->decimal_point[0]) > 127)
    {
        change_locale = 1;
    }
    if (strlen(lc->thousands_sep) > 1
        || ((unsigned char)lc->thousands_sep[0]) > 127)
    {
        change_locale = 1;
    }

    /* Keep a copy of the LC_CTYPE locale */
    char *oldloc = NULL, *loc = NULL;

    if (change_locale) {
        oldloc = setlocale(LC_CTYPE, NULL);
        if (!oldloc) {
            /* LCOV_EXCL_START */
            PyErr_SetString(PyExc_RuntimeWarning,
                            "failed to get LC_CTYPE locale");
            return -1;
            /* LCOV_EXCL_STOP */
        }
        oldloc = _PyMem_Strdup(oldloc);
        if (!oldloc) {
            /* LCOV_EXCL_START */
            PyErr_NoMemory();
            return -1;
            /* LCOV_EXCL_STOP */
        }
        loc = setlocale(LC_NUMERIC, NULL);
        if (loc != NULL && strcmp(loc, oldloc) == 0) {
            loc = NULL;
        }
        if (loc != NULL) {
            /* Only set the locale temporarily the LC_CTYPE locale
               if LC_NUMERIC locale is different than LC_CTYPE locale and
               decimal_point and/or thousands_sep are non-ASCII or longer than
               1 byte */
            setlocale(LC_CTYPE, loc);
        }
    }

#define GET_LOCALE_STRING(ATTR) PyUnicode_DecodeLocale(lc->ATTR, NULL)
#else /* MS_WINDOWS */
/* Use _W_* fields of Windows strcut lconv */
#define GET_LOCALE_STRING(ATTR) PyUnicode_FromWideChar(lc->_W_ ## ATTR, -1)
#endif /* MS_WINDOWS */
    int res = -1;

    *decimal_point = GET_LOCALE_STRING(decimal_point);
    if (*decimal_point == NULL) {
        goto done; /* LCOV_EXCL_LINE */
    }
    *thousands_sep = GET_LOCALE_STRING(thousands_sep);
    if (*thousands_sep == NULL) {
        goto done; /* LCOV_EXCL_LINE */
    }
    res = 0;
done:
#ifndef MS_WINDOWS
    if (loc != NULL) {
        setlocale(LC_CTYPE, oldloc);
    }
    PyMem_Free(oldloc);
#endif
    return res;
#undef GET_LOCALE_STRING
}

static const char no_grouping[1] = {CHAR_MAX};

/* Find the decimal point character(s?), thousands_separator(s?), and
   grouping description, either for the current locale if type is
   LT_CURRENT_LOCALE, a hard-coded locale if LT_DEFAULT_LOCALE or
   LT_UNDERSCORE_LOCALE/LT_UNDER_FOUR_LOCALE, or none if LT_NO_LOCALE. */
static int
get_locale_info(enum LocaleType type, enum LocaleType frac_type,
                LocaleInfo *locale_info)
{
    switch (type) {
    case LT_CURRENT_LOCALE: {
        struct lconv *lc = localeconv();
        if (_Py_GetLocaleconvNumeric(lc,
                                     &locale_info->decimal_point,
                                     &locale_info->thousands_sep) < 0)
        {
            return -1; /* LCOV_EXCL_LINE */
        }
        /* localeconv() grouping can become a dangling pointer or point
           to a different string if another thread calls localeconv() during
           the string formatting. Copy the string to avoid this risk. */
        locale_info->grouping_buffer = _PyMem_Strdup(lc->grouping);
        if (locale_info->grouping_buffer == NULL) {
            /* LCOV_EXCL_START */
            PyErr_NoMemory();
            return -1;
            /* LCOV_EXCL_STOP */
        }
        locale_info->grouping = locale_info->grouping_buffer;
        break;
    }
    case LT_DEFAULT_LOCALE:
    case LT_UNDERSCORE_LOCALE:
    case LT_UNDER_FOUR_LOCALE:
        locale_info->decimal_point = PyUnicode_FromOrdinal('.');
        locale_info->thousands_sep = PyUnicode_FromOrdinal(
            type == LT_DEFAULT_LOCALE ? ',' : '_');
        if (!locale_info->decimal_point || !locale_info->thousands_sep) {
            return -1;  /* LCOV_EXCL_LINE */
        }
        if (type != LT_UNDER_FOUR_LOCALE) {
            locale_info->grouping = "\3"; /* Group every 3 characters.  The
                                         (implicit) trailing 0 means repeat
                                         infinitely. */
        }
        else {
            locale_info->grouping = "\4"; /* Bin/oct/hex group every four. */
        }
        break;
    case LT_NO_LOCALE:
        locale_info->decimal_point = PyUnicode_FromOrdinal('.');
        locale_info->thousands_sep = Py_GetConstant(Py_CONSTANT_EMPTY_STR);
        if (!locale_info->decimal_point || !locale_info->thousands_sep) {
            return -1; /* LCOV_EXCL_LINE */
        }
        locale_info->grouping = no_grouping;
        break;
    }
    return 0;
}

static void
free_locale_info(LocaleInfo *locale_info)
{
    Py_XDECREF(locale_info->decimal_point);
    Py_XDECREF(locale_info->thousands_sep);
    Py_XDECREF(locale_info->frac_thousands_sep);
    PyMem_Free(locale_info->grouping_buffer);
}

extern PyObject * MPZ_to_str(MPZ_Object *u, int base, int options);
extern int OPT_PREFIX;

static PyObject *
format_long_internal(MPZ_Object *value, const InternalFormatSpec *format)
{
    Py_UCS4 maxchar = 127;
    PyObject *tmp = NULL;
    Py_ssize_t inumeric_chars;
    Py_UCS4 sign_char = '\0';
    Py_ssize_t n_digits;       /* count of digits need from the computed
                                  string */
    Py_ssize_t n_remainder = 0; /* Used only for 'c' formatting, which
                                   produces non-digits */
    Py_ssize_t n_prefix = 0;   /* Count of prefix chars, (e.g., '0x') */
    Py_ssize_t n_total;
    Py_ssize_t prefix = 0;
    NumberFieldWidths spec;
    zz_slimb_t x = -1;

    /* Locale settings, either from the actual locale or
       from a hard-code pseudo-locale */
    LocaleInfo locale = LocaleInfo_STATIC_INIT;

    /* no precision allowed on integers */
    if (format->precision != -1) {
        PyErr_SetString(PyExc_ValueError,
                        "Precision not allowed in integer format specifier");
        goto done;
    }
    /* no negative zero coercion on integers */
    if (format->no_neg_0) {
        PyErr_SetString(PyExc_ValueError,
                        "Negative zero coercion (z) not allowed in integer"
                        " format specifier");
        goto done;
    }
    /* special case for character formatting */
    if (format->type == 'c') {
        /* error to specify a sign */
        if (format->sign != '\0') {
            PyErr_SetString(PyExc_ValueError,
                            "Sign not allowed with integer"
                            " format specifier 'c'");
            goto done;
        }
        /* error to request alternate format */
        if (format->alternate) {
            PyErr_SetString(PyExc_ValueError,
                            "Alternate form (#) not allowed with integer"
                            " format specifier 'c'");
            goto done;
        }
        /* taken from unicodeobject.c formatchar() */
        /* Integer input truncated to a character */
        if (zz_to_sl(&value->z, &x) || x < 0 || x > 0x10ffff) {
            PyErr_SetString(PyExc_OverflowError,
                            "%c arg not in range(0x110000)");
            goto done;
        }
        tmp = PyUnicode_FromOrdinal((int)x);
        inumeric_chars = 0;
        n_digits = 1;
        maxchar = Py_MAX(maxchar, (Py_UCS4)x);
        /* As a sort-of hack, we tell calc_number_widths that we only
           have "remainder" characters. calc_number_widths thinks
           these are characters that don't get formatted, only copied
           into the output string. We do this for 'c' formatting,
           because the characters are likely to be non-digits. */
        n_remainder = 1;
    }
    else {
        int base;
        int leading_chars_to_skip = 0;  /* Number of characters added by
                                           PyNumber_ToBase that we want to
                                           skip over. */

        /* Compute the base and how many characters will be added by
           PyNumber_ToBase */
        switch (format->type) {
        case 'b':
            base = 2;
            leading_chars_to_skip = 2; /* 0b */
            break;
        case 'o':
            base = 8;
            leading_chars_to_skip = 2; /* 0o */
            break;
        case 'x':
            base = 16;
            leading_chars_to_skip = 2; /* 0x */
            break;
        case 'X':
            base = -16;
            leading_chars_to_skip = 2; /* 0x */
            break;
        default:  /* shouldn't be needed, but stops a compiler warning */
        case 'd':
        case 'n':
            base = 10;
            break;
        }
        if (format->sign != '+' && format->sign != ' '
            && format->width == -1 && format->type != 'n'
            && !format->thousands_separators
            && MPZ_CheckExact(value))
        {
            /* Fast path */
            return MPZ_to_str(value, base, format->alternate ? OPT_PREFIX : 0);
        }
        /* Do the hard part, converting to a string in a given base */
        tmp = MPZ_to_str(value, base, OPT_PREFIX);
        if (tmp == NULL) {
            goto done; /* LCOV_EXCL_LINE */
        }
        /* The number of prefix chars is the same as the leading
           chars to skip */
        if (format->alternate) {
            n_prefix = leading_chars_to_skip;
        }
        inumeric_chars = 0;
        n_digits = PyUnicode_GET_LENGTH(tmp);
        prefix = inumeric_chars;
        /* Is a sign character present in the output?  If so, remember it
           and skip it */
        if (PyUnicode_READ_CHAR(tmp, inumeric_chars) == '-') {
            sign_char = '-';
            ++prefix;
            ++leading_chars_to_skip;
        }

        /* Skip over the leading chars (0x, 0b, etc.) */
        n_digits -= leading_chars_to_skip;
        inumeric_chars += leading_chars_to_skip;
    }
    /* Determine the grouping, separator, and decimal point, if any. */
    if (get_locale_info(format->type == 'n' ? LT_CURRENT_LOCALE :
                        format->thousands_separators, 0,
                        &locale) == -1)
    {
        goto done; /* LCOV_EXCL_LINE */
    }
    /* Calculate how much memory we'll need. */
    n_total = calc_number_widths(&spec, n_prefix, sign_char, inumeric_chars,
                                 inumeric_chars + n_digits, n_remainder, 0, 0,
                                 &locale, format, &maxchar);
    if (n_total == -1) {
        goto done; /* LCOV_EXCL_LINE */
    }
    /* Allocate the memory. */

    PyUnicodeWriter *writer = PyUnicodeWriter_Create(n_total);

    if (!writer || PyUnicodeWriter_WriteChar(writer, maxchar)) {
        goto done; /* LCOV_EXCL_LINE */
    }
    ((_PyUnicodeWriter *)writer)->pos = 0;
    /* Populate the memory. */
    if (fill_number(writer, &spec, tmp, inumeric_chars, tmp, prefix,
                    format->fill_char, &locale))
    {
        /* LCOV_EXCL_START */
        PyUnicodeWriter_Discard(writer);
        goto done;
        /* LCOV_EXCL_STOP */
    }
    return PyUnicodeWriter_Finish(writer);
done:
    Py_XDECREF(tmp);
    free_locale_info(&locale);
    return NULL;
}

extern PyObject * to_float(PyObject *self);

PyObject *
__format__(PyObject *self, PyObject *format_spec)
{
    Py_ssize_t end = PyUnicode_GET_LENGTH(format_spec);

    if (!end) {
       return PyObject_Str(self);
    }

    InternalFormatSpec format;

    if (!parse_internal_render_format_spec(self, format_spec, 0, end,
                                           &format, 'd', '>'))
    {
        return NULL; /* LCOV_EXCL_LINE */
    }
    switch (format.type) {
    case 'b':
    case 'c':
    case 'd':
    case 'o':
    case 'x':
    case 'X':
    case 'n':
        return format_long_internal((MPZ_Object *)self, &format);
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case '%':
    {
        PyObject *flt = PyNumber_Float(self);

        if (!flt) {
            return NULL; /* LCOV_EXCL_LINE */
        }

        PyObject *res = PyObject_CallMethod(flt, "__format__", "O",
                                            format_spec);

        Py_DECREF(flt);
        return res;
    }
    default:
        unknown_presentation_type(format.type, Py_TYPE(self)->tp_name);
        return NULL;
    }
}
#else
extern PyObject * to_int(PyObject *self);

PyObject *
__format__(PyObject *self, PyObject *format_spec)
{
    PyObject *integer = to_int(self);

    if (!integer) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    PyObject *res = PyObject_CallMethod(integer, "__format__", "O",
                                        format_spec);

    Py_DECREF(integer);
    return res;
}
#endif /* !defined(PYPY_VERSION) && !defined(GRAALVM_PYTHON) */
