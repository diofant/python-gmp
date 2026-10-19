#include "mpz.h"

/* Accomulates decimal digit characters from an input string,
   returns nonnegative integer or -1 on error.
*/
Py_LOCAL(Py_ssize_t)
get_integer(char **start, const char *end)
{
    char *pos = *start;
    Py_ssize_t accumulator = 0, digitval;

    for (; pos < end; pos++) {
        digitval = *pos - '0';
        if (digitval < 0 || digitval > 9) {
            break;
        }
        if (accumulator > (PY_SSIZE_T_MAX - digitval) / 10) {
            PyErr_Format(PyExc_ValueError,
                         "Too many decimal digits in format string");
            return -1;
        }
        accumulator = accumulator * 10 + digitval;
    }
    *start = pos;
    return accumulator;
}

Py_LOCAL_INLINE(int)
is_alignment_token(char c)
{
    switch (c) {
    case '<':
    case '>':
    case '=':
    case '^':
        return 1;
    default:
        return 0;
    }
}

Py_LOCAL_INLINE(int)
is_sign_element(char c)
{
    switch (c) {
    case ' ':
    case '+':
    case '-':
        return 1;
    default:
        return 0;
    }
}

typedef struct {
    char fill_char;
    char align;
    bool zeropad;
    bool alternate;
    char sign;
    Py_ssize_t width;
    char group;
    int base;
    char prefix[2];
    char type;
} IntFormatSpec;

/* Fills in format with the parsed information.  Returns 1 on success,
   0 on failure with an exception set.
*/
Py_LOCAL(int)
parse_int_format_spec(PyObject *obj, PyObject *format_spec,
                      Py_ssize_t len, IntFormatSpec *format)
{
    const char *start = PyUnicode_AsUTF8AndSize(format_spec, NULL);
    char *pos = (char *)start;
    const char *end = start + len;
    bool align_specified = false;
    bool fill_char_specified = false;
    bool thousands_separators = false;

    format->fill_char = ' ';
    format->align = '>';
    format->zeropad = false;
    format->alternate = false;
    format->sign = '\0';
    format->width = -1;
    format->group = 0;
    format->base = 10;
    format->prefix[0] = '0';
    format->prefix[1] = '\0';
    format->type = 'd';
    assert(pos);
    /* If the second char is an alignment token, then parse the fill char. */
    if (end-pos >= 2 && is_alignment_token(*(pos + 1))) {
        format->fill_char = *pos++;
        format->align = *pos++;
        fill_char_specified = true;
        align_specified = true;
    }
    else if (end-pos >= 1 && is_alignment_token(*pos)) {
        format->align = *pos++;
        align_specified = true;
    }
    /* Parse the various sign options. */
    if (end-pos >= 1 && is_sign_element(*pos)) {
        format->sign = *pos++;
    }
    /* If the next character is #, we're in alternate mode. */
    if (end-pos >= 1 && *pos == '#') {
        format->alternate = true;
        pos++;
    }
    /* The special case for 0-padding (backwards compat). */
    if (!fill_char_specified && end-pos >= 1 && *pos == '0') {
        format->fill_char = '0';
        if (!align_specified) {
            format->align = '=';
        }
        pos++;
    }
    format->width = get_integer(&pos, end);
    if (format->width == -1) {
        return 0; /* overflow */
    }
    /* If we didn't consume any characters, reset the width to -1. */
    if (format->width == 0) {
        format->width = -1;
    }
    if (format->fill_char == '0' && format->align == '='
        && format->width > 0)
    {
        format->zeropad = true;
    }
    /* Underscore signifies thousands separators. */
    if (end-pos && *pos == '_') {
        thousands_separators = true;
        pos++;
    }
    /* Finally, parse the type field. */
    if (end-pos > 1) {
        /* More than one char remains, so this is an invalid format
           specifier. */
        PyErr_Format(PyExc_ValueError,
                     ("Invalid format specifier '%U' for object "
                      "of type '%.200U'"), format_spec,
                     PyType_GetFullyQualifiedName(Py_TYPE(obj)));
        return 0;
    }
    if (end-pos == 1) {
        format->type = *pos++;
        switch (format->type) {
        case 'b':
            format->base = 2;
            break;
        case 'o':
            format->base = 8;
            break;
        case 'x':
            format->base = 16;
            break;
        case 'X':
            format->base = -16;
            break;
        default:
        case 'd':
            format->base = 10;
            format->alternate = 0;
            break;
        }
    }
    if (format->alternate) {
        if (format->base == 2) {
            format->prefix[1] = 'b';
        }
        else if (format->base == 8) {
            format->prefix[1] = 'o';
        }
        else if (format->base == 16) {
            format->prefix[1] = 'x';
        }
        else {
            format->prefix[1] = 'X';
        }
    }
    if (thousands_separators) {
        if (format->base == 10) {
            format->group = 3;
        }
        else {
            format->group = 4;
        }
    }
    return 1;
}

/* Describes the layout for an integer, see the comment in
   calc_number_widths() for details. */
typedef struct {
    Py_ssize_t n_lpadding;
    Py_ssize_t n_prefix;
    Py_ssize_t n_spadding;
    Py_ssize_t n_rpadding;
    char sign;
    Py_ssize_t n_leading_zeros;
    Py_ssize_t n_sign; /* number of digits needed for sign */
    Py_ssize_t n_digits; /* the number of digits, */
    Py_ssize_t n_grouped_digits;  /* ... including separators. */
} IntFieldWidths;

Py_LOCAL(Py_ssize_t)
calc_number_widths(bool negative, size_t len, const IntFormatSpec *format,
                   IntFieldWidths *spec)
{
    spec->n_lpadding = 0;
    spec->n_prefix = format->alternate ? 2 : 0;
    spec->n_spadding = 0;
    spec->n_rpadding = 0;
    spec->sign = '\0';
    spec->n_sign = 0;
    spec->n_leading_zeros = 0;
    spec->n_digits = (Py_ssize_t)len;
    spec->n_grouped_digits = spec->n_digits;
    /* The output will look like:
       |                                                                   |
       | <lpadding> <sign> <prefix> <spadding> <grouped_digits> <rpadding> |
       |                                                                   |

       Sign is computed from format->sign and the actual sign of the number.
       Prefix is given (it's for the '0x' prefix).
       Digits is already known.
       The total width is either given, or computed from the actual digits.
    */
    switch (format->sign) {
    case '+':
        spec->n_sign = 1;
        spec->sign = negative ? '-' : '+';
        break;
    case ' ':
        spec->n_sign = 1;
        spec->sign = negative ? '-' : ' ';
        break;
    case '-':
    default:
        if (negative) {
            spec->n_sign = 1;
            spec->sign = '-';
        }
    }
    if (format->group) {
        if (format->zeropad) {
            spec->n_leading_zeros = (format->group*(format->width
                                                    - spec->n_sign
                                                    - spec->n_prefix + 1)
                                     + 1)/(format->group + 1) - spec->n_digits;
            if (spec->n_leading_zeros < 0) {
                spec->n_leading_zeros = 0;
            }
            spec->n_digits += spec->n_leading_zeros;
        }
        spec->n_grouped_digits = spec->n_digits + (spec->n_digits - 1)/format->group;
    }

    /* Given the desired width and the total of digit and non-digit
       space we consume, see if we need any padding.  format->width can
       be negative (meaning no padding). */
    Py_ssize_t n_padding = format->width - (spec->n_sign + spec->n_prefix
                                            + spec->n_grouped_digits);

    if (n_padding > 0) {
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
        default:
            spec->n_lpadding = n_padding;
            break;
        }
    }
    return (spec->n_lpadding + spec->n_sign + spec->n_prefix
            + spec->n_spadding + spec->n_grouped_digits + spec->n_rpadding);
}

Py_LOCAL(void)
insert_from_end_inplace(char *str, Py_ssize_t n, char c)
{
    Py_ssize_t len = (Py_ssize_t)strlen(str);
    Py_ssize_t src = len, dest = len + (len - 1)/n, count = -1;

    assert(str && 0 < n);
    while (src >= 0) {
        if (count > 0 && count % n == 0 && src < len) {
            str[dest--] = c;
        }
        str[dest--] = str[src--];
        count++;
    }
}

Py_LOCAL(PyObject *)
MPZ_format(const MPZ_Object *value, const IntFormatSpec *format)
{
    size_t n_total;
    bool negative = zz_isneg(&value->z);
    IntFieldWidths spec;

    /* n_total will be exact, unless base = 10.  Then it could
       be 1 too big. */
    (void)zz_sizeinbase(&value->z, format->base, &n_total);
    n_total = (size_t)calc_number_widths(negative, n_total, format, &spec);

    char *buf = malloc(n_total + 1), *p = buf;

    if (buf == NULL || zz_get_str(&value->z, format->base, buf)) {
        /* LCOV_EXCL_START */
        free(buf);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    if (format->base == 10) {
        n_total = (size_t)calc_number_widths(negative, strlen(buf) - negative,
                                             format, &spec);
    }
    /* Fill in leading zeros and separators, if specified. */
    memmove(buf + spec.n_leading_zeros, buf + negative, strlen(buf) + 1);
    for (Py_ssize_t i = 0; i < spec.n_leading_zeros; i++) {
        buf[i] = '0';
    }
    if (spec.n_grouped_digits > spec.n_digits) {
        insert_from_end_inplace(buf, format->group, '_');
    }
    memmove(buf + spec.n_lpadding + spec.n_sign + spec.n_prefix + spec.n_spadding,
            buf, (size_t)spec.n_grouped_digits);
    /* Fill in the non-digit parts of a number's string representation,
       as determined in calc_number_widths(). */
    for (Py_ssize_t i = 0; i < spec.n_lpadding; i++) {
        *(p++) = format->fill_char;
    }
    if (spec.n_sign) {
        *(p++) = spec.sign;
    }
    if (spec.n_prefix) {
        *(p++) = format->prefix[0];
        *(p++) = format->prefix[1];
    }
    for (Py_ssize_t i = 0; i < spec.n_spadding; i++) {
        *(p++) = format->fill_char;
    }
    p += spec.n_grouped_digits;
    for (Py_ssize_t i = 0; i < spec.n_rpadding; i++) {
        *(p++) = format->fill_char;
    }
    *p = '\0';

    PyObject *res = PyUnicode_FromString(buf);

    free(buf);
    return res;
}

extern PyObject * to_float(PyObject *self);
extern PyObject * to_int(PyObject *self);

PyObject *
__format__(PyObject *self, PyObject *format_spec)
{
    if (!PyUnicode_Check(format_spec)) {
        PyErr_Format(PyExc_TypeError,
                     "__format__() argument must be str, not %U",
                     PyType_GetFullyQualifiedName(Py_TYPE(format_spec)));
        return NULL;
    }

    Py_ssize_t len = PyUnicode_GetLength(format_spec);

    if (!len) {
       return PyObject_Str(self);
    }

    IntFormatSpec format;
    unaryfunc cast = to_int;

    if (PyUnicode_IS_ASCII(format_spec)) {
        if (!parse_int_format_spec(self, format_spec, len, &format)) {
            PyErr_Clear();
            goto fallback;
        }
    }
    else {
        goto fallback;
    }
    switch (format.type) {
    case 'b':
    case 'd':
    case 'o':
    case 'x':
    case 'X':
        return MPZ_format((MPZ_Object *)self, &format);
    case 'c':
    case 'n':
        break;
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case '%':
        cast = to_float;
        break;
    default:
        PyErr_Format(PyExc_ValueError,
                     "Unknown format code '%c' for object of type '%U'",
                     format.type, PyType_GetFullyQualifiedName(Py_TYPE(self)));
        return NULL;
    }

fallback:
    {
        PyObject *num = cast(self);

        if (!num) {
            return NULL; /* LCOV_EXCL_LINE */
        }

        PyObject *res = PyObject_CallMethod(num, "__format__", "O", format_spec);

        Py_DECREF(num);
        return res;
    }
}
