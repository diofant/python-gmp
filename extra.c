#include "extra.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <string.h>

static const zz_layout bytes_layout = {8, 1, 1, 0};

zz_err
zz_get_bytes(const zz_t *u, size_t len, bool is_signed, unsigned char **buf)
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
        if (zz_set(1, &tmp) || zz_mul_2exp(&tmp, 8*len, &tmp)
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

    if (zz_isneg(u) || nbits > 8*len
        || (is_signed && ((!nbits && is_negative)
        || (nbits && (nbits == 8 * len ? !is_negative : is_negative)))))
    {
        zz_clear(&tmp);
        return ZZ_BUF;
    }

    size_t gap = len - (nbits + bits_per_digit/8 - 1)/(bits_per_digit/8);

    (void)zz_export(u, bytes_layout, len - gap, *buf + gap);
    memset(*buf, is_negative ? 0xFF : 0, gap);
    zz_clear(&tmp);
    return ZZ_OK;
}

zz_err
zz_set_bytes(const unsigned char *buf, size_t len, bool is_signed, zz_t *u)
{
    if (!len) {
        return zz_set(0, u);
    }

    zz_err ret = zz_import(len, buf, bytes_layout, u);

    if (ret) {
        return ret; /* LCOV_EXCL_LINE */
    }
    (void)zz_abs(u, u);
    if (is_signed && zz_bitlen(u) == 8*(size_t)len) {
        zz_t tmp;

        /* we assume, that bytes were created by zz_get_bytes(),
           else there could be overflow in zz_mul_2exp() */
        if (zz_init(&tmp) || zz_set(1, &tmp)
            || zz_mul_2exp(&tmp, 8*len, &tmp)
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

zz_err
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
    /* No overflow is possible, since q can't have maximal value
       below (that means v==1) and r have same sign as v (with |r| < |v|) */
    if (cmp == unexpect && (zz_add(q, 1, q) || zz_sub(r, v, r))) {
        goto err; /* LCOV_EXCL_LINE */
    }
    return ZZ_OK;
}

zz_err
zz_truediv(const zz_t *u, const zz_t *v, double *w)
{
    if (zz_iszero(v)) {
        return ZZ_VAL;
    }
    if (zz_iszero(u)) {
        *w = zz_isneg(v) ? -0.0 : 0.0;
        return ZZ_OK;
    }

    zz_bitcnt_t ubits = zz_bitlen(u);
    zz_bitcnt_t vbits = zz_bitlen(v);

    if (ubits > vbits && ubits - vbits > DBL_MAX_EXP) {
        return ZZ_BUF;
    }
    if (ubits < vbits && vbits - ubits > -DBL_MIN_EXP + DBL_MANT_DIG + 1) {
        *w = zz_isneg(u) != zz_isneg(v) ? -0.0 : 0.0;
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
    /* No overflow is possible in shifts, since shift value
       much smaller here than zz_get_bitcnt_max() */
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
    (void)zz_get(&a, w);
    zz_clear(&a);
    *w = ldexp(*w, -shift);
    if (zz_isneg(u) != zz_isneg(v)) {
        *w = -*w;
    }
    if (isinf(*w)) {
        return ZZ_BUF;
    }
    return ZZ_OK;
}

zz_err
zz_lshift(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (zz_isneg(v)) {
        return ZZ_VAL;
    }

    uint64_t shift;

    if (zz_get(v, &shift)) {
        return ZZ_BUF;
    }
    return zz_mul_2exp(u, shift, w);
}

zz_err
zz_rshift(const zz_t *u, const zz_t *v, zz_t *w)
{
    if (zz_isneg(v)) {
        return ZZ_VAL;
    }

    uint64_t shift;

    if (zz_get(v, &shift)) {
        return zz_set(zz_isneg(u) ? -1 : 0, w);
    }
    return zz_quo_2exp(u, shift, w);
}

zz_err
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
        zz_err ret = ZZ_OK;

        if (zz_init(&tmp) || (ret = zz_add(exp, shift, exp))) {
            /* LCOV_EXCL_START */
            zz_clear(&tmp);
            return ret;
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
    zz_err ret = ZZ_OK;

    if (zz_init(&tmp) || (ret = zz_add(exp, zbits, exp))) {
        /* LCOV_EXCL_START */
        zz_clear(&tmp);
        return ret;
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

#define MAKE_MIX_LOGIC(func, op)                          \
    zz_err                                                \
    zz_##func##_##i64(const zz_t *u, int64_t v, zz_t *w)  \
    {                                                     \
        int64_t iu;                                       \
                                                          \
        if (zz_get_##i64(u, &iu)) {                       \
            zz_t tmp;                                     \
                                                          \
            if (zz_init(&tmp) || zz_set_##i64(v, &tmp)) { \
                /* LCOV_EXCL_START */                     \
                zz_clear(&tmp);                           \
                return ZZ_MEM;                            \
                /* LCOV_EXCL_STOP */                      \
            }                                             \
                                                          \
            zz_err ret = zz_##func(u, &tmp, w);           \
                                                          \
            zz_clear(&tmp);                               \
            return ret;                                   \
        }                                                 \
        return zz_set_##i64(iu op v, w);                  \
    }

MAKE_MIX_LOGIC(and, &)
MAKE_MIX_LOGIC(or, |)
MAKE_MIX_LOGIC(xor, ^)
