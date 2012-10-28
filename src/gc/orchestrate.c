#include "moarvm.h"

#define GCORCH_DEGUG 1
#define GCORCH_LOG(x) if (GCORCH_DEGUG) printf(x)

/* Does a garbage collection run (not updated for real multi-thread work yet). */
static void run_gc(MVMThreadContext *tc, MVMuint8 process_perms) {
    /* Do a nursery collection. We record the current tospace allocation
     * pointer to serve as a limit for the later sweep phase. */
    void *limit = tc->nursery_alloc;
    MVM_gc_nursery_collect(tc, process_perms);
    MVM_gc_nursery_free_uncopied(tc, limit);
}

/* Goes through all threads but the current one and notifies them that a
 * GC run is starting. Those that are blocked are considered excluded from
 * the run, but still counted. */
static void signal_one_thread(MVMThreadContext *tc) {
    /* Loop here since we may not succeed first time (e.g. the status of the
     * thread may change between the two ways we try to twiddle it). */
    while (1) {
        /* Try to set it from running to interrupted - the common case. */
        if (apr_atomic_cas32(&tc->gc_status, MVMGCStatus_INTERRUPT,
                MVMGCStatus_NONE) == MVMGCStatus_NONE)
            return;
        
        /* Otherwise, it's blocked; try to set it to work stolen. */
        if (apr_atomic_cas32(&tc->gc_status, MVMGCStatus_STOLEN,
                MVMGCStatus_UNABLE) == MVMGCStatus_UNABLE) {
            /* We stole the work; it's now sufficiently opted in to GC that
             * we can increment the count of threads that are opted in. */
            apr_atomic_inc32(&tc->instance->starting_gc);
            GCORCH_LOG("A blocked thread spotted\n");
            return;
        }
    }    
}
static void signal_all_but(MVMThreadContext *tc) {
    MVMInstance *ins = tc->instance;
    MVMuint32 i;
    if (ins->main_thread != tc)
        signal_one_thread(ins->main_thread);
    for (i = 0; i < ins->num_user_threads; i++) {
        MVMThreadContext *target = ins->user_threads[i]->body.tc;
        if (target != tc)
            signal_one_thread(target);
    }
}

/* Waits for all threads to have enlisted in the GC run. For now, just stupid
 * spinning. */
static void wait_for_all_threads(MVMInstance *i) {
    GCORCH_LOG("Waiting for all threads...\n");
    while (i->starting_gc != i->expected_gc_threads)
        1;
    GCORCH_LOG("All threads now registered for the GC run\n");
}

/* Called by a thread to indicate it is about to enter a blocking operation.
 * This gets any thread that is coordinating a GC run that this thread will
 * be unable to participate. */
void MVM_gc_mark_thread_blocked(MVMThreadContext *tc) {
    /* Try to set it from running to unable - the common case. */
    if (apr_atomic_cas32(&tc->gc_status, MVMGCStatus_UNABLE,
            MVMGCStatus_NONE) == MVMGCStatus_NONE)
        return;
    
    /* The only way this can fail is if another thread just decided we're to
     * participate in a GC run. */
    if (tc->gc_status == MVMGCStatus_INTERRUPT)
        MVM_gc_enter_from_interupt(tc);
    else
        MVM_panic(MVM_exitcode_gcorch, "Invalid GC status observed; aborting");
}

/* Called by a thread to indicate it has completed a block operation and is
 * thus able to particpate in a GC run again. Note that this case needs some
 * special handling if it comes out of this mode when a GC run is taking place. */
void MVM_gc_mark_thread_unblocked(MVMThreadContext *tc) {
    /* Try to set it from unable to running. */
    while (apr_atomic_cas32(&tc->gc_status, MVMGCStatus_NONE,
            MVMGCStatus_UNABLE) != MVMGCStatus_UNABLE) {
        /* We can't, presumably because a GC run is going on. We should wait
         * for that to finish before we go on, but without chewing CPU. */
        apr_thread_yield();
    }
}

/* This is called when the allocator finds it has run out of memory and wants
 * to trigger a GC run. In this case, it's possible (probable, really) that it
 * will need to do that triggering, notifying other running threads that the
 * time has come to GC. */
void MVM_gc_enter_from_allocator(MVMThreadContext *tc) {
    MVMuint32 num_gc_threads;
    
    /* Grab the thread starting mutex while we start GC. This is so we
     * can get an accurate and stable number of threads that we expect to
     * join in with the GC. */
     if (apr_thread_mutex_lock(tc->instance->mutex_user_threads) != APR_SUCCESS)
            MVM_panic(MVM_exitcode_gcorch, "Unable to lock user_threads mutex");
    num_gc_threads = tc->instance->num_user_threads + 1;
    
    /* Try to start the GC run. */
    if (apr_atomic_cas32(&tc->instance->expected_gc_threads, num_gc_threads, 0) == 0) {
        /* We are the winner of the GC starting race. This gives us some
         * extra responsibilities as well as doing the usual things.
         * First, increment GC sequence number. */
        GCORCH_LOG("GC thread elected coordinator\n");
        tc->instance->gc_seq_number++;

        /* Count us in to the GC run. */
        apr_atomic_inc32(&tc->instance->starting_gc);

        /* Signal other threads to do a GC run. */
        signal_all_but(tc);
        
        /* Now that we've signalled all threads we expect to join in,
         * we can safely release the thread starting mutex. */
        if (apr_thread_mutex_unlock(tc->instance->mutex_user_threads) != APR_SUCCESS)
            MVM_panic(MVM_exitcode_gcorch, "Unable to unlock user_threads mutex");
        
        /* Wait for all thread to indicate readiness to collect. */
        wait_for_all_threads(tc->instance);
        
        /* Do GC work for this thread. */
        /* XXX Finishing sync not at all handled yet... */
        run_gc(tc, MVMPerms_Yes);
        
        /* Clear the starting and expected GC counters (no other thread need do this). */
        tc->instance->starting_gc = 0;
        tc->instance->expected_gc_threads = 0;
    }
    else {
        /* Another thread beat us to starting the GC sync process. Thus, act as
         * if we were interupted to GC; also release that thread starting mutex
         * that we (in the end needlessly) took. */
        if (apr_thread_mutex_unlock(tc->instance->mutex_user_threads) != APR_SUCCESS)
            MVM_panic(MVM_exitcode_gcorch, "Unable to unlock user_threads mutex");
        MVM_gc_enter_from_interupt(tc);
    }
}

/* This is called when a thread hits an interupt at a GC safe point. This means
 * that another thread is already trying to start a GC run, so we don't need to
 * try and do that, just enlist in the run. */
void MVM_gc_enter_from_interupt(MVMThreadContext *tc) {
    /* Count us in to the GC run. */
    GCORCH_LOG("Entered from interrupt\n");
    apr_atomic_inc32(&tc->instance->starting_gc);

    /* Wait for all thread to indicate readiness to collect. */
    wait_for_all_threads(tc->instance);
    
    /* Do GC work for this thread. */
    /* XXX Finishing sync not at all handled yet... */
    run_gc(tc, MVMPerms_No);
}
