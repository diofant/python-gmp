#ifndef MPZ_H
#define MPZ_H

#include "utils.h"
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
