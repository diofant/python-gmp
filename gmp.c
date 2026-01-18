#include "mpz.h"

#include <ctype.h>
#include <float.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if !defined(PYPY_VERSION)
#  define CACHE_SIZE (99)
#else
#  define CACHE_SIZE (0)
#endif
#define MAX_CACHE_MPZ_DIGITS (64)

typedef struct {
    MPZ_Object *gmp_cache[CACHE_SIZE + 1];
    size_t gmp_cache_size;
} gmp_global;

_Thread_local gmp_global global = {
    .gmp_cache_size = 0,
};

uint8_t bits_per_digit;
Py_hash_t pyhash_modulus;

static MPZ_Object *
MPZ_new(void)
{
    MPZ_Object *res;

    if (global.gmp_cache_size) {
        res = global.gmp_cache[--(global.gmp_cache_size)];
        if (zz_set(0, &res->z)) {
            /* LCOV_EXCL_START */
            global.gmp_cache[(global.gmp_cache_size)++] = res;
            return (MPZ_Object *)PyErr_NoMemory();
            /* LCOV_EXCL_STOP */
        }
        Py_XINCREF((PyObject *)res);
    }
    else {
        res = PyObject_New(MPZ_Object, &MPZ_Type);
        if (!res) {
            return NULL; /* LCOV_EXCL_LINE */
        }
        if (zz_init(&res->z)) {
            return (MPZ_Object *)PyErr_NoMemory(); /* LCOV_EXCL_LINE */
        }
    }
    res->hash_cache = -1;
    return res;
}

static const char *MPZ_TAG = "mpz(";
static int OPT_TAG = 0x1;
int OPT_PREFIX = 0x2;

PyObject *
MPZ_to_str(MPZ_Object *u, int base, int options)
{
    size_t len;
    bool negative = zz_isneg(&u->z);

    if (zz_sizeinbase(&u->z, base, &len)) {
        PyErr_SetString(PyExc_ValueError,
                        "mpz base must be >= 2 and <= 36");
        return NULL;
    }
    len += negative;
    if (options & OPT_TAG) {
        len += strlen(MPZ_TAG) + 1;
    }
    if (options & OPT_PREFIX) {
        len += 2;
    }
    len++;

    char *buf = malloc(len), *p = buf, saved_char = 0;

    if (!buf) {
        return PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    if (options & OPT_TAG) {
        strcpy(p, MPZ_TAG);
        p += strlen(MPZ_TAG);
    }
    if (options & OPT_PREFIX) {
        if (negative) {
            *(p++) = '-';
            saved_char = '-';
        }
        if (base == 2) {
            *(p++) = '0';
            *(p++) = 'b';
        }
        else if (base == 8) {
            *(p++) = '0';
            *(p++) = 'o';
        }
        else if (base == 16) {
            *(p++) = '0';
            *(p++) = 'x';
        }
        else if (base == -16) {
            *(p++) = '0';
            *(p++) = 'X';
        }
    }
    if (saved_char) {
        saved_char = *(--p);
        assert(saved_char);
    }

    zz_err ret = zz_get_str(&u->z, base, p, &len);

    if (saved_char) {
        *p = saved_char;
    }
    if (ret) {
        /* LCOV_EXCL_START */
        free(buf);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    p += len;
    if (options & OPT_TAG) {
        *(p++) = ')';
    }
    *(p++) = '\0';

    PyObject *res = PyUnicode_FromString(buf);

    free(buf);
    return res;
}

static MPZ_Object *
MPZ_from_str(PyObject *obj, int base)
{
    Py_ssize_t len;
    const char *str = PyUnicode_AsUTF8AndSize(obj, &len);

    if (!str) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    if (base < 0) {
        goto bad_base;
    }

    MPZ_Object *res = MPZ_new();

    if (!res) {
        return (MPZ_Object *)PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    while (len && isspace(*str)) {
        str++;
        len--;
    }

    bool cast_negative = (str[0] == '-');

    str += cast_negative;
    len -= cast_negative;
    if (len && str[0] == '+') {
        str++;
        len--;
    }
    if (str[0] == '0' && len >= 2) {
        if (base == 0) {
            if (tolower(str[1]) == 'b') {
                base = 2;
            }
            else if (tolower(str[1]) == 'o') {
                base = 8;
            }
            else if (tolower(str[1]) == 'x') {
                base = 16;
            }
            else {
                goto err;
            }
        }
        if ((tolower(str[1]) == 'b' && base == 2)
            || (tolower(str[1]) == 'o' && base == 8)
            || (tolower(str[1]) == 'x' && base == 16))
        {
            str += 2;
            len -= 2;
            if (len && str[0] == '_') {
                str++;
                len--;
            }
        }
        else {
            goto skip_negation;
        }
    }
    else {
skip_negation:
        str -= cast_negative;
        len += cast_negative;
        cast_negative = false;
    }
    if (base == 0) {
        base = 10;
    }

    const char *end = str + len - 1;

    while (len > 0 && isspace(*end)) {
        end--;
        len--;
    }

    zz_err ret = zz_set_str(str, (size_t)len, base, &res->z);

    if (ret == ZZ_MEM) {
        /* LCOV_EXCL_START */
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    else if (ret == ZZ_VAL) {
        Py_DECREF(res);
        if (2 <= base && base <= 36) {
err:
            PyErr_Format(PyExc_ValueError,
                         "invalid literal for mpz() with base %d: %.200R",
                         base, obj);
        }
        else {
bad_base:
            PyErr_SetString(PyExc_ValueError,
                            "mpz base must be >= 2 and <= 36, or 0");
        }
        return NULL;
    }
    if (cast_negative) {
        (void)zz_neg(&res->z, &res->z);
    }
    return res;
}

static MPZ_Object *
MPZ_from_int(PyObject *obj)
{
#if !defined(PYPY_VERSION) && !defined(GRAALVM_PYTHON) \
    && !defined(Py_LIMITED_API)
    PyLongExport long_export = {0, 0, 0, 0, 0};
    const zz_layout *int_layout = (zz_layout *)PyLong_GetNativeLayout();
    MPZ_Object *res = NULL;

    if (PyLong_Export(obj, &long_export) < 0) {
        return res; /* LCOV_EXCL_LINE */
    }
    if (long_export.digits) {
        res = MPZ_new();
        if (!res || zz_import((size_t)long_export.ndigits,
                              long_export.digits, *int_layout, &res->z))
        {
            return (MPZ_Object *)PyErr_NoMemory(); /* LCOV_EXCL_LINE */
        }
        if (long_export.negative) {
            (void)zz_neg(&res->z, &res->z);
        }
        PyLong_FreeExport(&long_export);
    }
    else {
        res = MPZ_new();
        if (res && zz_set(long_export.value, &res->z)) {
            PyErr_NoMemory(); /* LCOV_EXCL_LINE */
        }
    }
    return res;
#else
    int64_t value;

    if (!PyLong_AsInt64(obj, &value)) {
        MPZ_Object *res = MPZ_new();

        if (res && zz_set(value, &res->z)) {
            PyErr_NoMemory(); /* LCOV_EXCL_LINE */
        }
        return res;
    }
    PyErr_Clear();

    PyObject *str = PyNumber_ToBase(obj, 16);

    if (!str) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    MPZ_Object *res = MPZ_from_str(str, 16);

    Py_DECREF(str);
    return res;
#endif
}

static PyObject *
MPZ_to_int(MPZ_Object *u)
{
    int64_t value;

    if (zz_get(&u->z, &value) == ZZ_OK) {
        return PyLong_FromInt64(value);
    }

#if !defined(PYPY_VERSION) && !defined(GRAALVM_PYTHON) \
    && !defined(Py_LIMITED_API)
    const zz_layout *int_layout = (zz_layout *)PyLong_GetNativeLayout();
    size_t size = (zz_bitlen(&u->z) + int_layout->bits_per_digit
                   - 1)/int_layout->bits_per_digit;
    void *digits;
    PyLongWriter *writer = PyLongWriter_Create(zz_isneg(&u->z),
                                               (Py_ssize_t)size, &digits);

    if (!writer) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    (void)zz_export(&u->z, *int_layout, size, digits);
    return PyLongWriter_Finish(writer);
#else
    size_t len;

    (void)zz_sizeinbase(&u->z, 16, &len);
    len += zz_isneg(&u->z);

    char *buf = malloc(len + 1);

    if (zz_get_str(&u->z, 16, buf, &len)) {
        /* LCOV_EXCL_START */
        free(buf);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    buf[len] = '\0';

    PyObject *res = PyLong_FromString(buf, NULL, 16);

    free(buf);
    return res;
#endif
}

static void
revstr(unsigned char *s, Py_ssize_t l, Py_ssize_t r)
{
    while (l < r) {
        unsigned char tmp = s[l];

        s[l] = s[r];
        s[r] = tmp;
        l++;
        r--;
    }
}

static const zz_layout bytes_layout = {8, 1, 1, 0};

static zz_err
zz_get_bytes(const zz_t *u, size_t length, bool is_signed,
             unsigned char **buffer)
{
    zz_t tmp;
    bool is_negative = zz_isneg(u);

    if (zz_init(&tmp)) {
        return ZZ_MEM; /* LCOV_EXCL_LINE */
    }
    if (is_negative) {
        if (!is_signed) {
            return ZZ_BUF;
        }
        if (8*length/bits_per_digit + 1 < u->size) {
            zz_clear(&tmp);
            return ZZ_BUF;
        }
        if (zz_set(1, &tmp) || zz_mul_2exp(&tmp, 8*length, &tmp)
            || zz_add(&tmp, u, &tmp))
        {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            return ZZ_MEM;
            /* LCOV_EXCL_STOP */
        }
        u = &tmp;
    }

    size_t nbits = zz_bitlen(u);

    if (nbits > 8*length
        || (is_signed && ((!nbits && is_negative)
            || (nbits && (nbits == 8 * length ? !is_negative : is_negative)))))
    {
        zz_clear(&tmp);
        return ZZ_BUF;
    }

    size_t gap = length - (nbits + bits_per_digit/8 - 1)/(bits_per_digit/8);

    zz_export(u, bytes_layout, length - gap, *buffer + gap);
    memset(*buffer, is_negative ? 0xFF : 0, gap);
    zz_clear(&tmp);
    return ZZ_OK;
}

static PyObject *
MPZ_to_bytes(MPZ_Object *u, Py_ssize_t length, int is_little, int is_signed)
{
    PyObject *bytes = PyBytes_FromStringAndSize(NULL, length);

    if (!bytes) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    unsigned char *buffer = (unsigned char *)PyBytes_AsString(bytes);
    zz_err ret = zz_get_bytes(&u->z, (size_t)length, is_signed, &buffer);

    if (ret == ZZ_OK) {
        if (is_little && length) {
            revstr(buffer, 0, length - 1);
        }
        return bytes;
    }
    if (ret == ZZ_BUF) {
        if (zz_isneg(&u->z) && !is_signed) {
            PyErr_SetString(PyExc_OverflowError,
                            "can't convert negative mpz to unsigned");
        }
        else {
#if (PY_VERSION_HEX < 0x030D08F0 || (PY_VERSION_HEX >= 0x030E0000 \
                                     && PY_VERSION_HEX < 0x030E00C3))
            if (!length && zz_cmp(&u->z, -1) == ZZ_EQ) {
                return bytes;
            }
#endif
            PyErr_SetString(PyExc_OverflowError, "int too big to convert");
        }
        Py_DECREF(bytes);
        return NULL;
    }
    /* LCOV_EXCL_START */
    Py_DECREF(bytes);
    return PyErr_NoMemory();
    /* LCOV_EXCL_STOP */
}

static zz_err
zz_set_bytes(const unsigned char *buffer, size_t length, bool is_signed,
             zz_t *u)
{
    if (!length) {
        return zz_set(0, u);
    }
    if (zz_import(length, buffer, bytes_layout, u)) {
        return ZZ_MEM; /* LCOV_EXCL_LINE */
    }
    (void)zz_abs(u, u);
    if (is_signed && zz_bitlen(u) == 8*(size_t)length) {
        zz_t tmp;

        if (zz_init(&tmp) || zz_set(1, &tmp)
            || zz_mul_2exp(&tmp, 8*length, &tmp)
            || zz_sub(&tmp, u, u) || zz_neg(u, u))
        {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            return ZZ_MEM;
            /* LCOV_EXCL_STOP */
        }
        zz_clear(&tmp);
    }
    return ZZ_OK;
}

static MPZ_Object *
MPZ_from_bytes(PyObject *obj, int is_little, int is_signed)
{
    PyObject *bytes = PyObject_Bytes(obj);
    unsigned char *buffer;
    Py_ssize_t length;

    if (bytes == NULL) {
        return NULL;
    }
    if (PyBytes_AsStringAndSize(bytes, (char **)&buffer, &length) == -1) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    if (!length) {
        is_little = 0;
    }
    if (is_little) {
        unsigned char *tmp = malloc((size_t)length);

        if (!tmp) {
            /* LCOV_EXCL_START */
            Py_DECREF(bytes);
            return (MPZ_Object *)PyErr_NoMemory();
            /* LCOV_EXCL_STOP */
        }
        memcpy(tmp, buffer, (size_t)length);
        revstr(tmp, 0, length - 1);
        buffer = tmp;
    }

    MPZ_Object *res = MPZ_new();

    if (!res || zz_set_bytes(buffer, (size_t)length, is_signed, &res->z)) {
        /* LCOV_EXCL_START */
        Py_DECREF(bytes);
        if (is_little) {
            free(buffer);
        }
        Py_XDECREF((PyObject *)res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    if (is_little) {
        free(buffer);
    }
    return res;
}

typedef PyObject * (*Py_nb_int_func)(PyObject *);

static PyObject *
new_impl(PyTypeObject *Py_UNUSED(type), PyObject *arg, PyObject *base_arg)
{
    int base = 10;

    if (Py_IsNone(base_arg)) {
        if (PyLong_Check(arg)) {
            return (PyObject *)MPZ_from_int(arg);
        }
        if (MPZ_CheckExact(arg)) {
            return Py_NewRef(arg);
        }
        if (PyNumber_Check(arg)) {
            PyObject *integer = NULL;
#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif
            Py_nb_int_func nb_int = PyType_GetSlot(Py_TYPE(arg), Py_nb_int);
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

            if (nb_int) {
                integer = nb_int(arg);
                if (!integer) {
                    return NULL;
                }
                if (!PyLong_Check(integer)) {
                    PyErr_Format(PyExc_TypeError,
                                 "__int__ returned non-int (type %U)",
                                 PyType_GetFullyQualifiedName(Py_TYPE(integer)));
                    Py_XDECREF(integer);
                    return NULL;
                }
                if (!PyLong_CheckExact(integer)
                    && PyErr_WarnFormat(PyExc_DeprecationWarning, 1,
                                        "__int__ returned non-int (type %U).  "
                                        "The ability to return an instance of a "
                                        "strict subclass of int "
                                        "is deprecated, and may be removed "
                                        "in a future version of Python.",
                                        PyType_GetFullyQualifiedName(Py_TYPE(integer))))
                {
                    Py_XDECREF(integer);
                    return NULL;
                }
            }
            else {
                integer = PyNumber_Index(arg);
                if (!integer) {
                    return NULL;
                }
            }
            if (integer) {
                PyObject *mpz = (PyObject *)MPZ_from_int(integer);

                Py_DECREF(integer);
                return (PyObject *)mpz;
            }
        }
        goto str;
    }
    else {
        base = PyLong_AsInt(base_arg);
        if (base == -1 && PyErr_Occurred()) {
            return NULL;
        }
    }
str:
    if (PyUnicode_Check(arg)) {
        PyObject *asciistr = gmp_PyUnicode_TransformDecimalAndSpaceToASCII(arg);

        if (!asciistr) {
            return NULL; /* LCOV_EXCL_LINE */
        }

        PyObject *res = (PyObject *)MPZ_from_str(asciistr, base);

        Py_DECREF(asciistr);
        return res;
    }
    else if (PyByteArray_Check(arg) || PyBytes_Check(arg)) {
        const char *string;

        if (PyByteArray_Check(arg)) {
            string = PyByteArray_AsString(arg);
        }
        else {
            string = PyBytes_AsString(arg);
        }

        PyObject *str = PyUnicode_FromString(string);

        if (!str) {
            return NULL; /* LCOV_EXCL_LINE */
        }

        PyObject *res = (PyObject *)MPZ_from_str(str, base);

        Py_DECREF(str);
        return res;
    }
    if (Py_IsNone(base_arg)) {
        PyErr_SetString(PyExc_TypeError,
                        "argument must be a number or a string");
    }
    else {
        PyErr_SetString(PyExc_TypeError,
                        "can't convert non-string with explicit base");
    }
    return NULL;
}

static PyObject *
new(PyTypeObject *type, PyObject *args, PyObject *keywds)
{
    static char *kwlist[] = {"", "base", NULL};
    Py_ssize_t argc = PyTuple_Size(args);
    PyObject *arg, *base = Py_None;

    if (type != &MPZ_Type) {
        MPZ_Object *tmp = (MPZ_Object *)new(&MPZ_Type, args, keywds);

        if (!tmp) {
            return NULL; /* LCOV_EXCL_LINE */
        }

        MPZ_Object *newobj = (MPZ_Object *)PyType_GenericNew(type, NULL, NULL);

        if (!newobj) {
            /* LCOV_EXCL_START */
            Py_DECREF(tmp);
            return NULL;
            /* LCOV_EXCL_STOP */
        }
        if (zz_init(&newobj->z) || zz_pos(&tmp->z, &newobj->z)) {
            /* LCOV_EXCL_START */
            Py_DECREF(tmp);
            return PyErr_NoMemory();
            /* LCOV_EXCL_STOP */
        }
        Py_DECREF(tmp);
        return (PyObject *)newobj;
    }
    if (argc == 0) {
        return (PyObject *)MPZ_new();
    }
    if (argc == 1 && !keywds) {
        arg = PyTuple_GetItem(args, 0);
        return new_impl(type, arg, Py_None);
    }
    if (!PyArg_ParseTupleAndKeywords(args, keywds, "O|O",
                                     kwlist, &arg, &base))
    {
        return NULL;
    }
    return new_impl(type, arg, base);
}

typedef void (*Py_tp_free_func)(void *);

static void
dealloc(PyObject *self)
{
    MPZ_Object *u = (MPZ_Object *)self;

    if (global.gmp_cache_size < CACHE_SIZE
        && (u->z).alloc <= MAX_CACHE_MPZ_DIGITS
        && MPZ_CheckExact(self))
    {
        global.gmp_cache[(global.gmp_cache_size)++] = u;
    }
    else {
        zz_clear(&u->z);
        if (MPZ_CheckExact(self)) {
            PyObject_Free(self);
        }
        else {
#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif
            Py_tp_free_func tp_free = PyType_GetSlot(Py_TYPE(self), Py_tp_free);
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

            tp_free(self);
        }
    }
}

static PyObject *
vectorcall(PyObject *type, PyObject *const *args, size_t nargsf,
           PyObject *kwnames)
{
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    static const char *const keywords[] = {"", "base"};
    const static gmp_pyargs fnargs = {
        .keywords = keywords,
        .maxpos = 2,
        .minargs = 0,
        .maxargs = 2,
        .fname = "mpz",
    };
    Py_ssize_t argidx[2] = {-1, -1};

    if (gmp_parse_pyargs(&fnargs, argidx, args, nargs, kwnames) == -1) {
        return NULL;
    }
    if (argidx[1] >= 0) {
        return new_impl((PyTypeObject *)type, args[argidx[0]],
                        args[argidx[1]]);
    }
    else if (argidx[0] >= 0) {
        return new_impl((PyTypeObject *)type, args[argidx[0]], Py_None);
    }
    else {
        return (PyObject *)MPZ_new();
    }
}

static PyObject *
repr(PyObject *self)
{
    return MPZ_to_str((MPZ_Object *)self, 10, OPT_TAG);
}

static PyObject *
str(PyObject *self)
{
    return MPZ_to_str((MPZ_Object *)self, 10, 0);
}

#define Number_Check(op) (PyFloat_Check((op)) || PyComplex_Check((op)))

#define CHECK_OP(u, a)          \
    if (MPZ_Check(a)) {         \
        u = (MPZ_Object *)a;    \
        Py_INCREF(a);           \
    }                           \
    else if (PyLong_Check(a)) { \
        u = MPZ_from_int(a);    \
        if (!u) {               \
            goto end;           \
        }                       \
    }                           \
    else if (Number_Check(a)) { \
        goto numbers;           \
    }                           \
    else {                      \
        goto fallback;          \
    }

PyObject *
to_float(PyObject *self)
{
    double d;
    MPZ_Object *u = (MPZ_Object *)self;
    zz_err ret = zz_get(&u->z, &d);

    if (ret == ZZ_BUF) {
        PyErr_SetString(PyExc_OverflowError,
                        "integer too large to convert to float");
        return NULL;
    }
    return PyFloat_FromDouble(d);
}

static inline int64_t
PyLong_AsSdigit_t(PyObject *obj, int *error)
{
#if SIZEOF_LONG == 8
    long value = PyLong_AsLongAndOverflow(obj, error);
#else
    long long value = PyLong_AsLongLongAndOverflow(obj, error);
#endif
    Py_BUILD_ASSERT(sizeof(value) == sizeof(int64_t));
    if (!error && (INT64_MIN > value || value > INT64_MAX)) {
        *error = 1;
    }
    return (int64_t)value;
}

static PyObject *
richcompare(PyObject *self, PyObject *other, int op)
{
    MPZ_Object *u = (MPZ_Object *)self;
    zz_ord r;

    if (MPZ_Check(other)) {
        r = zz_cmp(&u->z, &((MPZ_Object *)other)->z);
    }
    else if (PyLong_Check(other)) {
        int error;
        int64_t temp = PyLong_AsSdigit_t(other, &error);

        if (!error) {
            r = zz_cmp(&u->z, temp);
        }
        else {
            MPZ_Object *v = MPZ_from_int(other);

            if (!v) {
                goto end; /* LCOV_EXCL_LINE */
            }
            r = zz_cmp(&u->z, &v->z);
            Py_DECREF(v);
        }
    }
    else if (Number_Check(other)) {
        goto numbers;
    }
    else {
        goto fallback;
    }
    switch (op) {
        case Py_LT:
            return PyBool_FromLong(r == ZZ_LT);
        case Py_LE:
            return PyBool_FromLong(r != ZZ_GT);
        case Py_GT:
            return PyBool_FromLong(r == ZZ_GT);
        case Py_GE:
            return PyBool_FromLong(r != ZZ_LT);
        case Py_EQ:
            return PyBool_FromLong(r == ZZ_EQ);
        case Py_NE:
            return PyBool_FromLong(r != ZZ_EQ);
    }
    /* LCOV_EXCL_START */
end:
    return NULL;
    /* LCOV_EXCL_STOP */
fallback:
    Py_RETURN_NOTIMPLEMENTED;
numbers:
    self = to_float(self);
    if (!self) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    PyObject *res = PyObject_RichCompare(self, other, op);

    Py_DECREF(self);
    return res;
}

static Py_hash_t
hash(PyObject *self)
{
    MPZ_Object *u = (MPZ_Object *)self;

    if (u->hash_cache != -1) {
        return u->hash_cache;
    }

    zz_digit_t digits[1];
    zz_t w = {false, 1, 1, digits};

    assert((int64_t)INT64_MAX > pyhash_modulus);
    (void)zz_div(&u->z, (int64_t)pyhash_modulus, NULL, &w);

    Py_hash_t r = w.size ? (Py_hash_t)w.digits[0] : 0;

    if (zz_isneg(&u->z) && r) {
        r = -(pyhash_modulus - r);
    }
    if (r == -1) {
        r = -2;
    }
    return u->hash_cache = r;
}

#define UNOP(suff, func)                           \
    static PyObject *                              \
    func(PyObject *self)                           \
    {                                              \
        MPZ_Object *u = (MPZ_Object *)self;        \
        MPZ_Object *res = MPZ_new();               \
                                                   \
        if (res && zz_##suff(&u->z, &res->z)) {    \
            PyErr_NoMemory(); /* LCOV_EXCL_LINE */ \
        }                                          \
        return (PyObject *)res;                    \
    }

UNOP(pos, plus)
UNOP(neg, nb_negative)
UNOP(abs, nb_absolute)
UNOP(invert, nb_invert)

PyObject *
to_int(PyObject *self)
{
    return MPZ_to_int((MPZ_Object *)self);
}

static int
to_bool(PyObject *self)
{
    return !zz_iszero(&((MPZ_Object *)self)->z);
}

#define CHECK_OPv2(u, a)        \
    if (MPZ_Check(a)) {         \
        u = (MPZ_Object *)a;    \
        Py_INCREF(a);           \
    }                           \
    else if (PyLong_Check(a)) { \
        ;                       \
    }                           \
    else if (Number_Check(a)) { \
        goto numbers;           \
    }                           \
    else {                      \
        goto fallback;          \
    }

#define BINOP(suff, slot)                                       \
    static PyObject *                                           \
    nb_##suff(PyObject *self, PyObject *other)                  \
    {                                                           \
        MPZ_Object *u = NULL, *v = NULL, *res = NULL;           \
                                                                \
        CHECK_OPv2(u, self);                                    \
        CHECK_OPv2(v, other);                                   \
                                                                \
        res = MPZ_new();                                        \
        if (!res) {                                             \
            goto end;                                           \
        }                                                       \
                                                                \
        zz_err ret = ZZ_OK;                                     \
                                                                \
        if (!u) {                                               \
            int error;                                          \
            int64_t temp = PyLong_AsSdigit_t(self, &error);     \
                                                                \
            if (!error) {                                       \
                ret = zz_##suff(temp, &v->z, &res->z);          \
                goto done;                                      \
            }                                                   \
            u = MPZ_from_int(self);                             \
            if (!u) {                                           \
                goto end;                                       \
            }                                                   \
        }                                                       \
        if (!v) {                                               \
            int error;                                          \
            int64_t temp = PyLong_AsSdigit_t(other, &error);    \
                                                                \
            if (!error) {                                       \
                ret = zz_##suff(&u->z, temp, &res->z);          \
                goto done;                                      \
            }                                                   \
            v = MPZ_from_int(other);                            \
            if (!v) {                                           \
                goto end;                                       \
            }                                                   \
        }                                                       \
        ret = zz_##suff(&u->z, &v->z, &res->z);                 \
done:                                                           \
        if (ret == ZZ_OK) {                                     \
            goto end;                                           \
        }                                                       \
        if (ret == ZZ_VAL) {                                    \
            Py_CLEAR(res);                                      \
            PyErr_SetString(PyExc_ZeroDivisionError,            \
                            "division by zero");                \
        }                                                       \
        else {                                                  \
            Py_CLEAR(res);                                      \
            PyErr_NoMemory();                                   \
        }                                                       \
    end:                                                        \
        Py_XDECREF((PyObject *)u);                              \
        Py_XDECREF((PyObject *)v);                              \
        return (PyObject *)res;                                 \
    fallback:                                                   \
        Py_XDECREF((PyObject *)u);                              \
        Py_XDECREF((PyObject *)v);                              \
        Py_RETURN_NOTIMPLEMENTED;                               \
    numbers:                                                    \
        Py_XDECREF((PyObject *)u);                              \
        Py_XDECREF((PyObject *)v);                              \
                                                                \
        PyObject *uf, *vf, *rf;                                 \
                                                                \
        if (Number_Check(self)) {                               \
            uf = self;                                          \
            Py_INCREF(uf);                                      \
        }                                                       \
        else {                                                  \
            uf = to_float(self);                                \
            if (!uf) {                                          \
                return NULL;                                    \
            }                                                   \
        }                                                       \
        if (Number_Check(other)) {                              \
            vf = other;                                         \
            Py_INCREF(vf);                                      \
        }                                                       \
        else {                                                  \
            vf = to_float(other);                               \
            if (!vf) {                                          \
                Py_DECREF(uf);                                  \
                return NULL;                                    \
            }                                                   \
        }                                                       \
        rf = slot(uf, vf);                                      \
        Py_DECREF(uf);                                          \
        Py_DECREF(vf);                                          \
        return rf;                                              \
    }

BINOP(add, PyNumber_Add)
BINOP(sub, PyNumber_Subtract)
BINOP(mul, PyNumber_Multiply)

#define zz_quo_(u, v, w) zz_div((u), (v), (w), NULL)
#define zz_rem_(u, v, w) zz_div((u), (v), NULL, (w))

BINOP(quo_, PyNumber_FloorDivide)
BINOP(rem_, PyNumber_Remainder)

static PyObject *
nb_divmod(PyObject *self, PyObject *other)
{
    PyObject *res = PyTuple_New(2);
    MPZ_Object *u = NULL, *v = NULL;

    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    CHECK_OP(u, self);
    CHECK_OP(v, other);

    MPZ_Object *q = MPZ_new();
    MPZ_Object *r = MPZ_new();

    if (!q || !r) {
        /* LCOV_EXCL_START */
        Py_XDECREF((PyObject *)q);
        Py_XDECREF((PyObject *)r);
        return NULL;
        /* LCOV_EXCL_STOP */
    }

    zz_err ret = zz_div(&u->z, &v->z, &q->z, &r->z);

    if (ret) {
        Py_DECREF(q);
        Py_DECREF(r);
        if (ret == ZZ_VAL) {
            PyErr_SetString(PyExc_ZeroDivisionError, "division by zero");
        }
        else {
            PyErr_NoMemory(); /* LCOV_EXCL_LINE */
        }
        goto end;
    }
    Py_DECREF(u);
    Py_DECREF(v);
    (void)PyTuple_SetItem(res, 0, (PyObject *)q);
    (void)PyTuple_SetItem(res, 1, (PyObject *)r);
    return res;
    /* LCOV_EXCL_START */
end:
    Py_DECREF(res);
    Py_XDECREF((PyObject *)u);
    Py_XDECREF((PyObject *)v);
    return NULL;
    /* LCOV_EXCL_STOP */
fallback:
numbers:
    Py_DECREF(res);
    Py_XDECREF((PyObject *)u);
    Py_XDECREF((PyObject *)v);
    Py_RETURN_NOTIMPLEMENTED;
}

static zz_err
zz_divnear(const zz_t *u, const zz_t *v, zz_t *q, zz_t *r)
{
    if (!q || !r) {
        assert(q != NULL || r != NULL);
        if (!q) {
            zz_t tmp;

            if (zz_init(&tmp)) {
                return ZZ_MEM; /* LCOV_EXCL_LINE */
            }

            zz_err ret = zz_divnear(u, v, &tmp, r);

            zz_clear(&tmp);
            return ret;
        }
        else {
            zz_t tmp;

            if (zz_init(&tmp)) {
                return ZZ_MEM; /* LCOV_EXCL_LINE */
            }

            zz_err ret = zz_divnear(u, v, q, &tmp);

            zz_clear(&tmp);
            return ret;
        }
    }

    zz_err ret = zz_div(u, v, q, r);

    if (ret) {
        /* LCOV_EXCL_START */
err:
        zz_clear(q);
        zz_clear(r);
        return ret;
        /* LCOV_EXCL_STOP */
    }

    zz_ord unexpect = zz_isneg(v) ? ZZ_LT : ZZ_GT;
    zz_t halfQ;

    if (zz_init(&halfQ) || zz_quo_2exp(v, 1, &halfQ)) {
        /* LCOV_EXCL_START */
        zz_clear(&halfQ);
        goto err;
        /* LCOV_EXCL_STOP */
    }

    zz_ord cmp = zz_cmp(r, &halfQ);

    zz_clear(&halfQ);
    if (cmp == ZZ_EQ && !zz_isodd(v) && !zz_iszero(q) && zz_isodd(q)) {
        cmp = unexpect;
    }
    if (cmp == unexpect && (zz_add(q, 1, q) || zz_sub(r, v, r))) {
        goto err; /* LCOV_EXCL_LINE */
    }
    return ZZ_OK;
}

static zz_err
zz_truediv(const zz_t *u, const zz_t *v, double *res)
{
    if (zz_iszero(v)) {
        return ZZ_VAL;
    }
    if (zz_iszero(u)) {
        *res = zz_isneg(v) ? -0.0 : 0.0;
        return ZZ_OK;
    }

    zz_bitcnt_t ubits = zz_bitlen(u);
    zz_bitcnt_t vbits = zz_bitlen(v);

    if (ubits > vbits && ubits - vbits > DBL_MAX_EXP) {
        return ZZ_BUF;
    }
    if (ubits < vbits && vbits - ubits > -DBL_MIN_EXP + DBL_MANT_DIG + 1) {
        *res = zz_isneg(u) != zz_isneg(v) ? -0.0 : 0.0;
        return ZZ_OK;
    }

    int shift = (int)(vbits - ubits);
    int n = shift, whole = n / bits_per_digit;
    zz_t a, b;

    if (zz_init(&a) || zz_init(&b) || zz_abs(u, &a) || zz_abs(v, &b)) {
        /* LCOV_EXCL_START */
tmp_clear:
        zz_clear(&a);
        zz_clear(&b);
        return ZZ_MEM;
        /* LCOV_EXCL_STOP */
    }
    if (shift < 0) {
        const zz_t *t = u;

        u = v;
        v = t;
        n = -n;
        whole = -whole;
    }
    /*                       -shift - 1             -shift
      find shift satisfying 2           <= |a/b| < 2       */
    n %= bits_per_digit;
    for (zz_size_t i = v->size; i--;) {
        zz_digit_t du, dv = v->digits[i];

        if (i >= whole) {
            if (i - whole < u->size) {
                du = u->digits[i - whole] << n;
            }
            else {
                du = 0;
            }
            if (n && i > whole) {
                du |= u->digits[i - whole - 1] >> (bits_per_digit - n);
            }
        }
        else {
            du = 0;
        }
        if (du < dv) {
            if (shift < 0) {
                shift--;
            }
            break;
        }
        if (du > dv) {
            if (shift >= 0) {
                shift--;
            }
            break;
        }
    }
    shift += DBL_MANT_DIG;
    if (shift > 0 && zz_mul_2exp(&a, (uint64_t)shift, &a)) {
        goto tmp_clear; /* LCOV_EXCL_LINE */
    }
    if (shift < 0 && zz_mul_2exp(&b, (uint64_t)-shift, &b)) {
        goto tmp_clear; /* LCOV_EXCL_LINE */
    }
    if (zz_divnear(&a, &b, &a, NULL)) {
        /* LCOV_EXCL_START */
        zz_clear(&a);
        zz_clear(&b);
        return ZZ_MEM;
        /* LCOV_EXCL_STOP */
    }
    zz_clear(&b);
    (void)zz_get(&a, res);
    zz_clear(&a);
    *res = ldexp(*res, -shift);
    if (zz_isneg(u) != zz_isneg(v)) {
        *res = -*res;
    }
    if (isinf(*res)) {
        return ZZ_BUF;
    }
    return ZZ_OK;
}

static PyObject *
nb_truediv(PyObject *self, PyObject *other)
{
    PyObject *res = NULL;
    MPZ_Object *u = NULL, *v = NULL;

    CHECK_OP(u, self);
    CHECK_OP(v, other);

    double d;

    zz_err ret = zz_truediv(&u->z, &v->z, &d);

    if (ret == ZZ_OK) {
        res = PyFloat_FromDouble(d);
        goto end;
    }
    if (ret == ZZ_VAL) {
        PyErr_SetString(PyExc_ZeroDivisionError, "division by zero");
    }
    else if (ret == ZZ_BUF) {
        PyErr_SetString(PyExc_OverflowError,
                        "integer too large to convert to float");
    }
    else {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
end:
    Py_XDECREF((PyObject *)u);
    Py_XDECREF((PyObject *)v);
    return res;
fallback:
    Py_XDECREF((PyObject *)u);
    Py_XDECREF((PyObject *)v);
    Py_RETURN_NOTIMPLEMENTED;
numbers:
    Py_XDECREF((PyObject *)u);
    Py_XDECREF((PyObject *)v);

    PyObject *uf, *vf;

    if (Number_Check(self)) {
        uf = self;
        Py_INCREF(uf);
    }
    else {
        uf = to_float(self);
        if (!uf) {
            return NULL; /* LCOV_EXCL_LINE */
        }
    }
    if (Number_Check(other)) {
        vf = other;
        Py_INCREF(vf);
    }
    else {
        vf = to_float(other);
        if (!vf) {
            /* LCOV_EXCL_START */
            Py_DECREF(uf);
            return NULL;
            /* LCOV_EXCL_STOP */
        }
    }
    res = PyNumber_TrueDivide(uf, vf);
    Py_DECREF(uf);
    Py_DECREF(vf);
    return res;
}

#define CHECK_OP_INT(u, a)      \
    if (MPZ_Check(a)) {         \
        u = (MPZ_Object *)a;    \
        Py_INCREF(a);           \
    }                           \
    else {                      \
        u = MPZ_from_int(a);    \
        if (!u) {               \
            goto end;           \
        }                       \
    }                           \

#define CHECK_OP_INTv2(u, a)    \
    if (MPZ_Check(a)) {         \
        u = (MPZ_Object *)a;    \
        Py_INCREF(u);           \
    }                           \
    else if (PyLong_Check(a)) { \
        ;                       \
    }                           \
    else {                      \
        goto end;               \
    }                           \

#define BINOP_INT(suff)                                         \
    static PyObject *                                           \
    nb_##suff(PyObject *self, PyObject *other)                  \
    {                                                           \
        MPZ_Object *u = NULL, *v = NULL, *res = NULL;           \
                                                                \
        CHECK_OP_INT(u, self);                                  \
        CHECK_OP_INT(v, other);                                 \
                                                                \
        res = MPZ_new();                                        \
        zz_err ret = ZZ_OK;                                     \
                                                                \
        if (!res || (ret = zz_##suff(&u->z, &v->z, &res->z))) { \
            /* LCOV_EXCL_START */                               \
            Py_CLEAR(res);                                      \
            if (ret == ZZ_VAL) {                                \
                PyErr_SetString(PyExc_ValueError,               \
                                "negative shift count");        \
            }                                                   \
            else if (ret == ZZ_BUF) {                           \
                PyErr_SetString(PyExc_OverflowError,            \
                                "too many digits in integer");  \
            }                                                   \
            else {                                              \
                PyErr_NoMemory();                               \
            }                                                   \
            /* LCOV_EXCL_STOP */                                \
        }                                                       \
    end:                                                        \
        Py_XDECREF((PyObject *)u);                              \
        Py_XDECREF((PyObject *)v);                              \
        return (PyObject *)res;                                 \
    }

#define BINOP_INTv2(suff)                                            \
    static PyObject *                                                \
    nb_##suff(PyObject *self, PyObject *other)                       \
    {                                                                \
        MPZ_Object *u = NULL, *v = NULL, *res = NULL;                \
                                                                     \
        CHECK_OP_INTv2(u, self);                                     \
        CHECK_OP_INTv2(v, other);                                    \
                                                                     \
        res = MPZ_new();                                             \
        if (!res) {                                                  \
            goto end;                                                \
        }                                                            \
                                                                     \
        zz_err ret = ZZ_OK;                                          \
                                                                     \
        if (!u) {                                                    \
            int error = PyLong_IsNegative(self) || zz_isneg(&v->z);  \
                                                                     \
            if (!error) {                                            \
                int64_t temp = PyLong_AsSdigit_t(self, &error);      \
                                                                     \
                if (!error) {                                        \
                    ret = zz_i64_##suff(temp, &v->z, &res->z);       \
                    goto done;                                       \
                }                                                    \
            }                                                        \
            u = MPZ_from_int(self);                                  \
            if (!u) {                                                \
                goto end;                                            \
            }                                                        \
        }                                                            \
        if (!v) {                                                    \
            int error = zz_isneg(&u->z) || PyLong_IsNegative(other); \
                                                                     \
            if (!error) {                                            \
                int64_t temp = PyLong_AsSdigit_t(other, &error);     \
                                                                     \
                if (!error) {                                        \
                    ret = zz_##suff##_i64(&u->z, temp, &res->z);     \
                    goto done;                                       \
                }                                                    \
            }                                                        \
            v = MPZ_from_int(other);                                 \
            if (!v) {                                                \
                goto end;                                            \
            }                                                        \
        }                                                            \
        ret = zz_##suff(&u->z, &v->z, &res->z);                      \
done:                                                                \
        if (ret) {                                                   \
            /* LCOV_EXCL_START */                                    \
            Py_CLEAR(res);                                           \
            if (ret == ZZ_VAL) {                                     \
                PyErr_SetString(PyExc_ValueError,                    \
                                "negative shift count");             \
            }                                                        \
            else if (ret == ZZ_BUF) {                                \
                PyErr_SetString(PyExc_OverflowError,                 \
                                "too many digits in integer");       \
            }                                                        \
            else {                                                   \
                PyErr_NoMemory();                                    \
            }                                                        \
            /* LCOV_EXCL_STOP */                                     \
        }                                                            \
    end:                                                             \
        Py_XDECREF((PyObject *)u);                                   \
        Py_XDECREF((PyObject *)v);                                   \
        return (PyObject *)res;                                      \
    }

static inline zz_err
zz_and_i64(const zz_t *u, int64_t v, zz_t *w)
{
    if (zz_iszero(u) || !v) {
        return zz_set(0, w);
    }
    assert(!zz_isneg(u) && v > 0);
    return zz_set((int64_t)(u->digits[0] & (zz_digit_t)v), w);
}
#define zz_i64_and(x, y, r) zz_and_i64((y), (x), (r))

BINOP_INTv2(and)
BINOP_INT(or)
BINOP_INT(xor)

static inline zz_err
zz_lshift(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (zz_isneg(v)) {
        return ZZ_VAL;
    }

    int64_t shift;

    if (zz_get(v, &shift)) {
        return ZZ_BUF;
    }
    return zz_mul_2exp(u, (zz_bitcnt_t)shift, w);
}

static inline zz_err
zz_rshift(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (zz_isneg(v)) {
        return ZZ_VAL;
    }

    int64_t shift;

    if (zz_get(v, &shift)) {
        return zz_set(zz_isneg(u) ? -1 : 0, w);
    }
    return zz_quo_2exp(u, (zz_bitcnt_t)shift, w);
}

BINOP_INT(lshift)
BINOP_INT(rshift)

typedef PyObject * (*Py_nb_power_func)(PyObject *, PyObject *, PyObject *);

static PyObject *
power(PyObject *self, PyObject *other, PyObject *module)
{
    MPZ_Object *res = NULL;
    MPZ_Object *u = NULL, *v = NULL;

    CHECK_OP(u, self);
    CHECK_OP(v, other);
    if (Py_IsNone(module)) {
        if (zz_isneg(&v->z)) {
            PyObject *uf, *vf, *resf;

            uf = to_float((PyObject *)u);
            Py_DECREF(u);
            if (!uf) {
                Py_DECREF(v);
                return NULL;
            }
            vf = to_float((PyObject *)v);
            Py_DECREF(v);
            if (!vf) {
                Py_DECREF(uf);
                return NULL;
            }

#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif
            Py_nb_power_func nb_power = PyType_GetSlot(&PyFloat_Type,
                                                       Py_nb_power);
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

            resf = nb_power(uf, vf, Py_None);
            Py_DECREF(uf);
            Py_DECREF(vf);
            return resf;
        }
        res = MPZ_new();

        int64_t exp;

        if (!res || zz_get(&v->z, &exp)
            || zz_pow(&u->z, (zz_digit_t)exp, &res->z))
        {
            /* LCOV_EXCL_START */
            Py_CLEAR(res);
            PyErr_SetNone(PyExc_MemoryError);
            /* LCOV_EXCL_STOP */
        }
        Py_DECREF(u);
        Py_DECREF(v);
        return (PyObject *)res;
    }
    else {
        MPZ_Object *w = NULL;

        CHECK_OP_INT(w, module);

        zz_err ret = ZZ_OK;

        res = MPZ_new();
        if (!res || (ret = zz_powm(&u->z, &v->z, &w->z, &res->z))) {
            /* LCOV_EXCL_START */
            if (ret == ZZ_VAL) {
                PyErr_SetString(PyExc_ValueError,
                                "base is not invertible for the given modulus");
            }
            else {
                PyErr_NoMemory();
            }
            Py_CLEAR(res);
            /* LCOV_EXCL_STOP */
        }
        Py_DECREF(w);
    }
end:
    Py_XDECREF((PyObject *)u);
    Py_XDECREF((PyObject *)v);
    return (PyObject *)res;
fallback:
    Py_XDECREF((PyObject *)u);
    Py_XDECREF((PyObject *)v);
    Py_RETURN_NOTIMPLEMENTED;
numbers:
    Py_XDECREF((PyObject *)u);
    Py_XDECREF((PyObject *)v);

    PyObject *uf, *vf;

    if (Number_Check(self)) {
        uf = self;
        Py_INCREF(uf);
    }
    else {
        uf = to_float(self);
        if (!uf) {
            return NULL;
        }
    }
    if (Number_Check(other)) {
        vf = other;
        Py_INCREF(vf);
    }
    else {
        vf = to_float(other);
        if (!vf) {
            Py_DECREF(uf);
            return NULL;
        }
    }

    PyObject *res2 = PyNumber_Power(uf, vf, Py_None);

    Py_DECREF(uf);
    Py_DECREF(vf);
    return res2;
}

static PyNumberMethods as_number = {
    .nb_add = nb_add,
    .nb_subtract = nb_sub,
    .nb_multiply = nb_mul,
    .nb_divmod = nb_divmod,
    .nb_floor_divide = nb_quo_,
    .nb_true_divide = nb_truediv,
    .nb_remainder = nb_rem_,
    .nb_power = power,
    .nb_positive = plus,
    .nb_negative = nb_negative,
    .nb_absolute = nb_absolute,
    .nb_invert = nb_invert,
    .nb_lshift = nb_lshift,
    .nb_rshift = nb_rshift,
    .nb_and = nb_and,
    .nb_or = nb_or,
    .nb_xor = nb_xor,
    .nb_int = to_int,
    .nb_float = to_float,
    .nb_index = to_int,
    .nb_bool = to_bool,
};

static PyObject *
get_copy(PyObject *self, void *Py_UNUSED(closure))
{
    return Py_NewRef(self);
}

static PyObject *
get_one(PyObject *Py_UNUSED(self), void *Py_UNUSED(closure))
{
    MPZ_Object *res = MPZ_new();

    if (res && zz_set(1, &res->z)) {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    return (PyObject *)res;
}

static PyObject *
get_zero(PyObject *Py_UNUSED(self), void *Py_UNUSED(closure))
{
    return (PyObject *)MPZ_new();
}

static PyGetSetDef getsetters[] = {
    {"numerator", (getter)get_copy, NULL,
     "the numerator of self (the value itself)", NULL},
    {"denominator", (getter)get_one, NULL,
     "the denominator of self (mpz(1))", NULL},
    {"real", (getter)get_copy, NULL, "the real part of self (the value itself)",
     NULL},
    {"imag", (getter)get_zero, NULL, "the imaginary part of self (mpz(0))",
     NULL},
    {NULL} /* sentinel */
};

static PyObject *
bit_length(PyObject *self, PyObject *Py_UNUSED(args))
{
    MPZ_Object *u = (MPZ_Object *)self;
    zz_digit_t digit = zz_bitlen(&u->z);

    return PyLong_FromUnsignedLongLong(digit);
}

static PyObject *
bit_count(PyObject *self, PyObject *Py_UNUSED(args))
{
    MPZ_Object *u = (MPZ_Object *)self;
    zz_bitcnt_t count = zz_bitcnt(&u->z);

    return PyLong_FromUnsignedLongLong(count);
}

static PyObject *
to_bytes(PyObject *self, PyObject *const *args, Py_ssize_t nargs,
         PyObject *kwnames)
{
    static const char *const keywords[] = {"length", "byteorder", "signed"};
    const static gmp_pyargs fnargs = {
        .keywords = keywords,
        .maxpos = 2,
        .minargs = 0,
        .maxargs = 3,
        .fname = "to_bytes",
    };
    Py_ssize_t argidx[3] = {-1, -1, -1};

    if (gmp_parse_pyargs(&fnargs, argidx, args, nargs, kwnames) == -1) {
        return NULL;
    }

    Py_ssize_t length = 1;
    int is_little = 0, is_signed = 0;

    if (argidx[0] >= 0) {
        PyObject *arg = args[argidx[0]];

        if (PyLong_Check(arg)) {
            length = PyLong_AsSsize_t(arg);
            if (length < 0) {
                PyErr_SetString(PyExc_ValueError,
                                "length argument must be non-negative");
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_TypeError,
                            "to_bytes() takes an integer argument 'length'");
            return NULL;
        }
    }
    if (argidx[1] >= 0) {
        PyObject *arg = args[argidx[1]];

        if (PyUnicode_Check(arg)) {
            const char *byteorder = PyUnicode_AsUTF8AndSize(arg, NULL);

            if (!byteorder) {
                return NULL; /* LCOV_EXCL_LINE */
            }
            else if (strcmp(byteorder, "big") == 0) {
                is_little = 0;
            }
            else if (strcmp(byteorder, "little") == 0) {
                is_little = 1;
            }
            else {
                PyErr_SetString(PyExc_ValueError,
                                "byteorder must be either 'little' or 'big'");
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_TypeError,
                            "to_bytes() argument 'byteorder' must be str");
            return NULL;
        }
    }
    if (argidx[2] >= 0) {
        is_signed = PyObject_IsTrue(args[argidx[2]]);
    }
    return MPZ_to_bytes((MPZ_Object *)self, length, is_little, is_signed);
}

static PyObject *
_from_bytes(PyObject *Py_UNUSED(type), PyObject *arg)
{
    return (PyObject *)MPZ_from_bytes(arg, 0, 1);
}

static PyObject *
from_bytes(PyTypeObject *Py_UNUSED(type), PyObject *const *args,
           Py_ssize_t nargs, PyObject *kwnames)
{
    static const char *const keywords[] = {"bytes", "byteorder", "signed"};
    const static gmp_pyargs fnargs = {
        .keywords = keywords,
        .maxpos = 3,
        .minargs = 1,
        .maxargs = 3,
        .fname = "from_bytes",
    };
    Py_ssize_t argidx[3] = {-1, -1, -1};

    if (gmp_parse_pyargs(&fnargs, argidx, args, nargs, kwnames) == -1) {
        return NULL;
    }

    int is_little = 0, is_signed = 0;

    if (argidx[1] >= 0) {
        PyObject *arg = args[argidx[1]];

        if (PyUnicode_Check(arg)) {
            const char *byteorder = PyUnicode_AsUTF8AndSize(arg, NULL);

            if (!byteorder) {
                return NULL; /* LCOV_EXCL_LINE */
            }
            else if (strcmp(byteorder, "big") == 0) {
                is_little = 0;
            }
            else if (strcmp(byteorder, "little") == 0) {
                is_little = 1;
            }
            else {
                PyErr_SetString(PyExc_ValueError,
                                ("byteorder must be either 'little'"
                                 " or 'big'"));
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_TypeError,
                            ("from_bytes() argument 'byteorder'"
                             " must be str"));
            return NULL;
        }
    }
    if (argidx[2] >= 0) {
        is_signed = PyObject_IsTrue(args[argidx[2]]);
    }
    return (PyObject *)MPZ_from_bytes(args[argidx[0]], is_little, is_signed);
}

static PyObject *
as_integer_ratio(PyObject *self, PyObject *Py_UNUSED(args))
{
    PyObject *one = get_one(NULL, NULL);

    if (!one) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    PyObject *u = Py_NewRef(self);
    PyObject *ratio_tuple = PyTuple_Pack(2, u, one);

    Py_DECREF(u);
    Py_DECREF(one);
    return ratio_tuple;
}

static PyObject *
__round__(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (nargs > 1) {
        PyErr_Format(PyExc_TypeError,
                     "__round__ expected at most 1 argument, got %zu");
        return NULL;
    }

    MPZ_Object *u = (MPZ_Object *)self;

    if (!nargs) {
noop:
        return plus(self);
    }

    MPZ_Object *ndigits = NULL;
    zz_t exp, ten, p;

    CHECK_OP_INT(ndigits, args[0]);

    if (zz_isneg(&ndigits->z)) {
        if (zz_init(&exp) || zz_pos(&ndigits->z, &exp)) {
            /* LCOV_EXCL_START */
            zz_clear(&exp);
            Py_DECREF(ndigits);
            return PyErr_NoMemory();
            /* LCOV_EXCL_STOP */
        }
    }
    else {
        Py_DECREF(ndigits);
        goto noop;
    }
    Py_DECREF(ndigits);
    if (zz_init(&ten) || zz_set(10, &ten) || exp.size != 1) {
        /* LCOV_EXCL_START */
        zz_clear(&exp);
        zz_clear(&ten);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    if (zz_init(&p) || zz_pow(&ten, exp.digits[0], &p)) {
        /* LCOV_EXCL_START */
        zz_clear(&exp);
        zz_clear(&ten);
        zz_clear(&p);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    zz_clear(&exp);
    zz_clear(&ten);

    MPZ_Object *res = MPZ_new();

    if (!res) {
        /* LCOV_EXCL_START */
        zz_clear(&p);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    if (zz_divnear(&u->z, &p, NULL, &res->z)) {
        /* LCOV_EXCL_START */
        zz_clear(&p);
        Py_DECREF(res);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    zz_clear(&p);
    if (zz_sub(&u->z, &res->z, &res->z)) {
        /* LCOV_EXCL_START */
        Py_DECREF(res);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    return (PyObject *)res;
end:
    return NULL;
}

static PyObject *
__reduce_ex__(PyObject *self, PyObject *Py_UNUSED(args))
{
    MPZ_Object *u = (MPZ_Object *)self;
    zz_bitcnt_t len = zz_bitlen(&u->z);

    return Py_BuildValue("N(N)",
                         PyObject_GetAttrString(self, "_from_bytes"),
                         MPZ_to_bytes(u, (Py_ssize_t)(len + 7)/8 + 1, 0, 1));
}

static PyObject *
__sizeof__(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    MPZ_Object *u = (MPZ_Object *)self;

    return PyLong_FromSize_t(sizeof(MPZ_Object)
                             + (unsigned int)(u->z).alloc*sizeof(zz_digit_t));
}

static PyObject *
is_integer(PyObject *Py_UNUSED(self), PyObject *Py_UNUSED(args))
{
    Py_RETURN_TRUE;
}

static PyObject *
digits(PyObject *self, PyObject *const *args, Py_ssize_t nargs,
       PyObject *kwnames)
{
    static const char *const keywords[] = {"base"};
    const static gmp_pyargs fnargs = {
        .keywords = keywords,
        .maxpos = 1,
        .minargs = 0,
        .maxargs = 1,
        .fname = "digits",
    };
    Py_ssize_t argidx[1] = {-1};

    if (gmp_parse_pyargs(&fnargs, argidx, args, nargs, kwnames) == -1) {
        return NULL;
    }

    int base = 10;

    if (argidx[0] != -1) {
        PyObject *arg = args[argidx[0]];

        if (PyLong_Check(arg)) {
            base = PyLong_AsInt(args[argidx[0]]);
            if (base == -1 && PyErr_Occurred()) {
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_TypeError,
                            "digits() takes an integer argument 'base'");
            return NULL;
        }
    }
    return MPZ_to_str((MPZ_Object *)self, base, 0);
}

extern PyObject * __format__(PyObject *self, PyObject *format_spec);

/* Explicit signatures for METH_NOARGS methods are redundant
   since CPython 3.13. */

static PyMethodDef methods[] = {
    {"conjugate", (PyCFunction)plus, METH_NOARGS,
     "conjugate($self, /)\n--\n\nReturns self."},
    {"bit_length", bit_length, METH_NOARGS,
     ("bit_length($self, /)\n--\n\nNumber of bits necessary "
      "to represent self in binary.")},
    {"bit_count", bit_count, METH_NOARGS,
     ("bit_count($self, /)\n--\n\nNumber of ones in the binary "
      "representation of the absolute value of self.")},
    {"to_bytes", (PyCFunction)to_bytes, METH_FASTCALL | METH_KEYWORDS,
     "to_bytes($self, /, length=1, byteorder=\'big\', *, signed=False)\n--\n\n\
Return an array of bytes representing self.\n\n\
The integer is represented using length bytes.  An OverflowError is\n\
raised if self is not representable with the given number of bytes.\n\n\
The byteorder argument determines the byte order used to represent self.\n\
Accepted values are \'big\' and \'little\', when the most significant\n\
byte is at the beginning or at the end of the byte array, respectively.\n\n\
The signed argument determines whether two\'s complement is used to\n\
represent self.  If signed is False and a negative integer is given,\n\
an OverflowError is raised."},
    {"from_bytes", (PyCFunction)from_bytes,
     METH_FASTCALL | METH_KEYWORDS | METH_CLASS,
     "from_bytes($type, /, bytes, byteorder=\'big\', *, signed=False)\n--\n\n\
Return the integer represented by the given array of bytes.\n\n\
The argument bytes must either be a bytes-like object or an iterable\n\
producing bytes.\n\n\
The byteorder argument determines the byte order used to represent the\n\
integer.  Accepted values are \'big\' and \'little\', when the most\n\
significant byte is at the beginning or at the end of the byte array,\n\
respectively.\n\n\
The signed argument indicates whether two’s complement is used."},
    {"as_integer_ratio", as_integer_ratio, METH_NOARGS,
     ("as_integer_ratio($self, /)\n--\n\nReturn a pair of integers, "
      "whose ratio is equal to self.\n\n"
      "The ratio is in lowest terms and has a positive denominator.")},
    {"__trunc__", (PyCFunction)plus, METH_NOARGS,
     "__trunc__($self, /)\n--\n\nReturns self."},
    {"__floor__", (PyCFunction)plus, METH_NOARGS,
     "__floor__($self, /)\n--\n\nReturns self."},
    {"__ceil__", (PyCFunction)plus, METH_NOARGS,
     "__ceil__($self, /)\n--\n\nReturns self."},
    {"__round__", (PyCFunction)__round__, METH_FASTCALL,
     ("__round__($self, ndigits=0, /)\n--\n\n"
      "Round self to to the closest multiple of 10**-ndigits\n\n"
      "Always return an integer.  If two multiples are equally close,\n"
      "rounding is done toward the even choice.")},
    {"__reduce_ex__", __reduce_ex__, METH_O,
     ("__reduce_ex__($self, protocol, /)\n--\n\n"
      "Return state information for pickling.")},
    {"__format__", __format__, METH_O,
     ("__format__($self, format_spec, /)\n--\n\n"
      "Convert self to a string according to format_spec.")},
    {"__sizeof__", __sizeof__, METH_NOARGS,
     "__sizeof__($self, /)\n--\n\nReturns size of self in memory, in bytes."},
    {"is_integer", is_integer, METH_NOARGS,
     "is_integer($self, /)\n--\n\nReturns True."},
    {"digits", (PyCFunction)digits, METH_FASTCALL | METH_KEYWORDS,
     ("digits($self, base=10)\n--\n\n"
      "Return string representing self in the given base.\n\n"
      "Values for base can range between 2 to 36.")},
    {"_from_bytes", _from_bytes, METH_O | METH_CLASS, NULL},
    {NULL} /* sentinel */
};

PyDoc_STRVAR(mpz_doc,
             "mpz(number=0, /)\nmpz(string, /, base=10)\n\n\
Convert a number or a string to an integer.  If numeric argument is not\n\
an int subclass, return mpz(int(number)).\n\n\
If argument is not a number or if base is given, then it must be a string,\n\
bytes, or bytearray instance representing an integer literal in the\n\
given base.  The literal can be preceded by '+' or '-' and be surrounded\n\
by whitespace.  Valid bases are 0 and 2-36.  Base 0 means to interpret \n\
the base from the string as an integer literal.");

PyTypeObject MPZ_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "gmp.mpz",
    .tp_basicsize = sizeof(MPZ_Object),
    .tp_new = new,
    .tp_dealloc = dealloc,
    .tp_repr = repr,
    .tp_str = str,
    .tp_richcompare = richcompare,
    .tp_hash = hash,
    .tp_as_number = &as_number,
    .tp_getset = getsetters,
    .tp_methods = methods,
    .tp_doc = mpz_doc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_vectorcall = vectorcall,
};

static PyObject *
gmp_gcd(PyObject *Py_UNUSED(module), PyObject *const *args, Py_ssize_t nargs)
{
    MPZ_Object *res = MPZ_new();

    if (!res) {
        return (PyObject *)res; /* LCOV_EXCL_LINE */
    }
    for (Py_ssize_t i = 0; i < nargs; i++) {
        MPZ_Object *arg;

        CHECK_OP_INT(arg, args[i]);
        if (zz_cmp(&res->z, 1) == ZZ_EQ) {
            Py_DECREF(arg);
            continue;
        }
        if (zz_gcdext(&res->z, &arg->z, &res->z, NULL, NULL)) {
            /* LCOV_EXCL_START */
            Py_DECREF(res);
            Py_DECREF(arg);
            return PyErr_NoMemory();
            /* LCOV_EXCL_STOP */
        }
        Py_DECREF(arg);
    }
    return (PyObject *)res;
end:
    Py_DECREF(res);
    return NULL;
}

static PyObject *
gmp_gcdext(PyObject *Py_UNUSED(module), PyObject *const *args,
           Py_ssize_t nargs)
{
    if (nargs != 2) {
        PyErr_SetString(PyExc_TypeError, "gcdext() expects two arguments");
        return NULL;
    }
    MPZ_Object *x = NULL, *y = NULL;
    MPZ_Object *g = MPZ_new(), *s = MPZ_new(), *t = MPZ_new();

    if (!g || !s || !t) {
        /* LCOV_EXCL_START */
        Py_XDECREF((PyObject *)g);
        Py_XDECREF((PyObject *)s);
        Py_XDECREF((PyObject *)t);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    CHECK_OP_INT(x, args[0]);
    CHECK_OP_INT(y, args[1]);

    zz_err ret = zz_gcdext(&x->z, &y->z, &g->z, &s->z, &t->z);

    Py_XDECREF((PyObject *)x);
    Py_XDECREF((PyObject *)y);
    if (ret == ZZ_MEM) {
        return PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    PyObject *tup = PyTuple_Pack(3, g, s, t);

    Py_DECREF(g);
    Py_DECREF(s);
    Py_DECREF(t);
    return tup;
end:
    Py_DECREF(g);
    Py_DECREF(s);
    Py_DECREF(t);
    Py_XDECREF((PyObject *)x);
    Py_XDECREF((PyObject *)y);
    return NULL;
}

static PyObject *
gmp_lcm(PyObject *Py_UNUSED(module), PyObject *const *args, Py_ssize_t nargs)
{
    MPZ_Object *res = MPZ_new();

    if (!res || zz_set(1, &res->z)) {
        return PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    for (Py_ssize_t i = 0; i < nargs; i++) {
        MPZ_Object *arg;

        CHECK_OP_INT(arg, args[i]);
        if (zz_cmp(&res->z, 0) == ZZ_EQ) {
            Py_DECREF(arg);
            continue;
        }
        if (zz_lcm(&res->z, &arg->z, &res->z)) {
            /* LCOV_EXCL_START */
            Py_DECREF(res);
            Py_DECREF(arg);
            return PyErr_NoMemory();
            /* LCOV_EXCL_STOP */
        }
        Py_DECREF(arg);
    }
    return (PyObject *)res;
end:
    Py_DECREF(res);
    return NULL;
}

static PyObject *
gmp_isqrt(PyObject *Py_UNUSED(module), PyObject *arg)
{
    MPZ_Object *x, *root = MPZ_new();

    if (!root) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    CHECK_OP_INT(x, arg);

    zz_err ret = zz_sqrtrem(&x->z, &root->z, NULL);

    Py_DECREF(x);
    if (ret == ZZ_OK) {
        return (PyObject *)root;
    }
    if (ret == ZZ_VAL) {
        PyErr_SetString(PyExc_ValueError,
                        "isqrt() argument must be nonnegative");
    }
    if (ret == ZZ_MEM) {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
end:
    Py_DECREF(root);
    return NULL;
}

static PyObject *
gmp_isqrt_rem(PyObject *Py_UNUSED(module), PyObject *arg)
{
    MPZ_Object *x, *root = MPZ_new(), *rem = MPZ_new();
    PyObject *tup = NULL;

    if (!root || !rem) {
        /* LCOV_EXCL_START */
        Py_XDECREF((PyObject *)root);
        Py_XDECREF((PyObject *)rem);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    CHECK_OP_INT(x, arg);

    zz_err ret = zz_sqrtrem(&x->z, &root->z, &rem->z);

    Py_DECREF(x);
    if (ret == ZZ_OK) {
        tup = PyTuple_Pack(2, root, rem);
    }
    if (ret == ZZ_VAL) {
        PyErr_SetString(PyExc_ValueError,
                        "isqrt() argument must be nonnegative");
    }
    if (ret == ZZ_MEM) {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
end:
    Py_DECREF(root);
    Py_DECREF(rem);
    return tup;
}

static PyObject *
gmp_fac(PyObject *Py_UNUSED(module), PyObject *arg)
{
    MPZ_Object *x, *res = MPZ_new();

    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    CHECK_OP_INT(x, arg);
    if (zz_isneg(&x->z)) {
        PyErr_SetString(PyExc_ValueError,
                        "fac() not defined for negative values");
        goto err;
    }

    int64_t n;

    if (zz_get(&x->z, &n) || n > LONG_MAX) {
        PyErr_Format(PyExc_OverflowError,
                     "fac() argument should not exceed %ld",
                     LONG_MAX);
        goto err;
    }
    Py_XDECREF((PyObject *)x);
    if (zz_fac((zz_digit_t)n, &res->z)) {
        /* LCOV_EXCL_START */
        PyErr_NoMemory();
        goto err;
        /* LCOV_EXCL_STOP */
    }
    return (PyObject *)res;
err:
end:
    Py_DECREF(res);
    return NULL;
}

static PyObject *
gmp_comb(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (nargs != 2) {
        PyErr_SetString(PyExc_TypeError, "two arguments required");
        return NULL;
    }

    MPZ_Object *x, *y, *res = MPZ_new();

    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    CHECK_OP_INT(x, args[0]);
    CHECK_OP_INT(y, args[1]);
    if (zz_isneg(&x->z) || zz_isneg(&y->z)) {
        PyErr_SetString(PyExc_ValueError,
                        "comb() not defined for negative values");
        goto err;
    }

    int64_t n, k;

    if ((zz_get(&x->z, &n) || n > ULONG_MAX)
        || (zz_get(&y->z, &k) || k > ULONG_MAX))
    {
        PyErr_Format(PyExc_OverflowError,
                     "comb() arguments should not exceed %ld",
                     ULONG_MAX);
        goto err;
    }
    Py_XDECREF((PyObject *)x);
    Py_XDECREF((PyObject *)y);
    if (zz_bin((zz_digit_t)n, (zz_digit_t)k, &res->z)) {
        /* LCOV_EXCL_START */
        PyErr_NoMemory();
        goto err;
        /* LCOV_EXCL_STOP */
    }
    return (PyObject *)res;
err:
end:
    Py_DECREF(res);
    return NULL;
}

static PyObject *
gmp_perm(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (nargs > 2 || nargs < 1) {
        PyErr_SetString(PyExc_TypeError, "one or two arguments required");
        return NULL;
    }
    if (nargs == 1) {
        return gmp_fac(self, args[0]);
    }

    MPZ_Object *x, *y, *res = MPZ_new();

    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    CHECK_OP_INT(x, args[0]);
    CHECK_OP_INT(y, args[1]);
    if (zz_isneg(&x->z) || zz_isneg(&y->z)) {
        PyErr_SetString(PyExc_ValueError,
                        "perm() not defined for negative values");
        goto err;
    }

    int64_t n, k;

    if ((zz_get(&x->z, &n) || n > ULONG_MAX)
        || (zz_get(&y->z, &k) || k > ULONG_MAX))
    {
        PyErr_Format(PyExc_OverflowError,
                     "perm() arguments should not exceed %ld",
                     ULONG_MAX);
        goto err;
    }
    Py_XDECREF((PyObject *)x);
    Py_XDECREF((PyObject *)y);
    if (k > n) {
        return (PyObject *)res;
    }

    MPZ_Object *den = MPZ_new();

    if (!den) {
        /* LCOV_EXCL_START */
        PyErr_NoMemory();
        goto err;
        /* LCOV_EXCL_STOP */
    }
    if (zz_fac((zz_digit_t)n, &res->z)
        || zz_fac((zz_digit_t)(n-k), &den->z)
        || zz_div(&res->z, &den->z, &res->z, NULL))
    {
        /* LCOV_EXCL_START */
        Py_DECREF(den);
        PyErr_NoMemory();
        goto err;
        /* LCOV_EXCL_STOP */
    }
    Py_DECREF(den);
    return (PyObject *)res;
err:
end:
    Py_DECREF(res);
    return NULL;
}

typedef enum {
    ZZ_RNDD = 0,
    ZZ_RNDN = 1,
    ZZ_RNDU = 2,
    ZZ_RNDZ = 3,
    ZZ_RNDA = 4,
} zz_rnd;

static zz_rnd
get_round_mode(PyObject *rndstr)
{
    if (!PyUnicode_Check(rndstr)) {
invalid:
        PyErr_SetString(PyExc_ValueError, "invalid rounding mode specified");
        return (zz_rnd)-1;
    }

    Py_UCS4 rndchr = PyUnicode_ReadChar(rndstr, 0);
    zz_rnd rnd = ZZ_RNDN;

    assert(rndchr != -1);
    switch (rndchr) {
        case (Py_UCS4)'f':
            rnd = ZZ_RNDD;
            break;
        case (Py_UCS4)'c':
            rnd = ZZ_RNDU;
            break;
        case (Py_UCS4)'d':
            rnd = ZZ_RNDZ;
            break;
        case (Py_UCS4)'u':
            rnd = ZZ_RNDA;
            break;
        case (Py_UCS4)'n':
            rnd = ZZ_RNDN;
            break;
        default:
            goto invalid;
    }
    return rnd;
}

static zz_err
zz_mpmath_normalize(zz_bitcnt_t prec, zz_rnd rnd, bool *negative,
                     zz_t *man, zz_t *exp, zz_bitcnt_t *bc)
{
    /* If the mantissa is 0, return the normalized representation. */
    if (zz_iszero(man)) {
        *negative = false;
        *bc = 0;
        return zz_set(0, exp);
    }
    /* if size <= prec and the number is odd return it */
    if (*bc <= prec && zz_isodd(man)) {
        return ZZ_OK;
    }
    if (*bc > prec) {
        zz_bitcnt_t shift = *bc - prec;

do_rnd:
        switch (rnd) {
            case ZZ_RNDD:
                rnd = *negative ? ZZ_RNDA : ZZ_RNDZ;
                goto do_rnd;
            case ZZ_RNDU:
                rnd = *negative ? ZZ_RNDZ : ZZ_RNDA;
                goto do_rnd;
            case ZZ_RNDZ:
                zz_quo_2exp(man, shift, man);
                break;
            case ZZ_RNDA:
                zz_neg(man, man);
                zz_quo_2exp(man, shift, man);
                zz_abs(man, man);
                break;
            case ZZ_RNDN:
            default:
                {
                    bool t = zz_lsbpos(man) + 2 <= shift;

                    if (zz_quo_2exp(man, shift - 1, man)) {
                        return ZZ_MEM; /* LCOV_EXCL_LINE */
                    }
                    t = zz_isodd(man) && (man->digits[0]&2 || t);
                    if (zz_quo_2exp(man, 1, man)) {
                        return ZZ_MEM; /* LCOV_EXCL_LINE */
                    }
                    if (t && zz_add(man, 1, man)) {
                        return ZZ_MEM; /* LCOV_EXCL_LINE */
                    }
                }
        }

        zz_t tmp;

        if (zz_init(&tmp) || shift > INT64_MAX
            || zz_set((int64_t)shift, &tmp)
            || zz_add(exp, &tmp, exp))
        {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            return ZZ_MEM;
            /* LCOV_EXCL_STOP */
        }
        zz_clear(&tmp);
        *bc = prec;
    }

    zz_bitcnt_t zbits = 0;

    /* Strip trailing 0 bits. */
    if (!zz_iszero(man) && (zbits = zz_lsbpos(man))) {
        if (zz_quo_2exp(man, zbits, man)) {
            return ZZ_MEM; /* LCOV_EXCL_LINE */
        }
    }

    zz_t tmp;

    if (zz_init(&tmp) || zbits > INT64_MAX
        || zz_set((int64_t)zbits, &tmp)
        || zz_add(exp, &tmp, exp))
    {
        /* LCOV_EXCL_START */
        zz_clear(&tmp);
        return ZZ_MEM;
        /* LCOV_EXCL_STOP */
    }
    zz_clear(&tmp);
    *bc -= zbits;
    /* Check if one less than a power of 2 was rounded up. */
    if (zz_cmp(man, 1) == ZZ_EQ) {
        *bc = 1;
    }
    return ZZ_OK;
}
static PyObject *
gmp__mpmath_normalize(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (nargs != 6) {
        PyErr_SetString(PyExc_TypeError, "6 arguments required");
        return NULL;
    }

    long sign = PyLong_AsLong(args[0]);
    bool negative = (bool)sign;
    zz_bitcnt_t bc = PyLong_AsUnsignedLongLong(args[3]);
    zz_bitcnt_t prec = PyLong_AsUnsignedLongLong(args[4]);
    PyObject *rndstr = args[5];
    zz_rnd rnd = get_round_mode(rndstr);

    if (sign == -1 || bc == (zz_bitcnt_t)(-1) || prec == (zz_bitcnt_t)(-1)
        || !MPZ_Check(args[1]) || !PyLong_Check(args[2]))
    {
        PyErr_SetString(PyExc_TypeError,
                        ("arguments long, MPZ_Object*, PyObject*, "
                         "long, long, char needed"));
        return NULL;
    }
    if (rnd == -1) {
        return NULL;
    }

    MPZ_Object *man = (MPZ_Object *)plus(args[1]);
    MPZ_Object *exp = MPZ_from_int(args[2]);

    if (!exp || !man || zz_mpmath_normalize(prec, rnd, &negative,
                                            &man->z, &exp->z, &bc))
    {
        /* LCOV_EXCL_START */
        Py_XDECREF((PyObject *)man);
        Py_XDECREF((PyObject *)exp);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }

    PyObject *iexp = MPZ_to_int(exp);

    Py_DECREF(exp);
    if (!iexp) {
        /* LCOV_EXCL_START */
        Py_DECREF(man);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    return Py_BuildValue("(bNNK)", negative, man, iexp, bc);
}

static PyObject *
gmp__mpmath_create(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (nargs < 2 || nargs > 4) {
        PyErr_Format(PyExc_TypeError,
                     "_mpmath_create() takes from 2 to 4 arguments");
        return NULL;
    }

    MPZ_Object *man;

    if (MPZ_Check(args[0])) {
        man = (MPZ_Object *)plus(args[0]);
    }
    else if (PyLong_Check(args[0])) {
        man = MPZ_from_int(args[0]);
        if (!man) {
            return NULL; /* LCOV_EXCL_LINE */
        }
    }
    else {
        PyErr_Format(PyExc_TypeError, "_mpmath_create() expects an integer");
        return NULL;
    }
    if (!PyLong_Check(args[1])) {
        Py_DECREF(man);
        PyErr_Format(PyExc_TypeError,
                     "_mpmath_create() expects an integer exp");
        return NULL;
    }

    bool negative = zz_isneg(&man->z);
    zz_bitcnt_t bc = zz_bitlen(&man->z);
    zz_bitcnt_t prec = 0;
    zz_rnd rnd = ZZ_RNDZ;

    if (negative) {
        (void)zz_abs(&man->z, &man->z);
    }
    if (nargs > 2) {
        prec = PyLong_AsUnsignedLongLong(args[2]);
        if (prec == (zz_bitcnt_t)(-1) && PyErr_Occurred()) {
            Py_DECREF(man);
            PyErr_SetString(PyExc_TypeError, "bad prec argument");
            return NULL;
        }
    }
    if (nargs > 3) {
        rnd = get_round_mode(args[3]);

        if (rnd == -1) {
            Py_DECREF(man);
            return NULL;
        }
    }
    if (!prec) {
        prec = bc;
    }

    MPZ_Object *exp = MPZ_from_int(args[1]);

    if (!exp || zz_mpmath_normalize(prec, rnd, &negative,
                                    &man->z, &exp->z, &bc))
    {
        /* LCOV_EXCL_START */
        Py_DECREF(man);
        Py_XDECREF((PyObject *)exp);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }

    PyObject *iexp = MPZ_to_int(exp);

    Py_DECREF(exp);
    if (!iexp) {
        /* LCOV_EXCL_START */
        Py_DECREF(man);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    return Py_BuildValue("(bNNK)", negative, man, iexp, bc);
}

static PyObject *
gmp__free_cache(PyObject *Py_UNUSED(module), PyObject *Py_UNUSED(args))
{
    for (size_t i = 0; i < global.gmp_cache_size; i++) {
        MPZ_Object *u = global.gmp_cache[i];
        PyObject *self = (PyObject *)u;

        zz_clear(&u->z);
        PyObject_Free(self);
    }
    global.gmp_cache_size = 0;
    Py_RETURN_NONE;
}

static PyMethodDef gmp_functions[] = {
    {"gcd", (PyCFunction)gmp_gcd, METH_FASTCALL,
     ("gcd($module, /, *integers)\n--\n\n"
      "Greatest Common Divisor.")},
    {"gcdext", (PyCFunction)gmp_gcdext, METH_FASTCALL,
     ("gcdext($module, x, y, /)\n--\n\n"
      "Compute extended GCD.")},
    {"lcm", (PyCFunction)gmp_lcm, METH_FASTCALL,
     ("lcm($module, /, *integers)\n--\n\n"
      "Least Common Multiple.")},
    {"isqrt", gmp_isqrt, METH_O,
     ("isqrt($module, n, /)\n--\n\n"
      "Return the integer part of the square root of n.")},
    {"isqrt_rem", gmp_isqrt_rem, METH_O,
     ("isqrt_rem($module, n, /)\n--\n\n"
      "Return a 2-element tuple (s,t) such that s=isqrt(n) and t=n-s*s.")},
    {"factorial", gmp_fac, METH_O,
     ("factorial($module, n, /)\n--\n\n"
      "Find n!.")},
    {"comb", (PyCFunction)gmp_comb, METH_FASTCALL,
     ("comb($module, n, k, /)\n--\n\nNumber of ways to choose k"
      " items from n items without repetition and order.\n\n"
      "Also called the binomial coefficient.")},
    {"perm", (PyCFunction)gmp_perm, METH_FASTCALL,
     ("perm($module, n, k=None, /)\n--\n\nNumber of ways to choose k"
      " items from n items without repetition and with order.")},
    {"_mpmath_normalize", (PyCFunction)gmp__mpmath_normalize, METH_FASTCALL,
     NULL},
    {"_mpmath_create", (PyCFunction)gmp__mpmath_create, METH_FASTCALL, NULL},
    {"_free_cache", gmp__free_cache, METH_NOARGS, "Free mpz's cache."},
    {NULL} /* sentinel */
};

PyDoc_STRVAR(mpz_info__doc__,
             "gmp.mpz_info\n\n\
A named tuple that holds information about mpz type.\n\
The attributes are read only.");

static PyStructSequence_Field mpz_info_fields[] = {
    {"bits_per_digit", "size of a digit in bits"},
    {"sizeof_digit", "size in bytes of the C type, used to represent a digit"},
    {"bitcnt_max", "maximal count of bits in integer"},
    {NULL}};

static PyStructSequence_Desc mpz_info_desc = {
    "gmp.mpz_info", mpz_info__doc__, mpz_info_fields, 3};

static int
gmp_exec(PyObject *m)
{
    if (zz_setup()) {
        return -1; /* LCOV_EXCL_LINE */
    }
    if (PyModule_AddType(m, &MPZ_Type) < 0) {
        return -1; /* LCOV_EXCL_LINE */
    }

    PyTypeObject *MPZ_InfoType = PyStructSequence_NewType(&mpz_info_desc);

    if (!MPZ_InfoType) {
        return -1; /* LCOV_EXCL_LINE */
    }

    PyObject *mpz_info = PyStructSequence_New(MPZ_InfoType);
    const zz_layout *layout = zz_get_layout();

    bits_per_digit = layout->bits_per_digit;
    Py_DECREF(MPZ_InfoType);
    if (mpz_info == NULL) {
        return -1; /* LCOV_EXCL_LINE */
    }
    PyStructSequence_SetItem(mpz_info, 0, PyLong_FromLong(bits_per_digit));
    PyStructSequence_SetItem(mpz_info, 1, PyLong_FromLong(layout->digit_size));
    PyStructSequence_SetItem(mpz_info, 2,
                             PyLong_FromUInt64(zz_get_bitcnt_max()));
    if (PyErr_Occurred()) {
        /* LCOV_EXCL_START */
fail1:
        Py_DECREF(mpz_info);
        return -1;
        /* LCOV_EXCL_STOP */
    }
    if (PyModule_AddObject(m, "mpz_info", mpz_info) < 0) {
        goto fail1; /* LCOV_EXCL_LINE */
    }
    if (PyModule_AddStringConstant(m, "_zz_version", zz_get_version()) < 0) {
        goto fail1; /* LCOV_EXCL_LINE */
    }

    PyObject *ns = PyDict_New();

    if (!ns) {
        goto fail1; /* LCOV_EXCL_LINE */
    }
    if (PyDict_SetItemString(ns, "gmp", m) < 0) {
        /* LCOV_EXCL_START */
        Py_DECREF(mpz_info);
        Py_DECREF(ns);
        return -1;
        /* LCOV_EXCL_STOP */
    }

    const char *str = ("import numbers, importlib.metadata as imp\n"
                       "numbers.Integral.register(gmp.mpz)\n"
                       "gmp.fac = gmp.factorial\n"
                       "gmp.__all__ = ['comb', 'factorial', 'gcd', 'isqrt',\n"
                       "               'lcm', 'mpz', 'perm']\n"
                       "gmp.__version__ = imp.version('python-gmp')\n");
    PyObject *codeobj = Py_CompileString(str, "<file>", Py_file_input);
    PyObject *res;

    if (!codeobj) {
        goto fail1; /* LCOV_EXCL_LINE */
    }
    res = PyEval_EvalCode(codeobj, ns, NULL);
    Py_DECREF(codeobj);
    if (!res) {
        goto fail1; /* LCOV_EXCL_LINE */
    }
    Py_DECREF(res);

    PyObject *sys_mod = PyImport_ImportModuleLevel("sys", NULL, NULL, NULL, 0);

    if (!sys_mod || PyDict_SetItemString(ns, "sys", sys_mod) < 0) {
        /* LCOV_EXCL_START */
        Py_DECREF(ns);
        goto fail1;
        /* LCOV_EXCL_STOP */
    }
    codeobj = Py_CompileString("sys.hash_info.modulus", "<string>",
                               Py_eval_input);
    if (!codeobj || !(res = PyEval_EvalCode(codeobj, ns, NULL))) {
        /* LCOV_EXCL_START */
        Py_XDECREF(codeobj);
        Py_DECREF(ns);
        goto fail1;
        /* LCOV_EXCL_STOP */
    }
    Py_DECREF(codeobj);
    Py_DECREF(sys_mod);
    Py_DECREF(ns);
    pyhash_modulus = (Py_hash_t)PyLong_AsSsize_t(res);
    Py_DECREF(res);
    if (pyhash_modulus == -1) {
        goto fail1; /* LCOV_EXCL_LINE */
    }
    return 0;
}

#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif
static PyModuleDef_Slot gmp_slots[] = {
    {Py_mod_exec, gmp_exec},
#if PY_VERSION_HEX >= 0x030C0000
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
#endif
#if PY_VERSION_HEX >= 0x030D0000
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, NULL}};
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

static struct PyModuleDef gmp_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "gmp",
    .m_doc = "Bindings to the GNU GMP for Python.",
    .m_size = 0,
    .m_methods = gmp_functions,
    .m_slots = gmp_slots,
};

PyMODINIT_FUNC
PyInit_gmp(void)
{
    return PyModuleDef_Init(&gmp_module);
}
