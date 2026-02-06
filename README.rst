Python-GMP
==========

Python extension module, providing bindings to the GNU GMP via the `ZZ library
<https://github.com/diofant/zz>`_ (version 0.8.0 or later required).  This
module shouldn't crash the interpreter.

The gmp can be used as a `gmpy2`_/`python-flint`_ replacement to provide
integer type (`mpz`_), compatible with Python's `int`_.  It also includes
functions, compatible with the Python stdlib's submodule `math.integer
<https://docs.python.org/3.15/library/math.integer.html>`_.

This module requires Python 3.11 or later versions and has been tested with
CPython 3.11 through 3.14, with PyPy3.11 7.3.20 and with GraalPy 25.0.
Free-threading builds of the CPython are supported.

Releases are available in the Python Package Index (PyPI) at
https://pypi.org/project/python-gmp/.


Motivation
----------

The CPython (and most other Python implementations, like PyPy) is optimized to
work with small (machine-sized) integers.  Algorithms used here for big
integers usually aren't best known in the field.  Fortunately, it's possible to
use bindings (for example, the `gmpy2`_ package) to the GNU GMP, which aims to
be faster than any other bignum library for all operand sizes.

But such extension modules usually rely on default GMP's memory management and
can't recover from allocation failure.  So, it's easy to crash the Python
interpreter during the interactive session.  Following example with the gmpy2
will work if you set address space limit for the Python interpreter (e.g. by
``prlimit`` command on Linux):

.. code:: pycon

   >>> import gmpy2
   >>> gmpy2.__version__
   '2.2.1'
   >>> z = gmpy2.mpz(29925959575501)
   >>> while True:  # this loop will crash interpter
   ...     z = z*z
   ...
   GNU MP: Cannot allocate memory (size=46956584)
   Aborted

The gmp module handles such errors correctly:

.. code:: pycon

   >>> import gmp
   >>> z = gmp.mpz(29925959575501)
   >>> while True:
   ...     z = z*z
   ...
   Traceback (most recent call last):
     File "<python-input-3>", line 2, in <module>
       z = z*z
           ~^~
   MemoryError
   >>> # interpreter still works, all variables in
   >>> # the current scope are available,
   >>> z.bit_length()  # including pre-failure value of z
   93882077


Warning on --disable-alloca configure option
--------------------------------------------

You should use the GNU GMP library, compiled with the '--disable-alloca'
configure option to prevent using alloca() for temporary workspace allocation,
or this module may crash the interpreter in case of a stack overflow.


.. _gmpy2: https://pypi.org/project/gmpy2/
.. _python-flint: https://pypi.org/project/python-flint/
.. _mpz: https://python-gmp.readthedocs.io/en/latest/#gmp.mpz
.. _int: https://docs.python.org/3/library/functions.html#int
