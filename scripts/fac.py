from gmp import factorial, mpz


def factorial_outofmemory():
    import random
    import resource

    soft, hard = resource.getrlimit(resource.RLIMIT_AS)
    resource.setrlimit(resource.RLIMIT_AS, (1024*32*1024, hard))

    for _ in range(100):
        a = random.randint(12811, 24984)
        a = mpz(a)
        while True:
            try:
                factorial(a)
                a *= 2
            except MemoryError:
                break

    resource.setrlimit(resource.RLIMIT_AS, (soft, hard))

factorial_outofmemory()
