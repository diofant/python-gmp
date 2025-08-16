import math
import string
from functools import lru_cache

from gmp import gmp_info
from hypothesis.strategies import (
    booleans,
    complex_numbers,
    composite,
    floats,
    integers,
    sampled_from,
)

BITS_PER_LIMB = gmp_info[0]
SIZEOF_LIMB = gmp_info[1]
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


@lru_cache(maxsize=250)
def python_fib(n):
    if n < 0:
        return (-1)**(-n+1) * python_fib(-n)
    # Use Dijkstra's logarithmic algorithm
    # The following implementation is basically equivalent to
    # http://en.literateprograms.org/Fibonacci_numbers_(Scheme)
    a, b, p, q = 1, 0, 0, 1
    while n:
        if n & 1:
            aq = a*q
            a, b = b*q + aq + a*p, b*p + aq
            n -= 1
        else:
            qq = q*q
            p, q = p*p + qq, qq + 2*p*q
            n >>= 1
    return b


def python_fac2(n, _memo_pair=[{0:1}, {1:1}]):
    """Return n!! (double factorial), integers n >= 0 only."""
    memo = _memo_pair[n&1]
    f = memo.get(n)
    if f:
        return f
    k = max(memo)
    p = memo[k]
    while k < n:
        k += 2
        p *= k
        if k <= MAX_FACTORIAL_CACHE:
            memo[k] = p
    return p


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
    max_digit = 1<<BITS_PER_LIMB
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
        return draw(floats(allow_nan=False, allow_infinity=False))
    return draw(complex_numbers(allow_nan=False, allow_infinity=False))
