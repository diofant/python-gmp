Documentation for python-gmp
============================

.. automodule:: gmp


.. currentmodule:: gmp

.. data:: mpz_info

   A named tuple that holds information about mpz type.  The attributes are
   read only.

   .. attribute:: mpz_info.bits_per_digit

      The size of a digit in bits.

   .. attribute:: sizeof_digit

      The size in bytes of the C type used to represent a digit.

   .. attribute:: bitcnt_max

      The maximal count of bits in an integer.
