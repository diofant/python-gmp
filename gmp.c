#include "mpz.h"
#include "utils.h"

#include <ctype.h>
#include <float.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#  define _Thread_local __declspec(thread)
#endif

#if !defined(PYPY_VERSION)
#  define CACHE_SIZE (99)
#else
#  define CACHE_SIZE (0)
#endif
#define MAX_CACHE_MPZ_LIMBS (64)

typedef struct {
    MPZ_Object *gmp_cache[CACHE_SIZE + 1];
    size_t gmp_cache_size;
} gmp_global;

_Thread_local gmp_global global = {
    .gmp_cache_size = 0,
};

static MPZ_Object *
MPZ_new(int64_t size)
{
    MPZ_Object *res;

    if (global.gmp_cache_size && size <= MAX_CACHE_MPZ_LIMBS) {
        res = global.gmp_cache[--(global.gmp_cache_size)];
        if (zz_resize(size, &res->z) == ZZ_MEM) {
            /* LCOV_EXCL_START */
            global.gmp_cache[(global.gmp_cache_size)++] = res;
            return (MPZ_Object *)PyErr_NoMemory();
            /* LCOV_EXCL_STOP */
        }
        (void)zz_abs(&res->z, &res->z);
        Py_INCREF((PyObject *)res);
    }
    else {
        res = PyObject_New(MPZ_Object, &MPZ_Type);
        if (!res) {
            return NULL; /* LCOV_EXCL_LINE */
        }
        if (zz_init(&res->z) || zz_resize(size, &res->z)) {
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

    if ((base < INT8_MIN || base > INT8_MAX)
        || zz_sizeinbase(&u->z, (int8_t)base, &len))
    {
        goto bad_base;
    }
    if (options & OPT_TAG) {
        len += strlen(MPZ_TAG) + 1;
    }
    if (options & OPT_PREFIX) {
        len += 2;
    }
    len++;

    int8_t *buf = malloc(len), *p = buf;
    bool negative = zz_isneg(&u->z), cast_abs = false;

    if (!buf) {
        return PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    if (options & OPT_TAG) {
        strcpy((char *)p, MPZ_TAG);
        p += strlen(MPZ_TAG);
    }
    if (options & OPT_PREFIX) {
        if (negative) {
            *(p++) = '-';
            cast_abs = true;
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
    if (cast_abs) {
        (void)zz_abs(&u->z, &u->z);
    }

    zz_err ret = zz_to_str(&u->z, (int8_t)base, p, &len);

    if (cast_abs) {
        (void)zz_neg(&u->z, &u->z);
    }
    if (ret) {
        free(buf);
        if (ret == ZZ_VAL) {
bad_base:
            PyErr_SetString(PyExc_ValueError,
                            "mpz base must be >= 2 and <= 36");
            return NULL;
        }
        return PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    p += len;
    if (options & OPT_TAG) {
        *(p++) = ')';
    }
    *(p++) = '\0';

    PyObject *res = PyUnicode_FromString((char *)buf);

    free(buf);
    return res;
}

static MPZ_Object *
MPZ_from_str(PyObject *obj, int base)
{
    Py_ssize_t len;
    int8_t *str = (int8_t *)PyUnicode_AsUTF8AndSize(obj, &len);

    if (!str) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    if (base < 0 || base > INT8_MAX) {
        goto bad_base;
    }

    MPZ_Object *res = MPZ_new(0);

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

    int8_t *end = str + len - 1;

    while (len > 0 && isspace(*end)) {
        end--;
        len--;
    }

    zz_err ret = zz_from_str(str, (size_t)len, (int8_t)base, &res->z);

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
        res = MPZ_new(0);
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
        res = MPZ_new(0);
        if (res && zz_from_i64(long_export.value, &res->z)) {
            PyErr_NoMemory(); /* LCOV_EXCL_LINE */
        }
    }
    return res;
#else
    int64_t value;

    if (!PyLong_AsInt64(obj, &value)) {
        MPZ_Object *res = MPZ_new(0);

        if (res && zz_from_i64(value, &res->z)) {
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

    if (zz_to_i64(&u->z, &value) == ZZ_OK) {
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
    PyObject *str = MPZ_to_str(u, 16, 0);

    if (!str) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    PyObject *res = PyLong_FromUnicodeObject(str, 16);

    Py_DECREF(str);
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

static PyObject *
MPZ_to_bytes(MPZ_Object *u, Py_ssize_t length, int is_little, int is_signed)
{
    PyObject *bytes = PyBytes_FromStringAndSize(NULL, length);

    if (!bytes) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    uint8_t *buffer = (uint8_t *)PyBytes_AS_STRING(bytes);
    zz_err ret = zz_to_bytes(&u->z, (size_t)length, is_signed, &buffer);

    if (ret == ZZ_OK) {
        if (is_little && length) {
            revstr(buffer, 0, length - 1);
        }
        return bytes;
    }
    Py_DECREF(bytes);
    if (ret == ZZ_BUF) {
        if (zz_isneg(&u->z) && !is_signed) {
            PyErr_SetString(PyExc_OverflowError,
                            "can't convert negative mpz to unsigned");
        }
        else {
            PyErr_SetString(PyExc_OverflowError, "int too big to convert");
        }
        return NULL;
    }
    return PyErr_NoMemory(); /* LCOV_EXCL_LINE */
}

static MPZ_Object *
MPZ_from_bytes(PyObject *obj, int is_little, int is_signed)
{
    PyObject *bytes = PyObject_Bytes(obj);
    uint8_t *buffer;
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
        uint8_t *tmp = malloc((size_t)length);

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

    MPZ_Object *res = MPZ_new(0);

    if (!res || zz_from_bytes(buffer, (size_t)length, is_signed, &res->z)) {
        /* LCOV_EXCL_START */
        Py_DECREF(bytes);
        if (is_little) {
            free(buffer);
        }
        Py_XDECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    if (is_little) {
        free(buffer);
    }
    return res;
}

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

            if (Py_TYPE(arg)->tp_as_number->nb_int) {
                integer = Py_TYPE(arg)->tp_as_number->nb_int(arg);
                if (!integer) {
                    return NULL;
                }
                if (!PyLong_Check(integer)) {
                    PyErr_Format(PyExc_TypeError,
                                 "__int__ returned non-int (type %.200s)",
                                 Py_TYPE(integer)->tp_name);
                    Py_DECREF(integer);
                    return NULL;
                }
                if (!PyLong_CheckExact(integer)
                    && PyErr_WarnFormat(PyExc_DeprecationWarning, 1,
                                        "__int__ returned non-int (type %.200s).  "
                                        "The ability to return an instance of a "
                                        "strict subclass of int "
                                        "is deprecated, and may be removed "
                                        "in a future version of Python.",
                                        Py_TYPE(integer)->tp_name))
                {
                    Py_DECREF(integer);
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
                Py_SETREF(integer, (PyObject *)MPZ_from_int(integer));
                return integer;
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
            string = PyByteArray_AS_STRING(arg);
        }
        else {
            string = PyBytes_AS_STRING(arg);
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
    Py_ssize_t argc = PyTuple_GET_SIZE(args);
    PyObject *arg, *base = Py_None;

    if (type != &MPZ_Type) {
        MPZ_Object *tmp = (MPZ_Object *)new(&MPZ_Type, args, keywds);

        if (!tmp) {
            return NULL; /* LCOV_EXCL_LINE */
        }

        MPZ_Object *newobj = (MPZ_Object *)type->tp_alloc(type, 0);

        if (!newobj) {
            /* LCOV_EXCL_START */
            Py_DECREF(tmp);
            return NULL;
            /* LCOV_EXCL_STOP */
        }
        if (zz_init(&newobj->z) || zz_copy(&tmp->z, &newobj->z)) {
            /* LCOV_EXCL_START */
            Py_DECREF(tmp);
            return PyErr_NoMemory();
            /* LCOV_EXCL_STOP */
        }
        Py_DECREF(tmp);
        return (PyObject *)newobj;
    }
    if (argc == 0) {
        return (PyObject *)MPZ_new(0);
    }
    if (argc == 1 && !keywds) {
        arg = PyTuple_GET_ITEM(args, 0);
        return new_impl(type, arg, Py_None);
    }
    if (!PyArg_ParseTupleAndKeywords(args, keywds, "O|O",
                                     kwlist, &arg, &base))
    {
        return NULL;
    }
    return new_impl(type, arg, base);
}

static void
dealloc(PyObject *self)
{
    MPZ_Object *u = (MPZ_Object *)self;
    PyTypeObject *type = Py_TYPE(self);

    if (global.gmp_cache_size < CACHE_SIZE
        && (u->z).alloc <= MAX_CACHE_MPZ_LIMBS
        && MPZ_CheckExact(self))
    {
        global.gmp_cache[(global.gmp_cache_size)++] = u;
    }
    else {
        zz_clear(&u->z);
        type->tp_free(self);
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
        return (PyObject *)MPZ_new(0);
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
        Py_INCREF(u);           \
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
    zz_err ret = zz_to_double(&u->z, &d);

    if (ret == ZZ_BUF) {
        PyErr_SetString(PyExc_OverflowError,
                        "integer too large to convert to float");
        return NULL;
    }
    return PyFloat_FromDouble(d);
}

static PyObject *
richcompare(PyObject *self, PyObject *other, int op)
{
    MPZ_Object *u = (MPZ_Object *)self, *v = NULL;

    assert(MPZ_Check(self));
    CHECK_OP(v, other);

    zz_ord r = zz_cmp(&u->z, &v->z);

    Py_DECREF(v);
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

    bool negative = zz_isneg(&u->z);

    if (negative) {
        (void)zz_abs(&u->z, &u->z);
    }

    Py_hash_t r;

    assert(-(uint64_t)INT64_MIN > PyHASH_MODULUS);
    (void)zz_rem_u64(&u->z, (uint64_t)PyHASH_MODULUS, (uint64_t *)&r);
    if (negative) {
        (void)zz_neg(&u->z, &u->z);
        r = -r;
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
        MPZ_Object *res = MPZ_new(0);              \
                                                   \
        if (res && zz_##suff(&u->z, &res->z)) {    \
            PyErr_NoMemory(); /* LCOV_EXCL_LINE */ \
        }                                          \
        return (PyObject *)res;                    \
    }

UNOP(copy, plus)
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

#define BINOP_INT(suff)                                         \
    static PyObject *                                           \
    nb_##suff(PyObject *self, PyObject *other)                  \
    {                                                           \
        MPZ_Object *u = NULL, *v = NULL, *res = NULL;           \
                                                                \
        CHECK_OP(u, self);                                      \
        CHECK_OP(v, other);                                     \
                                                                \
        res = MPZ_new(0);                                       \
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
        Py_XDECREF(u);                                          \
        Py_XDECREF(v);                                          \
        return (PyObject *)res;                                 \
    fallback:                                                   \
    numbers:                                                    \
        Py_XDECREF(u);                                          \
        Py_XDECREF(v);                                          \
        Py_RETURN_NOTIMPLEMENTED;                               \
    }

#define BINOP(suff, slot)                                \
    static PyObject *                                    \
    nb_##suff(PyObject *self, PyObject *other)           \
    {                                                    \
        PyObject *res = NULL;                            \
        MPZ_Object *u = NULL, *v = NULL;                 \
                                                         \
        CHECK_OP(u, self);                               \
        CHECK_OP(v, other);                              \
                                                         \
        res = (PyObject *)MPZ_new(0);                    \
        if (!res) {                                      \
            goto end;                                    \
        }                                                \
                                                         \
        zz_err ret = zz_##suff(&u->z, &v->z,             \
                               &((MPZ_Object *)res)->z); \
                                                         \
        if (ret == ZZ_OK) {                              \
            goto end;                                    \
        }                                                \
        if (ret == ZZ_VAL) {                             \
            Py_CLEAR(res);                               \
            PyErr_SetString(PyExc_ZeroDivisionError,     \
                            "division by zero");         \
        }                                                \
        else {                                           \
            Py_CLEAR(res);                               \
            PyErr_NoMemory();                            \
        }                                                \
    end:                                                 \
        Py_XDECREF(u);                                   \
        Py_XDECREF(v);                                   \
        return res;                                      \
    fallback:                                            \
        Py_XDECREF(u);                                   \
        Py_XDECREF(v);                                   \
        Py_RETURN_NOTIMPLEMENTED;                        \
    numbers:                                             \
        Py_XDECREF(u);                                   \
        Py_XDECREF(v);                                   \
                                                         \
        PyObject *uf, *vf;                               \
                                                         \
        if (Number_Check(self)) {                        \
            uf = self;                                   \
            Py_INCREF(uf);                               \
        }                                                \
        else {                                           \
            uf = to_float(self);                         \
            if (!uf) {                                   \
                return NULL;                             \
            }                                            \
        }                                                \
        if (Number_Check(other)) {                       \
            vf = other;                                  \
            Py_INCREF(vf);                               \
        }                                                \
        else {                                           \
            vf = to_float(other);                        \
            if (!vf) {                                   \
                Py_DECREF(uf);                           \
                return NULL;                             \
            }                                            \
        }                                                \
        res = slot(uf, vf);                              \
        Py_DECREF(uf);                                   \
        Py_DECREF(vf);                                   \
        return res;                                      \
    }

BINOP(add, PyNumber_Add)
BINOP(sub, PyNumber_Subtract)
BINOP(mul, PyNumber_Multiply)

static zz_err
zz_quo(const zz_t *u, const zz_t *v, zz_t *w)
{
    return zz_div(u, v, ZZ_RNDD, w, NULL);
}

static zz_err
zz_rem(const zz_t *u, const zz_t *v, zz_t *w)
{
    return zz_div(u, v, ZZ_RNDD, NULL, w);
}

BINOP(quo, PyNumber_FloorDivide)
BINOP(rem, PyNumber_Remainder)

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

    MPZ_Object *q = MPZ_new(0);
    MPZ_Object *r = MPZ_new(0);

    if (!q || !r) {
        /* LCOV_EXCL_START */
        Py_XDECREF(q);
        Py_XDECREF(r);
        return NULL;
        /* LCOV_EXCL_STOP */
    }

    zz_err ret = zz_div(&u->z, &v->z, ZZ_RNDD, &q->z, &r->z);

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
    PyTuple_SET_ITEM(res, 0, (PyObject *)q);
    PyTuple_SET_ITEM(res, 1, (PyObject *)r);
    return res;
    /* LCOV_EXCL_START */
end:
    Py_DECREF(res);
    Py_XDECREF(u);
    Py_XDECREF(v);
    return NULL;
    /* LCOV_EXCL_STOP */
fallback:
numbers:
    Py_DECREF(res);
    Py_XDECREF(u);
    Py_XDECREF(v);
    Py_RETURN_NOTIMPLEMENTED;
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
    Py_XDECREF(u);
    Py_XDECREF(v);
    return res;
fallback:
    Py_XDECREF(u);
    Py_XDECREF(v);
    Py_RETURN_NOTIMPLEMENTED;
numbers:
    Py_XDECREF(u);
    Py_XDECREF(v);

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

static zz_err
zz_lshift(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (zz_isneg(v)) {
        return ZZ_VAL;
    }
    if (v->size > 1) {
        return ZZ_BUF;
    }
    return zz_mul_2exp(u, v->size ? v->digits[0] : 0, w);
}

static zz_err
zz_rshift(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (zz_isneg(v)) {
        return ZZ_VAL;
    }
    if (v->size > 1) {
        return zz_from_i32(zz_isneg(u) ? -1 : 0, w);
    }
    return zz_quo_2exp(u, v->size ? v->digits[0] : 0, w);
}

BINOP_INT(lshift)
BINOP_INT(rshift)
BINOP_INT(and)
BINOP_INT(or)
BINOP_INT(xor)

#define CHECK_OP_INT(u, a)      \
    if (MPZ_Check(a)) {         \
        u = (MPZ_Object *)a;    \
        Py_INCREF(u);           \
    }                           \
    else {                      \
        u = MPZ_from_int(a);    \
        if (!u) {               \
            goto end;           \
        }                       \
    }                           \

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
            resf = PyFloat_Type.tp_as_number->nb_power(uf, vf, Py_None);
            Py_DECREF(uf);
            Py_DECREF(vf);
            return resf;
        }
        res = MPZ_new(0);

        int64_t exp;

        if (!res || zz_to_i64(&v->z, &exp)
            || zz_pow(&u->z, (uint64_t)exp, &res->z))
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

        res = MPZ_new(0);
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
    Py_XDECREF(u);
    Py_XDECREF(v);
    return (PyObject *)res;
fallback:
    Py_XDECREF(u);
    Py_XDECREF(v);
    Py_RETURN_NOTIMPLEMENTED;
numbers:
    Py_XDECREF(u);
    Py_XDECREF(v);

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
    .nb_floor_divide = nb_quo,
    .nb_true_divide = nb_truediv,
    .nb_remainder = nb_rem,
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
    MPZ_Object *res = MPZ_new(0);

    if (res && zz_from_i32(1, &res->z)) {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    return (PyObject *)res;
}

static PyObject *
get_zero(PyObject *Py_UNUSED(self), void *Py_UNUSED(closure))
{
    return (PyObject *)MPZ_new(0);
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
    zz_limb_t digit = zz_bitlen(&u->z);

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
            const char *byteorder = PyUnicode_AsUTF8(arg);

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
            const char *byteorder = PyUnicode_AsUTF8(arg);

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
        return plus(self);
    }

    PyObject *ndigits = PyNumber_Index(args[0]);

    if (!ndigits) {
        return NULL;
    }
    if (!PyLong_IsNegative(ndigits)) {
        Py_DECREF(ndigits);
        return plus(self);
    }

    PyObject *tmp = PyNumber_Negative(ndigits);

    if (!tmp) {
        /* LCOV_EXCL_START */
        Py_DECREF(ndigits);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    Py_SETREF(ndigits, tmp);

    PyObject *ten = PyLong_FromLong(10);

    if (!ten) {
        /* LCOV_EXCL_START */
        Py_DECREF(ndigits);
        return NULL;
        /* LCOV_EXCL_STOP */
    }

    PyObject *p = power(ten, ndigits, Py_None);

    Py_DECREF(ten);
    Py_DECREF(ndigits);
    if (!p) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    MPZ_Object *r = MPZ_new(0);

    if (!r) {
        /* LCOV_EXCL_START */
        Py_XDECREF(r);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    if (zz_div(&u->z, &((MPZ_Object *)p)->z, ZZ_RNDN, NULL, &r->z)) {
        /* LCOV_EXCL_START */
        Py_DECREF(r);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    Py_DECREF(p);

    MPZ_Object *res = MPZ_new(0);

    if (!res || zz_sub(&u->z, &r->z, &res->z)) {
        /* LCOV_EXCL_START */
        Py_DECREF(r);
        Py_XDECREF(res);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    Py_DECREF(r);
    return (PyObject *)res;
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
                             + (unsigned int)(u->z).alloc*sizeof(zz_limb_t));
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

PyDoc_STRVAR(
    to_bytes__doc__,
    "to_bytes($self, /, length=1, byteorder=\'big\', *, signed=False)\n--\n\n\
Return an array of bytes representing self.\n\n\
The integer is represented using length bytes.  An OverflowError is\n\
raised if self is not representable with the given number of bytes.\n\n\
The byteorder argument determines the byte order used to represent self.\n\
Accepted values are \'big\' and \'little\', when the most significant\n\
byte is at the beginning or at the end of the byte array, respectively.\n\n\
The signed argument determines whether two\'s complement is used to\n\
represent self.  If signed is False and a negative integer is given,\n\
an OverflowError is raised.");
PyDoc_STRVAR(
    from_bytes__doc__,
    "from_bytes($type, /, bytes, byteorder=\'big\', *, signed=False)\n--\n\n\
Return the integer represented by the given array of bytes.\n\n\
The argument bytes must either be a bytes-like object or an iterable\n\
producing bytes.\n\n\
The byteorder argument determines the byte order used to represent the\n\
integer.  Accepted values are \'big\' and \'little\', when the most\n\
significant byte is at the beginning or at the end of the byte array,\n\
respectively.\n\n\
The signed argument indicates whether two’s complement is used.");

extern PyObject * __format__(PyObject *self, PyObject *format_spec);

static PyMethodDef methods[] = {
    {"conjugate", (PyCFunction)plus, METH_NOARGS,
     "Returns self."},
    {"bit_length", bit_length, METH_NOARGS,
     "Number of bits necessary to represent self in binary."},
    {"bit_count", bit_count, METH_NOARGS,
     ("Number of ones in the binary representation of the "
      "absolute value of self.")},
    {"to_bytes", (PyCFunction)to_bytes, METH_FASTCALL | METH_KEYWORDS,
     to_bytes__doc__},
    {"from_bytes", (PyCFunction)from_bytes,
     METH_FASTCALL | METH_KEYWORDS | METH_CLASS, from_bytes__doc__},
    {"as_integer_ratio", as_integer_ratio, METH_NOARGS,
     ("Return a pair of integers, whose ratio is equal to self.\n\n"
      "The ratio is in lowest terms and has a positive denominator.")},
    {"__trunc__", (PyCFunction)plus, METH_NOARGS, "Returns self."},
    {"__floor__", (PyCFunction)plus, METH_NOARGS, "Returns self."},
    {"__ceil__", (PyCFunction)plus, METH_NOARGS, "Returns self."},
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
     "Returns size of self in memory, in bytes."},
    {"is_integer", is_integer, METH_NOARGS, "Returns True."},
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
    MPZ_Object *res = MPZ_new(0);

    if (!res) {
        return (PyObject *)res; /* LCOV_EXCL_LINE */
    }
    for (Py_ssize_t i = 0; i < nargs; i++) {
        MPZ_Object *arg;

        CHECK_OP_INT(arg, args[i]);
        if (zz_cmp_i32(&res->z, 1) == ZZ_EQ) {
            Py_DECREF(arg);
            continue;
        }
        if (zz_gcd(&res->z, &arg->z, &res->z)) {
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
    MPZ_Object *g = MPZ_new(0), *s = MPZ_new(0), *t = MPZ_new(0);

    if (!g || !s || !t) {
        /* LCOV_EXCL_START */
        Py_XDECREF(g);
        Py_XDECREF(s);
        Py_XDECREF(t);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    CHECK_OP_INT(x, args[0]);
    CHECK_OP_INT(y, args[1]);

    zz_err ret = zz_gcdext(&x->z, &y->z, &g->z, &s->z, &t->z);

    Py_XDECREF(x);
    Py_XDECREF(y);
    if (ret == ZZ_MEM) {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
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
    Py_XDECREF(x);
    Py_XDECREF(y);
    return NULL;
}

static PyObject *
gmp_isqrt(PyObject *Py_UNUSED(module), PyObject *arg)
{
    MPZ_Object *x, *root = MPZ_new(0);

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
    MPZ_Object *x, *root = MPZ_new(0), *rem = MPZ_new(0);
    PyObject *tup = NULL;

    if (!root || !rem) {
        /* LCOV_EXCL_START */
        Py_XDECREF(root);
        Py_XDECREF(rem);
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

#define MAKE_MPZ_UI_FUN(name)                                            \
    static PyObject *                                                    \
    gmp_##name(PyObject *Py_UNUSED(module), PyObject *arg)               \
    {                                                                    \
        MPZ_Object *x, *res = MPZ_new(0);                                \
                                                                         \
        if (!res) {                                                      \
            return NULL; /* LCOV_EXCL_LINE */                            \
        }                                                                \
        CHECK_OP_INT(x, arg);                                            \
        if (zz_isneg(&x->z)) {                                           \
            PyErr_SetString(PyExc_ValueError,                            \
                            #name "() not defined for negative values"); \
            goto err;                                                    \
        }                                                                \
                                                                         \
        int64_t n;                                                       \
                                                                         \
        if (zz_to_i64(&x->z, &n) || n > LONG_MAX) {                      \
            PyErr_Format(PyExc_OverflowError,                            \
                         #name "() argument should not exceed %ld",      \
                         LONG_MAX);                                      \
            goto err;                                                    \
        }                                                                \
        Py_XDECREF(x);                                                   \
        if (zz_##name((uint64_t)n, &res->z)) {                           \
            /* LCOV_EXCL_START */                                        \
            PyErr_NoMemory();                                            \
            goto err;                                                    \
            /* LCOV_EXCL_STOP */                                         \
        }                                                                \
        return (PyObject *)res;                                          \
    err:                                                                 \
    end:                                                                 \
        Py_DECREF(res);                                                  \
        return NULL;                                                     \
    }

MAKE_MPZ_UI_FUN(fac)
MAKE_MPZ_UI_FUN(fac2)
MAKE_MPZ_UI_FUN(fib)

static zz_rnd
get_round_mode(PyObject *rndstr)
{
    if (!PyUnicode_Check(rndstr)) {
invalid:
        PyErr_SetString(PyExc_ValueError, "invalid rounding mode specified");
        return (zz_rnd)-1;
    }

    Py_UCS4 rndchr = PyUnicode_READ_CHAR(rndstr, 0);
    zz_rnd rnd = ZZ_RNDN;

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

    if (!exp || !man || _zz_mpmath_normalize(prec, rnd, &negative,
                                             &man->z, &exp->z, &bc))
    {
        /* LCOV_EXCL_START */
        Py_XDECREF(man);
        Py_XDECREF(exp);
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
end:
        return NULL;
    }

    MPZ_Object *man;

    CHECK_OP_INT(man, args[0]);
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

    if (!exp || _zz_mpmath_normalize(prec, rnd, &negative,
                                           &man->z, &exp->z, &bc))
    {
        /* LCOV_EXCL_START */
        Py_DECREF(man);
        Py_XDECREF(exp);
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
        PyTypeObject *type = Py_TYPE(self);

        zz_clear(&u->z);
        type->tp_free(self);
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
    {"isqrt", gmp_isqrt, METH_O,
     ("isqrt($module, n, /)\n--\n\n"
      "Return the integer part of the square root of n.")},
    {"isqrt_rem", gmp_isqrt_rem, METH_O,
     ("isqrt_rem($module, n, /)\n--\n\n"
      "Return a 2-element tuple (s,t) such that s=isqrt(n) and t=n-s*s.")},
    {"factorial", gmp_fac, METH_O,
     ("factorial($module, n, /)\n--\n\n"
      "Find n!.")},
    {"double_fac", gmp_fac2, METH_O,
     ("double_fac($module, n, /)\n--\n\n"
      "Return the exact double factorial n!!.")},
    {"fib", gmp_fib, METH_O,
     ("fib($module, n, /)\n--\n\n"
      "Return the n-th Fibonacci number.")},
    {"_mpmath_normalize", (PyCFunction)gmp__mpmath_normalize, METH_FASTCALL,
     NULL},
    {"_mpmath_create", (PyCFunction)gmp__mpmath_create, METH_FASTCALL, NULL},
    {"_free_cache", gmp__free_cache, METH_NOARGS, "Free mpz's cache."},
    {NULL} /* sentinel */
};

PyDoc_STRVAR(gmp_info__doc__,
             "gmp.gmplib_info\n\n\
A named tuple that holds information about GNU GMP\n\
and it's internal representation of integers.\n\
The attributes are read only.");

static PyStructSequence_Field gmp_info_fields[] = {
    {"bits_per_limb", "size of a limb in bits"},
    {"sizeof_limb", "size in bytes of the C type, used to represent a limb"},
    {"sizeof_limbcnt", ("size in bytes of the C type, used to"
                        " represent a count of limbs")},
    {"sizeof_bitcnt", ("size in bytes of the C type, used to"
                       " represent a count of bits")},
    {"version", "the GNU GMP version"},
    {NULL}};

static PyStructSequence_Desc gmp_info_desc = {
    "gmp.gmplib_info", gmp_info__doc__, gmp_info_fields, 5};

static int
gmp_exec(PyObject *m)
{
    static zz_info info;

    if (zz_setup(&info)) {
        return -1; /* LCOV_EXCL_LINE */
    }
    if (PyModule_AddType(m, &MPZ_Type) < 0) {
        return -1; /* LCOV_EXCL_LINE */
    }

    PyTypeObject *GMP_InfoType = PyStructSequence_NewType(&gmp_info_desc);

    if (!GMP_InfoType) {
        return -1; /* LCOV_EXCL_LINE */
    }

    PyObject *gmp_info = PyStructSequence_New(GMP_InfoType);

    Py_DECREF(GMP_InfoType);
    if (gmp_info == NULL) {
        return -1; /* LCOV_EXCL_LINE */
    }
    PyStructSequence_SET_ITEM(gmp_info, 0,
                              PyLong_FromLong(info.bits_per_limb));
    PyStructSequence_SET_ITEM(gmp_info, 1,
                              PyLong_FromLong(info.limb_bytes));
    PyStructSequence_SET_ITEM(gmp_info, 2,
                              PyLong_FromLong(info.limbcnt_bytes));
    PyStructSequence_SET_ITEM(gmp_info, 3,
                              PyLong_FromLong(info.bitcnt_bytes));
    PyStructSequence_SET_ITEM(gmp_info, 4,
                              PyUnicode_FromFormat("%d.%d.%d",
                                                   info.version[0],
                                                   info.version[1],
                                                   info.version[2]));
    if (PyErr_Occurred()) {
        /* LCOV_EXCL_START */
        Py_DECREF(gmp_info);
        return -1;
        /* LCOV_EXCL_STOP */
    }
    if (PyModule_AddObject(m, "gmp_info", gmp_info) < 0) {
        /* LCOV_EXCL_START */
        Py_DECREF(gmp_info);
        return -1;
        /* LCOV_EXCL_STOP */
    }

    PyObject *ns = PyDict_New();

    if (!ns) {
        return -1; /* LCOV_EXCL_LINE */
    }
    if (PyDict_SetItemString(ns, "gmp", m) < 0) {
        /* LCOV_EXCL_START */
        Py_DECREF(ns);
        return -1;
        /* LCOV_EXCL_STOP */
    }

    const char *str = ("import numbers, importlib.metadata as imp\n"
                       "numbers.Integral.register(gmp.mpz)\n"
                       "gmp.fac = gmp.factorial\n"
                       "gmp.__all__ = ['factorial', 'gcd', 'isqrt', 'mpz']\n"
                       "gmp.__version__ = imp.version('python-gmp')\n");

    PyObject *res = PyRun_String(str, Py_file_input, ns, ns);

    Py_DECREF(ns);
    if (!res) {
        return -1; /* LCOV_EXCL_LINE */
    }
    Py_DECREF(res);
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
