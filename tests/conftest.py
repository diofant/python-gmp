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
  The GNU GMP version: {gmp.gmp_info[2]}
  Bits per limb:       {gmp.gmp_info[0]}
  Size of a limb:      {gmp.gmp_info[1]}
""")


def pytest_sessionstart(session):
    os.environ["MPMATH_NOGMPY"] = "Y"
