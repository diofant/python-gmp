#ifndef MPZ_H
#define MPZ_H

#if defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wnewline-eof"
#endif

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#if defined(__clang__)
#  pragma GCC diagnostic pop
#endif

#include "zz.h"

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
