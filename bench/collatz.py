# collatz.py

import os

import pyperf

if os.getenv("T") == "gmpy2.mpz":
    from gmpy2 import mpz
elif os.getenv("T") == "flint.fmpz":
    from flint import fmpz as mpz
elif os.getenv("T") == "int":
    mpz = int
else:
    from gmp import mpz

zero = mpz(0)
one = mpz(1)
two = mpz(2)
three = mpz(3)

# https://en.wikipedia.org/wiki/Collatz_conjecture

def collatz0(n):
    total = 0
    n = mpz(n)
    while n > one:
        n = n*three + one if n & one else n//two
        total += 1
    return total

def collatz1(n):
    total = 0
    n = mpz(n)
    while n > 1:
        n = n*3 + 1 if n & 1 else n//2
        total += 1
    return total

runner = pyperf.Runner()
for f in [collatz0, collatz1]:
    for v in ["97", "871", "(1<<128)+31"]:
        h = f"{f.__name__}({v})"
        i = eval(v)
        runner.bench_func(h, f, i)
