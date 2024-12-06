import math
import operator
import pickle
import platform
import sys

import pytest
from hypothesis import example, given, settings
from hypothesis.strategies import integers

from gmp import isqrt, gcd, mpz
from gmp import _limb_size as limb_size


@given(integers(min_value=0))
@example(0)
@example(123)
@example(75424656551107706)
@example(1284673497348563845623546741523784516734143215346712)
@example(65869376547959985897597359)
def test_mpz_from_to_str(x):
    sx = str(x)
    snx = str(-x)
    mx = mpz(sx)
    mnx = mpz(snx)
    assert str(mx) == sx
    assert str(mnx) == snx


@given(integers(min_value=0))
@example(0)
@example(123)
@example(75424656551107706)
@example(1284673497348563845623546741523784516734143215346712)
@example(65869376547959985897597359)
def test_mpz_from_to_int(x):
    sx = str(x)
    snx = str(-x)
    mx = mpz(x)
    mnx = mpz(-x)
    assert mpz(mx) == mx == x
    assert mpz(mnx) == mnx == -x
    assert int(mx) == x
    assert int(mnx) == -x


@given(integers(), integers())
def test_mpz_richcompare(x, y):
    mx = mpz(x)
    my = mpz(y)

    for op in [operator.eq, operator.ne, operator.lt, operator.le,
               operator.gt, operator.ge]:
        assert op(mx, my) == op(x, y)

    assert bool(mx) == bool(x)


@given(integers())
@example(0)
@example(-1)
@example(-2)
def test_mpz_hash(x):
    mx = mpz(x)
    assert hash(mx) == hash(x)


@given(integers(min_value=0))
@example(0)
@example(123)
@example(75424656551107706)
@example(1284673497348563845623546741523784516734143215346712)
@example(65869376547959985897597359)
def test_mpz_plus_minus_abs(x):
    mx = mpz(x)
    mnx = mpz(-x)
    assert +mx == x
    assert +mnx == -x
    assert -mx == -x
    assert -mnx == x
    assert abs(mx) == abs(x)
    assert abs(mnx) == abs(x)


@given(integers(), integers())
def test_add_sub(x, y):
    r = x + y
    assert mpz(x) + mpz(y) == r
    assert mpz(x) + y == r
    assert x + mpz(y) == r
    r = x - y
    assert mpz(x) - mpz(y) == r
    assert mpz(x) - y == r
    assert x - mpz(y) == r


@given(integers(), integers())
def test_mul(x, y):
    assert mpz(x) * mpz(x) == x * x
    r = x * y
    assert mpz(x) * mpz(y) == r
    assert mpz(x) * y == r
    assert x * mpz(y) == r


@given(integers(), integers())
def test_divmod(x, y):
    if not y:
        return
    r = x // y
    assert mpz(x) // mpz(y) == r
    assert mpz(x) // y == r
    assert x // mpz(y) == r
    r = x % y
    assert mpz(x) % mpz(y) == r
    assert mpz(x) % y == r
    assert x % mpz(y) == r
    r = divmod(x, y)
    assert divmod(mpz(x), mpz(y)) == r


@given(integers(min_value=-1000000, max_value=1000000),
       integers(min_value=0, max_value=100000))
@example(123, 0)
@example(-321, 0)
@example(1, 123)
@example(-1, 123)
@example(123, 321)
@example(-56, 321)
def test_power(x, y):
    assert mpz(x)**mpz(y) == x**y


@given(integers())
def test_invert(x):
    assert ~mpz(x) == ~x


@given(integers(), integers())
def test_and(x, y):
    r = x & y
    assert mpz(x) & mpz(y) == r
    assert mpz(x) & y == r
    assert x & mpz(y) == r


@given(integers(), integers())
def test_or(x, y):
    r = x | y
    assert mpz(x) | mpz(y) == r
    assert mpz(x) | y == r
    assert x | mpz(y) == r


@given(integers(), integers())
def test_xor(x, y):
    r = x ^ y
    assert mpz(x) ^ mpz(y) == r
    assert mpz(x) ^ y == r
    assert x ^ mpz(y) == r


@given(integers())
def test_getseters(x):
    mx = mpz(x)
    assert mx.numerator == x.numerator
    assert mx.denominator == x.denominator
    assert mx.real == x.real
    assert mx.imag == x.imag


@given(integers())
def test_methods(x):
    if sys.version_info < (3, 12):
        pytest.skip("is_integer or/and bit_count are missing")
    gmpy2 = pytest.importorskip('gmpy2')
    mx = mpz(x)
    gx = gmpy2.mpz(x)
    assert mx.conjugate() == x.conjugate()
    assert mx.bit_length() == x.bit_length()
    assert mx.bit_count() == x.bit_count()
    assert mx.as_integer_ratio() == x.as_integer_ratio()
    assert mx.is_integer() == x.is_integer()
    assert math.trunc(mx) == math.trunc(x)
    assert math.floor(mx) == math.floor(x)
    assert math.ceil(mx) == math.ceil(x)
    # XXX: doesn't match with CPython
    try:
        float(gx)
    except OverflowError:
        pytest.raises(OverflowError, lambda: float(mx))
    else:
        assert float(mx) == float(gx)


@given(integers(), integers())
def test_functions(x, y):
    mx = mpz(x)
    my = mpz(y)
    assert gcd(mx, my) == math.gcd(x, y)
    if x < 0:
        pytest.raises(ValueError, lambda: isqrt(mx))
    else:
        assert isqrt(mx) == math.isqrt(x)


@pytest.mark.skipif(platform.python_implementation() == 'PyPy',
                    reason="FIXME: a different signature on PyPy")
def test___sizeof__():
    ms = [mpz(1<<i*(8*limb_size)) for i in range(3)]
    sz = sys.getsizeof(ms[1]) - sys.getsizeof(ms[0])
    assert sys.getsizeof(ms[2]) - sys.getsizeof(ms[1]) == sz


@given(integers(), integers(min_value=2, max_value=62))
def test_digits(x, base):
    gmpy2 = pytest.importorskip('gmpy2')
    res = gmpy2.mpz(x).digits(base)
    mx = mpz(x)
    assert mx.digits(base) == res
    assert mx.digits(base=base) == res


@given(integers(), integers(min_value=2, max_value=62))
def test_digits_frombase(x, base):
    mx = mpz(x)
    smx = mx.digits(base)
    assert mpz(smx, base) == mx
    assert mpz(smx, base=base) == mx
    if base <= 36:
        assert mpz(smx.upper(), base) == mx
        assert mpz(smx.upper(), base=base) == mx
        assert int(smx, base) == mx
        smaller_base = (base + 2)//2 + 1
        try:
            i = int(smx, smaller_base)
        except ValueError:
            with pytest.raises(ValueError):
                mpz(smx, smaller_base)
        else:
            assert mpz(smx, smaller_base) == i


@given(integers())
def test_frombase_auto(x):
    mx = mpz(x)
    smx10 = mx.digits(10)
    if mx >= 0:
        smx2 = '0b' + mx.digits(2)
        smx8 = '0o' + mx.digits(8)
        smx16 = '0x' + mx.digits(16)
    else:
        smx2 = '-0b' + mx.digits(2)[1:]
        smx8 = '-0o' + mx.digits(8)[1:]
        smx16 = '-0x' + mx.digits(16)[1:]
    assert mpz(smx10, 0) == mx
    assert mpz(smx2, 0) == mx
    assert mpz(smx8, 0) == mx
    assert mpz(smx16, 0) == mx
    assert mpz(smx2.upper(), 0) == mx
    assert mpz(smx8.upper(), 0) == mx
    assert mpz(smx16.upper(), 0) == mx


@pytest.mark.parametrize('protocol',
                         range(2, pickle.HIGHEST_PROTOCOL + 1))
@given(integers())
def test_pickle(protocol, x):
    mx = mpz(x)
    assert mx == pickle.loads(pickle.dumps(mx, protocol))
