# bench/mul.py

from operator import mul

import pyperf

from bench.utils import mpz

values = ["1<<7", "1<<38", "1<<300", "1<<3000"]

runner = pyperf.Runner()
for v in values:
    i = eval(v)
    bn = '("'+v+'")**2'
    x = mpz(i)
    # make y != x to avoid a quick mpn_sqr() path on gmp/flint
    y = mpz(i + 1)
    runner.bench_func(bn, mul, x, y)
