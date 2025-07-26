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
    sizeof,
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
    _fields_ = [("bits_per_digit", c_uint8),
                ("digit_size", c_uint8),
                ("digits_order", c_int8),
                ("digit_endianness", c_int8)]

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
zz_resize = libzz.zz_resize
zz_from_i64 = libzz.zz_from_i64
zz_cmp_i32 = libzz.zz_cmp_i32
zz_cmp = libzz.zz_cmp
zz_add_i32 = libzz.zz_add_i32
zz_lsbpos = libzz.zz_lsbpos
zz_export = libzz.zz_export
zz_mul = libzz.zz_mul
zz_div = libzz.zz_div
zz_rem_u64 = libzz.zz_rem_u64
zz_mul_2exp = libzz.zz_mul_2exp
zz_quo_2exp = libzz.zz_quo_2exp
zz_pow = libzz.zz_pow
zz_powm = libzz.zz_powm
zz_sqrtrem = libzz.zz_sqrtrem

u, v, w = map(byref, map(zz_t_struct, [[]]*3))
int_layout = zz_layout(30, 4, -1, -1)


@pytest.mark.skipif(sizeof(c_size_t) < 8, reason="Can't overflow zz_size_t")
def test_zz_resize():
    assert zz_resize(c_size_t(1<<34), u) == zz_err.ZZ_MEM


def test_zz_cmp_i32():
    assert zz_from_i64(13, u) == zz_err.ZZ_OK
    assert zz_cmp_i32(u, 1) == zz_ord.ZZ_GT
    assert zz_cmp_i32(u, 100) == zz_ord.ZZ_LT


def test_zz_add_i32():
    assert zz_from_i64(0, u) == zz_err.ZZ_OK
    assert zz_add_i32(u, 2, u) == zz_err.ZZ_OK
    assert zz_cmp_i32(u, 2) == zz_ord.ZZ_EQ


def test_zz_lsbpos():
    assert zz_from_i64(0, u) == zz_err.ZZ_OK
    assert zz_lsbpos(u, 0) == 0


@pytest.mark.skipif(platform.python_implementation() == "GraalVM",
                    reason=("NotImplementedError: Passing structs "
                            "by value is not supported on NFI backend"))
def test_zz_export():
    assert zz_from_i64(123, u) == zz_err.ZZ_OK
    assert zz_export(u, int_layout, 0, 0) == zz_err.ZZ_VAL


def test_zz_mul():
    assert zz_from_i64(2, u) == zz_err.ZZ_OK
    assert zz_from_i64(3, v) == zz_err.ZZ_OK
    assert zz_mul(u, v, u) == zz_err.ZZ_OK
    assert zz_cmp_i32(u, 6) == zz_ord.ZZ_EQ
    assert zz_mul(u, v, v) == zz_err.ZZ_OK
    assert zz_cmp_i32(v, 18) == zz_ord.ZZ_EQ


def test_zz_div():
    assert zz_from_i64(4, u) == zz_err.ZZ_OK
    assert zz_from_i64(2, v) == zz_err.ZZ_OK
    assert zz_div(u, v, zz_rnd.ZZ_RNDD, v, 0) == zz_err.ZZ_OK
    assert zz_cmp_i32(v, 2) == zz_ord.ZZ_EQ
    assert zz_div(u, v, 123, u, 0) == zz_err.ZZ_VAL


def test_zz_rem_u64():
    assert zz_from_i64(123, u) == zz_err.ZZ_OK
    p = c_uint64(0)
    assert zz_rem_u64(u, 0, byref(p)) == zz_err.ZZ_VAL
    assert zz_from_i64(111, u) == zz_err.ZZ_OK
    assert zz_rem_u64(u, 12, byref(p)) == zz_err.ZZ_OK
    assert p.value == 3
    assert zz_from_i64(-111, u) == zz_err.ZZ_OK
    assert zz_rem_u64(u, 12, byref(p)) == zz_err.ZZ_OK
    assert p.value == 9


def test_zz_quo_2exp():
    assert zz_from_i64(c_int64(0x7fffffffffffffff), u) == zz_err.ZZ_OK
    assert zz_mul_2exp(u, 1, u) == zz_err.ZZ_OK
    assert zz_add_i32(u, 1, u) == zz_err.ZZ_OK
    assert zz_mul_2exp(u, 64, u) == zz_err.ZZ_OK
    assert zz_quo_2exp(u, 64, u) == zz_err.ZZ_OK
    a = cast(u, POINTER(zz_t_struct)).contents
    assert a.negative is False
    assert a.alloc >= 1
    assert a.size == 1
    assert a.digits[0] == 0xffffffffffffffff
    assert zz_from_i64(c_int64(0x7fffffffffffffff), v) == zz_err.ZZ_OK
    assert zz_mul_2exp(v, 1, v) == zz_err.ZZ_OK
    assert zz_add_i32(v, 1, v) == zz_err.ZZ_OK
    assert zz_cmp(u, v) == zz_ord.ZZ_EQ


def test_zz_pow():
    assert zz_from_i64(2, u) == zz_err.ZZ_OK
    assert zz_pow(u, 2, u) == zz_err.ZZ_OK
    assert zz_cmp_i32(u, 4) == zz_ord.ZZ_EQ


def test_zz_sqrtrem():
    assert zz_from_i64(4, u) == zz_err.ZZ_OK
    assert zz_from_i64(0, v) == zz_err.ZZ_OK
    assert zz_sqrtrem(u, u, v) == zz_err.ZZ_OK
    assert zz_cmp_i32(u, 2) == zz_ord.ZZ_EQ
    assert zz_cmp_i32(v, 0) == zz_ord.ZZ_EQ

def test_zz_powm():
    assert zz_from_i64(12, u) == zz_err.ZZ_OK
    assert zz_from_i64(4, v) == zz_err.ZZ_OK
    assert zz_from_i64(7, w) == zz_err.ZZ_OK
    assert zz_powm(u, v, w, u) == zz_err.ZZ_OK
    assert zz_cmp_i32(u, 2) == zz_ord.ZZ_EQ
    assert zz_from_i64(12, u) == zz_err.ZZ_OK
    assert zz_powm(u, v, w, v) == zz_err.ZZ_OK
    assert zz_cmp_i32(v, 2) == zz_ord.ZZ_EQ
    assert zz_from_i64(4, v) == zz_err.ZZ_OK
    assert zz_powm(u, v, w, w) == zz_err.ZZ_OK
    assert zz_cmp_i32(w, 2) == zz_ord.ZZ_EQ
