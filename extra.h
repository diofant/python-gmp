#ifndef EXTRA_H
#define EXTRA_H

#include "zz/zz.h"

extern uint8_t bits_per_digit;

zz_err zz_get_bytes(const zz_t *u, size_t len, bool is_signed,
                    unsigned char **buf);
zz_err zz_set_bytes(const unsigned char *buf, size_t len,
                    bool is_signed, zz_t *u);

#define zz_quo(u, v, w) zz_div((u), (v), (w), NULL)
#define zz_rem(u, v, w) zz_div((u), (v), NULL, (w))

zz_err zz_divnear(const zz_t *u, const zz_t *v, zz_t *q, zz_t *r);

zz_err zz_truediv(const zz_t *u, const zz_t *v, double *w);
zz_err zz_lshift(const zz_t *u, const zz_t *v, zz_t *w);
zz_err zz_rshift(const zz_t *u, const zz_t *v, zz_t *w);

typedef enum {
    ZZ_RNDD = 0,
    ZZ_RNDN = 1,
    ZZ_RNDU = 2,
    ZZ_RNDZ = 3,
    ZZ_RNDA = 4,
} zz_rnd;

zz_err zz_mpmath_normalize(zz_bitcnt_t prec, zz_rnd rnd, bool *negative,
                           zz_t *man, zz_t *exp, zz_bitcnt_t *bc);

zz_err zz_and_i64(const zz_t *u, int64_t v, zz_t *w);
zz_err zz_or_i64(const zz_t *u, int64_t v, zz_t *w);
zz_err zz_xor_i64(const zz_t *u, int64_t v, zz_t *w);

static inline zz_err
zz_i64_and(int64_t u, const zz_t *v, zz_t *w)
{
    return zz_and_i64(v, u, w);
}

#define zz_and(U, V, W)                                  \
    _Generic((U),                                        \
             int: _Generic((V),                          \
                           default: zz_i64_and),         \
             long: _Generic((V),                         \
                            default: zz_i64_and),        \
             long long: _Generic((V),                    \
                                 default: zz_i64_and),   \
             default: _Generic((V),                      \
                               int: zz_and_i64,          \
                               long: zz_and_i64,         \
                               long long: zz_and_i64,    \
                               default: zz_and))(U, V, W)

static inline zz_err
zz_i64_or(int64_t u, const zz_t *v, zz_t *w)
{
    return zz_or_i64(v, u, w);
}

#define zz_or(U, V, W)                                  \
    _Generic((U),                                       \
             int: _Generic((V),                         \
                           default: zz_i64_or),         \
             long: _Generic((V),                        \
                            default: zz_i64_or),        \
             long long: _Generic((V),                   \
                                 default: zz_i64_or),   \
             default: _Generic((V),                     \
                               int: zz_or_i64,          \
                               long: zz_or_i64,         \
                               long long: zz_or_i64,    \
                               default: zz_or))(U, V, W)

static inline zz_err
zz_i64_xor(int64_t u, const zz_t *v, zz_t *w)
{
    return zz_xor_i64(v, u, w);
}

#define zz_xor(U, V, W)                                  \
    _Generic((U),                                        \
             int: _Generic((V),                          \
                           default: zz_i64_xor),         \
             long: _Generic((V),                         \
                            default: zz_i64_xor),        \
             long long: _Generic((V),                    \
                                 default: zz_i64_xor),   \
             default: _Generic((V),                      \
                               int: zz_xor_i64,          \
                               long: zz_xor_i64,         \
                               long long: zz_xor_i64,    \
                               default: zz_xor))(U, V, W)

#endif /* EXTRA_H */
