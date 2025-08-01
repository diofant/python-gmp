import math

import gmp
import pytest
from gmp import (
    _mpmath_create,
    _mpmath_normalize,
    double_fac,
    fac,
    factorial,
    fib,
    gcd,
    gcdext,
    isqrt,
    isqrt_rem,
    mpz,
)
from hypothesis import example, given
from hypothesis.strategies import booleans, integers, lists, sampled_from
from test_utils import bigints, python_gcdext


@given(bigints(min_value=0))
def test_isqrt_root(x):
    mx = mpz(x)
    r = math.isqrt(x)
    assert isqrt(mx) == r
    assert isqrt(x) == r


@given(bigints(min_value=0))
def test_isqrt_rem(x):
    mpmath = pytest.importorskip("mpmath")
    mx = mpz(x)
    r = mpmath.libmp.libintmath.sqrtrem_python(x)
    assert isqrt_rem(mx) == r
    assert isqrt_rem(x) == r


@given(integers(min_value=0, max_value=12345))
def test_double_fac(x):
    mpmath = pytest.importorskip("mpmath")
    mx = mpz(x)
    r = mpmath.libmp.libintmath.ifac2_python(x)
    assert double_fac(mx) == r
    assert double_fac(x) == r


@given(integers(min_value=0, max_value=12345))
def test_fib(x):
    mpmath = pytest.importorskip("mpmath")
    mx = mpz(x)
    r = mpmath.libmp.libintmath.ifib_python(x)
    assert fib(mx) == r
    assert fib(x) == r


@given(integers(min_value=0, max_value=12345))
def test_fac_bulk(x):
    mx = mpz(x)
    r = math.factorial(x)
    assert factorial(mx) == r
    assert factorial(x) == r


@given(bigints(), bigints(), bigints())
@example(1<<(67*2), 1<<65, 1)
@example(6277101735386680763835789423207666416102355444464034512895,
         6277101735386680763835789423207666416102355444464034512895,
         340282366920938463463374607431768211456)
def test_gcd_binary(x, y, c):
    x *= c
    y *= c
    mx = mpz(x)
    my = mpz(y)
    r = math.gcd(x, y)
    assert gcd(mx, my) == r
    assert gcd(x, my) == r
    assert gcd(mx, y) == r


@given(lists(bigints(), max_size=6), bigints())
@example([], 1)
@example([2, 3, 4], 1)
@example([18446744073709551616, 18446744073709551616,
          -340282366920938463446414324134825139571], -18446744073709551615)
def test_gcd_nary(xs, c):
    xs = [_*c for _ in xs]
    mxs = list(map(mpz, xs))
    r = math.gcd(*xs)
    assert gcd(*mxs) == r


@given(bigints(), bigints(), bigints())
@example(1<<(67*2), 1<<65, 1)
@example(123, 1<<70, 1)
def test_gcdext(x, y, c):
    x *= c
    y *= c
    mx = mpz(x)
    my = mpz(y)
    r = python_gcdext(x, y)
    r = r[2], r[0], r[1]
    assert gcdext(mx, my) == r
    assert gcdext(x, my) == r
    assert gcdext(mx, y) == r


@given(booleans(), bigints(min_value=0), bigints(),
       integers(min_value=1, max_value=1<<30),
       sampled_from(["n", "f", "c", "u", "d"]))
@example(0, 232, -4, 4, "n")
@example(0, 9727076909039105, -48, 53, "n")
@example(0, 9727076909039105, -48, 53, "f")
@example(1, 9727076909039105, -48, 53, "f")
@example(0, 9727076909039105, -48, 53, "c")
@example(1, 9727076909039105, -48, 53, "c")
@example(1, 9727076909039105, -48, 53, "d")
@example(1, 9727076909039105, -48, 53, "u")
@example(1, 6277101735386680763495507056286727952638980837032266301441,
         0, 64, "f")
def test__mpmath_normalize(sign, man, exp, prec, rnd):
    mpmath = pytest.importorskip("mpmath")
    mman = mpz(man)
    sign = int(sign)
    bc = mman.bit_length()
    res = mpmath.libmp.libmpf._normalize(sign, man, exp, bc, prec, rnd)
    assert _mpmath_normalize(sign, mman, exp, bc, prec, rnd) == res


@given(bigints(), bigints(),
       integers(min_value=0, max_value=1<<30),
       sampled_from(["n", "f", "c", "u", "d"]))
@example(-6277101735386680763495507056286727952638980837032266301441,
         0, 64, "f")
def test__mpmath_create(man, exp, prec, rnd):
    mpmath = pytest.importorskip("mpmath")
    mman = mpz(man)
    res = mpmath.libmp.from_man_exp(man, exp, prec, rnd)
    assert _mpmath_create(mman, exp, prec, rnd) == res
    assert _mpmath_create(man, exp, prec, rnd) == res


def test_interfaces():
    assert factorial(123) == fac(123)
    with pytest.raises(TypeError):
        gcd(1j)
    with pytest.raises(TypeError):
        gcd(1, 1j)
    with pytest.raises(TypeError):
        gcdext(1)
    with pytest.raises(TypeError):
        gcdext(2, 1j)
    with pytest.raises(TypeError):
        gcdext(2j, 2)
    with pytest.raises(TypeError):
        isqrt(1j)
    with pytest.raises(TypeError):
        isqrt_rem(1j)
    with pytest.raises(ValueError):
        isqrt(-1)
    with pytest.raises(ValueError):
        isqrt_rem(-1)
    with pytest.raises(TypeError):
        fac(1j)
    with pytest.raises(ValueError):
        fac(-1)
    with pytest.raises(OverflowError):
        fac(2**1000)
    with pytest.raises(TypeError):
        _mpmath_create(1j)
    with pytest.raises(TypeError):
        _mpmath_create(mpz(123))
    with pytest.raises(TypeError):
        _mpmath_create("!", 1)
    with pytest.raises(TypeError):
        _mpmath_create(mpz(123), 10, 1j)
    with pytest.raises(ValueError):
        _mpmath_create(mpz(123), 10, 3, 1j)
    with pytest.raises(TypeError):
        _mpmath_create(mpz(123), 1j, 3, "c")
    with pytest.raises(TypeError):
        _mpmath_normalize(123)
    with pytest.raises(TypeError):
        _mpmath_normalize(1, 111, 11, 12, 13, "c")
    with pytest.raises(ValueError):
        _mpmath_normalize(1, mpz(111), 11, 12, 13, "q")
    with pytest.raises(TypeError):
        _mpmath_normalize(1, mpz(111), 1j, 12, 13, "c")
    with pytest.raises(ValueError):
        _mpmath_normalize(1, mpz(111), 11, 12, 13, 1j)
    gmp._free_cache()
