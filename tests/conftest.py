import os
import platform
from datetime import timedelta

import gmp
from hypothesis import settings

default = settings.get_profile("default")
settings.register_profile("default",
                          settings(default,
                                   deadline=timedelta(seconds=300)))
ci = settings.get_profile("ci")
if platform.python_implementation() != "GraalVM":
    ci = settings(ci, max_examples=10000)
else:
    ci = settings(ci, max_examples=1000)
settings.register_profile("ci", ci)


def pytest_report_header(config):
    print(f"""
  The GNU GMP version: {gmp.gmp_info.version}
  Bits per limb:       {gmp.gmp_info.bits_per_limb}
  sizeof(mp_limb_t):   {gmp.gmp_info.sizeof_limb}
  sizeof(mp_size_t):   {gmp.gmp_info.sizeof_limbcnt}
  sizeof(mp_bitcnt_t): {gmp.gmp_info.sizeof_bitcnt}
""")


def pytest_sessionstart(session):
    os.environ["MPMATH_NOGMPY"] = "Y"
