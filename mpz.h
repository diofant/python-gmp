#ifndef MPZ_H
#define MPZ_H

#if defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wnewline-eof"
#endif

#include "pythoncapi_compat.h"

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

extern PyTypeObject MPZ_Type;

#define MPZ_CheckExact(u) Py_IS_TYPE((u), &MPZ_Type)
#define MPZ_Check(u) PyObject_TypeCheck((u), &MPZ_Type)

#endif /* MPZ_H */
