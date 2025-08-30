import inspect
import math
import platform
import sys

import gmp
import pytest
from gmp import (
    _mpmath_create,
    _mpmath_normalize,
    comb,
    double_fac,
    fac,
    factorial,
    fib,
    gcd,
    gcdext,
    isqrt,
    isqrt_rem,
    lcm,
    mpz,
    perm,
)
from hypothesis import example, given
from hypothesis.strategies import booleans, integers, lists, sampled_from
from test_utils import (
    bigints,
    python_fac2,
    python_fib,
    python_gcdext,
    python_isqrtrem,
)


@given(bigints(min_value=0))
def test_isqrt(x):
    mx = mpz(x)
    for fm, f in [(isqrt, math.isqrt), (isqrt_rem, python_isqrtrem)]:
        r = f(x)
        assert fm(mx) == r
        assert fm(x) == r


@given(integers(min_value=0, max_value=12345))
def test_factorials(x):
    mx = mpz(x)
    for fm, f in [(factorial, math.factorial), (fib, python_fib),
                  (double_fac, python_fac2)]:
        r = f(x)
        assert fm(mx) == r
        assert fm(x) == r


@given(integers(min_value=0, max_value=12345),
       integers(min_value=0, max_value=12345))
def test_comb(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = math.comb(x, y)
    assert comb(mx, my) == r
    assert comb(mx, y) == r
    assert comb(x, my) == r
    assert comb(x, y) == r


@given(integers(min_value=0, max_value=12345),
       integers(min_value=0, max_value=12345))
def test_perm(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = math.perm(x, y)
    assert perm(mx, my) == r
    assert perm(mx, y) == r
    assert perm(x, my) == r
    assert perm(x, y) == r
    rx = math.factorial(x)
    assert perm(mx) == rx
    assert perm(x) == rx


@given(bigints(), bigints(), bigints())
@example(1<<(67*2), 1<<65, 1)
@example(123, 1<<70, 1)
@example(6277101735386680763835789423207666416102355444464034512895,
         6277101735386680763835789423207666416102355444464034512895,
         340282366920938463463374607431768211456)
def test_gcdext_binary(x, y, c):
    x *= c
    y *= c
    mx = mpz(x)
    my = mpz(y)
    for fm, f in [(gcd, math.gcd), (gcdext, python_gcdext)]:
        r = f(x, y)
        assert fm(mx, my) == r
        assert fm(x, my) == r
        assert fm(mx, y) == r
        assert fm(x, y) == r


@given(bigints(), bigints())
def test_lcm_binary(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = math.lcm(x, y)
    assert lcm(mx, my) == r
    assert lcm(mx, y) == r
    assert lcm(x, my) == r
    assert lcm(x, y) == r


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
    assert gcd(*xs) == r


@given(lists(bigints(), max_size=6))
@example([])
def test_lcm_nary(xs):
    mxs = list(map(mpz, xs))
    r = math.lcm(*xs)
    assert lcm(*mxs) == r
    assert lcm(*xs) == r


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
def test_mpmath_normalize(sign, man, exp, prec, rnd):
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
def test_mpmath_create(man, exp, prec, rnd):
    mpmath = pytest.importorskip("mpmath")
    mman = mpz(man)
    res = mpmath.libmp.from_man_exp(man, exp, prec, rnd)
    assert _mpmath_create(mman, exp, prec, rnd) == res
    assert mman == man
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
        lcm(1j)
    with pytest.raises(TypeError):
        lcm(1, 1j)
    with pytest.raises(TypeError):
        isqrt(1j)
    with pytest.raises(TypeError):
        isqrt_rem(1j)
    with pytest.raises(ValueError, match="argument must be nonnegative"):
        isqrt(-1)
    with pytest.raises(ValueError, match="argument must be nonnegative"):
        isqrt_rem(-1)
    with pytest.raises(TypeError):
        fac(1j)
    with pytest.raises(ValueError, match="not defined for negative values"):
        fac(-1)
    with pytest.raises(OverflowError):
        fac(2**1000)
    with pytest.raises(TypeError):
        comb(123)
    with pytest.raises(ValueError, match="not defined for negative values"):
        comb(-1, 2)
    with pytest.raises(ValueError, match="not defined for negative values"):
        comb(2, -1)
    with pytest.raises(OverflowError):
        comb(2**1000, 1)
    with pytest.raises(OverflowError):
        comb(1, 2**1000)
    with pytest.raises(TypeError):
        perm(1, 2, 3)
    with pytest.raises(ValueError, match="not defined for negative values"):
        perm(-1, 2)
    with pytest.raises(ValueError, match="not defined for negative values"):
        perm(2, -1)
    with pytest.raises(OverflowError):
        perm(2**1000, 1)
    with pytest.raises(OverflowError):
        perm(1, 2**1000)
    with pytest.raises(TypeError):
        _mpmath_create(1j)
    with pytest.raises(TypeError):
        _mpmath_create(mpz(123))
    with pytest.raises(TypeError):
        _mpmath_create("!", 1)
    with pytest.raises(TypeError):
        _mpmath_create(mpz(123), 10, 1j)
    with pytest.raises(ValueError, match="invalid rounding mode specified"):
        _mpmath_create(mpz(123), 10, 3, 1j)
    with pytest.raises(TypeError):
        _mpmath_create(mpz(123), 1j, 3, "c")
    with pytest.raises(TypeError):
        _mpmath_normalize(123)
    with pytest.raises(TypeError):
        _mpmath_normalize(1, 111, 11, 12, 13, "c")
    with pytest.raises(ValueError, match="invalid rounding mode specified"):
        _mpmath_normalize(1, mpz(111), 11, 12, 13, "q")
    with pytest.raises(TypeError):
        _mpmath_normalize(1, mpz(111), 1j, 12, 13, "c")
    with pytest.raises(ValueError, match="invalid rounding mode specified"):
        _mpmath_normalize(1, mpz(111), 11, 12, 13, 1j)
    gmp._free_cache()  # just for coverage


@pytest.mark.skipif(platform.python_implementation() != "CPython"
                    or sys.version_info < (3, 11),
                    reason="no way to specify a signature")
def test_func_api():
    for fn in ["comb", "factorial", "gcd", "isqrt", "lcm", "perm"]:
        f = getattr(math, fn)
        fz = getattr(gmp, fn)
        assert inspect.signature(f) == inspect.signature(fz)
