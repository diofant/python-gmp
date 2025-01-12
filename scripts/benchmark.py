#!/usr/bin/env python3

import gmp
import gmpy2
import pyperf


# Benchmark context
x = [
    [int(1 << 1), int(1 << 30), int(1 << 300)],
    [gmp.mpz(1 << 1), gmp.mpz(1 << 30), gmp.mpz(1 << 300)],
    [gmpy2.mpz(1 << 1), gmpy2.mpz(1 << 30), gmpy2.mpz(1 << 300)],
]
shifts = [1, 30, 300]
operations = [("add", "+"), ("sub", "-"), ("mul", "*"), ("div", "/")]
runner = pyperf.Runner()


# ================================================ #
#       Basic numerical  operations benchmark      #
# ================================================ #
def benchmark_basic_numerical_operations():
    for op_name, op in operations:
        for i, shift in enumerate(shifts):
            intval, gmpval, gmp2val = x[0][i], x[1][i], x[2][i]
            runner.timeit(f"int {op_name} (1 << {shift})",
                          f"{intval} {op} {intval}")
            runner.timeit(f"gmp {op_name} (1 << {shift})",
                          f"{gmpval} {op} {gmpval}")
            runner.timeit(f"gmpy2 {op_name} (1 << {shift})",
                          f"{gmp2val} {op} {gmp2val}")


# How to run the benchmark:
# 1. Run benchmark
#  >> python3 scripts/benchmark.py -o <benchmark_result>
# 2. Show benchmark table
#  >> python3 -m pyperf compare_to <benchmark_file> <benchmark_result> --table
# or Show benchmark hist graph
#  >> python3 -m pyperf hist <benchmark_result>
if __name__ == "__main__":
    benchmark_basic_numerical_operations()
