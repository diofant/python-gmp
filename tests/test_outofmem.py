import platform

import pytest
from gmp import fac, mpz
from hypothesis import given
from hypothesis.strategies import integers

if platform.system() != "Linux":
    pytest.skip("FIXME: setrlimit fails with ValueError on MacOS",
                allow_module_level=True)
if platform.python_implementation() == "GraalVM":
    pytest.skip("XXX: module 'resource' has no attribute 'setrlimit'",
                allow_module_level=True)
if platform.python_implementation() == "PyPy":
    pytest.skip("XXX: diofant/python-gmp#73", allow_module_level=True)

resource = pytest.importorskip("resource")


@given(integers(min_value=12811, max_value=24984))
def test_fac_outofmem(x):
    soft, hard = resource.getrlimit(resource.RLIMIT_AS)
    resource.setrlimit(resource.RLIMIT_AS, (1024*32*1024, hard))
    a = mpz(x)
    while True:
        try:
            fac(a)
            a *= 2
        except MemoryError:
            break
    resource.setrlimit(resource.RLIMIT_AS, (soft, hard))


@given(integers(min_value=49846727467293, max_value=249846727467293))
def test_square_outofmem(x):
    soft, hard = resource.getrlimit(resource.RLIMIT_AS)
    resource.setrlimit(resource.RLIMIT_AS, (1024*32*1024, hard))
    mx = mpz(x)
    i = 1
    while True:
        try:
            mx = mx*mx
        except MemoryError:
            assert i > 5
            break
        i += 1
    resource.setrlimit(resource.RLIMIT_AS, (soft, hard))
