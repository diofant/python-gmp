# bench/harmonic.py

from fractions import Fraction

import pyperf

from bench.utils import mpz, mysum

runner = pyperf.Runner()
for n in [100, 1000]:
    xs = [Fraction(mpz(1), mpz(i)) for i in range(1, n + 1)]
    runner.bench_func(f"H({n})", mysum, xs)
