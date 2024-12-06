#include "pythoncapi_compat.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <gmp.h>


static const char python_gmp_version[] = "0.1.0";


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
MPZ_to_str(MPZ_Object *self, int base)
{
    if (base < 2 || base > 62) {
        PyErr_SetString(PyExc_ValueError,
                        "base must be in the interval [2, 62]");
        return NULL;
    }

    Py_ssize_t len = mpn_sizeinbase(self->digits, self->size, base);
    unsigned char *buf = PyMem_Malloc(len + 1), *p = buf;

    if (buf == NULL) {
        return PyErr_NoMemory();
    }
    buf[0] = '-';

    mp_size_t actual_len = mpn_get_str(buf + 1, base,
                                       self->digits, self->size);
    const char *num_to_text = ("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                               "abcdefghijklmnopqrstuvwxyz");

    len += (actual_len == len);
    if (base <= 36) {
        num_to_text = "0123456789abcdefghijklmnopqrstuvwxyz";
    }
    for (mp_size_t i = 1; i < actual_len + 1; i++) {
        buf[i] = num_to_text[buf[i]];
    }
    if (!self->negative) {
        p++;
        len--;
    }

    PyObject *res = PyUnicode_FromStringAndSize((char*)p, len);

    PyMem_Free(buf);
    return res;
}

/**
 * Table of digit values for 8-bit string->mpz conversion.
 * Note that when converting a base B string, a char c is a legitimate
 * base B digit iff gmp_digit_value_tab[c] < B.
 */
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
    res->size = mpn_set_str(res->digits, p, len, base);
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
    PyObject *str = MPZ_to_str(self, 16);

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
    MPZ_Object *u;                  \
    int free_##u = 0;               \
    if (MPZ_CheckExact(a)) {        \
        u = (MPZ_Object*)a;         \
    }                               \
    else if (PyLong_Check(a)) {     \
        u = from_int(a);            \
        if (!u) {                   \
            goto end;               \
        }                           \
        free_##u = 1;               \
    }                               \
    else {                          \
        Py_RETURN_NOTIMPLEMENTED;   \
    }
#define FREE_OP(u)                  \
    if (free_##u) {                 \
        Py_DECREF(u);               \
    }


static PyObject *
add(PyObject* a, PyObject *b)
{
    PyObject *res = NULL;

    CHECK_OP(u, a);
    CHECK_OP(v, b);

    res = MPZ_add(u, v, 0);
end:
    FREE_OP(u);
    FREE_OP(v);
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
    FREE_OP(u);
    FREE_OP(v);
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
        mpn_sqr(res->digits, u->digits, u->size);
    }
    else {
        mpn_mul(res->digits, u->digits, u->size,
                v->digits, v->size);
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
    FREE_OP(u);
    FREE_OP(v);
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
        mpn_tdiv_qr((*q)->digits, (*r)->digits, 0,
                    a->digits, a->size,
                    b->digits, b->size);
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
    FREE_OP(u);
    FREE_OP(v);
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
    FREE_OP(u);
    FREE_OP(v);
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
    FREE_OP(u);
    FREE_OP(v);
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


static PyObject *
lshift(PyObject *a, PyObject *b)
{
    PyErr_SetString(PyExc_NotImplementedError, "mpz.__lshift__");
    return NULL;
}


static PyObject *
rshift(PyObject *a, PyObject *b)
{
    PyErr_SetString(PyExc_NotImplementedError, "mpz.__rshift__");
    return NULL;
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
    FREE_OP(u);
    FREE_OP(v);
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
    FREE_OP(u);
    FREE_OP(v);
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
    FREE_OP(u);
    FREE_OP(v);
    return (PyObject*)res;
}


static PyObject *
power(PyObject *a, PyObject *b, PyObject *m)
{
    MPZ_Object *u = (MPZ_Object *)a, *v = (MPZ_Object *)b;

    if (Py_IsNone(m)) {
        if (!v->size) {
            return (PyObject*)MPZ_FromDigitSign(1, 0);
        }
        if (!u->size) {
            return (PyObject*)MPZ_FromDigitSign(0, 0);
        }
        if (v->negative) {
            PyErr_SetString(PyExc_NotImplementedError,
                            "mpz.__pow__: float arg");
            return NULL;
        }
        if (u->size == 1 && u->digits[0] == 1) {
            if (u->negative) {
                return (PyObject*)MPZ_FromDigitSign(1, v->digits[0]%2);
            }
            else {
                return (PyObject*)MPZ_FromDigitSign(1, 0);
            }
        }
        if (v->size == 1) {
            MPZ_Object *res = MPZ_new(u->size*v->digits[0],
                                      u->negative && v->digits[0]%2);

            if (!res) {
                return NULL;
            }

            mp_limb_t *tmp = PyMem_New(mp_limb_t, res->size);

            if (!tmp) {
                Py_DECREF(res);
                return PyErr_NoMemory();
            }
            res->size = mpn_pow_1(res->digits, u->digits, u->size,
                                  v->digits[0], tmp);
            PyMem_Free(tmp);
            tmp = res->digits;
            res->digits = PyMem_Resize(tmp, mp_limb_t, res->size);
            if (!res->digits) {
                res->digits = tmp;
                Py_DECREF(res);
                return PyErr_NoMemory();
            }
            return (PyObject*)res;
        }
        return PyErr_NoMemory();
    }
    else {
        PyErr_SetString(PyExc_NotImplementedError,
                        "mpz.__pow__: ternary power");
        return NULL;
    }
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
    return MPZ_to_str(self, 10);
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

    FREE_OP(u);
    FREE_OP(v);
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
    FREE_OP(u);
    FREE_OP(v);
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
    PyErr_SetString(PyExc_NotImplementedError, "from_bytes()");
    return NULL;
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
    return Py_BuildValue("(Ni)", MPZ_to_str((MPZ_Object*)self, 16), 16);
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
    return PyBool_FromLong(1);
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
    int base = 10, argidx[1] = {-1};

    if (nargs == 1) {
        argidx[0] = 0;
    }
    if (kwnames) {
        nkws = PyTuple_GET_SIZE(kwnames);
    }
    if (nkws > 1) {
        PyErr_SetString(PyExc_TypeError,
                        "digits() takes at most one keyword argument");
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

    return MPZ_to_str((MPZ_Object*)self, base);
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


PyTypeObject MPZ_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "gmp.mpz",
    .tp_basicsize = sizeof(MPZ_Object),
    .tp_new = new,
    .tp_dealloc = (destructor) dealloc,
    .tp_repr = (reprfunc) repr,
    .tp_as_number = &as_number,
    .tp_richcompare = richcompare,
    .tp_hash = (hashfunc) hash,
    .tp_getset = getsetters,
    .tp_methods = methods,
};


static PyObject *
gmp_gcd(PyObject *self, PyObject * const *args, Py_ssize_t nargs)
{
    if (!nargs) {
        return (PyObject*)MPZ_FromDigitSign(0, 0);
    }

    Py_ssize_t i = 0;
    mp_bitcnt_t nzeros_res = 0;
    MPZ_Object *res, *arg = (MPZ_Object*)args[i];

    for (; i < nargs; i++) {
        arg = (MPZ_Object*)args[i];
        if (arg->size) {
            i++;
            break;
        }
    }
    res = (MPZ_Object*)absolute(arg);
    if (!res) {
        return NULL;
    }
    for (; i < nargs; i++) {
        if (res->size != 1 || res->negative || res->digits[0] != 1)
        {
            nzeros_res = mpn_scan1(res->digits, 0);
            if (nzeros_res) {
                mpn_rshift(res->digits, res->digits,
                           res->size, nzeros_res);
            }
            arg = (MPZ_Object*)args[i];
            if (!arg->size) {
                continue;
            }
            nzeros_res = Py_MIN(nzeros_res, mpn_scan1(arg->digits, 0));
            if (nzeros_res) {
                mpn_rshift(arg->digits, arg->digits, arg->size, nzeros_res);
            }

            MPZ_Object *tmp = (MPZ_Object*)plus((MPZ_Object*)res);

            if (!tmp) {
                Py_DECREF(res);
                return NULL;
            }

            mp_size_t newsize;

            if (tmp->size >= arg->size) {
                newsize = mpn_gcd(res->digits, tmp->digits, tmp->size,
                                  arg->digits, arg->size);
            }
            else {
                newsize = mpn_gcd(res->digits, arg->digits, arg->size,
                                  tmp->digits, tmp->size);
            }
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
    MPZ_Object *x = (MPZ_Object*)other;

    if (x->negative) {
        PyErr_SetString(PyExc_ValueError,
                        "isqrt() argument must be nonnegative");
        return NULL;
    }
    else if (!x->size) {
        return (PyObject*)MPZ_FromDigitSign(0, 0);
    }

    MPZ_Object *res = MPZ_new((x->size + 1)/2, 0);

    if (!res) {
        return NULL;
    }
    mpn_sqrtrem(res->digits, NULL, x->digits, x->size);
    return (PyObject*)res;
}


static PyMethodDef functions [] =
{
    {"gcd", (PyCFunction)gmp_gcd, METH_FASTCALL, "Greatest Common Divisor."},
    {"isqrt", gmp_isqrt, METH_O,
     "Return the integer part of the square root of the input."},
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
    if (PyModule_AddStringConstant(m, "__version__",
                                   python_gmp_version) < 0)
    {
        return NULL;
    }
    if (PyModule_Add(m, "_limb_size",
                     PyLong_FromSize_t(sizeof(mp_limb_t))) < 0)
    {
        return NULL;
    }
    return m;
}
