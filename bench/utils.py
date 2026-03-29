import os

if os.getenv("T") == "gmpy2.mpz":
    from gmpy2 import mpz
elif os.getenv("T") == "flint.fmpz":
    from flint import fmpz as mpz
elif os.getenv("T") == "int":
    mpz = int
else:
    from gmp import mpz  # noqa: F401


def mysum(xs):
    total = xs[0]
    for t in xs[1:]:
        total += t
    return t
