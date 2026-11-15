#include "utils.h"

int
gmp_parse_pyargs(const gmp_pyargs *fnargs, Py_ssize_t argidx[],
                 PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    if (nargs > fnargs->maxpos) {
        PyErr_Format(PyExc_TypeError,
                     "%s() takes at most %zu positional arguments",
                     fnargs->fname, fnargs->maxpos);
        return -1;
    }
    for (Py_ssize_t i = 0; i < nargs; i++) {
        argidx[i] = i;
    }

    Py_ssize_t nkws = 0;

    if (kwnames) {
        nkws = PyTuple_Size(kwnames);
    }
    if (nkws > fnargs->maxpos) {
        PyErr_Format(PyExc_TypeError,
                     "%s() takes at most %zu keyword arguments", fnargs->fname,
                     fnargs->maxargs);
        return -1;
    }
    if (nkws + nargs < fnargs->minargs) {
        PyErr_Format(PyExc_TypeError,
                     ("%s() takes at least %zu positional or "
                      "keyword arguments"),
                     fnargs->fname, fnargs->minargs);
        return -1;
    }
    for (Py_ssize_t i = 0; i < nkws; i++) {
        const char *kwname = PyUnicode_AsUTF8AndSize(PyTuple_GetItem(kwnames,
                                                                     i), NULL);
        Py_ssize_t j = 0;

        for (; j < fnargs->maxargs; j++) {
            if (strcmp(kwname, fnargs->keywords[j]) == 0) {
                if (j > fnargs->maxpos || nargs <= j) {
                    argidx[j] = (int)(nargs + i);
                    break;
                }
                else {
                    PyErr_Format(PyExc_TypeError,
                                 ("argument for %s() given by name "
                                  "('%s') and position (%zu)"),
                                 fnargs->fname, fnargs->keywords[j], j + 1);
                    return -1;
                }
            }
        }
        if (j == fnargs->maxargs) {
            PyErr_Format(PyExc_TypeError,
                         "%s() got an unexpected keyword argument '%s'",
                         fnargs->fname, kwname);
            return -1;
        }
    }
    return 0;
}
