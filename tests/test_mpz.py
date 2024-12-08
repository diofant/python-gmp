import math
import operator
import pickle
import platform
import random
import resource
import sys

import pytest
from hypothesis import assume, example, given
from hypothesis.strategies import booleans, integers, sampled_from

from gmp import mpz
from gmp import _limb_size as limb_size


@given(integers())
@example(0)
@example(123)
@example(75424656551107706)
@example(1284673497348563845623546741523784516734143215346712)
@example(65869376547959985897597359)
def test_mpz_from_to_str(x):
    sx = str(x)
    mx = mpz(sx)
    assert str(mx) == sx


@given(integers())
@example(0)
@example(123)
@example(75424656551107706)
@example(1284673497348563845623546741523784516734143215346712)
@example(65869376547959985897597359)
def test_mpz_from_to_int(x):
    sx = str(x)
    mx = mpz(x)
    assert mpz(sx) == mpz(mx) == mx == x
    assert int(mx) == x


@given(integers())
def test_repr(x):
    mx = mpz(x)
    assert repr(mx) == f"mpz({x!s})"


@given(integers(), integers())
def test_richcompare(x, y):
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


@given(integers())
@example(0)
@example(123)
@example(75424656551107706)
@example(1284673497348563845623546741523784516734143215346712)
@example(65869376547959985897597359)
def test_mpz_plus_minus_abs(x):
    mx = mpz(x)
    assert +mx == x
    assert -mx == -x
    assert abs(mx) == abs(x)


@given(integers(), integers())
def test_add_sub(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = x + y
    assert mx + my == r
    assert mx + y == r
    assert x + my == r
    r = x - y
    assert mx - my == r
    assert mx - y == r
    assert x - my == r


@given(integers(), integers())
def test_mul(x, y):
    mx = mpz(x)
    my = mpz(y)
    assert mx * mx == x * x
    r = x * y
    assert mx * my == r
    assert mx * y == r
    assert x * my == r


@given(integers(), integers())
def test_divmod(x, y):
    if not y:
        return
    mx = mpz(x)
    my = mpz(y)
    r = x // y
    assert mx // my == r
    assert mx // y == r
    assert x // my == r
    r = x % y
    assert mx % my == r
    assert mx % y == r
    assert x % my == r
    r = divmod(x, y)
    assert divmod(mx, my) == r


@given(integers(min_value=-1000000, max_value=1000000),
       integers(min_value=0, max_value=100000))
@example(123, 0)
@example(-321, 0)
@example(1, 123)
@example(-1, 123)
@example(123, 321)
@example(-56, 321)
def test_power(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = x**y
    assert mx**my == r
    assert mx**y == r
    assert x**my == r


@given(integers())
def test_invert(x):
    mx = mpz(x)
    assert ~mx == ~x


@given(integers(), integers())
def test_and(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = x & y
    assert mx & my == r
    assert mx & y == r
    assert x & my == r


@given(integers(), integers())
def test_or(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = x | y
    assert mx | my == r
    assert mx | y == r
    assert x | my == r


@given(integers(), integers())
def test_xor(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = x ^ y
    assert mx ^ my == r
    assert mx ^ y == r
    assert x ^ my == r


@given(integers())
def test_getseters(x):
    mx = mpz(x)
    assert mx.numerator == x.numerator
    assert mx.denominator == x.denominator
    assert mx.real == x.real
    assert mx.imag == x.imag


@given(integers())
def test_methods(x):
    mx = mpz(x)
    assert mx.conjugate() == x.conjugate()
    assert mx.bit_length() == x.bit_length()
    if sys.version_info >= (3, 10):
        assert mx.bit_count() == x.bit_count()
    assert mx.as_integer_ratio() == x.as_integer_ratio()
    if sys.version_info >= (3, 12):
        assert mx.is_integer() == x.is_integer()
    assert math.trunc(mx) == math.trunc(x)
    assert math.floor(mx) == math.floor(x)
    assert math.ceil(mx) == math.ceil(x)


@pytest.mark.xfail(reason="https://github.com/diofant/python-gmp/issues/3")
@given(integers(), integers(min_value=0, max_value=10000),
       sampled_from(['big', 'little']), booleans())
@example(0, 0, 'big', False)
@example(0, 0, 'little', False)
@example(0, 1, 'big', False)
@example(128, 1, 'big', True)
@example(128, 1, 'little', True)
@example(-129, 1, 'big', True)
@example(-129, 1, 'little', True)
@example(-1, 0, 'big', True)
@example(-2, 0, 'big', True)
@example(-2, 0, 'little', True)
@example(42, 1, 'big', False)
@example(42, 1, 'little', False)
@example(42, 3, 'big', False)
@example(42, 3, 'little', False)
@example(1000, 2, 'big', False)
@example(1000, 4, 'big', False)
@example(-2049, 1, 'big', True)
@example(-65281, 3, 'big', True)
@example(-65281, 3, 'little', True)
def test_to_bytes(x, length, byteorder, signed):
    try:
        rx = x.to_bytes(length, byteorder, signed=signed)
    except OverflowError:
        with pytest.raises(OverflowError):
            mpz(x).to_bytes(length, byteorder, signed=signed)
    else:
        assert rx == mpz(x).to_bytes(length, byteorder, signed=signed)


@pytest.mark.xfail(reason="https://github.com/diofant/python-gmp/issues/3")
@given(integers(), integers(min_value=0, max_value=10000),
       sampled_from(['big', 'little']), booleans())
@example(0, 0, 'big', False)
@example(0, 0, 'little', False)
@example(0, 1, 'big', False)
@example(128, 1, 'big', True)
@example(128, 1, 'little', True)
@example(-129, 1, 'big', True)
@example(-129, 1, 'little', True)
@example(-1, 0, 'big', True)
@example(-1, 1, 'big', True)
@example(-1, 1, 'little', True)
@example(-2, 0, 'big', True)
@example(-2, 0, 'little', True)
@example(-1, 3, 'big', True)
@example(-2, 3, 'big', True)
@example(-2, 5, 'little', True)
def test_from_bytes(x, length, byteorder, signed):
    try:
        bytes = x.to_bytes(length, byteorder, signed=signed)
    except OverflowError:
        assume(False)
    else:
        rx = int.from_bytes(bytes, byteorder, signed=signed)
        assert rx == mpz.from_bytes(bytes, byteorder, signed=signed)
        assert rx == mpz.from_bytes(bytearray(bytes), byteorder, signed=signed)
        assert rx == mpz.from_bytes(list(bytes), byteorder, signed=signed)


@pytest.mark.xfail(reason="https://github.com/diofant/python-gmp/issues/2")
@given(integers())
@example(117529601297931785)
def test___float__(x):
    mx = mpz(x)
    try:
        fx = float(x)
    except OverflowError:
        pytest.raises(OverflowError, lambda: float(mx))
    else:
        assert float(mx) == fx


@pytest.mark.xfail(reason="https://github.com/diofant/python-gmp/issues/4")
@given(integers(), integers(min_value=-20, max_value=30))
def test___round__(x, n):
    mx = mpz(x)
    mn = mpz(n)
    assert round(mx, n) == round(mx, mn) == round(x, n)
    if not n:
        assert round(mx) == round(x)


@pytest.mark.skipif(platform.python_implementation() == 'PyPy',
                    reason="sys.getsizeof raises TypeError")
def test___sizeof__():
    ms = [mpz(1<<i*(8*limb_size)) for i in range(3)]
    sz = sys.getsizeof(ms[1]) - sys.getsizeof(ms[0])
    assert sys.getsizeof(ms[2]) - sys.getsizeof(ms[1]) == sz


@given(integers(), integers(min_value=2, max_value=62))
def test_digits(x, base):
    gmpy2 = pytest.importorskip('gmpy2')
    mx = mpz(x)
    gx = gmpy2.mpz(x)
    res = gx.digits(base)
    assert mx.digits(base) == res


@given(integers(), integers(min_value=2, max_value=36))
def test_digits_frombase_low(x, base):
    mx = mpz(x)
    smx = mx.digits(base)
    assert mpz(smx, base) == mx
    assert mpz(smx.upper(), base) == mx
    assert int(smx, base) == mx
    smaller_base = (base + 2)//2 + 1
    try:
        i = int(smx, smaller_base)
    except ValueError:
        with pytest.raises(ValueError):
            mpz(smx, smaller_base)
    else:
        assert mpz(smx, smaller_base) == i


@given(integers(), integers(min_value=37, max_value=62))
def test_digits_frombase_high(x, base):
    gmpy2 = pytest.importorskip('gmpy2')
    mx = mpz(x)
    smx = mx.digits(base)
    assert mpz(smx, base) == mx
    assert int(gmpy2.mpz(smx, base)) == mx
    smaller_base = (base + 2)//2 + 1
    try:
        g = gmpy2.mpz(smx, smaller_base)
    except ValueError:
        with pytest.raises(ValueError):
            mpz(smx, smaller_base)
    else:
        assert mpz(smx, smaller_base) == int(g)


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


def test_outofmemory():
    resource.setrlimit(resource.RLIMIT_AS, (1024*32*1024, -1))
    total = 20
    for n in range(total):
        a = random.randint(49846727467293, 249846727467293)
        a = mpz(a)
        i = 1
        while True:
            try:
                a = a*a
            except MemoryError:
                assert i > 5
                break
            i += 1
    assert n + 1 == total
    resource.setrlimit(resource.RLIMIT_AS, (-1, -1))
