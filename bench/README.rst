This directory holds some basic benchmarks for the gmp extension.

It's possible to run them also with gmpy2's and flint's integer types:

.. code:: sh

    ( export T="gmpy2.mpz"; \
      python bench/mul.py -q --copy-env --rigorous -o $T.json )

Beware, that the gmp prefers clang over gcc and extensions might
use different compiler options per default.
