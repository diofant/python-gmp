#include "pythoncapi_compat.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <setjmp.h>
#include <gmp.h>


static jmp_buf gmp_env;
#define GMP_TRACKER_SIZE_INCR 16
#define CHECK_NO_MEM_LEAK (setjmp(gmp_env) != 1)
static struct {
    size_t size;
    size_t alloc;
    void** ptrs;
} gmp_tracker;


static void *
gmp_allocate_function(size_t size)
{
    if (gmp_tracker.size >= gmp_tracker.alloc) {
        void** tmp = gmp_tracker.ptrs;

        gmp_tracker.alloc += GMP_TRACKER_SIZE_INCR;
        gmp_tracker.ptrs = realloc(tmp, gmp_tracker.alloc*sizeof(void*));
        if (!gmp_tracker.ptrs) {
            gmp_tracker.alloc -= GMP_TRACKER_SIZE_INCR;
            gmp_tracker.ptrs = tmp;
            goto err;
        }
    }

    void *ret = malloc(size);

    if (!ret) {
        goto err;
    }
    gmp_tracker.ptrs[gmp_tracker.size] = ret;
    gmp_tracker.size++;
    return ret;
err:
    for (size_t i = 0; i < gmp_tracker.size; i++) {
        if (gmp_tracker.ptrs[i]) {
            free(gmp_tracker.ptrs[i]);
            gmp_tracker.ptrs[i] = NULL;
        }
    }
    gmp_tracker.alloc = 0;
    gmp_tracker.size = 0;
    longjmp(gmp_env, 1);
}


static void
gmp_free_function(void *ptr, size_t size)
{
    for (size_t i = gmp_tracker.size - 1; i >= 0; i--) {
        if (gmp_tracker.ptrs[i] && gmp_tracker.ptrs[i] == ptr) {
            gmp_tracker.ptrs[i] = 0;
            if (i == gmp_tracker.size - 1) {
                gmp_tracker.size--;
            }
            break;
        }
    }
    free(ptr);
}



typedef struct _mpzobject {
    PyObject_HEAD
    uint8_t negative;
    mp_size_t size;
    mp_limb_t *digits;
} MPZ_Object;


PyTypeObject MPZ_Type;
#define MPZ_CheckExact(a) PyObject_TypeCheck((a), &MPZ_Type)

static void
MPZ_normalize(MPZ_Object *a)
{
    while (a->size && a->digits[a->size - 1] == 0) {
        a->size--;
    }
    if (!a->size) {
        a->negative = 0;
    }
}


static MPZ_Object *
MPZ_new(mp_size_t size, uint8_t negative)
{
    MPZ_Object *res = PyObject_New(MPZ_Object, &MPZ_Type);

    if (!res) {
        return NULL;
    }
    res->negative = negative;
    res->size = size;
    res->digits = PyMem_New(mp_limb_t, size);
    if (!res->digits) {
        return (MPZ_Object*)PyErr_NoMemory();
    }
    return res;
}


static MPZ_Object *
MPZ_FromDigitSign(mp_limb_t digit, uint8_t negative)
{
    MPZ_Object *res = MPZ_new(1, negative);

    if (!res) {
        return NULL;
    }
    res->digits[0] = digit;
    MPZ_normalize(res);
    return res;
}


static PyObject *
MPZ_to_str(MPZ_Object *self, int base, int repr, int auto_prefix)
{
    if (base < 2 || base > 62) {
        PyErr_SetString(PyExc_ValueError,
                        "base must be in the interval [2, 62]");
        return NULL;
    }
    if (auto_prefix) {
        repr = 0;
    }

    Py_ssize_t len = mpn_sizeinbase(self->digits, self->size, base);
    Py_ssize_t prefix = repr ? 4 : 0;

    if (auto_prefix && (base == 2 || base == 8 || base == 16)) {
        auto_prefix = 2;
    }
    else {
        auto_prefix = 0;
    }

    unsigned char *buf = PyMem_Malloc(len + auto_prefix + prefix + repr + self->negative);

    if (!buf) {
        return PyErr_NoMemory();
    }
    if (prefix) {
        strcpy((char*)buf, "mpz(");
    }
    if (self->negative) {
        buf[prefix] = '-';
    }
    repr += prefix;
    prefix += self->negative;
    if (auto_prefix) {
        if (base == 2) {
            memcpy(buf + prefix, "0b", 2);
        }
        else if (base == 8) {
            memcpy(buf + prefix, "0o", 2);
        }
        else {
            memcpy(buf + prefix, "0x", 2);
        }
    }

    const char *num_to_text = (base > 36 ?
                               ("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                "abcdefghijklmnopqrstuvwxyz") :
                               "0123456789abcdefghijklmnopqrstuvwxyz");

    if (CHECK_NO_MEM_LEAK) {
        len -= (mpn_get_str(buf + auto_prefix + prefix, base,
                            self->digits, self->size) != (size_t)len);
    }
    else {
        PyMem_Free(buf);
        return PyErr_NoMemory();
    }
    for (mp_size_t i = prefix + auto_prefix; i < len + prefix + auto_prefix; i++)
    {
        buf[i] = num_to_text[buf[i]];
    }
    if (repr) {
        buf[prefix + len] = ')';
    }

    PyObject *res = PyUnicode_FromStringAndSize((char*)buf,
                                                len + repr + self->negative + auto_prefix);

    PyMem_Free(buf);
    return res;
}


/* Table of digit values for 8-bit string->mpz conversion.
   Note that when converting a base B string, a char c is a legitimate
   base B digit iff gmp_digit_value_tab[c] < B. */
const unsigned char gmp_digit_value_tab[] =
{
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1,-1,-1,-1,-1,-1,
  -1,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,
  25,26,27,28,29,30,31,32,33,34,35,-1,-1,-1,-1,-1,
  -1,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,
  25,26,27,28,29,30,31,32,33,34,35,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1,-1,-1,-1,-1,-1,
  -1,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,
  25,26,27,28,29,30,31,32,33,34,35,-1,-1,-1,-1,-1,
  -1,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,
  51,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};


static MPZ_Object *
MPZ_from_str(PyObject *s, int base)
{
    if (base != 0 && (base < 2 || base > 62)) {
        PyErr_SetString(PyExc_ValueError,
                        "base must be 0 or in the interval [2, 62]");
        return NULL;
    }

    Py_ssize_t len;
    const char *str = PyUnicode_AsUTF8AndSize(s, &len);

    if (!str) {
        return NULL;
    }

    unsigned char *buf = PyMem_Malloc(len), *p = buf;

    if (!buf) {
        return (MPZ_Object*)PyErr_NoMemory();
    }
    memcpy(buf, str, len);

    int8_t negative = (buf[0] == '-');

    p += negative;
    len -= negative;
    if (p[0] == '0' && len > 2 && p[1] != '\0') {
        if (base == 0) {
            if (tolower(p[1]) == 'b') {
                base = 2;
            }
            else if (tolower(p[1]) == 'o') {
                base = 8;
            }
            else if (tolower(p[1]) == 'x') {
                base = 16;
            }
            else {
                PyErr_SetString(PyExc_ValueError,
                                "XXX: leading 0 for base=0");
                return NULL;
            }
        }
        if ((tolower(p[1]) == 'b' && base == 2)
            || (tolower(p[1]) == 'o' && base == 8)
            || (tolower(p[1]) == 'x' && base == 16))
        {
            p += 2;
            len -= 2;
        }
    }
    if (base == 0) {
        base = 10;
    }

    const unsigned char *digit_value = gmp_digit_value_tab;

    if (base > 36) {
        digit_value += 208;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        p[i] = digit_value[p[i]];
        if (p[i] >= base) {
            PyErr_Format(PyExc_ValueError,
                         "invalid literal for mpz() with base %d: %.200R",
                         base, s);
            return NULL;
        }
    }

    MPZ_Object *res = MPZ_new(1 + len/2, negative);

    if (CHECK_NO_MEM_LEAK) {
        res->size = mpn_set_str(res->digits, p, len, base);
    }
    else {
        Py_DECREF(res);
        PyMem_Free(buf);
        return (MPZ_Object*)PyErr_NoMemory();
    }
    PyMem_Free(buf);

    mp_limb_t *tmp = res->digits;

    res->digits = PyMem_Resize(tmp, mp_limb_t, res->size);
    if (!res->digits) {
        res->digits = tmp;
        Py_DECREF(res);
        return (MPZ_Object*)PyErr_NoMemory();
    }
    MPZ_normalize(res);
    return res;
}

static PyObject *
plus(MPZ_Object *a)
{
    if (!a->size) {
        return (PyObject*)MPZ_FromDigitSign(0, 0);
    }

    MPZ_Object *res = MPZ_new(a->size, a->negative);

    mpn_copyi(res->digits, a->digits, a->size);
    return (PyObject*)res;
}


static PyObject *
minus(MPZ_Object *a)
{
    PyObject *res = plus(a);

    if (!res) {
        return NULL;
    }
    if (a->size) {
        ((MPZ_Object*)res)->negative = !a->negative;
    }
    return res;
}


static PyObject *
absolute(MPZ_Object *a)
{
    PyObject *res = plus(a);

    if (!res) {
        return NULL;
    }
    ((MPZ_Object*)res)->negative = 0;
    return res;
}


static PyObject *
to_int(MPZ_Object *self)
{
    PyObject *str = MPZ_to_str(self, 16, 0, 0);

    if (!str) {
        return NULL;
    }

    PyObject *res = PyLong_FromUnicodeObject(str, 16);

    Py_DECREF(str);
    return res;
}


static PyObject *
to_float(MPZ_Object *self)
{
    __mpz_struct tmp;

    tmp._mp_d = self->digits;
    tmp._mp_size = (self->negative ? -1 : 1)*self->size;

    double d = mpz_get_d(&tmp);

    if (isinf(d)) {
        PyErr_SetString(PyExc_OverflowError,
                        "integer too large to convert to float");
        return NULL;
    }
    return PyFloat_FromDouble(d);
}


static MPZ_Object *
from_int(PyObject *a)
{
    PyObject *str = PyNumber_ToBase(a, 16);

    if (!str) {
        return NULL;
    }

    MPZ_Object *res = MPZ_from_str(str, 16);

    if (!res) {
        return NULL;
    }
    Py_DECREF(str);
    return res;
}


static int
to_bool(MPZ_Object *a)
{
    return a->size != 0;
}


#define SWAP(T, a, b)  \
    do {               \
        T tmp = a;     \
        a = b;         \
        b = tmp;       \
    } while (0);


static PyObject*
MPZ_add(MPZ_Object *u, MPZ_Object *v, int subtract)
{
    MPZ_Object *res;
    uint8_t negu = u->negative, negv = v->negative;

    if (subtract) {
        negv = !negv;
    }
    if (u->size < v->size) {
        SWAP(MPZ_Object*, u, v);
        SWAP(uint8_t, negu, negv);
    }
    if (negu == negv) {
        res = MPZ_new(Py_MAX(u->size, v->size) + 1, negu);
        if (!res) {
            return NULL;
        }
        res->digits[res->size - 1] = mpn_add(res->digits,
                                             u->digits, u->size,
                                             v->digits, v->size);
    }
    else {
        if (u->size > v->size || mpn_cmp(u->digits, v->digits,
                                         u->size) >= 0)
        {
            res = MPZ_new(Py_MAX(u->size, v->size), negu);
            if (!res) {
                return NULL;
            }
            mpn_sub(res->digits, u->digits, u->size,
                    v->digits, v->size);
        }
        else {
            res = MPZ_new(Py_MAX(u->size, v->size), negv);
            if (!res) {
                return NULL;
            }
            mpn_sub_n(res->digits, v->digits, u->digits, u->size);
        }
    }
    MPZ_normalize(res);
    return (PyObject*)res;
}


#define CHECK_OP(u, a)              \
    static MPZ_Object *u;           \
    if (MPZ_CheckExact(a)) {        \
        u = (MPZ_Object*)a;         \
        Py_INCREF(u);               \
    }                               \
    else if (PyLong_Check(a)) {     \
        u = from_int(a);            \
        if (!u) {                   \
            goto end;               \
        }                           \
    }                               \
    else {                          \
        Py_RETURN_NOTIMPLEMENTED;   \
    }


static PyObject *
add(PyObject* a, PyObject *b)
{
    PyObject *res = NULL;

    CHECK_OP(u, a);
    CHECK_OP(v, b);

    res = MPZ_add(u, v, 0);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return res;
}


static PyObject *
sub(PyObject* a, PyObject *b)
{
    PyObject *res = NULL;

    CHECK_OP(u, a);
    CHECK_OP(v, b);

    res = MPZ_add(u, v, 1);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return res;
}


static PyObject *
MPZ_mul(MPZ_Object *v, MPZ_Object *u)
{
    if (!u->size || !v->size) {
        return (PyObject*)MPZ_FromDigitSign(0, 0);
    }

    MPZ_Object *res = MPZ_new(u->size + v->size, u->negative != v->negative);

    if (!res) {
        return NULL;
    }
    if (u->size < v->size) {
        SWAP(MPZ_Object*, u, v);
    }
    if (u == v) {
        if (CHECK_NO_MEM_LEAK) {
            mpn_sqr(res->digits, u->digits, u->size);
        }
        else {
            Py_DECREF(res);
            return PyErr_NoMemory();
        }
    }
    else {
        if (CHECK_NO_MEM_LEAK) {
            mpn_mul(res->digits, u->digits, u->size,
                    v->digits, v->size);
        }
        else {
            Py_DECREF(res);
            return PyErr_NoMemory();
        }
    }
    MPZ_normalize(res);
    return (PyObject*)res;
}


static PyObject *
mul(PyObject* a, PyObject *b)
{
    PyObject *res = NULL;

    CHECK_OP(u, a);
    CHECK_OP(v, b);

    res = MPZ_mul(u, v);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return (PyObject*)res;
}


static int
MPZ_DivMod(MPZ_Object *a, MPZ_Object *b, MPZ_Object **q, MPZ_Object **r)
{
    if (!b->size) {
        PyErr_SetString(PyExc_ZeroDivisionError, "division by zero");
        return -1;
    }
    if (!a->size) {
        *q = MPZ_FromDigitSign(0, 0);
        *r = MPZ_FromDigitSign(0, 0);
    }
    else if (a->size < b->size) {
        if (a->negative != b->negative) {
            *q = MPZ_FromDigitSign(1, 1);
            *r = (MPZ_Object*)MPZ_add(a, b, 0);
        }
        else {
            *q = MPZ_FromDigitSign(0, 0);
            *r = (MPZ_Object*)plus(a);
        }
    }
    else {
        *q = MPZ_new(a->size - b->size + 1, a->negative != b->negative);
        if (!*q) {
            return -1;
        }
        *r = MPZ_new(b->size, b->negative);
        if (!*r) {
            Py_DECREF(*q);
            return -1;
        }
        if (CHECK_NO_MEM_LEAK) {
            mpn_tdiv_qr((*q)->digits, (*r)->digits, 0,
                        a->digits, a->size,
                        b->digits, b->size);
        }
        else {
            Py_DECREF(*q);
            Py_DECREF(*r);
            return -1;
        }
        if ((*q)->negative) {
            if (a->digits[a->size - 1] == GMP_NUMB_MAX
                && b->digits[b->size - 1] == 1)
            {
                (*q)->size++;

                mp_limb_t *tmp = (*q)->digits;

                (*q)->digits = PyMem_Resize(tmp, mp_limb_t, (*q)->size);
                if (!(*q)->digits) {
                    (*q)->digits = tmp;
                    Py_DECREF(*q);
                    Py_DECREF(*r);
                    return -1;
                }
                (*q)->digits[(*q)->size - 1] = 0;
            }
            for (mp_size_t i = 0; i < b->size; i++) {
                if ((*r)->digits[i]) {
                    mpn_sub_n((*r)->digits, b->digits, (*r)->digits, b->size);
                    if (mpn_add_1((*q)->digits, (*q)->digits,
                                  (*q)->size - 1, 1))
                    {
                        (*q)->digits[(*q)->size - 2] = 1;
                    }
                    break;
                }
            }
        }
        MPZ_normalize(*q);
        MPZ_normalize(*r);
        return 0;
    }
    if (!*q || !*r) {
        if (*q) {
            Py_DECREF(*q);
        }
        if (*r) {
            Py_DECREF(*r);
        }
        return -1;
    }
    return 0;
}


static PyObject *
divmod(PyObject *a, PyObject *b)
{
    PyObject *res = PyTuple_New(2);

    if (!res) {
        return NULL;
    }
    CHECK_OP(u, a);
    CHECK_OP(v, b);

    MPZ_Object *q, *r;

    if (MPZ_DivMod(u, v, &q, &r) == -1) {
        goto end;
    }
    PyTuple_SET_ITEM(res, 0, (PyObject*)q);
    PyTuple_SET_ITEM(res, 1, (PyObject*)r);
    return res;
end:
    Py_DECREF(res);
    Py_XDECREF(u);
    Py_XDECREF(v);
    return NULL;
}


static PyObject *
floordiv(PyObject *a, PyObject *b)
{
    MPZ_Object *q, *r;

    CHECK_OP(u, a);
    CHECK_OP(v, b);
    if (MPZ_DivMod(u, v, &q, &r) == -1) {
        goto end;
    }
    Py_DECREF(r);
    return (PyObject*)q;
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return NULL;
}


static PyObject *
truediv(PyObject *a, PyObject *b)
{
    PyErr_SetString(PyExc_NotImplementedError, "mpz.__truediv__");
    return NULL;
}


static PyObject *
rem(PyObject *a, PyObject *b)
{
    MPZ_Object *q, *r;

    CHECK_OP(u, a);
    CHECK_OP(v, b);
    if (MPZ_DivMod(u, v, &q, &r) == -1) {
        return NULL;
    }
    Py_DECREF(q);
    return (PyObject*)r;
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return NULL;
}


static PyObject *
invert(MPZ_Object *self)
{
    if (self->negative) {
        MPZ_Object *res = MPZ_new(self->size, 0);

        if (!res) {
            return NULL;
        }
        mpn_sub_1(res->digits, self->digits, self->size, 1);
        res->size -= res->digits[self->size - 1] == 0;
        return (PyObject*)res;
    }
    else if (!self->size) {
        return (PyObject*)MPZ_FromDigitSign(1, 1);
    }
    else {
        MPZ_Object *res = MPZ_new(self->size + 1, 1);

        if (!res) {
            return NULL;
        }
        res->digits[self->size] = mpn_add_1(res->digits, self->digits,
                                            self->size, 1);
        self->size += res->digits[self->size];
        MPZ_normalize(res);
        return (PyObject*)res;
    }
}


static MPZ_Object *
MPZ_lshift(MPZ_Object *u, MPZ_Object *v)
{
    if (v->negative) {
        PyErr_SetString(PyExc_ValueError, "negative shift count");
        return NULL;
    }
    if (!u->size) {
        return MPZ_FromDigitSign(0, 0);
    }
    if (!v->size) {
        return (MPZ_Object*)plus(u);
    }
    if (v->size > 1) {
        PyErr_SetString(PyExc_OverflowError,
                        "too many digits in integer");
        return NULL;
    }

    mp_limb_t lshift = v->digits[0];
    mp_size_t whole = lshift/GMP_NUMB_BITS;
    mp_size_t size = u->size + whole;

    lshift %= GMP_NUMB_BITS;
    if (lshift) {
        size++;
    }
    if (u->size == 1 && !whole) {
        mp_limb_t t = u->digits[0] << lshift;

        if (t >> lshift == u->digits[0]) {
            return MPZ_FromDigitSign(t, u->negative);
        }
    }

    MPZ_Object *res = MPZ_new(size, u->negative);

    if (!res) {
        return NULL;
    }
    if (whole) {
        mpn_zero(res->digits, whole);
    }

    mp_limb_t carry = mpn_lshift(res->digits + whole, u->digits,
                                 u->size, lshift);

    if (lshift) {
        res->digits[size - 1] = carry;
    }
    MPZ_normalize(res);
    return res;
}


static PyObject *
lshift(PyObject *a, PyObject *b)
{
    PyObject *res = NULL;

    CHECK_OP(u, a);
    CHECK_OP(v, b);

    res = (PyObject*)MPZ_lshift(u, v);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return (PyObject*)res;
}


static MPZ_Object *
MPZ_rshift(MPZ_Object *u, MPZ_Object *v)
{
    if (v->negative) {
        PyErr_SetString(PyExc_ValueError, "negative shift count");
        return NULL;
    }
    if (!u->size) {
        return MPZ_FromDigitSign(0, 0);
    }
    if (!v->size) {
        return (MPZ_Object*)plus(u);
    }
    if (v->size > 1) {
        if (u->negative) {
            return MPZ_FromDigitSign(1, 1);
        }
        else {
            return MPZ_FromDigitSign(0, 0);
        }
    }

    mp_size_t whole = v->digits[0] / GMP_NUMB_BITS;
    mp_size_t rshift = v->digits[0] % GMP_NUMB_BITS;
    mp_size_t size = u->size;

    if (whole >= size) {
        return MPZ_FromDigitSign(u->negative, u->negative);
    }
    size -= whole;

    MPZ_Object *res = MPZ_new(size + 1, u->negative);

    if (!res) {
        return NULL;
    }
    res->digits[size] = 0;

    int carry = 0;

    if (rshift) {
        if (mpn_rshift(res->digits, u->digits + whole, size, rshift)) {
            carry = u->negative;
        }
    }
    else {
        mpn_copyi(res->digits, u->digits + whole, size);
    }
    if (carry) {
        if (mpn_add_1(res->digits, res->digits, size, 1)) {
            res->digits[size] = 1;
        }
    }
    MPZ_normalize(res);
    return res;
}


static PyObject *
rshift(PyObject *a, PyObject *b)
{
    PyObject *res = NULL;

    CHECK_OP(u, a);
    CHECK_OP(v, b);

    res = (PyObject*)MPZ_rshift(u, v);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return (PyObject*)res;
}


static MPZ_Object *
MPZ_and(MPZ_Object *u, MPZ_Object *v)
{
    if (!u->size || !v->size) {
        return MPZ_FromDigitSign(0, 0);
    }

    MPZ_Object *res;

    if (u->negative || v->negative) {
        if (u->negative) {
            u = (MPZ_Object*)invert(u);
            if (!u) {
               return NULL;
            }
            u->negative = 1;
        }
        else {
            Py_INCREF(u);
        }
        if (v->negative) {
            v = (MPZ_Object*)invert(v);
            if (!v) {
               Py_DECREF(u);
               return NULL;
            }
            v->negative = 1;
        }
        else {
            Py_INCREF(v);
        }
        if (u->size < v->size) {
            SWAP(MPZ_Object*, u, v);
        }
        if (u->negative & v->negative) {
            if (!u->size) {
                Py_DECREF(u);
                Py_DECREF(v);
                return MPZ_FromDigitSign(1, 1);
            }
            res = MPZ_new(u->size + 1, 1);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            if (v->size) {
                mpn_ior_n(res->digits, u->digits, v->digits, v->size);
            }
            res->digits[u->size] = mpn_add_1(res->digits, res->digits,
                                             u->size, 1);
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else if (u->negative) {
            res = MPZ_new(v->size, 0);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            mpn_andn_n(res->digits, v->digits, u->digits, v->size);
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else {
            res = MPZ_new(u->size, 0);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            if (v->size) {
                mpn_andn_n(res->digits, u->digits, v->digits, v->size);
            }
            mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
    }
    if (u->size < v->size) {
        SWAP(MPZ_Object*, u, v);
    }
    res = MPZ_new(v->size, 0);
    if (!res) {
        return NULL;
    }
    mpn_and_n(res->digits, u->digits, v->digits, v->size);
    MPZ_normalize(res);
    return res;
}


static PyObject *
bitwise_and(PyObject *a, PyObject *b)
{
    PyObject *res = NULL;

    CHECK_OP(u, a);
    CHECK_OP(v, b);

    res = (PyObject*)MPZ_and(u, v);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return (PyObject*)res;
}


static MPZ_Object *
MPZ_or(MPZ_Object *u, MPZ_Object *v)
{
    if (!u->size) {
        return (MPZ_Object*)plus(v);
    }
    if (!v->size) {
        return (MPZ_Object*)plus(u);
    }

    MPZ_Object *res;

    if (u->negative || v->negative) {
        if (u->negative) {
            u = (MPZ_Object*)invert(u);
            if (!u) {
               return NULL;
            }
            u->negative = 1;
        }
        else {
            Py_INCREF(u);
        }
        if (v->negative) {
            v = (MPZ_Object*)invert(v);
            if (!v) {
               Py_DECREF(u);
               return NULL;
            }
            v->negative = 1;
        }
        else {
            Py_INCREF(v);
        }
        if (u->size < v->size) {
            SWAP(MPZ_Object*, u, v);
        }
        if (u->negative & v->negative) {
            if (!v->size) {
                Py_DECREF(u);
                Py_DECREF(v);
                return MPZ_FromDigitSign(1, 1);
            }
            res = MPZ_new(v->size + 1, 1);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            mpn_and_n(res->digits, u->digits, v->digits, v->size);
            res->digits[v->size] = mpn_add_1(res->digits, res->digits,
                                             v->size, 1);
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else if (u->negative) {
            res = MPZ_new(u->size + 1, 1);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            mpn_andn_n(res->digits, u->digits, v->digits, v->size);
            res->digits[u->size] = mpn_add_1(res->digits, res->digits,
                                             u->size, 1);
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else {
            res = MPZ_new(v->size + 1, 1);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            if (v->size) {
                mpn_andn_n(res->digits, v->digits, u->digits, v->size);
                res->digits[v->size] = mpn_add_1(res->digits, res->digits,
                                                 v->size, 1);
                MPZ_normalize(res);
            }
            else {
                res->digits[0] = 1;
            }
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
    }
    if (u->size < v->size) {
        SWAP(MPZ_Object*, u, v);
    }
    res = MPZ_new(u->size, 0);
    if (!res) {
        return NULL;
    }
    mpn_ior_n(res->digits, u->digits, v->digits, v->size);
    if (u->size != v->size) {
        mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                  u->size - v->size);
    }
    else {
        MPZ_normalize(res);
    }
    return res;
}


static PyObject *
bitwise_or(PyObject *a, PyObject *b)
{
    PyObject *res = NULL;

    CHECK_OP(u, a);
    CHECK_OP(v, b);

    res = (PyObject*)MPZ_or(u, v);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return (PyObject*)res;
}


static MPZ_Object *
MPZ_xor(MPZ_Object *u, MPZ_Object *v)
{
    if (!u->size) {
        return (MPZ_Object*)plus(v);
    }
    if (!v->size) {
        return (MPZ_Object*)plus(u);
    }

    MPZ_Object *res;

    if (u->negative || v->negative) {
        if (u->negative) {
            u = (MPZ_Object*)invert(u);
            if (!u) {
               return NULL;
            }
            u->negative = 1;
        }
        else {
            Py_INCREF(u);
        }
        if (v->negative) {
            v = (MPZ_Object*)invert(v);
            if (!v) {
               Py_DECREF(u);
               return NULL;
            }
            v->negative = 1;
        }
        else {
            Py_INCREF(v);
        }
        if (u->size < v->size) {
            SWAP(MPZ_Object*, u, v);
        }
        if (u->negative & v->negative) {
            if (!u->size) {
                Py_DECREF(u);
                Py_DECREF(v);
                return MPZ_FromDigitSign(0, 0);
            }
            res = MPZ_new(u->size, 0);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            if (v->size) {
                mpn_xor_n(res->digits, u->digits, v->digits, v->size);
            }
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else if (u->negative) {
            res = MPZ_new(u->size + 1, 1);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            mpn_xor_n(res->digits, v->digits, u->digits, v->size);
            res->digits[u->size] = mpn_add_1(res->digits, res->digits,
                                             u->size, 1);
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else {
            res = MPZ_new(u->size + 1, 1);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            if (v->size) {
                mpn_xor_n(res->digits, u->digits, v->digits, v->size);
            }
            res->digits[u->size] = mpn_add_1(res->digits, res->digits,
                                             u->size, 1);
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
    }
    if (u->size < v->size) {
        SWAP(MPZ_Object*, u, v);
    }
    res = MPZ_new(u->size, 0);
    if (!res) {
        return NULL;
    }
    mpn_xor_n(res->digits, u->digits, v->digits, v->size);
    if (u->size != v->size) {
        mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                  u->size - v->size);
    }
    else {
        MPZ_normalize(res);
    }
    return res;
}


static PyObject *
bitwise_xor(PyObject *a, PyObject *b)
{
    PyObject *res = NULL;

    CHECK_OP(u, a);
    CHECK_OP(v, b);

    res = (PyObject*)MPZ_xor(u, v);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return (PyObject*)res;
}


static PyObject *
power(PyObject *a, PyObject *b, PyObject *m)
{
    MPZ_Object *res = NULL;

    CHECK_OP(u, a);
    CHECK_OP(v, b);
    if (Py_IsNone(m)) {
        if (!v->size) {
            res = MPZ_FromDigitSign(1, 0);
            goto end;
        }
        if (!u->size) {
            res = MPZ_FromDigitSign(0, 0);
            goto end;
        }
        if (v->negative) {
            PyErr_SetString(PyExc_NotImplementedError,
                            "mpz.__pow__: float arg");
            goto end;
        }
        if (u->size == 1 && u->digits[0] == 1) {
            if (u->negative) {
                res = MPZ_FromDigitSign(1, v->digits[0]%2);
                goto end;
            }
            else {
                res = MPZ_FromDigitSign(1, 0);
                goto end;
            }
        }
        if (v->size == 1) {
            res = MPZ_new(u->size*v->digits[0],
                          u->negative && v->digits[0]%2);
            if (!res) {
                goto end;
            }

            mp_limb_t *tmp = PyMem_New(mp_limb_t, res->size);

            if (!tmp) {
                Py_DECREF(res);
                Py_DECREF(u);
                Py_DECREF(v);
                return PyErr_NoMemory();
            }
            if (CHECK_NO_MEM_LEAK) {
                res->size = mpn_pow_1(res->digits, u->digits, u->size,
                                      v->digits[0], tmp);
            }
            else {
                PyMem_Free(tmp);
                Py_DECREF(res);
                return PyErr_NoMemory();
            }
            PyMem_Free(tmp);
            tmp = res->digits;
            res->digits = PyMem_Resize(tmp, mp_limb_t, res->size);
            if (!res->digits) {
                res->digits = tmp;
                Py_DECREF(res);
                Py_DECREF(u);
                Py_DECREF(v);
                return PyErr_NoMemory();
            }
            goto end;
        }
        Py_DECREF(u);
        Py_DECREF(v);
        return PyErr_NoMemory();
    }
    else {
        PyErr_SetString(PyExc_NotImplementedError,
                        "mpz.__pow__: ternary power");
    }
end:
    Py_DECREF(u);
    Py_DECREF(v);
    return (PyObject*)res;
}


static PyNumberMethods as_number = {
    .nb_add = add,
    .nb_subtract = sub,
    .nb_multiply = mul,
    .nb_divmod = divmod,
    .nb_floor_divide = floordiv,
    .nb_true_divide = truediv,
    .nb_remainder = rem,
    .nb_power = power,
    .nb_positive = (unaryfunc) plus,
    .nb_negative = (unaryfunc) minus,
    .nb_absolute = (unaryfunc) absolute,
    .nb_invert = (unaryfunc) invert,
    .nb_lshift = lshift,
    .nb_rshift = rshift,
    .nb_and = bitwise_and,
    .nb_or = bitwise_or,
    .nb_xor = bitwise_xor,
    .nb_int = (unaryfunc) to_int,
    .nb_float = (unaryfunc) to_float,
    .nb_index = (unaryfunc) to_int,
    .nb_bool = (inquiry) to_bool,
};


static PyObject *
repr(MPZ_Object *self)
{
    return MPZ_to_str(self, 10, 1, 0);
}


static PyObject *
str(MPZ_Object *self)
{
    return MPZ_to_str(self, 10, 0, 0);
}


static PyObject *
new(PyTypeObject *type, PyObject *args, PyObject *keywds)
{
    static char *kwlist[] = {"", "base", NULL};
    Py_ssize_t argc = PyTuple_GET_SIZE(args);
    int base = 10;
    PyObject *arg;

    if (argc == 0) {
        return (PyObject*)MPZ_FromDigitSign(0, 0);
    }
    if (argc == 1 && !keywds) {
        arg = PyTuple_GET_ITEM(args, 0);
        if (PyLong_Check(arg)) {
            return (PyObject*)from_int(arg);
        }
        if (MPZ_CheckExact(arg)) {
            return Py_NewRef(arg);
        }
        goto str;
    }
    if (!PyArg_ParseTupleAndKeywords(args, keywds, "O|i",
                                     kwlist, &arg, &base))
    {
        return NULL;
    }
str:
    if (PyUnicode_Check(arg)) {
        return (PyObject*)MPZ_from_str(arg, base);
    }
    PyErr_SetString(PyExc_TypeError,
                    "can't convert non-string with explicit base");
    return NULL;
}


static void
dealloc(MPZ_Object *self)
{
    PyMem_Free(self->digits);
    PyObject_Free(self);
}


static int
MPZ_Compare(MPZ_Object *a, MPZ_Object *b)
{
    if (a == b) {
        return 0;
    }

    int sign = a->negative ? -1 : 1;

    if (a->negative != b->negative) {
        return sign;
    }
    else if (a->size != b->size) {
        return (a->size < b->size) ? -sign : sign;
    }

    int r = mpn_cmp(a->digits, b->digits, a->size);

    return a->negative ? -r : r;
}


static PyObject *
richcompare(PyObject *a, PyObject *b, int op)
{
    CHECK_OP(u, a);
    CHECK_OP(v, b);

    int r = MPZ_Compare(u, v);

    Py_XDECREF(u);
    Py_XDECREF(v);
    switch(op) {
    case Py_LT:
        return PyBool_FromLong(r == -1);
    case Py_LE:
        return PyBool_FromLong(r != 1);
    case Py_GT:
        return PyBool_FromLong(r == 1);
    case Py_GE:
        return PyBool_FromLong(r != -1);
    case Py_EQ:
        return PyBool_FromLong(r == 0);
    case Py_NE:
        return PyBool_FromLong(r != 0);
    }
    Py_RETURN_NOTIMPLEMENTED;
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return NULL;
}


static Py_hash_t
hash(MPZ_Object *self)
{
    Py_hash_t r = mpn_mod_1(self->digits, self->size, _PyHASH_MODULUS);

    if (self->negative) {
        r = -r;
    }
    if (r == -1) {
        r = -2;
    }
    return r;
}


static PyObject *
get_copy(MPZ_Object *a, void *closure)
{
    return Py_NewRef(a);
}


static PyObject *
get_one(MPZ_Object *a, void *closure)
{
    return (PyObject*)MPZ_FromDigitSign(1, 0);
}


static PyObject *
get_zero(MPZ_Object *a, void *closure)
{
    return (PyObject*)MPZ_FromDigitSign(0, 0);
}


static PyGetSetDef getsetters[] = {
    {"numerator", (getter)get_copy, NULL,
     "the numerator of a rational number in lowest terms", NULL},
    {"denominator", (getter)get_one, NULL,
     "the denominator of a rational number in lowest terms", NULL},
    {"real", (getter)get_copy, NULL,
     "the real part of a complex number", NULL},
    {"imag", (getter)get_zero, NULL,
     "the imaginary part of a complex number", NULL},
    {NULL}  /* sentinel */
};


static PyObject *
bit_length(PyObject *a)
{
    MPZ_Object *self = (MPZ_Object*)a;
    mp_limb_t digit = mpn_sizeinbase(self->digits, self->size, 2);

    return (PyObject*)MPZ_FromDigitSign(self->size ? digit : 0, 0);
}


static PyObject *
bit_count(PyObject *a)
{
    MPZ_Object *self = (MPZ_Object*)a;
    mp_bitcnt_t count = self->size ? mpn_popcount(self->digits,
                                                  self->size) : 0;

    return (PyObject*)MPZ_FromDigitSign(count, 0);
}


static void
revstr(char* s, size_t l, size_t r)
{
    while (l < r) {
        SWAP(char, s[l], s[r]);
        l++;
        r--;
    }
}


static PyObject *
to_bytes(PyObject *self, PyObject *const *args, Py_ssize_t nargs,
         PyObject *kwnames)
{
    PyErr_SetString(PyExc_NotImplementedError, "to_bytes()");
    return NULL;
}


static PyObject *
from_bytes(PyTypeObject *type, PyObject *const *args,
           Py_ssize_t nargs, PyObject *kwnames)
{
    if (nargs > 2) {
        PyErr_SetString(PyExc_TypeError,
                        ("from_bytes() takes at most 2"
                         " positional arguments"));
        return NULL;
    }

    Py_ssize_t nkws = 0;
    int is_little = 0, is_signed = 0, argidx[3] = {-1, -1, -1};

    if (nargs >= 1) {
        argidx[0] = 0;
    }
    if (nargs == 2) {
        argidx[1] = 1;
    }
    if (kwnames) {
        nkws = PyTuple_GET_SIZE(kwnames);
    }
    if (nkws > 3) {
        PyErr_SetString(PyExc_TypeError,
                        "from_bytes() takes at most 3 keyword arguments");
        return NULL;
    }
    if (nkws + nargs < 1) {
        PyErr_SetString(PyExc_TypeError,
                        ("from_bytes() missing required argument"
                         " 'bytes' (pos 1)"));
        return NULL;
    }
    for (Py_ssize_t i = 0; i < nkws; i++) {
        const char *kwname = PyUnicode_AsUTF8(PyTuple_GET_ITEM(kwnames, i));

        if (strcmp(kwname, "bytes") == 0) {
            if (nargs == 0) {
                argidx[0] = (int)(nargs + i);
            }
            else {
                PyErr_SetString(PyExc_TypeError,
                                ("argument for from_bytes() given by"
                                 " name ('bytes') and position (1)"));
                return NULL;
            }
        }
        else if (strcmp(kwname, "byteorder") == 0) {
            if (nargs <= 1) {
                argidx[1] = (int)(nargs + i);
            }
            else {
                PyErr_SetString(PyExc_TypeError,
                                ("argument for from_bytes() given by"
                                 " name ('byteorder') and position (2)"));
                return NULL;
            }
        }
        else if (strcmp(kwname, "signed") == 0) {
            argidx[2] = (int)(nargs + i);
        }
        else {
            PyErr_SetString(PyExc_TypeError, ("got an invalid keyword "
                                              "argument for from_bytes()"));
            return NULL;
        }
    }
    if (argidx[1] >= 0) {
        PyObject *arg = args[argidx[1]];

        if (PyUnicode_Check(arg)) {
            const char* byteorder = PyUnicode_AsUTF8(arg);

            if (!byteorder) {
                return NULL;
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

    PyObject *bytes = PyObject_Bytes(args[argidx[0]]);
    char *buffer;
    Py_ssize_t length;

    if (bytes == NULL) {
        return NULL;
    }
    if (PyBytes_AsStringAndSize(bytes, &buffer, &length) == -1) {
        return NULL;
    }
    if (!length) {
        Py_DECREF(bytes);
        return (PyObject*)MPZ_FromDigitSign(0, 0);
    }

    MPZ_Object *res = MPZ_new(1 + length/2, 0);

    if (!res) {
        Py_DECREF(bytes);
        return NULL;
    }
    if (is_little) {
        char *tmp = PyMem_Malloc(length);

        if (!tmp) {
            Py_DECREF(bytes);
            return PyErr_NoMemory();
        }
        memcpy(tmp, buffer, length);
        buffer = tmp;
        revstr(buffer, 0, length - 1);
    }
    if (CHECK_NO_MEM_LEAK) {
        res->size = mpn_set_str(res->digits, (unsigned char*)buffer,
                                length, 256);
    }
    else {
        Py_DECREF(res);
        PyMem_Free(bytes);
        if (is_little) {
            PyMem_Free(buffer);
        }
        return PyErr_NoMemory();
    }
    Py_DECREF(bytes);
    if (is_little) {
        PyMem_Free(buffer);
    }

    mp_limb_t *tmp = res->digits;

    res->digits = PyMem_Resize(tmp, mp_limb_t, res->size);
    if (!res->digits) {
        res->digits = tmp;
        Py_DECREF(res);
        return PyErr_NoMemory();
    }
    MPZ_normalize(res);
    if (is_signed && mpn_sizeinbase(res->digits, res->size,
                                    2) == 8*(size_t)length)
    {
        if (res->size > 1) {
            if (mpn_sub_1(res->digits, res->digits, res->size, 1)) {
                res->digits[res->size - 1] -= 1;
            }
            mpn_com(res->digits, res->digits, res->size - 1);
        }
        else {
            res->digits[res->size - 1] -= 1;
        }
        res->digits[res->size - 1] = ~res->digits[res->size - 1];

        mp_size_t shift = GMP_LIMB_BITS*res->size - 8*length;

        res->digits[res->size - 1] <<= shift;
        res->digits[res->size - 1] >>= shift;
        res->negative = 1;
        MPZ_normalize(res);
    }
    return (PyObject*)res;
}


static PyObject *
as_integer_ratio(PyObject *a)
{
    PyObject *one = (PyObject*)MPZ_FromDigitSign(1, 0);

    if (!one) {
        return NULL;
    }

    PyObject *clone = Py_NewRef(a);
    PyObject *ratio_tuple = PyTuple_Pack(2, clone, one);

    Py_DECREF(clone);
    Py_DECREF(one);
    return ratio_tuple;
}

static PyObject *
__round__(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    PyErr_SetString(PyExc_NotImplementedError, "mpz.__round__");
    return NULL;
}


static PyObject *
__getnewargs__(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    return Py_BuildValue("(Ni)", MPZ_to_str((MPZ_Object*)self, 16, 0, 0), 16);
}

static PyObject *
__format__(PyObject *self, PyObject *format_spec)
{
    PyErr_SetString(PyExc_NotImplementedError, "mpz.__format__");
    return NULL;
}


static PyObject *
__sizeof__(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    return PyLong_FromSize_t(sizeof(MPZ_Object) +
                             ((MPZ_Object*)self)->size*sizeof(mp_limb_t));
}


static PyObject *
is_integer(PyObject *a)
{
    Py_RETURN_TRUE;
}


static PyObject *
digits(PyObject *self, PyObject *const *args, Py_ssize_t nargs,
       PyObject *kwnames)
{
    if (nargs > 1) {
        PyErr_SetString(PyExc_TypeError,
                        "digits() takes at most one positional argument");
        return NULL;
    }

    Py_ssize_t nkws = 0;
    int base = 10, prefix = 0, argidx[2] = {-1, -1};

    if (nargs >= 1) {
        argidx[0] = 0;
    }
    if (nargs == 2) {
        argidx[1] = 1;
    }
    if (kwnames) {
        nkws = PyTuple_GET_SIZE(kwnames);
    }
    if (nkws > 2) {
        PyErr_SetString(PyExc_TypeError,
                        "digits() takes at most two keyword argument");
        return NULL;
    }
    for (Py_ssize_t i = 0; i < nkws; i++) {
        const char *kwname = PyUnicode_AsUTF8(PyTuple_GET_ITEM(kwnames, i));

        if (strcmp(kwname, "base") == 0) {
            if (nargs == 0) {
                argidx[0] = (int)(nargs + i);
            }
            else {
                PyErr_SetString(PyExc_TypeError,
                                "argument for digits() given by name ('base') and position (1)");
                return NULL;
            }
        }
        else if (strcmp(kwname, "prefix") == 0) {
            if (nargs <= 1) {
                argidx[1] = (int)(nargs + i);
            }
            else {
                PyErr_SetString(PyExc_TypeError,
                                "argument for digits() given by name ('prefix') and position (2)");
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_TypeError,
                            "got an invalid keyword argument for digits()");
            return NULL;
        }
    }
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
    if (argidx[1] != -1) {
        prefix = PyObject_IsTrue(args[argidx[1]]);
    }
    return MPZ_to_str((MPZ_Object*)self, base, 0, prefix);
}

PyDoc_STRVAR(to_bytes__doc__,
"to_bytes($self, /, length=1, byteorder=\'big\', *, signed=False)\n--\n\n"
"Return an array of bytes representing an integer.\n\n"
"  length\n"
"    Length of bytes object to use.  An OverflowError is raised if the\n"
"    integer is not representable with the given number of bytes.  Default\n"
"    is length 1.\n"
"  byteorder\n"
"    The byte order used to represent the integer.  If byteorder is \'big\',\n"
"    the most significant byte is at the beginning of the byte array.  If\n"
"    byteorder is \'little\', the most significant byte is at the end of the\n"
"    byte array.  To request the native byte order of the host system, use\n"
"    sys.byteorder as the byte order value.  Default is to use \'big\'.\n"
"  signed\n"
"    Determines whether two\'s complement is used to represent the integer.\n"
"    If signed is False and a negative integer is given, an OverflowError\n"
"    is raised.");
PyDoc_STRVAR(from_bytes__doc__,
"from_bytes($type, /, bytes, byteorder=\'big\', *, signed=False)\n--\n\n"
"Return the integer represented by the given array of bytes.\n\n"
"  bytes\n"
"    Holds the array of bytes to convert.  The argument must either\n"
"    support the buffer protocol or be an iterable object producing bytes.\n"
"    Bytes and bytearray are examples of built-in objects that support the\n"
"    buffer protocol.\n"
"  byteorder\n"
"    The byte order used to represent the integer.  If byteorder is \'big\',\n"
"    the most significant byte is at the beginning of the byte array.  If\n"
"    byteorder is \'little\', the most significant byte is at the end of the\n"
"    byte array.  To request the native byte order of the host system, use\n"
"    sys.byteorder as the byte order value.  Default is to use \'big\'.\n"
"  signed\n"
"    Indicates whether two\'s complement is used to represent the integer.");


static PyMethodDef methods[] = {
    {"conjugate", (PyCFunction)plus, METH_NOARGS,
     "Returns self, the complex conjugate of any int."},
    {"bit_length", (PyCFunction)bit_length, METH_NOARGS,
     "Number of bits necessary to represent self in binary."},
    {"bit_count", (PyCFunction)bit_count, METH_NOARGS,
     ("Number of ones in the binary representation of the "
      "absolute value of self.")},
    {"to_bytes", (PyCFunction)to_bytes, METH_FASTCALL|METH_KEYWORDS,
     to_bytes__doc__},
    {"from_bytes", (PyCFunction)from_bytes,
     METH_FASTCALL|METH_KEYWORDS|METH_CLASS,
     from_bytes__doc__},
    {"as_integer_ratio", (PyCFunction)as_integer_ratio, METH_NOARGS,
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
      "Rounding with an ndigits argument also returns an integer.")
    },
    {"__getnewargs__", (PyCFunction)__getnewargs__, METH_NOARGS, NULL},
    {"__format__", (PyCFunction)__format__, METH_O,
     ("__format__($self, format_spec, /)\n--\n\n"
      "Convert to a string according to format_spec.")},
    {"__sizeof__", (PyCFunction)__sizeof__, METH_NOARGS,
     "Returns size in memory, in bytes."},
    {"is_integer", (PyCFunction)is_integer, METH_NOARGS,
     ("Returns True.  Exists for duck type compatibility "
      "with float.is_integer.")},
    {"digits", (PyCFunction)digits, METH_FASTCALL|METH_KEYWORDS,
     ("digits($self, base=10)\n--\n\n"
      "Return Python string representing self in the given base.\n\n"
      "Values for base can range between 2 to 62.")},
    {NULL}  /* sentinel */
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
    .tp_dealloc = (destructor) dealloc,
    .tp_repr = (reprfunc) repr,
    .tp_str = (reprfunc) str,
    .tp_as_number = &as_number,
    .tp_richcompare = richcompare,
    .tp_hash = (hashfunc) hash,
    .tp_getset = getsetters,
    .tp_methods = methods,
    .tp_doc = mpz_doc,
};


static PyObject *
gmp_gcd(PyObject *self, PyObject * const *args, Py_ssize_t nargs)
{
    if (!nargs) {
        return (PyObject*)MPZ_FromDigitSign(0, 0);
    }

    mp_bitcnt_t nzeros_res = 0;
    MPZ_Object *res, *arg, *tmp;

    if (MPZ_CheckExact(args[0])) {
        arg = (MPZ_Object*)args[0];
        Py_INCREF(arg);
    }
    else if (PyLong_Check(args[0])) {
        arg = from_int(args[0]);
        if (!arg) {
            return NULL;
        }
    }
    else {
        PyErr_SetString(PyExc_TypeError,
                        "gcd() arguments must be integers");
        return NULL;
    }
    res = (MPZ_Object*)absolute(arg);
    Py_DECREF(arg);
    if (!res) {
        return NULL;
    }
    for (Py_ssize_t i = 1; i < nargs; i++) {
        if (res->size != 1 || res->negative || res->digits[0] != 1)
        {
            if (MPZ_CheckExact(args[i])) {
                arg = (MPZ_Object*)absolute((MPZ_Object*)args[i]);
            }
            else if (PyLong_Check(args[i])) {
                tmp = from_int(args[i]);
                if (!tmp) {
                    Py_DECREF(res);
                    return NULL;
                }
                arg = (MPZ_Object*)absolute(tmp);
                if (!arg) {
                    Py_DECREF(tmp);
                    Py_DECREF(res);
                    return NULL;
                }
            }
            else {
                Py_DECREF(res);
                PyErr_SetString(PyExc_TypeError,
                                "gcd() arguments must be integers");
                return NULL;
            }
            if (!res->size) {
                Py_DECREF(res);
                res = (MPZ_Object*)absolute(arg);
                Py_DECREF(arg);
                continue;
            }
            nzeros_res = mpn_scan1(res->digits, 0);
            if (nzeros_res) {
                mpn_rshift(res->digits, res->digits,
                           res->size, nzeros_res);
            }
            if (!arg->size) {
                Py_DECREF(arg);
                continue;
            }
            nzeros_res = Py_MIN(nzeros_res, mpn_scan1(arg->digits, 0));
            if (nzeros_res) {
                mpn_rshift(arg->digits, arg->digits, arg->size, nzeros_res);
            }
            tmp = (MPZ_Object*)plus((MPZ_Object*)res);
            if (!tmp) {
                Py_DECREF(res);
                Py_DECREF(arg);
                return NULL;
            }

            mp_size_t newsize;

            if (tmp->size >= arg->size) {
                if (CHECK_NO_MEM_LEAK) {
                    newsize = mpn_gcd(res->digits, tmp->digits, tmp->size,
                                      arg->digits, arg->size);
                }
                else {
                    Py_DECREF(tmp);
                    Py_DECREF(res);
                    Py_DECREF(arg);
                    return PyErr_NoMemory();
                }
            }
            else {
                if (CHECK_NO_MEM_LEAK) {
                    newsize = mpn_gcd(res->digits, arg->digits, arg->size,
                                      tmp->digits, tmp->size);
                }
                else {
                    Py_DECREF(tmp);
                    Py_DECREF(res);
                    Py_DECREF(arg);
                    return PyErr_NoMemory();
                }
            }
            Py_DECREF(arg);
            Py_DECREF(tmp);
            if (newsize != res->size) {
                mp_limb_t *tmp_limbs = res->digits;

                res->digits = PyMem_Resize(tmp_limbs, mp_limb_t, newsize);
                if (!res->digits) {
                    res->digits = tmp_limbs;
                    Py_DECREF(res);
                    return PyErr_NoMemory();
                }
                res->size = newsize;
            }
        }
    }
    if (nzeros_res) {
        mpn_lshift(res->digits, res->digits, res->size, nzeros_res);
    }
    return (PyObject*)res;
}


static PyObject *
gmp_isqrt(PyObject *self, PyObject *other)
{
    static MPZ_Object *x, *res;

    if (MPZ_CheckExact(other)) {
        x = (MPZ_Object*)other;
        Py_INCREF(x);
    }
    else if (PyLong_Check(other)) {
        x = from_int(other);
        if (!x) {
            goto end;
        }
    }
    else {
        PyErr_SetString(PyExc_TypeError,
                        "isqrt() argument must be an integer");
        return NULL;
    }
    if (x->negative) {
        PyErr_SetString(PyExc_ValueError,
                        "isqrt() argument must be nonnegative");
        return NULL;
    }
    else if (!x->size) {
        return (PyObject*)MPZ_FromDigitSign(0, 0);
    }
    res = MPZ_new((x->size + 1)/2, 0);
    if (!res) {
        return NULL;
    }
    if (CHECK_NO_MEM_LEAK) {
        mpn_sqrtrem(res->digits, NULL, x->digits, x->size);
    }
    else {
        Py_DECREF(res);
        Py_DECREF(x);
        return PyErr_NoMemory();
    }
end:
    Py_DECREF(x);
    return (PyObject*)res;
}


static PyObject *
gmp_factorial(PyObject *self, PyObject *other)
{
    PyErr_SetString(PyExc_NotImplementedError, "factorial");
    return NULL;
}


static PyMethodDef functions [] =
{
    {"gcd", (PyCFunction)gmp_gcd, METH_FASTCALL,
     ("gcd($module, /, *integers)\n--\n\n"
      "Greatest Common Divisor.")},
    {"isqrt", gmp_isqrt, METH_O,
     ("isqrt($module, n, /)\n--\n\n"
      "Return the integer part of the square root of the input.")},
    {"factorial", gmp_factorial, METH_O,
     ("factorial($module, n, /)\n--\n\n"
      "Find n!.\n\nRaise a ValueError if x is negative or non-integral.")},
    {NULL}  /* sentinel */
};


static struct PyModuleDef gmp_module = {
    PyModuleDef_HEAD_INIT,
    "gmp",
    "Bindings to the GNU GMP for Python.",
    -1,
    functions,
};


PyMODINIT_FUNC
PyInit_gmp(void)
{
    PyObject *m = PyModule_Create(&gmp_module);

    if (PyModule_AddType(m, &MPZ_Type) < 0) {
        return NULL;
    }
    if (PyModule_Add(m, "_limb_size",
                     PyLong_FromSize_t(sizeof(mp_limb_t))) < 0)
    {
        return NULL;
    }
    mp_set_memory_functions(gmp_allocate_function, NULL,
                            gmp_free_function);

    PyObject *numbers = PyImport_ImportModule("numbers");

    if (!numbers) {
        return NULL;
    }

    const char* str = "numbers.Integral.register(gmp.mpz)\n";
    PyObject *ns = PyDict_New();

    if (!ns) {
        Py_DECREF(numbers);
        return NULL;
    }
    if ((PyDict_SetItemString(ns, "numbers", numbers) < 0)
        || (PyDict_SetItemString(ns, "gmp", m) < 0))
    {
        Py_DECREF(numbers);
        Py_DECREF(ns);
        return NULL;
    }

    PyObject *res = PyRun_String(str, Py_file_input, ns, ns);

    if (!res) {
        Py_DECREF(numbers);
        Py_DECREF(ns);
        return NULL;
    }
    Py_DECREF(res);

    PyObject *importlib = PyImport_ImportModule("importlib.metadata");

    if (!importlib) {
        Py_DECREF(ns);
        return NULL;
    }
    if (PyDict_SetItemString(ns, "importlib", importlib) < 0) {
        Py_DECREF(ns);
        Py_DECREF(importlib);
        return NULL;
    }
    str = "gmp.__version__ = importlib.version('python-gmp')\n";
    res = PyRun_String(str, Py_file_input, ns, ns);
    if (!res) {
        Py_DECREF(ns);
        Py_DECREF(importlib);
        return NULL;
    }
    Py_DECREF(ns);
    Py_DECREF(importlib);
    Py_DECREF(res);
    return m;
}
