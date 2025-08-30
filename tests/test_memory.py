import gc
import platform
import random
import threading
from concurrent.futures import ThreadPoolExecutor

import pytest
from gmp import fac, mpz

if platform.system() != "Linux":
    pytest.skip("FIXME: setrlimit fails with ValueError on MacOS",
                allow_module_level=True)
if platform.python_implementation() == "GraalVM":
    pytest.skip("XXX: module 'resource' has no attribute 'setrlimit'",
                allow_module_level=True)
if platform.python_implementation() != "PyPy":
    VMEM_LIMIT = 64*1000**2
else:
    VMEM_LIMIT = 128*1000**2

resource = pytest.importorskip("resource")


def test_fac_outofmem():
    for _ in range(100):
        x = random.randint(12811, 24984)
        soft, hard = resource.getrlimit(resource.RLIMIT_AS)
        resource.setrlimit(resource.RLIMIT_AS, (VMEM_LIMIT, hard))
        a = mpz(x)
        while True:
            try:
                fac(a)
                a *= 2
            except MemoryError:
                del a
                gc.collect()
                break
        resource.setrlimit(resource.RLIMIT_AS, (soft, hard))


def test_square_outofmem():
    for _ in range(100):
        x = random.randint(49846727467293, 249846727467293)
        soft, hard = resource.getrlimit(resource.RLIMIT_AS)
        resource.setrlimit(resource.RLIMIT_AS, (VMEM_LIMIT, hard))
        mx = mpz(x)
        i = 1
        while True:
            try:
                mx = mx*mx
            except MemoryError:
                del mx
                gc.collect()
                assert i > 5
                break
            i += 1
        resource.setrlimit(resource.RLIMIT_AS, (soft, hard))


@pytest.mark.skipif(platform.python_implementation() == "PyPy",
                    reason="XXX: pypy/pypy#5325")
def test_square_with_threads():
    soft, hard = resource.getrlimit(resource.RLIMIT_AS)
    def f(n, barrier):
        barrier.wait()
        resource.setrlimit(resource.RLIMIT_AS, (VMEM_LIMIT, hard))
        for i in range(100):
            n = n*n
        return n
    num_threads = 7
    with ThreadPoolExecutor(max_workers=num_threads) as tpe:
        futures = []
        barrier = threading.Barrier(num_threads)
        for i in range(num_threads):
            futures.append(tpe.submit(f, mpz(10 + 201*i), barrier))
    resource.setrlimit(resource.RLIMIT_AS, (soft, hard))
