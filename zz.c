#include "zz.h"

#include <ctype.h>
#include <float.h>
#include <gmp.h>
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

jmp_buf gmp_env;
#define TMP_OVERFLOW (setjmp(gmp_env) == 1)

mp_err
zz_init(zz_t *u)
{
    u->negative = false;
    u->alloc = 0;
    u->size = 0;
    u->digits = NULL;
    return MP_OK;
}

mp_err
zz_resize(mp_size_t size, zz_t *u)
{
    if (u->alloc >= size) {
        u->size = size;
        return MP_OK;
    }

    mp_size_t alloc = size;
    mp_limb_t *t = u->digits;

    if (!alloc) {
        alloc = 1;
    }
    u->digits = realloc(u->digits, alloc * sizeof(mp_limb_t));
    if (u->digits) {
        u->alloc = alloc;
        u->size = size;
        return MP_OK;
    }
    u->digits = t;
    return MP_MEM;
}

void
zz_clear(zz_t *u)
{
    free(u->digits);
    u->negative = false;
    u->alloc = 0;
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
        u->negative = false;
    }
}

mp_ord
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

#define ABS(a) ((a) < 0 ? (-a) : (a))

mp_ord
zz_cmp_i32(const zz_t *u, int32_t v)
{
    mp_ord sign = u->negative ? MP_LT : MP_GT;
    bool v_negative = v < 0;

    if (u->negative != v_negative) {
        return sign;
    }
    else if (u->size != 1) {
        return (u->size < 1) ? -sign : sign;
    }

    mp_limb_t digit = ABS(v);
    mp_ord r = u->digits[0] != digit;

    if (u->digits[0] < digit) {
        r = MP_LT;
    }
    else if (u->digits[0] > digit) {
        r = MP_GT;
    }
    return u->negative ? -r : r;
}

mp_err
zz_from_i32(int32_t u, zz_t *v)
{
    if (!u) {
        v->size = 0;
        v->negative = false;
        return MP_OK;
    }
    if (zz_resize(1, v)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    v->negative = u < 0;
    v->digits[0] = v->negative ? -u : u;
    return MP_OK;
}

mp_err
zz_to_i32(const zz_t *u, int32_t *v)
{
    mp_size_t n = u->size;

    if (!n) {
        *v = 0;
        return MP_OK;
    }
    if (n > 1) {
        return MP_VAL;
    }

    uint32_t uv = u->digits[0];

    if (u->negative) {
        if (uv <= INT32_MAX + 1UL) {
            *v = -1 - (int32_t)((uv - 1) & INT32_MAX);
            return MP_OK;
        }
    }
    else {
        if (uv <= INT32_MAX) {
            *v = (int32_t)uv;
            return MP_OK;
        }
    }
    return MP_VAL;
}

mp_err
zz_from_i64(int64_t u, zz_t *v)
{
    if (!u) {
        v->size = 0;
        v->negative = false;
        return MP_OK;
    }

    bool negative = u < 0;
    uint64_t uv = (negative ? -((uint64_t)(u + 1) - 1) : (uint64_t)(u));
#if GMP_NUMB_BITS < 64
    mp_size_t size = 1 + (uv > GMP_NUMB_MAX);
#else
    mp_size_t size = 1;
#endif

    if (zz_resize(size, v)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    v->negative = negative;
    v->digits[0] = uv & GMP_NUMB_MASK;
#if GMP_NUMB_BITS < 64
    if (size == 2) {
        v->digits[1] = uv >> GMP_NUMB_BITS;
    }
#endif
    return MP_OK;
}

mp_err
zz_to_i64(const zz_t *u, int64_t *v)
{
    mp_size_t n = u->size;

    if (!n) {
        *v = 0;
        return MP_OK;
    }
    if (n > 2) {
        return MP_VAL;
    }

    uint64_t uv = u->digits[0];

#if GMP_NUMB_BITS < 64
    if (n == 2) {
        if (u->digits[1] >> GMP_NAIL_BITS) {
            return MP_VAL;
        }
        uv += u->digits[1] << GMP_NUMB_BITS;
    }
#else
    if (n > 1) {
        return MP_VAL;
    }
#endif
    if (u->negative) {
        if (uv <= INT64_MAX + 1ULL) {
            *v = -1 - (int64_t)((uv - 1) & INT64_MAX);
            return MP_OK;
        }
    }
    else {
        if (uv <= INT64_MAX) {
            *v = (int64_t)uv;
            return MP_OK;
        }
    }
    return MP_VAL;
}

mp_err
zz_copy(const zz_t *u, zz_t *v)
{
    if (u != v) {
        if (!u->size) {
            return zz_from_i32(0, v);
        }
        if (zz_resize(u->size, v)) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        v->negative = u->negative;
        mpn_copyi(v->digits, u->digits, u->size);
    }
    return MP_OK;
}

mp_err
zz_abs(const zz_t *u, zz_t *v)
{
    if (u != v && zz_copy(u, v)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    v->negative = false;
    return MP_OK;
}

mp_err
zz_neg(const zz_t *u, zz_t *v)
{
    if (u != v && zz_copy(u, v)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    if (v->size) {
        v->negative = !u->negative;
    }
    return MP_OK;
}

mp_err
zz_sizeinbase(const zz_t *u, int8_t base, size_t *len)
{
    if (base < 2 || base > 36) {
        return MP_VAL;
    }
    *len = mpn_sizeinbase(u->digits, u->size, base) + u->negative;
    return MP_OK;
}

mp_err
zz_to_str(const zz_t *u, int8_t base, int8_t *str, size_t *len)
{
    /* Maps 1-byte integer to digit character for bases up to 36. */
    const char *NUM_TO_TEXT = "0123456789abcdefghijklmnopqrstuvwxyz";

    if (base < 0) {
        base = -base;
        NUM_TO_TEXT = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    }
    if (base < 2 || base > 36) {
        return MP_VAL;
    }

    uint8_t *p = (uint8_t *)str;

    if (u->negative) {
        *(p++) = '-';
    }
    if ((base & (base - 1)) == 0) {
        *len = mpn_get_str(p, base, u->digits, u->size);
    }
    else { /* generic base, not power of 2, input might be clobbered */
        mp_limb_t *volatile tmp = malloc(sizeof(mp_limb_t) * u->alloc);

        if (!tmp || TMP_OVERFLOW) {
            /* LCOV_EXCL_START */
            free(tmp);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }
        mpn_copyi(tmp, u->digits, u->size);
        *len = mpn_get_str(p, base, tmp, u->size);
        free(tmp);
    }
    for (size_t i = 0; i < *len; i++) {
        *p = NUM_TO_TEXT[*p];
        p++;
    }
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

mp_err
zz_from_str(const int8_t *str, size_t len, int8_t base, zz_t *u)
{
    if (base < 2 || base > 36) {
        return MP_VAL;
    }

    uint8_t *volatile buf = malloc(len), *p = buf;

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
    if (!len) {
        goto err;
    }
    if (p[0] == '_') {
        goto err;
    }

    const uint8_t *digit_value = DIGIT_VALUE_TAB;
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
    if (zz_resize(1 + len/2, u) || TMP_OVERFLOW) {
        /* LCOV_EXCL_START */
        free(buf);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }
    u->negative = negative;
    u->size = mpn_set_str(u->digits, p, len, base);
    free(buf);
    if (zz_resize(u->size, u) == MP_MEM) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    zz_normalize(u);
    return MP_OK;
err:
    free(buf);
    return MP_VAL;
}

static mp_err
_zz_to_double(const zz_t *u, mp_size_t shift, double *d)
{
    mp_limb_t high = 1ULL << DBL_MANT_DIG;
    mp_limb_t man = 0, carry, left;
    mp_size_t us = u->size, i, bits = 0, e = 0;

    if (!us) {
        man = 0;
        goto done;
    }
    man = u->digits[us - 1];
    if (man >= high) {
        while ((man >> bits) >= high) {
            bits++;
        }
        left = 1ULL << (bits - 1);
        carry = man & (2*left - 1);
        man >>= bits;
        i = us - 1;
        e = (us - 1)*GMP_NUMB_BITS + DBL_MANT_DIG + bits;
    }
    else {
        while (!((man << 1) & high)) {
            man <<= 1;
            bits++;
        }
        i = us - 1;
        e = (us - 1)*GMP_NUMB_BITS + DBL_MANT_DIG - bits;
        for (i = us - 1; i && bits >= GMP_NUMB_BITS;) {
            bits -= GMP_NUMB_BITS;
            man += u->digits[--i] << bits;
        }
        if (i == 0) {
            goto done;
        }
        if (bits) {
            bits = GMP_NUMB_BITS - bits;
            left = 1ULL << (bits - 1);
            man += u->digits[i - 1] >> bits;
            carry = u->digits[i - 1] & (2*left - 1);
            i--;
        }
        else {
            left = 1ULL << (GMP_NUMB_BITS - 1);
            carry = u->digits[i - 1];
            i--;
        }
    }
    if (carry > left) {
        man++;
    }
    else if (carry == left) {
        if (man%2 == 1) {
            man++;
        }
        else {
            mp_size_t j;

            for (j = 0; j < i; j++) {
                if (u->digits[j]) {
                    break;
                }
            }
            if (i != j) {
                man++;
            }
        }
    }
done:
    *d = ldexp(man, -DBL_MANT_DIG);
    if (u->negative) {
        *d = -*d;
    }
    *d = ldexp(*d, e - shift);
    if (e > DBL_MAX_EXP || isinf(*d)) {
        return MP_BUF;
    }
    return MP_OK;
}

mp_err
zz_to_double(const zz_t *u, double *d)
{
    return _zz_to_double(u, 0, d);
}

mp_err
zz_to_bytes(const zz_t *u, size_t length, bool is_signed, uint8_t **buffer)
{
    zz_t tmp;
    bool is_negative = u->negative;

    if (zz_init(&tmp)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    if (is_negative) {
        if (!is_signed) {
            return MP_BUF;
        }
        if (zz_resize(8*length/GMP_NUMB_BITS + 1, &tmp)) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        if (tmp.size < u->size) {
            goto overflow;
        }
        mpn_zero(tmp.digits, tmp.size);
        tmp.digits[tmp.size - 1] = 1;
        tmp.digits[tmp.size - 1] <<= (8*length) % (GMP_NUMB_BITS*tmp.size);
        mpn_sub(tmp.digits, tmp.digits, tmp.size, u->digits, u->size);
        zz_normalize(&tmp);
        u = &tmp;
    }

    size_t nbits = u->size ? mpn_sizeinbase(u->digits, u->size, 2) : 0;

    if (nbits > 8*length
        || (is_signed && nbits
            && (nbits == 8 * length ? !is_negative : is_negative)))
    {
overflow:
        zz_clear(&tmp);
        return MP_BUF;
    }

    size_t gap = length - (nbits + GMP_NUMB_BITS/8 - 1)/(GMP_NUMB_BITS/8);

    if (u->size) {
        mpn_get_str(*buffer + gap, 256, u->digits, u->size);
    }
    memset(*buffer, is_negative ? 0xFF : 0, gap);
    zz_clear(&tmp);
    return MP_OK;
}

mp_err
zz_from_bytes(const uint8_t *buffer, size_t length, bool is_signed, zz_t *u)
{
    if (!length) {
        return zz_from_i32(0, u);
    }
    if (zz_resize(1 + length/2, u)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    u->size = mpn_set_str(u->digits, buffer, length, 256);
    if (zz_resize(u->size, u) == MP_MEM) {
        /* LCOV_EXCL_START */
        zz_clear(u);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }
    zz_normalize(u);
    if (is_signed
        && mpn_sizeinbase(u->digits, u->size, 2) == 8*(size_t)length)
    {
        if (u->size > 1) {
            mpn_sub_1(u->digits, u->digits, u->size, 1);
            mpn_com(u->digits, u->digits, u->size - 1);
        }
        else {
            u->digits[u->size - 1] -= 1;
        }
        u->digits[u->size - 1] = ~u->digits[u->size - 1];

        mp_size_t shift = GMP_NUMB_BITS*u->size - 8*length;

        u->digits[u->size - 1] <<= shift;
        u->digits[u->size - 1] >>= shift;
        u->negative = true;
        zz_normalize(u);
    }
    return MP_OK;
}

size_t
zz_bitlen(const zz_t *u)
{
    return u->size ? mpn_sizeinbase(u->digits, u->size, 2) : 0;
}

mp_bitcnt_t
zz_scan1(const zz_t *u, mp_bitcnt_t bit)
{
    if (!u->size || u->negative) {
        return ~(mp_bitcnt_t)0; /* XXX */
    }
    else {
        return mpn_scan1(u->digits, bit);
    }
}

mp_bitcnt_t
zz_bitcnt(const zz_t *u)
{
    return u->size ? mpn_popcount(u->digits, u->size) : 0;
}

#define TMP_ZZ(z, u)                                \
    mpz_t z;                                        \
                                                    \
    z->_mp_d = u->digits;                           \
    z->_mp_size = (u->negative ? -1 : 1) * u->size; \
    z->_mp_alloc = u->alloc;

#define BITS_TO_LIMBS(n) (((n) + (GMP_NUMB_BITS - 1))/GMP_NUMB_BITS)

mp_err
zz_import(size_t len, const void *digits, mp_layout layout, zz_t *u)
{
    mp_size_t size = BITS_TO_LIMBS(len * layout.bits_per_digit);

    if (zz_resize(size, u)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }

    TMP_ZZ(z, u);
    mpz_import(z, len, layout.digits_order, layout.digit_size,
               layout.digit_endianness,
               layout.digit_size*8 - layout.bits_per_digit, digits);
    u->size = z->_mp_size;
    return MP_OK;
}

#define MAX(a, b) ((a) > (b) ? (a) : (b))

mp_err
zz_export(const zz_t *u, mp_layout layout, size_t len, void *digits)
{
    if (len < MAX((zz_bitlen(u) + layout.bits_per_digit
                   - 1)/layout.bits_per_digit, 1))
    {
        return MP_VAL;
    }

    TMP_ZZ(z, u);
    mpz_export(digits, NULL, layout.digits_order, layout.digit_size,
               layout.digit_endianness,
               layout.digit_size*8 - layout.bits_per_digit, z);
    return MP_OK;
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
    mp_size_t u_size = u->size, v_size = v->size;

    if (u_size < v_size) {
        SWAP(const zz_t *, u, v);
        SWAP(bool, negu, negv);
        SWAP(mp_size_t, u_size, v_size);
    }

    if (zz_resize(u_size + same_sign, w) || TMP_OVERFLOW) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    w->negative = negu;
    if (same_sign) {
        w->digits[w->size - 1] = mpn_add(w->digits, u->digits, u_size,
                                         v->digits, v_size);
    }
    else if (u_size != v_size) {
        mpn_sub(w->digits, u->digits, u_size, v->digits, v_size);
    }
    else {
        int cmp = mpn_cmp(u->digits, v->digits, u_size);

        if (cmp < 0) {
            mpn_sub_n(w->digits, v->digits, u->digits, u->size);
            w->negative = negv;
        }
        else if (cmp > 0) {
            mpn_sub_n(w->digits, u->digits, v->digits, u_size);
        }
        else {
            w->size = 0;
        }
    }
    zz_normalize(w);
    return MP_OK;
}

static mp_err
_zz_addsub_i32(const zz_t *u, int32_t v, bool subtract, zz_t *w)
{
    bool negu = u->negative, negv = subtract ? v >= 0 : v < 0;
    bool same_sign = negu == negv;
    mp_size_t u_size = u->size, v_size = v != 0;
    mp_limb_t digit = ABS(v);

    if (u_size < v_size) {
        if (zz_resize(v_size, w)) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        if (v_size) {
            w->digits[0] = digit;
            w->negative = negv;
        }
        else {
            w->negative = false;
        }
    }

    if (zz_resize(u_size + same_sign, w) || TMP_OVERFLOW) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    w->negative = negu;
    if (same_sign) {
        w->digits[w->size - 1] = mpn_add_1(w->digits, u->digits, u_size, digit);
    }
    else {
        mpn_sub_1(w->digits, u->digits, u_size, digit);
    }
    zz_normalize(w);
    return MP_OK;
}

mp_err
zz_add(const zz_t *u, const zz_t *v, zz_t *w)
{
    return _zz_addsub(u, v, false, w);
}

mp_err
zz_sub(const zz_t *u, const zz_t *v, zz_t *w)
{
    return _zz_addsub(u, v, true, w);
}

mp_err
zz_add_i32(const zz_t *u, int32_t v, zz_t *w)
{
    return _zz_addsub_i32(u, v, false, w);
}

mp_err
zz_sub_i32(const zz_t *u, int32_t v, zz_t *w)
{
    return _zz_addsub_i32(u, v, true, w);
}

mp_err
zz_mul(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (u->size < v->size) {
        SWAP(const zz_t *, u, v);
    }
    if (!v->size) {
        return zz_from_i32(0, w);
    }
    if (u == w) {
        zz_t tmp;

        if (zz_init(&tmp) || zz_copy(u, &tmp)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }

        mp_err ret = zz_mul(&tmp, v, w);

        zz_clear(&tmp);
        return ret;
    }
    if (v == w) {
        zz_t tmp;

        if (zz_init(&tmp) || zz_copy(v, &tmp)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }

        mp_err ret = zz_mul(u, &tmp, w);

        zz_clear(&tmp);
        return ret;
    }
    if (zz_resize(u->size + v->size, w) || TMP_OVERFLOW) {
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

mp_err
zz_div(const zz_t *u, const zz_t *v, mp_rnd rnd, zz_t *q, zz_t *r)
{
    if (!v->size) {
        return MP_VAL;
    }
    if (!q || !r) {
        if (!q) {
            zz_t tmp;

            if (zz_init(&tmp)) {
                return MP_MEM; /* LCOV_EXCL_LINE */
            }

            mp_err ret = zz_div(u, v, rnd, &tmp, r);

            zz_clear(&tmp);
            return ret;
        }
        else {
            zz_t tmp;

            if (zz_init(&tmp)) {
                return MP_MEM; /* LCOV_EXCL_LINE */
            }

            mp_err ret = zz_div(u, v, rnd, q, &tmp);

            zz_clear(&tmp);
            return ret;
        }
    }
    if (!u->size) {
        if (zz_from_i32(0, q) || zz_from_i32(0, r)) {
            goto err; /* LCOV_EXCL_LINE */
        }
    }
    else if (u->size < v->size) {
        if (u->negative != v->negative) {
            if (zz_from_i32(-1, q) || zz_add(u, v, r)) {
                goto err; /* LCOV_EXCL_LINE */
            }
        }
        else {
            if (zz_from_i32(0, q) || zz_copy(u, r)) {
                goto err; /* LCOV_EXCL_LINE */
            }
        }
    }
    else {
        if (u == q) {
            zz_t tmp;

            if (zz_init(&tmp) || zz_copy(u, &tmp)) {
                /* LCOV_EXCL_START */
                zz_clear(&tmp);
                return MP_MEM;
                /* LCOV_EXCL_STOP */
            }

            mp_err ret = zz_div(&tmp, v, rnd, q, r);

            zz_clear(&tmp);
            return ret;
        }
        if (v == q || v == r) {
            zz_t tmp;

            if (zz_init(&tmp) || zz_copy(v, &tmp)) {
                /* LCOV_EXCL_START */
                zz_clear(&tmp);
                return MP_MEM;
                /* LCOV_EXCL_STOP */
            }

            mp_err ret = zz_div(u, &tmp, rnd, q, r);

            zz_clear(&tmp);
            return ret;
        }

        bool q_negative = (u->negative != v->negative);
        mp_size_t u_size = u->size;

        if (zz_resize(u_size - v->size + 1 + q_negative, q)
            || zz_resize(v->size, r) || TMP_OVERFLOW)
        {
            goto err; /* LCOV_EXCL_LINE */
        }
        q->negative = q_negative;
        if (q_negative) {
            q->digits[q->size - 1] = 0;
        }
        r->negative = v->negative;
        mpn_tdiv_qr(q->digits, r->digits, 0, u->digits, u_size, v->digits,
                    v->size);
        zz_normalize(r);
        if (q_negative && r->size) {
            r->size = v->size;
            mpn_sub_n(r->digits, v->digits, r->digits, v->size);
            mpn_add_1(q->digits, q->digits, q->size, 1);
        }
        zz_normalize(q);
        zz_normalize(r);
    }
    switch (rnd) {
        case MP_RNDD:
            return MP_OK;
        case MP_RNDN:
        {
            mp_ord unexpect = v->negative ? MP_LT : MP_GT;
            zz_t halfQ;

            if (zz_init(&halfQ) || zz_quo_2exp(v, 1, &halfQ)) {
                /* LCOV_EXCL_START */
                zz_clear(&halfQ);
                goto err;
                /* LCOV_EXCL_STOP */
            }

            mp_ord cmp = zz_cmp(r, &halfQ);

            zz_clear(&halfQ);
            if (cmp == MP_EQ && v->digits[0]%2 == 0 && q->size
                && q->digits[0]%2 != 0)
            {
                cmp = unexpect;
            }
            if (cmp == unexpect && (zz_add_i32(q, 1, q) || zz_sub(r, v, r))) {
                goto err; /* LCOV_EXCL_LINE */
            }
            return MP_OK;
        }
        default:
            return MP_VAL;
    }
    /* LCOV_EXCL_START */
err:
    zz_clear(q);
    zz_clear(r);
    return MP_MEM;
    /* LCOV_EXCL_STOP */
}

mp_err
zz_quo_2exp(const zz_t *u, uint64_t shift, zz_t *v)
{
    if (!u->size) {
        v->size = 0;
        return MP_OK;
    }

    mp_size_t whole = shift / GMP_NUMB_BITS;
    mp_size_t size = u->size;

    shift %= GMP_NUMB_BITS;
    if (whole >= size) {
        return zz_from_i32(u->negative ? -1 : 0, v);
    }
    size -= whole;

    bool carry = 0, extra = 1;

    for (mp_size_t i = 0; i < whole; i++) {
        if (u->digits[i]) {
            carry = u->negative;
            break;
        }
    }
    for (mp_size_t i = whole; i < u->size; i++) {
        if (u->digits[i] != GMP_NUMB_MAX) {
            extra = 0;
            break;
        }
    }
    if (zz_resize(size + extra, v)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    v->negative = u->negative;
    if (extra) {
        v->digits[size] = 0;
    }
    if (shift) {
        if (mpn_rshift(v->digits, u->digits + whole, size, shift)) {
            carry = u->negative;
        }
    }
    else {
        mpn_copyi(v->digits, u->digits + whole, size);
    }
    if (carry) {
        if (mpn_add_1(v->digits, v->digits, size, 1)) {
            v->digits[size] = 1;
        }
    }
    zz_normalize(v);
    return MP_OK;
}

mp_err
zz_mul_2exp(const zz_t *u, uint64_t shift, zz_t *v)
{
    if (!u->size) {
        v->size = 0;
        return MP_OK;
    }

    mp_size_t whole = shift / GMP_NUMB_BITS;
    mp_size_t u_size = u->size, v_size = u_size + whole;

    shift %= GMP_NUMB_BITS;
    if (zz_resize(v_size + (bool)shift, v)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    v->negative = u->negative;
    if (shift) {
        v->size -= !(bool)(v->digits[v_size] = mpn_lshift(v->digits + whole,
                                                          u->digits, u_size,
                                                          shift));
    }
    else {
        mpn_copyd(v->digits + whole, u->digits, u_size);
    }
    mpn_zero(v->digits, whole);
    return MP_OK;
}

mp_err
zz_truediv(const zz_t *u, const zz_t *v, double *res)
{
    if (!v->size) {
        return MP_VAL;
    }
    if (!u->size) {
        *res = v->negative ? -0.0 : 0.0;
        return MP_OK;
    }

    mp_size_t shift = (mpn_sizeinbase(v->digits, v->size, 2)
                       - mpn_sizeinbase(u->digits, u->size, 2));
    mp_size_t n = shift;
    zz_t *a = (zz_t *)u, *b = (zz_t *)v;

    if (shift < 0) {
        SWAP(zz_t *, a, b);
        n = -n;
    }

    mp_size_t whole = n / GMP_NUMB_BITS;

    n %= GMP_NUMB_BITS;
    for (mp_size_t i = b->size; i--;) {
        mp_limb_t da, db = b->digits[i];

        if (i >= whole) {
            if (i - whole < a->size) {
                da = a->digits[i - whole] << n;
            }
            else {
                da = 0;
            }
            if (n && i > whole) {
                da |= a->digits[i - whole - 1] >> (GMP_NUMB_BITS - n);
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

    zz_t tmp1, tmp2;

    if (zz_init(&tmp1) || zz_init(&tmp2)) {
        /* LCOV_EXCL_START */
tmp_clear:
        zz_clear(&tmp1);
        zz_clear(&tmp2);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }
    if (zz_abs(u, &tmp1)) {
        goto tmp_clear; /* LCOV_EXCL_LINE */
    }
    if (shift > 0 && zz_mul_2exp(&tmp1, shift, &tmp1)) {
        goto tmp_clear; /* LCOV_EXCL_LINE */
    }
    a = &tmp1;
    if (zz_abs(v, &tmp2)) {
        goto tmp_clear; /* LCOV_EXCL_LINE */
    }
    if (shift < 0 && zz_mul_2exp(&tmp2, -shift, &tmp2)) {
        goto tmp_clear; /* LCOV_EXCL_LINE */
    }
    b = &tmp2;

    zz_t c;

    if (zz_init(&c) || zz_div(a, b, MP_RNDN, &c, NULL)) {
        /* LCOV_EXCL_START */
        zz_clear(a);
        zz_clear(b);
        zz_clear(&c);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }
    zz_clear(a);
    zz_clear(b);

    mp_err ret = _zz_to_double(&c, shift, res);

    zz_clear(&c);
    if (u->negative != v->negative) {
        *res = -*res;
    }
    return ret;
}

mp_err
zz_invert(const zz_t *u, zz_t *v)
{
    mp_size_t u_size = u->size;

    if (u->negative) {
        if (zz_resize(u_size, v)) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        mpn_sub_1(v->digits, u->digits, u_size, 1);
        v->size -= v->digits[u_size - 1] == 0;
    }
    else if (!u_size) {
        return zz_from_i32(-1, v);
    }
    else {
        if (zz_resize(u_size + 1, v)) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        v->digits[u_size] = mpn_add_1(v->digits, u->digits, u_size, 1);
        v->size -= v->digits[u_size] == 0;
    }
    v->negative = !u->negative;
    return MP_OK;
}

mp_err
zz_and(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (!u->size || !v->size) {
        return zz_from_i32(0, w);
    }

    mp_size_t u_size = u->size, v_size = v->size;

    if (u->negative || v->negative) {
        zz_t o1, o2;

        if (zz_init(&o1) || zz_init(&o2)) {
            /* LCOV_EXCL_START */
err:
            zz_clear(&o1);
            zz_clear(&o2);
            /* LCOV_EXCL_STOP */
            return MP_MEM;
        }
        if (u->negative) {
            if (zz_invert(u, &o1)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            o1.negative = true;
            u = &o1;
            u_size = u->size;
        }
        if (v->negative) {
            if (zz_invert(v, &o2)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            o2.negative = true;
            v = &o2;
            v_size = v->size;
        }
        if (u_size < v_size) {
            SWAP(const zz_t *, u, v);
            SWAP(mp_size_t, u_size, v_size);
        }
        if (u->negative & v->negative) {
            if (!u_size) {
                zz_clear(&o1);
                zz_clear(&o2);
                return zz_from_i32(-1, w);
            }
            if (zz_resize(u_size + 1, w)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            w->negative = true;
            mpn_copyi(&w->digits[v_size], &u->digits[v_size], u_size - v_size);
            if (v_size) {
                mpn_ior_n(w->digits, u->digits, v->digits, v_size);
            }
            w->digits[u_size] = mpn_add_1(w->digits, w->digits, u_size, 1);
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
        else if (u->negative) {
            if (zz_resize(v_size, w)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            w->negative = false;
            mpn_andn_n(w->digits, v->digits, u->digits, v_size);
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
        else {
            if (zz_resize(u_size, w)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            w->negative = false;
            if (v_size) {
                mpn_andn_n(w->digits, u->digits, v->digits, v_size);
            }
            mpn_copyi(&w->digits[v_size], &u->digits[v_size], u_size - v_size);
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
    }
    if (u_size < v_size) {
        SWAP(const zz_t *, u, v);
        SWAP(mp_size_t, u_size, v_size);
    }
    if (zz_resize(v_size, w)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    w->negative = false;
    mpn_and_n(w->digits, u->digits, v->digits, v_size);
    zz_normalize(w);
    return MP_OK;
}

mp_err
zz_or(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (!u->size) {
        return zz_copy(v, w);
    }
    if (!v->size) {
        return zz_copy(u, w);
    }

    mp_size_t u_size = u->size, v_size = v->size;

    if (u->negative || v->negative) {
        zz_t o1, o2;

        if (zz_init(&o1) || zz_init(&o2)) {
            /* LCOV_EXCL_START */
err:
            zz_clear(&o1);
            zz_clear(&o2);
            /* LCOV_EXCL_STOP */
            return MP_MEM;
        }
        if (u->negative) {
            if (zz_invert(u, &o1)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            o1.negative = true;
            u = &o1;
        }
        if (v->negative) {
            if (zz_invert(v, &o2)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            o2.negative = true;
            v = &o2;
        }
        if (u_size < v_size) {
            SWAP(const zz_t *, u, v);
            SWAP(mp_size_t, u_size, v_size);
        }
        if (u->negative & v->negative) {
            if (!v_size) {
                zz_clear(&o1);
                zz_clear(&o2);
                return zz_from_i32(-1, w);
            }
            if (zz_resize(v_size + 1, w)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            w->negative = true;
            mpn_and_n(w->digits, u->digits, v->digits, v_size);
            w->digits[v_size] = mpn_add_1(w->digits, w->digits, v_size, 1);
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
        else if (u->negative) {
            if (zz_resize(u_size + 1, w)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            w->negative = true;
            mpn_copyi(&w->digits[v_size], &u->digits[v_size], u_size - v_size);
            mpn_andn_n(w->digits, u->digits, v->digits, v_size);
            w->digits[u_size] = mpn_add_1(w->digits, w->digits, u_size, 1);
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
        else {
            if (zz_resize(v_size + 1, w)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            w->negative = true;
            if (v_size) {
                mpn_andn_n(w->digits, v->digits, u->digits, v_size);
                w->digits[v_size] = mpn_add_1(w->digits, w->digits, v_size, 1);
                zz_normalize(w);
            }
            else {
                w->digits[0] = 1;
            }
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
    }
    if (u_size < v_size) {
        SWAP(const zz_t *, u, v);
        SWAP(mp_size_t, u_size, v_size);
    }
    if (zz_resize(u_size, w)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    w->negative = false;
    mpn_ior_n(w->digits, u->digits, v->digits, v_size);
    if (u_size != v_size) {
        mpn_copyi(&w->digits[v_size], &u->digits[v_size], u_size - v_size);
    }
    return MP_OK;
}

mp_err
zz_xor(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (!u->size) {
        return zz_copy(v, w);
    }
    if (!v->size) {
        return zz_copy(u, w);
    }

    mp_size_t u_size = u->size, v_size = v->size;

    if (u->negative || v->negative) {
        zz_t o1, o2;

        if (zz_init(&o1) || zz_init(&o2)) {
            /* LCOV_EXCL_START */
err:
            zz_clear(&o1);
            zz_clear(&o2);
            /* LCOV_EXCL_STOP */
            return MP_MEM;
        }
        if (u->negative) {
            if (zz_invert(u, &o1)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            o1.negative = true;
            u = &o1;
        }
        if (v->negative) {
            if (zz_invert(v, &o2)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            o2.negative = true;
            v = &o2;
        }
        if (u_size < v_size) {
            SWAP(const zz_t *, u, v);
            SWAP(mp_size_t, u_size, v_size);
        }
        if (u->negative & v->negative) {
            if (!u_size) {
                zz_clear(&o1);
                zz_clear(&o2);
                return zz_from_i32(0, w);
            }
            if (zz_resize(u_size, w)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            w->negative = false;
            mpn_copyi(&w->digits[v_size], &u->digits[v_size], u_size - v_size);
            if (v_size) {
                mpn_xor_n(w->digits, u->digits, v->digits, v_size);
            }
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
        else if (u->negative) {
            if (zz_resize(u_size + 1, w)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            w->negative = true;
            mpn_copyi(&w->digits[v_size], &u->digits[v_size], u_size - v_size);
            mpn_xor_n(w->digits, v->digits, u->digits, v_size);
            w->digits[u_size] = mpn_add_1(w->digits, w->digits, u_size, 1);
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
        else {
            if (zz_resize(u_size + 1, w)) {
                goto err; /* LCOV_EXCL_LINE */
            }
            w->negative = true;
            mpn_copyi(&w->digits[v_size], &u->digits[v_size], u_size - v_size);
            if (v_size) {
                mpn_xor_n(w->digits, u->digits, v->digits, v_size);
            }
            w->digits[u_size] = mpn_add_1(w->digits, w->digits, u_size, 1);
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
    }
    if (u_size < v_size) {
        SWAP(const zz_t *, u, v);
        SWAP(mp_size_t, u_size, v_size);
    }
    if (zz_resize(u_size, w)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    w->negative = false;
    mpn_xor_n(w->digits, u->digits, v->digits, v_size);
    if (u_size != v_size) {
        mpn_copyi(&w->digits[v_size], &u->digits[v_size], u_size - v_size);
    }
    else {
        zz_normalize(w);
    }
    return MP_OK;
}

#define GMP_LIMB_MAX ((mp_limb_t) ~(mp_limb_t)0)

mp_err
zz_pow(const zz_t *u, uint64_t v, zz_t *w)
{
    if (u == w) {
        zz_t tmp;

        if (zz_init(&tmp) || zz_copy(u, &tmp)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }

        mp_err ret = zz_pow(&tmp, v, w);

        zz_clear(&tmp);
        return ret;
    }
    if (!v) {
        return zz_from_i32(1, w);
    }
    if (!u->size) {
        return zz_from_i32(0, w);
    }
    if (zz_cmp_i32(u, 1) == MP_EQ) {
        if (u->negative) {
            return zz_from_i32(v%2 ? -1 : 1, w);
        }
        else {
            return zz_from_i32(1, w);
        }
    }
    if (v > GMP_LIMB_MAX) {
        return MP_BUF;
    }

    mp_limb_t e = (mp_limb_t)v;
    mp_size_t w_size = u->size * e;
    mp_limb_t *tmp = malloc(w_size * sizeof(mp_limb_t));

    if (!tmp || zz_resize(w_size, w)) {
        /* LCOV_EXCL_START */
        free(tmp);
        zz_clear(w);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }
    w->negative = u->negative && e%2;
    w->size = mpn_pow_1(w->digits, u->digits, u->size, e, tmp);
    free(tmp);
    if (zz_resize(w->size, w)) {
        /* LCOV_EXCL_START */
        zz_clear(w);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }
    return MP_OK;
}

#define MIN(a, b) ((a) < (b) ? (a) : (b))

mp_err
zz_gcd(const zz_t *u, const zz_t *v, zz_t *w)
{
    w->negative = false;
    if (!u->size) {
        if (zz_resize(v->size, w) == MP_MEM) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        mpn_copyi(w->digits, v->digits, v->size);
        return MP_OK;
    }
    if (!v->size) {
        if (zz_resize(u->size, w) == MP_MEM) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        mpn_copyi(w->digits, u->digits, u->size);
        return MP_OK;
    }

    mp_limb_t shift = MIN(mpn_scan1(u->digits, 0), mpn_scan1(v->digits, 0));
    zz_t *volatile o1 = malloc(sizeof(zz_t));
    zz_t *volatile o2 = malloc(sizeof(zz_t));

    if (!o1 || !o2) {
        goto free; /* LCOV_EXCL_LINE */
    }
    if (zz_init(o1) || zz_init(o2)) {
        goto clear; /* LCOV_EXCL_LINE */
    }
    if (zz_copy(u, o1) || zz_copy(v, o2)) {
        goto clear; /* LCOV_EXCL_LINE */
    }
    if (shift && (zz_quo_2exp(o1, shift, o1) || zz_quo_2exp(o2, shift, o2))) {
        goto clear; /* LCOV_EXCL_LINE */
    }
    u = o1;
    v = o2;
    if (u->size < v->size) {
        SWAP(const zz_t *, u, v);
    }
    if (zz_resize(v->size, w) == MP_MEM || TMP_OVERFLOW) {
        goto clear; /* LCOV_EXCL_LINE */
    }
    w->size = mpn_gcd(w->digits, u->digits, u->size, v->digits, v->size);
    zz_clear(o1);
    zz_clear(o2);
    free(o1);
    free(o2);
    if (shift && zz_mul_2exp(w, shift, w)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    return MP_OK;
clear:
    zz_clear(o1);
    zz_clear(o2);
free:
    free(o1);
    free(o2);
    return MP_MEM;
}

mp_err
zz_gcdext(const zz_t *u, const zz_t *v, zz_t *g, zz_t *s, zz_t *t)
{
    if (u->size < v->size) {
        SWAP(const zz_t *, u, v);
        SWAP(zz_t *, s, t);
    }
    if (!v->size) {
        if (g) {
            if (zz_resize(u->size, g) == MP_MEM) {
                return MP_MEM; /* LCOV_EXCL_LINE */
            }
            g->negative = false;
            mpn_copyi(g->digits, u->digits, u->size);
        }
        if (s) {
            if (zz_resize(1, s) == MP_MEM) {
                return MP_MEM; /* LCOV_EXCL_LINE */
            }
            s->digits[0] = 1;
            s->size = u->size > 0;
            s->negative = u->negative;
        }
        if (t) {
            t->size = 0;
            t->negative = false;
        }
        return MP_OK;
    }

    zz_t *volatile o1 = malloc(sizeof(zz_t));
    zz_t *volatile o2 = malloc(sizeof(zz_t));
    zz_t *volatile tmp_g = malloc(sizeof(zz_t));
    zz_t *volatile tmp_s = malloc(sizeof(zz_t));

    if (!o1 || !o2 || !tmp_g || !tmp_s) {
        goto free; /* LCOV_EXCL_LINE */
    }

    if (zz_init(o1) || zz_init(o2)
        || zz_init(tmp_g) || zz_init(tmp_s)
        || zz_copy(u, o1) || zz_copy(v, o2)
        || zz_resize(v->size, tmp_g) || zz_resize(v->size + 1, tmp_s)
        || TMP_OVERFLOW)
    {
        goto clear; /* LCOV_EXCL_LINE */
    }
    tmp_g->negative = false;
    tmp_s->negative = false;
    mp_size_t ssize;

    tmp_g->size = mpn_gcdext(tmp_g->digits, tmp_s->digits, &ssize,
                             o1->digits, u->size, o2->digits, v->size);
    tmp_s->size = ABS(ssize);
    tmp_s->negative = ((u->negative && ssize > 0)
                       || (!u->negative && ssize < 0));
    zz_clear(o1);
    zz_clear(o2);
    free(o1);
    free(o2);
    o1 = o2 = NULL;
    if (t) {
        zz_t tmp;

        if (zz_init(&tmp) || zz_mul(u, tmp_s, &tmp)
            || zz_sub(tmp_g, &tmp, &tmp)
            || zz_div(&tmp, v, MP_RNDD, &tmp, NULL)
            || zz_resize(tmp.size, t) == MP_MEM)
        {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            goto clear;
            /* LCOV_EXCL_STOP */
        }
        mpn_copyi(t->digits, tmp.digits, tmp.size);
        t->negative = tmp.negative;
        zz_clear(&tmp);
    }
    if (s) {
        if (zz_resize(tmp_s->size, s) == MP_MEM) {
            /* LCOV_EXCL_START */
            goto clear;
            /* LCOV_EXCL_STOP */
        }
        mpn_copyi(s->digits, tmp_s->digits, tmp_s->size);
        s->negative = tmp_s->negative;
        zz_clear(tmp_s);
    }
    if (g) {
        if (zz_resize(tmp_g->size, g) == MP_MEM) {
            /* LCOV_EXCL_START */
            goto clear;
            /* LCOV_EXCL_STOP */
        }
        mpn_copyi(g->digits, tmp_g->digits, tmp_g->size);
        g->negative = false;
        zz_clear(tmp_g);
    }
    return MP_OK;
clear:
    zz_clear(o1);
    zz_clear(o2);
    zz_clear(tmp_g);
    zz_clear(tmp_s);
free:
    free(o1);
    free(o2);
    free(tmp_g);
    free(tmp_s);
    return MP_MEM;
}

mp_err
zz_inverse(const zz_t *u, const zz_t *v, zz_t *w)
{
    zz_t g;

    if (zz_init(&g) || zz_gcdext(u, v, &g, w, NULL) == MP_MEM) {
        /* LCOV_EXCL_START */
        zz_clear(&g);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }
    if (zz_cmp_i32(&g, 1) == MP_EQ) {
        zz_clear(&g);
        return MP_OK;
    }
    zz_clear(&g);
    return MP_VAL;
}

static mp_err
_zz_powm(const zz_t *u, const zz_t *v, const zz_t *w, zz_t *res)
{
    if (mpn_scan1(w->digits, 0)) {
        mpz_t z;
        TMP_ZZ(b, u)
        TMP_ZZ(e, v)
        TMP_ZZ(m, w)
        if (TMP_OVERFLOW) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        mpz_init(z);
        mpz_powm(z, b, e, m);
        if (zz_resize(z->_mp_size, res)) {
            /* LCOV_EXCL_START */
            mpz_clear(z);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }
        res->negative = false;
        mpn_copyi(res->digits, z->_mp_d, res->size);
        mpz_clear(z);
        return MP_OK;
    }
    if (zz_resize(w->size, res)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    res->negative = false;

    mp_size_t enb = v->size * GMP_NUMB_BITS;
    mp_size_t tmp_size = mpn_sec_powm_itch(u->size, enb, w->size);
    mp_limb_t *volatile tmp = malloc(tmp_size * sizeof(mp_limb_t));

    if (!tmp || TMP_OVERFLOW) {
        /* LCOV_EXCL_START */
        free(tmp);
        zz_clear(res);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }
    mpn_sec_powm(res->digits, u->digits, u->size, v->digits, enb, w->digits,
                 w->size, tmp);
    free(tmp);
    zz_normalize(res);
    return MP_OK;
}

mp_err
zz_powm(const zz_t *u, const zz_t *v, const zz_t *w, zz_t *res)
{
    if (!w->size) {
        return MP_VAL;
    }
    if (u == res) {
        zz_t tmp;

        if (zz_init(&tmp) || zz_copy(u, &tmp)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }

        mp_err ret = zz_powm(&tmp, v, w, res);

        zz_clear(&tmp);
        return ret;
    }
    if (v == res) {
        zz_t tmp;

        if (zz_init(&tmp) || zz_copy(v, &tmp)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }

        mp_err ret = zz_powm(u, &tmp, w, res);

        zz_clear(&tmp);
        return ret;
    }
    if (w == res) {
        zz_t tmp;

        if (zz_init(&tmp) || zz_copy(w, &tmp)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }

        mp_err ret = zz_powm(u, v, &tmp, res);

        zz_clear(&tmp);
        return ret;
    }

    int negativeOutput = 0;
    zz_t o1, o2, o3;
    mp_err ret = MP_OK;

    if (zz_init(&o1) || zz_init(&o2) || zz_init(&o3)) {
        /* LCOV_EXCL_START */
        zz_clear(&o1);
        zz_clear(&o2);
        zz_clear(&o3);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }
    if (w->negative) {
        if (zz_copy(w, &o3)) {
            goto end3; /* LCOV_EXCL_LINE */
        }
        negativeOutput = 1;
        o3.negative = false;
        w = &o3;
    }
    if (v->negative) {
        if (zz_copy(v, &o2)) {
            goto end3; /* LCOV_EXCL_LINE */
        }
        o2.negative = false;
        v = &o2;

        if ((ret = zz_inverse(u, w, &o1)) == MP_MEM) {
            goto end3; /* LCOV_EXCL_LINE */
        }
        if (ret == MP_VAL) {
end3:
            zz_clear(&o1);
            zz_clear(&o2);
            zz_clear(&o3);
            return MP_VAL;
        }
        u = &o1;
    }
    if (u->negative || u->size > w->size) {
        zz_t tmp;

        if (zz_init(&tmp) || zz_div(u, w, MP_RNDD, NULL, &tmp)
            || zz_copy(&tmp, &o1))
        {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            goto end3;
            /* LCOV_EXCL_STOP */
        }
        zz_clear(&tmp);
        u = &o1;
    }
    if (zz_cmp_i32(w, 1) == MP_EQ) {
        ret = zz_from_i32(0, res);
    }
    else if (!v->size) {
        ret = zz_from_i32(1, res);
    }
    else if (!u->size) {
        ret = zz_from_i32(0, res);
    }
    else if (zz_cmp_i32(u, 1) == MP_EQ) {
        ret = zz_from_i32(1, res);
    }
    else {
        if (_zz_powm(u, v, w, res)) {
            goto end3; /* LCOV_EXCL_LINE */
        }
    }
    if (negativeOutput && !ret && res->size && zz_sub(res, w, res)) {
        goto end3; /* LCOV_EXCL_LINE */
    }
    zz_clear(&o1);
    zz_clear(&o2);
    zz_clear(&o3);
    return ret;
}

mp_err
zz_sqrtrem(const zz_t *u, zz_t *v, zz_t *w)
{
    if (u->negative) {
        return MP_VAL;
    }
    v->negative = false;
    if (!u->size) {
        v->size = 0;
        if (w) {
            w->size = 0;
            w->negative = false;
        }
        return MP_OK;
    }
    if (u == v) {
        zz_t tmp;

        if (zz_init(&tmp) || zz_copy(v, &tmp)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }

        mp_err ret = zz_sqrtrem(&tmp, v, w);

        zz_clear(&tmp);
        return ret;
    }
    if (zz_resize((u->size + 1)/2, v) == MP_MEM || TMP_OVERFLOW) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    if (w) {
        w->negative = false;
        if (zz_resize(u->size, w) == MP_MEM) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        w->size = mpn_sqrtrem(v->digits, w->digits, u->digits, u->size);
    }
    else {
        mpn_sqrtrem(v->digits, NULL, u->digits, u->size);
    }
    return MP_OK;
}

#define MK_ZZ_FUNC_UL(name, mpz_suff)                \
    mp_err                                           \
    zz_##name(uint64_t u, zz_t *v)                   \
    {                                                \
        if (u > ULONG_MAX) {                         \
            return MP_BUF;                           \
        }                                            \
        if (TMP_OVERFLOW) {                          \
            return MP_MEM; /* LCOV_EXCL_LINE */      \
        }                                            \
                                                     \
        mpz_t z;                                     \
                                                     \
        mpz_init(z);                                 \
        mpz_##mpz_suff(z, (unsigned long)u);         \
        if (zz_resize(z->_mp_size, v) == MP_MEM) {   \
            /* LCOV_EXCL_START */                    \
            mpz_clear(z);                            \
            return MP_MEM;                           \
            /* LCOV_EXCL_STOP */                     \
        }                                            \
        mpn_copyi(v->digits, z->_mp_d, z->_mp_size); \
        mpz_clear(z);                                \
        return MP_OK;                                \
    }

MK_ZZ_FUNC_UL(fac, fac_ui)
MK_ZZ_FUNC_UL(fac2, 2fac_ui)
MK_ZZ_FUNC_UL(fib, fib_ui)
