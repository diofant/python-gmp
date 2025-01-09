import math
import platform

import pytest
from gmp import factorial, gcd, isqrt, mpz
from hypothesis import given
from hypothesis.strategies import integers


@given(integers(min_value=0))
def test_isqrt(x):
    mx = mpz(x)
    r = math.isqrt(x)
    assert isqrt(mx) == isqrt(x) == r


@given(integers(min_value=0, max_value=12345))
def test_factorial(x):
    mx = mpz(x)
    r = math.factorial(x)
    try:
        assert factorial(mx) == factorial(x) == r
    except MemoryError:
        pass


@pytest.mark.skipif(platform.system() != "Linux",
                    reason="FIXME: setrlimit fails with ValueError on MacOS")
@pytest.mark.skipif(platform.python_implementation() == "PyPy",
                    reason="XXX: bug in PyNumber_ToBase()?")
def test_factorial_outofmemory():
    import random
    import resource

    for _ in range(100):
        soft, hard = resource.getrlimit(resource.RLIMIT_AS)
        resource.setrlimit(resource.RLIMIT_AS, (1024*32*1024, hard))
        a = random.randint(12811, 24984)
        a = mpz(a)
        while True:
            try:
                factorial(a)
                a *= 2
            except MemoryError:
                break
        resource.setrlimit(resource.RLIMIT_AS, (soft, hard))


@given(integers(), integers())
def test_gcd(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = math.gcd(x, y)
    assert gcd(mx, my) == r
    assert gcd(x, my) == r
    assert gcd(mx, y) == r


def test_interfaces():
    assert gcd() == 0
    with pytest.raises(TypeError):
        gcd(1j)
    with pytest.raises(TypeError):
        gcd(1, 1j)
    with pytest.raises(TypeError):
        isqrt(1j)
    with pytest.raises(ValueError):
        isqrt(-1)
    with pytest.raises(TypeError):
        factorial(1j)
    with pytest.raises(ValueError):
        factorial(-1)
    with pytest.raises(OverflowError):
        factorial(2**1000)
