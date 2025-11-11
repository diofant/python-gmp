import platform
from ctypes import (
    CDLL,
    POINTER,
    Structure,
    byref,
    c_bool,
    c_int8,
    c_int32,
    c_int64,
    c_size_t,
    c_uint8,
    c_uint64,
    c_ulong,
    cast,
)
from enum import IntEnum

import pytest

if platform.system() != "Linux":
    pytest.skip("FIXME: find library!", allow_module_level=True)

class zz_t_struct(Structure):
    _fields_ = [("negative", c_bool),
                ("alloc", c_int32),
                ("size", c_int32),
                ("digits", POINTER(c_ulong))]

class zz_layout(Structure):
    _fields_ = [("bits_per_limb", c_uint8),
                ("limb_size", c_uint8),
                ("limbs_order", c_int8),
                ("limb_endianness", c_int8)]

class zz_err(IntEnum):
    ZZ_OK = 0
    ZZ_MEM = -1
    ZZ_VAL = -2
    ZZ_BUF = -3

class zz_ord(IntEnum):
    ZZ_GT = +1
    ZZ_EQ = 0
    ZZ_LT = -1

class zz_rnd(IntEnum):
    ZZ_RNDD = 0
    ZZ_RNDN = 1

libzz = CDLL("libzz.so")
zz_setup = libzz.zz_setup
zz_finish = libzz.zz_finish
zz_from_sl = libzz.zz_from_sl
zz_cmp_sl = libzz.zz_cmp_sl
zz_cmp = libzz.zz_cmp
zz_add_sl = libzz.zz_add_sl
zz_lsbpos = libzz.zz_lsbpos
zz_export = libzz.zz_export
zz_mul = libzz.zz_mul
zz_div = libzz.zz_div
zz_rem_ul = libzz.zz_rem_ul
zz_mul_2exp = libzz.zz_mul_2exp
zz_quo_2exp = libzz.zz_quo_2exp
zz_pow = libzz.zz_pow
zz_powm = libzz.zz_powm
zz_sqrtrem = libzz.zz_sqrtrem
zz_to_str = libzz.zz_to_str

u, v, w = map(byref, map(zz_t_struct, [[]]*3))
int_layout = zz_layout(30, 4, -1, -1)


@pytest.fixture(autouse=True, scope="module")
def libzz_setup_teardown():
    assert zz_setup(None) == zz_err.ZZ_OK
    yield
    zz_finish()


def test_zz_cmp_sl():
    assert zz_from_sl(13, u) == zz_err.ZZ_OK
    assert zz_cmp_sl(u, 1) == zz_ord.ZZ_GT
    assert zz_cmp_sl(u, 100) == zz_ord.ZZ_LT


def test_zz_cmp():
    assert zz_from_sl(13, u) == zz_err.ZZ_OK
    assert zz_cmp(u, u) == zz_ord.ZZ_EQ


def test_zz_add_sl():
    assert zz_from_sl(0, u) == zz_err.ZZ_OK
    assert zz_add_sl(u, 2, u) == zz_err.ZZ_OK
    assert zz_cmp_sl(u, 2) == zz_ord.ZZ_EQ
    assert zz_from_sl(0, u) == zz_err.ZZ_OK
    assert zz_add_sl(u, 0, u) == zz_err.ZZ_OK
    assert zz_cmp_sl(u, 0) == zz_ord.ZZ_EQ


def test_zz_lsbpos():
    assert zz_from_sl(0, u) == zz_err.ZZ_OK
    assert zz_lsbpos(u, 0) == 0


@pytest.mark.skipif(platform.python_implementation() == "GraalVM",
                    reason=("NotImplementedError: Passing structs "
                            "by value is not supported on NFI backend"))
def test_zz_export():
    assert zz_from_sl(123, u) == zz_err.ZZ_OK
    assert zz_export(u, int_layout, 0, 0) == zz_err.ZZ_VAL


def test_zz_to_str():
    assert zz_from_sl(123, u) == zz_err.ZZ_OK
    p = c_int8()
    s = c_size_t()
    assert zz_to_str(u, 38, byref(p), byref(s)) == zz_err.ZZ_VAL


def test_zz_mul():
    assert zz_from_sl(2, u) == zz_err.ZZ_OK
    assert zz_from_sl(3, v) == zz_err.ZZ_OK
    assert zz_mul(u, v, u) == zz_err.ZZ_OK
    assert zz_cmp_sl(u, 6) == zz_ord.ZZ_EQ
    assert zz_mul(u, v, v) == zz_err.ZZ_OK
    assert zz_cmp_sl(v, 18) == zz_ord.ZZ_EQ


def test_zz_div():
    assert zz_from_sl(4, u) == zz_err.ZZ_OK
    assert zz_from_sl(2, v) == zz_err.ZZ_OK
    assert zz_div(u, v, zz_rnd.ZZ_RNDD, v, None) == zz_err.ZZ_OK
    assert zz_cmp_sl(v, 2) == zz_ord.ZZ_EQ
    assert zz_div(u, v, 123, u, None) == zz_err.ZZ_VAL
    assert zz_div(u, v, zz_rnd.ZZ_RNDD, None, None) == zz_err.ZZ_VAL


def test_zz_rem_ul():
    assert zz_from_sl(123, u) == zz_err.ZZ_OK
    p = c_uint64(0)
    assert zz_rem_ul(u, 0, zz_rnd.ZZ_RNDD, byref(p)) == zz_err.ZZ_VAL
    assert zz_from_sl(111, u) == zz_err.ZZ_OK
    assert zz_rem_ul(u, 12, zz_rnd.ZZ_RNDD, byref(p)) == zz_err.ZZ_OK
    assert p.value == 3
    assert zz_from_sl(-111, u) == zz_err.ZZ_OK
    assert zz_rem_ul(u, 12, zz_rnd.ZZ_RNDD, byref(p)) == zz_err.ZZ_OK
    assert p.value == 9


def test_zz_quo_2exp():
    assert zz_from_sl(c_int64(0x7fffffffffffffff), u) == zz_err.ZZ_OK
    assert zz_mul_2exp(u, 1, u) == zz_err.ZZ_OK
    assert zz_add_sl(u, 1, u) == zz_err.ZZ_OK
    assert zz_mul_2exp(u, 64, u) == zz_err.ZZ_OK
    assert zz_quo_2exp(u, 64, u) == zz_err.ZZ_OK
    a = cast(u, POINTER(zz_t_struct)).contents
    assert a.negative is False
    assert a.alloc >= 1
    assert a.size == 1
    assert a.digits[0] == 0xffffffffffffffff
    assert zz_from_sl(c_int64(0x7fffffffffffffff), v) == zz_err.ZZ_OK
    assert zz_mul_2exp(v, 1, v) == zz_err.ZZ_OK
    assert zz_add_sl(v, 1, v) == zz_err.ZZ_OK
    assert zz_cmp(u, v) == zz_ord.ZZ_EQ


def test_zz_pow():
    assert zz_from_sl(2, u) == zz_err.ZZ_OK
    assert zz_pow(u, 2, u) == zz_err.ZZ_OK
    assert zz_cmp_sl(u, 4) == zz_ord.ZZ_EQ


def test_zz_sqrtrem():
    assert zz_from_sl(4, u) == zz_err.ZZ_OK
    assert zz_from_sl(0, v) == zz_err.ZZ_OK
    assert zz_sqrtrem(u, u, v) == zz_err.ZZ_OK
    assert zz_cmp_sl(u, 2) == zz_ord.ZZ_EQ
    assert zz_cmp_sl(v, 0) == zz_ord.ZZ_EQ

def test_zz_powm():
    assert zz_from_sl(12, u) == zz_err.ZZ_OK
    assert zz_from_sl(4, v) == zz_err.ZZ_OK
    assert zz_from_sl(7, w) == zz_err.ZZ_OK
    assert zz_powm(u, v, w, u) == zz_err.ZZ_OK
    assert zz_cmp_sl(u, 2) == zz_ord.ZZ_EQ
    assert zz_from_sl(12, u) == zz_err.ZZ_OK
    assert zz_powm(u, v, w, v) == zz_err.ZZ_OK
    assert zz_cmp_sl(v, 2) == zz_ord.ZZ_EQ
    assert zz_from_sl(4, v) == zz_err.ZZ_OK
    assert zz_powm(u, v, w, w) == zz_err.ZZ_OK
    assert zz_cmp_sl(w, 2) == zz_ord.ZZ_EQ
