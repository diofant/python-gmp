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
zz_resize(zz_t *u, mp_size_t size)
{
    if (u->alloc >= size) {
        u->size = size;
    }

    mp_size_t alloc = size;
    mp_limb_t *t = u->digits;

    if (!alloc) {
        alloc = 1;
    }
    u->digits = realloc(u->digits, alloc*sizeof(mp_limb_t));
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

void
zz_normalize(zz_t *u)
{
    while (u->size && u->digits[u->size - 1] == 0) {
        u->size--;
    }
    if (!u->size) {
        u->negative = 0;
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

mp_err
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

mp_err
zz_abs(const zz_t *u, zz_t *v)
{
    mp_err ret = zz_copy(u, v);

    if (!ret) {
        v->negative = false;
    }
    return ret;
}

mp_err
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
int OPT_TAG = 0x1;
int OPT_PREFIX = 0x2;

mp_err
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
        mp_limb_t *volatile tmp = malloc(sizeof(mp_limb_t) * u->alloc);

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

mp_err
zz_from_str(const char *str, size_t len, int base, zz_t *u)
{
    if (base != 0 && (base < 2 || base > 36)) {
        return MP_VAL;
    }
    if (!len) {
        return MP_VAL;
    }

    unsigned char *volatile buf = malloc(len), *p = buf;

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

mp_err
zz_to_double(const zz_t *u, mp_size_t shift, double *d)
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

#define SWAP(T, a, b) \
    do {              \
        T _tmp = a;   \
        a = b;        \
        b = _tmp;     \
    } while (0);

static void
revstr(unsigned char *s, size_t l, size_t r)
{
    while (l < r) {
        SWAP(unsigned char, s[l], s[r]);
        l++;
        r--;
    }
}

mp_err
zz_to_bytes(const zz_t *u, size_t length, int is_little, int is_signed,
            unsigned char **buffer)
{
    zz_t tmp;
    int is_negative = u->negative;

    if (zz_init(&tmp)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    if (is_negative) {
        if (!is_signed) {
            return MP_BUF;
        }
        if (zz_resize(&tmp, (8*length)/GMP_NUMB_BITS + 1)) {
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

    memset(*buffer, is_negative ? 0xFF : 0, gap);
    if (u->size) {
        mpn_get_str(*buffer + gap, 256, u->digits, u->size);
    }
    zz_clear(&tmp);
    if (is_little && length) {
        revstr(*buffer, 0, length - 1);
    }
    return MP_OK;
}

mp_err
zz_from_bytes(const unsigned char *buffer, size_t length, int is_little,
              int is_signed, zz_t *u)
{
    if (!length) {
        return zz_from_i64(u, 0);
    }
    if (zz_resize(u, 1 + length/2)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    if (is_little) {
        unsigned char *tmp = malloc(length);

        if (!tmp) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        memcpy(tmp, buffer, length);
        revstr(tmp, 0, length - 1);
        u->size = mpn_set_str(u->digits, tmp, length, 256);
        free(tmp);
    }
    else {
        u->size = mpn_set_str(u->digits, buffer, length, 256);
    }
    if (zz_resize(u, u->size) == MP_MEM) {
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
        w->digits[w->size - 1] = mpn_add(w->digits, u->digits, u->size,
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

mp_err
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

        if (zz_resize(q, u->size - v->size + 1 + q_negative)
            || zz_resize(r, v->size) || TMP_OVERFLOW)
        {
            goto err; /* LCOV_EXCL_LINE */
        }
        q->negative = q_negative;
        if (q_negative) {
            q->digits[q->size - 1] = 0;
        }
        r->negative = v->negative;
        mpn_tdiv_qr(q->digits, r->digits, 0, u->digits, u->size, v->digits,
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
    return MP_OK;
    /* LCOV_EXCL_START */
err:
    zz_clear(q);
    zz_clear(r);
    return MP_MEM;
    /* LCOV_EXCL_STOP */
}

mp_err
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

mp_err
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

mp_err
zz_rshift1(const zz_t *u, mp_limb_t rshift, zz_t *v)
{
    mp_size_t whole = rshift / GMP_NUMB_BITS;
    mp_size_t size = u->size;

    rshift %= GMP_NUMB_BITS;
    if (whole >= size) {
        return zz_from_i64(v, u->negative ? -1 : 0);
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
    if (zz_resize(v, size + extra)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    v->negative = u->negative;
    if (extra) {
        v->digits[size] = 0;
    }
    if (rshift) {
        if (mpn_rshift(v->digits, u->digits + whole, size, rshift)) {
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
zz_lshift1(const zz_t *u, mp_limb_t lshift, zz_t *v)
{
    mp_size_t whole = lshift / GMP_NUMB_BITS;
    mp_size_t size = u->size + whole;

    lshift %= GMP_NUMB_BITS;
    if (lshift) {
        size++;
    }
    if (zz_resize(v, size)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    v->negative = u->negative;
    if (whole) {
        mpn_zero(v->digits, whole);
    }
    if (lshift) {
        v->digits[size - 1] = mpn_lshift(v->digits + whole, u->digits, u->size,
                                         lshift);
    }
    else {
        mpn_copyi(v->digits + whole, u->digits, u->size);
    }
    zz_normalize(v);
    return MP_OK;
}

mp_err
zz_lshift(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (v->negative) {
        return MP_VAL;
    }
    if (!u->size) {
        return zz_from_i64(w, 0);
    }
    if (!v->size) {
        return zz_copy(u, w);
    }
    if (v->size > 1) {
        return MP_BUF;
    }
    return zz_lshift1(u, v->digits[0], w);
}

mp_err
zz_rshift(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (v->negative) {
        return MP_VAL;
    }
    if (!u->size) {
        return zz_from_i64(w, 0);
    }
    if (!v->size) {
        return zz_copy(u, w);
    }
    if (v->size > 1) {
        if (u->negative) {
            return zz_from_i64(w, -1);
        }
        else {
            return zz_from_i64(w, 0);
        }
    }
    return zz_rshift1(u, v->digits[0], w);
}

mp_err
zz_divmod_near(zz_t *q, zz_t *r, const zz_t *u, const zz_t *v)
{
    mp_ord unexpect = v->negative ? MP_LT : MP_GT;
    mp_err ret = zz_divmod(q, r, u, v);

    if (ret) {
        /* LCOV_EXCL_START */
        zz_clear(q);
        zz_clear(r);
        return ret;
        /* LCOV_EXCL_STOP */
    }

    zz_t halfQ;

    if (zz_init(&halfQ) || zz_rshift1(v, 1, &halfQ)) {
        /* LCOV_EXCL_START */
        zz_clear(q);
        zz_clear(r);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }

    mp_ord cmp = zz_cmp(r, &halfQ);

    zz_clear(&halfQ);
    if (cmp == MP_EQ && v->digits[0]%2 == 0 && q->size && q->digits[0]%2 != 0) {
        cmp = unexpect;
    }
    if (cmp == unexpect) {
        zz_t one, tmp;

        if (zz_init(&one) || zz_from_i64(&one, 1)) {
            /* LCOV_EXCL_START */
            zz_clear(&one);
            goto err;
            /* LCOV_EXCL_STOP */
        }
        if (zz_init(&tmp) || zz_add(q, &one, &tmp)) {
            /* LCOV_EXCL_START */
            zz_clear(&one);
            zz_clear(&tmp);
            goto err;
            /* LCOV_EXCL_STOP */
        }
        zz_clear(&one);
        zz_clear(q);
        if (zz_copy(&tmp, q) || zz_sub(r, v, &tmp)) {
            goto err; /* LCOV_EXCL_LINE */
        }
        zz_clear(r);
        if (zz_copy(&tmp, r)) {
            goto err; /* LCOV_EXCL_LINE */
        }
        zz_clear(&tmp);
    }
    return MP_OK;
err:
    /* LCOV_EXCL_START */
    zz_clear(q);
    zz_clear(r);
    return MP_MEM;
    /* LCOV_EXCL_STOP */
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

    zz_t tmp0, tmp1, tmp2;

    if (zz_init(&tmp1)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    if (shift > 0) {
        if (zz_init(&tmp0) || zz_abs(u, &tmp0)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp0);
            zz_clear(&tmp1);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }
        if (zz_lshift1(&tmp0, shift, &tmp1)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp0);
            zz_clear(&tmp1);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }
        zz_clear(&tmp0);
    }
    else {
        if (zz_abs(u, &tmp1)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp1);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }
    }
    a = &tmp1;
    if (zz_init(&tmp2)) {
        /* LCOV_EXCL_START */
        zz_clear(&tmp1);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }
    if (shift < 0) {
        if (zz_init(&tmp0) || zz_abs(v, &tmp0)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp0);
            zz_clear(&tmp1);
            zz_clear(&tmp2);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }
        if (zz_lshift1(&tmp0, -shift, &tmp2)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp0);
            zz_clear(&tmp1);
            zz_clear(&tmp2);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }
        zz_clear(&tmp0);
    }
    else {
        if (zz_abs(v, &tmp2)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp1);
            zz_clear(&tmp2);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }
    }
    b = &tmp2;

    zz_t c, d;

    if (zz_init(&c) || zz_init(&d) || zz_divmod_near(&c, &d, a, b)) {
        /* LCOV_EXCL_START */
        zz_clear(a);
        zz_clear(b);
        zz_clear(&c);
        zz_clear(&d);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }
    zz_clear(a);
    zz_clear(b);
    zz_clear(&d);

    mp_err ret = zz_to_double(&c, shift, res);

    zz_clear(&c);
    if (u->negative != v->negative) {
        *res = -*res;
    }
    return ret;
}

mp_err
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
        v->negative = true;
        v->digits[u->size] = mpn_add_1(v->digits, u->digits, u->size, 1);
        v->size -= v->digits[u->size] == 0;
    }
    return MP_OK;
}

mp_err
zz_and(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (!u->size || !v->size) {
        return zz_from_i64(w, 0);
    }
    if (u->negative || v->negative) {
        zz_t o1, o2;

        if (zz_init(&o1) || zz_init(&o2)) {
            /* LCOV_EXCL_START */
            zz_clear(&o1);
            zz_clear(&o2);
            /* LCOV_EXCL_STOP */
            return MP_MEM;
        }

        mp_err ret;

        if (u->negative) {
            ret = zz_invert(u, &o1);
            o1.negative = 1;
        }
        else {
            ret = zz_copy(u, &o1);
        }
        if (ret) {
            /* LCOV_EXCL_START */
            zz_clear(&o1);
            zz_clear(&o2);
            /* LCOV_EXCL_STOP */
            return MP_MEM;
        }
        if (v->negative) {
            ret = zz_invert(v, &o2);
            o2.negative = 1;
        }
        else {
            ret = zz_copy(v, &o2);
        }
        if (ret) {
            /* LCOV_EXCL_START */
            zz_clear(&o1);
            zz_clear(&o2);
            /* LCOV_EXCL_STOP */
            return MP_MEM;
        }
        u = &o1;
        v = &o2;
        if (u->size < v->size) {
            SWAP(const zz_t *, u, v);
        }
        if (u->negative & v->negative) {
            if (!u->size) {
                zz_clear(&o1);
                zz_clear(&o2);
                return zz_from_i64(w, -1);
            }
            if (zz_resize(w, u->size + 1)) {
                /* LCOV_EXCL_START */
                zz_clear(&o1);
                zz_clear(&o2);
                return MP_MEM;
                /* LCOV_EXCL_STOP */
            }
            w->negative = true;
            mpn_copyi(&w->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            if (v->size) {
                mpn_ior_n(w->digits, u->digits, v->digits, v->size);
            }
            w->digits[u->size] = mpn_add_1(w->digits, w->digits, u->size, 1);
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
        else if (u->negative) {
            if (zz_resize(w, v->size)) {
                /* LCOV_EXCL_START */
                zz_clear(&o1);
                zz_clear(&o2);
                return MP_MEM;
                /* LCOV_EXCL_STOP */
            }
            w->negative = false;
            mpn_andn_n(w->digits, v->digits, u->digits, v->size);
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
        else {
            if (zz_resize(w, u->size)) {
                /* LCOV_EXCL_START */
                zz_clear(&o1);
                zz_clear(&o2);
                return MP_MEM;
                /* LCOV_EXCL_STOP */
            }
            w->negative = false;
            if (v->size) {
                mpn_andn_n(w->digits, u->digits, v->digits, v->size);
            }
            mpn_copyi(&w->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
    }
    if (u->size < v->size) {
        SWAP(const zz_t *, u, v);
    }
    if (zz_resize(w, v->size)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    w->negative = false;
    mpn_and_n(w->digits, u->digits, v->digits, v->size);
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
    if (u->negative || v->negative) {
        zz_t o1, o2;

        if (zz_init(&o1) || zz_init(&o2)) {
            /* LCOV_EXCL_START */
            zz_clear(&o1);
            zz_clear(&o2);
            /* LCOV_EXCL_STOP */
            return MP_MEM;
        }

        mp_err ret;

        if (u->negative) {
            ret = zz_invert(u, &o1);
            o1.negative = 1;
        }
        else {
            ret = zz_copy(u, &o1);
        }
        if (ret) {
            /* LCOV_EXCL_START */
            zz_clear(&o1);
            zz_clear(&o2);
            /* LCOV_EXCL_STOP */
            return MP_MEM;
        }
        if (v->negative) {
            ret = zz_invert(v, &o2);
            o2.negative = true;
        }
        else {
            ret = zz_copy(v, &o2);
        }
        if (ret) {
            /* LCOV_EXCL_START */
            zz_clear(&o1);
            zz_clear(&o2);
            /* LCOV_EXCL_STOP */
            return MP_MEM;
        }
        u = &o1;
        v = &o2;
        if (u->size < v->size) {
            SWAP(const zz_t *, u, v);
        }
        if (u->negative & v->negative) {
            if (!v->size) {
                zz_clear(&o1);
                zz_clear(&o2);
                return zz_from_i64(w, -1);
            }
            if (zz_resize(w, v->size + 1)) {
                /* LCOV_EXCL_START */
                zz_clear(&o1);
                zz_clear(&o2);
                return MP_MEM;
                /* LCOV_EXCL_STOP */
            }
            w->negative = true;
            mpn_and_n(w->digits, u->digits, v->digits, v->size);
            w->digits[v->size] = mpn_add_1(w->digits, w->digits, v->size, 1);
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
        else if (u->negative) {
            if (zz_resize(w, u->size + 1)) {
                /* LCOV_EXCL_START */
                zz_clear(&o1);
                zz_clear(&o2);
                return MP_MEM;
                /* LCOV_EXCL_STOP */
            }
            w->negative = true;
            mpn_copyi(&w->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            mpn_andn_n(w->digits, u->digits, v->digits, v->size);
            w->digits[u->size] = mpn_add_1(w->digits, w->digits, u->size, 1);
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
        else {
            if (zz_resize(w, v->size + 1)) {
                /* LCOV_EXCL_START */
                zz_clear(&o1);
                zz_clear(&o2);
                return MP_MEM;
                /* LCOV_EXCL_STOP */
            }
            w->negative = true;
            if (v->size) {
                mpn_andn_n(w->digits, v->digits, u->digits, v->size);
                w->digits[v->size] = mpn_add_1(w->digits, w->digits, v->size,
                                               1);
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
    if (u->size < v->size) {
        SWAP(const zz_t *, u, v);
    }
    if (zz_resize(w, u->size)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    w->negative = false;
    mpn_ior_n(w->digits, u->digits, v->digits, v->size);
    if (u->size != v->size) {
        mpn_copyi(&w->digits[v->size], &u->digits[v->size], u->size - v->size);
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
    if (u->negative || v->negative) {
        zz_t o1, o2;

        if (zz_init(&o1) || zz_init(&o2)) {
            /* LCOV_EXCL_START */
            zz_clear(&o1);
            zz_clear(&o2);
            /* LCOV_EXCL_STOP */
            return MP_MEM;
        }

        mp_err ret;

        if (u->negative) {
            ret = zz_invert(u, &o1);
            o1.negative = 1;
        }
        else {
            ret = zz_copy(u, &o1);
        }
        if (ret) {
            /* LCOV_EXCL_START */
            zz_clear(&o1);
            zz_clear(&o2);
            /* LCOV_EXCL_STOP */
            return MP_MEM;
        }
        if (v->negative) {
            ret = zz_invert(v, &o2);
            o2.negative = 1;
        }
        else {
            ret = zz_copy(v, &o2);
        }
        if (ret) {
            /* LCOV_EXCL_START */
            zz_clear(&o1);
            zz_clear(&o2);
            /* LCOV_EXCL_STOP */
            return MP_MEM;
        }
        u = &o1;
        v = &o2;
        if (u->size < v->size) {
            SWAP(const zz_t *, u, v);
        }
        if (u->negative & v->negative) {
            if (!u->size) {
                zz_clear(&o1);
                zz_clear(&o2);
                return zz_from_i64(w, 0);
            }
            if (zz_resize(w, u->size)) {
                /* LCOV_EXCL_START */
                zz_clear(&o1);
                zz_clear(&o2);
                return MP_MEM;
                /* LCOV_EXCL_STOP */
            }
            w->negative = false;
            mpn_copyi(&w->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            if (v->size) {
                mpn_xor_n(w->digits, u->digits, v->digits, v->size);
            }
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
        else if (u->negative) {
            if (zz_resize(w, u->size + 1)) {
                /* LCOV_EXCL_START */
                zz_clear(&o1);
                zz_clear(&o2);
                return MP_MEM;
                /* LCOV_EXCL_STOP */
            }
            w->negative = true;
            mpn_copyi(&w->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            mpn_xor_n(w->digits, v->digits, u->digits, v->size);
            w->digits[u->size] = mpn_add_1(w->digits, w->digits, u->size, 1);
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
        else {
            if (zz_resize(w, u->size + 1)) {
                /* LCOV_EXCL_START */
                zz_clear(&o1);
                zz_clear(&o2);
                return MP_MEM;
                /* LCOV_EXCL_STOP */
            }
            w->negative = true;
            mpn_copyi(&w->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            if (v->size) {
                mpn_xor_n(w->digits, u->digits, v->digits, v->size);
            }
            w->digits[u->size] = mpn_add_1(w->digits, w->digits, u->size, 1);
            zz_normalize(w);
            zz_clear(&o1);
            zz_clear(&o2);
            return MP_OK;
        }
    }
    if (u->size < v->size) {
        SWAP(const zz_t *, u, v);
    }
    if (zz_resize(w, u->size)) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    w->negative = false;
    mpn_xor_n(w->digits, u->digits, v->digits, v->size);
    if (u->size != v->size) {
        mpn_copyi(&w->digits[v->size], &u->digits[v->size], u->size - v->size);
    }
    else {
        zz_normalize(w);
    }
    return MP_OK;
}

mp_err
zz_pow(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (!v->size) {
        return zz_from_i64(w, 1);
    }
    if (!u->size) {
        return zz_from_i64(w, 0);
    }
    if (u->size == 1 && u->digits[0] == 1) {
        if (u->negative) {
            return zz_from_i64(w, (v->digits[0] % 2) ? -1 : 1);
        }
        else {
            return zz_from_i64(w, 1);
        }
    }
    if (v->size > 1 || v->negative) {
        return MP_MEM;
    }

    mp_limb_t e = v->digits[0];
    mp_size_t w_size = u->size * e;
    mp_limb_t *tmp = malloc(w_size * sizeof(mp_limb_t));

    if (!tmp || zz_resize(w, w_size)) {
        /* LCOV_EXCL_START */
        free(tmp);
        zz_clear(w);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }
    w->negative = u->negative && e%2;
    w->size = mpn_pow_1(w->digits, u->digits, u->size, e, tmp);
    free(tmp);
    if (zz_resize(w, w->size)) {
        /* LCOV_EXCL_START */
        zz_clear(w);
        return MP_MEM;
        /* LCOV_EXCL_STOP */
    }
    return MP_OK;
}

#define ABS(a) ((a) < 0 ? (-a) : (a))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

mp_err
zz_gcd(const zz_t *u, const zz_t *v, zz_t *gcd)
{
    gcd->negative = 0;
    if (!u->size) {
        if (zz_resize(gcd, v->size) == MP_MEM) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        mpn_copyi(gcd->digits, v->digits, v->size);
        return MP_OK;
    }
    if (!v->size) {
        if (zz_resize(gcd, u->size) == MP_MEM) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        mpn_copyi(gcd->digits, u->digits, u->size);
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
    if (shift) {
        if (zz_rshift1(u, shift, o1) || zz_rshift1(v, shift, o2)) {
            goto clear; /* LCOV_EXCL_LINE */
        }
    }
    else {
        if (zz_copy(u, o1) || zz_copy(v, o2)) {
            goto clear; /* LCOV_EXCL_LINE */
        }
    }
    u = o1;
    v = o2;
    if (u->size < v->size) {
        SWAP(const zz_t *, u, v);
    }
    if (zz_resize(gcd, v->size) == MP_MEM || TMP_OVERFLOW) {
        goto clear; /* LCOV_EXCL_LINE */
    }
    gcd->size = mpn_gcd(gcd->digits, u->digits, u->size, v->digits, v->size);
    zz_clear(o1);
    zz_clear(o2);
    free(o1);
    free(o2);
    if (shift) {
        zz_t tmp;

        if (zz_init(&tmp) || zz_lshift1(gcd, shift, &tmp)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }
        if (zz_resize(gcd, tmp.size) == MP_MEM) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            return MP_MEM;
            /* LCOV_EXCL_STOP */
        }
        mpn_copyi(gcd->digits, tmp.digits, tmp.size);
        zz_clear(&tmp);
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
            if (zz_resize(g, u->size) == MP_MEM) {
                return MP_MEM; /* LCOV_EXCL_LINE */
            }
            g->negative = 0;
            mpn_copyi(g->digits, u->digits, u->size);
        }
        if (s) {
            if (zz_resize(s, 1) == MP_MEM) {
                return MP_MEM; /* LCOV_EXCL_LINE */
            }
            s->digits[0] = 1;
            s->size = u->size > 0;
            s->negative = u->negative;
        }
        if (t) {
            t->size = 0;
            t->negative = 0;
        }
        return MP_OK;
    }

    zz_t *volatile arg_u = malloc(sizeof(zz_t));
    zz_t *volatile arg_v = malloc(sizeof(zz_t));
    zz_t *volatile tmp_g = malloc(sizeof(zz_t));
    zz_t *volatile tmp_s = malloc(sizeof(zz_t));

    if (!arg_u || !arg_v || !tmp_g || !tmp_s) {
        goto free; /* LCOV_EXCL_LINE */
    }

    if (zz_init(arg_u) || zz_init(arg_v)
        || zz_init(tmp_g) || zz_init(tmp_s)
        || zz_copy(u, arg_u) || zz_copy(v, arg_v)
        || zz_resize(tmp_g, v->size) || zz_resize(tmp_s, v->size + 1)
        || TMP_OVERFLOW)
    {
        goto clear; /* LCOV_EXCL_LINE */
    }
    tmp_g->negative = false;
    tmp_s->negative = false;
    mp_size_t ssize;

    tmp_g->size = mpn_gcdext(tmp_g->digits, tmp_s->digits, &ssize,
                             arg_u->digits, u->size, arg_v->digits, v->size);
    tmp_s->size = ABS(ssize);
    tmp_s->negative = ((u->negative && ssize > 0)
                       || (!u->negative && ssize < 0));
    zz_clear(arg_u);
    zz_clear(arg_v);
    free(arg_u);
    free(arg_v);
    arg_u = arg_v = NULL;
    if (t) {
        zz_t us, x, q;

        if (zz_init(&us) || zz_mul(u, tmp_s, &us)) {
            /* LCOV_EXCL_START */
            zz_clear(&us);
            goto clear;
            /* LCOV_EXCL_STOP */
        }
        if (zz_init(&x) || zz_sub(tmp_g, &us, &x)) {
            /* LCOV_EXCL_START */
            zz_clear(&us);
            zz_clear(&x);
            goto clear;
            /* LCOV_EXCL_STOP */
        }
        zz_clear(&us);
        if (zz_init(&q) || zz_quo(&x, v, &q)) {
            /* LCOV_EXCL_START */
            zz_clear(&x);
            zz_clear(&q);
            goto clear;
            /* LCOV_EXCL_STOP */
        }
        zz_clear(&x);
        if (zz_resize(t, q.size) == MP_MEM) {
            /* LCOV_EXCL_START */
            zz_clear(&q);
            goto clear;
            /* LCOV_EXCL_STOP */
        }
        mpn_copyi(t->digits, q.digits, q.size);
        t->negative = q.negative;
        zz_clear(&q);
    }
    if (s) {
        if (zz_resize(s, tmp_s->size) == MP_MEM) {
            /* LCOV_EXCL_START */
            goto clear;
            /* LCOV_EXCL_STOP */
        }
        mpn_copyi(s->digits, tmp_s->digits, tmp_s->size);
        s->negative = tmp_s->negative;
        zz_clear(tmp_s);
    }
    if (g) {
        if (zz_resize(g, tmp_g->size) == MP_MEM) {
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
    zz_clear(arg_u);
    zz_clear(arg_v);
    zz_clear(tmp_g);
    zz_clear(tmp_s);
free:
    free(arg_u);
    free(arg_v);
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
    if (g.size == 1 && g.digits[0] == 1) {
        zz_clear(&g);
        return MP_OK;
    }
    zz_clear(&g);
    return MP_VAL;
}

#define TMP_ZZ(z, u)                                \
    mpz_t z;                                        \
                                                    \
    z->_mp_d = u->digits;                           \
    z->_mp_size = (u->negative ? -1 : 1) * u->size; \
    z->_mp_alloc = u->alloc;

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
        if (zz_resize(res, z->_mp_size)) {
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
    if (zz_resize(res, w->size)) {
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

        if (zz_init(&tmp) || zz_rem(u, w, &tmp) || zz_copy(&tmp, &o1)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            goto end3;
            /* LCOV_EXCL_STOP */
        }
        zz_clear(&tmp);
        u = &o1;
    }
    if (w->size == 1 && w->digits[0] == 1) {
        ret = zz_from_i64(res, 0);
    }
    else if (!v->size) {
        ret = zz_from_i64(res, 1);
    }
    else if (!u->size) {
        ret = zz_from_i64(res, 0);
    }
    else if (u->size == 1 && u->digits[0] == 1) {
        ret = zz_from_i64(res, 1);
    }
    else {
        if (_zz_powm(u, v, w, res)) {
            goto end3; /* LCOV_EXCL_LINE */
        }
    }
    if (negativeOutput && !ret && res->size) {
        zz_t tmp;

        if (zz_init(&tmp) || zz_copy(res, &tmp) || zz_sub(&tmp, w, res)) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            goto end3;
            /* LCOV_EXCL_STOP */
        }
        zz_clear(&tmp);
    }
    zz_clear(&o1);
    zz_clear(&o2);
    zz_clear(&o3);
    return ret;
}

mp_err
zz_sqrtrem(const zz_t *u, zz_t *root, zz_t *rem)
{
    if (u->negative) {
        return MP_VAL;
    }
    root->negative = false;
    if (!u->size) {
        root->size = 0;
        if (rem) {
            rem->size = 0;
            rem->negative = false;
        }
        return MP_OK;
    }
    if (zz_resize(root, (u->size + 1)/2) == MP_MEM || TMP_OVERFLOW) {
        return MP_MEM; /* LCOV_EXCL_LINE */
    }
    if (rem) {
        rem->negative = false;
        if (zz_resize(rem, u->size) == MP_MEM) {
            return MP_MEM; /* LCOV_EXCL_LINE */
        }
        rem->size = mpn_sqrtrem(root->digits, rem->digits, u->digits, u->size);
    }
    else {
        mpn_sqrtrem(root->digits, NULL, u->digits, u->size);
    }
    return MP_OK;
}

#define MK_ZZ_FUNC_UL(name, mpz_suff)                \
    mp_err                                           \
    zz_##name##_ul(const zz_t *u, zz_t *v)           \
    {                                                \
        TMP_ZZ(z, u)                                 \
        if (u->negative) {                           \
            return MP_VAL;                           \
        }                                            \
        if (!mpz_fits_ulong_p(z)) {                  \
            return MP_BUF;                           \
        }                                            \
                                                     \
        unsigned long n = mpz_get_ui(z);             \
                                                     \
        if (TMP_OVERFLOW) {                          \
            return MP_MEM; /* LCOV_EXCL_LINE */      \
        }                                            \
        mpz_init(z);                                 \
        mpz_##mpz_suff(z, n);                        \
        if (zz_resize(v, z->_mp_size) == MP_MEM) {   \
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
MK_ZZ_FUNC_UL(double_fac, 2fac_ui)
MK_ZZ_FUNC_UL(fib, fib_ui)
