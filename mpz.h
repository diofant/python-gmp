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

#if !defined(PYPY_VERSION)
#  define CACHE_SIZE (99)
#else
#  define CACHE_SIZE (0)
#endif
#define MAX_CACHE_MPZ_LIMBS (64)

typedef struct {
    void *(*default_allocate_func)(size_t);
    void *(*default_reallocate_func)(void *, size_t, size_t);
    void (*default_free_func)(void *, size_t);

    PyTypeObject *MPZ_Type;

    MPZ_Object *gmp_cache[CACHE_SIZE + 1];
    size_t gmp_cache_size;
} gmp_state;

#endif /* MPZ_H */
