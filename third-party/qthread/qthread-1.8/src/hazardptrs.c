#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/* The API */
#include "qthread/qthread.h"

/* System Headers */
#include <stdlib.h>            /* for qsort() and abort() */

/* Internal Headers */
#include "qt_hazardptrs.h"
#include "qt_shepherd_innards.h"
#include "qthread_innards.h"
#include "qt_debug.h" /* for malloc debug headers */

static TLS_DECL_INIT(uintptr_t *, ts_hazard_ptrs);

static uintptr_t *QTHREAD_CASLOCK(hzptr_list);
static aligned_t hzptr_list_len = 0;
static unsigned  freelist_max   = 0;

static hazard_freelist_entry_t *free_these_freelists = NULL;

static void hazardptr_internal_teardown(void)
{   /*{{{*/
    while (free_these_freelists != NULL) {
        hazard_freelist_entry_t *tmp = free_these_freelists;
        free_these_freelists = (hazard_freelist_entry_t *)tmp[freelist_max].ptr;
        FREE(tmp, (freelist_max + 1) * sizeof(hazard_freelist_entry_t));
    }
    TLS_DELETE(ts_hazard_ptrs);
    while (hzptr_list != NULL) {
        uintptr_t *hzptr_tmp = hzptr_list;
        hzptr_list = (uintptr_t *)hzptr_tmp[HAZARD_PTRS_PER_SHEP];
        FREE(hzptr_tmp, (HAZARD_PTRS_PER_SHEP + 1) * sizeof(uintptr_t));
    }
    QTHREAD_CASLOCK_DESTROY(hzptr_list);
} /*}}}*/

void INTERNAL initialize_hazardptrs(void)
{/*{{{*/
#ifdef QTHREAD_MULTITHREADED_SHEPHERDS
    freelist_max = qthread_num_shepherds() * qlib->nworkerspershep + 7;
    for (qthread_shepherd_id_t i = 0; i < qthread_num_shepherds(); ++i) {
        for (qthread_worker_id_t j = 0; j < qlib->nworkerspershep; ++j) {
            memset(qlib->shepherds[i].workers[j].hazard_ptrs, 0, sizeof(uintptr_t) * HAZARD_PTRS_PER_SHEP);
            memset(&qlib->shepherds[i].workers[j].hazard_free_list, 0, sizeof(hazard_freelist_t));
            qlib->shepherds[i].workers[j].hazard_free_list.freelist = calloc(freelist_max + 1, sizeof(hazard_freelist_entry_t));
            assert(qlib->shepherds[i].workers[j].hazard_free_list.freelist);
            qlib->shepherds[i].workers[j].hazard_free_list.freelist[freelist_max].ptr = free_these_freelists;
            free_these_freelists                                                      = qlib->shepherds[i].workers[j].hazard_free_list.freelist;
        }
    }
#else /* ifdef QTHREAD_MULTITHREADED_SHEPHERDS */
    freelist_max = qthread_num_shepherds() + 7;
    for (qthread_shepherd_id_t i = 0; i < qthread_num_shepherds(); ++i) {
        memset(qlib->shepherds[i].hazard_ptrs, 0, sizeof(uintptr_t) * HAZARD_PTRS_PER_SHEP);
        memset(&qlib->shepherds[i].hazard_free_list, 0, sizeof(hazard_freelist_t));
        qlib->shepherds[i].hazard_free_list.freelist = calloc(freelist_max + 1, sizeof(hazard_freelist_entry_t));
        assert(qlib->shepherds[i].hazard_free_list.freelist);
        qlib->shepherds[i].hazard_free_list.freelist[freelist_max].ptr = free_these_freelists;
        free_these_freelists                                           = qlib->shepherds[i].hazard_free_list.freelist;
    }
#endif /* ifdef QTHREAD_MULTITHREADED_SHEPHERDS */
    TLS_INIT(ts_hazard_ptrs);
    QTHREAD_CASLOCK_INIT(hzptr_list, NULL);
    qthread_internal_cleanup(hazardptr_internal_teardown);
}/*}}}*/

void INTERNAL hazardous_ptr(unsigned int which,
                            void        *ptr)
{/*{{{*/
    uintptr_t *hzptrs = TLS_GET(ts_hazard_ptrs);

    if (hzptrs == NULL) {
#ifdef QTHREAD_MULTITHREADED_SHEPHERDS
        {
            qthread_worker_t *wkr = qthread_internal_getworker();
            if (wkr == NULL) {
                hzptrs = calloc(sizeof(uintptr_t), HAZARD_PTRS_PER_SHEP + 1);
                assert(hzptrs);
                do {
                    hzptrs[HAZARD_PTRS_PER_SHEP] = (uintptr_t)QTHREAD_CASLOCK_READ(hzptr_list);
                } while (QT_CAS(hzptr_list, hzptrs[HAZARD_PTRS_PER_SHEP], hzptrs)
                         != (void *)hzptrs[HAZARD_PTRS_PER_SHEP]);
                (void)qthread_incr(&hzptr_list_len, 1);
            } else {
                hzptrs = wkr->hazard_ptrs;
            }
        }
#else   /* ifdef QTHREAD_MULTITHREADED_SHEPHERDS */
        {
            qthread_shepherd_t *shep = qthread_internal_getshep();
            if (shep == NULL) {
                hzptrs = calloc(sizeof(uintptr_t), HAZARD_PTRS_PER_SHEP + 1);
                assert(hzptrs);
                do {
                    hzptrs[HAZARD_PTRS_PER_SHEP] = (uintptr_t)QTHREAD_CASLOCK_READ(hzptr_list);
                } while (QT_CAS(hzptr_list, hzptrs[HAZARD_PTRS_PER_SHEP], hzptrs)
                         != (void *)hzptrs[HAZARD_PTRS_PER_SHEP]);
                (void)qthread_incr(&hzptr_list_len, 1);
            } else {
                hzptrs = shep->hazard_ptrs;
            }
        }
#endif  /* ifdef QTHREAD_MULTITHREADED_SHEPHERDS */
        TLS_SET(ts_hazard_ptrs, hzptrs);
    }

    assert(hzptrs);
    assert(which < HAZARD_PTRS_PER_SHEP);
    hzptrs[which] = (uintptr_t)ptr;
}/*}}}*/

static int void_cmp(const void *a,
                    const void *b)
{/*{{{*/
    return (*(intptr_t *)a) - (*(intptr_t *)b);
}/*}}}*/

static int binary_search(uintptr_t *list,
                         uintptr_t  findme,
                         size_t     len)
{/*{{{*/
    size_t max  = len;
    size_t min  = 0;
    size_t curs = max / 2;

    while (list[curs] != findme) {
        if (list[curs] > findme) {
            max = curs;
        } else if (list[curs] < findme) {
            min = curs;
        }
        if (max == min + 1) { break; }
        curs = (max + min) / 2;
    }
    return (list[curs] == findme);
}/*}}}*/

static void hazardous_scan(hazard_freelist_t *hfl)
{/*{{{*/
#ifdef QTHREAD_MULTITHREADED_SHEPHERDS
    const size_t num_hps = qthread_num_workers() * HAZARD_PTRS_PER_SHEP;
#else
    const size_t num_hps = qthread_num_shepherds() * HAZARD_PTRS_PER_SHEP;
#endif
    void            **plist = MALLOC(sizeof(void *) * (num_hps + hzptr_list_len));
    hazard_freelist_t tmpfreelist;

    assert(plist);
    tmpfreelist.freelist = calloc(freelist_max, sizeof(hazard_freelist_entry_t));
    assert(tmpfreelist.freelist);
    do {
        /* Stage 1: Collect hazardpointers */
#ifdef QTHREAD_MULTITHREADED_SHEPHERDS
        {
            qthread_shepherd_id_t i;
            for (i = 0; i < qthread_num_shepherds(); ++i) {
                for (qthread_worker_id_t j = 0; j < qlib->nworkerspershep; ++j) {
                    if (&(qlib->shepherds[i].workers[j].hazard_free_list) != hfl) {
                        memcpy(plist + (i * qlib->nworkerspershep * HAZARD_PTRS_PER_SHEP) + (j * HAZARD_PTRS_PER_SHEP),
                               qlib->shepherds[i].workers[j].hazard_ptrs,
                               sizeof(void *) * HAZARD_PTRS_PER_SHEP);
                    } else {
                        memset(plist + (i * qlib->nworkerspershep * HAZARD_PTRS_PER_SHEP) + (j * HAZARD_PTRS_PER_SHEP),
                               0,
                               sizeof(void *) * HAZARD_PTRS_PER_SHEP);
                    }
                }
            }
            uintptr_t *hzptr_tmp = QTHREAD_CASLOCK_READ(hzptr_list);
            while (hzptr_tmp != NULL) {
                memcpy(plist + (i * qlib->nworkerspershep * HAZARD_PTRS_PER_SHEP),
                       hzptr_tmp,
                       sizeof(uintptr_t) * HAZARD_PTRS_PER_SHEP);
                hzptr_tmp = (uintptr_t *)hzptr_tmp[HAZARD_PTRS_PER_SHEP];
            }
        }
#else   /* ifdef QTHREAD_MULTITHREADED_SHEPHERDS */
        qthread_shepherd_id_t i;
        for (i = 0; i < qthread_num_shepherds(); ++i) {
            if (&(qlib->shepherds[i].hazard_free_list) != hfl) {
                memcpy(plist + (i * HAZARD_PTRS_PER_SHEP), qlib->shepherds[i].hazard_ptrs, sizeof(void *) * HAZARD_PTRS_PER_SHEP);
            } else {
                memset(plist + (i * HAZARD_PTRS_PER_SHEP), 0, sizeof(void *) * HAZARD_PTRS_PER_SHEP);
            }
        }
        {
            uintptr_t *hzptr_tmp = QTHREAD_CASLOCK_READ(hzptr_list);
            while (hzptr_tmp != NULL) {
                memcpy(plist + (i * HAZARD_PTRS_PER_SHEP),
                       hzptr_tmp,
                       sizeof(uintptr_t) * HAZARD_PTRS_PER_SHEP);
                hzptr_tmp = (uintptr_t *)hzptr_tmp[HAZARD_PTRS_PER_SHEP];
            }
        }
#endif  /* ifdef QTHREAD_MULTITHREADED_SHEPHERDS */

        /* Stage 2: free pointers that are not in the set of hazardous pointers */
        tmpfreelist.count = 0;
        qsort(plist, num_hps, sizeof(void *), void_cmp);
        assert(hfl->count == freelist_max);
        for (size_t i = 0; i < freelist_max; ++i) {
            const uintptr_t ptr = (uintptr_t)hfl->freelist[i].ptr;
            if (ptr == 0) { break; }
            /* look for this ptr in the plist */
            if (binary_search((uintptr_t *)plist, ptr, num_hps)) {
                /* if found, cannot free it */
                tmpfreelist.freelist[tmpfreelist.count] = hfl->freelist[i];
                tmpfreelist.count++;
            } else {
                /* not found, therefore, we can free it */
                hfl->freelist[i].free((void *)ptr);
            }
        }
        if (tmpfreelist.count == freelist_max) {
            /* This will ONLY happen under *extremely* heavy contention. */
            MACHINE_FENCE;
        }
    } while (tmpfreelist.count == freelist_max);
    assert(tmpfreelist.count < freelist_max);
    memcpy(hfl->freelist, tmpfreelist.freelist, tmpfreelist.count * sizeof(hazard_freelist_entry_t));
    hfl->count = tmpfreelist.count;
    FREE(tmpfreelist.freelist, sizeof(hazard_freelist_entry_t));
    FREE(plist, sizeof(void *) * (num_hps + hzptr_list_len));
}/*}}}*/

void INTERNAL hazardous_release_node(void  (*freefunc)(void *),
                                     void *ptr)
{/*{{{*/
#ifdef QTHREAD_MULTITHREADED_SHEPHERDS
    hazard_freelist_t *hfl = &(qthread_internal_getworker()->hazard_free_list);
#else
    hazard_freelist_t *hfl = &(qthread_internal_getshep()->hazard_free_list);
#endif
    uintptr_t *hzptrs = TLS_GET(ts_hazard_ptrs);

    assert(ptr != NULL);
    assert(freefunc != NULL);
    assert(hfl->count < freelist_max);
    hfl->freelist[hfl->count].free = freefunc;
    hfl->freelist[hfl->count].ptr  = ptr;
    hfl->count++;
    if (hzptrs != NULL) {
        memset(hzptrs, 0, sizeof(uintptr_t) * HAZARD_PTRS_PER_SHEP);
    }
    if (hfl->count == freelist_max) {
        hazardous_scan(hfl);
    }
}/*}}}*/

/* vim:set expandtab: */
