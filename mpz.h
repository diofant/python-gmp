#ifndef MPZ_H
#define MPZ_H

#if defined(__MINGW32__) && defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Warray-bounds"
#endif
#if defined(__clang__)
#  pragma GCC diagnostic push /* XXX: pypy/pypy#5312 */
#  pragma GCC diagnostic ignored "-Wnewline-eof"
#endif
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push /* XXX: oracle/graalpython#580 */
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

#include "zz/zz.h"

typedef struct {
    PyObject_HEAD
    Py_hash_t hash_cache;
    zz_t z;
} MPZ_Object;

#define MPZ_CheckExact(st, u) Py_IS_TYPE((u), (st)->MPZ_Type)
#define MPZ_Check(st, u) PyObject_TypeCheck((u), (st)->MPZ_Type)

typedef struct {
    PyTypeObject *MPZ_Type;
} gmp_state;

#endif /* MPZ_H */
