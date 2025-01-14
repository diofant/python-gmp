import ctypes


def multiplicity(n, p):
    """Return the power of the prime number p in the factorization of n!"""
    import gmp

    mpz = gmp.mpz
    if p > n:
        return mpz(0)
    if p > n//2:
        return mpz(1)
    q, m = n, mpz(0)
    while q >= p:
        q //= p
        m += q
    return m


def primes(n):
    """Generate a list of the prime numbers [2, 3, ... m], m <= n."""
    import gmp

    mpz = gmp.mpz
    isqrt = gmp.isqrt
    n = n + mpz(1)
    sieve = [mpz(_) for _ in range(n)]
    sieve[:2] = [mpz(0), mpz(0)]
    for i in range(2, isqrt(n) + 1):
        if sieve[i]:
            for j in range(i**2, n, i):
                sieve[j] = mpz(0)
    # Filter out the composites, which have been replaced by 0's
    return [p for p in sieve if p]


def powproduct(ns):
    import gmp

    mpz = gmp.mpz
    if not ns:
        return mpz(1)
    units = mpz(1)
    multi = []
    for base, exp in ns:
        if exp == 0:
            continue
        elif exp == 1:
            units *= base
        else:
            if exp % 2:
                units *= base
            multi.append((base, exp//2))
    p = powproduct(multi)
    p2 = p**2
    del p
    #pp = int(units) * int(p2)
    print(units.bit_length(), p2.bit_length(), flush=True)
    pp2 = units * p2
    del units
    del p2
    return pp2


def factorial(n, /):
    """
    Find n!.

    Raise a ValueError if n is negative or non-integral.
    """
    import gmp

    mpz = gmp.mpz
    n = mpz(n)
    if n < 0:
        raise ValueError("factorial() not defined for negative values")
    if ctypes.c_long(n).value != n:
        raise OverflowError("factorial() argument should not exceed LONG_MAX")
    return powproduct((p, multiplicity(n, p)) for p in primes(n))
