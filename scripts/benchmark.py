#!/usr/bin/env python3


import gmp
import gmpy2
import pyperf


def run_benchmark(func, args_list):
    for args in args_list:
        func(*args)


# benchmarking int base operations
def benchmark_int():
    benchmark_args = [
        [int(1), int(1)],
    ]
    run_benchmark(int.__add__, benchmark_args)
    run_benchmark(int.__sub__, benchmark_args)
    run_benchmark(int.__mul__, benchmark_args)
    run_benchmark(int.__floordiv__, benchmark_args)
    run_benchmark(int.__truediv__, benchmark_args)
    run_benchmark(int.__eq__, benchmark_args)
    run_benchmark(int.__ne__, benchmark_args)
    run_benchmark(int.__lt__, benchmark_args)
    run_benchmark(int.__le__, benchmark_args)
    run_benchmark(int.__gt__, benchmark_args)
    run_benchmark(int.__ge__, benchmark_args)


# benchmarking gmp.mpz base operations
def benchmark_gmp_mpz():
    benchmark_args = [
        [gmp.mpz(1), gmp.mpz(1)],
    ]
    run_benchmark(gmp.mpz.__add__, benchmark_args)
    run_benchmark(gmp.mpz.__sub__, benchmark_args)
    run_benchmark(gmp.mpz.__mul__, benchmark_args)
    run_benchmark(gmp.mpz.__floordiv__, benchmark_args)
    run_benchmark(gmp.mpz.__truediv__, benchmark_args)
    run_benchmark(gmp.mpz.__eq__, benchmark_args)
    run_benchmark(gmp.mpz.__ne__, benchmark_args)
    run_benchmark(gmp.mpz.__lt__, benchmark_args)
    run_benchmark(gmp.mpz.__le__, benchmark_args)
    run_benchmark(gmp.mpz.__gt__, benchmark_args)
    run_benchmark(gmp.mpz.__ge__, benchmark_args)


# benchmarking gmpy2.mpz base operations
def benchmark_gmpy2_mpz():
    benchmark_args = [
        [gmpy2.mpz(1), gmpy2.mpz(1)],
    ]
    run_benchmark(gmpy2.mpz.__add__, benchmark_args)
    run_benchmark(gmpy2.mpz.__sub__, benchmark_args)
    run_benchmark(gmpy2.mpz.__mul__, benchmark_args)
    run_benchmark(gmpy2.mpz.__floordiv__, benchmark_args)
    run_benchmark(gmpy2.mpz.__truediv__, benchmark_args)
    run_benchmark(gmpy2.mpz.__eq__, benchmark_args)
    run_benchmark(gmpy2.mpz.__ne__, benchmark_args)
    run_benchmark(gmpy2.mpz.__lt__, benchmark_args)
    run_benchmark(gmpy2.mpz.__le__, benchmark_args)
    run_benchmark(gmpy2.mpz.__gt__, benchmark_args)
    run_benchmark(gmpy2.mpz.__ge__, benchmark_args)


# 1. Run benchmark
#  >> python3 scripts/benchmark.py -o <benchmark_result>
# 2. Display benchmark Table
#  >> python3 -m pyperf compare_to <benchmark_file> <benchmark_result> --table
# or Display benchmark hist graph
#  >> python3 -m pyperf hist <benchmark_result>
if __name__ == "__main__":
    runner = pyperf.Runner(loops=1)
    runner.bench_func("int", benchmark_int)
    runner.bench_func("gmp.mpz", benchmark_gmp_mpz)
    runner.bench_func("gmpy2.mpz", benchmark_gmpy2_mpz)
