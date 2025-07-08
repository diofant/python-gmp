#include "pythoncapi_compat.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "zz.h"
#include "mpz.h"

#include <float.h>
#include <gmp.h>
#include <setjmp.h>
#include <stdbool.h>

extern jmp_buf gmp_env;

#if !defined(_MSC_VER)
#  define _Py_thread_local _Thread_local
#else
#  define _Py_thread_local __declspec(thread)
#endif

#define TRACKER_MAX_SIZE 64
_Py_thread_local struct {
    size_t size;
    void *ptrs[TRACKER_MAX_SIZE];
} gmp_tracker;

static void *
gmp_reallocate_function(void *ptr, size_t old_size, size_t new_size)
{
    if (gmp_tracker.size >= TRACKER_MAX_SIZE) {
        goto err; /* LCOV_EXCL_LINE */
    }
    if (!ptr) {
        void *ret = malloc(new_size);

        if (!ret) {
            goto err; /* LCOV_EXCL_LINE */
        }
        gmp_tracker.ptrs[gmp_tracker.size] = ret;
        gmp_tracker.size++;
        return ret;
    }
    size_t i = gmp_tracker.size - 1;

    for (;; i--) {
        if (gmp_tracker.ptrs[i] == ptr) {
            break;
        }
    }

    void *ret = realloc(ptr, new_size);

    if (!ret) {
        goto err; /* LCOV_EXCL_LINE */
    }
    gmp_tracker.ptrs[i] = ret;
    return ret;
err:
    /* LCOV_EXCL_START */
    for (size_t i = 0; i < gmp_tracker.size; i++) {
        free(gmp_tracker.ptrs[i]);
        gmp_tracker.ptrs[i] = NULL;
    }
    gmp_tracker.size = 0;
    longjmp(gmp_env, 1);
    /* LCOV_EXCL_STOP */
}

static void *
gmp_allocate_function(size_t size)
{
    return gmp_reallocate_function(NULL, 0, size);
}

static void
gmp_free_function(void *ptr, size_t size)
{
    for (size_t i = gmp_tracker.size - 1; i >= 0; i--) {
        if (gmp_tracker.ptrs[i] == ptr) {
            gmp_tracker.ptrs[i] = NULL;
            break;
        }
    }
    free(ptr);

    size_t i = gmp_tracker.size - 1;

    while (gmp_tracker.size > 0) {
        if (gmp_tracker.ptrs[i]) {
            break;
        }
        gmp_tracker.size--;
        i--;
    }
}

#define LS(op) (((op)->z).digits)
#define SZ(op) (((op)->z).size)
#define ISNEG(op) (((op)->z).negative)

#if !defined(PYPY_VERSION) && !defined(Py_GIL_DISABLED)
#  define CACHE_SIZE (99)
#else
#  define CACHE_SIZE (0)
#endif
#define MAX_CACHE_MPZ_LIMBS (64)

typedef struct {
    MPZ_Object *gmp_cache[CACHE_SIZE + 1];
    size_t gmp_cache_size;
} gmp_global;

static gmp_global global = {
    .gmp_cache_size = 0,
};

typedef struct {
    void *(*default_allocate_func)(size_t);
    void *(*default_reallocate_func)(void *, size_t, size_t);
    void (*default_free_func)(void *, size_t);
} gmp_state;

static MPZ_Object *
MPZ_new(mp_size_t size, bool negative)
{
    MPZ_Object *res;

    if (global.gmp_cache_size && size <= MAX_CACHE_MPZ_LIMBS) {
        res = global.gmp_cache[--(global.gmp_cache_size)];
        if (SZ(res) < size && zz_resize(size, &res->z) == MP_MEM) {
            /* LCOV_EXCL_START */
            global.gmp_cache[(global.gmp_cache_size)++] = res;
            return (MPZ_Object *)PyErr_NoMemory();
            /* LCOV_EXCL_STOP */
        }
        Py_INCREF((PyObject *)res);
        SZ(res) = size;
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
    ISNEG(res) = negative;
    return res;
}

static const char *MPZ_TAG = "mpz(";
static int OPT_TAG = 0x1;
int OPT_PREFIX = 0x2;

PyObject *
MPZ_to_str(MPZ_Object *u, int base, int options)
{
    size_t len;

    if (zz_sizeinbase(&u->z, Py_ABS(base), &len)) {
        PyErr_SetString(PyExc_ValueError, "mpz base must be >= 2 and <= 36");
        return NULL;
    }
    if (options & OPT_TAG) {
        len += strlen(MPZ_TAG) + 1;
    }
    if (options & OPT_PREFIX) {
        len += 2;
    }
    len++;

    int8_t *buf = malloc(len), *p = buf;
    bool negative = ISNEG(u);

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
    else if (negative) {
        *(p++) = '-';
    }
    ISNEG(u) = false;
    if (zz_to_str(&u->z, base, p, &len)) {
        /* LCOV_EXCL_START */
        ISNEG(u) = negative;
        free(buf);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    ISNEG(u) = negative;
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

    MPZ_Object *res = MPZ_new(0, 0);

    if (!res) {
        return (MPZ_Object *)PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    while (len && isspace(*str)) {
        str++;
        len--;
    }

    bool negative = (str[0] == '-');

    str += negative;
    len -= negative;
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
    }
    if (base == 0) {
        base = 10;
    }

    int8_t *end = str + len - 1;

    while (len > 0 && isspace(*end)) {
        end--;
        len--;
    }

    mp_err ret = zz_from_str(str, len, base, &res->z);

    if (ret == MP_MEM) {
        /* LCOV_EXCL_START */
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    else if (ret == MP_VAL) {
        Py_DECREF(res);
        if (2 <= base && base <= 36) {
        err:
            PyErr_Format(PyExc_ValueError,
                         "invalid literal for mpz() with base %d: %.200R",
                         base, obj);
        }
        else {
            PyErr_SetString(PyExc_ValueError,
                            "mpz base must be >= 2 and <= 36, or 0");
        }
        return NULL;
    }
    ISNEG(res) = negative;
    return res;
}

static MPZ_Object *
MPZ_from_int(PyObject *obj)
{
#if !defined(PYPY_VERSION) && !defined(GRAALVM_PYTHON)
    PyLongExport long_export = {0, 0, 0, 0, 0};
    const mp_layout *int_layout = (mp_layout *)PyLong_GetNativeLayout();
    MPZ_Object *res = NULL;

    if (PyLong_Export(obj, &long_export) < 0) {
        return res; /* LCOV_EXCL_LINE */
    }
    if (long_export.digits) {
        res = MPZ_new(0, long_export.negative);
        if (!res || zz_import(long_export.ndigits,
                              long_export.digits, *int_layout, &res->z))
        {
            return (MPZ_Object *)PyErr_NoMemory(); /* LCOV_EXCL_LINE */
        }
        PyLong_FreeExport(&long_export);
    }
    else {
        res = MPZ_new(0, 0);
        if (res && zz_from_i64(long_export.value, &res->z)) {
            PyErr_NoMemory(); /* LCOV_EXCL_LINE */
        }
    }
    return res;
#else
    int64_t value;

    if (!PyLong_AsInt64(obj, &value)) {
        MPZ_Object *res = MPZ_new(0, 0);

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

    if (zz_to_i64(&u->z, &value) == MP_OK) {
        return PyLong_FromInt64(value);
    }

#if !defined(PYPY_VERSION) && !defined(GRAALVM_PYTHON)
    const mp_layout *int_layout = (mp_layout *)PyLong_GetNativeLayout();
    size_t size = (zz_bitlen(&u->z) + int_layout->bits_per_digit
                   - 1)/int_layout->bits_per_digit;
    void *digits;
    PyLongWriter *writer = PyLongWriter_Create(ISNEG(u), size, &digits);

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

static MPZ_Object *
MPZ_rshift1(const MPZ_Object *u, mp_limb_t rshift)
{
    MPZ_Object *res = MPZ_new(0, 0);

    if (!res || zz_quo_2exp(&u->z, rshift, &res->z)) {
        /* LCOV_EXCL_START */
        Py_XDECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    return res;
}

static void
revstr(unsigned char *s, size_t l, size_t r)
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
    mp_err ret = zz_to_bytes(&u->z, length, is_signed, &buffer);

    if (ret == MP_OK) {
        if (is_little && length) {
            revstr(buffer, 0, length - 1);
        }
        return bytes;
    }
    Py_DECREF(bytes);
    if (ret == MP_BUF) {
        if (ISNEG(u) && !is_signed) {
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
        uint8_t *tmp = malloc(length);

        if (!tmp) {
            /* LCOV_EXCL_START */
            Py_DECREF(bytes);
            return (MPZ_Object *)PyErr_NoMemory();
            /* LCOV_EXCL_STOP */
        }
        memcpy(tmp, buffer, length);
        revstr(tmp, 0, length - 1);
        buffer = tmp;
    }

    MPZ_Object *res = MPZ_new(0, 0);

    if (!res || zz_from_bytes(buffer, length, is_signed, &res->z)) {
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

#if (PY_VERSION_HEX >= 0x030D0000 || defined(PYPY_VERSION) \
     || defined(GRAALVM_PYTHON))
/* copied from CPython internals */
static PyObject *
PyUnicode_TransformDecimalAndSpaceToASCII(PyObject *unicode)
{
    if (PyUnicode_IS_ASCII(unicode)) {
        return Py_NewRef(unicode);
    }

    Py_ssize_t len = PyUnicode_GET_LENGTH(unicode);
    PyObject *result = PyUnicode_New(len, 127);

    if (result == NULL) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    Py_UCS1 *out = PyUnicode_1BYTE_DATA(result);
    int kind = PyUnicode_KIND(unicode);
    const void *data = PyUnicode_DATA(unicode);

    for (Py_ssize_t i = 0; i < len; ++i) {
        Py_UCS4 ch = PyUnicode_READ(kind, data, i);

        if (ch < 127) {
            out[i] = ch;
        }
        else if (Py_UNICODE_ISSPACE(ch)) {
            out[i] = ' ';
        }
        else {
            int decimal = Py_UNICODE_TODECIMAL(ch);

            if (decimal < 0) {
                out[i] = '?';
                out[i + 1] = '\0';
                ((PyASCIIObject *)result)->length = i + 1;
                break;
            }
            out[i] = '0' + decimal;
        }
    }
    return result;
}
#else
#  define PyUnicode_TransformDecimalAndSpaceToASCII \
      _PyUnicode_TransformDecimalAndSpaceToASCII
#endif

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
        if (PyNumber_Check(arg) && Py_TYPE(arg)->tp_as_number->nb_int) {
            PyObject *integer = Py_TYPE(arg)->tp_as_number->nb_int(arg);

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

            PyObject *res = (PyObject *)MPZ_from_int(integer);

            Py_DECREF(integer);
            return res;
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
        PyObject *asciistr = PyUnicode_TransformDecimalAndSpaceToASCII(arg);

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
    PyErr_SetString(PyExc_TypeError,
                    "can't convert non-string with explicit base");
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

        mp_size_t n = SZ(tmp);
        MPZ_Object *newobj = (MPZ_Object *)type->tp_alloc(type, 0);

        if (!newobj) {
            /* LCOV_EXCL_START */
            Py_DECREF(tmp);
            return NULL;
            /* LCOV_EXCL_STOP */
        }
        ISNEG(newobj) = ISNEG(tmp);
        if (zz_init(&newobj->z) || zz_resize(n, &newobj->z)) {
            /* LCOV_EXCL_START */
            Py_DECREF(tmp);
            return PyErr_NoMemory();
            /* LCOV_EXCL_STOP */
        }
        memcpy(LS(newobj), LS(tmp), sizeof(mp_limb_t)*n);

        Py_DECREF(tmp);
        return (PyObject *)newobj;
    }
    if (argc == 0) {
        return (PyObject *)MPZ_new(0, 0);
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

    if (global.gmp_cache_size < CACHE_SIZE
        && SZ(u) <= MAX_CACHE_MPZ_LIMBS
        && MPZ_CheckExact((PyObject *)u))
    {
        global.gmp_cache[(global.gmp_cache_size)++] = u;
    }
    else {
        zz_clear(&u->z);
        Py_TYPE((PyObject *)u)->tp_free((PyObject *)u);
    }
}

typedef struct gmp_pyargs {
    Py_ssize_t maxpos;
    Py_ssize_t minargs;
    Py_ssize_t maxargs;
    const char *fname;
    const char *const *keywords;
} gmp_pyargs;

static int
gmp_parse_pyargs(const gmp_pyargs *fnargs, int argidx[], PyObject *const *args,
                 Py_ssize_t nargs, PyObject *kwnames)
{
    if (nargs > fnargs->maxpos) {
        PyErr_Format(PyExc_TypeError,
                     "%s() takes at most %zu positional arguments",
                     fnargs->fname, fnargs->maxpos);
        return -1;
    }
    for (Py_ssize_t i = 0; i < nargs; i++) {
        argidx[i] = i;
    }

    Py_ssize_t nkws = 0;

    if (kwnames) {
        nkws = PyTuple_GET_SIZE(kwnames);
    }
    if (nkws > fnargs->maxpos) {
        PyErr_Format(PyExc_TypeError,
                     "%s() takes at most %zu keyword arguments", fnargs->fname,
                     fnargs->maxargs);
        return -1;
    }
    if (nkws + nargs < fnargs->minargs) {
        PyErr_Format(PyExc_TypeError,
                     ("%s() takes at least %zu positional or "
                      "keyword arguments"),
                     fnargs->fname, fnargs->minargs);
        return -1;
    }
    for (Py_ssize_t i = 0; i < nkws; i++) {
        const char *kwname = PyUnicode_AsUTF8(PyTuple_GET_ITEM(kwnames, i));
        Py_ssize_t j = 0;

        for (; j < fnargs->maxargs; j++) {
            if (strcmp(kwname, fnargs->keywords[j]) == 0) {
                if (j > fnargs->maxpos || nargs <= j) {
                    argidx[j] = (int)(nargs + i);
                    break;
                }
                else {
                    PyErr_Format(PyExc_TypeError,
                                 ("argument for %s() given by name "
                                  "('%s') and position (%zu)"),
                                 fnargs->fname, fnargs->keywords[j], j + 1);
                    return -1;
                }
            }
        }
        if (j == fnargs->maxargs) {
            PyErr_Format(PyExc_TypeError,
                         "%s() got an unexpected keyword argument '%s'",
                         fnargs->fname, kwname);
            return -1;
        }
    }
    return 0;
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
    int argidx[2] = {-1, -1};

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
        return (PyObject *)MPZ_new(0, 0);
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

PyObject * to_float(PyObject *self);

static PyObject *
richcompare(PyObject *self, PyObject *other, int op)
{
    MPZ_Object *u = NULL, *v = NULL;

    CHECK_OP(u, self);
    CHECK_OP(v, other);

    mp_ord r = zz_cmp(&u->z, &v->z);

    Py_XDECREF(u);
    Py_XDECREF(v);
    switch (op) {
        case Py_LT:
            return PyBool_FromLong(r == MP_LT);
        case Py_LE:
            return PyBool_FromLong(r != MP_GT);
        case Py_GT:
            return PyBool_FromLong(r == MP_GT);
        case Py_GE:
            return PyBool_FromLong(r != MP_LT);
        case Py_EQ:
            return PyBool_FromLong(r == MP_EQ);
        case Py_NE:
            return PyBool_FromLong(r != MP_EQ);
    }
    /* LCOV_EXCL_START */
    Py_RETURN_NOTIMPLEMENTED;
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return NULL;
    /* LCOV_EXCL_STOP */
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

    PyObject *res = PyObject_RichCompare(uf, vf, op);

    Py_DECREF(uf);
    Py_DECREF(vf);
    return res;
}

static Py_hash_t
hash(PyObject *self)
{
    MPZ_Object *u = (MPZ_Object *)self;
    Py_hash_t r = mpn_mod_1(LS(u), SZ(u), _PyHASH_MODULUS);

    if (ISNEG(u)) {
        r = -r;
    }
    if (r == -1) {
        r = -2;
    }
    return r;
}

#define UNOP(suff, func)                           \
    static PyObject *                              \
    func(PyObject *self)                           \
    {                                              \
        MPZ_Object *u = (MPZ_Object *)self;        \
        MPZ_Object *res = MPZ_new(0, 0);           \
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

PyObject *
to_float(PyObject *self)
{
    double d;
    mp_err ret = zz_to_double(&((MPZ_Object *)self)->z, &d);

    if (ret == MP_BUF) {
        PyErr_SetString(PyExc_OverflowError,
                        "integer too large to convert to float");
        return NULL;
    }
    return PyFloat_FromDouble(d);
}

static int
to_bool(PyObject *self)
{
    return SZ((MPZ_Object *)self) != 0;
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
        res = MPZ_new(0, 0);                                    \
        mp_err ret = MP_OK;                                     \
                                                                \
        if (!res || (ret = zz_##suff(&u->z, &v->z, &res->z))) { \
            /* LCOV_EXCL_START */                               \
            Py_CLEAR(res);                                      \
            if (ret == MP_VAL) {                                \
                PyErr_SetString(PyExc_ValueError,               \
                                "negative shift count");        \
            }                                                   \
            else if (ret == MP_BUF) {                           \
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
        res = (PyObject *)MPZ_new(0, 0);                 \
        if (!res) {                                      \
            goto end;                                    \
        }                                                \
                                                         \
        mp_err ret = zz_##suff(&u->z, &v->z,             \
                               &((MPZ_Object *)res)->z); \
                                                         \
        if (ret == MP_OK) {                              \
            goto end;                                    \
        }                                                \
        if (ret == MP_VAL) {                             \
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

static mp_err
zz_quo(const zz_t *u, const zz_t *v, zz_t *w)
{
    return zz_div(u, v, MP_RNDD, w, NULL);
}

static mp_err
zz_rem(const zz_t *u, const zz_t *v, zz_t *w)
{
    return zz_div(u, v, MP_RNDD, NULL, w);
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

    MPZ_Object *q = MPZ_new(0, 0);
    MPZ_Object *r = MPZ_new(0, 0);

    if (!q || !r) {
        /* LCOV_EXCL_START */
        Py_XDECREF(q);
        Py_XDECREF(r);
        return NULL;
        /* LCOV_EXCL_STOP */
    }

    mp_err ret = zz_div(&u->z, &v->z, MP_RNDD, &q->z, &r->z);

    if (ret) {
        Py_DECREF(q);
        Py_DECREF(r);
        if (ret == MP_VAL) {
            PyErr_SetString(PyExc_ZeroDivisionError,
                            "division by zero");
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

    mp_err ret = zz_truediv(&u->z, &v->z, &d);

    if (ret == MP_OK) {
        res = PyFloat_FromDouble(d);
        goto end;
    }
    if (ret == MP_VAL) {
        PyErr_SetString(PyExc_ZeroDivisionError,
                        "division by zero");
    }
    else if (ret == MP_BUF) {
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
    res = PyNumber_TrueDivide(uf, vf);
    Py_DECREF(uf);
    Py_DECREF(vf);
    return res;
}

static mp_err
zz_lshift(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (v->negative) {
        return MP_VAL;
    }
    if (v->size > 1) {
        return MP_BUF;
    }
    return zz_mul_2exp(u, v->size ? v->digits[0] : 0, w);
}

static mp_err
zz_rshift(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (v->negative) {
        return MP_VAL;
    }
    if (v->size > 1) {
        return zz_from_i32(u->negative ? -1 : 0, w);
    }
    return zz_quo_2exp(u, v->size ? v->digits[0] : 0, w);
}

BINOP_INT(lshift)
BINOP_INT(rshift)
BINOP_INT(and)
BINOP_INT(or)
BINOP_INT(xor)

static PyObject *
power(PyObject *self, PyObject *other, PyObject *module)
{
    MPZ_Object *res = NULL;
    MPZ_Object *u = NULL, *v = NULL;

    CHECK_OP(u, self);
    CHECK_OP(v, other);
    if (Py_IsNone(module)) {
        if (ISNEG(v)) {
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
        res = MPZ_new(0, 0);
        if (!res || SZ(v) > 1 || ISNEG(v)
            || zz_pow(&u->z, SZ(v) ? LS(v)[0] : 0, &res->z))
        {
            Py_CLEAR(res);
            PyErr_SetNone(PyExc_MemoryError); /* LCOV_EXCL_LINE */
        }
        Py_DECREF(u);
        Py_DECREF(v);
        return (PyObject *)res;
    }
    else {
        MPZ_Object *w = NULL;

        if (MPZ_Check(module)) {
            w = (MPZ_Object *)module;
            Py_INCREF(w);
        }
        else if (PyLong_Check(module)) {
            w = MPZ_from_int(module);
            if (!w) {
                goto end;
            }
        }
        else {
            PyErr_SetString(PyExc_TypeError,
                            ("pow() 3rd argument not allowed "
                             "unless all arguments are integers"));
            goto end;
        }

        mp_err ret = MP_OK;

        res = MPZ_new(0, 0);
        if (!res || (ret = zz_powm(&u->z, &v->z, &w->z, &res->z))) {
            /* LCOV_EXCL_START */
            if (ret == MP_VAL) {
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
    MPZ_Object *res = MPZ_new(0, 0);

    if (res && zz_from_i32(1, &res->z)) {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    return (PyObject *)res;
}

static PyObject *
get_zero(PyObject *Py_UNUSED(self), void *Py_UNUSED(closure))
{
    return (PyObject *)MPZ_new(0, 0);
}

static PyGetSetDef getsetters[] = {
    {"numerator", (getter)get_copy, NULL,
     "the numerator of a rational number in lowest terms", NULL},
    {"denominator", (getter)get_one, NULL,
     "the denominator of a rational number in lowest terms", NULL},
    {"real", (getter)get_copy, NULL, "the real part of a complex number",
     NULL},
    {"imag", (getter)get_zero, NULL, "the imaginary part of a complex number",
     NULL},
    {NULL} /* sentinel */
};

static PyObject *
bit_length(PyObject *self, PyObject *Py_UNUSED(args))
{
    MPZ_Object *u = (MPZ_Object *)self;
    mp_limb_t digit = zz_bitlen(&u->z);

    return PyLong_FromUnsignedLongLong(digit);
}

static PyObject *
bit_count(PyObject *self, PyObject *Py_UNUSED(args))
{
    MPZ_Object *u = (MPZ_Object *)self;
    mp_bitcnt_t count = SZ(u) ? mpn_popcount(LS(u), SZ(u)) : 0;

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
    int argidx[3] = {-1, -1, -1};

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
_from_bytes(PyObject *Py_UNUSED(module), PyObject *arg)
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
    int argidx[3] = {-1, -1, -1};

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

    MPZ_Object *r = MPZ_new(0, 0);

    if (!r) {
        /* LCOV_EXCL_START */
        Py_XDECREF(r);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    if (zz_div(&u->z, &((MPZ_Object *)p)->z, MP_RNDN, NULL, &r->z)) {
        /* LCOV_EXCL_START */
        Py_DECREF(r);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    Py_DECREF(p);

    MPZ_Object *res = MPZ_new(0, 0);

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

static PyObject *from_bytes_func;

static PyObject *
__reduce_ex__(PyObject *self, PyObject *Py_UNUSED(args))
{
    MPZ_Object *u = (MPZ_Object *)self;
    Py_ssize_t len = zz_bitlen(&u->z);

    return Py_BuildValue("O(N)", from_bytes_func,
                         MPZ_to_bytes(u, (len + 7)/8 + 1, 0, 1));
}

static PyObject *
__sizeof__(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    MPZ_Object *u = (MPZ_Object *)self;

    return PyLong_FromSize_t(sizeof(MPZ_Object) + SZ(u)*sizeof(mp_limb_t));
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
    static const char *const keywords[] = {"base", "prefix"};
    const static gmp_pyargs fnargs = {
        .keywords = keywords,
        .maxpos = 2,
        .minargs = 0,
        .maxargs = 2,
        .fname = "digits",
    };
    int argidx[2] = {-1, -1};

    if (gmp_parse_pyargs(&fnargs, argidx, args, nargs, kwnames) == -1) {
        return NULL;
    }

    int base = 10, prefix = 0;

    if (argidx[0] != -1) {
        PyObject *arg = args[argidx[0]];

        if (PyLong_Check(arg)) {
            base = PyLong_AsInt(arg);
            if (base == -1 && PyErr_Occurred()) {
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_TypeError,
                            "digits() takes an integer argument 'length'");
            return NULL;
        }
    }
    if (argidx[1] != -1 && PyObject_IsTrue(args[argidx[1]])) {
        prefix = OPT_PREFIX;
    }
    return MPZ_to_str((MPZ_Object *)self, base, prefix);
}

PyDoc_STRVAR(
    to_bytes__doc__,
    "to_bytes($self, /, length=1, byteorder=\'big\', *, signed=False)\n--\n\n\
Return an array of bytes representing an integer.\n\n\
  length\n\
    Length of bytes object to use.  An OverflowError is raised if the\n\
    integer is not representable with the given number of bytes.  Default\n\
    is length 1.\n\
  byteorder\n\
    The byte order used to represent the integer.  If byteorder is \'big\',\n\
    the most significant byte is at the beginning of the byte array.  If\n\
    byteorder is \'little\', the most significant byte is at the end of the\n\
    byte array.  To request the native byte order of the host system, use\n\
    sys.byteorder as the byte order value.  Default is to use \'big\'.\n\
  signed\n\
    Determines whether two\'s complement is used to represent the integer.\n\
    If signed is False and a negative integer is given, an OverflowError\n\
    is raised.");
PyDoc_STRVAR(
    from_bytes__doc__,
    "from_bytes($type, /, bytes, byteorder=\'big\', *, signed=False)\n--\n\n\
Return the integer represented by the given array of bytes.\n\n\
  bytes\n\
    Holds the array of bytes to convert.  The argument must either\n\
    support the buffer protocol or be an iterable object producing bytes.\n\
    Bytes and bytearray are examples of built-in objects that support the\n\
    buffer protocol.\n\
  byteorder\n\
    The byte order used to represent the integer.  If byteorder is \'big\',\n\
    the most significant byte is at the beginning of the byte array.  If\n\
    byteorder is \'little\', the most significant byte is at the end of the\n\
    byte array.  To request the native byte order of the host system, use\n\
    sys.byteorder as the byte order value.  Default is to use \'big\'.\n\
  signed\n\
    Indicates whether two\'s complement is used to represent the integer.");

extern PyObject * __format__(PyObject *self, PyObject *format_spec);

static PyMethodDef methods[] = {
    {"conjugate", (PyCFunction)plus, METH_NOARGS,
     "Returns self, the complex conjugate of any int."},
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
     ("Return a pair of integers, whose ratio is equal to "
      "the original int.\n\nThe ratio is in lowest terms "
      "and has a positive denominator.")},
    {"__trunc__", (PyCFunction)plus, METH_NOARGS,
     "Truncating an Integral returns itself."},
    {"__floor__", (PyCFunction)plus, METH_NOARGS,
     "Flooring an Integral returns itself."},
    {"__ceil__", (PyCFunction)plus, METH_NOARGS,
     "Ceiling of an Integral returns itself."},
    {"__round__", (PyCFunction)__round__, METH_FASTCALL,
     ("__round__($self, ndigits=None, /)\n--\n\n"
      "Rounding an Integral returns itself.\n\n"
      "Rounding with an ndigits argument also returns an integer.")},
    {"__reduce_ex__", __reduce_ex__, METH_O, NULL},
    {"__format__", __format__, METH_O,
     ("__format__($self, format_spec, /)\n--\n\n"
      "Convert to a string according to format_spec.")},
    {"__sizeof__", __sizeof__, METH_NOARGS,
     "Returns size in memory, in bytes."},
    {"is_integer", is_integer, METH_NOARGS,
     ("Returns True.  Exists for duck type compatibility "
      "with float.is_integer.")},
    {"digits", (PyCFunction)digits, METH_FASTCALL | METH_KEYWORDS,
     ("digits($self, base=10, prefix=False)\n--\n\n"
      "Return Python string representing self in the given base.\n\n"
      "Values for base can range between 2 to 62.")},
    {NULL} /* sentinel */
};

PyDoc_STRVAR(mpz_doc,
             "mpz(x, /)\n\
mpz(x, /, base=10)\n\
\n\
Convert a number or string to an integer, or return 0 if no arguments\n\
are given.  If x is a number, return x.__int__().  For floating-point\n\
numbers, this truncates towards zero.\n\
\n\
If x is not a number or if base is given, then x must be a string,\n\
bytes, or bytearray instance representing an integer literal in the\n\
given base.  The literal can be preceded by '+' or '-' and be surrounded\n\
by whitespace.  The base defaults to 10.  Valid bases are 0 and 2-36.\n\
Base 0 means to interpret the base from the string as an integer literal.");

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
    MPZ_Object *res = MPZ_new(0, 0);

    if (!res) {
        return (PyObject *)res; /* LCOV_EXCL_LINE */
    }
    for (Py_ssize_t i = 0; i < nargs; i++) {
        MPZ_Object *arg;

        if (MPZ_Check(args[i])) {
            arg = (MPZ_Object *)args[i];
            Py_INCREF(arg);
        }
        else if (PyLong_Check(args[i])) {
            arg = MPZ_from_int(args[i]);
            if (!arg) {
                /* LCOV_EXCL_START */
                Py_DECREF(res);
                return NULL;
                /* LCOV_EXCL_STOP */
            }
        }
        else {
            Py_DECREF(res);
            PyErr_SetString(PyExc_TypeError,
                            "gcd() arguments must be integers");
            return NULL;
        }
        if (zz_cmp_i32(&res->z, 1) == MP_EQ) {
            Py_DECREF(arg);
            continue;
        }

        MPZ_Object *tmp = MPZ_new(0, 0);

        if (!tmp || zz_gcd(&res->z, &arg->z, &tmp->z)) {
            /* LCOV_EXCL_START */
            Py_DECREF(res);
            Py_DECREF(arg);
            return PyErr_NoMemory();
            /* LCOV_EXCL_STOP */
        }
        Py_DECREF(arg);
        Py_SETREF(res, tmp);
    }
    if (zz_resize(SZ(res), &res->z) == MP_MEM) {
        /* LCOV_EXCL_START */
        Py_DECREF(res);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    return (PyObject *)res;
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
    MPZ_Object *g = MPZ_new(0, 0), *s = MPZ_new(0, 0), *t = MPZ_new(0, 0);

    if (!g || !s || !t) {
        /* LCOV_EXCL_START */
        Py_XDECREF(g);
        Py_XDECREF(s);
        Py_XDECREF(t);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    if (MPZ_Check(args[0])) {
        x = (MPZ_Object *)args[0];
        Py_INCREF(x);
    }
    else if (PyLong_Check(args[0])) {
        x = MPZ_from_int(args[0]);
        if (!x) {
            goto err; /* LCOV_EXCL_LINE */
        }
    }
    else {
        PyErr_SetString(PyExc_TypeError, "gcdext() expects integer arguments");
        goto err;
    }
    if (MPZ_Check(args[1])) {
        y = (MPZ_Object *)args[1];
        Py_INCREF(y);
    }
    else if (PyLong_Check(args[1])) {
        y = MPZ_from_int(args[1]);
        if (!y) {
            goto err; /* LCOV_EXCL_LINE */
        }
    }
    else {
        PyErr_SetString(PyExc_TypeError, "gcdext() expects integer arguments");
        goto err;
    }

    mp_err ret = zz_gcdext(&x->z, &y->z, &g->z, &s->z, &t->z);

    Py_XDECREF(x);
    Py_XDECREF(y);
    if (ret == MP_MEM) {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    if (ret == MP_OK) {
        PyObject *tup = PyTuple_Pack(3, g, s, t);

        Py_DECREF(g);
        Py_DECREF(s);
        Py_DECREF(t);
        return tup;
    }
err:
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
    MPZ_Object *x, *root = MPZ_new(0, 0);

    if (!root) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    if (MPZ_Check(arg)) {
        x = (MPZ_Object *)arg;
        Py_INCREF(x);
    }
    else if (PyLong_Check(arg)) {
        x = MPZ_from_int(arg);
        if (!x) {
            goto err; /* LCOV_EXCL_LINE */
        }
    }
    else {
        PyErr_SetString(PyExc_TypeError,
                        "isqrt() argument must be an integer");
        goto err;
    }

    mp_err ret = zz_sqrtrem(&x->z, &root->z, NULL);

    Py_DECREF(x);
    if (ret == MP_OK) {
        return (PyObject *)root;
    }
    if (ret == MP_VAL) {
        PyErr_SetString(PyExc_ValueError,
                        "isqrt() argument must be nonnegative");
    }
    if (ret == MP_MEM) {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
err:
    Py_DECREF(root);
    return NULL;
}

static PyObject *
gmp_isqrt_rem(PyObject *Py_UNUSED(module), PyObject *arg)
{
    MPZ_Object *x, *root = MPZ_new(0, 0), *rem = MPZ_new(0, 0);
    PyObject *tup = NULL;

    if (!root || !rem) {
        /* LCOV_EXCL_START */
        Py_XDECREF(root);
        Py_XDECREF(rem);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    if (MPZ_Check(arg)) {
        x = (MPZ_Object *)arg;
        Py_INCREF(x);
    }
    else if (PyLong_Check(arg)) {
        x = MPZ_from_int(arg);
        if (!x) {
            goto err; /* LCOV_EXCL_LINE */
        }
    }
    else {
        PyErr_SetString(PyExc_TypeError,
                        "isqrt() argument must be an integer");
        goto err;
    }

    mp_err ret = zz_sqrtrem(&x->z, &root->z, &rem->z);

    Py_DECREF(x);
    if (ret == MP_OK) {
        tup = PyTuple_Pack(2, root, rem);
    }
    if (ret == MP_VAL) {
        PyErr_SetString(PyExc_ValueError,
                        "isqrt() argument must be nonnegative");
    }
    if (ret == MP_MEM) {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
err:
    Py_DECREF(root);
    Py_DECREF(rem);
    return tup;
}

#define MAKE_MPZ_UI_FUN(name)                                            \
    static PyObject *                                                    \
    gmp_##name(PyObject *Py_UNUSED(module), PyObject *arg)               \
    {                                                                    \
        MPZ_Object *x, *res = MPZ_new(0, 0);                             \
                                                                         \
        if (!res) {                                                      \
            return NULL; /* LCOV_EXCL_LINE */                            \
        }                                                                \
        if (MPZ_Check(arg)) {                                            \
            x = (MPZ_Object *)arg;                                       \
            Py_INCREF(x);                                                \
        }                                                                \
        else if (PyLong_Check(arg)) {                                    \
            x = MPZ_from_int(arg);                                       \
            if (!x) {                                                    \
                goto err; /* LCOV_EXCL_LINE */                           \
            }                                                            \
        }                                                                \
        else {                                                           \
            PyErr_SetString(PyExc_TypeError,                             \
                            #name "() argument must be an integer");     \
            goto err;                                                    \
        }                                                                \
        if (ISNEG(x)) {                                                  \
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
        if (zz_##name(n, &res->z)) {                                     \
            /* LCOV_EXCL_START */                                        \
            PyErr_NoMemory();                                            \
            goto err;                                                    \
            /* LCOV_EXCL_STOP */                                         \
        }                                                                \
        return (PyObject *)res;                                          \
    err:                                                                 \
        Py_DECREF(res);                                                  \
        return NULL;                                                     \
    }

MAKE_MPZ_UI_FUN(fac)
MAKE_MPZ_UI_FUN(fac2)
MAKE_MPZ_UI_FUN(fib)

static PyObject *
build_mpf(long sign, MPZ_Object *man, PyObject *exp, mp_bitcnt_t bc)
{
    PyObject *tup, *tsign, *tbc;

    if (!(tup = PyTuple_New(4))) {
        /* LCOV_EXCL_START */
        Py_DECREF((PyObject *)man);
        Py_DECREF(exp);
        return NULL;
        /* LCOV_EXCL_STOP */
    }

    if (!(tsign = PyLong_FromLong(sign))) {
        /* LCOV_EXCL_START */
        Py_DECREF((PyObject *)man);
        Py_DECREF(exp);
        Py_DECREF(tup);
        return NULL;
        /* LCOV_EXCL_STOP */
    }

    if (!(tbc = PyLong_FromUnsignedLongLong(bc))) {
        /* LCOV_EXCL_START */
        Py_DECREF((PyObject *)man);
        Py_DECREF(exp);
        Py_DECREF(tup);
        Py_DECREF(tsign);
        return NULL;
        /* LCOV_EXCL_STOP */
    }

    PyTuple_SET_ITEM(tup, 0, tsign);
    PyTuple_SET_ITEM(tup, 1, (PyObject *)man);
    PyTuple_SET_ITEM(tup, 2, exp ? exp : PyLong_FromLong(0));
    PyTuple_SET_ITEM(tup, 3, tbc);
    return tup;
}

static PyObject *
normalize_mpf(long sign, MPZ_Object *man, PyObject *exp, mp_bitcnt_t bc,
              mp_bitcnt_t prec, Py_UCS4 rnd)
{
    mp_bitcnt_t zbits = 0;
    PyObject *newexp = NULL, *tmp = NULL;
    MPZ_Object *res = NULL;

    /* If the mantissa is 0, return the normalized representation. */
    if (!SZ(man)) {
        Py_INCREF((PyObject *)man);
        return build_mpf(0, man, 0, 0);
    }
    /* if bc <= prec and the number is odd return it */
    if ((bc <= prec) && LS(man)[0] & 1) {
        Py_INCREF((PyObject *)man);
        Py_INCREF((PyObject *)exp);
        return build_mpf(sign, man, exp, bc);
    }
    Py_INCREF(exp);
    if (bc > prec) {
        mp_bitcnt_t shift = bc - prec;

        switch (rnd) {
            case (Py_UCS4)'f':
                rnd = (Py_UCS4)(sign ? 'u' : 'd');
                break;
            case (Py_UCS4)'c':
                rnd = (Py_UCS4)(sign ? 'd' : 'u');
                break;
        }
        switch (rnd) {
            case (Py_UCS4)'d':
                res = MPZ_rshift1(man, shift);
                break;
            case (Py_UCS4)'u':
                if (!(tmp = PyNumber_Negative((PyObject *)man))) {
                    /* LCOV_EXCL_START */
                    Py_DECREF(exp);
                    return NULL;
                    /* LCOV_EXCL_STOP */
                }
                res = MPZ_rshift1((MPZ_Object *)tmp, shift);
                Py_DECREF(tmp);
                ISNEG(res) = 0;
                break;
            case (Py_UCS4)'n':
            default:
                res = MPZ_rshift1(man, shift - 1);

                int t = (LS(res)[0]&1 && (LS(res)[0]&2
                         || mpn_scan1(LS(man), 0) + 2 <= shift));

                zz_quo_2exp(&res->z, 1, &res->z);
                if (t && zz_add_i32(&res->z, 1, &res->z)) {
                    /* LCOV_EXCL_START */
                    Py_DECREF((PyObject *)res);
                    Py_DECREF(exp);
                    return NULL;
                    /* LCOV_EXCL_STOP */
                }
        }
        if (!(tmp = PyLong_FromUnsignedLongLong(shift))) {
            /* LCOV_EXCL_START */
            Py_DECREF((PyObject *)res);
            Py_DECREF(exp);
            return NULL;
            /* LCOV_EXCL_STOP */
        }
        if (!(newexp = PyNumber_Add(exp, tmp))) {
            /* LCOV_EXCL_START */
            Py_DECREF((PyObject *)res);
            Py_DECREF(exp);
            Py_DECREF(tmp);
            return NULL;
            /* LCOV_EXCL_STOP */
        }
        Py_SETREF(exp, newexp);
        Py_DECREF(tmp);
        bc = prec;
    }
    else {
        res = (MPZ_Object *)plus((PyObject *)man);
    }
    /* Strip trailing 0 bits. */
    if (SZ(res) && (zbits = mpn_scan1(LS(res), 0))) {
        tmp = (PyObject *)MPZ_rshift1(res, zbits);
        if (!tmp) {
            /* LCOV_EXCL_START */
            Py_DECREF((PyObject *)res);
            Py_DECREF(exp);
            return NULL;
            /* LCOV_EXCL_STOP */
        }
        Py_DECREF((PyObject *)res);
        res = (MPZ_Object *)tmp;
    }
    if (!(tmp = PyLong_FromUnsignedLongLong(zbits))) {
        /* LCOV_EXCL_START */
        Py_DECREF((PyObject *)res);
        Py_DECREF(exp);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    if (!(newexp = PyNumber_Add(exp, tmp))) {
        /* LCOV_EXCL_START */
        Py_DECREF((PyObject *)res);
        Py_DECREF(tmp);
        Py_DECREF(exp);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    Py_SETREF(exp, newexp);
    Py_DECREF(tmp);

    bc -= zbits;
    /* Check if one less than a power of 2 was rounded up. */
    if (zz_cmp_i32(&res->z, 1) == MP_EQ) {
        bc = 1;
    }
    return build_mpf(sign, res, exp, bc);
}

static PyObject *
gmp__mpmath_normalize(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (nargs != 6) {
        PyErr_SetString(PyExc_TypeError, "6 arguments required");
        return NULL;
    }

    long sign = PyLong_AsLong(args[0]);
    MPZ_Object *man = (MPZ_Object*)args[1];
    PyObject *exp = args[2];
    mp_bitcnt_t bc = PyLong_AsUnsignedLongLong(args[3]);
    mp_bitcnt_t prec = PyLong_AsUnsignedLongLong(args[4]);
    PyObject *rndstr = args[5];

    if (sign == -1 || bc == (mp_bitcnt_t)(-1) || prec == (mp_bitcnt_t)(-1)
        || !MPZ_Check(man))
    {
        PyErr_SetString(PyExc_TypeError,
                        ("arguments long, MPZ_Object*, PyObject*, "
                         "long, long, char needed"));
        return NULL;
    }
    if (!PyUnicode_Check(rndstr)) {
        PyErr_SetString(PyExc_ValueError, "invalid rounding mode specified");
        return NULL;
    }

    Py_UCS4 rnd = PyUnicode_READ_CHAR(rndstr, 0);

    return normalize_mpf(sign, man, exp, bc, prec, rnd);
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
            return NULL;
        }
    }
    else {
        PyErr_Format(PyExc_TypeError, "_mpmath_create() expects an integer");
        return NULL;
    }

    PyObject *exp = args[1];
    long sign = ISNEG(man);

    if (sign) {
        ISNEG(man) = 0;
    }

    mp_bitcnt_t bc = zz_bitlen(&man->z);
    mp_bitcnt_t prec = 0;
    Py_UCS4 rnd = 'd';

    if (nargs > 2) {
        prec = PyLong_AsUnsignedLongLong(args[2]);
        if (prec == (mp_bitcnt_t)(-1) && PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError, "bad prec argument");
            return NULL;
        }
    }
    if (nargs > 3) {
        PyObject *rndstr = args[3];

        if (!PyUnicode_Check(rndstr)) {
            PyErr_SetString(PyExc_ValueError,
                            "invalid rounding mode specified");
            return NULL;
        }
        rnd = PyUnicode_READ_CHAR(rndstr, 0);
    }

    if (!prec) {
        if (!SZ(man)) {
            return build_mpf(0, man, 0, 0);
        }

        mp_bitcnt_t zbits = 0;
        PyObject *tmp, *newexp;

        /* Strip trailing 0 bits. */
        if (SZ(man) && (zbits = mpn_scan1(LS(man), 0))) {
            tmp = (PyObject *)MPZ_rshift1(man, zbits);
            if (!tmp) {
                /* LCOV_EXCL_START */
                Py_DECREF((PyObject *)man);
                Py_DECREF(exp);
                return NULL;
                /* LCOV_EXCL_STOP */
            }
            Py_DECREF((PyObject *)man);
            man = (MPZ_Object *)tmp;
        }
        if (!(tmp = PyLong_FromUnsignedLongLong(zbits))) {
            /* LCOV_EXCL_START */
            Py_DECREF((PyObject *)man);
            Py_DECREF(exp);
            return NULL;
            /* LCOV_EXCL_STOP */
        }
        Py_INCREF(exp);
        if (!(newexp = PyNumber_Add(exp, tmp))) {
            /* LCOV_EXCL_START */
            Py_DECREF((PyObject *)man);
            Py_DECREF(tmp);
            Py_DECREF(exp);
            return NULL;
            /* LCOV_EXCL_STOP */
        }
        Py_SETREF(exp, newexp);
        Py_DECREF(tmp);
        bc -= zbits;
        PyObject *res = build_mpf(sign, man, exp, bc);
        return res;
    }

    PyObject *res = normalize_mpf(sign, man, exp, bc, prec, rnd);

    Py_DECREF(man);
    return res;
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
      "Return the integer part of the square root of the input.")},
    {"isqrt_rem", gmp_isqrt_rem, METH_O,
     ("isqrt_rem($module, n, /)\n--\n\n"
      "Return a 2-element tuple (s,t) such that s=isqrt(x) and t=x-s*s.")},
    {"factorial", gmp_fac, METH_O,
     ("factorial($module, n, /)\n--\n\n"
      "Find n!.")},
    {"double_fac", gmp_fac2, METH_O,
     ("double_fac($module, n, /)\n--\n\n"
      "Return the exact double factorial (n!!) of n.")},
    {"fib", gmp_fib, METH_O,
     ("fib($module, n, /)\n--\n\n"
      "Return the n-th Fibonacci number.")},
    {"_from_bytes", _from_bytes, METH_O, NULL},
    {"_mpmath_normalize", (PyCFunction)gmp__mpmath_normalize, METH_FASTCALL,
     NULL},
    {"_mpmath_create", (PyCFunction)gmp__mpmath_create, METH_FASTCALL, NULL},
    {NULL} /* sentinel */
};

PyDoc_STRVAR(gmp_info__doc__,
             "gmp.gmplib_info\n\
\n\
A named tuple that holds information about GNU GMP\n\
and it's internal representation of integers.\n\
The attributes are read only.");

static PyStructSequence_Field gmp_info_fields[] = {
    {"bits_per_limb", "size of a limb in bits"},
    {"sizeof_limb", "size in bytes of the C type used to represent a limb"},
    {"version", "the GNU GMP version"},
    {NULL}};

static PyStructSequence_Desc gmp_info_desc = {
    "gmp.gmplib_info", gmp_info__doc__, gmp_info_fields, 3};

static int
gmp_exec(PyObject *m)
{
    gmp_state *state = PyModule_GetState(m);

    mp_get_memory_functions(&state->default_allocate_func,
                            &state->default_reallocate_func,
                            &state->default_free_func);
    mp_set_memory_functions(gmp_allocate_function, gmp_reallocate_function,
                            gmp_free_function);
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
    PyStructSequence_SET_ITEM(gmp_info, 0, PyLong_FromLong(GMP_LIMB_BITS));
    PyStructSequence_SET_ITEM(gmp_info, 1, PyLong_FromLong(sizeof(mp_limb_t)));
    PyStructSequence_SET_ITEM(gmp_info, 2, PyUnicode_FromString(gmp_version));
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
    if (PyModule_AddObject(m, "fac",
                           PyObject_GetAttrString(m, "factorial")) < 0) {
        return -1; /* LCOV_EXCL_LINE */
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

    PyObject *numbers = PyImport_ImportModule("numbers");

    if (!numbers) {
        /* LCOV_EXCL_START */
        Py_DECREF(ns);
        return -1;
        /* LCOV_EXCL_STOP */
    }

    const char *str = "numbers.Integral.register(gmp.mpz)\n";

    if (PyDict_SetItemString(ns, "numbers", numbers) < 0) {
        /* LCOV_EXCL_START */
        Py_DECREF(numbers);
        Py_DECREF(ns);
        return -1;
        /* LCOV_EXCL_STOP */
    }

    PyObject *res = PyRun_String(str, Py_file_input, ns, ns);

    if (!res) {
        /* LCOV_EXCL_START */
        Py_DECREF(numbers);
        Py_DECREF(ns);
        return -1;
        /* LCOV_EXCL_STOP */
    }
    Py_DECREF(res);

    PyObject *importlib = PyImport_ImportModule("importlib.metadata");

    if (!importlib) {
        /* LCOV_EXCL_START */
        Py_DECREF(ns);
        return -1;
        /* LCOV_EXCL_STOP */
    }
    if (PyDict_SetItemString(ns, "importlib", importlib) < 0) {
        /* LCOV_EXCL_START */
        Py_DECREF(ns);
        Py_DECREF(importlib);
        return -1;
        /* LCOV_EXCL_STOP */
    }
    str = "gmp.__version__ = importlib.version('python-gmp')\n";
    res = PyRun_String(str, Py_file_input, ns, ns);
    if (!res) {
        /* LCOV_EXCL_START */
        Py_DECREF(ns);
        Py_DECREF(importlib);
        return -1;
        /* LCOV_EXCL_STOP */
    }
    Py_DECREF(ns);
    Py_DECREF(importlib);
    Py_DECREF(res);
    from_bytes_func = PyObject_GetAttrString(m, "_from_bytes");
    Py_INCREF(from_bytes_func);
    return 0;
}

static int
gmp_clear(PyObject *module)
{
    gmp_state *state = (gmp_state *)PyModule_GetState(module);

    mp_set_memory_functions(state->default_allocate_func,
                            state->default_reallocate_func,
                            state->default_free_func);
    return 0;
}

static void
gmp_free(void *module)
{
    (void)gmp_clear((PyObject *)module);
}

#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif
static PyModuleDef_Slot gmp_slots[] = {
    {Py_mod_exec, gmp_exec},
#if PY_VERSION_HEX >= 0x030C0000
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED},
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
    .m_size = sizeof(gmp_state),
    .m_methods = gmp_functions,
    .m_slots = gmp_slots,
    .m_free = gmp_free,
    .m_clear = gmp_clear,
};

PyMODINIT_FUNC
PyInit_gmp(void)
{
    return PyModuleDef_Init(&gmp_module);
}
