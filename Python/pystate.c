
/* Thread and interpreter state structures and their interfaces */

#include "Python.h"
#include "pycore_pymem.h"
#include "pycore_pystate.h"

#define _PyThreadState_SET(value) \
    _Py_atomic_store_relaxed(&_PyRuntime.gilstate.tstate_current, \
                             (uintptr_t)(value))


/* --------------------------------------------------------------------------
CAUTION

Always use PyMem_RawMalloc() and PyMem_RawFree() directly in this file.  A
number of these functions are advertised as safe to call when the GIL isn't
held, and in a debug build Python redirects (e.g.) PyMem_NEW (etc) to Python's
debugging obmalloc functions.  Those aren't thread-safe (they rely on the GIL
to avoid the expense of doing their own locking).
-------------------------------------------------------------------------- */

#ifdef HAVE_DLOPEN
#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif
#if !HAVE_DECL_RTLD_LAZY
#define RTLD_LAZY 1
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

static _PyInitError
_PyRuntimeState_Init_impl(_PyRuntimeState *runtime)
{
    memset(runtime, 0, sizeof(*runtime));

    _PyGC_Initialize(&runtime->gc);
    _PyEval_Initialize(&runtime->ceval);

    runtime->gilstate.check_enabled = 1;

    /* A TSS key must be initialized with Py_tss_NEEDS_INIT
       in accordance with the specification. */
    Py_tss_t initial = Py_tss_NEEDS_INIT;
    runtime->gilstate.autoTSSkey = initial;

    runtime->interpreters.mutex = PyThread_allocate_lock();
    if (runtime->interpreters.mutex == NULL) {
        return _Py_INIT_ERR("Can't initialize threads for interpreter");
    }
    runtime->interpreters.next_id = -1;

    runtime->xidregistry.mutex = PyThread_allocate_lock();
    if (runtime->xidregistry.mutex == NULL) {
        return _Py_INIT_ERR("Can't initialize threads for cross-interpreter data registry");
    }

    return _Py_INIT_OK();
}

_PyInitError
_PyRuntimeState_Init(_PyRuntimeState *runtime)
{
    /* Force default allocator, since _PyRuntimeState_Fini() must
       use the same allocator than this function. */
    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    _PyInitError err = _PyRuntimeState_Init_impl(runtime);

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);
    return err;
}

void
_PyRuntimeState_Fini(_PyRuntimeState *runtime)
{
    /* Force the allocator used by _PyRuntimeState_Init(). */
    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    if (runtime->interpreters.mutex != NULL) {
        PyThread_free_lock(runtime->interpreters.mutex);
        runtime->interpreters.mutex = NULL;
    }

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);
}

#define HEAD_LOCK() PyThread_acquire_lock(_PyRuntime.interpreters.mutex, \
                                          WAIT_LOCK)
#define HEAD_UNLOCK() PyThread_release_lock(_PyRuntime.interpreters.mutex)

static void _PyGILState_NoteThreadState(PyThreadState* tstate);

_PyInitError
_PyInterpreterState_Enable(_PyRuntimeState *runtime)
{
    runtime->interpreters.next_id = 0;

    /* Py_Finalize() calls _PyRuntimeState_Fini() which clears the mutex.
       Create a new mutex if needed. */
    if (runtime->interpreters.mutex == NULL) {
        /* Force default allocator, since _PyRuntimeState_Fini() must
           use the same allocator than this function. */
        PyMemAllocatorEx old_alloc;
        _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

        runtime->interpreters.mutex = PyThread_allocate_lock();

        PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

        if (runtime->interpreters.mutex == NULL) {
            return _Py_INIT_ERR("Can't initialize threads for interpreter");
        }
    }

    return _Py_INIT_OK();
}

PyInterpreterState *
PyInterpreterState_New(void)
{
    PyInterpreterState *interp = (PyInterpreterState *)
                                 PyMem_RawMalloc(sizeof(PyInterpreterState));

    if (interp == NULL) {
        return NULL;
    }

    memset(interp, 0, sizeof(*interp));
    interp->id_refcount = -1;
    interp->check_interval = 100;

    interp->ceval.pending.lock = PyThread_allocate_lock();
    if (interp->ceval.pending.lock == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "failed to create interpreter ceval pending mutex");
        return NULL;
    }
    interp->core_config = _PyCoreConfig_INIT;
    interp->config = _PyMainInterpreterConfig_INIT;
    interp->eval_frame = _PyEval_EvalFrameDefault;
#ifdef HAVE_DLOPEN
#if HAVE_DECL_RTLD_NOW
    interp->dlopenflags = RTLD_NOW;
#else
    interp->dlopenflags = RTLD_LAZY;
#endif
#endif

    if (_PyRuntime.main_thread == 0) {
        _PyRuntime.main_thread = PyThread_get_thread_ident();
    }

    HEAD_LOCK();
    if (_PyRuntime.interpreters.next_id < 0) {
        /* overflow or Py_Initialize() not called! */
        PyErr_SetString(PyExc_RuntimeError,
                        "failed to get an interpreter ID");
        PyMem_RawFree(interp);
        interp = NULL;
    } else {
        interp->id = _PyRuntime.interpreters.next_id;
        _PyRuntime.interpreters.next_id += 1;
        interp->next = _PyRuntime.interpreters.head;
        if (_PyRuntime.interpreters.main == NULL) {
            _PyRuntime.interpreters.main = interp;
        }
        _PyRuntime.interpreters.head = interp;
    }
    HEAD_UNLOCK();

    if (interp == NULL) {
        return NULL;
    }

    interp->tstate_next_unique_id = 0;

    return interp;
}


void
PyInterpreterState_Clear(PyInterpreterState *interp)
{
    PyThreadState *p;
    HEAD_LOCK();
    for (p = interp->tstate_head; p != NULL; p = p->next)
        PyThreadState_Clear(p);
    HEAD_UNLOCK();
    _PyCoreConfig_Clear(&interp->core_config);
    _PyMainInterpreterConfig_Clear(&interp->config);
    Py_CLEAR(interp->codec_search_path);
    Py_CLEAR(interp->codec_search_cache);
    Py_CLEAR(interp->codec_error_registry);
    Py_CLEAR(interp->modules);
    Py_CLEAR(interp->modules_by_index);
    Py_CLEAR(interp->sysdict);
    Py_CLEAR(interp->builtins);
    Py_CLEAR(interp->builtins_copy);
    Py_CLEAR(interp->importlib);
    Py_CLEAR(interp->import_func);
#ifdef HAVE_FORK
    Py_CLEAR(interp->before_forkers);
    Py_CLEAR(interp->after_forkers_parent);
    Py_CLEAR(interp->after_forkers_child);
#endif
    // XXX Once we have one allocator per interpreter (i.e.
    // per-interpreter GC) we must ensure that all of the interpreter's
    // objects have been cleaned up at the point.
}


static void
zapthreads(PyInterpreterState *interp)
{
    PyThreadState *p;
    /* No need to lock the mutex here because this should only happen
       when the threads are all really dead (XXX famous last words). */
    while ((p = interp->tstate_head) != NULL) {
        PyThreadState_Delete(p);
    }
}


void
PyInterpreterState_Delete(PyInterpreterState *interp)
{
    PyInterpreterState **p;
    zapthreads(interp);
    HEAD_LOCK();
    for (p = &_PyRuntime.interpreters.head; ; p = &(*p)->next) {
        if (*p == NULL)
            Py_FatalError(
                "PyInterpreterState_Delete: invalid interp");
        if (*p == interp)
            break;
    }
    if (interp->tstate_head != NULL)
        Py_FatalError("PyInterpreterState_Delete: remaining threads");
    *p = interp->next;
    if (_PyRuntime.interpreters.main == interp) {
        _PyRuntime.interpreters.main = NULL;
        if (_PyRuntime.interpreters.head != NULL)
            Py_FatalError("PyInterpreterState_Delete: remaining subinterpreters");
    }
    HEAD_UNLOCK();
    if (interp->id_mutex != NULL) {
        PyThread_free_lock(interp->id_mutex);
    }
    if (interp->ceval.pending.lock != NULL) {
        PyThread_free_lock(interp->ceval.pending.lock);
    }
    PyMem_RawFree(interp);
}


/*
 * Delete all interpreter states except the main interpreter.  If there
 * is a current interpreter state, it *must* be the main interpreter.
 */
void
_PyInterpreterState_DeleteExceptMain()
{
    PyThreadState *tstate = PyThreadState_Swap(NULL);
    if (tstate != NULL && tstate->interp != _PyRuntime.interpreters.main) {
        Py_FatalError("PyInterpreterState_DeleteExceptMain: not main interpreter");
    }

    HEAD_LOCK();
    PyInterpreterState *interp = _PyRuntime.interpreters.head;
    _PyRuntime.interpreters.head = NULL;
    while (interp != NULL) {
        if (interp == _PyRuntime.interpreters.main) {
            _PyRuntime.interpreters.main->next = NULL;
            _PyRuntime.interpreters.head = interp;
            interp = interp->next;
            continue;
        }

        PyInterpreterState_Clear(interp);  // XXX must activate?
        zapthreads(interp);
        if (interp->id_mutex != NULL) {
            PyThread_free_lock(interp->id_mutex);
        }
        PyInterpreterState *prev_interp = interp;
        interp = interp->next;
        PyMem_RawFree(prev_interp);
    }
    HEAD_UNLOCK();

    if (_PyRuntime.interpreters.head == NULL) {
        Py_FatalError("PyInterpreterState_DeleteExceptMain: missing main");
    }
    PyThreadState_Swap(tstate);
}


PyInterpreterState *
_PyInterpreterState_Get(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate == NULL) {
        Py_FatalError("_PyInterpreterState_Get(): no current thread state");
    }
    PyInterpreterState *interp = tstate->interp;
    if (interp == NULL) {
        Py_FatalError("_PyInterpreterState_Get(): no current interpreter");
    }
    return interp;
}


int64_t
PyInterpreterState_GetID(PyInterpreterState *interp)
{
    if (interp == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "no interpreter provided");
        return -1;
    }
    return interp->id;
}


PyInterpreterState *
_PyInterpreterState_LookUpID(PY_INT64_T requested_id)
{
    if (requested_id < 0)
        goto error;

    PyInterpreterState *interp = PyInterpreterState_Head();
    while (interp != NULL) {
        PY_INT64_T id = PyInterpreterState_GetID(interp);
        if (id < 0)
            return NULL;
        if (requested_id == id)
            return interp;
        interp = PyInterpreterState_Next(interp);
    }

error:
    PyErr_Format(PyExc_RuntimeError,
                 "unrecognized interpreter ID %lld", requested_id);
    return NULL;
}


int
_PyInterpreterState_IDInitref(PyInterpreterState *interp)
{
    if (interp->id_mutex != NULL) {
        return 0;
    }
    interp->id_mutex = PyThread_allocate_lock();
    if (interp->id_mutex == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "failed to create init interpreter ID mutex");
        return -1;
    }
    interp->id_refcount = 0;
    return 0;
}


void
_PyInterpreterState_IDIncref(PyInterpreterState *interp)
{
    if (interp->id_mutex == NULL) {
        return;
    }
    PyThread_acquire_lock(interp->id_mutex, WAIT_LOCK);
    interp->id_refcount += 1;
    PyThread_release_lock(interp->id_mutex);
}


void
_PyInterpreterState_IDDecref(PyInterpreterState *interp)
{
    if (interp->id_mutex == NULL) {
        return;
    }
    PyThread_acquire_lock(interp->id_mutex, WAIT_LOCK);
    assert(interp->id_refcount != 0);
    interp->id_refcount -= 1;
    int64_t refcount = interp->id_refcount;
    PyThread_release_lock(interp->id_mutex);

    if (refcount == 0) {
        // XXX Using the "head" thread isn't strictly correct.
        PyThreadState *tstate = PyInterpreterState_ThreadHead(interp);
        // XXX Possible GILState issues?
        PyThreadState *save_tstate = PyThreadState_Swap(tstate);
        Py_EndInterpreter(tstate);
        PyThreadState_Swap(save_tstate);
    }
}

_PyCoreConfig *
_PyInterpreterState_GetCoreConfig(PyInterpreterState *interp)
{
    return &interp->core_config;
}

_PyMainInterpreterConfig *
_PyInterpreterState_GetMainConfig(PyInterpreterState *interp)
{
    return &interp->config;
}

/* Default implementation for _PyThreadState_GetFrame */
static struct _frame *
threadstate_getframe(PyThreadState *self)
{
    return self->frame;
}

static PyThreadState *
new_threadstate(PyInterpreterState *interp, int init)
{
    PyThreadState *tstate = (PyThreadState *)PyMem_RawMalloc(sizeof(PyThreadState));

    if (_PyThreadState_GetFrame == NULL)
        _PyThreadState_GetFrame = threadstate_getframe;

    if (tstate != NULL) {
        tstate->interp = interp;

        tstate->frame = NULL;
        tstate->recursion_depth = 0;
        tstate->overflowed = 0;
        tstate->recursion_critical = 0;
        tstate->stackcheck_counter = 0;
        tstate->tracing = 0;
        tstate->use_tracing = 0;
        tstate->gilstate_counter = 0;
        tstate->async_exc = NULL;
        tstate->thread_id = PyThread_get_thread_ident();

        tstate->dict = NULL;

        tstate->curexc_type = NULL;
        tstate->curexc_value = NULL;
        tstate->curexc_traceback = NULL;

        tstate->exc_state.exc_type = NULL;
        tstate->exc_state.exc_value = NULL;
        tstate->exc_state.exc_traceback = NULL;
        tstate->exc_state.previous_item = NULL;
        tstate->exc_info = &tstate->exc_state;

        tstate->c_profilefunc = NULL;
        tstate->c_tracefunc = NULL;
        tstate->c_profileobj = NULL;
        tstate->c_traceobj = NULL;

        tstate->trash_delete_nesting = 0;
        tstate->trash_delete_later = NULL;
        tstate->on_delete = NULL;
        tstate->on_delete_data = NULL;

        tstate->coroutine_origin_tracking_depth = 0;

        tstate->coroutine_wrapper = NULL;
        tstate->in_coroutine_wrapper = 0;

        tstate->async_gen_firstiter = NULL;
        tstate->async_gen_finalizer = NULL;

        tstate->context = NULL;
        tstate->context_ver = 1;

        tstate->id = ++interp->tstate_next_unique_id;

        if (init)
            _PyThreadState_Init(tstate);

        HEAD_LOCK();
        tstate->prev = NULL;
        tstate->next = interp->tstate_head;
        if (tstate->next)
            tstate->next->prev = tstate;
        interp->tstate_head = tstate;
        HEAD_UNLOCK();
    }

    return tstate;
}

PyThreadState *
PyThreadState_New(PyInterpreterState *interp)
{
    return new_threadstate(interp, 1);
}

PyThreadState *
_PyThreadState_Prealloc(PyInterpreterState *interp)
{
    return new_threadstate(interp, 0);
}

void
_PyThreadState_Init(PyThreadState *tstate)
{
    _PyGILState_NoteThreadState(tstate);
}

PyObject*
PyState_FindModule(struct PyModuleDef* module)
{
    Py_ssize_t index = module->m_base.m_index;
    PyInterpreterState *state = _PyInterpreterState_GET_UNSAFE();
    PyObject *res;
    if (module->m_slots) {
        return NULL;
    }
    if (index == 0)
        return NULL;
    if (state->modules_by_index == NULL)
        return NULL;
    if (index >= PyList_GET_SIZE(state->modules_by_index))
        return NULL;
    res = PyList_GET_ITEM(state->modules_by_index, index);
    return res==Py_None ? NULL : res;
}

int
_PyState_AddModule(PyObject* module, struct PyModuleDef* def)
{
    PyInterpreterState *state;
    if (!def) {
        assert(PyErr_Occurred());
        return -1;
    }
    if (def->m_slots) {
        PyErr_SetString(PyExc_SystemError,
                        "PyState_AddModule called on module with slots");
        return -1;
    }
    state = _PyInterpreterState_GET_UNSAFE();
    if (!state->modules_by_index) {
        state->modules_by_index = PyList_New(0);
        if (!state->modules_by_index)
            return -1;
    }
    while(PyList_GET_SIZE(state->modules_by_index) <= def->m_base.m_index)
        if (PyList_Append(state->modules_by_index, Py_None) < 0)
            return -1;
    Py_INCREF(module);
    return PyList_SetItem(state->modules_by_index,
                          def->m_base.m_index, module);
}

int
PyState_AddModule(PyObject* module, struct PyModuleDef* def)
{
    Py_ssize_t index;
    PyInterpreterState *state = _PyInterpreterState_GET_UNSAFE();
    if (!def) {
        Py_FatalError("PyState_AddModule: Module Definition is NULL");
        return -1;
    }
    index = def->m_base.m_index;
    if (state->modules_by_index) {
        if(PyList_GET_SIZE(state->modules_by_index) >= index) {
            if(module == PyList_GET_ITEM(state->modules_by_index, index)) {
                Py_FatalError("PyState_AddModule: Module already added!");
                return -1;
            }
        }
    }
    return _PyState_AddModule(module, def);
}

int
PyState_RemoveModule(struct PyModuleDef* def)
{
    PyInterpreterState *state;
    Py_ssize_t index = def->m_base.m_index;
    if (def->m_slots) {
        PyErr_SetString(PyExc_SystemError,
                        "PyState_RemoveModule called on module with slots");
        return -1;
    }
    state = _PyInterpreterState_GET_UNSAFE();
    if (index == 0) {
        Py_FatalError("PyState_RemoveModule: Module index invalid.");
        return -1;
    }
    if (state->modules_by_index == NULL) {
        Py_FatalError("PyState_RemoveModule: Interpreters module-list not acessible.");
        return -1;
    }
    if (index > PyList_GET_SIZE(state->modules_by_index)) {
        Py_FatalError("PyState_RemoveModule: Module index out of bounds.");
        return -1;
    }
    Py_INCREF(Py_None);
    return PyList_SetItem(state->modules_by_index, index, Py_None);
}

/* used by import.c:PyImport_Cleanup */
void
_PyState_ClearModules(void)
{
    PyInterpreterState *state = _PyInterpreterState_GET_UNSAFE();
    if (state->modules_by_index) {
        Py_ssize_t i;
        for (i = 0; i < PyList_GET_SIZE(state->modules_by_index); i++) {
            PyObject *m = PyList_GET_ITEM(state->modules_by_index, i);
            if (PyModule_Check(m)) {
                /* cleanup the saved copy of module dicts */
                PyModuleDef *md = PyModule_GetDef(m);
                if (md)
                    Py_CLEAR(md->m_base.m_copy);
            }
        }
        /* Setting modules_by_index to NULL could be dangerous, so we
           clear the list instead. */
        if (PyList_SetSlice(state->modules_by_index,
                            0, PyList_GET_SIZE(state->modules_by_index),
                            NULL))
            PyErr_WriteUnraisable(state->modules_by_index);
    }
}

void
PyThreadState_Clear(PyThreadState *tstate)
{
    int verbose = tstate->interp->core_config.verbose;

    if (verbose && tstate->frame != NULL)
        fprintf(stderr,
          "PyThreadState_Clear: warning: thread still has a frame\n");

    Py_CLEAR(tstate->frame);

    Py_CLEAR(tstate->dict);
    Py_CLEAR(tstate->async_exc);

    Py_CLEAR(tstate->curexc_type);
    Py_CLEAR(tstate->curexc_value);
    Py_CLEAR(tstate->curexc_traceback);

    Py_CLEAR(tstate->exc_state.exc_type);
    Py_CLEAR(tstate->exc_state.exc_value);
    Py_CLEAR(tstate->exc_state.exc_traceback);

    /* The stack of exception states should contain just this thread. */
    if (verbose && tstate->exc_info != &tstate->exc_state) {
        fprintf(stderr,
          "PyThreadState_Clear: warning: thread still has a generator\n");
    }

    tstate->c_profilefunc = NULL;
    tstate->c_tracefunc = NULL;
    Py_CLEAR(tstate->c_profileobj);
    Py_CLEAR(tstate->c_traceobj);

    Py_CLEAR(tstate->coroutine_wrapper);
    Py_CLEAR(tstate->async_gen_firstiter);
    Py_CLEAR(tstate->async_gen_finalizer);

    Py_CLEAR(tstate->context);
}


/* Common code for PyThreadState_Delete() and PyThreadState_DeleteCurrent() */
static void
tstate_delete_common(PyThreadState *tstate)
{
    PyInterpreterState *interp;
    if (tstate == NULL)
        Py_FatalError("PyThreadState_Delete: NULL tstate");
    interp = tstate->interp;
    if (interp == NULL)
        Py_FatalError("PyThreadState_Delete: NULL interp");
    HEAD_LOCK();
    if (tstate->prev)
        tstate->prev->next = tstate->next;
    else
        interp->tstate_head = tstate->next;
    if (tstate->next)
        tstate->next->prev = tstate->prev;
    HEAD_UNLOCK();
    if (tstate->on_delete != NULL) {
        tstate->on_delete(tstate->on_delete_data);
    }
    PyMem_RawFree(tstate);
}


void
PyThreadState_Delete(PyThreadState *tstate)
{
    if (tstate == _PyThreadState_GET())
        Py_FatalError("PyThreadState_Delete: tstate is still current");
    if (_PyRuntime.gilstate.autoInterpreterState &&
        PyThread_tss_get(&_PyRuntime.gilstate.autoTSSkey) == tstate)
    {
        PyThread_tss_set(&_PyRuntime.gilstate.autoTSSkey, NULL);
    }
    tstate_delete_common(tstate);
}


void
PyThreadState_DeleteCurrent()
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate == NULL)
        Py_FatalError(
            "PyThreadState_DeleteCurrent: no current tstate");
    tstate_delete_common(tstate);
    if (_PyRuntime.gilstate.autoInterpreterState &&
        PyThread_tss_get(&_PyRuntime.gilstate.autoTSSkey) == tstate)
    {
        PyThread_tss_set(&_PyRuntime.gilstate.autoTSSkey, NULL);
    }
    _PyThreadState_SET(NULL);
    PyEval_ReleaseLock();
}


/*
 * Delete all thread states except the one passed as argument.
 * Note that, if there is a current thread state, it *must* be the one
 * passed as argument.  Also, this won't touch any other interpreters
 * than the current one, since we don't know which thread state should
 * be kept in those other interpreteres.
 */
void
_PyThreadState_DeleteExcept(PyThreadState *tstate)
{
    PyInterpreterState *interp = tstate->interp;
    PyThreadState *p, *next, *garbage;
    HEAD_LOCK();
    /* Remove all thread states, except tstate, from the linked list of
       thread states.  This will allow calling PyThreadState_Clear()
       without holding the lock. */
    garbage = interp->tstate_head;
    if (garbage == tstate)
        garbage = tstate->next;
    if (tstate->prev)
        tstate->prev->next = tstate->next;
    if (tstate->next)
        tstate->next->prev = tstate->prev;
    tstate->prev = tstate->next = NULL;
    interp->tstate_head = tstate;
    HEAD_UNLOCK();
    /* Clear and deallocate all stale thread states.  Even if this
       executes Python code, we should be safe since it executes
       in the current thread, not one of the stale threads. */
    for (p = garbage; p; p = next) {
        next = p->next;
        PyThreadState_Clear(p);
        PyMem_RawFree(p);
    }
}


PyThreadState *
_PyThreadState_UncheckedGet(void)
{
    return _PyThreadState_GET();
}


PyThreadState *
PyThreadState_Get(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate == NULL)
        Py_FatalError("PyThreadState_Get: no current thread");

    return tstate;
}


PyThreadState *
PyThreadState_Swap(PyThreadState *newts)
{
    PyThreadState *oldts = _PyThreadState_GET();

    _PyThreadState_SET(newts);
    /* It should not be possible for more than one thread state
       to be used for a thread.  Check this the best we can in debug
       builds.
    */
#if defined(Py_DEBUG)
    if (newts) {
        /* This can be called from PyEval_RestoreThread(). Similar
           to it, we need to ensure errno doesn't change.
        */
        int err = errno;
        PyThreadState *check = PyGILState_GetThisThreadState();
        if (check && check->interp == newts->interp && check != newts)
            Py_FatalError("Invalid thread state for this thread");
        errno = err;
    }
#endif
    return oldts;
}

/* An extension mechanism to store arbitrary additional per-thread state.
   PyThreadState_GetDict() returns a dictionary that can be used to hold such
   state; the caller should pick a unique key and store its state there.  If
   PyThreadState_GetDict() returns NULL, an exception has *not* been raised
   and the caller should assume no per-thread state is available. */

PyObject *
PyThreadState_GetDict(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate == NULL)
        return NULL;

    if (tstate->dict == NULL) {
        PyObject *d;
        tstate->dict = d = PyDict_New();
        if (d == NULL)
            PyErr_Clear();
    }
    return tstate->dict;
}


/* Asynchronously raise an exception in a thread.
   Requested by Just van Rossum and Alex Martelli.
   To prevent naive misuse, you must write your own extension
   to call this, or use ctypes.  Must be called with the GIL held.
   Returns the number of tstates modified (normally 1, but 0 if `id` didn't
   match any known thread id).  Can be called with exc=NULL to clear an
   existing async exception.  This raises no exceptions. */

int
PyThreadState_SetAsyncExc(unsigned long id, PyObject *exc)
{
    PyInterpreterState *interp = _PyInterpreterState_GET_UNSAFE();
    PyThreadState *p;

    /* Although the GIL is held, a few C API functions can be called
     * without the GIL held, and in particular some that create and
     * destroy thread and interpreter states.  Those can mutate the
     * list of thread states we're traversing, so to prevent that we lock
     * head_mutex for the duration.
     */
    HEAD_LOCK();
    for (p = interp->tstate_head; p != NULL; p = p->next) {
        if (p->thread_id == id) {
            /* Tricky:  we need to decref the current value
             * (if any) in p->async_exc, but that can in turn
             * allow arbitrary Python code to run, including
             * perhaps calls to this function.  To prevent
             * deadlock, we need to release head_mutex before
             * the decref.
             */
            PyObject *old_exc = p->async_exc;
            Py_XINCREF(exc);
            p->async_exc = exc;
            HEAD_UNLOCK();
            Py_XDECREF(old_exc);
            _PyEval_SignalAsyncExc(interp);
            return 1;
        }
    }
    HEAD_UNLOCK();
    return 0;
}


/* Routines for advanced debuggers, requested by David Beazley.
   Don't use unless you know what you are doing! */

PyInterpreterState *
PyInterpreterState_Head(void)
{
    return _PyRuntime.interpreters.head;
}

PyInterpreterState *
PyInterpreterState_Main(void)
{
    return _PyRuntime.interpreters.main;
}

PyInterpreterState *
PyInterpreterState_Next(PyInterpreterState *interp) {
    return interp->next;
}

PyThreadState *
PyInterpreterState_ThreadHead(PyInterpreterState *interp) {
    return interp->tstate_head;
}

PyThreadState *
PyThreadState_Next(PyThreadState *tstate) {
    return tstate->next;
}

/* The implementation of sys._current_frames().  This is intended to be
   called with the GIL held, as it will be when called via
   sys._current_frames().  It's possible it would work fine even without
   the GIL held, but haven't thought enough about that.
*/
PyObject *
_PyThread_CurrentFrames(void)
{
    PyObject *result;
    PyInterpreterState *i;

    result = PyDict_New();
    if (result == NULL)
        return NULL;

    /* for i in all interpreters:
     *     for t in all of i's thread states:
     *          if t's frame isn't NULL, map t's id to its frame
     * Because these lists can mutate even when the GIL is held, we
     * need to grab head_mutex for the duration.
     */
    HEAD_LOCK();
    for (i = _PyRuntime.interpreters.head; i != NULL; i = i->next) {
        PyThreadState *t;
        for (t = i->tstate_head; t != NULL; t = t->next) {
            PyObject *id;
            int stat;
            struct _frame *frame = t->frame;
            if (frame == NULL)
                continue;
            id = PyLong_FromUnsignedLong(t->thread_id);
            if (id == NULL)
                goto Fail;
            stat = PyDict_SetItem(result, id, (PyObject *)frame);
            Py_DECREF(id);
            if (stat < 0)
                goto Fail;
        }
    }
    HEAD_UNLOCK();
    return result;

 Fail:
    HEAD_UNLOCK();
    Py_DECREF(result);
    return NULL;
}

/* Python "auto thread state" API. */

/* Keep this as a static, as it is not reliable!  It can only
   ever be compared to the state for the *current* thread.
   * If not equal, then it doesn't matter that the actual
     value may change immediately after comparison, as it can't
     possibly change to the current thread's state.
   * If equal, then the current thread holds the lock, so the value can't
     change until we yield the lock.
*/
static int
PyThreadState_IsCurrent(PyThreadState *tstate)
{
    /* Must be the tstate for this thread */
    assert(PyGILState_GetThisThreadState()==tstate);
    return tstate == _PyThreadState_GET();
}

/* Internal initialization/finalization functions called by
   Py_Initialize/Py_FinalizeEx
*/
void
_PyGILState_Init(PyInterpreterState *i, PyThreadState *t)
{
    assert(i && t); /* must init with valid states */
    if (PyThread_tss_create(&_PyRuntime.gilstate.autoTSSkey) != 0) {
        Py_FatalError("Could not allocate TSS entry");
    }
    _PyRuntime.gilstate.autoInterpreterState = i;
    assert(PyThread_tss_get(&_PyRuntime.gilstate.autoTSSkey) == NULL);
    assert(t->gilstate_counter == 0);

    _PyGILState_NoteThreadState(t);
}

PyInterpreterState *
_PyGILState_GetInterpreterStateUnsafe(void)
{
    return _PyRuntime.gilstate.autoInterpreterState;
}

void
_PyGILState_Fini(void)
{
    PyThread_tss_delete(&_PyRuntime.gilstate.autoTSSkey);
    _PyRuntime.gilstate.autoInterpreterState = NULL;
}

/* Reset the TSS key - called by PyOS_AfterFork_Child().
 * This should not be necessary, but some - buggy - pthread implementations
 * don't reset TSS upon fork(), see issue #10517.
 */
void
_PyGILState_Reinit(void)
{
    /* Force default allocator, since _PyRuntimeState_Fini() must
       use the same allocator than this function. */
    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    _PyRuntime.interpreters.mutex = PyThread_allocate_lock();

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    if (_PyRuntime.interpreters.mutex == NULL) {
        Py_FatalError("Can't initialize threads for interpreter");
    }

    PyThreadState *tstate = PyGILState_GetThisThreadState();
    PyThread_tss_delete(&_PyRuntime.gilstate.autoTSSkey);
    if (PyThread_tss_create(&_PyRuntime.gilstate.autoTSSkey) != 0) {
        Py_FatalError("Could not allocate TSS entry");
    }

    /* If the thread had an associated auto thread state, reassociate it with
     * the new key. */
    if (tstate &&
        PyThread_tss_set(&_PyRuntime.gilstate.autoTSSkey, (void *)tstate) != 0)
    {
        Py_FatalError("Couldn't create autoTSSkey mapping");
    }
}

/* When a thread state is created for a thread by some mechanism other than
   PyGILState_Ensure, it's important that the GILState machinery knows about
   it so it doesn't try to create another thread state for the thread (this is
   a better fix for SF bug #1010677 than the first one attempted).
*/
static void
_PyGILState_NoteThreadState(PyThreadState* tstate)
{
    /* If autoTSSkey isn't initialized, this must be the very first
       threadstate created in Py_Initialize().  Don't do anything for now
       (we'll be back here when _PyGILState_Init is called). */
    if (!_PyRuntime.gilstate.autoInterpreterState)
        return;

    /* Stick the thread state for this thread in thread specific storage.

       The only situation where you can legitimately have more than one
       thread state for an OS level thread is when there are multiple
       interpreters.

       You shouldn't really be using the PyGILState_ APIs anyway (see issues
       #10915 and #15751).

       The first thread state created for that given OS level thread will
       "win", which seems reasonable behaviour.
    */
    if (PyThread_tss_get(&_PyRuntime.gilstate.autoTSSkey) == NULL) {
        if ((PyThread_tss_set(&_PyRuntime.gilstate.autoTSSkey, (void *)tstate)
             ) != 0)
        {
            Py_FatalError("Couldn't create autoTSSkey mapping");
        }
    }

    /* PyGILState_Release must not try to delete this thread state. */
    tstate->gilstate_counter = 1;
}

/* The public functions */
PyThreadState *
PyGILState_GetThisThreadState(void)
{
    if (_PyRuntime.gilstate.autoInterpreterState == NULL)
        return NULL;
    return (PyThreadState *)PyThread_tss_get(&_PyRuntime.gilstate.autoTSSkey);
}

int
PyGILState_Check(void)
{
    PyThreadState *tstate;

    if (!_PyGILState_check_enabled)
        return 1;

    if (!PyThread_tss_is_created(&_PyRuntime.gilstate.autoTSSkey)) {
        return 1;
    }

    tstate = _PyThreadState_GET();
    if (tstate == NULL)
        return 0;

    return (tstate == PyGILState_GetThisThreadState());
}

PyGILState_STATE
PyGILState_Ensure(void)
{
    int current;
    PyThreadState *tcur;
    int need_init_threads = 0;

    /* Note that we do not auto-init Python here - apart from
       potential races with 2 threads auto-initializing, pep-311
       spells out other issues.  Embedders are expected to have
       called Py_Initialize() and usually PyEval_InitThreads().
    */
    /* Py_Initialize() hasn't been called! */
    assert(_PyRuntime.gilstate.autoInterpreterState);

    tcur = (PyThreadState *)PyThread_tss_get(&_PyRuntime.gilstate.autoTSSkey);
    if (tcur == NULL) {
        need_init_threads = 1;

        /* Create a new thread state for this thread */
        tcur = PyThreadState_New(_PyRuntime.gilstate.autoInterpreterState);
        if (tcur == NULL)
            Py_FatalError("Couldn't create thread-state for new thread");
        /* This is our thread state!  We'll need to delete it in the
           matching call to PyGILState_Release(). */
        tcur->gilstate_counter = 0;
        current = 0; /* new thread state is never current */
    }
    else {
        current = PyThreadState_IsCurrent(tcur);
    }

    if (current == 0) {
        PyEval_RestoreThread(tcur);
    }

    /* Update our counter in the thread-state - no need for locks:
       - tcur will remain valid as we hold the GIL.
       - the counter is safe as we are the only thread "allowed"
         to modify this value
    */
    ++tcur->gilstate_counter;

    if (need_init_threads) {
        /* At startup, Python has no concrete GIL. If PyGILState_Ensure() is
           called from a new thread for the first time, we need the create the
           GIL. */
        PyEval_InitThreads();
    }

    return current ? PyGILState_LOCKED : PyGILState_UNLOCKED;
}

void
PyGILState_Release(PyGILState_STATE oldstate)
{
    PyThreadState *tcur = (PyThreadState *)PyThread_tss_get(
                                &_PyRuntime.gilstate.autoTSSkey);
    if (tcur == NULL)
        Py_FatalError("auto-releasing thread-state, "
                      "but no thread-state for this thread");
    /* We must hold the GIL and have our thread state current */
    /* XXX - remove the check - the assert should be fine,
       but while this is very new (April 2003), the extra check
       by release-only users can't hurt.
    */
    if (! PyThreadState_IsCurrent(tcur))
        Py_FatalError("This thread state must be current when releasing");
    assert(PyThreadState_IsCurrent(tcur));
    --tcur->gilstate_counter;
    assert(tcur->gilstate_counter >= 0); /* illegal counter value */

    /* If we're going to destroy this thread-state, we must
     * clear it while the GIL is held, as destructors may run.
     */
    if (tcur->gilstate_counter == 0) {
        /* can't have been locked when we created it */
        assert(oldstate == PyGILState_UNLOCKED);
        PyThreadState_Clear(tcur);
        /* Delete the thread-state.  Note this releases the GIL too!
         * It's vital that the GIL be held here, to avoid shutdown
         * races; see bugs 225673 and 1061968 (that nasty bug has a
         * habit of coming back).
         */
        PyThreadState_DeleteCurrent();
    }
    /* Release the lock if necessary */
    else if (oldstate == PyGILState_UNLOCKED)
        PyEval_SaveThread();
}


/**************************/
/* cross-interpreter data */
/**************************/

/* cross-interpreter data */

crossinterpdatafunc _PyCrossInterpreterData_Lookup(PyObject *);

/* This is a separate func from _PyCrossInterpreterData_Lookup in order
   to keep the registry code separate. */
static crossinterpdatafunc
_lookup_getdata(PyObject *obj)
{
    crossinterpdatafunc getdata = _PyCrossInterpreterData_Lookup(obj);
    if (getdata == NULL && PyErr_Occurred() == 0)
        PyErr_Format(PyExc_ValueError,
                     "%S does not support cross-interpreter data", obj);
    return getdata;
}

int
_PyObject_CheckCrossInterpreterData(PyObject *obj)
{
    crossinterpdatafunc getdata = _lookup_getdata(obj);
    if (getdata == NULL) {
        return -1;
    }
    return 0;
}

static int
_check_xidata(_PyCrossInterpreterData *data)
{
    // data->data can be anything, including NULL, so we don't check it.

    // data->obj may be NULL, so we don't check it.

    if (data->interp < 0) {
        PyErr_SetString(PyExc_SystemError, "missing interp");
        return -1;
    }

    if (data->new_object == NULL) {
        PyErr_SetString(PyExc_SystemError, "missing new_object func");
        return -1;
    }

    // data->free may be NULL, so we don't check it.

    return 0;
}

int
_PyObject_GetCrossInterpreterData(PyObject *obj, _PyCrossInterpreterData *data)
{
    // _PyInterpreterState_Get() aborts if lookup fails, so we don't need
    // to check the result for NULL.
    PyInterpreterState *interp = _PyInterpreterState_Get();

    // Reset data before re-populating.
    *data = (_PyCrossInterpreterData){0};
    data->free = PyMem_RawFree;  // Set a default that may be overridden.

    // Call the "getdata" func for the object.
    Py_INCREF(obj);
    crossinterpdatafunc getdata = _lookup_getdata(obj);
    if (getdata == NULL) {
        Py_DECREF(obj);
        return -1;
    }
    int res = getdata(obj, data);
    Py_DECREF(obj);
    if (res != 0) {
        return -1;
    }

    // Fill in the blanks and validate the result.
    data->interp = interp->id;
    if (_check_xidata(data) != 0) {
        _PyCrossInterpreterData_Release(data);
        return -1;
    }

    return 0;
}

static void
_release_xidata(void *arg)
{
    _PyCrossInterpreterData *data = (_PyCrossInterpreterData *)arg;
    if (data->free != NULL) {
        data->free(data->data);
    }
    Py_XDECREF(data->obj);
}

static void
_call_in_interpreter(PyInterpreterState *interp,
                     void (*func)(void *), void *arg)
{
    /* We would use Py_AddPendingCall() if it weren't specific to the
     * main interpreter (see bpo-33608).  In the meantime we take a
     * naive approach.
     */
    PyThreadState *save_tstate = NULL;
    if (interp != _PyInterpreterState_Get()) {
        // XXX Using the "head" thread isn't strictly correct.
        PyThreadState *tstate = PyInterpreterState_ThreadHead(interp);
        // XXX Possible GILState issues?
        save_tstate = PyThreadState_Swap(tstate);
    }

    func(arg);

    // Switch back.
    if (save_tstate != NULL) {
        PyThreadState_Swap(save_tstate);
    }
}

void
_PyCrossInterpreterData_Release(_PyCrossInterpreterData *data)
{
    if (data->data == NULL && data->obj == NULL) {
        // Nothing to release!
        return;
    }

    // Switch to the original interpreter.
    PyInterpreterState *interp = _PyInterpreterState_LookUpID(data->interp);
    if (interp == NULL) {
        // The intepreter was already destroyed.
        if (data->free != NULL) {
            // XXX Someone leaked some memory...
        }
        return;
    }

    // "Release" the data and/or the object.
    // XXX Use _Py_AddPendingCall().
    _call_in_interpreter(interp, _release_xidata, data);
}

PyObject *
_PyCrossInterpreterData_NewObject(_PyCrossInterpreterData *data)
{
    return data->new_object(data);
}

/* registry of {type -> crossinterpdatafunc} */

/* For now we use a global registry of shareable classes.  An
   alternative would be to add a tp_* slot for a class's
   crossinterpdatafunc. It would be simpler and more efficient. */

static int
_register_xidata(PyTypeObject *cls, crossinterpdatafunc getdata)
{
    // Note that we effectively replace already registered classes
    // rather than failing.
    struct _xidregitem *newhead = PyMem_RawMalloc(sizeof(struct _xidregitem));
    if (newhead == NULL)
        return -1;
    newhead->cls = cls;
    newhead->getdata = getdata;
    newhead->next = _PyRuntime.xidregistry.head;
    _PyRuntime.xidregistry.head = newhead;
    return 0;
}

static void _register_builtins_for_crossinterpreter_data(void);

int
_PyCrossInterpreterData_Register_Class(PyTypeObject *cls,
                                       crossinterpdatafunc getdata)
{
    if (!PyType_Check(cls)) {
        PyErr_Format(PyExc_ValueError, "only classes may be registered");
        return -1;
    }
    if (getdata == NULL) {
        PyErr_Format(PyExc_ValueError, "missing 'getdata' func");
        return -1;
    }

    // Make sure the class isn't ever deallocated.
    Py_INCREF((PyObject *)cls);

    PyThread_acquire_lock(_PyRuntime.xidregistry.mutex, WAIT_LOCK);
    if (_PyRuntime.xidregistry.head == NULL) {
        _register_builtins_for_crossinterpreter_data();
    }
    int res = _register_xidata(cls, getdata);
    PyThread_release_lock(_PyRuntime.xidregistry.mutex);
    return res;
}

/* Cross-interpreter objects are looked up by exact match on the class.
   We can reassess this policy when we move from a global registry to a
   tp_* slot. */

crossinterpdatafunc
_PyCrossInterpreterData_Lookup(PyObject *obj)
{
    PyObject *cls = PyObject_Type(obj);
    crossinterpdatafunc getdata = NULL;
    PyThread_acquire_lock(_PyRuntime.xidregistry.mutex, WAIT_LOCK);
    struct _xidregitem *cur = _PyRuntime.xidregistry.head;
    if (cur == NULL) {
        _register_builtins_for_crossinterpreter_data();
        cur = _PyRuntime.xidregistry.head;
    }
    for(; cur != NULL; cur = cur->next) {
        if (cur->cls == (PyTypeObject *)cls) {
            getdata = cur->getdata;
            break;
        }
    }
    Py_DECREF(cls);
    PyThread_release_lock(_PyRuntime.xidregistry.mutex);
    return getdata;
}

/* cross-interpreter data for builtin types */

struct _shared_bytes_data {
    char *bytes;
    Py_ssize_t len;
};

static PyObject *
_new_bytes_object(_PyCrossInterpreterData *data)
{
    struct _shared_bytes_data *shared = (struct _shared_bytes_data *)(data->data);
    return PyBytes_FromStringAndSize(shared->bytes, shared->len);
}

static int
_bytes_shared(PyObject *obj, _PyCrossInterpreterData *data)
{
    struct _shared_bytes_data *shared = PyMem_NEW(struct _shared_bytes_data, 1);
    if (PyBytes_AsStringAndSize(obj, &shared->bytes, &shared->len) < 0) {
        return -1;
    }
    data->data = (void *)shared;
    Py_INCREF(obj);
    data->obj = obj;  // Will be "released" (decref'ed) when data released.
    data->new_object = _new_bytes_object;
    data->free = PyMem_Free;
    return 0;
}

struct _shared_str_data {
    int kind;
    const void *buffer;
    Py_ssize_t len;
};

static PyObject *
_new_str_object(_PyCrossInterpreterData *data)
{
    struct _shared_str_data *shared = (struct _shared_str_data *)(data->data);
    return PyUnicode_FromKindAndData(shared->kind, shared->buffer, shared->len);
}

static int
_str_shared(PyObject *obj, _PyCrossInterpreterData *data)
{
    struct _shared_str_data *shared = PyMem_NEW(struct _shared_str_data, 1);
    shared->kind = PyUnicode_KIND(obj);
    shared->buffer = PyUnicode_DATA(obj);
    shared->len = PyUnicode_GET_LENGTH(obj) - 1;
    data->data = (void *)shared;
    Py_INCREF(obj);
    data->obj = obj;  // Will be "released" (decref'ed) when data released.
    data->new_object = _new_str_object;
    data->free = PyMem_Free;
    return 0;
}

static PyObject *
_new_long_object(_PyCrossInterpreterData *data)
{
    return PyLong_FromSsize_t((Py_ssize_t)(data->data));
}

static int
_long_shared(PyObject *obj, _PyCrossInterpreterData *data)
{
    /* Note that this means the size of shareable ints is bounded by
     * sys.maxsize.  Hence on 32-bit architectures that is half the
     * size of maximum shareable ints on 64-bit.
     */
    Py_ssize_t value = PyLong_AsSsize_t(obj);
    if (value == -1 && PyErr_Occurred()) {
        if (PyErr_ExceptionMatches(PyExc_OverflowError)) {
            PyErr_SetString(PyExc_OverflowError, "try sending as bytes");
        }
        return -1;
    }
    data->data = (void *)value;
    data->obj = NULL;
    data->new_object = _new_long_object;
    data->free = NULL;
    return 0;
}

static PyObject *
_new_none_object(_PyCrossInterpreterData *data)
{
    // XXX Singleton refcounts are problematic across interpreters...
    Py_INCREF(Py_None);
    return Py_None;
}

static int
_none_shared(PyObject *obj, _PyCrossInterpreterData *data)
{
    data->data = NULL;
    // data->obj remains NULL
    data->new_object = _new_none_object;
    data->free = NULL;  // There is nothing to free.
    return 0;
}

static void
_register_builtins_for_crossinterpreter_data(void)
{
    // None
    if (_register_xidata((PyTypeObject *)PyObject_Type(Py_None), _none_shared) != 0) {
        Py_FatalError("could not register None for cross-interpreter sharing");
    }

    // int
    if (_register_xidata(&PyLong_Type, _long_shared) != 0) {
        Py_FatalError("could not register int for cross-interpreter sharing");
    }

    // bytes
    if (_register_xidata(&PyBytes_Type, _bytes_shared) != 0) {
        Py_FatalError("could not register bytes for cross-interpreter sharing");
    }

    // str
    if (_register_xidata(&PyUnicode_Type, _str_shared) != 0) {
        Py_FatalError("could not register str for cross-interpreter sharing");
    }
}


#ifdef __cplusplus
}
#endif
