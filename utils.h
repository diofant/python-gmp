#ifndef UTILS_H
#define UTILS_H

#if defined(__MINGW32__) && defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Warray-bounds"
#endif
#if defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wnewline-eof" /* XXX: pypy/pypy#5312 */
#endif
/* For GraalVM: unicodeobject.h, implicit conversion changes
   signedness: 'enum PyUnicode_Kind' to 'int' */
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

#include "pythoncapi_compat.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif
#if defined(__clang__)
#  pragma GCC diagnostic pop
#endif
#if defined(__MINGW32__) && defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

typedef struct gmp_pyargs {
    Py_ssize_t maxpos;
    Py_ssize_t minargs;
    Py_ssize_t maxargs;
    const char *fname;
    const char *const *keywords;
} gmp_pyargs;

int gmp_parse_pyargs(const gmp_pyargs *fnargs, Py_ssize_t argidx[],
                     PyObject *const *args, Py_ssize_t nargs,
                     PyObject *kwnames);

PyObject * gmp_PyUnicode_TransformDecimalAndSpaceToASCII(PyObject *unicode);

#endif /* UTILS_H */
