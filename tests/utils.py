import math
import string
import sys

from gmp import mpz_info
from hypothesis.strategies import (
    booleans,
    complex_numbers,
    composite,
    floats,
    integers,
    sampled_from,
)

BITS_PER_DIGIT = mpz_info.bits_per_digit
SIZEOF_DIGIT = mpz_info.sizeof_digit
BITCNT_MAX = mpz_info.bitcnt_max
MAX_FACTORIAL_CACHE = 1000


def python_gcdext(a, b):
    if not a and not b:
        return 0, 0, 0
    if not a:
        return abs(b), 0, b//abs(b)
    if not b:
        return abs(a), a//abs(a), 0
    if a < 0:
        a, x_sign = -a, -1
    else:
        x_sign = 1
    if b < 0:
        b, y_sign = -b, -1
    else:
        y_sign = 1
    x, y, r, s = 1, 0, 0, 1
    while b:
        c, q = a % b, a // b
        a, b, r, s, x, y = b, c, x - q*r, y - q*s, r, s
    return a, x*x_sign, y*y_sign


def python_isqrtrem(x):
    y = math.isqrt(x)
    return y, x - y*y


DBL_MAX_EXP = sys.float_info.max_exp
DBL_MIN_EXP = sys.float_info.min_exp
DBL_MANT_DIG = sys.float_info.mant_dig
DBL_MIN_OVERFLOW = 2**DBL_MAX_EXP - 2**(DBL_MAX_EXP - DBL_MANT_DIG - 1)


# Pure Python version of correctly-rounded true division,
# copied from CPython test suite (Lib/test/test_long.py).
# The CPython's true division of integers suffers from double
# rounding on x86 machines that operate with e x87 FPU set
# to 64-bit precision.  See python/cpython#142449.
def python_truediv(a, b):
    """Correctly-rounded true division for integers."""
    negative = a^b < 0
    a, b = abs(a), abs(b)

    # exceptions:  division by zero, overflow
    if not b:
        raise ZeroDivisionError("division by zero")
    if a >= DBL_MIN_OVERFLOW * b:
        raise OverflowError("int/int too large to represent as a float")

   # find integer d satisfying 2**(d - 1) <= a/b < 2**d
    d = a.bit_length() - b.bit_length()
    if d >= 0 and a >= 2**d * b or d < 0 and a * 2**-d >= b:
        d += 1

    # compute 2**-exp * a / b for suitable exp
    exp = max(d, DBL_MIN_EXP) - DBL_MANT_DIG
    a, b = a << max(-exp, 0), b << max(exp, 0)
    q, r = divmod(a, b)

    # round-half-to-even: fractional part is r/b, which is > 0.5 iff
    # 2*r > b, and == 0.5 iff 2*r == b.
    if 2*r > b or 2*r == b and q % 2 == 1:
        q += 1

    result = math.ldexp(q, exp)
    return -result if negative else result


def to_digits(n, base):
    if n == 0:
        return "0"
    if base < 2 or base > 36:
        raise ValueError("mpz base must be >= 2 and <= 36")
    num_to_text = string.digits + string.ascii_lowercase
    digits = []
    if n < 0:
        sign = "-"
        n = -n
    else:
        sign = ""
    while n:
        i = n % base
        d = num_to_text[i]
        digits.append(d)
        n //= base
    return sign + "".join(digits[::-1])


@composite
def fmt_str(draw, types="bdoxXn"):
    res = ""
    type = draw(sampled_from(types))

    # fill_char and align
    fill_char = draw(sampled_from([""]*3 + list("z;clxvjqwer")))
    if fill_char:
        skip_0_padding = True
        align = draw(sampled_from(list("<^>=")))
        res += fill_char + align
    else:
        align = draw(sampled_from([""] + list("<^>=")))
        if align:
            skip_0_padding = True
            res += align
        else:
            skip_0_padding = False

    # sign character
    res += draw(sampled_from([""] + list("-+ ")))

    # alternate mode
    res += draw(sampled_from(["", "#"]))

    # pad with 0s
    pad0 = draw(sampled_from(["", "0"]))
    skip_thousand_separators = False
    if pad0 and not skip_0_padding:
        res += pad0
        skip_thousand_separators = True

    # Width
    res += draw(sampled_from([""]*7 + list(map(str, range(1, 40)))))

    # grouping character (thousand_separators)
    gchar = draw(sampled_from([""] + list(",_")))
    if (gchar and not skip_thousand_separators
            and not (gchar == "," and type in ["b", "o", "x", "X"])
            and type != "n"):
        res += gchar

    # Type
    res += type

    return res


@composite
def bigints(draw, min_value=None, max_value=None):
    # Hypothesis uses integer sizes up to 128-bit in unbounded case with
    # different weights (16-bit - most preferred).  If upper/lower boundaries
    # are specified, values near boundries are more likely (roughly 1.56%
    # chance for boundary and 0.78% - for 1-bit off).
    max_digit = 1<<BITS_PER_DIGIT
    ndigits = draw(sampled_from([1]*12 +
                                [2]*8 +
                                [3]*6 +
                                [4]*4 +
                                [5]*2 +
                                [6]))
    max_abs = max_digit**ndigits
    if min_value is None:
        min_value = -max_abs
    if max_value is None:
        max_value = max_abs
    min_value = max(min_value, -max_abs)
    max_value = min(max_value, +max_abs)
    return draw(integers(min_value=min_value, max_value=max_value))


@composite
def numbers(draw):
    if draw(booleans()):
        return draw(floats())
    return draw(complex_numbers())


# All supported rounding modes
round_nearest = sys.intern("n")
round_floor = sys.intern("f")
round_ceiling = sys.intern("c")
round_up = sys.intern("u")
round_down = sys.intern("d")
round_fast = round_down


# These masks are used to pick out segments of numbers to determine
# which direction to round when rounding to nearest.
class h_mask_big:
    def __getitem__(self, n):
        return (1<<(n-1))-1

h_mask_small = [0]+[((1<<(_-1))-1) for _ in range(1, 300)]
h_mask = [h_mask_big(), h_mask_small]


# The >> operator rounds to floor. shifts_down[rnd][sign]
# tells whether this is the right direction to use, or if the
# number should be negated before shifting
shifts_down = {round_floor:(1,0), round_ceiling:(0,1),
               round_down:(1,1), round_up:(0,0)}


small_trailing = [0] * 256
for j in range(1, 8):
    small_trailing[1<<j::1<<(j+1)] = [j] * (1<<(7-j))


def trailing(n):
    """Count the number of trailing zero bits in abs(n)."""
    if not n:
        return 0
    low_byte = n & 0xff
    if low_byte:
        return small_trailing[low_byte]
    t = 8
    n >>= 8
    while not n & 0xff:
        n >>= 8
        t += 8
    return t + small_trailing[n & 0xff]

trailtable = [trailing(n) for n in range(256)]


def mpmath_normalize(sign, man, exp, bc, prec, rnd):
    """
    Create a raw mpf tuple with value (-1)**sign * man * 2**exp and
    normalized mantissa. The mantissa is rounded in the specified
    direction if its size exceeds the precision. Trailing zero bits
    are also stripped from the mantissa to ensure that the
    representation is canonical.
    """
    if not man:
        return 0, 0, 0, 0
    # Cut mantissa down to size if larger than target precision
    n = bc - prec
    if n > 0:
        if rnd == round_nearest:
            t = man >> (n-1)
            if t & 1 and ((t & 2) or (man & h_mask[n<300][n])):
                man = (t>>1)+1
            else:
                man = t>>1
        elif shifts_down[rnd][sign]:
            man >>= n
        else:
            man = -((-man)>>n)
        exp += n
        bc = prec
    # Strip trailing bits
    if not man & 1:
        t = trailtable[man & 255]
        if not t:
            while not man & 255:
                man >>= 8
                exp += 8
                bc -= 8
            t = trailtable[man & 255]
        man >>= t
        exp += t
        bc -= t
    # Bit count can be wrong if the input mantissa was 1 less than
    # a power of 2 and got rounded up, thereby adding an extra bit.
    # With trailing bits removed, all powers of two have mantissa 1,
    # so this is easy to check for.
    if man == 1:
        bc = 1
    return sign, man, exp, bc


def mpmath_from_man_exp(man, exp, prec=0, rnd=round_fast):
    """Create raw mpf from (man, exp) pair. The mantissa may be signed.
    If no precision is specified, the mantissa is stored exactly."""
    sign = 0
    if man < 0:
        sign = 1
        man = -man
    bc = man.bit_length()
    if not prec:
        if not man:
            return 0, 0, 0, 0
        if not man & 1:
            if man & 2:
                return sign, man >> 1, exp + 1, bc - 1
            t = trailtable[man & 255]
            if not t:
                while not man & 255:
                    man >>= 8
                    exp += 8
                    bc -= 8
                t = trailtable[man & 255]
            man >>= t
            exp += t
            bc -= t
        return sign, man, exp, bc
    return mpmath_normalize(sign, man, exp, bc, prec, rnd)
