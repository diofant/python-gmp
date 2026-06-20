# bench/sum.py

import pyperf

from bench.utils import mpz, mysum

runner = pyperf.Runner()
for n in [100, 1000]:
    xs = [mpz(i) for i in range(1, n + 1)]
    runner.bench_func(f"sum({n})", mysum, xs)
