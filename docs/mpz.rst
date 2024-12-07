mpz objects - :class:`mpz`
==========================

.. currentmodule:: gmp

.. autoclass:: mpz
    :special-members: __format__

mpz object supports the following basic calculations.

+----------------+------------------------------------+---------+
| Operation      | Result                             | Notes   |
+================+====================================+=========+
| ``x + y``      | sum of *x* and *y*                 |         |
+----------------+------------------------------------+---------+
| ``x - y``      | difference of *x* and *y*          |         |
+----------------+------------------------------------+---------+
| ``x * y``      | product of *x* and *y*             |         |
+----------------+------------------------------------+---------+
| ``x // y``     | floored quotient of *x* and *y*    |         |
+----------------+------------------------------------+---------+
| ``x % y``      | remainder of ``x / y``             |         |
+----------------+------------------------------------+---------+
| ``x ** y``     | *x* to the power *y*               |         |
+----------------+------------------------------------+---------+
| ``-x``         | *x* negated                        |         |
+----------------+------------------------------------+---------+
| ``+x``         | *x* unchanged                      |         |
+----------------+------------------------------------+---------+
| ``int(x)``     | *x* converted to integer           |         |
+----------------+------------------------------------+---------+
| ``float(x)``   | *x* converted to floating point    |         |
+----------------+------------------------------------+---------+
| ``pow(x)``     | *x* to the power *y*               |         |
+----------------+------------------------------------+---------+


It also has the following methods.

.. automethod:: mpz.conjugate
.. automethod:: mpz.bit_length
.. automethod:: mpz.bit_count
.. automethod:: mpz.as_integer_ratio
.. automethod:: mpz.is_integer
.. automethod:: mpz.digits
