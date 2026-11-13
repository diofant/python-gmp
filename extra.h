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

#endif /* EXTRA_H */
