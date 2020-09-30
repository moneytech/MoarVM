#include "moar.h"

#define INDEX_MIN_SIZE_BASE_2 3

MVM_STATIC_INLINE void hash_demolish_internal(MVMThreadContext *tc,
                                              struct MVMIndexHashTableControl *control) {
    size_t allocated_items = MVM_index_hash_allocated_items(control);
    size_t entries_size = sizeof(struct MVMIndexHashEntry) * allocated_items;
    char *start = (char *)control - entries_size;
    MVM_free(start);
}

/* Frees the entire contents of the hash, leaving you just the hashtable itself,
   which you allocated (heap, stack, inside another struct, wherever) */
void MVM_index_hash_demolish(MVMThreadContext *tc, MVMIndexHashTable *hashtable) {
    struct MVMIndexHashTableControl *control = hashtable->table;
    if (!control)
        return;
    hash_demolish_internal(tc, control);
    hashtable->table = NULL;
}
/* and then free memory if you allocated it */


MVM_STATIC_INLINE struct MVMIndexHashTableControl *hash_allocate_common(MVMThreadContext *tc,
                                                                        MVMuint8 key_right_shift,
                                                                        MVMuint8 official_size_log2) {
    MVMuint32 official_size = 1 << (MVMuint32)official_size_log2;
    MVMuint32 max_items = official_size * MVM_INDEX_HASH_LOAD_FACTOR;
    /* -1 because...
     * probe distance of 1 is the correct bucket.
     * hence for a value whose ideal slot is the last bucket, it's *in* the
     * official allocation.
     * probe distance of 2 is the first extra bucket beyond the official
     * allocation
     * probe distance of 255 is the 254th beyond the official allocation.
     */
    MVMuint8 max_probe_distance_limit;
    if ((MVM_HASH_MAX_PROBE_DISTANCE - 1) < (max_items - 1)) {
        max_probe_distance_limit = MVM_HASH_MAX_PROBE_DISTANCE - 1;
    } else {
        max_probe_distance_limit = max_items - 1;
    }
    size_t allocated_items = official_size + max_probe_distance_limit;
    size_t entries_size = sizeof(struct MVMIndexHashEntry) * allocated_items;
    size_t metadata_size = MVM_hash_round_size_up(allocated_items + 1);
    size_t total_size
        = entries_size + sizeof(struct MVMIndexHashTableControl) + metadata_size;

    struct MVMIndexHashTableControl *control =
        (struct MVMIndexHashTableControl *) ((char *)MVM_malloc(total_size) + entries_size);

    control->official_size_log2 = official_size_log2;
    control->max_items = max_items;
    control->cur_items = 0;
    control->max_probe_distance = max_probe_distance_limit;
    control->max_probe_distance_limit = max_probe_distance_limit;
    control->key_right_shift = key_right_shift;

    MVMuint8 *metadata = (MVMuint8 *)(control + 1);
    memset(metadata, 0, metadata_size);

    /* A sentinel. This marks an occupied slot, at its ideal position. */
    metadata[allocated_items] = 1;

    return control;
}

void MVM_index_hash_build(MVMThreadContext *tc,
                          MVMIndexHashTable *hashtable,
                          MVMuint32 entries) {
    MVMuint32 initial_size_base2;
    if (!entries) {
        initial_size_base2 = INDEX_MIN_SIZE_BASE_2;
    } else {
        /* Minimum size we need to allocate, given the load factor. */
        MVMuint32 min_needed = entries * (1.0 / MVM_INDEX_HASH_LOAD_FACTOR);
        initial_size_base2 = MVM_round_up_log_base2(min_needed);
        if (initial_size_base2 < INDEX_MIN_SIZE_BASE_2) {
            /* "Too small" - use our original defaults. */
            initial_size_base2 = INDEX_MIN_SIZE_BASE_2;
        }
    }

    hashtable->table = hash_allocate_common(tc,
                                            (8 * sizeof(MVMuint64) - initial_size_base2),
                                            initial_size_base2);
}

MVM_STATIC_INLINE void hash_insert_internal(MVMThreadContext *tc,
                                            struct MVMIndexHashTableControl *control,
                                            MVMString **list,
                                            MVMuint32 idx) {
    if (MVM_UNLIKELY(control->cur_items >= control->max_items)) {
        MVM_oops(tc, "oops, attempt to recursively call grow when adding %i",
                 idx);
    }

    struct MVM_hash_loop_state ls = MVM_index_hash_create_loop_state(tc, control, list[idx]);

    while (1) {
        if (*ls.metadata < ls.probe_distance) {
            /* this is our slot. occupied or not, it is our rightful place. */

            if (*ls.metadata == 0) {
                /* Open goal. Score! */
            } else {
                /* make room. */

                /* Optimisation first seen in Martin Ankerl's implementation -
                   we don't need actually implement the "stealing" by swapping
                   elements and carrying on with insert. The invariant of the
                   hash is that probe distances are never out of order, and as
                   all the following elements have probe distances in order, we
                   can maintain the invariant just as well by moving everything
                   along by one. */
                MVMuint8 *find_me_a_gap = ls.metadata;
                MVMuint8 old_probe_distance = *ls.metadata;
                do {
                    MVMuint8 new_probe_distance = 1 + old_probe_distance;
                    if (new_probe_distance == control->max_probe_distance) {
                        /* Optimisation from Martin Ankerl's implementation:
                           setting this to zero forces a resize on any insert,
                           *before* the actual insert, so that we never end up
                           having to handle overflow *during* this loop. This
                           loop can always complete. */
                        control->max_items = 0;
                    }
                    /* a swap: */
                    old_probe_distance = *++find_me_a_gap;
                    *find_me_a_gap = new_probe_distance;
                } while (old_probe_distance);

                MVMuint32 entries_to_move = find_me_a_gap - ls.metadata;
                size_t size_to_move = ls.entry_size * entries_to_move;
                /* When we had entries *ascending* this was
                 * memmove(entry_raw + sizeof(struct MVMIndexHashEntry), entry_raw,
                 *         sizeof(struct MVMIndexHashEntry) * entries_to_move);
                 * because we point to the *start* of the block of memory we
                 * want to move, and we want to move it one "entry" forwards.
                 * `entry_raw` is still a pointer to where we want to make free
                 * space, but what want to do now is move everything at it and
                 * *before* it downwards. */
                MVMuint8 *dest = ls.entry_raw - size_to_move;
                memmove(dest, dest + ls.entry_size, size_to_move);
            }

            /* The same test and optimisation as in the "make room" loop - we're
             * about to insert something at the (current) max_probe_distance, so
             * signal to the next insertion that it needs to take action first.
             */
            if (ls.probe_distance == control->max_probe_distance) {
                control->max_items = 0;
            }

            ++control->cur_items;

            *ls.metadata = ls.probe_distance;
            struct MVMIndexHashEntry *entry = (struct MVMIndexHashEntry *) ls.entry_raw;
            entry->index = idx;

            return;
        }

        if (*ls.metadata == ls.probe_distance) {
            struct MVMIndexHashEntry *entry = (struct MVMIndexHashEntry *) ls.entry_raw;
            if (entry->index == idx) {
                /* definately XXX - what should we do here? */
                MVM_oops(tc, "insert duplicate for %u", idx);
            }
        }
        ++ls.probe_distance;
        ++ls.metadata;
        ls.entry_raw -= ls.entry_size;
        assert(ls.probe_distance <= (unsigned int) control->max_probe_distance + 1);
        assert(ls.metadata < MVM_index_hash_metadata(control) + MVM_index_hash_official_size(control) + control->max_items);
        assert(ls.metadata < MVM_index_hash_metadata(control) + MVM_index_hash_official_size(control) + 256);
    }
}

static struct MVMIndexHashTableControl *maybe_grow_hash(MVMThreadContext *tc,
                                                        struct MVMIndexHashTableControl *control,
                                                        MVMString **list) {
    MVMuint32 entries_in_use =  MVM_index_hash_kompromat(control);
    MVMuint8 *entry_raw_orig = MVM_index_hash_entries(control);
    MVMuint8 *metadata_orig = MVM_index_hash_metadata(control);

    struct MVMIndexHashTableControl *control_orig = control;

    control = hash_allocate_common(tc,
                                   control_orig->key_right_shift - 1,
                                   control_orig->official_size_log2 + 1);

    MVMuint8 *entry_raw = entry_raw_orig;
    MVMuint8 *metadata = metadata_orig;
    MVMHashNumItems bucket = 0;
    while (bucket < entries_in_use) {
        if (*metadata) {
            struct MVMIndexHashEntry *entry = (struct MVMIndexHashEntry *) entry_raw;
            hash_insert_internal(tc, control, list, entry->index);
        }
        ++bucket;
        ++metadata;
        entry_raw -= sizeof(struct MVMIndexHashEntry);
    }
    hash_demolish_internal(tc, control_orig);
    return control;
}

/* UNCONDITIONALLY creates a new hash entry with the given key and value.
 * Doesn't check if the key already exists. Use with care. */
void MVM_index_hash_insert_nocheck(MVMThreadContext *tc,
                                   MVMIndexHashTable *hashtable,
                                   MVMString **list,
                                   MVMuint32 idx) {
    struct MVMIndexHashTableControl *control = hashtable->table;
    assert(control);
    assert(MVM_index_hash_entries(control) != NULL);
    if (MVM_UNLIKELY(control->cur_items >= control->max_items)) {
        struct MVMIndexHashTableControl *new_control = maybe_grow_hash(tc, control, list);
        if (new_control) {
            /* We could unconditionally assign this, but that would mean CPU
             * cache writes even when it was unchanged, and the address of
             * hashtable will not be in the same cache lines as we are writing
             * for the hash internals, so it will churn the write cache. */
            hashtable->table = control = new_control;
        }
    }
    return hash_insert_internal(tc, control, list, idx);
}
