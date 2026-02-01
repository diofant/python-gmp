#!/usr/bin/env python3
import os
import re
import subprocess


def git_version():
    # Append last commit date and hash to dev version information,
    # if available
    return version, git_hash


if __name__ == "__main__":
    try:
        p = subprocess.Popen(["git", "describe"],
                             stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE,
                             cwd=os.path.dirname(__file__))
    except FileNotFoundError:
        exit(1)
    else:
        out, err = p.communicate()
        if p.returncode:
            exit(p.returncode)
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
    print(version)
    exit(0)
