#include "mpz.h"
#include "extra.h"

#ifdef Py_GIL_DISABLED
#  include <stdatomic.h>
#endif

#ifdef ON_CPYTHON
#  define MAX_FREELIST_SIZE 100
#  define MAX_FREELIST_SIZEOF 256

typedef struct {
    MPZ_Object *freelist[MAX_FREELIST_SIZE + 1];
    size_t freelist_size;
} gmp_global;

_Thread_local gmp_global global = {
    .freelist_size = 0,
};
#endif

uint8_t bits_per_digit;
Py_hash_t pyhash_modulus;

static MPZ_Object *
MPZ_new(void)
{
    MPZ_Object *res;

#ifdef ON_CPYTHON
    if (global.freelist_size) {
        res = global.freelist[--global.freelist_size];
        (void)zz_set(0, &res->z);
        Py_INCREF((PyObject *)res);
    }
    else {
#endif
        res = PyObject_New(MPZ_Object, &MPZ_Type);
        if (!res) {
            return NULL; /* LCOV_EXCL_LINE */
        }
        if (zz_init(&res->z)) {
            return (MPZ_Object *)PyErr_NoMemory(); /* LCOV_EXCL_LINE */
        }
#ifdef ON_CPYTHON
    }
#endif
    res->hash = -1;
    return res;
}

static const char *MPZ_TAG = "mpz(";

static PyObject *
MPZ_to_str(MPZ_Object *u, int base, bool tag)
{
    size_t len = 0;
    bool negative = zz_isneg(&u->z);

    if (zz_sizeinbase(&u->z, base, &len)) {
        PyErr_SetString(PyExc_ValueError,
                        "mpz base must be >= 2 and <= 36");
        return NULL;
    }
    if (tag) {
        len += strlen(MPZ_TAG) + 1;
    }
    len += negative;
    len++; /* '\0' */

    char *buf = malloc(len), *p = buf;

    if (!buf) {
        return PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    if (tag) {
        strcpy(p, MPZ_TAG);
        p += strlen(MPZ_TAG);
    }

    zz_err ret = zz_get_str(&u->z, base, p);

    if (ret) {
        /* LCOV_EXCL_START */
        free(buf);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    p += strlen(p);
    if (tag) {
        *(p++) = ')';
        *p = '\0';
    }

    PyObject *res = PyUnicode_FromString(buf);

    free(buf);
    return res;
}

static MPZ_Object *
MPZ_from_str(PyObject *obj, int base)
{
    const char *str = PyUnicode_AsUTF8AndSize(obj, NULL);

    if (!str) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    MPZ_Object *res = MPZ_new();

    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    zz_err ret = zz_set_str(str, base, &res->z);

    if (ret == ZZ_MEM) {
        /* LCOV_EXCL_START */
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    else if (ret == ZZ_BUF) {
        /* LCOV_EXCL_START */
        Py_DECREF(res);
        PyErr_SetString(PyExc_OverflowError,
                        "too many digits in integer");
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    else if (ret == ZZ_VAL) {
        Py_DECREF(res);
        if (!base || (2 <= base && base <= 36)) {
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
    return res;
}

static MPZ_Object *
MPZ_from_int(PyObject *obj)
{
#if defined(ON_CPYTHON) && !defined(Py_LIMITED_API)
    PyLongExport long_export = {0, 0, 0, 0, 0};
    const zz_layout *int_layout = (zz_layout *)PyLong_GetNativeLayout();
    MPZ_Object *res = NULL;

    if (PyLong_Export(obj, &long_export) < 0) {
        return res; /* LCOV_EXCL_LINE */
    }
    if (long_export.digits) {
        res = MPZ_new();
        if (!res) {
            return NULL; /* LCOV_EXCL_LINE */
        }

        zz_err ret = zz_import((size_t)long_export.ndigits,
                               long_export.digits, *int_layout, &res->z);

        if (ret) {
            /* LCOV_EXCL_START */
            if (ret == ZZ_MEM) {
                return (MPZ_Object *)PyErr_NoMemory();
            }
            PyErr_SetString(PyExc_OverflowError,
                            "too many digits in integer");
            return NULL;
            /* LCOV_EXCL_STOP */
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
#endif /* defined(ON_CPYTHON) && !defined(Py_LIMITED_API) */
}

static PyObject *
MPZ_to_int(MPZ_Object *u)
{
    int64_t value;

    if (zz_get(&u->z, &value) == ZZ_OK) {
        return PyLong_FromInt64(value);
    }

#if defined(ON_CPYTHON) && !defined(Py_LIMITED_API)
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

    if (zz_get_str(&u->z, 16, buf)) {
        /* LCOV_EXCL_START */
        free(buf);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }

    PyObject *res = PyLong_FromString(buf, NULL, 16);

    free(buf);
    return res;
#endif /* defined(ON_CPYTHON) && !defined(Py_LIMITED_API) */
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

    if (!res) {
        /* LCOV_EXCL_START */
        Py_DECREF(bytes);
        if (is_little) {
            free(buffer);
        }
        return NULL;
        /* LCOV_EXCL_STOP */
    }

    zz_err ret = zz_set_bytes(buffer, (size_t)length, is_signed, &res->z);

    Py_DECREF(bytes);
    if (is_little) {
        free(buffer);
    }
    if (ret) {
        /* LCOV_EXCL_START */
        Py_DECREF((PyObject *)res);
        if (ret == ZZ_MEM) {
            return (MPZ_Object *)PyErr_NoMemory();
        }
        PyErr_SetString(PyExc_OverflowError,
                        "too many digits in integer");
        return NULL;
        /* LCOV_EXCL_STOP */
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
            PyObject *integer = PyNumber_Long(arg);
            PyObject *mpz = NULL;

            if (integer) {
                mpz = (PyObject *)MPZ_from_int(integer);
                Py_DECREF(integer);
            }
            return mpz;
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
        PyObject *res = (PyObject *)MPZ_from_str(arg, base);

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

static void
dealloc(PyObject *self)
{
    MPZ_Object *u = (MPZ_Object *)self;

#ifdef ON_CPYTHON
    if (global.freelist_size < MAX_FREELIST_SIZE
        && zz_sizeof(&u->z) <= MAX_FREELIST_SIZEOF
        && MPZ_CheckExact(self))
    {
        global.freelist[global.freelist_size++] = u;
    }
    else {
#endif
        freefunc tp_free;

        if (MPZ_CheckExact(self)) {
            tp_free = PyObject_Free;
        }
        else {
            tp_free = PyType_GetSlot(Py_TYPE(self), Py_tp_free);
        }
        zz_clear(&u->z);
        tp_free(self);
#ifdef ON_CPYTHON
    }
#endif
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
    return MPZ_to_str((MPZ_Object *)self, 10, true);
}

static PyObject *
str(PyObject *self)
{
    return MPZ_to_str((MPZ_Object *)self, 10, false);
}

#define Number_Check(op) (PyFloat_Check((op)) || PyComplex_Check((op)))

#define CHECK_OP(u, a)                  \
    if (MPZ_Check(a)) {                 \
        u = (MPZ_Object *)Py_NewRef(a); \
    }                                   \
    else if (PyLong_Check(a)) {         \
        u = MPZ_from_int(a);            \
        if (!u) {                       \
            goto end;                   \
        }                               \
    }                                   \
    else if (Number_Check(a)) {         \
        goto numbers;                   \
    }                                   \
    else {                              \
        goto fallback;                  \
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
    self = MPZ_to_int((MPZ_Object *)self);
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
#ifdef Py_GIL_DISABLED
    Py_hash_t hash = atomic_load_explicit((const _Atomic(Py_hash_t) *)&u->hash,
                                          memory_order_relaxed);
#else
    Py_hash_t hash = u->hash;
#endif

    if (hash != -1) {
        return hash;
    }

    zz_t w;

    if (zz_init(&w)) {
        return -1; /* LCOV_EXCL_LINE */
    }
    assert((int64_t)INT64_MAX > pyhash_modulus);
    (void)zz_rem(&u->z, (int64_t)pyhash_modulus, &w);
    assert(sizeof(Py_hash_t) == 8);
    (void)zz_get(&w, (int64_t *)&hash);
    zz_clear(&w);
    if (zz_isneg(&u->z) && hash) {
        hash = -(pyhash_modulus - hash);
    }
    if (hash == -1) {
        hash = -2;
    }
#ifdef Py_GIL_DISABLED
    atomic_store_explicit((_Atomic(Py_hash_t) *)&u->hash,
                          hash, memory_order_relaxed);
    return hash;
#else
    return u->hash = hash;
#endif
}

#define UNOP(suff, func)                                       \
    static PyObject *                                          \
    func(PyObject *self)                                       \
    {                                                          \
        MPZ_Object *u = (MPZ_Object *)self;                    \
        MPZ_Object *res = MPZ_new();                           \
                                                               \
        if (!res) {                                            \
            return NULL;                                       \
        }                                                      \
                                                               \
        zz_err ret = zz_##suff(&u->z, &res->z);                \
                                                               \
        if (ret) {                                             \
            Py_CLEAR(res);                                     \
            if (ret == ZZ_BUF) {                               \
                PyErr_SetString(PyExc_OverflowError,           \
                                "too many digits in integer"); \
            }                                                  \
            else {                                             \
                PyErr_NoMemory();                              \
            }                                                  \
        }                                                      \
        return (PyObject *)res;                                \
    }

UNOP(pos, plus)
UNOP(neg, nb_negative)
UNOP(abs, nb_absolute)
UNOP(invert, nb_invert) /* may overflow */

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

#define CHECK_OPv2(u, a)                     \
    if (MPZ_Check(a)) {                      \
        u = (MPZ_Object *)Py_NewRef(a);      \
    }                                        \
    else if (PyLong_Check(a)) {              \
        int error;                           \
                                             \
        ival = PyLong_AsSdigit_t(a, &error); \
        if (error) {                         \
            u = MPZ_from_int(a);             \
            if (!u) {                        \
                goto end;                    \
            }                                \
        }                                    \
    }                                        \
    else if (Number_Check(a)) {              \
        goto numbers;                        \
    }                                        \
    else {                                   \
        goto fallback;                       \
    }

#define BINOP(suff, slot)                                       \
    static PyObject *                                           \
    nb_##suff(PyObject *self, PyObject *other)                  \
    {                                                           \
        MPZ_Object *u = NULL, *v = NULL, *res = NULL;           \
        int64_t ival = 0;                                       \
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
            ret = zz_##suff(ival, &v->z, &res->z);              \
        }                                                       \
        else if (!v) {                                          \
            ret = zz_##suff(&u->z, ival, &res->z);              \
        }                                                       \
        else {                                                  \
            ret = zz_##suff(&u->z, &v->z, &res->z);             \
        }                                                       \
        if (ret) {                                              \
            Py_CLEAR(res);                                      \
            if (ret == ZZ_VAL) {                                \
                PyErr_SetString(PyExc_ZeroDivisionError,        \
                                "division by zero");            \
            }                                                   \
            else if (ret == ZZ_BUF) {                           \
                PyErr_SetString(PyExc_OverflowError,            \
                                "too many digits in integer");  \
            }                                                   \
            else {                                              \
                PyErr_NoMemory();                               \
            }                                                   \
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
            uf = Py_NewRef(self);                               \
        }                                                       \
        else {                                                  \
            uf = to_float(self);                                \
            if (!uf) {                                          \
                return NULL;                                    \
            }                                                   \
        }                                                       \
        if (Number_Check(other)) {                              \
            vf = Py_NewRef(other);                              \
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

/* can't overflow */
BINOP(quo, PyNumber_FloorDivide)
BINOP(rem, PyNumber_Remainder)

#define CHECK_OP_INT(u, a)              \
    if (MPZ_Check(a)) {                 \
        u = (MPZ_Object *)Py_NewRef(a); \
    }                                   \
    else {                              \
        u = MPZ_from_int(a);            \
        if (!u) {                       \
            goto end;                   \
        }                               \
    }                                   \

static PyObject *
nb_divmod(PyObject *self, PyObject *other)
{
    PyObject *res = PyTuple_New(2);
    MPZ_Object *u = NULL, *v = NULL;

    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    CHECK_OP_INT(u, self);
    CHECK_OP_INT(v, other);

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
        uf = Py_NewRef(self);
    }
    else {
        uf = to_float(self);
        if (!uf) {
            return NULL; /* LCOV_EXCL_LINE */
        }
    }
    if (Number_Check(other)) {
        vf = Py_NewRef(other);
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
        if (res && (ret = zz_##suff(&u->z, &v->z, &res->z))) {  \
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

BINOP_INT(and)
BINOP_INT(or)
BINOP_INT(xor)
BINOP_INT(lshift)
BINOP_INT(rshift)

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

            ternaryfunc nb_power = PyType_GetSlot(&PyFloat_Type, Py_nb_power);

            resf = nb_power(uf, vf, Py_None);
            Py_DECREF(uf);
            Py_DECREF(vf);
            return resf;
        }

        uint64_t exp;

        if (zz_get(&v->z, &exp)) {
            PyErr_SetString(PyExc_OverflowError,
                            "too many digits in integer");
        }
        else {
            res = MPZ_new();
            if (res) {
                zz_err ret = zz_pow(&u->z, exp, &res->z);

                if (ret) {
                    Py_CLEAR(res);
                    if (ret == ZZ_MEM) {
                        PyErr_SetNone(PyExc_MemoryError); /* LCOV_EXCL_LINE */
                    }
                    else {
                        PyErr_SetString(PyExc_OverflowError,
                                        "too many digits in integer");
                    }
                }
            }
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
        uf = Py_NewRef(self);
    }
    else {
        uf = to_float(self);
        if (!uf) {
            return NULL;
        }
    }
    if (Number_Check(other)) {
        vf = Py_NewRef(other);
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

    if (MPZ_Check(args[0])) {
        ndigits = (MPZ_Object *)plus(args[0]);
    }
    else {
        ndigits = MPZ_from_int(args[0]);
    }
    if (!ndigits) {
        return NULL;
    }
    if (!zz_isneg(&ndigits->z)) {
        Py_DECREF(ndigits);
        goto noop;
    }

    zz_t divisor;
    uint64_t exp;

    (void)zz_neg(&ndigits->z, &ndigits->z);
    if (zz_init(&divisor) || zz_set(10, &divisor)) {
        /* LCOV_EXCL_START */
        zz_clear(&divisor);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    if (zz_get(&ndigits->z, &exp)) {
        zz_clear(&divisor);
        goto overflow;
    }

    zz_err ret = zz_pow(&divisor, exp, &divisor);

    if (ret) {
        zz_clear(&divisor);
        Py_DECREF(ndigits);
        if (ret == ZZ_MEM) {
            return PyErr_NoMemory(); /* LCOV_EXCL_LINE */
        }
overflow:
        PyErr_SetString(PyExc_OverflowError,
                        "too many digits in integer");
        return NULL;
    }
    Py_DECREF(ndigits);

    MPZ_Object *res = MPZ_new();

    if (!res) {
        /* LCOV_EXCL_START */
        zz_clear(&divisor);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    if (zz_divnear(&u->z, &divisor, NULL, &res->z)) {
        /* LCOV_EXCL_START */
        zz_clear(&divisor);
        Py_DECREF(res);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    zz_clear(&divisor);
    if (zz_sub(&u->z, &res->z, &res->z)) {
        /* LCOV_EXCL_START */
        Py_DECREF(res);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
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

    return PyLong_FromSize_t(sizeof(MPZ_Object) - sizeof(zz_t)
                             + zz_sizeof(&u->z));
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
    return MPZ_to_str((MPZ_Object *)self, base, false);
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
Convert a number or an ASCII string to an integer.  If numeric argument is not\n\
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

    Py_DECREF(x);
    Py_DECREF(y);
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

        zz_err ret = zz_lcm(&res->z, &arg->z, &res->z);

        if (ret) {
            /* LCOV_EXCL_START */
            Py_DECREF(res);
            Py_DECREF(arg);
            if (ret == ZZ_BUF) {
                PyErr_SetString(PyExc_OverflowError,
                                "too many digits in integer");
                return NULL;
            }
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
    else {
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
    else if (ret == ZZ_VAL) {
        PyErr_SetString(PyExc_ValueError,
                        "isqrt() argument must be nonnegative");
    }
    else {
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

    uint64_t n;

    if (zz_get(&x->z, &n)) {
overflow:
        PyErr_Format(PyExc_OverflowError,
                     "fac() argument should not exceed %ld",
                     ULONG_MAX);
        goto err;
    }
    Py_DECREF((PyObject *)x);

    zz_err ret = zz_fac((zz_digit_t)n, &res->z);

    if (ret) {
        /* LCOV_EXCL_START */
        if (ret == ZZ_BUF) {
            goto overflow;
        }
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

    uint64_t n, k;

    if (zz_get(&x->z, &n) || zz_get(&y->z, &k)) {
overflow:
        PyErr_Format(PyExc_OverflowError,
                     "comb() arguments should not exceed %ld",
                     ULONG_MAX);
        goto err;
    }
    Py_DECREF((PyObject *)x);
    Py_DECREF((PyObject *)y);

    zz_err ret = zz_bin(n, k, &res->z);

    if (ret) {
        /* LCOV_EXCL_START */
        if (ret == ZZ_BUF) {
            goto overflow;
        }
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

    uint64_t n, k;

    if (zz_get(&x->z, &n) || zz_get(&y->z, &k)) {
overflow:
        PyErr_Format(PyExc_OverflowError,
                     "perm() arguments should not exceed %ld",
                     ULONG_MAX);
        goto err;
    }
    Py_DECREF((PyObject *)x);
    Py_DECREF((PyObject *)y);
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

    zz_err ret = zz_fac((zz_digit_t)n, &res->z);

    if (ret || zz_fac((zz_digit_t)(n-k), &den->z)
        || zz_quo(&res->z, &den->z, &res->z))
    {
        /* LCOV_EXCL_START */
        Py_DECREF(den);
        if (ret == ZZ_BUF) {
            goto overflow;
        }
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
    zz_err ret = ZZ_OK;

    if (!exp || !man || (ret = zz_mpmath_normalize(prec, rnd, &negative,
                                                   &man->z, &exp->z, &bc)))
    {
        /* LCOV_EXCL_START */
        Py_XDECREF((PyObject *)man);
        Py_XDECREF((PyObject *)exp);
        if (ret == ZZ_BUF) {
            PyErr_SetString(PyExc_OverflowError,
                            "too many digits in integer");
            return NULL;
        }
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
    zz_err ret = ZZ_OK;

    if (!exp || (ret = zz_mpmath_normalize(prec, rnd, &negative,
                                           &man->z, &exp->z, &bc)))
    {
        /* LCOV_EXCL_START */
        Py_DECREF(man);
        Py_XDECREF((PyObject *)exp);
        if (ret == ZZ_BUF) {
            PyErr_SetString(PyExc_OverflowError,
                            "too many digits in integer");
            return NULL;
        }
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
     ("_mpmath_normalize($module, sign, man, exp, bc, prec, rnd, /)\n--\n\n"
      "Helper function for mpmath.")},
    {"_mpmath_create", (PyCFunction)gmp__mpmath_create, METH_FASTCALL,
     ("_mpmath_create($module, man, exp, prec=0, rnd='d', /)\n--\n\n"
      "Helper function for mpmath.")},
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

static PyModuleDef_Slot gmp_slots[] = {
    {Py_mod_exec, gmp_exec},
#if PY_VERSION_HEX >= 0x030C0000
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
#endif
#if PY_VERSION_HEX >= 0x030D0000
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, NULL}};

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
