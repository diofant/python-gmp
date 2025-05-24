#include "pythoncapi_compat.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <float.h>
#include <gmp.h>
#include <setjmp.h>
#include <stdbool.h>

static jmp_buf gmp_env;
#define ENOUGH_MEMORY (setjmp(gmp_env) != 1)
#define TMP_OVERFLOW (setjmp(gmp_env) == 1)
#define TRACKER_MAX_SIZE 64
static struct {
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

typedef struct {
    bool negative;
    mp_size_t size;
    /* XXX: add alloc field? */
    mp_limb_t *digits;
} zz_t;

#define MP_OK   0
#define MP_MEM -1
#define MP_VAL -2
#define MP_BUF -3

typedef int8_t mp_err;

static mp_err
zz_init(zz_t *u)
{
    u->negative = false;
    u->size = 0;
    u->digits = NULL;
    return MP_OK;
}

static mp_err
zz_resize(zz_t *u, mp_size_t size)
{
    mp_limb_t *t = u->digits;

    u->digits = realloc(u->digits, (size ? size : 1)*sizeof(mp_limb_t));
    if (u->digits) {
        u->size = size;
        return MP_OK;
    }
    u->digits = t;
    return MP_MEM;
}

static void
zz_clear(zz_t *u)
{
    free(u->digits);
    u->negative = false;
    u->size = 0;
    u->digits = NULL;
}

static void
zz_normalize(zz_t *u)
{
    while (u->size && u->digits[u->size - 1] == 0) {
        u->size--;
    }
    if (!u->size) {
        u->negative = 0;
    }
}

#define mp_ord int8_t

#define MP_GT +1
#define MP_EQ  0
#define MP_LT -1

static mp_ord
zz_cmp(const zz_t *u, const zz_t *v)
{
    if (u == v) {
        return MP_EQ;
    }

    mp_ord sign = u->negative ? MP_LT : MP_GT;

    if (u->negative != v->negative) {
        return sign;
    }
    else if (u->size != v->size) {
        return (u->size < v->size) ? -sign : sign;
    }

    mp_ord r = mpn_cmp(u->digits, v->digits, u->size);

    return u->negative ? -r : r;
}

static mp_err
zz_from_i64(zz_t *u, int64_t v)
{
    if (!v) {
        return MP_OK;
    }

    bool negative = v < 0;
    uint64_t uv = (negative ? -((uint64_t)(v + 1) - 1) : (uint64_t)(v));
#if GMP_NUMB_BITS < 64
    mp_size_t size = 1 + (uv > GMP_NUMB_MAX);
#else
    mp_size_t size = 1;
#endif

    if (zz_resize(u, size)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    u->negative = negative;
    u->digits[0] = uv & GMP_NUMB_MASK;
#if GMP_NUMB_BITS < 64
    if (size == 2) {
        u->digits[1] = uv >> GMP_NUMB_BITS;
    }
#endif
    return MP_OK;
}

static mp_err
zz_copy(const zz_t *u, zz_t *v)
{
    if (!u->size) {
        return zz_from_i64(v, 0);
    }
    if (zz_resize(v, u->size)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    v->negative = u->negative;
    mpn_copyi(v->digits, u->digits, u->size);
    return MP_OK;
}

static mp_err
zz_abs(const zz_t *u, zz_t *v)
{
    mp_err ret = zz_copy(u, v);

    if (!ret) {
        v->negative = false;
    }
    return ret;
}

static mp_err
zz_neg(const zz_t *u, zz_t *v)
{
    mp_err ret = zz_copy(u, v);

    if (!ret && u->size) {
        v->negative = !u->negative;
    }
    return ret;
}

/* Maps 1-byte integer to digit character for bases up to 36. */
static const char *NUM_TO_TEXT = "0123456789abcdefghijklmnopqrstuvwxyz";
static const char *MPZ_TAG = "mpz(";
static int OPT_TAG = 0x1;
static int OPT_PREFIX = 0x2;

static mp_err
zz_to_str(const zz_t *u, int base, int options, char **buf)
{
    if (base < 2 || base > 36) {
        return MP_VAL;
    }

    size_t len = mpn_sizeinbase(u->digits, u->size, base);

    /*            tag sign prefix        )   \0 */
    *buf = malloc(4 + 1   + 2    + len + 1 + 1);

    unsigned char *p = (unsigned char *)(*buf);

    if (!p) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    if (options & OPT_TAG) {
        strcpy((char *)p, MPZ_TAG);
        p += strlen(MPZ_TAG);
    }
    if (u->negative) {
        *(p++) = '-';
    }
    if (options & OPT_PREFIX) {
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
    }
    if ((base & (base - 1)) == 0) {
        len -= (mpn_get_str(p, base, u->digits, u->size) != len);
    }
    else { /* generic base, not power of 2, input might be clobbered */
        mp_limb_t *tmp = malloc(sizeof(mp_limb_t) * (u->size ? u->size : 1));

        if (!tmp || TMP_OVERFLOW) {
            /* LCOV_EXCL_START */
            free(tmp);
            free(*buf);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }
        mpn_copyi(tmp, u->digits, u->size);
        len -= (mpn_get_str(p, base, tmp, u->size) != len);
        free(tmp);
    }
    for (size_t i = 0; i < len; i++) {
        *p = NUM_TO_TEXT[*p];
        p++;
    }
    if (options & OPT_TAG) {
        *(p++) = ')';
    }
    *(p++) = '\0';
    return MP_OK;
}

/* Table of digit values for 8-bit string->mpz conversion.
   Note that when converting a base B string, a char c is a legitimate
   base B digit iff DIGIT_VALUE_TAB[c] < B. */
const unsigned char DIGIT_VALUE_TAB[] =
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

static mp_err
zz_from_str(const char *str, size_t len, int base, zz_t *u)
{
    if (base != 0 && (base < 2 || base > 36)) {
        return MP_VAL;
    }
    if (!len) {
        return MP_VAL;
    }

    unsigned char *buf = malloc(len), *p = buf;

    if (!buf) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    memcpy(buf, str, len);

    bool negative = (p[0] == '-');

    p += negative;
    len -= negative;
    if (len && p[0] == '+') {
        p++;
        len--;
    }
    if (p[0] == '0' && len >= 2) {
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
                goto err;
            }
        }
        if ((tolower(p[1]) == 'b' && base == 2)
            || (tolower(p[1]) == 'o' && base == 8)
            || (tolower(p[1]) == 'x' && base == 16))
        {
            p += 2;
            len -= 2;
            if (len && p[0] == '_') {
                p++;
                len--;
            }
        }
    }
    if (base == 0) {
        base = 10;
    }
    if (!len) {
        goto err;
    }
    if (p[0] == '_') {
        goto err;
    }

    const unsigned char *digit_value = DIGIT_VALUE_TAB;
    size_t new_len = len;

    for (size_t i = 0; i < len; i++) {
        if (p[i] == '_') {
            if (i == len - 1 || p[i + 1] == '_') {
                goto err;
            }
            new_len--;
            memmove(p + i, p + i + 1, len - i - 1);
        }
        p[i] = digit_value[p[i]];
        if (p[i] >= base) {
            goto err;
        }
    }
    len = new_len;
    if (zz_resize(u, 1 + len/2) || TMP_OVERFLOW) {
        /* LCOV_EXCL_START */
        free(buf);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }
    u->negative = negative;
    u->size = mpn_set_str(u->digits, p, len, base);
    free(buf);
    if (zz_resize(u, u->size) == MP_MEM) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    zz_normalize(u);
    return MP_OK;
err:
    free(buf);
    return MP_VAL;
}

#define SWAP(T, a, b) \
    do {              \
        T _tmp = a;   \
        a = b;        \
        b = _tmp;     \
    } while (0);

static mp_err
_zz_addsub(const zz_t *u, const zz_t *v, bool subtract, zz_t *w)
{
    bool negu = u->negative, negv = subtract ? !v->negative : v->negative;
    bool same_sign = negu == negv;

    if (u->size < v->size) {
        SWAP(const zz_t *, u, v);
        SWAP(bool, negu, negv);
    }

    if (zz_resize(w, u->size + same_sign) || TMP_OVERFLOW) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    w->negative = negu;
    if (same_sign) {
        w->digits[w->size - 1] = mpn_add(w->digits,
                                         u->digits, u->size,
                                         v->digits, v->size);
    }
    else if (u->size != v->size) {
        mpn_sub(w->digits, u->digits, u->size, v->digits, v->size);
    }
    else {
        int cmp = mpn_cmp(u->digits, v->digits, u->size);

        if (cmp < 0) {
            mpn_sub_n(w->digits, v->digits, u->digits, u->size);
            w->negative = negv;
        }
        else if (cmp > 0) {
            mpn_sub_n(w->digits, u->digits, v->digits, u->size);
        }
        else {
            w->size = 0;
        }
    }
    zz_normalize(w);
    return MP_OK;
}

static mp_err
zz_add(const zz_t *u, const zz_t *v, zz_t *w)
{
    return _zz_addsub(u, v, false, w);
}

static mp_err
zz_sub(const zz_t *u, const zz_t *v, zz_t *w)
{
    return _zz_addsub(u, v, true, w);
}

static mp_err
zz_mul(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (u->size < v->size) {
        SWAP(const zz_t *, u, v);
    }
    if (!v->size) {
        return zz_from_i64(w, 0);
    }
    if (zz_resize(w, u->size + v->size) || TMP_OVERFLOW) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    w->negative = u->negative != v->negative;
    if (v->size == 1) {
        w->digits[w->size - 1] = mpn_mul_1(w->digits, u->digits, u->size,
                                           v->digits[0]);
    }
    else if (u->size == v->size) {
        if (u != v) {
            mpn_mul_n(w->digits, u->digits, v->digits, u->size);
        }
        else {
            mpn_sqr(w->digits, u->digits, u->size);
        }
    }
    else {
        mpn_mul(w->digits, u->digits, u->size, v->digits, v->size);
    }
    w->size -= w->digits[w->size - 1] == 0;
    return MP_OK;
}

static mp_err
zz_divmod(zz_t *q, zz_t *r, const zz_t *u, const zz_t *v)
{
    if (!v->size) {
        return MP_VAL;
    }
    if (!u->size) {
        if (zz_from_i64(q, 0) || zz_from_i64(r, 0)) {
            goto err; /* LCOV_EXCL_LINE */
        }
    }
    else if (u->size < v->size) {
        if (u->negative != v->negative) {
            if (zz_from_i64(q, -1) || zz_add(u, v, r)) {
                goto err; /* LCOV_EXCL_LINE */
            }
        }
        else {
            if (zz_from_i64(q, 0) || zz_copy(u, r)) {
                goto err; /* LCOV_EXCL_LINE */
            }
        }
    }
    else {
        bool q_negative = (u->negative != v->negative);

        if (zz_resize(q, u->size - v->size + 1 + q_negative)) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        q->negative = q_negative;
        if (q_negative) {
            q->digits[q->size - 1] = 0;
        }
        if (zz_resize(r, v->size)) {
            /* LCOV_EXCL_START */
            zz_clear(q);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }
        r->negative = v->negative;
        if (ENOUGH_MEMORY) {
            mpn_tdiv_qr(q->digits, r->digits, 0, u->digits, u->size,
                        v->digits, v->size);
        }
        else {
            goto err; /* LCOV_EXCL_LINE */
        }
        zz_normalize(r);
        if (q_negative && r->size) {
            r->size = v->size;
            mpn_sub_n(r->digits, v->digits, r->digits, v->size);
            mpn_add_1(q->digits, q->digits, q->size, 1);
        }
        zz_normalize(q);
        zz_normalize(r);
    }
    return MP_OK;
    /* LCOV_EXCL_START */
err:
    zz_clear(q);
    zz_clear(r);
    return MP_MEM;
    /* LCOV_EXCL_STOP */
}

static mp_err
zz_quo(const zz_t *u, const zz_t *v, zz_t *w)
{
    zz_t r;

    if (zz_init(&r)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }

    mp_err ret = zz_divmod(w, &r, u, v);

    zz_clear(&r);
    return ret;
}

static mp_err
zz_rem(const zz_t *u, const zz_t *v, zz_t *w)
{
    zz_t q;

    if (zz_init(&q)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }

    mp_err ret = zz_divmod(&q, w, u, v);

    zz_clear(&q);
    return ret;
}

static mp_err
zz_invert(const zz_t *u, zz_t *v)
{
    if (u->negative) {
        if (zz_resize(v, u->size)) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        mpn_sub_1(v->digits, u->digits, u->size, 1);
        v->size -= v->digits[u->size - 1] == 0;
    }
    else if (!u->size) {
        return zz_from_i64(v, -1);
    }
    else {
        if (zz_resize(v, u->size + 1)) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        v->negative = 1;
        v->digits[u->size] = mpn_add_1(v->digits, u->digits, u->size, 1);
        v->size -= v->digits[u->size] == 0;
    }
    return MP_OK;
}

typedef struct {
    PyObject_HEAD
    zz_t z;
} MPZ_Object;

PyTypeObject MPZ_Type;

#define LS(op) (((op)->z).digits)
#define SZ(op) (((op)->z).size)
#define ISNEG(op) (((op)->z).negative)

#if !defined(PYPY_VERSION) && !Py_GIL_DISABLED
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

typedef int8_t MPZ_err;

#define MPZ_OK   0
#define MPZ_MEM -1
#define MPZ_VAL -2
#define MPZ_BUF -3

static MPZ_Object *
MPZ_new(mp_size_t size, bool negative)
{
    MPZ_Object *res;

    if (global.gmp_cache_size && size <= MAX_CACHE_MPZ_LIMBS) {
        res = global.gmp_cache[--(global.gmp_cache_size)];
        if (SZ(res) < size && zz_resize(&res->z, size) == MPZ_MEM) {
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
        if (zz_init(&res->z) || zz_resize(&res->z, size)) {
            return (MPZ_Object *)PyErr_NoMemory(); /* LCOV_EXCL_LINE */
        }
    }
    ISNEG(res) = negative;
    return res;
}

#define MPZ_CheckExact(u) Py_IS_TYPE((u), &MPZ_Type)

static MPZ_Object *
MPZ_from_i64(int64_t v)
{
    MPZ_Object *res = MPZ_new(0, 0);

    if (res && zz_from_i64(&res->z, v)) {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    return res;
}

static MPZ_Object *
MPZ_copy(const MPZ_Object *u)
{
    MPZ_Object *res = MPZ_new(0, 0);

    if (res && zz_copy(&u->z, &res->z)) {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    return res;
}

static MPZ_Object *
MPZ_abs(const MPZ_Object *u)
{
    MPZ_Object *res = MPZ_new(0, 0);

    if (res && zz_abs(&u->z, &res->z)) {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    return res;
}

static PyObject *
MPZ_to_str(MPZ_Object *u, int base, int options)
{
    char *buf;
    mp_err ret = zz_to_str(&u->z, base, options, &buf);

    if (ret == MP_VAL) {
        PyErr_SetString(PyExc_ValueError, "mpz base must be >= 2 and <= 36");
        return NULL;
    }
    else if (ret == MP_MEM) {
        return PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }

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

    MPZ_Object *res = MPZ_new(0, 0);

    if (!res) {
        return (MPZ_Object *)PyErr_NoMemory(); /* LCOV_EXCL_LINE */
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

#define TMP_MPZ(z, u)                          \
    mpz_t z;                                   \
                                               \
    z->_mp_d = LS(u);                          \
    z->_mp_size = (ISNEG(u) ? -1 : 1) * SZ(u); \
    z->_mp_alloc = SZ(u);

#if !defined(PYPY_VERSION) && !defined(GRAALVM_PYTHON)
#  define BITS_TO_LIMBS(n) (((n) + (GMP_NUMB_BITS - 1))/GMP_NUMB_BITS)

static size_t int_digit_size, int_nails, int_bits_per_digit;
static int int_digits_order, int_endianness;
#endif

static MPZ_Object *
MPZ_from_int(PyObject *obj)
{
#if !defined(PYPY_VERSION) && !defined(GRAALVM_PYTHON)
    static PyLongExport long_export;
    MPZ_Object *res = NULL;

    if (PyLong_Export(obj, &long_export) < 0) {
        return res; /* LCOV_EXCL_LINE */
    }
    if (long_export.digits) {
        mp_size_t ndigits = long_export.ndigits;
        mp_size_t size = BITS_TO_LIMBS(ndigits*(8*int_digit_size - int_nails));
        res = MPZ_new(size, long_export.negative);
        if (!res) {
            return res; /* LCOV_EXCL_LINE */
        }

        TMP_MPZ(z, res)
        mpz_import(z, ndigits, int_digits_order, int_digit_size,
                   int_endianness, int_nails, long_export.digits);
        zz_normalize(&res->z);
        PyLong_FreeExport(&long_export);
    }
    else {
        res = MPZ_from_i64(long_export.value);
    }
    return res;
#else
    int64_t value;

    if (!PyLong_AsInt64(obj, &value)) {
        return MPZ_from_i64(value);
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

static MPZ_err
MPZ_to_i64(const MPZ_Object *u, int64_t *v)
{
    mp_size_t n = SZ(u);

    if (!n) {
        *v = 0;
        return MPZ_OK;
    }
    if (n > 2) {
        return MPZ_VAL;
    }

    uint64_t uv = LS(u)[0];

#if GMP_NUMB_BITS < 64
    if (n == 2) {
        if (LS(u)[1] >> GMP_NAIL_BITS) {
            return MPZ_VAL;
        }
        uv += LS(u)[1] << GMP_NUMB_BITS;
    }
#else
    if (n > 1) {
        return MPZ_VAL;
    }
#endif
    if (ISNEG(u)) {
        if (uv <= INT64_MAX + 1ULL) {
            *v = -1 - (int64_t)((uv - 1) & INT64_MAX);
            return MPZ_OK;
        }
    }
    else {
        if (uv <= INT64_MAX) {
            *v = (int64_t)uv;
            return MPZ_OK;
        }
    }
    return MPZ_VAL;
}

static PyObject *
MPZ_to_int(MPZ_Object *u)
{
    int64_t value;

    if (MPZ_to_i64(u, &value) == MPZ_OK) {
        return PyLong_FromInt64(value);
    }

#if !defined(PYPY_VERSION) && !defined(GRAALVM_PYTHON)
    size_t size = (mpn_sizeinbase(LS(u), SZ(u), 2) +
                   int_bits_per_digit - 1)/int_bits_per_digit;
    void *digits;
    PyLongWriter *writer = PyLongWriter_Create(ISNEG(u), size, &digits);

    if (!writer) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    TMP_MPZ(z, u);
    mpz_export(digits, NULL, int_digits_order, int_digit_size, int_endianness,
               int_nails, z);
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

static mp_limb_t
MPZ_AsManAndExp(MPZ_Object *u, Py_ssize_t *e)
{
    mp_limb_t high = 1ULL << DBL_MANT_DIG;
    mp_limb_t r = 0, carry, left;
    mp_size_t us = SZ(u), i, bits = 0;

    if (!us) {
        *e = 0;
        return 0;
    }
    r = LS(u)[us - 1];
    if (r >= high) {
        while ((r >> bits) >= high) {
            bits++;
        }
        left = 1ULL << (bits - 1);
        carry = r & (2*left - 1);
        r >>= bits;
        i = us - 1;
        *e = (us - 1)*GMP_NUMB_BITS + DBL_MANT_DIG + bits;
    }
    else {
        while (!((r << 1) & high)) {
            r <<= 1;
            bits++;
        }
        i = us - 1;
        *e = (us - 1)*GMP_NUMB_BITS + DBL_MANT_DIG - bits;
        for (i = us - 1; i && bits >= GMP_NUMB_BITS;) {
            bits -= GMP_NUMB_BITS;
            r += LS(u)[--i] << bits;
        }
        if (i == 0) {
            return r;
        }
        if (bits) {
            bits = GMP_NUMB_BITS - bits;
            left = 1ULL << (bits - 1);
            r += LS(u)[i - 1] >> bits;
            carry = LS(u)[i - 1] & (2*left - 1);
            i--;
        }
        else {
            left = 1ULL << (GMP_NUMB_BITS - 1);
            carry = LS(u)[i - 1];
            i--;
        }
    }
    if (carry > left) {
        r++;
    }
    else if (carry == left) {
        if (r%2 == 1) {
            r++;
        }
        else {
            mp_size_t j;

            for (j = 0; j < i; j++) {
                if (LS(u)[j]) {
                    break;
                }
            }
            if (i != j) {
                r++;
            }
        }
    }
    return r;
}

static double
MPZ_AsDoubleAndExp(MPZ_Object *u, Py_ssize_t *e)
{
    mp_limb_t man = MPZ_AsManAndExp(u, e);
    double d = ldexp(man, -DBL_MANT_DIG);

    if (ISNEG(u)) {
        d = -d;
    }
    return d;
}

static MPZ_Object *
MPZ_add(const MPZ_Object *u, const MPZ_Object *v)
{
    MPZ_Object *res = MPZ_new(0, 0);

    if (!res || zz_add(&u->z, &v->z, &res->z)) {
        /* LCOV_EXCL_START */
        Py_XDECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    return res;
}

static MPZ_Object *
MPZ_sub(const MPZ_Object *u, const MPZ_Object *v)
{
    MPZ_Object *res = MPZ_new(0, 0);

    if (!res || zz_sub(&u->z, &v->z, &res->z)) {
        /* LCOV_EXCL_START */
        Py_XDECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    return res;
}

static MPZ_Object *
MPZ_mul(const MPZ_Object *u, const MPZ_Object *v)
{
    MPZ_Object *res = MPZ_new(0, 0);

    if (!res || zz_mul(&u->z, &v->z, &res->z)) {
        /* LCOV_EXCL_START */
        Py_XDECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    return res;
}

static MPZ_err
MPZ_divmod(MPZ_Object **q, MPZ_Object **r, const MPZ_Object *u,
           const MPZ_Object *v)
{
    *q = MPZ_new(0, 0);
    *r = MPZ_new(0, 0);
    if (!*q || !*r) {
        /* LCOV_EXCL_START */
        Py_XDECREF(*q);
        Py_XDECREF(*r);
        return MPZ_MEM;
        /* LCOV_EXCL_STOP */
    }
    mp_err ret = zz_divmod(&(*q)->z, &(*r)->z, &u->z, &v->z);

    if (ret == MP_OK) {
        return MPZ_OK;
    }
    Py_DECREF(*q);
    Py_DECREF(*r);
    if (ret == MP_VAL) {
        PyErr_SetString(PyExc_ZeroDivisionError, "division by zero");
        return MPZ_VAL;
    }
    /* LCOV_EXCL_START */
    PyErr_NoMemory();
    return MPZ_MEM;
    /* LCOV_EXCL_STOP */
}

static MPZ_Object *
MPZ_quo(const MPZ_Object *u, const MPZ_Object *v)
{
    MPZ_Object *res = MPZ_new(0, 0);

    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    mp_err ret = zz_quo(&u->z, &v->z, &res->z);

    if (ret == MP_OK) {
        return res;
    }
    if (ret == MP_VAL) {
        PyErr_SetString(PyExc_ZeroDivisionError, "division by zero");
        return NULL;
    }
    /* LCOV_EXCL_START */
    return (MPZ_Object *)PyErr_NoMemory();
    /* LCOV_EXCL_STOP */
}

static MPZ_Object *
MPZ_rem(MPZ_Object *u, MPZ_Object *v)
{
    MPZ_Object *res = MPZ_new(0, 0);

    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    mp_err ret = zz_rem(&u->z, &v->z, &res->z);

    if (ret == MP_OK) {
        return res;
    }
    if (ret == MP_VAL) {
        PyErr_SetString(PyExc_ZeroDivisionError, "division by zero");
        return NULL;
    }
    /* LCOV_EXCL_START */
    return (MPZ_Object *)PyErr_NoMemory();
    /* LCOV_EXCL_STOP */
}

static MPZ_Object *
MPZ_rshift1(const MPZ_Object *u, mp_limb_t rshift, bool negative)
{
    mp_size_t whole = rshift / GMP_NUMB_BITS;
    mp_size_t size = SZ(u);

    rshift %= GMP_NUMB_BITS;
    if (whole >= size) {
        return MPZ_from_i64((negative ? -1 : 1) * ISNEG(u));
    }
    size -= whole;

    bool carry = 0, extra = 1;

    for (mp_size_t i = 0; i < whole; i++) {
        if (LS(u)[i]) {
            carry = negative;
            break;
        }
    }
    for (mp_size_t i = whole; i < SZ(u); i++) {
        if (LS(u)[i] != GMP_NUMB_MAX) {
            extra = 0;
            break;
        }
    }

    MPZ_Object *res = MPZ_new(size + extra, negative);

    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    if (extra) {
        LS(res)[size] = 0;
    }
    if (rshift) {
        if (mpn_rshift(LS(res), LS(u) + whole, size, rshift)) {
            carry = negative;
        }
    }
    else {
        mpn_copyi(LS(res), LS(u) + whole, size);
    }
    if (carry) {
        if (mpn_add_1(LS(res), LS(res), size, 1)) {
            LS(res)[size] = 1;
        }
    }
    zz_normalize(&res->z);
    return res;
}

static int
MPZ_divmod_near(MPZ_Object **q, MPZ_Object **r, MPZ_Object *u, MPZ_Object *v)
{
    int unexpect = ISNEG(v) ? -1 : 1;

    if (MPZ_divmod(q, r, u, v)) {
        return -1; /* LCOV_EXCL_LINE */
    }

    MPZ_Object *halfQ = MPZ_rshift1(v, 1, 0);

    if (!halfQ) {
        /* LCOV_EXCL_START */
        Py_DECREF(*q);
        Py_DECREF(*r);
        return -1;
        /* LCOV_EXCL_STOP */
    }

    int cmp = zz_cmp(&(*r)->z, &halfQ->z);

    Py_DECREF(halfQ);
    if (cmp == MP_EQ && LS(v)[0]%2 == 0 && SZ(*q) && LS(*q)[0]%2 != 0) {
        cmp = unexpect;
    }
    if (cmp == unexpect) {
        MPZ_Object *tmp = *q;
        MPZ_Object *one = MPZ_from_i64(1);

        if (!one) {
            return -1; /* LCOV_EXCL_LINE */
        }
        *q = MPZ_add(*q, one);
        if (!*q) {
            /* LCOV_EXCL_START */
            Py_DECREF(tmp);
            Py_DECREF(*r);
            Py_DECREF(one);
            return -1;
            /* LCOV_EXCL_STOP */
        }
        Py_DECREF(tmp);
        Py_DECREF(one);
        tmp = *r;
        *r = MPZ_sub(*r, v);
        if (!*r) {
            /* LCOV_EXCL_START */
            Py_DECREF(tmp);
            Py_DECREF(*q);
            return -1;
            /* LCOV_EXCL_STOP */
        }
        Py_DECREF(tmp);
    }
    return 0;
}

static MPZ_Object *
MPZ_lshift1(MPZ_Object *u, mp_limb_t lshift, bool negative)
{
    mp_size_t whole = lshift / GMP_NUMB_BITS;
    mp_size_t size = SZ(u) + whole;

    lshift %= GMP_NUMB_BITS;
    if (lshift) {
        size++;
    }

    MPZ_Object *res = MPZ_new(size, negative);

    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    if (whole) {
        mpn_zero(LS(res), whole);
    }
    if (lshift) {
        LS(res)[size - 1] = mpn_lshift(LS(res) + whole, LS(u), SZ(u), lshift);
    }
    else {
        mpn_copyi(LS(res) + whole, LS(u), SZ(u));
    }
    zz_normalize(&res->z);
    return res;
}

static PyObject *
MPZ_truediv(MPZ_Object *u, MPZ_Object *v)
{
    if (!SZ(v)) {
        PyErr_SetString(PyExc_ZeroDivisionError, "division by zero");
        return NULL;
    }
    if (!SZ(u)) {
        return PyFloat_FromDouble(ISNEG(v) ? -0.0 : 0.0);
    }

    Py_ssize_t shift = (mpn_sizeinbase(LS(v), SZ(v), 2)
                        - mpn_sizeinbase(LS(u), SZ(u), 2));
    Py_ssize_t n = shift;
    MPZ_Object *a = u, *b = v;

    if (shift < 0) {
        SWAP(MPZ_Object *, a, b);
        n = -n;
    }

    mp_size_t whole = n / GMP_NUMB_BITS;

    n %= GMP_NUMB_BITS;
    for (mp_size_t i = SZ(b); i--;) {
        mp_limb_t da, db = LS(b)[i];

        if (i >= whole) {
            if (i - whole < SZ(a)) {
                da = LS(a)[i - whole] << n;
            }
            else {
                da = 0;
            }
            if (n && i > whole) {
                da |= LS(a)[i - whole - 1] >> (GMP_NUMB_BITS - n);
            }
        }
        else {
            da = 0;
        }
        if (da < db) {
            if (shift >= 0) {
                shift++;
            }
            break;
        }
        if (da > db) {
            if (shift < 0) {
                shift++;
            }
            break;
        }
    }
    shift += DBL_MANT_DIG - 1;
    if (shift > 0) {
        a = MPZ_lshift1(u, shift, 0);
    }
    else {
        a = MPZ_abs(u);
    }
    if (!a) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    if (shift < 0) {
        b = MPZ_lshift1(v, -shift, 0);
    }
    else {
        b = MPZ_abs(v);
    }
    if (!b) {
        /* LCOV_EXCL_START */
        Py_DECREF(a);
        return NULL;
        /* LCOV_EXCL_STOP */
    }

    MPZ_Object *c, *d;

    if (MPZ_divmod_near(&c, &d, a, b) == -1) {
        /* LCOV_EXCL_START */
        Py_DECREF(a);
        Py_DECREF(b);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    Py_DECREF(a);
    Py_DECREF(b);
    Py_DECREF(d);

    Py_ssize_t exp;
    double res = MPZ_AsDoubleAndExp(c, &exp);

    Py_DECREF(c);
    if (ISNEG(u) != ISNEG(v)) {
        res = -res;
    }
    exp -= shift;
    res = ldexp(res, exp);
    if (exp > DBL_MAX_EXP || isinf(res)) {
        PyErr_SetString(PyExc_OverflowError,
                        "integer too large to convert to float");
        return NULL;
    }
    return PyFloat_FromDouble(res);
}

static MPZ_Object *
MPZ_invert(MPZ_Object *u)
{
    MPZ_Object *res = MPZ_new(0, 0);

    if (!res || zz_invert(&u->z, &res->z)) {
        /* LCOV_EXCL_START */
        Py_XDECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    return res;
}

static MPZ_Object *
MPZ_lshift(MPZ_Object *u, MPZ_Object *v)
{
    if (ISNEG(v)) {
        PyErr_SetString(PyExc_ValueError, "negative shift count");
        return NULL;
    }
    if (!SZ(u)) {
        return MPZ_from_i64(0);
    }
    if (!SZ(v)) {
        return MPZ_copy(u);
    }
    if (SZ(v) > 1) {
        PyErr_SetString(PyExc_OverflowError, "too many digits in integer");
        return NULL;
    }
    return MPZ_lshift1(u, LS(v)[0], ISNEG(u));
}

static MPZ_Object *
MPZ_rshift(MPZ_Object *u, MPZ_Object *v)
{
    if (ISNEG(v)) {
        PyErr_SetString(PyExc_ValueError, "negative shift count");
        return NULL;
    }
    if (!SZ(u)) {
        return MPZ_from_i64(0);
    }
    if (!SZ(v)) {
        return MPZ_copy(u);
    }
    if (SZ(v) > 1) {
        if (ISNEG(u)) {
            return MPZ_from_i64(-1);
        }
        else {
            return MPZ_from_i64(0);
        }
    }
    return MPZ_rshift1(u, LS(v)[0], ISNEG(u));
}

static MPZ_Object *
MPZ_and(MPZ_Object *u, MPZ_Object *v)
{
    if (!SZ(u) || !SZ(v)) {
        return MPZ_from_i64(0);
    }

    MPZ_Object *res;

    if (ISNEG(u) || ISNEG(v)) {
        if (ISNEG(u)) {
            u = MPZ_invert(u);
            if (!u) {
                return NULL; /* LCOV_EXCL_LINE */
            }
            ISNEG(u) = 1;
        }
        else {
            Py_INCREF(u);
        }
        if (ISNEG(v)) {
            v = MPZ_invert(v);
            if (!v) {
                /* LCOV_EXCL_START */
                Py_DECREF(u);
                return NULL;
                /* LCOV_EXCL_STOP */
            }
            ISNEG(v) = 1;
        }
        else {
            Py_INCREF(v);
        }
        if (SZ(u) < SZ(v)) {
            SWAP(MPZ_Object *, u, v);
        }
        if (ISNEG(u) & ISNEG(v)) {
            if (!SZ(u)) {
                Py_DECREF(u);
                Py_DECREF(v);
                return MPZ_from_i64(-1);
            }
            res = MPZ_new(SZ(u) + 1, 1);
            if (!res) {
                /* LCOV_EXCL_START */
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
                /* LCOV_EXCL_STOP */
            }
            mpn_copyi(&LS(res)[SZ(v)], &LS(u)[SZ(v)], SZ(u) - SZ(v));
            if (SZ(v)) {
                mpn_ior_n(LS(res), LS(u), LS(v), SZ(v));
            }
            LS(res)[SZ(u)] = mpn_add_1(LS(res), LS(res), SZ(u), 1);
            zz_normalize(&res->z);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else if (ISNEG(u)) {
            res = MPZ_new(SZ(v), 0);
            if (!res) {
                /* LCOV_EXCL_START */
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
                /* LCOV_EXCL_STOP */
            }
            mpn_andn_n(LS(res), LS(v), LS(u), SZ(v));
            zz_normalize(&res->z);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else {
            res = MPZ_new(SZ(u), 0);
            if (!res) {
                /* LCOV_EXCL_START */
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
                /* LCOV_EXCL_STOP */
            }
            if (SZ(v)) {
                mpn_andn_n(LS(res), LS(u), LS(v), SZ(v));
            }
            mpn_copyi(&LS(res)[SZ(v)], &LS(u)[SZ(v)], SZ(u) - SZ(v));
            zz_normalize(&res->z);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
    }
    if (SZ(u) < SZ(v)) {
        SWAP(MPZ_Object *, u, v);
    }
    res = MPZ_new(SZ(v), 0);
    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    mpn_and_n(LS(res), LS(u), LS(v), SZ(v));
    zz_normalize(&res->z);
    return res;
}

static MPZ_Object *
MPZ_or(MPZ_Object *u, MPZ_Object *v)
{
    if (!SZ(u)) {
        return MPZ_copy(v);
    }
    if (!SZ(v)) {
        return MPZ_copy(u);
    }

    MPZ_Object *res;

    if (ISNEG(u) || ISNEG(v)) {
        if (ISNEG(u)) {
            u = MPZ_invert(u);
            if (!u) {
                return NULL; /* LCOV_EXCL_LINE */
            }
            ISNEG(u) = 1;
        }
        else {
            Py_INCREF(u);
        }
        if (ISNEG(v)) {
            v = MPZ_invert(v);
            if (!v) {
                /* LCOV_EXCL_START */
                Py_DECREF(u);
                return NULL;
                /* LCOV_EXCL_STOP */
            }
            ISNEG(v) = 1;
        }
        else {
            Py_INCREF(v);
        }
        if (SZ(u) < SZ(v)) {
            SWAP(MPZ_Object *, u, v);
        }
        if (ISNEG(u) & ISNEG(v)) {
            if (!SZ(v)) {
                Py_DECREF(u);
                Py_DECREF(v);
                return MPZ_from_i64(-1);
            }
            res = MPZ_new(SZ(v) + 1, 1);
            if (!res) {
                /* LCOV_EXCL_START */
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
                /* LCOV_EXCL_STOP */
            }
            mpn_and_n(LS(res), LS(u), LS(v), SZ(v));
            LS(res)[SZ(v)] = mpn_add_1(LS(res), LS(res), SZ(v), 1);
            zz_normalize(&res->z);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else if (ISNEG(u)) {
            res = MPZ_new(SZ(u) + 1, 1);
            if (!res) {
                /* LCOV_EXCL_START */
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
                /* LCOV_EXCL_STOP */
            }
            mpn_copyi(&LS(res)[SZ(v)], &LS(u)[SZ(v)], SZ(u) - SZ(v));
            mpn_andn_n(LS(res), LS(u), LS(v), SZ(v));
            LS(res)[SZ(u)] = mpn_add_1(LS(res), LS(res), SZ(u), 1);
            zz_normalize(&res->z);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else {
            res = MPZ_new(SZ(v) + 1, 1);
            if (!res) {
                /* LCOV_EXCL_START */
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
                /* LCOV_EXCL_STOP */
            }
            if (SZ(v)) {
                mpn_andn_n(LS(res), LS(v), LS(u), SZ(v));
                LS(res)[SZ(v)] = mpn_add_1(LS(res), LS(res), SZ(v), 1);
                zz_normalize(&res->z);
            }
            else {
                LS(res)[0] = 1;
            }
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
    }
    if (SZ(u) < SZ(v)) {
        SWAP(MPZ_Object *, u, v);
    }
    res = MPZ_new(SZ(u), 0);
    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    mpn_ior_n(LS(res), LS(u), LS(v), SZ(v));
    if (SZ(u) != SZ(v)) {
        mpn_copyi(&LS(res)[SZ(v)], &LS(u)[SZ(v)], SZ(u) - SZ(v));
    }
    return res;
}

static MPZ_Object *
MPZ_xor(MPZ_Object *u, MPZ_Object *v)
{
    if (!SZ(u)) {
        return MPZ_copy(v);
    }
    if (!SZ(v)) {
        return MPZ_copy(u);
    }

    MPZ_Object *res;

    if (ISNEG(u) || ISNEG(v)) {
        if (ISNEG(u)) {
            u = MPZ_invert(u);
            if (!u) {
                return NULL; /* LCOV_EXCL_LINE */
            }
            ISNEG(u) = 1;
        }
        else {
            Py_INCREF(u);
        }
        if (ISNEG(v)) {
            v = MPZ_invert(v);
            if (!v) {
                /* LCOV_EXCL_START */
                Py_DECREF(u);
                return NULL;
                /* LCOV_EXCL_STOP */
            }
            ISNEG(v) = 1;
        }
        else {
            Py_INCREF(v);
        }
        if (SZ(u) < SZ(v)) {
            SWAP(MPZ_Object *, u, v);
        }
        if (ISNEG(u) & ISNEG(v)) {
            if (!SZ(u)) {
                Py_DECREF(u);
                Py_DECREF(v);
                return MPZ_from_i64(0);
            }
            res = MPZ_new(SZ(u), 0);
            if (!res) {
                /* LCOV_EXCL_START */
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
                /* LCOV_EXCL_STOP */
            }
            mpn_copyi(&LS(res)[SZ(v)], &LS(u)[SZ(v)], SZ(u) - SZ(v));
            if (SZ(v)) {
                mpn_xor_n(LS(res), LS(u), LS(v), SZ(v));
            }
            zz_normalize(&res->z);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else if (ISNEG(u)) {
            res = MPZ_new(SZ(u) + 1, 1);
            if (!res) {
                /* LCOV_EXCL_START */
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
                /* LCOV_EXCL_STOP */
            }
            mpn_copyi(&LS(res)[SZ(v)], &LS(u)[SZ(v)], SZ(u) - SZ(v));
            mpn_xor_n(LS(res), LS(v), LS(u), SZ(v));
            LS(res)[SZ(u)] = mpn_add_1(LS(res), LS(res), SZ(u), 1);
            zz_normalize(&res->z);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else {
            res = MPZ_new(SZ(u) + 1, 1);
            if (!res) {
                /* LCOV_EXCL_START */
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
                /* LCOV_EXCL_STOP */
            }
            mpn_copyi(&LS(res)[SZ(v)], &LS(u)[SZ(v)], SZ(u) - SZ(v));
            if (SZ(v)) {
                mpn_xor_n(LS(res), LS(u), LS(v), SZ(v));
            }
            LS(res)[SZ(u)] = mpn_add_1(LS(res), LS(res), SZ(u), 1);
            zz_normalize(&res->z);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
    }
    if (SZ(u) < SZ(v)) {
        SWAP(MPZ_Object *, u, v);
    }
    res = MPZ_new(SZ(u), 0);
    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    mpn_xor_n(LS(res), LS(u), LS(v), SZ(v));
    if (SZ(u) != SZ(v)) {
        mpn_copyi(&LS(res)[SZ(v)], &LS(u)[SZ(v)], SZ(u) - SZ(v));
    }
    else {
        zz_normalize(&res->z);
    }
    return res;
}

static MPZ_Object *
MPZ_pow(MPZ_Object *u, MPZ_Object *v)
{
    if (!SZ(v)) {
        return MPZ_from_i64(1);
    }
    if (!SZ(u)) {
        return MPZ_from_i64(0);
    }
    if (SZ(u) == 1 && LS(u)[0] == 1) {
        if (ISNEG(u)) {
            return MPZ_from_i64((LS(v)[0] % 2) ? -1 : 1);
        }
        else {
            return MPZ_from_i64(1);
        }
    }
    if (SZ(v) > 1 || ISNEG(v)) {
        return NULL;
    }

    mp_limb_t e = LS(v)[0];
    MPZ_Object *res = MPZ_new(SZ(u) * e, ISNEG(u) && e%2);

    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    mp_limb_t *tmp = PyMem_New(mp_limb_t, SZ(res));

    if (!tmp) {
        /* LCOV_EXCL_START */
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    if (ENOUGH_MEMORY) {
        SZ(res) = mpn_pow_1(LS(res), LS(u), SZ(u), e, tmp);
    }
    else {
        /* LCOV_EXCL_START */
        PyMem_Free(tmp);
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    PyMem_Free(tmp);
    if (zz_resize(&res->z, SZ(res)) == MPZ_MEM) {
        /* LCOV_EXCL_START */
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    return res;
}

/* XXX: don't use mpz_powm() for even w, replace
   mpn_sec_powm() by mpn_powm(). */
static MPZ_Object *
MPZ_powm(MPZ_Object *u, MPZ_Object *v, MPZ_Object *w)
{
    if (mpn_scan1(LS(w), 0)) {
        mpz_t z;
        TMP_MPZ(b, u)
        TMP_MPZ(e, v)
        TMP_MPZ(m, w)
        if (ENOUGH_MEMORY) {
            mpz_init(z);
            mpz_powm(z, b, e, m);
        }
        else {
            return (MPZ_Object *)PyErr_NoMemory(); /* LCOV_EXCL_LINE */
        }

        MPZ_Object *res = MPZ_new(z->_mp_size, 0);

        if (!res) {
            /* LCOV_EXCL_START */
            mpz_clear(z);
            return NULL;
            /* LCOV_EXCL_STOP */
        }
        mpn_copyi(LS(res), z->_mp_d, SZ(res));
        mpz_clear(z);
        return res;
    }

    MPZ_Object *res = MPZ_new(SZ(w), 0);

    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    mp_size_t enb = SZ(v) * GMP_NUMB_BITS;
    mp_size_t tmp_size = mpn_sec_powm_itch(SZ(u), enb, SZ(w));
    mp_limb_t *tmp = PyMem_New(mp_limb_t, tmp_size);

    if (!tmp) {
        /* LCOV_EXCL_START */
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    if (ENOUGH_MEMORY) {
        mpn_sec_powm(LS(res), LS(u), SZ(u), LS(v), enb, LS(w), SZ(w), tmp);
    }
    else {
        /* LCOV_EXCL_START */
        PyMem_Free(tmp);
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    PyMem_Free(tmp);
    zz_normalize(&res->z);
    return res;
}

MPZ_err
MPZ_gcdext(const MPZ_Object *u, const MPZ_Object *v, MPZ_Object *g,
           MPZ_Object *s, MPZ_Object *t)
{
    if (SZ(u) < SZ(v)) {
        SWAP(const MPZ_Object *, u, v);
        SWAP(MPZ_Object *, s, t);
    }
    if (!SZ(v)) {
        if (g) {
            if (zz_resize(&g->z, SZ(u)) == MPZ_MEM) {
                return MPZ_MEM; /* LCOV_EXCL_LINE */
            }
            ISNEG(g) = 0;
            mpn_copyi(LS(g), LS(u), SZ(u));
        }
        if (s) {
            if (zz_resize(&s->z, 1) == MPZ_MEM) {
                return MPZ_MEM; /* LCOV_EXCL_LINE */
            }
            LS(s)[0] = 1;
            SZ(s) = SZ(u) > 0;
            ISNEG(s) = ISNEG(u);
        }
        if (t) {
            SZ(t) = 0;
            ISNEG(t) = 0;
        }
        return MPZ_OK;
    }

    MPZ_Object *arg_u = MPZ_copy(u), *arg_v = MPZ_copy(v);
    MPZ_Object *tmp_g = MPZ_new(SZ(v), 0);
    MPZ_Object *tmp_s = MPZ_new(SZ(v) + 1, 0);

    if (!arg_u || !arg_v || !tmp_g || !tmp_s) {
        /* LCOV_EXCL_START */
        Py_XDECREF(arg_u);
        Py_XDECREF(arg_v);
        Py_XDECREF(tmp_g);
        Py_XDECREF(tmp_s);
        return MPZ_MEM;
        /* LCOV_EXCL_STOP */
    }
    if (ENOUGH_MEMORY) {
        mp_size_t ssize;

        SZ(tmp_g) = mpn_gcdext(LS(tmp_g), LS(tmp_s), &ssize, LS(arg_u), SZ(u),
                               LS(arg_v), SZ(v));
        ISNEG(tmp_g) = 0;
        SZ(tmp_s) = Py_ABS(ssize);
        ISNEG(tmp_s) = (ISNEG(u) && ssize > 0) || (!ISNEG(u) && ssize < 0);
    }
    else {
        /* LCOV_EXCL_START */
        Py_DECREF(arg_u);
        Py_DECREF(arg_v);
        Py_XDECREF(tmp_g);
        Py_XDECREF(tmp_s);
        return MPZ_MEM;
        /* LCOV_EXCL_STOP */
    }
    Py_DECREF(arg_u);
    Py_DECREF(arg_v);
    if (t) {
        MPZ_Object *us = MPZ_mul(u, tmp_s);

        if (!us) {
            /* LCOV_EXCL_START */
            Py_XDECREF(tmp_g);
            Py_XDECREF(tmp_s);
            return MPZ_MEM;
            /* LCOV_EXCL_STOP */
        }

        MPZ_Object *x = MPZ_sub(tmp_g, us);

        Py_DECREF(us);
        if (!x) {
            /* LCOV_EXCL_START */
            Py_XDECREF(tmp_g);
            Py_XDECREF(tmp_s);
            return MPZ_MEM;
            /* LCOV_EXCL_STOP */
        }

        MPZ_Object *q = MPZ_quo(x, v);

        Py_DECREF(x);
        if (!q) {
            /* LCOV_EXCL_START */
            Py_XDECREF(tmp_g);
            Py_XDECREF(tmp_s);
            return MPZ_MEM;
            /* LCOV_EXCL_STOP */
        }
        if (zz_resize(&t->z, SZ(q)) == MPZ_MEM) {
            /* LCOV_EXCL_START */
            Py_XDECREF(tmp_g);
            Py_XDECREF(tmp_s);
            Py_XDECREF(q);
            return MPZ_MEM;
            /* LCOV_EXCL_STOP */
        }
        mpn_copyi(LS(t), LS(q), SZ(q));
        ISNEG(t) = ISNEG(q);
        Py_XDECREF(q);
    }
    if (s) {
        if (zz_resize(&s->z, SZ(tmp_s)) == MPZ_MEM) {
            /* LCOV_EXCL_START */
            Py_XDECREF(tmp_g);
            Py_XDECREF(tmp_s);
            return MPZ_MEM;
            /* LCOV_EXCL_STOP */
        }
        mpn_copyi(LS(s), LS(tmp_s), SZ(tmp_s));
        ISNEG(s) = ISNEG(tmp_s);
        Py_XDECREF(tmp_s);
    }
    if (g) {
        if (zz_resize(&g->z, SZ(tmp_g)) == MPZ_MEM) {
            /* LCOV_EXCL_START */
            Py_XDECREF(tmp_g);
            return MPZ_MEM;
            /* LCOV_EXCL_STOP */
        }
        mpn_copyi(LS(g), LS(tmp_g), SZ(tmp_g));
        ISNEG(g) = 0;
        Py_XDECREF(tmp_g);
    }
    return MPZ_OK;
}

static MPZ_Object *
MPZ_inverse(MPZ_Object *u, MPZ_Object *v)
{
    MPZ_Object *g = MPZ_new(0, 0), *s = MPZ_new(0, 0);

    if (!g || !s || MPZ_gcdext(u, v, g, s, NULL) == MPZ_MEM) {
        /* LCOV_EXCL_START */
        Py_XDECREF(g);
        Py_XDECREF(s);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    if (SZ(g) == 1 && LS(g)[0] == 1) {
        Py_DECREF(g);
        return s;
    }
    Py_DECREF(g);
    Py_DECREF(s);
    PyErr_SetString(PyExc_ValueError,
                    "base is not invertible for the given modulus");
    return NULL;
}

static void
revstr(char *s, size_t l, size_t r)
{
    while (l < r) {
        SWAP(char, s[l], s[r]);
        l++;
        r--;
    }
}

static PyObject *
MPZ_to_bytes(MPZ_Object *u, Py_ssize_t length, int is_little, int is_signed)
{
    MPZ_Object *tmp = NULL;
    int is_negative = ISNEG(u);

    if (is_negative) {
        if (!is_signed) {
            PyErr_SetString(PyExc_OverflowError,
                            "can't convert negative mpz to unsigned");
            return NULL;
        }
        tmp = MPZ_new((8*length)/GMP_NUMB_BITS + 1, 0);
        if (!tmp) {
            return NULL; /* LCOV_EXCL_LINE */
        }
        if (SZ(tmp) < SZ(u)) {
            goto overflow;
        }
        mpn_zero(LS(tmp), SZ(tmp));
        LS(tmp)[SZ(tmp) - 1] = 1;
        LS(tmp)[SZ(tmp) - 1] <<= (8*length) % (GMP_NUMB_BITS*SZ(tmp));
        mpn_sub(LS(tmp), LS(tmp), SZ(tmp), LS(u), SZ(u));
        zz_normalize(&tmp->z);
        u = tmp;
    }

    Py_ssize_t nbits = SZ(u) ? mpn_sizeinbase(LS(u), SZ(u), 2) : 0;

    if (nbits > 8*length
        || (is_signed && nbits
            && (nbits == 8 * length ? !is_negative : is_negative)))
    {
overflow:
        Py_XDECREF(tmp);
        PyErr_SetString(PyExc_OverflowError, "int too big to convert");
        return NULL;
    }

    char *buffer = PyMem_Malloc(length);
    Py_ssize_t gap = length - (nbits + GMP_NUMB_BITS/8 - 1)/(GMP_NUMB_BITS/8);

    if (!buffer) {
        /* LCOV_EXCL_START */
        Py_XDECREF(tmp);
        return PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    memset(buffer, is_negative ? 0xFF : 0, gap);
    if (SZ(u)) {
        mpn_get_str((unsigned char *)(buffer + gap), 256, LS(u), SZ(u));
    }
    Py_XDECREF(tmp);
    if (is_little && length) {
        revstr(buffer, 0, length - 1);
    }

    PyObject *bytes = PyBytes_FromStringAndSize(buffer, length);

    PyMem_Free(buffer);
    return bytes;
}

static MPZ_Object *
MPZ_from_bytes(PyObject *obj, int is_little, int is_signed)
{
    PyObject *bytes = PyObject_Bytes(obj);
    char *buffer;
    Py_ssize_t length;

    if (bytes == NULL) {
        return NULL;
    }
    if (PyBytes_AsStringAndSize(bytes, &buffer, &length) == -1) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    if (!length) {
        Py_DECREF(bytes);
        return MPZ_from_i64(0);
    }

    MPZ_Object *res = MPZ_new(1 + length/2, 0);

    if (!res) {
        /* LCOV_EXCL_START */
        Py_DECREF(bytes);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    if (is_little) {
        char *tmp = PyMem_Malloc(length);

        if (!tmp) {
            /* LCOV_EXCL_START */
            Py_DECREF(bytes);
            return (MPZ_Object *)PyErr_NoMemory();
            /* LCOV_EXCL_STOP */
        }
        memcpy(tmp, buffer, length);
        buffer = tmp;
        revstr(buffer, 0, length - 1);
    }
    SZ(res) = mpn_set_str(LS(res), (unsigned char *)buffer, length, 256);
    Py_DECREF(bytes);
    if (is_little) {
        PyMem_Free(buffer);
    }
    if (zz_resize(&res->z, SZ(res)) == MPZ_MEM) {
        /* LCOV_EXCL_START */
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
        /* LCOV_EXCL_STOP */
    }
    zz_normalize(&res->z);
    if (is_signed && mpn_sizeinbase(LS(res), SZ(res), 2) == 8*(size_t)length)
    {
        if (SZ(res) > 1) {
            mpn_sub_1(LS(res), LS(res), SZ(res), 1);
            mpn_com(LS(res), LS(res), SZ(res) - 1);
        }
        else {
            LS(res)[SZ(res) - 1] -= 1;
        }
        LS(res)[SZ(res) - 1] = ~LS(res)[SZ(res) - 1];

        mp_size_t shift = GMP_NUMB_BITS*SZ(res) - 8*length;

        LS(res)[SZ(res) - 1] <<= shift;
        LS(res)[SZ(res) - 1] >>= shift;
        ISNEG(res) = 1;
        zz_normalize(&res->z);
    }
    return res;
}

static MPZ_err
MPZ_gcd(const MPZ_Object *u, const MPZ_Object *v, MPZ_Object *gcd)
{
    ISNEG(gcd) = 0;
    if (!SZ(u)) {
        if (zz_resize(&gcd->z, SZ(v)) == MPZ_MEM) {
            return MPZ_MEM; /* LCOV_EXCL_LINE */
        }
        mpn_copyi(LS(gcd), LS(v), SZ(v));
        return MPZ_OK;
    }
    if (!SZ(v)) {
        if (zz_resize(&gcd->z, SZ(u)) == MPZ_MEM) {
            return MPZ_MEM; /* LCOV_EXCL_LINE */
        }
        mpn_copyi(LS(gcd), LS(u), SZ(u));
        return MPZ_OK;
    }

    mp_limb_t shift = Py_MIN(mpn_scan1(LS(u), 0), mpn_scan1(LS(v), 0));

    if (shift) {
        u = MPZ_rshift1(u, shift, 0);
        v = MPZ_rshift1(v, shift, 0);
    }
    else {
        u = MPZ_copy(u);
        v = MPZ_copy(v);
    }
    if (SZ(u) < SZ(v)) {
        SWAP(const MPZ_Object *, u, v);
    }
    if (!u || !v || zz_resize(&gcd->z, SZ(v)) == MPZ_MEM || TMP_OVERFLOW) {
        /* LCOV_EXCL_START */
        Py_XDECREF(u);
        Py_XDECREF(v);
        return MPZ_MEM;
        /* LCOV_EXCL_STOP */
    }
    SZ(gcd) = mpn_gcd(LS(gcd), LS(u), SZ(u), LS(v), SZ(v));
    Py_DECREF(u);
    Py_DECREF(v);
    if (shift) {
        MPZ_Object *tmp = MPZ_lshift1(gcd, shift, 0);

        if (!tmp) {
            return MPZ_MEM; /* LCOV_EXCL_LINE */
        }
        if (zz_resize(&gcd->z, SZ(tmp)) == MPZ_MEM) {
            /* LCOV_EXCL_START */
            Py_DECREF(tmp);
            return MPZ_MEM;
            /* LCOV_EXCL_STOP */
        }
        mpn_copyi(LS(gcd), LS(tmp), SZ(tmp));
        Py_DECREF(tmp);
    }
    return MPZ_OK;
}

static MPZ_err
MPZ_sqrtrem(const MPZ_Object *u, MPZ_Object *root, MPZ_Object *rem)
{
    if (ISNEG(u)) {
        return MPZ_VAL;
    }
    ISNEG(root) = 0;
    if (!SZ(u)) {
        SZ(root) = 0;
        if (rem) {
            SZ(rem) = 0;
            ISNEG(rem) = 0;
        }
        return MPZ_OK;
    }
    if (zz_resize(&root->z, (SZ(u) + 1)/2) == MPZ_MEM || TMP_OVERFLOW) {
        return MPZ_MEM; /* LCOV_EXCL_LINE */
    }
    if (rem) {
        ISNEG(rem) = 0;
        if (zz_resize(&rem->z, SZ(u)) == MPZ_MEM) {
            return MPZ_MEM; /* LCOV_EXCL_LINE */
        }
        SZ(rem) = mpn_sqrtrem(LS(root), LS(rem), LS(u), SZ(u));
    }
    else {
        mpn_sqrtrem(LS(root), NULL, LS(u), SZ(u));
    }
    return MPZ_OK;
}

#define MK_MPZ_func_ul(name, mpz_suff)                    \
    static MPZ_err                                        \
    MPZ_##name##_ul(const MPZ_Object *u, MPZ_Object *res) \
    {                                                     \
        TMP_MPZ(z, u)                                     \
        if (ISNEG(u)) {                                   \
            return MPZ_VAL;                               \
        }                                                 \
        if (!mpz_fits_ulong_p(z)) {                       \
            return MPZ_BUF;                               \
        }                                                 \
                                                          \
        unsigned long n = mpz_get_ui(z);                  \
                                                          \
        if (ENOUGH_MEMORY) {                              \
            mpz_init(z);                                  \
            mpz_##mpz_suff(z, n);                         \
        }                                                 \
        else {                                            \
            return MPZ_MEM; /* LCOV_EXCL_LINE */          \
        }                                                 \
        if (zz_resize(&res->z, z->_mp_size) == MPZ_MEM) { \
            /* LCOV_EXCL_START */                         \
            mpz_clear(z);                                 \
            return MPZ_MEM;                               \
            /* LCOV_EXCL_STOP */                          \
        }                                                 \
        mpn_copyi(LS(res), z->_mp_d, z->_mp_size);        \
        mpz_clear(z);                                     \
        return MPZ_OK;                                    \
    }

MK_MPZ_func_ul(fac, fac_ui)
MK_MPZ_func_ul(double_fac, 2fac_ui)
MK_MPZ_func_ul(fib, fib_ui)

#define MPZ_Check(u) PyObject_TypeCheck((u), &MPZ_Type)

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
        if (zz_init(&newobj->z) || zz_resize(&newobj->z, n)) {
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
        return (PyObject *)MPZ_from_i64(0);
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
        return (PyObject *)MPZ_from_i64(0);
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

static PyObject *
to_float(PyObject *self);

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

static PyObject *
plus(PyObject *self)
{
    return (PyObject *)MPZ_copy((MPZ_Object *)self);
}

static PyObject *
minus(PyObject *self)
{
    MPZ_Object *u = (MPZ_Object *)self;
    ;
    MPZ_Object *res = MPZ_new(0, 0);

    if (res && zz_neg(&u->z, &res->z)) {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    return (PyObject *)res;
}

static PyObject *
absolute(PyObject *self)
{
    return (PyObject *)MPZ_abs((MPZ_Object *)self);
}

static PyObject *
to_int(PyObject *self)
{
    return MPZ_to_int((MPZ_Object *)self);
}

static PyObject *
invert(PyObject *self)
{
    return (PyObject *)MPZ_invert((MPZ_Object *)self);
}

static PyObject *
to_float(PyObject *self)
{
    Py_ssize_t exp;
    MPZ_Object *u = (MPZ_Object *)self;
    double d = MPZ_AsDoubleAndExp(u, &exp);

    d = ldexp(d, exp);
    if (exp > DBL_MAX_EXP || isinf(d)) {
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

#define BINOP_INT(suff)                        \
    static PyObject *                          \
    nb_##suff(PyObject *self, PyObject *other) \
    {                                          \
        PyObject *res = NULL;                  \
        MPZ_Object *u = NULL, *v = NULL;       \
                                               \
        CHECK_OP(u, self);                     \
        CHECK_OP(v, other);                    \
                                               \
        res = (PyObject *)MPZ_##suff(u, v);    \
    end:                                       \
        Py_XDECREF(u);                         \
        Py_XDECREF(v);                         \
        return res;                            \
    fallback:                                  \
    numbers:                                   \
        Py_XDECREF(u);                         \
        Py_XDECREF(v);                         \
        Py_RETURN_NOTIMPLEMENTED;              \
    }

#define BINOP(suff, slot)                      \
    static PyObject *                          \
    nb_##suff(PyObject *self, PyObject *other) \
    {                                          \
        PyObject *res = NULL;                  \
        MPZ_Object *u = NULL, *v = NULL;       \
                                               \
        CHECK_OP(u, self);                     \
        CHECK_OP(v, other);                    \
                                               \
        res = (PyObject *)MPZ_##suff(u, v);    \
    end:                                       \
        Py_XDECREF(u);                         \
        Py_XDECREF(v);                         \
        return res;                            \
    fallback:                                  \
        Py_XDECREF(u);                         \
        Py_XDECREF(v);                         \
        Py_RETURN_NOTIMPLEMENTED;              \
    numbers:                                   \
        Py_XDECREF(u);                         \
        Py_XDECREF(v);                         \
                                               \
        PyObject *uf, *vf;                     \
                                               \
        if (Number_Check(self)) {              \
            uf = self;                         \
            Py_INCREF(uf);                     \
        }                                      \
        else {                                 \
            uf = to_float(self);               \
            if (!uf) {                         \
                return NULL;                   \
            }                                  \
        }                                      \
        if (Number_Check(other)) {             \
            vf = other;                        \
            Py_INCREF(vf);                     \
        }                                      \
        else {                                 \
            vf = to_float(other);              \
            if (!vf) {                         \
                Py_DECREF(uf);                 \
                return NULL;                   \
            }                                  \
        }                                      \
        res = slot(uf, vf);                    \
        Py_DECREF(uf);                         \
        Py_DECREF(vf);                         \
        return res;                            \
    }

BINOP(add, PyNumber_Add)
BINOP(sub, PyNumber_Subtract)
BINOP(mul, PyNumber_Multiply)

static PyObject *
divmod(PyObject *self, PyObject *other)
{
    PyObject *res = PyTuple_New(2);
    MPZ_Object *u = NULL, *v = NULL;

    if (!res) {
        return NULL; /* LCOV_EXCL_LINE */
    }
    CHECK_OP(u, self);
    CHECK_OP(v, other);

    MPZ_Object *q, *r;

    if (MPZ_divmod(&q, &r, u, v)) {
        goto end; /* LCOV_EXCL_LINE */
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

BINOP(quo, PyNumber_FloorDivide)
BINOP(rem, PyNumber_Remainder)
BINOP(truediv, PyNumber_TrueDivide)
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
        res = MPZ_pow(u, v);
        Py_DECREF(u);
        Py_DECREF(v);
        if (!res) {
            PyErr_SetNone(PyExc_MemoryError); /* LCOV_EXCL_LINE */
        }
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
        if (!SZ(w)) {
            PyErr_SetString(PyExc_ValueError,
                            "pow() 3rd argument cannot be 0");
            Py_DECREF(w);
            goto end;
        }

        int negativeOutput = 0;

        if (ISNEG(w)) {
            MPZ_Object *tmp = MPZ_copy(w);

            if (!tmp) {
                goto end3; /* LCOV_EXCL_LINE */
            }
            negativeOutput = 1;
            ISNEG(tmp) = 0;
            Py_SETREF(w, tmp);
        }
        if (ISNEG(v)) {
            MPZ_Object *tmp = MPZ_copy(v);

            if (!tmp) {
                goto end3; /* LCOV_EXCL_LINE */
            }
            ISNEG(tmp) = 0;
            Py_SETREF(v, tmp);

            tmp = MPZ_inverse(u, w);
            if (!tmp) {
                goto end3; /* LCOV_EXCL_LINE */
            }
            Py_SETREF(u, tmp);
        }
        if (ISNEG(u) || SZ(u) > SZ(w)) {
            MPZ_Object *tmp = MPZ_rem(u, w);

            if (!tmp) {
                goto end3; /* LCOV_EXCL_LINE */
            }
            Py_SETREF(u, tmp);
        }
        if (SZ(w) == 1 && LS(w)[0] == 1) {
            res = MPZ_from_i64(0);
        }
        else if (!SZ(v)) {
            res = MPZ_from_i64(1);
        }
        else if (!SZ(u)) {
            res = MPZ_from_i64(0);
        }
        else if (SZ(u) == 1 && LS(u)[0] == 1) {
            res = MPZ_from_i64(1);
        }
        else {
            res = MPZ_powm(u, v, w);
        }
        if (negativeOutput && res && SZ(res)) {
            MPZ_Object *tmp = res;

            res = MPZ_sub(res, w);
            Py_DECREF(tmp);
        }
    end3:
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
    .nb_divmod = divmod,
    .nb_floor_divide = nb_quo,
    .nb_true_divide = nb_truediv,
    .nb_remainder = nb_rem,
    .nb_power = power,
    .nb_positive = plus,
    .nb_negative = minus,
    .nb_absolute = absolute,
    .nb_invert = invert,
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
    return (PyObject *)MPZ_from_i64(1);
}

static PyObject *
get_zero(PyObject *Py_UNUSED(self), void *Py_UNUSED(closure))
{
    return (PyObject *)MPZ_from_i64(0);
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
    mp_limb_t digit = SZ(u) ? mpn_sizeinbase(LS(u), SZ(u), 2) : 0;

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
    PyObject *one = (PyObject *)MPZ_from_i64(1);

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

    PyObject *ten = (PyObject *)MPZ_from_i64(10);

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

    MPZ_Object *q, *r;

    if (MPZ_divmod_near(&q, &r, u, (MPZ_Object *)p) == -1) {
        /* LCOV_EXCL_START */
        Py_DECREF(p);
        return NULL;
        /* LCOV_EXCL_STOP */
    }
    Py_DECREF(p);
    Py_DECREF(q);

    PyObject *res = (PyObject *)MPZ_sub(u, r);

    Py_DECREF(r);
    return res;
}

static PyObject *from_bytes_func;

static PyObject *
__reduce_ex__(PyObject *self, PyObject *Py_UNUSED(args))
{
    MPZ_Object *u = (MPZ_Object *)self;
    Py_ssize_t len = SZ(u) ? mpn_sizeinbase(LS(u), SZ(u), 2) : 1;

    return Py_BuildValue("O(N)", from_bytes_func,
                         MPZ_to_bytes(u, (len + 7)/8 + 1, 0, 1));
}

/* XXX: replace this stub */
static PyObject *
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
    MPZ_Object *res = MPZ_from_i64(0);

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
        if (SZ(res) == 1 && LS(res)[0] == 1) {
            Py_DECREF(arg);
            continue;
        }

        MPZ_Object *tmp = MPZ_new(0, 0);

        if (!tmp || MPZ_gcd(res, arg, tmp) == MPZ_MEM) {
            /* LCOV_EXCL_START */
            Py_DECREF(res);
            Py_DECREF(arg);
            return PyErr_NoMemory();
            /* LCOV_EXCL_STOP */
        }
        Py_DECREF(arg);
        Py_SETREF(res, tmp);
    }
    if (zz_resize(&res->z, SZ(res)) == MPZ_MEM) {
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

    MPZ_err ret = MPZ_gcdext(x, y, g, s, t);

    Py_XDECREF(x);
    Py_XDECREF(y);
    if (ret == MPZ_MEM) {
        PyErr_NoMemory(); /* LCOV_EXCL_LINE */
    }
    if (ret == MPZ_OK) {
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

    MPZ_err ret = MPZ_sqrtrem(x, root, NULL);

    Py_DECREF(x);
    if (ret == MPZ_OK) {
        return (PyObject *)root;
    }
    if (ret == MPZ_VAL) {
        PyErr_SetString(PyExc_ValueError,
                        "isqrt() argument must be nonnegative");
    }
    if (ret == MPZ_MEM) {
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

    MPZ_err ret = MPZ_sqrtrem(x, root, rem);

    Py_DECREF(x);
    if (ret == MPZ_OK) {
        tup = PyTuple_Pack(2, root, rem);
    }
    if (ret == MPZ_VAL) {
        PyErr_SetString(PyExc_ValueError,
                        "isqrt() argument must be nonnegative");
    }
    if (ret == MPZ_MEM) {
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
                                                                         \
        MPZ_err ret = MPZ_##name##_ul(x, res);                           \
                                                                         \
        Py_XDECREF(x);                                                   \
        if (ret == MPZ_VAL) {                                            \
            PyErr_SetString(PyExc_ValueError,                            \
                            #name "() not defined for negative values"); \
            goto err;                                                    \
        }                                                                \
        if (ret == MPZ_BUF) {                                            \
            PyErr_Format(PyExc_OverflowError,                            \
                         #name "() argument should not exceed %ld",      \
                         LONG_MAX);                                      \
            goto err;                                                    \
        }                                                                \
        if (ret == MPZ_MEM) {                                            \
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
MAKE_MPZ_UI_FUN(double_fac)
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
                if (sign) {
                    res = MPZ_rshift1(man, shift, 1);
                    ISNEG(res) = 0;
                }
                else {
                    res = MPZ_rshift1(man, shift, 0);
                }
                break;
            case (Py_UCS4)'c':
                if (sign) {
                    res = MPZ_rshift1(man, shift, 0);
                }
                else {
                    res = MPZ_rshift1(man, shift, 1);
                    ISNEG(res) = 0;
                }
                break;
            case (Py_UCS4)'d':
                res = MPZ_rshift1(man, shift, 0);
                break;
            case (Py_UCS4)'u':
                res = MPZ_rshift1(man, shift, 1);
                ISNEG(res) = 0;
                break;
            case (Py_UCS4)'n':
            default:
                res = MPZ_rshift1(man, shift - 1, 0);

                int t = (LS(res)[0]&1 && (LS(res)[0]&2
                         || mpn_scan1(LS(man), 0) + 2 <= shift));

                mpn_rshift(LS(res), LS(res), SZ(res), 1);
                if (t) {
                    mpn_add_1(LS(res), LS(res), SZ(res), 1);
                }
                zz_normalize(&res->z);
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
        res = MPZ_copy(man);
    }
    /* Strip trailing 0 bits. */
    if (SZ(res) && (zbits = mpn_scan1(LS(res), 0))) {
        tmp = (PyObject *)MPZ_rshift1(res, zbits, 0);
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
    if (SZ(res) == 1 && LS(res)[0] == 1) {
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
    if (!MPZ_Check(args[0])) {
        PyErr_Format(PyExc_TypeError, "_mpmath_create() expects mpz");
        return NULL;
    }

    MPZ_Object *man = MPZ_copy((MPZ_Object *)args[0]);
    PyObject *exp = args[1];
    long sign = ISNEG(man);

    if (sign) {
        ISNEG(man) = 0;
    }

    mp_bitcnt_t bc = SZ(man) ? mpn_sizeinbase(LS(man), SZ(man), 2) : 0;
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
            tmp = (PyObject *)MPZ_rshift1(man, zbits, 0);
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
    {"fac", gmp_fac, METH_O,
     ("fac($module, n, /)\n--\n\n"
      "Find n!.\n\nRaise a ValueError if n is negative or non-integral.")},
    {"double_fac", gmp_double_fac, METH_O,
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
    mp_set_memory_functions(gmp_allocate_function, gmp_reallocate_function,
                            gmp_free_function);
#if !defined(PYPY_VERSION) && !defined(GRAALVM_PYTHON)
    /* Query parameters of Python’s internal representation of integers. */
    const PyLongLayout *layout = PyLong_GetNativeLayout();

    int_digit_size = layout->digit_size;
    int_digits_order = layout->digits_order;
    int_bits_per_digit = layout->bits_per_digit;
    int_nails = int_digit_size*8 - int_bits_per_digit;
    int_endianness = layout->digit_endianness;
#endif
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

    const char *str = ("numbers.Integral.register(gmp.mpz)\n"
                       "import fractions\n"
                       "class mpq(fractions.Fraction):\n"
                       "    '''Subclass of the fractions.Fraction.'''\n"
                       "    def __new__(cls, numerator=0, denominator=None):\n"
                       "        self = super(mpq, cls).__new__(cls, numerator, denominator)\n"
                       "        self._numerator = gmp.mpz(self._numerator)\n"
                       "        self._denominator = gmp.mpz(self._denominator)\n"
                       "        return self\n"
                       "\n"
                       "gmp.mpq = mpq\n"
                       "gmp.mpq.__module__ = 'gmp'\n"
                       "gmp.mpq.__new__.__module__ = 'gmp'\n"
                       "del fractions\n");

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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
static PyModuleDef_Slot gmp_slots[] = {
    {Py_mod_exec, gmp_exec},
#  if PY_VERSION_HEX >= 0x030C0000
    {Py_mod_multiple_interpreters,
     Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED},
#  endif
#  if PY_VERSION_HEX >= 0x030D0000
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#  endif
    {0, NULL}
};
#pragma GCC diagnostic pop

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
