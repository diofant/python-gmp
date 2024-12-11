"""
python-gmp documentation build configuration file.

This file is execfile()d with the current directory set to its containing dir.

The contents of this file are pickled, so don't put values in the namespace
that aren't pickleable (module imports are okay, they're removed
automatically).
"""

import packaging.version

import gmp

# Add any Sphinx extension module names here, as strings. They can be extensions
# coming with Sphinx (named 'sphinx.ext.*') or your custom ones.
extensions = ['sphinx.ext.autodoc']

# The name of a reST role (builtin or Sphinx extension) to use as the
# default role, that is, for text marked up `like this`.
default_role = 'py:obj'

# Sphinx will warn about all references where the target cannot be found.
nitpicky = True

# This value selects if automatically documented members are sorted
# alphabetical (value 'alphabetical'), by member type (value 'groupwise')
# or by source order (value 'bysource').
autodoc_member_order = 'groupwise'

# The default options for autodoc directives. They are applied to all
# autodoc directives automatically.
autodoc_default_options = {'members': True}

# General information about the project.
project = gmp.__package__
copyright = '2024, Sergey B Kirpichev'

gmp_version = packaging.version.parse(gmp.__version__)

# The version info for the project you're documenting, acts as replacement for
# |version| and |release|, also used in various other places throughout the
# built documents.
#
# The short X.Y version.
version = f"{gmp_version.major}.{gmp_version.minor}"
# The full version, including alpha/beta/rc tags.
release = gmp.__version__

# Grouping the document tree into LaTeX files. List of tuples
# (source start file, target name, title, author, documentclass [howto/manual]).
latex_documents = [('index', 'python-gmp.tex', 'python-gmp Documentation',
                    'Sergey B Kirpichev', 'manual')]
