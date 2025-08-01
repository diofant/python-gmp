import cmath
import locale
import math
import operator
import pickle
import platform
import sys
import warnings
from concurrent.futures import ThreadPoolExecutor

import pytest
from gmp import mpz
from hypothesis import assume, example, given, settings
from hypothesis.strategies import (
    booleans,
    characters,
    complex_numbers,
    floats,
    integers,
    lists,
    sampled_from,
    text,
)
from test_utils import BITS_PER_LIMB, SIZEOF_LIMB, bigints, fmt_str, to_digits


class with_int:
    def __init__(self, value):
        self.value = value
    def __int__(self):
        try:
            exc = issubclass(self.value, Exception)
        except TypeError:
            exc = False
        if exc:
            raise self.value
        return self.value

class int2(int):
    pass

class mpz2(mpz):
    pass


@given(bigints())
@example(0)
@example(123)
@example(75424656551107706)
@example(1284673497348563845623546741523784516734143215346712)
@example(65869376547959985897597359)
def test_from_to_str(x):
    sx = str(x)
    mx = mpz(sx)
    assert str(mx) == sx


@given(text(alphabet=characters(min_codepoint=48, max_codepoint=57,
                                include_characters=["_"])))
def test_underscores_bulk(s):
    try:
        i = int(s)
    except ValueError:
        with pytest.raises(ValueError):
            mpz(s)
    else:
        assert mpz(s) == i


@given(text(alphabet=characters(min_codepoint=48, max_codepoint=57,
                                include_characters=["_", "a", "b",
                                                    "c", "d", "e", "f"])))
def test_underscores_auto(s):
    s = "0x" + s
    try:
        i = int(s, base=0)
    except ValueError:
        with pytest.raises(ValueError):
            mpz(s, base=0)
    else:
        assert mpz(s, base=0) == i




@given(bigints(), fmt_str())
@example(69, "r<-6_b")
@example(3351, "e=+8o")
@example(-3912, "1=28d")
@example(-3912, "0=28d")
@example(-3912, "028d")
@example(-3912, "028_d")
@example(-3912, "28n")
def test___format___bulk(x, fmt):
    mx = mpz(x)
    r = format(x, fmt)
    assert format(mx, fmt) == r


def test___format___interface():
    mx = mpz(123)
    pytest.raises(ValueError, lambda: format(mx, "q"))
    if platform.python_implementation() != "PyPy":  # XXX
        pytest.raises(ValueError, lambda: format(mx, "\x81"))
    pytest.raises(ValueError, lambda: format(mx, "zd"))
    pytest.raises(ValueError, lambda: format(mx, ".10d"))
    pytest.raises(ValueError, lambda: format(mx, "_n"))
    pytest.raises(ValueError, lambda: format(mx, ",n"))
    pytest.raises(ValueError, lambda: format(mx, "_c"))
    pytest.raises(ValueError, lambda: format(mx, "f=10dx"))
    pytest.raises(ValueError, lambda: format(mx, ".d"))
    pytest.raises(ValueError, lambda: format(mx, "f=10000000000000000000d"))
    pytest.raises(ValueError, lambda: format(mx, ".10000000000000000000f"))
    pytest.raises(ValueError, lambda: format(mx, ",_d"))
    pytest.raises(ValueError, lambda: format(mx, "_,d"))
    pytest.raises(ValueError, lambda: format(mx, ",x"))
    pytest.raises(ValueError, lambda: format(mx, ",\xa0"))
    pytest.raises(ValueError, lambda: format(mx, "+c"))
    pytest.raises(ValueError, lambda: format(mx, "#c"))
    pytest.raises(OverflowError, lambda: format(mpz(123456789), "c"))
    pytest.raises(OverflowError, lambda: format(mpz(10**100), "c"))
    pytest.raises(OverflowError, lambda: format(mpz(-1), "c"))
    pytest.raises(OverflowError, lambda: format(mpz(1<<32), "c"))
    if sys.version_info >= (3, 14):
        pytest.raises(ValueError, lambda: format(mx, ".10,_f"))
        pytest.raises(ValueError, lambda: format(mx, ".10_,f"))
        pytest.raises(ValueError, lambda: format(mx, ".10_n"))
        pytest.raises(ValueError, lambda: format(mx, ".10,n"))
    assert format(mx, ".2f") == "123.00"
    assert format(mx, "") == "123"
    assert format(mx, "c") == "{"
    assert format(mpz(0), "c") == "\x00"
    assert format(mx, "010c") == "000000000{"
    assert format(mx, "<10c") == "{         "

    if sys.version_info >= (3, 14):
        assert format(mx, ".,f") == "123.000,000"
        assert format(mx, "._f") == "123.000_000"

    if (platform.python_implementation() == "PyPy"
            and sys.pypy_version_info[:3] <= (7, 3, 20)):
        return  # issue pypy/pypy#5311

    try:
        locale.setlocale(locale.LC_ALL, "ru_RU.UTF-8")
        s = locale.localeconv()["thousands_sep"]
        assert format(mpz(123456789), "n") == f"123{s}456{s}789"
        assert format(mpz(123), "011n") == f"000{s}000{s}123"
        locale.setlocale(locale.LC_ALL, "C")
        if platform.python_implementation() == "GraalVM":
            return  # issue oracle/graalpython#521
        locale.setlocale(locale.LC_NUMERIC, "ps_AF.UTF-8")
        s = locale.localeconv()["thousands_sep"]
        assert format(mpz(123456789), "n") == f"123{s}456{s}789"
    except locale.Error:
        pass
    locale.setlocale(locale.LC_ALL, "C")


@given(bigints())
@example(0)
@example(123)
@example(75424656551107706)
@example(1284673497348563845623546741523784516734143215346712)
@example(65869376547959985897597359)
@example(-1329227995784915872903807060280344576)
@example(1<<63)
def test_from_to_int(x):
    sx = str(x)
    bx = bytes(sx, "ascii")
    bax = bytearray(sx, "ascii")
    mx = mpz(x)
    assert mpz(sx) == mpz(mx) == mx == x
    assert int(mx) == x
    assert mpz(bx) == mpz(bax) == x


@given(floats(allow_nan=False, allow_infinity=False))
def test_from_floats(x):
    assert mpz(x) == int(x)


def test_mpz_interface():
    with pytest.raises(ValueError):
        mpz(123).digits(-1)
    with pytest.raises(ValueError):
        mpz(123).digits(123)
    with pytest.raises(ValueError):
        mpz(-123).digits(123, prefix=True)
    with pytest.raises(ValueError):
        mpz("123", 1)
    with pytest.raises(ValueError):
        mpz("123", 123)
    with pytest.raises(ValueError):
        mpz("123", 129)
    with pytest.raises(ValueError):
        mpz("0123", 0)
    with pytest.raises(ValueError):
        mpz("0x", 0)
    with pytest.raises(TypeError):
        mpz(1j, 10)
    with pytest.raises(TypeError):
        mpz(123, spam=321)
    with pytest.raises(OverflowError):
        mpz("1", base=10**1000)
    with pytest.raises(ValueError):
        mpz(" ")
    with pytest.raises(ValueError):
        mpz("ыыы")
    assert mpz() == mpz(0) == 0
    assert mpz("  -123") == -123
    assert mpz("123  ") == 123
    assert mpz("    -123  ") == -123
    assert mpz("+123") == 123
    assert mpz("١٢٣٤") == 1234  # unicode decimal digits
    assert mpz("١23") == 123
    assert mpz("\t123") == 123
    assert mpz("\xa0123") == 123
    assert mpz("-010") == -10
    assert mpz("-10") == -10
    assert mpz("0b_10", 0) == 2

    assert mpz(with_int(123)) == 123
    with pytest.raises(RuntimeError):
        mpz(with_int(RuntimeError))
    with pytest.deprecated_call():
        assert mpz(with_int(int2(123))) == 123
    with warnings.catch_warnings():
        warnings.simplefilter("error", DeprecationWarning)
        with pytest.raises(DeprecationWarning):
            mpz(with_int(int2(123)))
    with pytest.raises(TypeError):
        mpz(with_int(1j))


def test_mpz_subclasses():
    assert issubclass(mpz2, mpz)
    x = mpz2(123)
    assert type(x) is mpz2
    assert type(x) is not mpz
    assert isinstance(x, mpz2)
    assert isinstance(x, mpz)
    assert x == mpz(123)
    assert mpz2() == 0
    assert mpz2("123", 16) == int("123", 16)

    with pytest.raises(TypeError):
        mpz2(1, spam=2)

    assert mpz2(with_int(123)) == 123
    with pytest.raises(RuntimeError):
        mpz2(with_int(RuntimeError))
    with pytest.deprecated_call():
        assert mpz2(with_int(int2(123))) == 123
    with warnings.catch_warnings():
        warnings.simplefilter("error", DeprecationWarning)
        with pytest.raises(DeprecationWarning):
            mpz2(with_int(int2(123)))
    with pytest.raises(TypeError):
        mpz2(with_int(1j))


@given(bigints())
def test_repr(x):
    mx = mpz(x)
    assert repr(mx) == f"mpz({x!s})"


@given(bigints(), bigints())
def test_richcompare_bulk(x, y):
    mx = mpz(x)
    my = mpz(y)
    for op in [operator.eq, operator.ne, operator.lt, operator.le,
               operator.gt, operator.ge]:
        assert op(mx, my) == op(x, y)
    assert bool(mx) == bool(x)


@given(bigints(), floats(allow_nan=False))
def test_richcompare_mixed(x, y):
    mx = mpz(x)
    for op in [operator.eq, operator.ne, operator.lt, operator.le,
               operator.gt, operator.ge]:
        assert op(mx, y) == op(x, y)
        assert op(y, mx) == op(y, x)


def test_richcompare_errors():
    mx = mpz(123)
    with pytest.raises(TypeError):
        mx > 1j
    with pytest.raises(TypeError):
        mx > object()
    with pytest.raises(OverflowError):
        1.1 > mpz(10**1000)
    with pytest.raises(OverflowError):
        mpz(10**1000) > 1.1


@given(bigints())
@example(0)
@example(-1)
@example(-2)
def test_hash_bulk(x):
    mx = mpz(x)
    assert hash(mx) == hash(x)


def test_hash_caching():
    mx = mpz(123)
    assert hash(mx) == 123
    assert hash(mx) == 123  # in cache


@given(bigints())
@example(0)
@example(123)
@example(75424656551107706)
@example(1284673497348563845623546741523784516734143215346712)
@example(65869376547959985897597359)
def test_plus_minus_abs(x):
    mx = mpz(x)
    assert +(+mx) == mx
    assert +mx == x
    assert -(-mx) == mx
    assert -mx == -x
    assert abs(mx) == abs(x)


@given(bigints(), bigints())
def test_add_sub_bulk(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = x + y
    assert mx + my == my + mx
    assert mx + my == r
    assert mx + y == r
    assert x + my == r
    r = x - y
    assert mx - my == r
    assert mx - y == r
    assert x - my == r


def test_add_int_subclasses():
    x = 123
    mx = mpz(x)
    class int2(int):
        pass
    y = int2(321)
    r = x + 321
    assert mx + y == r
    assert y + mx == r


@given(bigints(), floats(allow_nan=False), complex_numbers(allow_nan=False))
def test_add_sub_mixed(x, y, z):
    mx = mpz(x)
    r = x + y
    assert mx + y == y + mx
    assert mx + y == r
    r = x - y
    assert mx - y == r
    r = x + z
    assert mx + z == z + mx
    assert mx + z == r
    r = x - z
    assert mx - z == r


@given(bigints(), bigints())
@example(123 + (1<<64), 1<<200)
def test_mul_bulk(x, y):
    mx = mpz(x)
    my = mpz(y)
    assert mx * mx == x * x
    r = x * y
    assert mx * my == my * mx
    assert mx * my == r
    assert mx * y == r
    assert x * my == r


@given(bigints(), bigints(), bigints())
def test_addmul_associativity(x, y, z):
    mx = mpz(x)
    my = mpz(y)
    mz = mpz(z)
    assert (mx + my) + mz == mx + (my + mz)
    assert (mx * my) * mz == mx * (my * mz)


@given(bigints(), bigints(), bigints())
def test_mul_distributivity(x, y, z):
    mx = mpz(x)
    my = mpz(y)
    mz = mpz(z)
    assert (mx + my) * mz == mx*mz + my*mz
    assert (mx - my) * mz == mx*mz - my*mz


@given(bigints(), floats(allow_nan=False), complex_numbers(allow_nan=False))
def test_mul_mixed(x, y, z):
    mx = mpz(x)
    r = x * y
    if math.isnan(r):
        assert math.isnan(mx * y)
        assert math.isnan(y * mx)
    else:
        assert mx * y == y * mx
        assert mx * y == r
    r = x * z
    if cmath.isnan(r):
        assert cmath.isnan(mx * z)
        assert cmath.isnan(z * mx)
    else:
        assert mx * z == z * mx
        assert mx * z == r


@pytest.mark.skipif(platform.python_implementation() == "GraalVM",
                    reason="XXX: fails in CI for x,y=0,-1382074480823709287")
@given(bigints(), bigints())
@example(18446744073709551615, -1)
@example(-2, 1<<64)
@example(2, 1<<64)
@example(18446744073709551615<<64, -1<<64)
@example(int("0x"+"f"*32, 0), -1<<64)  # XXX: assuming BITS_PER_LIMB == 64
@example(-68501870735943706700000000000000000001, 10**20)  # issue 117
@example(0, 123)
def test_divmod_bulk(x, y):
    mx = mpz(x)
    my = mpz(y)
    if not y:
        with pytest.raises(ZeroDivisionError):
            mx // my
        return
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


@given(bigints(), floats(allow_nan=False))
def test_divmod_mixed(x, y):
    mx = mpz(x)
    if not y:
        with pytest.raises(ZeroDivisionError):
            mx // y
    else:
        assert mx // y == x // y
        assert mx % y == x % y


def test_divmod_errors():
    mx = mpz(123)
    with pytest.raises(TypeError):
        divmod(mx, 1j)
    with pytest.raises(TypeError):
        divmod(mx, object())
    with pytest.raises(ZeroDivisionError):
        divmod(mx, 0)


@pytest.mark.skipif(platform.python_implementation() == "GraalVM",
                    reason="XXX: oracle/graalpython#474")
@given(bigints(), bigints())
@example(0, -1)
@example(0, 123)
@example(10**1000, 2)
@example(2, 18446744073709551616)
@example(11811160064<<606, 11<<11)
def test_truediv_bulk(x, y):
    mx = mpz(x)
    my = mpz(y)
    if not y:
        with pytest.raises(ZeroDivisionError):
            mx / my
        return
    try:
        r = x / y
    except OverflowError:
        with pytest.raises(OverflowError):
            mx / my
    else:
        assert mx / my == r
        assert mx / y == r
        assert x / my == r


@given(bigints(), floats(allow_nan=False), complex_numbers(allow_nan=False))
def test_truediv_mixed(x, y, z):
    mx = mpz(x)
    if not x:
        with pytest.raises(ZeroDivisionError):
            y / mx
    else:
        try:
            r = y / mx
        except OverflowError:
            with pytest.raises(OverflowError):
                y / mx
        else:
            assert y / mx == r
    if not y:
        with pytest.raises(ZeroDivisionError):
            mx / y
    else:
        try:
            r = x / y
        except OverflowError:
            with pytest.raises(OverflowError):
                mx / y
        else:
            assert mx / y == r
    if not z:
        with pytest.raises(ZeroDivisionError):
            mx / z
    else:
        try:
            r = x / z
        except OverflowError:
            with pytest.raises(OverflowError):
                mx / z
        else:
            if cmath.isnan(r):
                assert cmath.isnan(mx / z)
            else:
                assert mx / z == r


def test_truediv_errors():
    mx = mpz(123)
    pytest.raises(TypeError, lambda: mx / object())
    pytest.raises(TypeError, lambda: object() / mx)


@pytest.mark.skipif(platform.python_implementation() == "GraalVM",
                    reason="XXX: oracle/graalpython#473")
@given(bigints(), integers(max_value=100000))
@example(0, 123)
@example(123, 0)
@example(-321, 0)
@example(1, 123)
@example(-1, 123)
@example(123, 321)
@example(-56, 321)
@example(10**1000, -1)
@example(10, -10**1000)
def test_power_bulk(x, y):
    if y > 0 and abs(x) > 1000000:
        return
    mx = mpz(x)
    my = mpz(y)
    try:
        r = x**y
    except OverflowError:
        with pytest.raises(OverflowError):
            mx**my
    except ZeroDivisionError:
        with pytest.raises(ZeroDivisionError):
            mx**my
    else:
        assert mx**my == r
        assert mx**y == r
        assert x**my == r


@given(bigints(), floats(allow_nan=False))
def test_power_mixed(x, y):
    mx = mpz(x)
    try:
        r = x**y
    except OverflowError:
        with pytest.raises(OverflowError):
            mx**y
    except ZeroDivisionError:
        with pytest.raises(ZeroDivisionError):
            mx**y
    else:
        assert mx**y == r
    try:
        r = y**x
    except OverflowError:
        with pytest.raises(OverflowError):
            y**mx
    except ZeroDivisionError:
        with pytest.raises(ZeroDivisionError):
            y**mx
    else:
        assert y**mx == r


@given(bigints(), integers(max_value=1000000), bigints())
@example(123, 111, 1)
@example(123, 1, 12)
@example(1, 123, 12)
@example(0, 123, 7)
@example(123, 0, 7)
@example(123, 1, 7)
@example(123, 11, 4)
@example(122, 11, 4)
@example(111, 11, 10001)
@example(111, 11, 10001*2**67)
@example(110, 11*2**80, 10001*2**67)
def test_power_mod(x, y, z):
    mx = mpz(x)
    my = mpz(y)
    mz = mpz(z)
    try:
        r = pow(x, y, z)
    except ValueError:
        with pytest.raises(ValueError):
            pow(mx, my, mz)
    except ZeroDivisionError:
        with pytest.raises(ZeroDivisionError):
            pow(mx, my, mz)
    else:
        assert pow(mx, my, mz) == r
        assert pow(mx, my, z) == r
        assert pow(mx, y, mz) == r
        if platform.python_implementation() == "PyPy":
            return  # XXX: pypy/pypy#5207
        assert pow(x, my, mz) == r
        assert pow(mx, y, z) == r
        assert pow(x, my, z) == r
        assert pow(x, y, mz) == r


def test_power_errors():
    with pytest.raises(TypeError):
        pow(123, mpz(321), 1.23)
    assert pow(1j, mpz(2)) == -1
    with pytest.raises(OverflowError):
        pow(1j, mpz(10**1000))
    with pytest.raises(OverflowError):
        pow(mpz(10**1000), 1j)
    with pytest.raises(TypeError):
        pow(object(), mpz(321))


@given(bigints())
def test_invert(x):
    mx = mpz(x)
    assert ~mx == ~x


@given(bigints(), bigints())
@example(1, 1<<67)
@example(1, -(1<<67))
@example(-2, -1)
@example(-1, -1)
def test_and(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = x & y
    assert mx & my == r
    assert mx & y == r
    assert x & my == r


@given(bigints(), bigints())
@example(1, 1<<67)
@example(1, -(1<<67))
@example(-2, -1)
@example(2, -1)
def test_or(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = x | y
    assert mx | my == r
    assert mx | y == r
    assert x | my == r


@given(bigints(), bigints())
@example(1, 1<<67)
@example(1, -(1<<67))
@example(-2, -1)
@example(-1, -1)
def test_xor(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = x ^ y
    assert mx ^ my == r
    assert mx ^ y == r
    assert x ^ my == r


@given(bigints(), integers(max_value=12345))
@example(18446744073709551618, 64)
@example(1, 1<<128)
@example(90605555449081991889354259339521952450308780844225461, 64)
def test_lshift(x, y):
    if platform.python_implementation() == "GraalVM" and y < 0:
        return  # XXX: oracle/graalpython#516
    mx = mpz(x)
    my = mpz(y)
    try:
        r = x << y
    except OverflowError:
        with pytest.raises(OverflowError):
            mx << my
        with pytest.raises(OverflowError):
            x << my
        with pytest.raises(OverflowError):
            mx << y
    except ValueError:
        with pytest.raises(ValueError):
            mx << my
        with pytest.raises(ValueError):
            x << my
        with pytest.raises(ValueError):
            mx << y
    else:
        assert mx << my == r
        assert mx << y == r
        assert x << my == r


@given(bigints(), bigints())
@example(1, 1<<78)
@example(-1, 1<<128)
@example(-340282366920938463444927863358058659840, 64)
@example(-514220174162876888173427869549172032807104958010493707296440352, 206)
@example(-6277101735386680763495507056286727952638980837032266301441, 128)
def test_rshift(x, y):
    # XXX: mp_size_t might be smaller than mp_limb_t
    if abs(y) >= 2**32 and platform.system() == "Windows":
        return
    mx = mpz(x)
    my = mpz(y)
    try:
        r = x >> y
    except OverflowError:
        with pytest.raises(OverflowError):
            mx >> my
        with pytest.raises(OverflowError):
            x >> my
        with pytest.raises(OverflowError):
            mx >> y
    except ValueError:
        with pytest.raises(ValueError):
            mx >> my
        with pytest.raises(ValueError):
            x >> my
        with pytest.raises(ValueError):
            mx >> y
    else:
        assert mx >> my == r
        assert mx >> y == r
        assert x >> my == r


@given(bigints())
def test_getseters(x):
    mx = mpz(x)
    assert mx.numerator == x.numerator
    assert mx.denominator == x.denominator
    assert mx.real == x.real
    assert mx.imag == x.imag


@given(bigints())
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


@given(bigints(), integers(min_value=0, max_value=10000),
       sampled_from(["big", "little"]), booleans())
@example(0, 0, "big", False)
@example(0, 0, "little", False)
@example(0, 1, "big", False)
@example(128, 1, "big", True)
@example(128, 1, "little", True)
@example(-129, 1, "big", True)
@example(-129, 1, "little", True)
@example(-1, 0, "big", True)
@example(-2, 0, "big", True)
@example(-2, 0, "little", True)
@example(42, 1, "big", False)
@example(42, 1, "little", False)
@example(42, 3, "big", False)
@example(42, 3, "little", False)
@example(1000, 2, "big", False)
@example(1000, 4, "big", False)
@example(-2049, 1, "big", True)
@example(-65281, 3, "big", True)
@example(-65281, 3, "little", True)
@example(128, 1, "big", False)
@example(-32383289396013590652, 0, "big", True)
def test_to_bytes_bulk(x, length, byteorder, signed):
    try:
        rx = x.to_bytes(length, byteorder, signed=signed)
    except OverflowError:
        if platform.python_implementation() == "GraalVM" and not length:
            return  # XXX: oracle/graalpython#475
        with pytest.raises(OverflowError):
            mpz(x).to_bytes(length, byteorder, signed=signed)
    else:
        assert rx == mpz(x).to_bytes(length, byteorder, signed=signed)


def test_to_bytes_interface():
    x = mpz(1)
    with pytest.raises(TypeError):
        x.to_bytes(1, 2, 3)
    with pytest.raises(TypeError):
        x.to_bytes(1, 2)
    with pytest.raises(TypeError):
        x.to_bytes("spam")
    with pytest.raises(TypeError):
        x.to_bytes(a=1, b=2, c=3, d=4)
    with pytest.raises(TypeError):
        x.to_bytes(2, length=2)
    with pytest.raises(TypeError):
        x.to_bytes(2, "big", byteorder="big")
    with pytest.raises(TypeError):
        x.to_bytes(spam=1)

    with pytest.raises(ValueError):
        x.to_bytes(2, "spam")
    with pytest.raises(ValueError):
        x.to_bytes(-1)

    assert x.to_bytes(2) == x.to_bytes(length=2)
    assert x.to_bytes(2, byteorder="little") == x.to_bytes(2, "little")

    assert x.to_bytes() == x.to_bytes(1)
    assert x.to_bytes() == x.to_bytes(1, "big")
    assert x.to_bytes() == x.to_bytes(signed=False)


@given(bigints(), integers(min_value=0, max_value=10000),
       sampled_from(["big", "little"]), booleans())
@example(0, 0, "big", False)
@example(0, 0, "little", False)
@example(0, 1, "big", False)
@example(128, 1, "big", True)
@example(128, 1, "little", True)
@example(-129, 1, "big", True)
@example(-129, 1, "little", True)
@example(-1, 0, "big", True)
@example(-1, 1, "big", True)
@example(-1, 1, "little", True)
@example(-2, 0, "big", True)
@example(-2, 0, "little", True)
@example(-1, 3, "big", True)
@example(-2, 3, "big", True)
@example(-2, 5, "little", True)
def test_from_bytes_bulk(x, length, byteorder, signed):
    try:
        bytes = x.to_bytes(length, byteorder, signed=signed)
    except OverflowError:
        assume(False)
    else:
        rx = int.from_bytes(bytes, byteorder, signed=signed)
        assert rx == mpz.from_bytes(bytes, byteorder, signed=signed)
        if platform.python_implementation() != "GraalVM":
            # XXX: oracle/graalpython#476
            assert rx == mpz.from_bytes(bytearray(bytes), byteorder,
                                        signed=signed)
        assert rx == mpz.from_bytes(list(bytes), byteorder, signed=signed)


def test_from_bytes_interface():
    with pytest.raises(TypeError):
        mpz.from_bytes()
    with pytest.raises(TypeError):
        mpz.from_bytes(1, 2, 3)
    with pytest.raises(TypeError):
        mpz.from_bytes(b"", 2)
    with pytest.raises(TypeError):
        mpz.from_bytes(1)
    with pytest.raises(TypeError):
        mpz.from_bytes(b"", bytes=b"")
    with pytest.raises(TypeError):
        mpz.from_bytes(b"", "big", byteorder="big")
    with pytest.raises(TypeError):
        mpz.from_bytes(b"", spam=1)
    with pytest.raises(TypeError):
        mpz.from_bytes(a=1, b=2, c=3, d=4)

    with pytest.raises(ValueError):
        mpz.from_bytes(b"", "spam")

    assert (mpz.from_bytes(b"\x01", byteorder="little")
            == mpz.from_bytes(b"\x01", "little"))

    assert mpz.from_bytes(b"\x01") == mpz.from_bytes(bytes=b"\x01")
    assert mpz.from_bytes(b"\x01") == mpz.from_bytes(b"\x01", "big")
    assert mpz.from_bytes(b"\x01") == mpz.from_bytes(b"\x01", signed=False)


@given(bigints())
@example(117529601297931785)
@example(1<<64)
@example(9007199254740993)
@example(10965857771245191)
@example(10<<10000)
@example((1<<53) + 1)
@example(1<<116)
@example(646541478744828163276576707651635923929979156076518566789121)
def test___float__(x):
    mx = mpz(x)
    try:
        fx = float(x)
    except OverflowError:
        pytest.raises(OverflowError, lambda: float(mx))
    else:
        assert float(mx) == fx


@given(bigints(), integers(min_value=-20, max_value=30))
@example(-75, -1)
@example(-68501870735943706700000000000000000001, -20)  # issue 117
@example(20775, -1)
def test___round__bulk(x, n):
    mx = mpz(x)
    mn = mpz(n)
    assert round(mx, n) == round(mx, mn) == round(x, n)
    if not n:
        assert round(mx) == round(x)


def test___round__interface():
    x = mpz(133)
    with pytest.raises(TypeError):
        x.__round__(1, 2)
    with pytest.raises(TypeError):
        x.__round__(1j)


@pytest.mark.skipif(platform.python_implementation() == "PyPy",
                    reason="sys.getsizeof raises TypeError")
def test___sizeof__():
    for i in [1, 20, 300]:
        ms = mpz(1 << i*BITS_PER_LIMB)
        assert sys.getsizeof(ms) >= i*SIZEOF_LIMB


@given(bigints(), integers(min_value=2, max_value=36))
def test_digits_bulk(x, base):
    mx = mpz(x)
    res = to_digits(x, base)
    assert mx.digits(base) == res


def test_digits_interface():
    x = mpz(123)
    with pytest.raises(TypeError):
        x.digits(1, 2, 3)
    with pytest.raises(TypeError):
        x.digits("", 2)
    with pytest.raises(TypeError):
        x.digits(10, base=16)
    with pytest.raises(TypeError):
        x.digits(1, True, prefix=True)
    with pytest.raises(TypeError):
        x.digits(spam=1)
    with pytest.raises(TypeError):
        x.digits(a=1, b=2, c=3)
    with pytest.raises(OverflowError):
        x.digits(base=10**100)
    assert x.digits(10) == x.digits(base=10) == x.digits()
    assert x.digits(16, prefix=True) == x.digits(16, True)
    assert x.digits(16, prefix=False) == x.digits(16, False) == x.digits(16)


@pytest.mark.skipif(platform.python_implementation() == "GraalVM",
                    reason="XXX: oracle/graalpython#479")
@given(bigints(), integers(min_value=2, max_value=36))
def test_digits_frombase(x, base):
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


@given(bigints())
def test_frombase_auto(x):
    mx = mpz(x)
    smx10 = mx.digits(10)
    smx2 = mx.digits(2, prefix=True)
    smx8 = mx.digits(8, prefix=True)
    smx16 = mx.digits(16, prefix=True)
    assert mpz(smx10, 0) == mx
    assert mpz(smx2, 0) == mx
    assert mpz(smx8, 0) == mx
    assert mpz(smx16, 0) == mx
    assert mpz(smx2.upper(), 0) == mx
    assert mpz(smx8.upper(), 0) == mx
    assert mpz(smx16.upper(), 0) == mx


@pytest.mark.parametrize("protocol",
                         range(pickle.HIGHEST_PROTOCOL + 1))
@given(bigints())
def test_pickle(protocol, x):
    mx = mpz(x)
    assert mx == pickle.loads(pickle.dumps(mx, protocol))


@pytest.mark.skipif(platform.system() == "Darwin", reason="XXX")
@settings(max_examples=100)
@given(lists(integers(min_value=2), min_size=3, max_size=20))
def test_mpz_collatz(xs):
    # https://en.wikipedia.org/wiki/Collatz_conjecture
    def f(n):
        while n > 1:
            n = 3*n + 1 if n & 1 else n//2
        return n
    with ThreadPoolExecutor(max_workers=7) as tpe:
        futures = [tpe.submit(f, mpz(x)) for x in xs]
        assert all(f.result() == 1 for f in futures)
