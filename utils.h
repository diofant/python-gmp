#ifndef UTILS_H
#define UTILS_H

#include <string.h>

/* For GraalVM: unicodeobject.h, implicit conversion changes
   signedness: 'enum PyUnicode_Kind' to 'int' */
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push /* XXX: oracle/graalpython#580 */
#  pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

#define ON_CPYTHON 1
#if defined(PYPY_VERSION) || defined(GRAALVM_PYTHON)
#  undef ON_CPYTHON
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

#if PY_VERSION_HEX < 0x030D00A6
PyObject * PyEval_GetFrameBuiltins(void);
#endif

#endif /* UTILS_H */
