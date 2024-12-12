import math

import pytest
from hypothesis import given
from hypothesis.strategies import integers

from gmp import mpz, factorial, gcd, isqrt


@given(integers(min_value=0))
def test_isqrt(x):
    mx = mpz(x)
    r = math.isqrt(x)
    assert isqrt(mx) == isqrt(x) == r


@pytest.mark.xfail(reason="diofant/python-gmp#12")
@given(integers(min_value=0, max_value=12345))
def test_factorial(x):
    mx = mpz(x)
    r = math.factorial(x)
    assert factorial(mx) == factorial(x) == r


@given(integers(), integers())
def test_gcd(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = math.gcd(x, y)
    assert gcd(mx, my) == r
    assert gcd(x, my) == r
    assert gcd(mx, y) == r
