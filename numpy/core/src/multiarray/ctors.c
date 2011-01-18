#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "structmember.h"

#define _MULTIARRAYMODULE
#define NPY_NO_PREFIX
#include "numpy/arrayobject.h"
#include "numpy/arrayscalars.h"

#include "numpy/npy_math.h"

#include "npy_config.h"

#include "numpy/npy_3kcompat.h"

#include "common.h"

#include "ctors.h"

#include "buffer.h"

#include "numpymemoryview.h"

#include "lowlevel_strided_loops.h"

/*
 * Reading from a file or a string.
 *
 * As much as possible, we try to use the same code for both files and strings,
 * so the semantics for fromstring and fromfile are the same, especially with
 * regards to the handling of text representations.
 */

typedef int (*next_element)(void **, void *, PyArray_Descr *, void *);
typedef int (*skip_separator)(void **, const char *, void *);

static int
fromstr_next_element(char **s, void *dptr, PyArray_Descr *dtype,
                     const char *end)
{
    int r = dtype->f->fromstr(*s, dptr, s, dtype);
    if (end != NULL && *s > end) {
        return -1;
    }
    return r;
}

static int
fromfile_next_element(FILE **fp, void *dptr, PyArray_Descr *dtype,
                      void *NPY_UNUSED(stream_data))
{
    /* the NULL argument is for backwards-compatibility */
    return dtype->f->scanfunc(*fp, dptr, NULL, dtype);
}

/*
 * Remove multiple whitespace from the separator, and add a space to the
 * beginning and end. This simplifies the separator-skipping code below.
 */
static char *
swab_separator(char *sep)
{
    int skip_space = 0;
    char *s, *start;

    s = start = malloc(strlen(sep)+3);
    /* add space to front if there isn't one */
    if (*sep != '\0' && !isspace(*sep)) {
        *s = ' '; s++;
    }
    while (*sep != '\0') {
        if (isspace(*sep)) {
            if (skip_space) {
                sep++;
            }
            else {
                *s = ' ';
                s++;
                sep++;
                skip_space = 1;
            }
        }
        else {
            *s = *sep;
            s++;
            sep++;
            skip_space = 0;
        }
    }
    /* add space to end if there isn't one */
    if (s != start && s[-1] == ' ') {
        *s = ' ';
        s++;
    }
    *s = '\0';
    return start;
}

/*
 * Assuming that the separator is the next bit in the string (file), skip it.
 *
 * Single spaces in the separator are matched to arbitrary-long sequences
 * of whitespace in the input. If the separator consists only of spaces,
 * it matches one or more whitespace characters.
 *
 * If we can't match the separator, return -2.
 * If we hit the end of the string (file), return -1.
 * Otherwise, return 0.
 */
static int
fromstr_skip_separator(char **s, const char *sep, const char *end)
{
    char *string = *s;
    int result = 0;
    while (1) {
        char c = *string;
        if (c == '\0' || (end != NULL && string >= end)) {
            result = -1;
            break;
        }
        else if (*sep == '\0') {
            if (string != *s) {
                /* matched separator */
                result = 0;
                break;
            }
            else {
                /* separator was whitespace wildcard that didn't match */
                result = -2;
                break;
            }
        }
        else if (*sep == ' ') {
            /* whitespace wildcard */
            if (!isspace(c)) {
                sep++;
                continue;
            }
        }
        else if (*sep != c) {
            result = -2;
            break;
        }
        else {
            sep++;
        }
        string++;
    }
    *s = string;
    return result;
}

static int
fromfile_skip_separator(FILE **fp, const char *sep, void *NPY_UNUSED(stream_data))
{
    int result = 0;
    const char *sep_start = sep;

    while (1) {
        int c = fgetc(*fp);

        if (c == EOF) {
            result = -1;
            break;
        }
        else if (*sep == '\0') {
            ungetc(c, *fp);
            if (sep != sep_start) {
                /* matched separator */
                result = 0;
                break;
            }
            else {
                /* separator was whitespace wildcard that didn't match */
                result = -2;
                break;
            }
        }
        else if (*sep == ' ') {
            /* whitespace wildcard */
            if (!isspace(c)) {
                sep++;
                sep_start++;
                ungetc(c, *fp);
            }
            else if (sep == sep_start) {
                sep_start--;
            }
        }
        else if (*sep != c) {
            ungetc(c, *fp);
            result = -2;
            break;
        }
        else {
            sep++;
        }
    }
    return result;
}

/*
 * Change a sub-array field to the base descriptor
 * and update the dimensions and strides
 * appropriately.  Dimensions and strides are added
 * to the end.
 *
 * Strides are only added if given (because data is given).
 */
static int
_update_descr_and_dimensions(PyArray_Descr **des, npy_intp *newdims,
                             npy_intp *newstrides, int oldnd)
{
    PyArray_Descr *old;
    int newnd;
    int numnew;
    npy_intp *mydim;
    int i;
    int tuple;

    old = *des;
    *des = old->subarray->base;


    mydim = newdims + oldnd;
    tuple = PyTuple_Check(old->subarray->shape);
    if (tuple) {
        numnew = PyTuple_GET_SIZE(old->subarray->shape);
    }
    else {
        numnew = 1;
    }


    newnd = oldnd + numnew;
    if (newnd > MAX_DIMS) {
        goto finish;
    }
    if (tuple) {
        for (i = 0; i < numnew; i++) {
            mydim[i] = (npy_intp) PyInt_AsLong(
                    PyTuple_GET_ITEM(old->subarray->shape, i));
        }
    }
    else {
        mydim[0] = (npy_intp) PyInt_AsLong(old->subarray->shape);
    }

    if (newstrides) {
        npy_intp tempsize;
        npy_intp *mystrides;

        mystrides = newstrides + oldnd;
        /* Make new strides -- alwasy C-contiguous */
        tempsize = (*des)->elsize;
        for (i = numnew - 1; i >= 0; i--) {
            mystrides[i] = tempsize;
            tempsize *= mydim[i] ? mydim[i] : 1;
        }
    }

 finish:
    Py_INCREF(*des);
    Py_DECREF(old);
    return newnd;
}

/*
 * If s is not a list, return 0
 * Otherwise:
 *
 * run object_depth_and_dimension on all the elements
 * and make sure the returned shape and size is the
 * same for each element
 */
static int
object_depth_and_dimension(PyObject *s, int max, npy_intp *dims)
{
    npy_intp *newdims, *test_dims;
    int nd, test_nd;
    int i, islist, istuple;
    npy_intp size;
    PyObject *obj;

    islist = PyList_Check(s);
    istuple = PyTuple_Check(s);
    if (!(islist || istuple)) {
        return 0;
    }

    size = PySequence_Size(s);
    if (size == 0) {
        return 0;
    }
    if (max < 1) {
        return 0;
    }
    if (max < 2) {
        dims[0] = size;
        return 1;
    }

    newdims = PyDimMem_NEW(2*(max - 1));
    test_dims = newdims + (max - 1);
    if (islist) {
        obj = PyList_GET_ITEM(s, 0);
    }
    else {
        obj = PyTuple_GET_ITEM(s, 0);
    }
    nd = object_depth_and_dimension(obj, max - 1, newdims);

    for (i = 1; i < size; i++) {
        if (islist) {
            obj = PyList_GET_ITEM(s, i);
        }
        else {
            obj = PyTuple_GET_ITEM(s, i);
        }
        test_nd = object_depth_and_dimension(obj, max-1, test_dims);

        if ((nd != test_nd) ||
            (!PyArray_CompareLists(newdims, test_dims, nd))) {
            nd = 0;
            break;
        }
    }

    for (i = 1; i <= nd; i++) {
        dims[i] = newdims[i-1];
    }
    dims[0] = size;
    PyDimMem_FREE(newdims);
    return nd + 1;
}

static void
_strided_byte_copy(char *dst, npy_intp outstrides, char *src,
                    npy_intp instrides, npy_intp N, int elsize)
{
    npy_intp i, j;
    char *tout = dst;
    char *tin = src;

#define _FAST_MOVE(_type_)                              \
    for(i=0; i<N; i++) {                               \
        ((_type_ *)tout)[0] = ((_type_ *)tin)[0];       \
        tin += instrides;                               \
        tout += outstrides;                             \
    }                                                   \
    return

    switch(elsize) {
    case 8:
        _FAST_MOVE(Int64);
    case 4:
        _FAST_MOVE(Int32);
    case 1:
        _FAST_MOVE(Int8);
    case 2:
        _FAST_MOVE(Int16);
    case 16:
        for (i = 0; i < N; i++) {
            ((Int64 *)tout)[0] = ((Int64 *)tin)[0];
            ((Int64 *)tout)[1] = ((Int64 *)tin)[1];
            tin += instrides;
            tout += outstrides;
        }
        return;
    default:
        for(i = 0; i < N; i++) {
            for(j=0; j<elsize; j++) {
                *tout++ = *tin++;
            }
            tin = tin + instrides - elsize;
            tout = tout + outstrides - elsize;
        }
    }
#undef _FAST_MOVE

}

NPY_NO_EXPORT void
_unaligned_strided_byte_copy(char *dst, npy_intp outstrides, char *src,
                             npy_intp instrides, npy_intp N, int elsize)
{
    npy_intp i;
    char *tout = dst;
    char *tin = src;

#define _COPY_N_SIZE(size)                      \
    for(i=0; i<N; i++) {                       \
        memcpy(tout, tin, size);                \
        tin += instrides;                       \
        tout += outstrides;                     \
    }                                           \
    return

    switch(elsize) {
    case 8:
        _COPY_N_SIZE(8);
    case 4:
        _COPY_N_SIZE(4);
    case 1:
        _COPY_N_SIZE(1);
    case 2:
        _COPY_N_SIZE(2);
    case 16:
        _COPY_N_SIZE(16);
    default:
        _COPY_N_SIZE(elsize);
    }
#undef _COPY_N_SIZE

}

NPY_NO_EXPORT void
_strided_byte_swap(void *p, npy_intp stride, npy_intp n, int size)
{
    char *a, *b, c = 0;
    int j, m;

    switch(size) {
    case 1: /* no byteswap necessary */
        break;
    case 4:
        for (a = (char*)p; n > 0; n--, a += stride - 1) {
            b = a + 3;
            c = *a; *a++ = *b; *b-- = c;
            c = *a; *a = *b; *b   = c;
        }
        break;
    case 8:
        for (a = (char*)p; n > 0; n--, a += stride - 3) {
            b = a + 7;
            c = *a; *a++ = *b; *b-- = c;
            c = *a; *a++ = *b; *b-- = c;
            c = *a; *a++ = *b; *b-- = c;
            c = *a; *a = *b; *b   = c;
        }
        break;
    case 2:
        for (a = (char*)p; n > 0; n--, a += stride) {
            b = a + 1;
            c = *a; *a = *b; *b = c;
        }
        break;
    default:
        m = size/2;
        for (a = (char *)p; n > 0; n--, a += stride - m) {
            b = a + (size - 1);
            for (j = 0; j < m; j++) {
                c=*a; *a++ = *b; *b-- = c;
            }
        }
        break;
    }
}

NPY_NO_EXPORT void
byte_swap_vector(void *p, npy_intp n, int size)
{
    _strided_byte_swap(p, (npy_intp) size, n, size);
    return;
}

/* If numitems > 1, then dst must be contiguous */
NPY_NO_EXPORT void
copy_and_swap(void *dst, void *src, int itemsize, npy_intp numitems,
              npy_intp srcstrides, int swap)
{
    npy_intp i;
    char *s1 = (char *)src;
    char *d1 = (char *)dst;


    if ((numitems == 1) || (itemsize == srcstrides)) {
        memcpy(d1, s1, itemsize*numitems);
    }
    else {
        for (i = 0; i < numitems; i++) {
            memcpy(d1, s1, itemsize);
            d1 += itemsize;
            s1 += srcstrides;
        }
    }

    if (swap) {
        byte_swap_vector(d1, numitems, itemsize);
    }
}

/*
 * Special-case of PyArray_CopyInto when dst is 1-d
 * and contiguous (and aligned).
 * PyArray_CopyInto requires broadcastable arrays while
 * this one is a flattening operation...
 *
 * TODO: Delete this function when its usage is removed.
 */
NPY_NO_EXPORT int
_flat_copyinto(PyObject *dst, PyObject *src, NPY_ORDER order)
{
    PyArrayIterObject *it;
    PyObject *orig_src;
    void (*myfunc)(char *, npy_intp, char *, npy_intp, npy_intp, int);
    char *dptr;
    int axis;
    int elsize;
    npy_intp nbytes;
    NPY_BEGIN_THREADS_DEF;


    orig_src = src;
    if (PyArray_NDIM(src) == 0) {
        /* Refcount note: src and dst have the same size */
        PyArray_INCREF((PyArrayObject *)src);
        PyArray_XDECREF((PyArrayObject *)dst);
        NPY_BEGIN_THREADS;
        memcpy(PyArray_BYTES(dst), PyArray_BYTES(src),
                PyArray_ITEMSIZE(src));
        NPY_END_THREADS;
        return 0;
    }

    axis = PyArray_NDIM(src)-1;

    if (order == PyArray_FORTRANORDER) {
        if (PyArray_NDIM(src) <= 2) {
            axis = 0;
        }
        /* fall back to a more general method */
        else {
            src = PyArray_Transpose((PyArrayObject *)orig_src, NULL);
        }
    }

    it = (PyArrayIterObject *)PyArray_IterAllButAxis(src, &axis);
    if (it == NULL) {
        if (src != orig_src) {
            Py_DECREF(src);
        }
        return -1;
    }

    if (PyArray_SAFEALIGNEDCOPY(src)) {
        myfunc = _strided_byte_copy;
    }
    else {
        myfunc = _unaligned_strided_byte_copy;
    }

    dptr = PyArray_BYTES(dst);
    elsize = PyArray_ITEMSIZE(dst);
    nbytes = elsize * PyArray_DIM(src, axis);

    /* Refcount note: src and dst have the same size */
    PyArray_INCREF((PyArrayObject *)src);
    PyArray_XDECREF((PyArrayObject *)dst);
    NPY_BEGIN_THREADS;
    while(it->index < it->size) {
        myfunc(dptr, elsize, it->dataptr, PyArray_STRIDE(src,axis),
                PyArray_DIM(src,axis), elsize);
        dptr += nbytes;
        PyArray_ITER_NEXT(it);
    }
    NPY_END_THREADS;

    if (src != orig_src) {
        Py_DECREF(src);
    }
    Py_DECREF(it);
    return 0;
}

/* Gets a half-open range [start, end) which contains the array data */
void _get_memory_extents(PyArrayObject *arr,
                    npy_uintp *out_start, npy_uintp *out_end)
{
    npy_uintp start, end;
    npy_intp idim, ndim = PyArray_NDIM(arr);
    npy_intp *dimensions = PyArray_DIMS(arr),
            *strides = PyArray_STRIDES(arr);

    /* Calculate with a closed range [start, end] */
    start = end = (npy_uintp)PyArray_DATA(arr);
    for (idim = 0; idim < ndim; ++idim) {
        npy_intp stride = strides[idim], dim = dimensions[idim];
        /* If the array size is zero, return an empty range */
        if (dim == 0) {
            *out_start = *out_end = (npy_uintp)PyArray_DATA(arr);
            return;
        }
        /* Expand either upwards or downwards depending on stride */
        else {
            if (stride > 0) {
                end += stride*(dim-1);
            }
            else if (stride < 0) {
                start += stride*(dim-1);
            }
        }
    }

    /* Return a half-open range */
    *out_start = start;
    *out_end = end + arr->descr->elsize;
}

/* Returns 1 if the arrays have overlapping data, 0 otherwise */
int _arrays_overlap(PyArrayObject *arr1, PyArrayObject *arr2)
{
    npy_uintp start1 = 0, start2 = 0, end1 = 0, end2 = 0;

    _get_memory_extents(arr1, &start1, &end1);
    _get_memory_extents(arr2, &start2, &end2);

    return (start1 < end2) && (start2 < end1);
}

/*NUMPY_API
 * Move the memory of one array into another, allowing for overlapping data.
 *
 * This is in general a difficult problem to solve efficiently, because
 * strides can be negative.  Consider "a = np.arange(3); a[::-1] = a", which
 * currently incorrectly produces [0, 1, 0].
 *
 * Instead of trying to be fancy, we simply check for overlap and make
 * a temporary copy when one exists.
 *
 * A special case is when there is just one dimension with positive
 * strides, and we pass that to CopyInto, which correctly handles
 * it for most cases.  It may still incorrectly handle copying of
 * partially-overlapping data elements, where the data pointer was offset
 * by a fraction of the element size.
 */
NPY_NO_EXPORT int
PyArray_MoveInto(PyArrayObject *dst, PyArrayObject *src)
{
    if ((PyArray_NDIM(dst) == 1 &&
                        PyArray_NDIM(src) == 1 &&
                        PyArray_STRIDE(dst, 0) > 0 &&
                        PyArray_STRIDE(src, 0) > 0) ||
                        !_arrays_overlap(dst, src)) {
        return PyArray_CopyInto(dst, src);
    }
    else {
        PyArrayObject *tmp;
        int ret;

        /*
         * Allocate a temporary copy array.
         * TODO: For efficiency, this should have a memory ordering
         *       matching 'dst', even if 'dst' has its axes arbitrarily
         *       scrambled.  A function to allocate this array needs to
         *       be created.
         */
        Py_INCREF(PyArray_DESCR(dst));
        tmp = (PyArrayObject *)PyArray_NewFromDescr(&PyArray_Type,
                        PyArray_DESCR(dst),
                        PyArray_NDIM(dst), PyArray_DIMS(dst), NULL,
                        NULL, PyArray_ISFORTRAN(dst) ? NPY_F_CONTIGUOUS : 0,
                        NULL);
        if (tmp == NULL) {
            return -1;
        }
        ret = PyArray_CopyInto(tmp, src);
        if (ret == 0) {
            ret = PyArray_CopyInto(dst, tmp);
        }
        Py_DECREF(tmp);
        return ret;
    }
}



/* adapted from Numarray */
static int
setArrayFromSequence(PyArrayObject *a, PyObject *s, int dim, npy_intp offset)
{
    Py_ssize_t i, slen;
    int res = -1;

    /*
     * This code is to ensure that the sequence access below will
     * return a lower-dimensional sequence.
     */

    /* INCREF on entry DECREF on exit */
    Py_INCREF(s);

    if (PyArray_Check(s) && !(PyArray_CheckExact(s))) {
      /*
       * FIXME:  This could probably copy the entire subarray at once here using
       * a faster algorithm.  Right now, just make sure a base-class array is
       * used so that the dimensionality reduction assumption is correct.
       */
        /* This will DECREF(s) if replaced */
        s = PyArray_EnsureArray(s);
    }

    if (dim > a->nd) {
        PyErr_Format(PyExc_ValueError,
                     "setArrayFromSequence: sequence/array dimensions mismatch.");
        goto fail;
    }

    slen = PySequence_Length(s);
    if (slen != a->dimensions[dim]) {
        PyErr_Format(PyExc_ValueError,
                     "setArrayFromSequence: sequence/array shape mismatch.");
        goto fail;
    }

    for (i = 0; i < slen; i++) {
        PyObject *o = PySequence_GetItem(s, i);
        if ((a->nd - dim) > 1) {
            res = setArrayFromSequence(a, o, dim+1, offset);
        }
        else {
            res = a->descr->f->setitem(o, (a->data + offset), a);
        }
        Py_DECREF(o);
        if (res < 0) {
            goto fail;
        }
        offset += a->strides[dim];
    }

    Py_DECREF(s);
    return 0;

 fail:
    Py_DECREF(s);
    return res;
}

static int
Assign_Array(PyArrayObject *self, PyObject *v)
{
    if (!PySequence_Check(v)) {
        PyErr_SetString(PyExc_ValueError,
                        "assignment from non-sequence");
        return -1;
    }
    if (self->nd == 0) {
        PyErr_SetString(PyExc_ValueError,
                        "assignment to 0-d array");
        return -1;
    }
    return setArrayFromSequence(self, v, 0, 0);
}

/*
 * "Array Scalars don't call this code"
 * steals reference to typecode -- no NULL
 */
static PyObject *
Array_FromPyScalar(PyObject *op, PyArray_Descr *typecode)
{
    PyArrayObject *ret;
    int itemsize;
    int type;

    itemsize = typecode->elsize;
    type = typecode->type_num;

    if (itemsize == 0 && PyTypeNum_ISEXTENDED(type)) {
        itemsize = PyObject_Length(op);
        if (type == PyArray_UNICODE) {
            itemsize *= 4;
        }
        if (itemsize != typecode->elsize) {
            PyArray_DESCR_REPLACE(typecode);
            typecode->elsize = itemsize;
        }
    }

    ret = (PyArrayObject *)PyArray_NewFromDescr(&PyArray_Type, typecode,
                                                0, NULL,
                                                NULL, NULL, 0, NULL);
    if (ret == NULL) {
        return NULL;
    }
    if (ret->nd > 0) {
        PyErr_SetString(PyExc_ValueError,
                        "shape-mismatch on array construction");
        Py_DECREF(ret);
        return NULL;
    }

    ret->descr->f->setitem(op, ret->data, ret);
    if (PyErr_Occurred()) {
        Py_DECREF(ret);
        return NULL;
    }
    else {
        return (PyObject *)ret;
    }
}


static PyObject *
ObjectArray_FromNestedList(PyObject *s, PyArray_Descr *typecode, int fortran)
{
    int nd;
    npy_intp d[MAX_DIMS];
    PyArrayObject *r;

    /* Get the depth and the number of dimensions */
    nd = object_depth_and_dimension(s, MAX_DIMS, d);
    if (nd < 0) {
        return NULL;
    }
    if (nd == 0) {
        return Array_FromPyScalar(s, typecode);
    }
    r = (PyArrayObject*)PyArray_NewFromDescr(&PyArray_Type, typecode,
                                             nd, d,
                                             NULL, NULL,
                                             fortran, NULL);
    if (!r) {
        return NULL;
    }
    if(Assign_Array(r,s) == -1) {
        Py_DECREF(r);
        return NULL;
    }
    return (PyObject*)r;
}

/*
 * The rest of this code is to build the right kind of array
 * from a python object.
 */

static int
discover_depth(PyObject *s, int max, int stop_at_string, int stop_at_tuple)
{
    int d = 0;
    PyObject *e;
#if PY_VERSION_HEX >= 0x02060000
    Py_buffer buffer_view;
#endif

    if(max < 1) {
        return -1;
    }
    if(!PySequence_Check(s) ||
#if defined(NPY_PY3K)
       /* FIXME: XXX -- what is the correct thing to do here? */
#else
       PyInstance_Check(s) ||
#endif
       PySequence_Length(s) < 0) {
        PyErr_Clear();
        return 0;
    }
    if (PyArray_Check(s)) {
        return PyArray_NDIM(s);
    }
    if (PyArray_IsScalar(s, Generic)) {
        return 0;
    }
    if (PyString_Check(s) ||
#if defined(NPY_PY3K)
#else
        PyBuffer_Check(s) ||
#endif
        PyUnicode_Check(s)) {
        return stop_at_string ? 0:1;
    }
    if (stop_at_tuple && PyTuple_Check(s)) {
        return 0;
    }
#if PY_VERSION_HEX >= 0x02060000
    /* PEP 3118 buffer interface */
    memset(&buffer_view, 0, sizeof(Py_buffer));
    if (PyObject_GetBuffer(s, &buffer_view, PyBUF_STRIDES) == 0 ||
        PyObject_GetBuffer(s, &buffer_view, PyBUF_ND) == 0) {
        d = buffer_view.ndim;
        PyBuffer_Release(&buffer_view);
        return d;
    }
    else if (PyObject_GetBuffer(s, &buffer_view, PyBUF_SIMPLE) == 0) {
        PyBuffer_Release(&buffer_view);
        return 1;
    }
    else {
        PyErr_Clear();
    }
#endif
    if ((e = PyObject_GetAttrString(s, "__array_struct__")) != NULL) {
        d = -1;
        if (NpyCapsule_Check(e)) {
            PyArrayInterface *inter;
            inter = (PyArrayInterface *)NpyCapsule_AsVoidPtr(e);
            if (inter->two == 2) {
                d = inter->nd;
            }
        }
        Py_DECREF(e);
        if (d > -1) {
            return d;
        }
    }
    else {
        PyErr_Clear();
    }
    if ((e=PyObject_GetAttrString(s, "__array_interface__")) != NULL) {
        d = -1;
        if (PyDict_Check(e)) {
            PyObject *new;
            new = PyDict_GetItemString(e, "shape");
            if (new && PyTuple_Check(new)) {
                d = PyTuple_GET_SIZE(new);
            }
        }
        Py_DECREF(e);
        if (d>-1) {
            return d;
        }
    }
    else PyErr_Clear();

    if (PySequence_Length(s) == 0) {
        return 1;
    }
    if ((e=PySequence_GetItem(s,0)) == NULL) {
        return -1;
    }
    if (e != s) {
        d = discover_depth(e, max-1, stop_at_string, stop_at_tuple);
        if (d >= 0) {
            d++;
        }
    }
    Py_DECREF(e);
    return d;
}

static int
discover_itemsize(PyObject *s, int nd, int *itemsize)
{
    int n, r, i;
    PyObject *e;

    if (PyArray_Check(s)) {
        *itemsize = MAX(*itemsize, PyArray_ITEMSIZE(s));
        return 0;
    }

    n = PyObject_Length(s);
    if ((nd == 0) || PyString_Check(s) ||
#if defined(NPY_PY3K)
        PyMemoryView_Check(s) ||
#else
        PyBuffer_Check(s) ||
#endif
        PyUnicode_Check(s)) {

        *itemsize = MAX(*itemsize, n);
        return 0;
    }
    for (i = 0; i < n; i++) {
        if ((e = PySequence_GetItem(s,i))==NULL) {
            return -1;
        }
        r = discover_itemsize(e,nd-1,itemsize);
        Py_DECREF(e);
        if (r == -1) {
            return -1;
        }
    }
    return 0;
}

/*
 * Take an arbitrary object known to represent
 * an array of ndim nd, and determine the size in each dimension
 */
static int
discover_dimensions(PyObject *s, int nd, npy_intp *d, int check_it)
{
    PyObject *e;
    int r, n, i, n_lower;


    if (PyArray_Check(s)) {
        /*
         * XXXX: we handle the case of scalar arrays (0 dimensions) separately.
         * This is an hack, the function discover_dimensions needs to be
         * improved.
         */
        if (PyArray_NDIM(s) == 0) {
            d[0] = 0;
        } else {
            for (i=0; i<nd; i++) {
                d[i] = PyArray_DIM(s,i);
            }
        }
        return 0;
    }
    n = PyObject_Length(s);
    *d = n;
    if (*d < 0) {
        return -1;
    }
    if (nd <= 1) {
        return 0;
    }
    n_lower = 0;
    for(i = 0; i < n; i++) {
        if ((e = PySequence_GetItem(s,i)) == NULL) {
            return -1;
        }
        r = discover_dimensions(e, nd - 1, d + 1, check_it);
        Py_DECREF(e);

        if (r == -1) {
            return -1;
        }
        if (check_it && n_lower != 0 && n_lower != d[1]) {
            PyErr_SetString(PyExc_ValueError,
                            "inconsistent shape in sequence");
            return -1;
        }
        if (d[1] > n_lower) {
            n_lower = d[1];
        }
    }
    d[1] = n_lower;

    return 0;
}

/*
 * isobject means that we are constructing an
 * object array on-purpose with a nested list.
 * Only a list is interpreted as a sequence with these rules
 * steals reference to typecode
 */
static PyObject *
Array_FromSequence(PyObject *s, PyArray_Descr *typecode, int fortran,
                   int min_depth, int max_depth)
{
    PyArrayObject *r;
    int nd;
    int err;
    npy_intp d[MAX_DIMS];
    int stop_at_string;
    int stop_at_tuple;
    int check_it;
    int type = typecode->type_num;
    int itemsize = typecode->elsize;

    check_it = (typecode->type != PyArray_CHARLTR);
    stop_at_string = (type != PyArray_STRING) ||
                     (typecode->type == PyArray_STRINGLTR);
    stop_at_tuple = (type == PyArray_VOID && (typecode->names
                                              || typecode->subarray));

    nd = discover_depth(s, MAX_DIMS + 1, stop_at_string, stop_at_tuple);
    if (nd == 0) {
        return Array_FromPyScalar(s, typecode);
    }
    else if (nd < 0) {
        PyErr_SetString(PyExc_ValueError,
                "invalid input sequence");
        goto fail;
    }
    if (max_depth && PyTypeNum_ISOBJECT(type) && (nd > max_depth)) {
        nd = max_depth;
    }
    if ((max_depth && nd > max_depth) || (min_depth && nd < min_depth)) {
        PyErr_SetString(PyExc_ValueError,
                "invalid number of dimensions");
        goto fail;
    }

    err = discover_dimensions(s, nd, d, check_it);
    if (err == -1) {
        goto fail;
    }
    if (typecode->type == PyArray_CHARLTR && nd > 0 && d[nd - 1] == 1) {
        nd = nd - 1;
    }

    if (itemsize == 0 && PyTypeNum_ISEXTENDED(type)) {
        err = discover_itemsize(s, nd, &itemsize);
        if (err == -1) {
            goto fail;
        }
        if (type == PyArray_UNICODE) {
            itemsize *= 4;
        }
    }
    if (itemsize != typecode->elsize) {
        PyArray_DESCR_REPLACE(typecode);
        typecode->elsize = itemsize;
    }

    r = (PyArrayObject*)PyArray_NewFromDescr(&PyArray_Type, typecode,
                                             nd, d,
                                             NULL, NULL,
                                             fortran, NULL);
    if (!r) {
        return NULL;
    }

    err = Assign_Array(r,s);
    if (err == -1) {
        Py_DECREF(r);
        return NULL;
    }
    return (PyObject*)r;

 fail:
    Py_DECREF(typecode);
    return NULL;
}



/*NUMPY_API
 * Generic new array creation routine.
 *
 * steals a reference to descr (even on failure)
 */
NPY_NO_EXPORT PyObject *
PyArray_NewFromDescr(PyTypeObject *subtype, PyArray_Descr *descr, int nd,
                     npy_intp *dims, npy_intp *strides, void *data,
                     int flags, PyObject *obj)
{
    PyArrayObject *self;
    int i;
    size_t sd;
    npy_intp largest;
    npy_intp size;

    if (descr->subarray) {
        PyObject *ret;
        npy_intp newdims[2*MAX_DIMS];
        npy_intp *newstrides = NULL;
        memcpy(newdims, dims, nd*sizeof(npy_intp));
        if (strides) {
            newstrides = newdims + MAX_DIMS;
            memcpy(newstrides, strides, nd*sizeof(npy_intp));
        }
        nd =_update_descr_and_dimensions(&descr, newdims,
                                         newstrides, nd);
        ret = PyArray_NewFromDescr(subtype, descr, nd, newdims,
                                   newstrides,
                                   data, flags, obj);
        return ret;
    }
    if (nd < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "number of dimensions must be >=0");
        Py_DECREF(descr);
        return NULL;
    }
    if (nd > MAX_DIMS) {
        PyErr_Format(PyExc_ValueError,
                     "maximum number of dimensions is %d", MAX_DIMS);
        Py_DECREF(descr);
        return NULL;
    }

    /* Check dimensions */
    size = 1;
    sd = (size_t) descr->elsize;
    if (sd == 0) {
        if (!PyDataType_ISSTRING(descr)) {
            PyErr_SetString(PyExc_ValueError, "Empty data-type");
            Py_DECREF(descr);
            return NULL;
        }
        PyArray_DESCR_REPLACE(descr);
        if (descr->type_num == NPY_STRING) {
            descr->elsize = 1;
        }
        else {
            descr->elsize = sizeof(PyArray_UCS4);
        }
        sd = descr->elsize;
    }

    largest = NPY_MAX_INTP / sd;
    for (i = 0; i < nd; i++) {
        npy_intp dim = dims[i];

        if (dim == 0) {
            /*
             * Compare to PyArray_OverflowMultiplyList that
             * returns 0 in this case.
             */
            continue;
        }
        if (dim < 0) {
            PyErr_SetString(PyExc_ValueError,
                            "negative dimensions "  \
                            "are not allowed");
            Py_DECREF(descr);
            return NULL;
        }
        if (dim > largest) {
            PyErr_SetString(PyExc_ValueError,
                            "array is too big.");
            Py_DECREF(descr);
            return NULL;
        }
        size *= dim;
        largest /= dim;
    }

    self = (PyArrayObject *) subtype->tp_alloc(subtype, 0);
    if (self == NULL) {
        Py_DECREF(descr);
        return NULL;
    }
    self->nd = nd;
    self->dimensions = NULL;
    self->data = NULL;
    if (data == NULL) {
        self->flags = DEFAULT;
        if (flags) {
            self->flags |= FORTRAN;
            if (nd > 1) {
                self->flags &= ~CONTIGUOUS;
            }
            flags = FORTRAN;
        }
    }
    else {
        self->flags = (flags & ~UPDATEIFCOPY);
    }
    self->descr = descr;
    self->base = (PyObject *)NULL;
    self->weakreflist = (PyObject *)NULL;

    if (nd > 0) {
        self->dimensions = PyDimMem_NEW(2*nd);
        if (self->dimensions == NULL) {
            PyErr_NoMemory();
            goto fail;
        }
        self->strides = self->dimensions + nd;
        memcpy(self->dimensions, dims, sizeof(npy_intp)*nd);
        if (strides == NULL) { /* fill it in */
            sd = _array_fill_strides(self->strides, dims, nd, sd,
                                     flags, &(self->flags));
        }
        else {
            /*
             * we allow strides even when we create
             * the memory, but be careful with this...
             */
            memcpy(self->strides, strides, sizeof(npy_intp)*nd);
            sd *= size;
        }
    }
    else {
        self->dimensions = self->strides = NULL;
    }

    if (data == NULL) {
        /*
         * Allocate something even for zero-space arrays
         * e.g. shape=(0,) -- otherwise buffer exposure
         * (a.data) doesn't work as it should.
         */

        if (sd == 0) {
            sd = descr->elsize;
        }
        if ((data = PyDataMem_NEW(sd)) == NULL) {
            PyErr_NoMemory();
            goto fail;
        }
        self->flags |= OWNDATA;

        /*
         * It is bad to have unitialized OBJECT pointers
         * which could also be sub-fields of a VOID array
         */
        if (PyDataType_FLAGCHK(descr, NPY_NEEDS_INIT)) {
            memset(data, 0, sd);
        }
    }
    else {
        /*
         * If data is passed in, this object won't own it by default.
         * Caller must arrange for this to be reset if truly desired
         */
        self->flags &= ~OWNDATA;
    }
    self->data = data;

    /*
     * call the __array_finalize__
     * method if a subtype.
     * If obj is NULL, then call method with Py_None
     */
    if ((subtype != &PyArray_Type)) {
        PyObject *res, *func, *args;

        func = PyObject_GetAttrString((PyObject *)self, "__array_finalize__");
        if (func && func != Py_None) {
            if (strides != NULL) {
                /*
                 * did not allocate own data or funny strides
                 * update flags before finalize function
                 */
                PyArray_UpdateFlags(self, UPDATE_ALL);
            }
            if (NpyCapsule_Check(func)) {
                /* A C-function is stored here */
                PyArray_FinalizeFunc *cfunc;
                cfunc = NpyCapsule_AsVoidPtr(func);
                Py_DECREF(func);
                if (cfunc(self, obj) < 0) {
                    goto fail;
                }
            }
            else {
                args = PyTuple_New(1);
                if (obj == NULL) {
                    obj=Py_None;
                }
                Py_INCREF(obj);
                PyTuple_SET_ITEM(args, 0, obj);
                res = PyObject_Call(func, args, NULL);
                Py_DECREF(args);
                Py_DECREF(func);
                if (res == NULL) {
                    goto fail;
                }
                else {
                    Py_DECREF(res);
                }
            }
        }
        else Py_XDECREF(func);
    }
    return (PyObject *)self;

 fail:
    Py_DECREF(self);
    return NULL;
}

/*NUMPY_API
 * Generic new array creation routine.
 */
NPY_NO_EXPORT PyObject *
PyArray_New(PyTypeObject *subtype, int nd, npy_intp *dims, int type_num,
            npy_intp *strides, void *data, int itemsize, int flags,
            PyObject *obj)
{
    PyArray_Descr *descr;
    PyObject *new;

    descr = PyArray_DescrFromType(type_num);
    if (descr == NULL) {
        return NULL;
    }
    if (descr->elsize == 0) {
        if (itemsize < 1) {
            PyErr_SetString(PyExc_ValueError,
                            "data type must provide an itemsize");
            Py_DECREF(descr);
            return NULL;
        }
        PyArray_DESCR_REPLACE(descr);
        descr->elsize = itemsize;
    }
    new = PyArray_NewFromDescr(subtype, descr, nd, dims, strides,
                               data, flags, obj);
    return new;
}


NPY_NO_EXPORT int
_array_from_buffer_3118(PyObject *obj, PyObject **out)
{
#if PY_VERSION_HEX >= 0x02060000
    /* PEP 3118 */
    PyObject *memoryview;
    Py_buffer *view;
    PyArray_Descr *descr = NULL;
    PyObject *r;
    int nd, flags, k;
    Py_ssize_t d;
    npy_intp shape[NPY_MAXDIMS], strides[NPY_MAXDIMS];

    memoryview = PyMemoryView_FromObject(obj);
    if (memoryview == NULL) {
        PyErr_Clear();
        return -1;
    }

    view = PyMemoryView_GET_BUFFER(memoryview);
    if (view->format != NULL) {
        descr = _descriptor_from_pep3118_format(view->format);
        if (descr == NULL) {
            PyObject *msg;
            msg = PyBytes_FromFormat("Invalid PEP 3118 format string: '%s'",
                                     view->format);
            PyErr_WarnEx(PyExc_RuntimeWarning, PyBytes_AS_STRING(msg), 0);
            Py_DECREF(msg);
            goto fail;
        }

        /* Sanity check */
        if (descr->elsize != view->itemsize) {
            PyErr_WarnEx(PyExc_RuntimeWarning,
                         "Item size computed from the PEP 3118 buffer format "
                         "string does not match the actual item size.",
                         0);
            goto fail;
        }
    }
    else {
        descr = PyArray_DescrNewFromType(PyArray_STRING);
        descr->elsize = view->itemsize;
    }

    if (view->shape != NULL) {
        nd = view->ndim;
        if (nd >= NPY_MAXDIMS || nd < 0) {
            goto fail;
        }
        for (k = 0; k < nd; ++k) {
            if (k >= NPY_MAXDIMS) {
                goto fail;
            }
            shape[k] = view->shape[k];
        }
        if (view->strides != NULL) {
            for (k = 0; k < nd; ++k) {
                strides[k] = view->strides[k];
            }
        }
        else {
            d = view->len;
            for (k = 0; k < nd; ++k) {
                d /= view->shape[k];
                strides[k] = d;
            }
        }
    }
    else {
        nd = 1;
        shape[0] = view->len / view->itemsize;
        strides[0] = view->itemsize;
    }

    flags = BEHAVED & (view->readonly ? ~NPY_WRITEABLE : ~0);
    r = PyArray_NewFromDescr(&PyArray_Type, descr,
                             nd, shape, strides, view->buf,
                             flags, NULL);
    ((PyArrayObject *)r)->base = memoryview;
    PyArray_UpdateFlags((PyArrayObject *)r, UPDATE_ALL);

    *out = r;
    return 0;

fail:
    Py_XDECREF(descr);
    Py_DECREF(memoryview);
    return -1;

#else
    return -1;
#endif
}


/*NUMPY_API
 * Does not check for ENSURECOPY and NOTSWAPPED in flags
 * Steals a reference to newtype --- which can be NULL
 */
NPY_NO_EXPORT PyObject *
PyArray_FromAny(PyObject *op, PyArray_Descr *newtype, int min_depth,
                int max_depth, int flags, PyObject *context)
{
    /*
     * This is the main code to make a NumPy array from a Python
     * Object.  It is called from lot's of different places which
     * is why there are so many checks.  The comments try to
     * explain some of the checks.
     */
    PyObject *r = NULL;
    int seq = FALSE;

    /*
     * Is input object already an array?
     * This is where the flags are used
     */
    if (PyArray_Check(op)) {
        r = PyArray_FromArray((PyArrayObject *)op, newtype, flags);
    }
    else if (PyArray_IsScalar(op, Generic)) {
        if (flags & UPDATEIFCOPY) {
            goto err;
        }
        r = PyArray_FromScalar(op, newtype);
    }
    else if (newtype == NULL &&
               (newtype = _array_find_python_scalar_type(op))) {
        if (flags & UPDATEIFCOPY) {
            goto err;
        }
        r = Array_FromPyScalar(op, newtype);
    }
    else if (!PyBytes_Check(op) && !PyUnicode_Check(op) &&
             _array_from_buffer_3118(op, &r) == 0) {
        /* PEP 3118 buffer -- but don't accept Bytes objects here */
        PyObject *new;
        if (newtype != NULL || flags != 0) {
            new = PyArray_FromArray((PyArrayObject *)r, newtype, flags);
            Py_DECREF(r);
            r = new;
        }
    }
    else if (PyArray_HasArrayInterfaceType(op, newtype, context, r)) {
        PyObject *new;
        if (r == NULL) {
            Py_XDECREF(newtype);
            return NULL;
        }
        if (newtype != NULL || flags != 0) {
            new = PyArray_FromArray((PyArrayObject *)r, newtype, flags);
            Py_DECREF(r);
            r = new;
        }
    }
    else {
        int isobject = 0;

        if (flags & UPDATEIFCOPY) {
            goto err;
        }
        if (newtype == NULL) {
            newtype = _array_find_type(op, NULL, MAX_DIMS);
        }
        else if (newtype->type_num == PyArray_OBJECT) {
            isobject = 1;
        }
        if (PySequence_Check(op)) {
            PyObject *thiserr = NULL;

            /* necessary but not sufficient */
            Py_INCREF(newtype);
            r = Array_FromSequence(op, newtype, flags & FORTRAN,
                                   min_depth, max_depth);
            if (r == NULL && (thiserr=PyErr_Occurred())) {
                if (PyErr_GivenExceptionMatches(thiserr,
                                                PyExc_MemoryError)) {
                    return NULL;
                }
                /*
                 * If object was explicitly requested,
                 * then try nested list object array creation
                 */
                PyErr_Clear();
                if (isobject) {
                    Py_INCREF(newtype);
                    r = ObjectArray_FromNestedList
                        (op, newtype, flags & FORTRAN);
                    seq = TRUE;
                    Py_DECREF(newtype);
                }
            }
            else {
                seq = TRUE;
                Py_DECREF(newtype);
            }
        }
        if (!seq) {
            r = Array_FromPyScalar(op, newtype);
        }
    }

    /* If we didn't succeed return NULL */
    if (r == NULL) {
        return NULL;
    }

    /* Be sure we succeed here */
    if(!PyArray_Check(r)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "internal error: PyArray_FromAny "\
                        "not producing an array");
        Py_DECREF(r);
        return NULL;
    }

    if (min_depth != 0 && ((PyArrayObject *)r)->nd < min_depth) {
        PyErr_SetString(PyExc_ValueError,
                        "object of too small depth for desired array");
        Py_DECREF(r);
        return NULL;
    }
    if (max_depth != 0 && ((PyArrayObject *)r)->nd > max_depth) {
        PyErr_SetString(PyExc_ValueError,
                        "object too deep for desired array");
        Py_DECREF(r);
        return NULL;
    }
    return r;

 err:
    Py_XDECREF(newtype);
    PyErr_SetString(PyExc_TypeError,
                    "UPDATEIFCOPY used for non-array input.");
    return NULL;
}

/*
 * flags is any of
 * CONTIGUOUS,
 * FORTRAN,
 * ALIGNED,
 * WRITEABLE,
 * NOTSWAPPED,
 * ENSURECOPY,
 * UPDATEIFCOPY,
 * FORCECAST,
 * ENSUREARRAY,
 * ELEMENTSTRIDES
 *
 * or'd (|) together
 *
 * Any of these flags present means that the returned array should
 * guarantee that aspect of the array.  Otherwise the returned array
 * won't guarantee it -- it will depend on the object as to whether or
 * not it has such features.
 *
 * Note that ENSURECOPY is enough
 * to guarantee CONTIGUOUS, ALIGNED and WRITEABLE
 * and therefore it is redundant to include those as well.
 *
 * BEHAVED == ALIGNED | WRITEABLE
 * CARRAY = CONTIGUOUS | BEHAVED
 * FARRAY = FORTRAN | BEHAVED
 *
 * FORTRAN can be set in the FLAGS to request a FORTRAN array.
 * Fortran arrays are always behaved (aligned,
 * notswapped, and writeable) and not (C) CONTIGUOUS (if > 1d).
 *
 * UPDATEIFCOPY flag sets this flag in the returned array if a copy is
 * made and the base argument points to the (possibly) misbehaved array.
 * When the new array is deallocated, the original array held in base
 * is updated with the contents of the new array.
 *
 * FORCECAST will cause a cast to occur regardless of whether or not
 * it is safe.
 */

/*NUMPY_API
 * steals a reference to descr -- accepts NULL
 */
NPY_NO_EXPORT PyObject *
PyArray_CheckFromAny(PyObject *op, PyArray_Descr *descr, int min_depth,
                     int max_depth, int requires, PyObject *context)
{
    PyObject *obj;
    if (requires & NOTSWAPPED) {
        if (!descr && PyArray_Check(op) &&
            !PyArray_ISNBO(PyArray_DESCR(op)->byteorder)) {
            descr = PyArray_DescrNew(PyArray_DESCR(op));
        }
        else if (descr && !PyArray_ISNBO(descr->byteorder)) {
            PyArray_DESCR_REPLACE(descr);
        }
        if (descr) {
            descr->byteorder = PyArray_NATIVE;
        }
    }

    obj = PyArray_FromAny(op, descr, min_depth, max_depth, requires, context);
    if (obj == NULL) {
        return NULL;
    }
    if ((requires & ELEMENTSTRIDES) &&
        !PyArray_ElementStrides(obj)) {
        PyObject *new;
        new = PyArray_NewCopy((PyArrayObject *)obj, PyArray_ANYORDER);
        Py_DECREF(obj);
        obj = new;
    }
    return obj;
}

/*NUMPY_API
 * steals reference to newtype --- acc. NULL
 */
NPY_NO_EXPORT PyObject *
PyArray_FromArray(PyArrayObject *arr, PyArray_Descr *newtype, int flags)
{

    PyArrayObject *ret = NULL;
    int itemsize;
    int copy = 0;
    int arrflags;
    PyArray_Descr *oldtype;
    char *msg = "cannot copy back to a read-only array";
    PyTypeObject *subtype;

    oldtype = PyArray_DESCR(arr);
    subtype = Py_TYPE(arr);
    if (newtype == NULL) {
        newtype = oldtype; Py_INCREF(oldtype);
    }
    itemsize = newtype->elsize;
    if (itemsize == 0) {
        PyArray_DESCR_REPLACE(newtype);
        if (newtype == NULL) {
            return NULL;
        }
        newtype->elsize = oldtype->elsize;
        itemsize = newtype->elsize;
    }

    /*
     * Can't cast unless ndim-0 array, FORCECAST is specified
     * or the cast is safe.
     */
    if (!(flags & FORCECAST) && !PyArray_NDIM(arr) == 0 &&
        !PyArray_CanCastTo(oldtype, newtype)) {
        Py_DECREF(newtype);
        PyErr_SetString(PyExc_TypeError,
                        "array cannot be safely cast "  \
                        "to required type");
        return NULL;
    }

    /* Don't copy if sizes are compatible */
    if ((flags & ENSURECOPY) || PyArray_EquivTypes(oldtype, newtype)) {
        arrflags = arr->flags;
        copy = (flags & ENSURECOPY) ||
            ((flags & CONTIGUOUS) && (!(arrflags & CONTIGUOUS)))
            || ((flags & ALIGNED) && (!(arrflags & ALIGNED)))
            || (arr->nd > 1 &&
                ((flags & FORTRAN) && (!(arrflags & FORTRAN))))
            || ((flags & WRITEABLE) && (!(arrflags & WRITEABLE)));

        if (copy) {
            if ((flags & UPDATEIFCOPY) &&
                (!PyArray_ISWRITEABLE(arr))) {
                Py_DECREF(newtype);
                PyErr_SetString(PyExc_ValueError, msg);
                return NULL;
            }
            if ((flags & ENSUREARRAY)) {
                subtype = &PyArray_Type;
            }
            ret = (PyArrayObject *)
                PyArray_NewFromDescr(subtype, newtype,
                                     arr->nd,
                                     arr->dimensions,
                                     NULL, NULL,
                                     flags & FORTRAN,
                                     (PyObject *)arr);
            if (ret == NULL) {
                return NULL;
            }
            if (PyArray_CopyInto(ret, arr) == -1) {
                Py_DECREF(ret);
                return NULL;
            }
            if (flags & UPDATEIFCOPY)  {
                ret->flags |= UPDATEIFCOPY;
                ret->base = (PyObject *)arr;
                PyArray_FLAGS(ret->base) &= ~WRITEABLE;
                Py_INCREF(arr);
            }
        }
        /*
         * If no copy then just increase the reference
         * count and return the input
         */
        else {
            Py_DECREF(newtype);
            if ((flags & ENSUREARRAY) &&
                !PyArray_CheckExact(arr)) {
                Py_INCREF(arr->descr);
                ret = (PyArrayObject *)
                    PyArray_NewFromDescr(&PyArray_Type,
                                         arr->descr,
                                         arr->nd,
                                         arr->dimensions,
                                         arr->strides,
                                         arr->data,
                                         arr->flags,NULL);
                if (ret == NULL) {
                    return NULL;
                }
                ret->base = (PyObject *)arr;
            }
            else {
                ret = arr;
            }
            Py_INCREF(arr);
        }
    }

    /*
     * The desired output type is different than the input
     * array type and copy was not specified
     */
    else {
        if ((flags & UPDATEIFCOPY) &&
            (!PyArray_ISWRITEABLE(arr))) {
            Py_DECREF(newtype);
            PyErr_SetString(PyExc_ValueError, msg);
            return NULL;
        }
        if ((flags & ENSUREARRAY)) {
            subtype = &PyArray_Type;
        }
        ret = (PyArrayObject *)
            PyArray_NewFromDescr(subtype, newtype,
                                 arr->nd, arr->dimensions,
                                 NULL, NULL,
                                 flags & FORTRAN,
                                 (PyObject *)arr);
        if (ret == NULL) {
            return NULL;
        }
        if (PyArray_CastTo(ret, arr) < 0) {
            Py_DECREF(ret);
            return NULL;
        }
        if (flags & UPDATEIFCOPY)  {
            ret->flags |= UPDATEIFCOPY;
            ret->base = (PyObject *)arr;
            PyArray_FLAGS(ret->base) &= ~WRITEABLE;
            Py_INCREF(arr);
        }
    }
    return (PyObject *)ret;
}

/*NUMPY_API */
NPY_NO_EXPORT PyObject *
PyArray_FromStructInterface(PyObject *input)
{
    PyArray_Descr *thetype = NULL;
    char buf[40];
    PyArrayInterface *inter;
    PyObject *attr, *r;
    char endian = PyArray_NATBYTE;

    attr = PyObject_GetAttrString(input, "__array_struct__");
    if (attr == NULL) {
        PyErr_Clear();
        return Py_NotImplemented;
    }
    if (!NpyCapsule_Check(attr)) {
        goto fail;
    }
    inter = NpyCapsule_AsVoidPtr(attr);
    if (inter->two != 2) {
        goto fail;
    }
    if ((inter->flags & NOTSWAPPED) != NOTSWAPPED) {
        endian = PyArray_OPPBYTE;
        inter->flags &= ~NOTSWAPPED;
    }

    if (inter->flags & ARR_HAS_DESCR) {
        if (PyArray_DescrConverter(inter->descr, &thetype) == PY_FAIL) {
            thetype = NULL;
            PyErr_Clear();
        }
    }

    if (thetype == NULL) {
        PyOS_snprintf(buf, sizeof(buf),
                "%c%c%d", endian, inter->typekind, inter->itemsize);
        if (!(thetype=_array_typedescr_fromstr(buf))) {
            Py_DECREF(attr);
            return NULL;
        }
    }

    r = PyArray_NewFromDescr(&PyArray_Type, thetype,
                             inter->nd, inter->shape,
                             inter->strides, inter->data,
                             inter->flags, NULL);
    Py_INCREF(input);
    PyArray_BASE(r) = input;
    Py_DECREF(attr);
    PyArray_UpdateFlags((PyArrayObject *)r, UPDATE_ALL);
    return r;

 fail:
    PyErr_SetString(PyExc_ValueError, "invalid __array_struct__");
    Py_DECREF(attr);
    return NULL;
}

#define PyIntOrLong_Check(obj) (PyInt_Check(obj) || PyLong_Check(obj))

/*NUMPY_API*/
NPY_NO_EXPORT PyObject *
PyArray_FromInterface(PyObject *input)
{
    PyObject *attr = NULL, *item = NULL;
    PyObject *tstr = NULL, *shape = NULL;
    PyObject *inter = NULL;
    PyObject *base = NULL;
    PyArrayObject *ret;
    PyArray_Descr *type=NULL;
    char *data;
    Py_ssize_t buffer_len;
    int res, i, n;
    intp dims[MAX_DIMS], strides[MAX_DIMS];
    int dataflags = BEHAVED;

    /* Get the memory from __array_data__ and __array_offset__ */
    /* Get the shape */
    /* Get the typestring -- ignore array_descr */
    /* Get the strides */

    inter = PyObject_GetAttrString(input, "__array_interface__");
    if (inter == NULL) {
        PyErr_Clear();
        return Py_NotImplemented;
    }
    if (!PyDict_Check(inter)) {
        Py_DECREF(inter);
        return Py_NotImplemented;
    }
    shape = PyDict_GetItemString(inter, "shape");
    if (shape == NULL) {
        Py_DECREF(inter);
        return Py_NotImplemented;
    }
    tstr = PyDict_GetItemString(inter, "typestr");
    if (tstr == NULL) {
        Py_DECREF(inter);
        return Py_NotImplemented;
    }

    attr = PyDict_GetItemString(inter, "data");
    base = input;
    if ((attr == NULL) || (attr==Py_None) || (!PyTuple_Check(attr))) {
        if (attr && (attr != Py_None)) {
            item = attr;
        }
        else {
            item = input;
        }
        res = PyObject_AsWriteBuffer(item, (void **)&data, &buffer_len);
        if (res < 0) {
            PyErr_Clear();
            res = PyObject_AsReadBuffer(
                    item, (const void **)&data, &buffer_len);
            if (res < 0) {
                goto fail;
            }
            dataflags &= ~WRITEABLE;
        }
        attr = PyDict_GetItemString(inter, "offset");
        if (attr) {
            longlong num = PyLong_AsLongLong(attr);
            if (error_converting(num)) {
                PyErr_SetString(PyExc_TypeError,
                                "offset "\
                                "must be an integer");
                goto fail;
            }
            data += num;
        }
        base = item;
    }
    else {
        PyObject *dataptr;
        if (PyTuple_GET_SIZE(attr) != 2) {
            PyErr_SetString(PyExc_TypeError,
                            "data must return "     \
                            "a 2-tuple with (data pointer "\
                            "integer, read-only flag)");
            goto fail;
        }
        dataptr = PyTuple_GET_ITEM(attr, 0);
        if (PyString_Check(dataptr)) {
            res = sscanf(PyString_AsString(dataptr),
                         "%p", (void **)&data);
            if (res < 1) {
                PyErr_SetString(PyExc_TypeError,
                                "data string cannot be " \
                                "converted");
                goto fail;
            }
        }
        else if (PyIntOrLong_Check(dataptr)) {
            data = PyLong_AsVoidPtr(dataptr);
        }
        else {
            PyErr_SetString(PyExc_TypeError, "first element " \
                            "of data tuple must be integer" \
                            " or string.");
            goto fail;
        }
        if (PyObject_IsTrue(PyTuple_GET_ITEM(attr,1))) {
            dataflags &= ~WRITEABLE;
        }
    }
    attr = tstr;
#if defined(NPY_PY3K)
    if (PyUnicode_Check(tstr)) {
        /* Allow unicode type strings */
        attr = PyUnicode_AsASCIIString(tstr);
    }
#endif
    if (!PyBytes_Check(attr)) {
        PyErr_SetString(PyExc_TypeError, "typestr must be a string");
        goto fail;
    }
    type = _array_typedescr_fromstr(PyString_AS_STRING(attr));
#if defined(NPY_PY3K)
    if (attr != tstr) {
        Py_DECREF(attr);
    }
#endif
    if (type == NULL) {
        goto fail;
    }
    attr = shape;
    if (!PyTuple_Check(attr)) {
        PyErr_SetString(PyExc_TypeError, "shape must be a tuple");
        Py_DECREF(type);
        goto fail;
    }
    n = PyTuple_GET_SIZE(attr);
    for (i = 0; i < n; i++) {
        item = PyTuple_GET_ITEM(attr, i);
        dims[i] = PyArray_PyIntAsIntp(item);
        if (error_converting(dims[i])) {
            break;
        }
    }

    ret = (PyArrayObject *)PyArray_NewFromDescr(&PyArray_Type, type,
                                                n, dims,
                                                NULL, data,
                                                dataflags, NULL);
    if (ret == NULL) {
        return NULL;
    }
    Py_INCREF(base);
    ret->base = base;

    attr = PyDict_GetItemString(inter, "strides");
    if (attr != NULL && attr != Py_None) {
        if (!PyTuple_Check(attr)) {
            PyErr_SetString(PyExc_TypeError,
                            "strides must be a tuple");
            Py_DECREF(ret);
            return NULL;
        }
        if (n != PyTuple_GET_SIZE(attr)) {
            PyErr_SetString(PyExc_ValueError,
                            "mismatch in length of "\
                            "strides and shape");
            Py_DECREF(ret);
            return NULL;
        }
        for (i = 0; i < n; i++) {
            item = PyTuple_GET_ITEM(attr, i);
            strides[i] = PyArray_PyIntAsIntp(item);
            if (error_converting(strides[i])) {
                break;
            }
        }
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        memcpy(ret->strides, strides, n*sizeof(npy_intp));
    }
    else PyErr_Clear();
    PyArray_UpdateFlags(ret, UPDATE_ALL);
    Py_DECREF(inter);
    return (PyObject *)ret;

 fail:
    Py_XDECREF(inter);
    return NULL;
}

/*NUMPY_API*/
NPY_NO_EXPORT PyObject *
PyArray_FromArrayAttr(PyObject *op, PyArray_Descr *typecode, PyObject *context)
{
    PyObject *new;
    PyObject *array_meth;

    array_meth = PyObject_GetAttrString(op, "__array__");
    if (array_meth == NULL) {
        PyErr_Clear();
        return Py_NotImplemented;
    }
    if (context == NULL) {
        if (typecode == NULL) {
            new = PyObject_CallFunction(array_meth, NULL);
        }
        else {
            new = PyObject_CallFunction(array_meth, "O", typecode);
        }
    }
    else {
        if (typecode == NULL) {
            new = PyObject_CallFunction(array_meth, "OO", Py_None, context);
            if (new == NULL && PyErr_ExceptionMatches(PyExc_TypeError)) {
                PyErr_Clear();
                new = PyObject_CallFunction(array_meth, "");
            }
        }
        else {
            new = PyObject_CallFunction(array_meth, "OO", typecode, context);
            if (new == NULL && PyErr_ExceptionMatches(PyExc_TypeError)) {
                PyErr_Clear();
                new = PyObject_CallFunction(array_meth, "O", typecode);
            }
        }
    }
    Py_DECREF(array_meth);
    if (new == NULL) {
        return NULL;
    }
    if (!PyArray_Check(new)) {
        PyErr_SetString(PyExc_ValueError,
                        "object __array__ method not "  \
                        "producing an array");
        Py_DECREF(new);
        return NULL;
    }
    return new;
}

/*NUMPY_API
* new reference -- accepts NULL for mintype
*/
NPY_NO_EXPORT PyArray_Descr *
PyArray_DescrFromObject(PyObject *op, PyArray_Descr *mintype)
{
    return _array_find_type(op, mintype, MAX_DIMS);
}

/* These are also old calls (should use PyArray_NewFromDescr) */

/* They all zero-out the memory as previously done */

/* steals reference to descr -- and enforces native byteorder on it.*/
/*NUMPY_API
  Like FromDimsAndData but uses the Descr structure instead of typecode
  as input.
*/
NPY_NO_EXPORT PyObject *
PyArray_FromDimsAndDataAndDescr(int nd, int *d,
                                PyArray_Descr *descr,
                                char *data)
{
    PyObject *ret;
    int i;
    npy_intp newd[MAX_DIMS];
    char msg[] = "PyArray_FromDimsAndDataAndDescr: use PyArray_NewFromDescr.";

    if (DEPRECATE(msg) < 0) {
        return NULL;
    }
    if (!PyArray_ISNBO(descr->byteorder))
        descr->byteorder = '=';
    for (i = 0; i < nd; i++) {
        newd[i] = (npy_intp) d[i];
    }
    ret = PyArray_NewFromDescr(&PyArray_Type, descr,
                               nd, newd,
                               NULL, data,
                               (data ? CARRAY : 0), NULL);
    return ret;
}

/*NUMPY_API
  Construct an empty array from dimensions and typenum
*/
NPY_NO_EXPORT PyObject *
PyArray_FromDims(int nd, int *d, int type)
{
    PyObject *ret;
    char msg[] = "PyArray_FromDims: use PyArray_SimpleNew.";

    if (DEPRECATE(msg) < 0) {
        return NULL;
    }
    ret = PyArray_FromDimsAndDataAndDescr(nd, d,
                                          PyArray_DescrFromType(type),
                                          NULL);
    /*
     * Old FromDims set memory to zero --- some algorithms
     * relied on that.  Better keep it the same. If
     * Object type, then it's already been set to zero, though.
     */
    if (ret && (PyArray_DESCR(ret)->type_num != PyArray_OBJECT)) {
        memset(PyArray_DATA(ret), 0, PyArray_NBYTES(ret));
    }
    return ret;
}

/* end old calls */

/*NUMPY_API
 * This is a quick wrapper around PyArray_FromAny(op, NULL, 0, 0, ENSUREARRAY)
 * that special cases Arrays and PyArray_Scalars up front
 * It *steals a reference* to the object
 * It also guarantees that the result is PyArray_Type
 * Because it decrefs op if any conversion needs to take place
 * so it can be used like PyArray_EnsureArray(some_function(...))
 */
NPY_NO_EXPORT PyObject *
PyArray_EnsureArray(PyObject *op)
{
    PyObject *new;

    if ((op == NULL) || (PyArray_CheckExact(op))) {
        new = op;
        Py_XINCREF(new);
    }
    else if (PyArray_Check(op)) {
        new = PyArray_View((PyArrayObject *)op, NULL, &PyArray_Type);
    }
    else if (PyArray_IsScalar(op, Generic)) {
        new = PyArray_FromScalar(op, NULL);
    }
    else {
        new = PyArray_FromAny(op, NULL, 0, 0, ENSUREARRAY, NULL);
    }
    Py_XDECREF(op);
    return new;
}

/*NUMPY_API*/
NPY_NO_EXPORT PyObject *
PyArray_EnsureAnyArray(PyObject *op)
{
    if (op && PyArray_Check(op)) {
        return op;
    }
    return PyArray_EnsureArray(op);
}

/*NUMPY_API
 * Copy an Array into another array -- memory must not overlap
 * Does not require src and dest to have "broadcastable" shapes
 * (only the same number of elements).
 *
 * TODO: For NumPy 2.0, this could accept an order parameter which
 *       only allows NPY_CORDER and NPY_FORDER.  Could also rename
 *       this to CopyAsFlat to make the name more intuitive.
 *
 * Returns 0 on success, -1 on error.
 */
NPY_NO_EXPORT int
PyArray_CopyAnyInto(PyArrayObject *dst, PyArrayObject *src)
{
    PyArray_StridedTransferFn *stransfer = NULL;
    void *transferdata = NULL;
    NpyIter *dst_iter, *src_iter;
    NPY_BEGIN_THREADS_DEF;

    NpyIter_IterNext_Fn dst_iternext, src_iternext;
    char **dst_dataptr, **src_dataptr;
    npy_intp dst_stride, src_stride;
    npy_intp *dst_countptr, *src_countptr;

    char *dst_data, *src_data;
    npy_intp dst_count, src_count, count;
    npy_intp src_itemsize;
    npy_intp dst_size, src_size;
    int needs_api;

    if (!PyArray_ISWRITEABLE(dst)) {
        PyErr_SetString(PyExc_RuntimeError,
                "cannot write to array");
        return -1;
    }

    /* If the shapes match, use the more efficient CopyInto */
    if (PyArray_NDIM(dst) == PyArray_NDIM(src) &&
            PyArray_CompareLists(PyArray_DIMS(dst), PyArray_DIMS(src),
                                PyArray_NDIM(dst))) {
        return PyArray_CopyInto(dst, src);
    }

    dst_size = PyArray_SIZE(dst);
    src_size = PyArray_SIZE(src);
    if (dst_size != src_size) {
        PyErr_SetString(PyExc_ValueError,
                "arrays must have the same number of elements"
                " for copy");
        return -1;
    }

    /* Zero-sized arrays require nothing be done */
    if (dst_size == 0) {
        return 0;
    }


    /*
     * This copy is based on matching C-order traversals of src and dst.
     * By using two iterators, we can find maximal sub-chunks that
     * can be processed at once.
     */
    dst_iter = NpyIter_New(dst, NPY_ITER_WRITEONLY|
                                NPY_ITER_NO_INNER_ITERATION|
                                NPY_ITER_REFS_OK,
                                NPY_CORDER,
                                NPY_NO_CASTING,
                                NULL, 0, NULL, 0);
    if (dst_iter == NULL) {
        return -1;
    }
    src_iter = NpyIter_New(src, NPY_ITER_READONLY|
                                NPY_ITER_NO_INNER_ITERATION|
                                NPY_ITER_REFS_OK,
                                NPY_CORDER,
                                NPY_NO_CASTING,
                                NULL, 0, NULL, 0);
    if (src_iter == NULL) {
        NpyIter_Deallocate(dst_iter);
        return -1;
    }

    /* Get all the values needed for the inner loop */
    dst_iternext = NpyIter_GetIterNext(dst_iter, NULL);
    dst_dataptr = NpyIter_GetDataPtrArray(dst_iter);
    /* Since buffering is disabled, we can cache the stride */
    dst_stride = *NpyIter_GetInnerStrideArray(dst_iter);
    dst_countptr = NpyIter_GetInnerLoopSizePtr(dst_iter);

    src_iternext = NpyIter_GetIterNext(src_iter, NULL);
    src_dataptr = NpyIter_GetDataPtrArray(src_iter);
    /* Since buffering is disabled, we can cache the stride */
    src_stride = *NpyIter_GetInnerStrideArray(src_iter);
    src_countptr = NpyIter_GetInnerLoopSizePtr(src_iter);

    if (dst_iternext == NULL || src_iternext == NULL) {
        NpyIter_Deallocate(dst_iter);
        NpyIter_Deallocate(src_iter);
        return -1;
    }

    src_itemsize = PyArray_DESCR(src)->elsize;

    needs_api = NpyIter_IterationNeedsAPI(dst_iter) ||
                NpyIter_IterationNeedsAPI(src_iter);

    /*
     * Because buffering is disabled in the iterator, the inner loop
     * strides will be the same throughout the iteration loop.  Thus,
     * we can pass them to this function to take advantage of
     * contiguous strides, etc.
     */
    if (PyArray_GetDTypeTransferFunction(
                    PyArray_ISALIGNED(src) && PyArray_ISALIGNED(dst),
                    src_stride, dst_stride,
                    PyArray_DESCR(src), PyArray_DESCR(dst),
                    0,
                    &stransfer, &transferdata,
                    &needs_api) != NPY_SUCCEED) {
        NpyIter_Deallocate(dst_iter);
        NpyIter_Deallocate(src_iter);
        return -1;
    }


    if (!needs_api) {
        NPY_BEGIN_THREADS;
    }

    dst_count = *dst_countptr;
    src_count = *src_countptr;
    dst_data = *dst_dataptr;
    src_data = *src_dataptr;
    /*
     * The tests did not trigger this code, so added a new function
     * ndarray.setasflat to the Python exposure in order to test it.
     */
    for(;;) {
        /* Transfer the biggest amount that fits both */
        count = (src_count < dst_count) ? src_count : dst_count;
        stransfer(dst_data, dst_stride,
                    src_data, src_stride,
                    count, src_itemsize, transferdata);

        /* If we exhausted the dst block, refresh it */
        if (dst_count == count) {
            if (!dst_iternext(dst_iter)) {
                break;
            }
            dst_count = *dst_countptr;
            dst_data = *dst_dataptr;
        }
        else {
            dst_count -= count;
            dst_data += count*dst_stride;
        }

        /* If we exhausted the src block, refresh it */
        if (src_count == count) {
            if (!src_iternext(src_iter)) {
                break;
            }
            src_count = *src_countptr;
            src_data = *src_dataptr;
        }
        else {
            src_count -= count;
            src_data += count*src_stride;
        }
    }

    if (!needs_api) {
        NPY_END_THREADS;
    }

    PyArray_FreeStridedTransferData(transferdata);
    NpyIter_Deallocate(dst_iter);
    NpyIter_Deallocate(src_iter);

    return PyErr_Occurred() ? -1 : 0;
}

/*NUMPY_API
 * Copy an Array into another array -- memory must not overlap.
 * Broadcast to the destination shape if necessary.
 *
 * Returns 0 on success, -1 on failure.
 */
NPY_NO_EXPORT int
PyArray_CopyInto(PyArrayObject *dst, PyArrayObject *src)
{
    PyArray_StridedTransferFn *stransfer = NULL;
    void *transferdata = NULL;
    NPY_BEGIN_THREADS_DEF;

    if (!PyArray_ISWRITEABLE(dst)) {
        PyErr_SetString(PyExc_RuntimeError,
                "cannot write to array");
        return -1;
    }

    if (PyArray_SIZE(src) == 0) {
        if (PyArray_SIZE(dst) == 0) {
            return 0;
        }
        else {
            PyErr_SetString(PyExc_ValueError,
                    "cannot copy from zero-sized array");
            return -1;
        }
    }
    if (PyArray_SIZE(dst) == 0) {
        /* Allow a scalar to be assigned to anything, even an empty array */
        if (PyArray_NDIM(src) == 0) {
            return 0;
        }
        else {
            PyErr_SetString(PyExc_ValueError,
                    "cannot copy to zero-sized array");
            return -1;
        }
    }

    if (PyArray_TRIVIALLY_ITERABLE_PAIR(dst, src)) {
        char *dst_data, *src_data;
        npy_intp count, dst_stride, src_stride, src_itemsize;

        int needs_api = 0;

        PyArray_PREPARE_TRIVIAL_PAIR_ITERATION(dst, src, count,
                              dst_data, src_data, dst_stride, src_stride);

        /*
         * Check for overlap with positive strides, and if found,
         * possibly reverse the order
         */
        if (dst_data > src_data && src_stride > 0 && dst_stride > 0 &&
                        (dst_data < src_data+src_stride*count) &&
                        (src_data < dst_data+dst_stride*count)) {
            dst_data += dst_stride*(count-1);
            src_data += src_stride*(count-1);
            dst_stride = -dst_stride;
            src_stride = -src_stride;
        }

        if (PyArray_GetDTypeTransferFunction(
                        PyArray_ISALIGNED(src) && PyArray_ISALIGNED(dst),
                        src_stride, dst_stride,
                        PyArray_DESCR(src), PyArray_DESCR(dst),
                        0,
                        &stransfer, &transferdata,
                        &needs_api) != NPY_SUCCEED) {
            return -1;
        }

        src_itemsize = PyArray_DESCR(src)->elsize;

        if (!needs_api) {
            NPY_BEGIN_THREADS;
        }

        stransfer(dst_data, dst_stride, src_data, src_stride,
                    count, src_itemsize, transferdata);

        if (!needs_api) {
            NPY_END_THREADS;
        }

        PyArray_FreeStridedTransferData(transferdata);

        return PyErr_Occurred() ? -1 : 0;
    }
    else {
        PyArrayObject *op[2];
        npy_uint32 op_flags[2];
        NpyIter *iter;

        NpyIter_IterNext_Fn iternext;
        char **dataptr;
        npy_intp *stride;
        npy_intp *countptr;
        npy_intp src_itemsize;
        int needs_api;

        op[0] = dst;
        op[1] = src;
        op_flags[0] = NPY_ITER_WRITEONLY|NPY_ITER_NO_BROADCAST;
        op_flags[1] = NPY_ITER_READONLY;

        iter = NpyIter_MultiNew(2, op,
                            NPY_ITER_NO_INNER_ITERATION|
                            NPY_ITER_REFS_OK,
                            NPY_KEEPORDER,
                            NPY_NO_CASTING,
                            op_flags,
                            NULL, 0, NULL, 0);
        if (iter == NULL) {
            return -1;
        }

        iternext = NpyIter_GetIterNext(iter, NULL);
        if (iternext == NULL) {
            NpyIter_Deallocate(iter);
            return -1;
        }
        dataptr = NpyIter_GetDataPtrArray(iter);
        stride = NpyIter_GetInnerStrideArray(iter);
        countptr = NpyIter_GetInnerLoopSizePtr(iter);
        src_itemsize = PyArray_DESCR(src)->elsize;

        needs_api = NpyIter_IterationNeedsAPI(iter);

        /*
         * Because buffering is disabled in the iterator, the inner loop
         * strides will be the same throughout the iteration loop.  Thus,
         * we can pass them to this function to take advantage of
         * contiguous strides, etc.
         */
        if (PyArray_GetDTypeTransferFunction(
                        PyArray_ISALIGNED(src) && PyArray_ISALIGNED(dst),
                        stride[1], stride[0],
                        PyArray_DESCR(src), PyArray_DESCR(dst),
                        0,
                        &stransfer, &transferdata,
                        &needs_api) != NPY_SUCCEED) {
            NpyIter_Deallocate(iter);
            return -1;
        }


        if (needs_api) {
            do {
                stransfer(dataptr[0], stride[0],
                            dataptr[1], stride[1],
                            *countptr, src_itemsize, transferdata);
            } while(iternext(iter));
        }
        else {
            NPY_BEGIN_THREADS;
            do {
                stransfer(dataptr[0], stride[0],
                            dataptr[1], stride[1],
                            *countptr, src_itemsize, transferdata);
            } while(iternext(iter));
            NPY_END_THREADS;
        }

        PyArray_FreeStridedTransferData(transferdata);
        NpyIter_Deallocate(iter);

        return PyErr_Occurred() ? -1 : 0;
    }
}


/*NUMPY_API
  PyArray_CheckAxis

  check that axis is valid
  convert 0-d arrays to 1-d arrays
*/
NPY_NO_EXPORT PyObject *
PyArray_CheckAxis(PyArrayObject *arr, int *axis, int flags)
{
    PyObject *temp1, *temp2;
    int n = arr->nd;

    if (*axis == MAX_DIMS || n == 0) {
        if (n != 1) {
            temp1 = PyArray_Ravel(arr,0);
            if (temp1 == NULL) {
                *axis = 0;
                return NULL;
            }
            if (*axis == MAX_DIMS) {
                *axis = PyArray_NDIM(temp1)-1;
            }
        }
        else {
            temp1 = (PyObject *)arr;
            Py_INCREF(temp1);
            *axis = 0;
        }
        if (!flags && *axis == 0) {
            return temp1;
        }
    }
    else {
        temp1 = (PyObject *)arr;
        Py_INCREF(temp1);
    }
    if (flags) {
        temp2 = PyArray_CheckFromAny((PyObject *)temp1, NULL,
                                     0, 0, flags, NULL);
        Py_DECREF(temp1);
        if (temp2 == NULL) {
            return NULL;
        }
    }
    else {
        temp2 = (PyObject *)temp1;
    }
    n = PyArray_NDIM(temp2);
    if (*axis < 0) {
        *axis += n;
    }
    if ((*axis < 0) || (*axis >= n)) {
        PyErr_Format(PyExc_ValueError,
                     "axis(=%d) out of bounds", *axis);
        Py_DECREF(temp2);
        return NULL;
    }
    return temp2;
}

/*NUMPY_API
 * Zeros
 *
 * steal a reference
 * accepts NULL type
 */
NPY_NO_EXPORT PyObject *
PyArray_Zeros(int nd, npy_intp *dims, PyArray_Descr *type, int fortran)
{
    PyArrayObject *ret;

    if (!type) {
        type = PyArray_DescrFromType(PyArray_DEFAULT);
    }
    ret = (PyArrayObject *)PyArray_NewFromDescr(&PyArray_Type,
                                                type,
                                                nd, dims,
                                                NULL, NULL,
                                                fortran, NULL);
    if (ret == NULL) {
        return NULL;
    }
    if (_zerofill(ret) < 0) {
        return NULL;
    }
    return (PyObject *)ret;

}

/*NUMPY_API
 * Empty
 *
 * accepts NULL type
 * steals referenct to type
 */
NPY_NO_EXPORT PyObject *
PyArray_Empty(int nd, npy_intp *dims, PyArray_Descr *type, int fortran)
{
    PyArrayObject *ret;

    if (!type) type = PyArray_DescrFromType(PyArray_DEFAULT);
    ret = (PyArrayObject *)PyArray_NewFromDescr(&PyArray_Type,
                                                type, nd, dims,
                                                NULL, NULL,
                                                fortran, NULL);
    if (ret == NULL) {
        return NULL;
    }
    if (PyDataType_REFCHK(type)) {
        PyArray_FillObjectArray(ret, Py_None);
        if (PyErr_Occurred()) {
            Py_DECREF(ret);
            return NULL;
        }
    }
    return (PyObject *)ret;
}

/*
 * Like ceil(value), but check for overflow.
 *
 * Return 0 on success, -1 on failure. In case of failure, set a PyExc_Overflow
 * exception
 */
static int _safe_ceil_to_intp(double value, npy_intp* ret)
{
    double ivalue;

    ivalue = npy_ceil(value);
    if (ivalue < NPY_MIN_INTP || ivalue > NPY_MAX_INTP) {
        return -1;
    }

    *ret = (npy_intp)ivalue;
    return 0;
}


/*NUMPY_API
  Arange,
*/
NPY_NO_EXPORT PyObject *
PyArray_Arange(double start, double stop, double step, int type_num)
{
    npy_intp length;
    PyObject *range;
    PyArray_ArrFuncs *funcs;
    PyObject *obj;
    int ret;

    if (_safe_ceil_to_intp((stop - start)/step, &length)) {
        PyErr_SetString(PyExc_OverflowError,
                "arange: overflow while computing length");
    }

    if (length <= 0) {
        length = 0;
        return PyArray_New(&PyArray_Type, 1, &length, type_num,
                           NULL, NULL, 0, 0, NULL);
    }
    range = PyArray_New(&PyArray_Type, 1, &length, type_num,
                        NULL, NULL, 0, 0, NULL);
    if (range == NULL) {
        return NULL;
    }
    funcs = PyArray_DESCR(range)->f;

    /*
     * place start in the buffer and the next value in the second position
     * if length > 2, then call the inner loop, otherwise stop
     */
    obj = PyFloat_FromDouble(start);
    ret = funcs->setitem(obj, PyArray_DATA(range), (PyArrayObject *)range);
    Py_DECREF(obj);
    if (ret < 0) {
        goto fail;
    }
    if (length == 1) {
        return range;
    }
    obj = PyFloat_FromDouble(start + step);
    ret = funcs->setitem(obj, PyArray_BYTES(range)+PyArray_ITEMSIZE(range),
                         (PyArrayObject *)range);
    Py_DECREF(obj);
    if (ret < 0) {
        goto fail;
    }
    if (length == 2) {
        return range;
    }
    if (!funcs->fill) {
        PyErr_SetString(PyExc_ValueError, "no fill-function for data-type.");
        Py_DECREF(range);
        return NULL;
    }
    funcs->fill(PyArray_DATA(range), length, (PyArrayObject *)range);
    if (PyErr_Occurred()) {
        goto fail;
    }
    return range;

 fail:
    Py_DECREF(range);
    return NULL;
}

/*
 * the formula is len = (intp) ceil((start - stop) / step);
 */
static npy_intp
_calc_length(PyObject *start, PyObject *stop, PyObject *step, PyObject **next, int cmplx)
{
    npy_intp len, tmp;
    PyObject *val;
    double value;

    *next = PyNumber_Subtract(stop, start);
    if (!(*next)) {
        if (PyTuple_Check(stop)) {
            PyErr_Clear();
            PyErr_SetString(PyExc_TypeError,
                            "arange: scalar arguments expected "\
                            "instead of a tuple.");
        }
        return -1;
    }
    val = PyNumber_TrueDivide(*next, step);
    Py_DECREF(*next);
    *next = NULL;
    if (!val) {
        return -1;
    }
    if (cmplx && PyComplex_Check(val)) {
        value = PyComplex_RealAsDouble(val);
        if (error_converting(value)) {
            Py_DECREF(val);
            return -1;
        }
        if (_safe_ceil_to_intp(value, &len)) {
            Py_DECREF(val);
            PyErr_SetString(PyExc_OverflowError,
                    "arange: overflow while computing length");
            return -1;
        }
        value = PyComplex_ImagAsDouble(val);
        Py_DECREF(val);
        if (error_converting(value)) {
            return -1;
        }
        if (_safe_ceil_to_intp(value, &tmp)) {
            PyErr_SetString(PyExc_OverflowError,
                    "arange: overflow while computing length");
            return -1;
        }
        len = MIN(len, tmp);
    }
    else {
        value = PyFloat_AsDouble(val);
        Py_DECREF(val);
        if (error_converting(value)) {
            return -1;
        }
        if (_safe_ceil_to_intp(value, &len)) {
            PyErr_SetString(PyExc_OverflowError,
                    "arange: overflow while computing length");
            return -1;
        }
    }
    if (len > 0) {
        *next = PyNumber_Add(start, step);
        if (!next) {
            return -1;
        }
    }
    return len;
}

/*NUMPY_API
 *
 * ArangeObj,
 *
 * this doesn't change the references
 */
NPY_NO_EXPORT PyObject *
PyArray_ArangeObj(PyObject *start, PyObject *stop, PyObject *step, PyArray_Descr *dtype)
{
    PyObject *range;
    PyArray_ArrFuncs *funcs;
    PyObject *next, *err;
    npy_intp length;
    PyArray_Descr *native = NULL;
    int swap;

    if (!dtype) {
        PyArray_Descr *deftype;
        PyArray_Descr *newtype;
        /* intentionally made to be PyArray_LONG default */
        deftype = PyArray_DescrFromType(PyArray_LONG);
        newtype = PyArray_DescrFromObject(start, deftype);
        Py_DECREF(deftype);
        deftype = newtype;
        if (stop && stop != Py_None) {
            newtype = PyArray_DescrFromObject(stop, deftype);
            Py_DECREF(deftype);
            deftype = newtype;
        }
        if (step && step != Py_None) {
            newtype = PyArray_DescrFromObject(step, deftype);
            Py_DECREF(deftype);
            deftype = newtype;
        }
        dtype = deftype;
    }
    else {
        Py_INCREF(dtype);
    }
    if (!step || step == Py_None) {
        step = PyInt_FromLong(1);
    }
    else {
        Py_XINCREF(step);
    }
    if (!stop || stop == Py_None) {
        stop = start;
        start = PyInt_FromLong(0);
    }
    else {
        Py_INCREF(start);
    }
    /* calculate the length and next = start + step*/
    length = _calc_length(start, stop, step, &next,
                          PyTypeNum_ISCOMPLEX(dtype->type_num));
    err = PyErr_Occurred();
    if (err) {
        Py_DECREF(dtype);
        if (err && PyErr_GivenExceptionMatches(err, PyExc_OverflowError)) {
            PyErr_SetString(PyExc_ValueError, "Maximum allowed size exceeded");
        }
        goto fail;
    }
    if (length <= 0) {
        length = 0;
        range = PyArray_SimpleNewFromDescr(1, &length, dtype);
        Py_DECREF(step);
        Py_DECREF(start);
        return range;
    }

    /*
     * If dtype is not in native byte-order then get native-byte
     * order version.  And then swap on the way out.
     */
    if (!PyArray_ISNBO(dtype->byteorder)) {
        native = PyArray_DescrNewByteorder(dtype, PyArray_NATBYTE);
        swap = 1;
    }
    else {
        native = dtype;
        swap = 0;
    }

    range = PyArray_SimpleNewFromDescr(1, &length, native);
    if (range == NULL) {
        goto fail;
    }

    /*
     * place start in the buffer and the next value in the second position
     * if length > 2, then call the inner loop, otherwise stop
     */
    funcs = PyArray_DESCR(range)->f;
    if (funcs->setitem(
                start, PyArray_DATA(range), (PyArrayObject *)range) < 0) {
        goto fail;
    }
    if (length == 1) {
        goto finish;
    }
    if (funcs->setitem(next, PyArray_BYTES(range)+PyArray_ITEMSIZE(range),
                       (PyArrayObject *)range) < 0) {
        goto fail;
    }
    if (length == 2) {
        goto finish;
    }
    if (!funcs->fill) {
        PyErr_SetString(PyExc_ValueError, "no fill-function for data-type.");
        Py_DECREF(range);
        goto fail;
    }
    funcs->fill(PyArray_DATA(range), length, (PyArrayObject *)range);
    if (PyErr_Occurred()) {
        goto fail;
    }
 finish:
    if (swap) {
        PyObject *new;
        new = PyArray_Byteswap((PyArrayObject *)range, 1);
        Py_DECREF(new);
        Py_DECREF(PyArray_DESCR(range));
        PyArray_DESCR(range) = dtype;  /* steals the reference */
    }
    Py_DECREF(start);
    Py_DECREF(step);
    Py_DECREF(next);
    return range;

 fail:
    Py_DECREF(start);
    Py_DECREF(step);
    Py_XDECREF(next);
    return NULL;
}

static PyArrayObject *
array_fromfile_binary(FILE *fp, PyArray_Descr *dtype, npy_intp num, size_t *nread)
{
    PyArrayObject *r;
    npy_intp start, numbytes;

    if (num < 0) {
        int fail = 0;

        start = (npy_intp)ftell(fp);
        if (start < 0) {
            fail = 1;
        }
        if (fseek(fp, 0, SEEK_END) < 0) {
            fail = 1;
        }
        numbytes = (npy_intp) ftell(fp);
        if (numbytes < 0) {
            fail = 1;
        }
        numbytes -= start;
        if (fseek(fp, start, SEEK_SET) < 0) {
            fail = 1;
        }
        if (fail) {
            PyErr_SetString(PyExc_IOError,
                            "could not seek in file");
            Py_DECREF(dtype);
            return NULL;
        }
        num = numbytes / dtype->elsize;
    }
    r = (PyArrayObject *)PyArray_NewFromDescr(&PyArray_Type,
                                              dtype,
                                              1, &num,
                                              NULL, NULL,
                                              0, NULL);
    if (r == NULL) {
        return NULL;
    }
    NPY_BEGIN_ALLOW_THREADS;
    *nread = fread(r->data, dtype->elsize, num, fp);
    NPY_END_ALLOW_THREADS;
    return r;
}

/*
 * Create an array by reading from the given stream, using the passed
 * next_element and skip_separator functions.
 */
#define FROM_BUFFER_SIZE 4096
static PyArrayObject *
array_from_text(PyArray_Descr *dtype, npy_intp num, char *sep, size_t *nread,
                void *stream, next_element next, skip_separator skip_sep,
                void *stream_data)
{
    PyArrayObject *r;
    npy_intp i;
    char *dptr, *clean_sep, *tmp;
    int err = 0;
    npy_intp thisbuf = 0;
    npy_intp size;
    npy_intp bytes, totalbytes;

    size = (num >= 0) ? num : FROM_BUFFER_SIZE;
    r = (PyArrayObject *)
        PyArray_NewFromDescr(&PyArray_Type,
                             dtype,
                             1, &size,
                             NULL, NULL,
                             0, NULL);
    if (r == NULL) {
        return NULL;
    }
    clean_sep = swab_separator(sep);
    NPY_BEGIN_ALLOW_THREADS;
    totalbytes = bytes = size * dtype->elsize;
    dptr = r->data;
    for (i= 0; num < 0 || i < num; i++) {
        if (next(&stream, dptr, dtype, stream_data) < 0) {
            break;
        }
        *nread += 1;
        thisbuf += 1;
        dptr += dtype->elsize;
        if (num < 0 && thisbuf == size) {
            totalbytes += bytes;
            tmp = PyDataMem_RENEW(r->data, totalbytes);
            if (tmp == NULL) {
                err = 1;
                break;
            }
            r->data = tmp;
            dptr = tmp + (totalbytes - bytes);
            thisbuf = 0;
        }
        if (skip_sep(&stream, clean_sep, stream_data) < 0) {
            break;
        }
    }
    if (num < 0) {
        tmp = PyDataMem_RENEW(r->data, NPY_MAX(*nread,1)*dtype->elsize);
        if (tmp == NULL) {
            err = 1;
        }
        else {
            PyArray_DIM(r,0) = *nread;
            r->data = tmp;
        }
    }
    NPY_END_ALLOW_THREADS;
    free(clean_sep);
    if (err == 1) {
        PyErr_NoMemory();
    }
    if (PyErr_Occurred()) {
        Py_DECREF(r);
        return NULL;
    }
    return r;
}
#undef FROM_BUFFER_SIZE

/*NUMPY_API
 *
 * Given a ``FILE *`` pointer ``fp``, and a ``PyArray_Descr``, return an
 * array corresponding to the data encoded in that file.
 *
 * If the dtype is NULL, the default array type is used (double).
 * If non-null, the reference is stolen.
 *
 * The number of elements to read is given as ``num``; if it is < 0, then
 * then as many as possible are read.
 *
 * If ``sep`` is NULL or empty, then binary data is assumed, else
 * text data, with ``sep`` as the separator between elements. Whitespace in
 * the separator matches any length of whitespace in the text, and a match
 * for whitespace around the separator is added.
 *
 * For memory-mapped files, use the buffer interface. No more data than
 * necessary is read by this routine.
 */
NPY_NO_EXPORT PyObject *
PyArray_FromFile(FILE *fp, PyArray_Descr *dtype, npy_intp num, char *sep)
{
    PyArrayObject *ret;
    size_t nread = 0;

    if (PyDataType_REFCHK(dtype)) {
        PyErr_SetString(PyExc_ValueError,
                "Cannot read into object array");
        Py_DECREF(dtype);
        return NULL;
    }
    if (dtype->elsize == 0) {
        PyErr_SetString(PyExc_ValueError,
                "The elements are 0-sized.");
        Py_DECREF(dtype);
        return NULL;
    }
    if ((sep == NULL) || (strlen(sep) == 0)) {
        ret = array_fromfile_binary(fp, dtype, num, &nread);
    }
    else {
        if (dtype->f->scanfunc == NULL) {
            PyErr_SetString(PyExc_ValueError,
                    "Unable to read character files of that array type");
            Py_DECREF(dtype);
            return NULL;
        }
        ret = array_from_text(dtype, num, sep, &nread, fp,
                (next_element) fromfile_next_element,
                (skip_separator) fromfile_skip_separator, NULL);
    }
    if (ret == NULL) {
        Py_DECREF(dtype);
        return NULL;
    }
    if (((npy_intp) nread) < num) {
        /* Realloc memory for smaller number of elements */
        const size_t nsize = NPY_MAX(nread,1)*ret->descr->elsize;
        char *tmp;

        if((tmp = PyDataMem_RENEW(ret->data, nsize)) == NULL) {
            Py_DECREF(ret);
            return PyErr_NoMemory();
        }
        ret->data = tmp;
        PyArray_DIM(ret,0) = nread;
    }
    return (PyObject *)ret;
}

/*NUMPY_API*/
NPY_NO_EXPORT PyObject *
PyArray_FromBuffer(PyObject *buf, PyArray_Descr *type,
                   npy_intp count, npy_intp offset)
{
    PyArrayObject *ret;
    char *data;
    Py_ssize_t ts;
    npy_intp s, n;
    int itemsize;
    int write = 1;


    if (PyDataType_REFCHK(type)) {
        PyErr_SetString(PyExc_ValueError,
                        "cannot create an OBJECT array from memory"\
                        " buffer");
        Py_DECREF(type);
        return NULL;
    }
    if (type->elsize == 0) {
        PyErr_SetString(PyExc_ValueError,
                        "itemsize cannot be zero in type");
        Py_DECREF(type);
        return NULL;
    }
    if (Py_TYPE(buf)->tp_as_buffer == NULL
#if defined(NPY_PY3K)
        || Py_TYPE(buf)->tp_as_buffer->bf_getbuffer == NULL
#else
        || (Py_TYPE(buf)->tp_as_buffer->bf_getwritebuffer == NULL
            && Py_TYPE(buf)->tp_as_buffer->bf_getreadbuffer == NULL)
#endif
        ) {
        PyObject *newbuf;
        newbuf = PyObject_GetAttrString(buf, "__buffer__");
        if (newbuf == NULL) {
            Py_DECREF(type);
            return NULL;
        }
        buf = newbuf;
    }
    else {
        Py_INCREF(buf);
    }

    if (PyObject_AsWriteBuffer(buf, (void *)&data, &ts) == -1) {
        write = 0;
        PyErr_Clear();
        if (PyObject_AsReadBuffer(buf, (void *)&data, &ts) == -1) {
            Py_DECREF(buf);
            Py_DECREF(type);
            return NULL;
        }
    }

    if ((offset < 0) || (offset >= ts)) {
        PyErr_Format(PyExc_ValueError,
                     "offset must be non-negative and smaller than buffer "\
                     "lenth (%" INTP_FMT ")", (npy_intp)ts);
        Py_DECREF(buf);
        Py_DECREF(type);
        return NULL;
    }

    data += offset;
    s = (npy_intp)ts - offset;
    n = (npy_intp)count;
    itemsize = type->elsize;
    if (n < 0 ) {
        if (s % itemsize != 0) {
            PyErr_SetString(PyExc_ValueError,
                            "buffer size must be a multiple"\
                            " of element size");
            Py_DECREF(buf);
            Py_DECREF(type);
            return NULL;
        }
        n = s/itemsize;
    }
    else {
        if (s < n*itemsize) {
            PyErr_SetString(PyExc_ValueError,
                            "buffer is smaller than requested"\
                            " size");
            Py_DECREF(buf);
            Py_DECREF(type);
            return NULL;
        }
    }

    if ((ret = (PyArrayObject *)PyArray_NewFromDescr(&PyArray_Type,
                                                     type,
                                                     1, &n,
                                                     NULL, data,
                                                     DEFAULT,
                                                     NULL)) == NULL) {
        Py_DECREF(buf);
        return NULL;
    }

    if (!write) {
        ret->flags &= ~WRITEABLE;
    }
    /* Store a reference for decref on deallocation */
    ret->base = buf;
    PyArray_UpdateFlags(ret, ALIGNED);
    return (PyObject *)ret;
}

/*NUMPY_API
 *
 * Given a pointer to a string ``data``, a string length ``slen``, and
 * a ``PyArray_Descr``, return an array corresponding to the data
 * encoded in that string.
 *
 * If the dtype is NULL, the default array type is used (double).
 * If non-null, the reference is stolen.
 *
 * If ``slen`` is < 0, then the end of string is used for text data.
 * It is an error for ``slen`` to be < 0 for binary data (since embedded NULLs
 * would be the norm).
 *
 * The number of elements to read is given as ``num``; if it is < 0, then
 * then as many as possible are read.
 *
 * If ``sep`` is NULL or empty, then binary data is assumed, else
 * text data, with ``sep`` as the separator between elements. Whitespace in
 * the separator matches any length of whitespace in the text, and a match
 * for whitespace around the separator is added.
 */
NPY_NO_EXPORT PyObject *
PyArray_FromString(char *data, npy_intp slen, PyArray_Descr *dtype,
                   npy_intp num, char *sep)
{
    int itemsize;
    PyArrayObject *ret;
    Bool binary;

    if (dtype == NULL) {
        dtype=PyArray_DescrFromType(PyArray_DEFAULT);
    }
    if (PyDataType_FLAGCHK(dtype, NPY_ITEM_IS_POINTER)) {
        PyErr_SetString(PyExc_ValueError,
                        "Cannot create an object array from"    \
                        " a string");
        Py_DECREF(dtype);
        return NULL;
    }
    itemsize = dtype->elsize;
    if (itemsize == 0) {
        PyErr_SetString(PyExc_ValueError, "zero-valued itemsize");
        Py_DECREF(dtype);
        return NULL;
    }

    binary = ((sep == NULL) || (strlen(sep) == 0));
    if (binary) {
        if (num < 0 ) {
            if (slen % itemsize != 0) {
                PyErr_SetString(PyExc_ValueError,
                                "string size must be a "\
                                "multiple of element size");
                Py_DECREF(dtype);
                return NULL;
            }
            num = slen/itemsize;
        }
        else {
            if (slen < num*itemsize) {
                PyErr_SetString(PyExc_ValueError,
                                "string is smaller than " \
                                "requested size");
                Py_DECREF(dtype);
                return NULL;
            }
        }
        ret = (PyArrayObject *)
            PyArray_NewFromDescr(&PyArray_Type, dtype,
                                 1, &num, NULL, NULL,
                                 0, NULL);
        if (ret == NULL) {
            return NULL;
        }
        memcpy(ret->data, data, num*dtype->elsize);
    }
    else {
        /* read from character-based string */
        size_t nread = 0;
        char *end;

        if (dtype->f->scanfunc == NULL) {
            PyErr_SetString(PyExc_ValueError,
                            "don't know how to read "       \
                            "character strings with that "  \
                            "array type");
            Py_DECREF(dtype);
            return NULL;
        }
        if (slen < 0) {
            end = NULL;
        }
        else {
            end = data + slen;
        }
        ret = array_from_text(dtype, num, sep, &nread,
                              data,
                              (next_element) fromstr_next_element,
                              (skip_separator) fromstr_skip_separator,
                              end);
    }
    return (PyObject *)ret;
}

/*NUMPY_API
 *
 * steals a reference to dtype (which cannot be NULL)
 */
NPY_NO_EXPORT PyObject *
PyArray_FromIter(PyObject *obj, PyArray_Descr *dtype, npy_intp count)
{
    PyObject *value;
    PyObject *iter = PyObject_GetIter(obj);
    PyArrayObject *ret = NULL;
    npy_intp i, elsize, elcount;
    char *item, *new_data;

    if (iter == NULL) {
        goto done;
    }
    elcount = (count < 0) ? 0 : count;
    if ((elsize=dtype->elsize) == 0) {
        PyErr_SetString(PyExc_ValueError, "Must specify length "\
                        "when using variable-size data-type.");
        goto done;
    }

    /*
     * We would need to alter the memory RENEW code to decrement any
     * reference counts before throwing away any memory.
     */
    if (PyDataType_REFCHK(dtype)) {
        PyErr_SetString(PyExc_ValueError, "cannot create "\
                        "object arrays from iterator");
        goto done;
    }

    ret = (PyArrayObject *)PyArray_NewFromDescr(&PyArray_Type, dtype, 1,
                                                &elcount, NULL,NULL, 0, NULL);
    dtype = NULL;
    if (ret == NULL) {
        goto done;
    }
    for (i = 0; (i < count || count == -1) &&
             (value = PyIter_Next(iter)); i++) {
        if (i >= elcount) {
            /*
              Grow ret->data:
              this is similar for the strategy for PyListObject, but we use
              50% overallocation => 0, 4, 8, 14, 23, 36, 56, 86 ...
            */
            elcount = (i >> 1) + (i < 4 ? 4 : 2) + i;
            if (elcount <= NPY_MAX_INTP/elsize) {
                new_data = PyDataMem_RENEW(ret->data, elcount * elsize);
            }
            else {
                new_data = NULL;
            }
            if (new_data == NULL) {
                PyErr_SetString(PyExc_MemoryError,
                                "cannot allocate array memory");
                Py_DECREF(value);
                goto done;
            }
            ret->data = new_data;
        }
        ret->dimensions[0] = i + 1;

        if (((item = index2ptr(ret, i)) == NULL)
            || (ret->descr->f->setitem(value, item, ret) == -1)) {
            Py_DECREF(value);
            goto done;
        }
        Py_DECREF(value);
    }

    if (i < count) {
        PyErr_SetString(PyExc_ValueError, "iterator too short");
        goto done;
    }

    /*
     * Realloc the data so that don't keep extra memory tied up
     * (assuming realloc is reasonably good about reusing space...)
     */
    if (i == 0) {
        i = 1;
    }
    new_data = PyDataMem_RENEW(ret->data, i * elsize);
    if (new_data == NULL) {
        PyErr_SetString(PyExc_MemoryError, "cannot allocate array memory");
        goto done;
    }
    ret->data = new_data;

 done:
    Py_XDECREF(iter);
    Py_XDECREF(dtype);
    if (PyErr_Occurred()) {
        Py_XDECREF(ret);
        return NULL;
    }
    return (PyObject *)ret;
}

/*
 * This is the main array creation routine.
 *
 * Flags argument has multiple related meanings
 * depending on data and strides:
 *
 * If data is given, then flags is flags associated with data.
 * If strides is not given, then a contiguous strides array will be created
 * and the CONTIGUOUS bit will be set.  If the flags argument
 * has the FORTRAN bit set, then a FORTRAN-style strides array will be
 * created (and of course the FORTRAN flag bit will be set).
 *
 * If data is not given but created here, then flags will be DEFAULT
 * and a non-zero flags argument can be used to indicate a FORTRAN style
 * array is desired.
 */

NPY_NO_EXPORT size_t
_array_fill_strides(npy_intp *strides, npy_intp *dims, int nd, size_t itemsize,
                    int inflag, int *objflags)
{
    int i;
    /* Only make Fortran strides if not contiguous as well */
    if ((inflag & FORTRAN) && !(inflag & CONTIGUOUS)) {
        for (i = 0; i < nd; i++) {
            strides[i] = itemsize;
            itemsize *= dims[i] ? dims[i] : 1;
        }
        *objflags |= FORTRAN;
        if (nd > 1) {
            *objflags &= ~CONTIGUOUS;
        }
        else {
            *objflags |= CONTIGUOUS;
        }
    }
    else {
        for (i = nd - 1; i >= 0; i--) {
            strides[i] = itemsize;
            itemsize *= dims[i] ? dims[i] : 1;
        }
        *objflags |= CONTIGUOUS;
        if (nd > 1) {
            *objflags &= ~FORTRAN;
        }
        else {
            *objflags |= FORTRAN;
        }
    }
    return itemsize;
}

