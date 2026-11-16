# bench/import.py

import pyperf

from bench.utils import mpz

cases = ["1<<7", "1<<38", "1<<300", "1<<3000", "1<<10000"]
runner = pyperf.Runner()
for c in cases:
    i = eval(c)
    runner.bench_func(c, mpz, i)
