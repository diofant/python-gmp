#ifndef ZZ_H
#define ZZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int32_t zz_size_t;
#ifndef __APPLE__
typedef uint64_t zz_limb_t;
#else
typedef unsigned long zz_limb_t;
#endif
typedef uint64_t zz_bitcnt_t;

typedef enum {
    ZZ_OK = 0,
    ZZ_MEM = -1,
    ZZ_VAL = -2,
    ZZ_BUF = -3,
} zz_err;

typedef struct {
    uint8_t version[3];
    uint8_t bits_per_limb;
    uint8_t limb_bytes;
    uint8_t limbcnt_bytes;
    uint8_t bitcnt_bytes;
} zz_info;

zz_err zz_setup(zz_info *info);
void zz_finish(void);

typedef struct {
    bool negative;
    zz_size_t alloc;
    zz_size_t size;
    zz_limb_t *digits;
} zz_t;

zz_err zz_init(zz_t *u);
zz_err zz_resize(int64_t size, zz_t *u);
void zz_clear(zz_t *u);

typedef enum {
    ZZ_GT = +1,
    ZZ_EQ = 0,
    ZZ_LT = -1,
} zz_ord;

zz_ord zz_cmp(const zz_t *u, const zz_t *v);
zz_ord zz_cmp_i32(const zz_t *u, int32_t v);

zz_err zz_from_i32(int32_t u, zz_t *v);
zz_err zz_to_i32(const zz_t *u, int32_t *v);
zz_err zz_from_i64(int64_t u, zz_t *v);
zz_err zz_to_i64(const zz_t *u, int64_t *v);

bool zz_iszero(const zz_t *u);
bool zz_isneg(const zz_t *u);
bool zz_isodd(const zz_t *u);

zz_err zz_copy(const zz_t *u, zz_t *v);
zz_err zz_abs(const zz_t *u, zz_t *v);
zz_err zz_neg(const zz_t *u, zz_t *v);

zz_err zz_sizeinbase(const zz_t *u, int8_t base, size_t *len);
zz_err zz_to_str(const zz_t *u, int8_t base, int8_t *str, size_t *len);
zz_err zz_from_str(const int8_t *str, size_t len, int8_t base, zz_t *u);

zz_err zz_to_double(const zz_t *u, double *d);

zz_err zz_to_bytes(const zz_t *u, size_t length, bool is_signed,
                   uint8_t **buffer);
zz_err zz_from_bytes(const uint8_t *buffer, size_t length,
                     bool is_signed, zz_t *u);

zz_bitcnt_t zz_bitlen(const zz_t *u);
zz_bitcnt_t zz_lsbpos(const zz_t *u);
zz_bitcnt_t zz_bitcnt(const zz_t *u);

typedef struct {
    uint8_t bits_per_digit;
    uint8_t digit_size;
    int8_t digits_order;
    int8_t digit_endianness;
} zz_layout;

zz_err zz_import(size_t len, const void *digits, zz_layout layout, zz_t *u);
zz_err zz_export(const zz_t *u, zz_layout layout, size_t len, void *digits);

zz_err zz_add(const zz_t *u, const zz_t *v, zz_t *w);
zz_err zz_add_i32(const zz_t *u, int32_t v, zz_t *w);
zz_err zz_sub(const zz_t *u, const zz_t *v, zz_t *w);

zz_err zz_mul(const zz_t *u, const zz_t *v, zz_t *w);

typedef enum {
    ZZ_RNDD = 0,
    ZZ_RNDN = 1,
    ZZ_RNDU = 2,
    ZZ_RNDZ = 3,
    ZZ_RNDA = 4,
} zz_rnd;

zz_err zz_div(const zz_t *u, const zz_t *v, zz_rnd rnd, zz_t *q, zz_t *r);

zz_err zz_rem_u64(const zz_t* u, uint64_t v, uint64_t *w);

zz_err zz_quo_2exp(const zz_t *u, uint64_t rshift, zz_t *v);
zz_err zz_mul_2exp(const zz_t *u, uint64_t lshift, zz_t *v);

zz_err zz_truediv(const zz_t *u, const zz_t *v, double *res);

zz_err zz_invert(const zz_t *u, zz_t *v);
zz_err zz_and(const zz_t *u, const zz_t *v, zz_t *w);
zz_err zz_or(const zz_t *u, const zz_t *v, zz_t *w);
zz_err zz_xor(const zz_t *u, const zz_t *v, zz_t *w);

zz_err zz_pow(const zz_t *u, uint64_t v, zz_t *w);

zz_err zz_gcd(const zz_t *u, const zz_t *v, zz_t *w);
zz_err zz_gcdext(const zz_t *u, const zz_t *v, zz_t *g, zz_t *s, zz_t *t);
zz_err zz_inverse(const zz_t *u, const zz_t *v, zz_t *w);

zz_err zz_powm(const zz_t *u, const zz_t *v, const zz_t *w, zz_t *res);

zz_err zz_sqrtrem(const zz_t *u, zz_t *v, zz_t *w);

zz_err zz_fac(uint64_t u, zz_t *v);
zz_err zz_fac2(uint64_t u, zz_t *v);
zz_err zz_fib(uint64_t u, zz_t *v);

zz_err _zz_mpmath_normalize(zz_bitcnt_t prec, zz_rnd rnd, bool *negative,
                            zz_t *man, zz_t *exp, zz_bitcnt_t *bc);

#endif /* ZZ_H */
