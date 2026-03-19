#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
import sys
import textwrap


def git_version():
    p = subprocess.Popen(["git", "describe"],
                         stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE,
                         cwd=os.path.dirname(__file__))
    out, err = p.communicate()
    if p.returncode:
        raise RuntimeError("Non-zero return code from git-describe: "
                           f"{p.returncode}")
    out = out.decode("ascii").removesuffix("\n")

    version, *other = out.removesuffix("\n").split("-")
    if other:
        g, h = other
        m = re.match("(.*)([0-9]+)", version)
        version = m[1] + str(int(m[2])+1) + ".dev" + g
        git_hash = h
    else:
        git_hash = ""

    if git_hash:
        version += "+" + git_hash
    return version


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--write", help="Save version to this file")
    parser.add_argument(
        "--meson-dist",
        help="Output path is relative to MESON_DIST_ROOT",
        action="store_true"
    )
    args = parser.parse_args()

    try:
        version = git_version()
    except (FileNotFoundError, RuntimeError):
        sys.path.insert(0, os.getcwd())
        from version import version

    if args.write:
        template = textwrap.dedent(f"""
            version = "{version}"
            __version__ = version
                                   """)

        outfile = args.write
        if args.meson_dist:
            outfile = os.path.join(
                os.environ.get("MESON_DIST_ROOT", ""),
                outfile
            )

        # Print human readable output path
        relpath = os.path.relpath(outfile)
        if relpath.startswith("."):
            relpath = outfile

        with open(outfile, "w") as f:
            print(f"Saving version to {relpath}")
            f.write(template)
    else:
        print(version)
