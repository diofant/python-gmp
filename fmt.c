#include "mpz.h"

extern PyObject * to_int(PyObject *self);

#if defined(ON_CPYTHON) && PY_VERSION_HEX >= 0x030D00A0

/************************************************************************/
/*********** standard format specifier parsing **************************/
/************************************************************************/

/* Accomulates decimal digit characters from an input string,
   returns nonnegative integer or -1 on error.
*/
Py_LOCAL(Py_ssize_t)
get_integer(const char *str, Py_ssize_t *ppos, Py_ssize_t end)
{
    Py_ssize_t accumulator = 0, digitval, pos = *ppos;

    for (; pos < end; pos++) {
        digitval = str[pos] - '0';
        if (digitval < 0 || digitval > 9) {
            break;
        }
        /* Detect possible overflow before it happens */
        if (accumulator > (PY_SSIZE_T_MAX - digitval) / 10) {
            PyErr_Format(PyExc_ValueError,
                         "Too many decimal digits in format string");
            *ppos = pos;
            return -1;
        }
        accumulator = accumulator * 10 + digitval;
    }
    *ppos = pos;
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
    int alternate;
    char sign;
    Py_ssize_t width;
    char group;
    int base;
    char type;
} InternalFormatSpec;

/*
  ptr points to the start of the format_spec, end points just past its end.
  fills in format with the parsed information.
  returns 1 on success, 0 on failure.
  if failure, sets the exception
*/
Py_LOCAL(int)
parse_internal_render_format_spec(PyObject *obj,
                                  PyObject *format_spec,
                                  Py_ssize_t start, Py_ssize_t end,
                                  InternalFormatSpec *format)
{
    Py_ssize_t pos = start;
    const char *data = PyUnicode_AsUTF8AndSize(format_spec, NULL);
    int align_specified = 0;
    int fill_char_specified = 0;
    int thousands_separators = 0;

    format->fill_char = ' ';
    format->align = '>';
    format->alternate = 0;
    format->sign = '\0';
    format->width = -1;
    format->group = 0;
    format->base = 10;
    format->type = 'd';
    if (!data) {
        return 0; /* LCOV_EXCL_LINE */
    }
    /* If the second char is an alignment token, then parse the fill char */
    if (end-pos >= 2 && is_alignment_token(data[pos+1])) {
        format->align = data[pos+1];
        format->fill_char = data[pos];
        fill_char_specified = 1;
        align_specified = 1;
        pos += 2;
    }
    else if (end-pos >= 1 && is_alignment_token(data[pos])) {
        format->align = data[pos];
        align_specified = 1;
        ++pos;
    }
    /* Parse the various sign options */
    if (end-pos >= 1 && is_sign_element(data[pos])) {
        format->sign = data[pos];
        ++pos;
    }
    /* If the next character is #, we're in alternate mode */
    if (end-pos >= 1 && data[pos] == '#') {
        format->alternate = 1;
        ++pos;
    }
    /* The special case for 0-padding (backwards compat) */
    if (!fill_char_specified && end-pos >= 1 && data[pos] == '0') {
        format->fill_char = '0';
        if (!align_specified) {
            format->align = '=';
        }
        ++pos;
    }
    format->width = get_integer(data, &pos, end);
    if (format->width == -1) {
        return 0; /* overflow */
    }
    /* If we didn't consume any characters, reset the width to -1 */
    if (format->width == 0) {
        format->width = -1;
    }
    /* Underscore signifies add thousands separators */
    if (end-pos && data[pos] == '_') {
        thousands_separators = 1;
        ++pos;
    }
    /* Finally, parse the type field. */
    if (end-pos > 1) {
        /* More than one char remains, so this is an invalid format
           specifier. */
        /* Create a temporary object that contains the format spec we're
           operating on.  It's format_spec[start:end] (in Python syntax). */
        PyObject* actual_format_spec = PyUnicode_FromStringAndSize(data
                                                                   + start,
                                                                   end-start);
        if (actual_format_spec != NULL) {
            PyErr_Format(PyExc_ValueError,
                         ("Invalid format specifier '%U' for object "
                          "of type '%.200s'"), actual_format_spec,
                         PyType_GetFullyQualifiedName(Py_TYPE(obj)));
            Py_DECREF(actual_format_spec);
        }
        return 0;
    }
    if (end-pos == 1) {
        format->type = data[pos];
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
            break;
        }
        ++pos;
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

/* describes the layout for an integer, see the comment in
   calc_number_widths() for details */
typedef struct {
    Py_ssize_t n_lpadding;
    Py_ssize_t n_prefix;
    Py_ssize_t n_spadding;
    Py_ssize_t n_rpadding;
    char sign;
    Py_ssize_t n_sign; /* number of digits needed for sign (0/1) */
    Py_ssize_t n_digits; /* The number of digits, including separators. */
} NumberFieldWidths;

Py_LOCAL(Py_ssize_t)
calc_number_widths(NumberFieldWidths *spec, Py_ssize_t n_prefix,
                   char sign_char, Py_ssize_t n_start,
                   Py_ssize_t n_end, const InternalFormatSpec *format)
{
    spec->n_digits = n_end - n_start;
    spec->n_lpadding = 0;
    spec->n_prefix = n_prefix;
    spec->n_spadding = 0;
    spec->n_rpadding = 0;
    spec->sign = '\0';
    spec->n_sign = 0;
    /* The output will look like:
       |                                                           |
       | <lpadding> <sign> <prefix> <spadding> <digits> <rpadding> |
       |                                                           |

       Sign is computed from format->sign and the actual sign of the number.
       Prefix is given (it's for the '0x' prefix).
       Digits is already known.
       The total width is either given, or computed from the
       actual digits.

       Only one of lpadding, spadding, and rpadding can be non-zero,
       and it's calculated from the width and other fields.
    */
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
    case '-':
    default:
        if (sign_char == '-') {
            spec->n_sign = 1;
            spec->sign = '-';
        }
    }

    /* Given the desired width and the total of digit and non-digit
       space we consume, see if we need any padding. format->width can
       be negative (meaning no padding), but this code still works in
       that case. */
    Py_ssize_t n_padding = format->width - (spec->n_sign + spec->n_prefix
                                            + spec->n_digits);

    if (n_padding > 0) {
        /* Some padding is needed.  Determine if it's left, space, or right. */
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
            + spec->n_spadding + spec->n_digits + spec->n_rpadding);
}

/* Fill in the digit parts of a number's string representation,
   as determined in calc_number_widths().
   Return -1 on error, or 0 on success. */
Py_LOCAL(int)
fill_number(PyUnicodeWriter *writer, const NumberFieldWidths *spec,
            PyObject *digits, Py_ssize_t d_start,
            Py_ssize_t p_start, char fill_char)
{
    if (spec->n_lpadding) {
        for (Py_ssize_t i = 0; i < spec->n_lpadding; i++) {
            if (PyUnicodeWriter_WriteChar(writer, (Py_UCS4)fill_char)) {
                return -1; /* LCOV_EXCL_LINE */
            }
        }
    }
    if (spec->n_sign == 1) {
        if(PyUnicodeWriter_WriteChar(writer, (Py_UCS4)spec->sign)) {
            return -1; /* LCOV_EXCL_LINE */
        }
    }
    if (spec->n_prefix) {
        if (PyUnicodeWriter_WriteSubstring(writer, digits, p_start,
                                           spec->n_prefix + p_start))
        {
            return -1; /* LCOV_EXCL_LINE */
        }
    }
    if (spec->n_spadding) {
        for (Py_ssize_t i = 0; i < spec->n_spadding; i++) {
            if (PyUnicodeWriter_WriteChar(writer, (Py_UCS4)fill_char)) {
                return -1; /* LCOV_EXCL_LINE */
            }
        }
    }
    if (PyUnicodeWriter_WriteSubstring(writer, digits, d_start,
                                       spec->n_digits + d_start))
    {
        return -1; /* LCOV_EXCL_LINE */
    }
    if (spec->n_rpadding) {
        for (Py_ssize_t i = 0; i < spec->n_rpadding; i++) {
            if (PyUnicodeWriter_WriteChar(writer, (Py_UCS4)fill_char)) {
                return -1; /* LCOV_EXCL_LINE */
            }
        }
    }
    return 0;
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
MPZ_format(MPZ_Object *u, const InternalFormatSpec *format)
{
    size_t len = 0;
    bool negative = zz_isneg(&u->z);
    bool sign = format->sign == '+' || format->sign == ' ';
    Py_ssize_t min_leading = 0, width = -1;

    if (format->fill_char == '0' && format->align == '=') {
        width = format->width;
    }
    sign |= negative;
    (void)zz_sizeinbase(&u->z, format->base, &len);
    if (format->alternate) {
        len += 2;
    }
    min_leading = width - (Py_ssize_t)len - sign;
    if (min_leading > 0) {
        if (format->group) {
            min_leading = ((format->group*(width - sign))/(format->group + 1)
                           + 1 - (Py_ssize_t)len);
        }
        if (min_leading > 0) {
            len += (size_t)min_leading;
        }
    }
    if (format->group) {
        len += (len - 1) / (size_t)format->group;
    }
    len += sign;
    len++; /* '\0' */

    char *buf = malloc(len), *p = buf, saved_char = 0;

    if (!buf) {
        return PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    if (negative) {
        saved_char = '-';
        *(p++) = saved_char;
    }
    if (format->alternate) {
        if (format->base == 2) {
            *(p++) = '0';
            *(p++) = 'b';
        }
        else if (format->base == 8) {
            *(p++) = '0';
            *(p++) = 'o';
        }
        else if (format->base == 16) {
            *(p++) = '0';
            *(p++) = 'x';
        }
        else if (format->base == -16) {
            *(p++) = '0';
            *(p++) = 'X';
        }
    }
    if (saved_char) {
        saved_char = *(--p);
        assert(saved_char);
    }
    for (int i = 0; i < min_leading; i++) {
        *(p++) = '0';
    }

    zz_err ret = zz_get_str(&u->z, format->base, p);

    if (min_leading > 0) {
       if (negative) {
           *p = '0';
       }
       p -= min_leading;
    }
    p += negative;
    if (format->group && u->z.size) {
        insert_from_end_inplace(p, format->group, '_');
    }
    p -= negative;
    if (saved_char) {
        *p = saved_char;
    }
    if (ret) {
        /* LCOV_EXCL_START */
        free(buf);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    p += strlen(p);

    PyObject *res = PyUnicode_FromString(buf);

    free(buf);
    return res;
}

Py_LOCAL(PyObject *)
format_mpz_internal(MPZ_Object *value, const InternalFormatSpec *format)
{
    PyObject *tmp = NULL;
    Py_ssize_t inumeric_chars = 0;
    char sign_char = '\0';
    Py_ssize_t n_digits; /* Count of digits need from the computed string */
    Py_ssize_t n_prefix = 2; /* Count of prefix chars, (e.g., '0x') */
    Py_ssize_t n_total;
    Py_ssize_t prefix = 0;
    NumberFieldWidths spec;

    /* The number of prefix chars is the same as the leading
       chars to skip */
    if (!format->alternate || format->base == 10) {
        n_prefix = 0;
    }
    tmp = MPZ_format(value, format);
    if (tmp == NULL) {
        goto done; /* LCOV_EXCL_LINE */
    }
    n_digits = PyUnicode_GetLength(tmp);
    /* Is a sign character present in the output?  If so, remember it
       and skip it */
    if (PyUnicode_ReadChar(tmp, inumeric_chars) == '-') {
        sign_char = '-';
        prefix++;
        n_digits--;
        inumeric_chars++;
    }
    /* Skip over the leading chars (0x, 0b, etc.) */
    n_digits -= n_prefix;
    inumeric_chars += n_prefix;
    /* Calculate how much memory we'll need. */
    n_total = calc_number_widths(&spec, n_prefix, sign_char, inumeric_chars,
                                 inumeric_chars + n_digits, format);
    if (n_total == -1) {
        goto done; /* LCOV_EXCL_LINE */
    }

    /* Allocate the memory. */
    PyUnicodeWriter *writer = PyUnicodeWriter_Create(n_total);

    if (!writer) {
        goto done; /* LCOV_EXCL_LINE */
    }
    /* Populate the memory. */
    if (fill_number(writer, &spec, tmp, inumeric_chars, prefix,
                    format->fill_char))
    {
        /* LCOV_EXCL_START */
        PyUnicodeWriter_Discard(writer);
        goto done;
        /* LCOV_EXCL_STOP */
    }
    return PyUnicodeWriter_Finish(writer);
    /* LCOV_EXCL_START */
done:
    Py_XDECREF(tmp);
    return NULL;
    /* LCOV_EXCL_STOP */
}

extern PyObject * to_float(PyObject *self);

PyObject *
__format__(PyObject *self, PyObject *format_spec)
{
    if (!PyUnicode_Check(format_spec)) {
        PyErr_Format(PyExc_TypeError,
                     "__format__() argument must be str, not %U",
                     PyType_GetFullyQualifiedName(Py_TYPE(format_spec)));
        return NULL;
    }

    Py_ssize_t end = PyUnicode_GetLength(format_spec);

    if (!end) {
       return PyObject_Str(self);
    }

    InternalFormatSpec format;
    unaryfunc cast = to_int;

    if (!parse_internal_render_format_spec(self, format_spec, 0, end, &format))
    {
        PyErr_Clear();
        cast = to_int;
        goto fallback;
    }

    switch (format.type) {
    case 'b':
    case 'd':
    case 'o':
    case 'x':
    case 'X':
        return format_mpz_internal((MPZ_Object *)self, &format);
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
        {
            PyObject *type_name = PyType_GetFullyQualifiedName(Py_TYPE(self));

            /* %c might be out-of-range, hence the two cases. */
            if (format.type > 32) {
                PyErr_Format(PyExc_ValueError,
                             "Unknown format code '%c' for object of type '%U'",
                             (char)format.type, type_name);
            }
            else {
                PyErr_Format(PyExc_ValueError,
                             "Unknown format code '\\x%x' for object of type '%U'",
                             (unsigned int)format.type, type_name);
            }
        }
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
#else
PyObject *
__format__(PyObject *self, PyObject *format_spec)
{
    PyObject *num = to_int(self);

    if (!num) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    PyObject *res = PyObject_CallMethod(num, "__format__", "O", format_spec);

    Py_DECREF(num);
    return res;
}
#endif /* defined(ON_CPYTHON) && PY_VERSION_HEX >= 0x030D00A0 */
