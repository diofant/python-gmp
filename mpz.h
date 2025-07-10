#ifndef MPZ_H
#define MPZ_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "zz.h"

typedef struct {
    PyObject_HEAD
    zz_t z;
} MPZ_Object;

#define MPZ_CheckExact(st, u) Py_IS_TYPE((u), (st)->MPZ_Type)
#define MPZ_Check(st, u) PyObject_TypeCheck((u), (st)->MPZ_Type)

typedef struct {
    void *(*default_allocate_func)(size_t);
    void *(*default_reallocate_func)(void *, size_t, size_t);
    void (*default_free_func)(void *, size_t);

    PyTypeObject *MPZ_Type;
} gmp_state;

#endif /* MPZ_H */
