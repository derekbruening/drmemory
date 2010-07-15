/* **********************************************************
 * Copyright (c) 2008-2009 VMware, Inc.  All rights reserved.
 * **********************************************************/

/* Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; 
 * version 2.1 of the License, and no later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "dr_api.h"
#include "drmemory.h"
#include "readwrite.h"
#include "report.h"
#include "shadow.h"
#include "syscall.h"
#include "alloc.h"
#include "heap.h"
#include "redblack.h"
#include "leak.h"
#ifdef LINUX
# include "sysnum_linux.h"
# include <signal.h>
#else
# include "stack.h"
#endif

/* PR 465174: share allocation site callstacks.
 * This table should only be accessed while holding the lock for
 * malloc_table (via malloc_lock()), which makes the coordinated
 * operations with malloc_table atomic.
 */
#define ASTACK_TABLE_HASH_BITS 8
static hashtable_t alloc_stack_table;

#ifdef LINUX
/* Track all signal handlers registered by app so we can instrument them */
#define SIGHAND_HASH_BITS 6
hashtable_t sighand_table;

/* PR 418629: to determine stack bounds accurately we track anon mmaps */
static rb_tree_t *mmap_tree;
static void *mmap_tree_lock; /* maybe rbtree should support internal synch */
#endif

#ifdef STATISTICS
uint alloc_stack_count;
#endif

/***************************************************************************
 * DELAYED-FREE LIST
 */

/* A FIFO implemented by an array since we have a fixed size equal
 * to options.delay_frees.
 * We store the address that should be passed to free() (i.e., it
 * includes the redzone).
 */
typedef struct _delay_free_t {
    app_pc addr;
#ifdef WINDOWS
    /* We assume the only flag even at Rtl level is HEAP_NO_SERIALIZE so we only have
     * to record the Heap (xref PR 502150)
     */
    app_pc heap;
#endif
#ifdef STATISTICS
    size_t size;
#endif
} delay_free_t;
static delay_free_t *delay_free_list;
/* We could do per-thread free lists but could strand frees in idle threads;
 * plus, already impacting performance plenty so global synch ok.
 */
static void *delay_free_lock;
/* Head of FIFO array */
static int delay_free_head;
/* If FIFO is full, equals options.delay_frees; else, equals
 * one past the furthest index that has been filled.
 */
static int delay_free_fill;

/* Interval tree for looking up whether an address is on the list (PR 535568) */
static rb_tree_t *delay_free_tree;

#define DELAY_FREE_FULL() (delay_free_fill == options.delay_frees)

#ifdef STATISTICS
uint delayed_free_bytes;
#endif

/***************************************************************************/

static void
alloc_callstack_free(void *p);

static byte *
next_defined_dword(byte *start, byte *end);

static byte *
end_of_defined_region(byte *start, byte *end);

static bool
is_register_defined(void *drcontext, reg_id_t reg);

void
alloc_drmem_init(void)
{
    alloc_init(options.track_heap,
               options.redzone_size,
               options.size_in_redzone,
               true, /* record allocs: used to only need for -count_leaks */
               false/*don't need padding size*/);

    hashtable_init_ex(&alloc_stack_table, ASTACK_TABLE_HASH_BITS, HASH_CUSTOM,
                      false/*!str_dup*/, true/* synch (higher-level synch covered
                                              * by malloc_table's lock) */,
                      alloc_callstack_free,
                      (uint (*)(void*)) packed_callstack_hash,
                      (bool (*)(void*, void*)) packed_callstack_cmp);

#ifdef LINUX
    hashtable_init(&sighand_table, SIGHAND_HASH_BITS, HASH_INTPTR, false/*!strdup*/);
    mmap_tree = rb_tree_create(NULL);
    mmap_tree_lock = dr_mutex_create();
#endif

    leak_init(!options.leaks_only,
              options.check_leaks_on_destroy,
              options.midchunk_new_ok,
              options.midchunk_inheritance_ok,
              options.midchunk_string_ok,
              options.midchunk_size_ok,
              next_defined_dword,
              end_of_defined_region,
              is_register_defined);

    if (options.delay_frees > 0) {
        delay_free_lock = dr_mutex_create();
        delay_free_list = (delay_free_t *)
            global_alloc(options.delay_frees * sizeof(*delay_free_list), HEAPSTAT_MISC);
        delay_free_head = 0;
        delay_free_fill = 0;
        delay_free_tree = rb_tree_create(NULL);
    }
}

void
alloc_drmem_exit(void)
{
    alloc_exit(); /* must be before deleting alloc_stack_table */
    LOG(1, "final alloc stack table size: %u bits, %u entries\n",
        alloc_stack_table.table_bits, alloc_stack_table.entries);
    hashtable_delete(&alloc_stack_table);
#ifdef LINUX
    hashtable_delete(&sighand_table);
    rb_tree_destroy(mmap_tree);
    dr_mutex_destroy(mmap_tree_lock);
#endif
    if (options.delay_frees > 0) {
        global_free(delay_free_list, options.delay_frees * sizeof(*delay_free_list),
                    HEAPSTAT_MISC);
        rb_tree_destroy(delay_free_tree);
        dr_mutex_destroy(delay_free_lock);
    }
}

/***************************************************************************
 * MMAP TABLE
 *
 * PR 418629: to determine stack bounds accurately we track mmaps
 */

#ifdef LINUX
static void
mmap_tree_add(byte *base, size_t size)
{
    dr_mutex_lock(mmap_tree_lock);
    rb_node_t *node = rb_insert(mmap_tree, base, size, NULL);
    if (node != NULL) {
        /* merge overlap */
        app_pc merge_base, merge_end;
        size_t merge_size;
        rb_node_fields(node, &merge_base, &merge_size, NULL);
        rb_delete(mmap_tree, node);
        merge_end = (base + size > merge_base + merge_size) ?
            base + size : merge_base + merge_size;
        merge_base = (base < merge_base) ? base : merge_base;
        LOG(2, "mmap add: merged "PFX"-"PFX" with existing => "PFX"-"PFX"\n",
            base, base+size, merge_base, merge_end);
        node = rb_insert(mmap_tree, merge_base, merge_end - merge_base, NULL);
        ASSERT(node != NULL, "mmap tree error");
    }
    dr_mutex_unlock(mmap_tree_lock);
}

static bool
mmap_tree_remove(byte *base, size_t size)
{
    dr_mutex_lock(mmap_tree_lock);
    bool res = false;
    rb_node_t *node = rb_overlaps_node(mmap_tree, base, base+size);
    /* we don't know whether anon or not so ok to not be there */
    while (node != NULL) {
        /* FIXME: should we create a general data struct for interval tree that
         * does not merge adjacent, but handles removing or adding subsets/overlaps?
         * Getting similar to vm_areas, heap.c => PR 210669 as Extension for clients
         * to use too?
         */
        app_pc node_base;
        size_t node_size;
        rb_node_fields(node, &node_base, &node_size, NULL);
        rb_delete(mmap_tree, node);
        if (node_base < base) {
            node = rb_insert(mmap_tree, node_base, base - node_base, NULL);
            ASSERT(node == NULL, "mmap tree error");
        }
        if (node_base + node_size > base + size) {
            node = rb_insert(mmap_tree, base + size, (node_base + node_size) -
                             (base + size), NULL);
            ASSERT(node == NULL, "mmap tree error");
        }
        res = true;
        /* handle overlapping multiple regions */
        node = rb_overlaps_node(mmap_tree, base, base+size);
    }
    dr_mutex_unlock(mmap_tree_lock);
    return res;
}

bool
mmap_anon_lookup(byte *addr, byte **start OUT, size_t *size OUT)
{
    dr_mutex_lock(mmap_tree_lock);
    bool res = false;
    rb_node_t *node = rb_in_node(mmap_tree, addr);
    if (node != NULL) {
        rb_node_fields(node, start, size, NULL);
        res = true;
    }
    dr_mutex_unlock(mmap_tree_lock);
    return res;
}
#endif

/***************************************************************************
 * EVENTS FOR COMMON/ALLOC.C
 */

void
alloc_callstack_free(void *p)
{
    packed_callstack_t *pcs = (packed_callstack_t *) p;
    packed_callstack_free(pcs);
}

void
client_malloc_data_free(void *data)
{
    packed_callstack_t *pcs = (packed_callstack_t *) data;
    uint count;
    ASSERT(pcs != NULL, "malloc data must exist");
    count = packed_callstack_free(pcs);
    ASSERT(count != 0, "refcount should not hit 0 in malloc_table");
    if (count == 1) {
        /* One ref left, which must be the alloc_stack_table.
         * packed_callstack_free will be called by hashtable_remove
         * to dec refcount to 0 and do the actual free.
         */
        hashtable_remove(&alloc_stack_table, (void *)pcs);
    }
}

void *
client_add_malloc_pre(app_pc start, app_pc end, app_pc real_end,
                      void *existing_data, dr_mcontext_t *mc, app_pc post_call)
{
    packed_callstack_t *pcs;
    if (existing_data != NULL)
        pcs = (packed_callstack_t *) existing_data;
    else {
        app_loc_t loc;
        pc_to_loc(&loc, post_call);
        packed_callstack_record(&pcs, mc, &loc);
    }
    /* add returns false if already there */
    if (hashtable_add(&alloc_stack_table, (void *)pcs, (void *)pcs)) {
        DOLOG(2, {
            LOG(2, "@@@ unique callstack #%d\n", alloc_stack_count);
            packed_callstack_log(pcs, INVALID_FILE);
        });
        STATS_INC(alloc_stack_count);
    } else {
        uint count;
        packed_callstack_t *existing = hashtable_lookup(&alloc_stack_table, (void *)pcs);
        ASSERT(existing != NULL, "callstack must exist");
        if (existing_data == NULL) {    /* PR 533755 */
            count = packed_callstack_free(pcs);
            ASSERT(count == 0, "refcount should be 0");
        }
        else
            ASSERT(pcs == existing, "invalid params");
        pcs = existing;
    }
    /* The alloc_stack_table is one reference, and the others are all
     * in the malloc_table.  Once all malloc_table entries are gone
     * and the refcount hits 1 we remove from alloc_stack_table.
     */
    packed_callstack_add_ref(pcs);
    return (void *) pcs;
}

void
client_add_malloc_post(app_pc start, app_pc end, app_pc real_end, void *data)
{
    /* nothing to do */
}

void
client_remove_malloc_pre(app_pc start, app_pc end, app_pc real_end, void *data)
{
    /* nothing to do: client_malloc_data_free() does the work */
}

void
client_remove_malloc_post(app_pc start, app_pc end, app_pc real_end)
{
    /* nothing to do */
}

void
client_invalid_heap_arg(app_pc pc, app_pc target, dr_mcontext_t *mc, const char *routine)
{
    app_loc_t loc;
    pc_to_loc(&loc, pc);
    report_invalid_heap_arg(&loc, target, mc, routine);
}

void
client_handle_malloc(per_thread_t *pt, app_pc base, size_t size,
                     app_pc real_base, bool zeroed, bool realloc, dr_mcontext_t *mc)
{
    /* For calloc via malloc, post-malloc marks as undefined, and we should
     * see the memset which should then mark as defined.
     * But when calloc allocates memory itself, the memset happens
     * while the memory is still unaddressable, and those writes are
     * suppressed => zeroed should be true and we mark as defined here.
     * Plus, for calloc via mmap it's simpler to not have the mmap handler
     * mark as defined and to leave as unaddressable and to mark as
     * defined here (xref PR 531619).
     */
    if (!options.leaks_only && options.shadowing) {
        uint val = zeroed ? SHADOW_DEFINED : SHADOW_UNDEFINED;
        shadow_set_range(base, base + size, val);
    }
    report_malloc(base, base + size,
                  realloc ? "realloc" : "malloc", mc);
    leak_handle_alloc(pt, base, size);
}

void
client_handle_realloc(per_thread_t *pt, app_pc old_base, size_t old_size,
                      app_pc new_base, size_t new_size, app_pc new_real_base,
                      dr_mcontext_t *mc)
{
    /* FIXME: racy: old region could have been malloc'd again by now! 
     * We should synchronize all malloc/free calls w/ our own locks.
     * The real routines have locks already, so shouldn't be any
     * perf impact.
     */
    /* FIXME PR 493888: realloc-freed memory not delayed with rest of
     * delayed free queue!
     */
    /* Copy over old allocation's shadow values.  If new region is bigger, mark
     * the extra space at the end as undefined.  PR 486049.
     */ 
    if (!options.leaks_only && options.shadowing) {
        if (new_size > old_size) {
            shadow_copy_range(old_base, new_base, old_size);
            shadow_set_range(new_base + old_size, new_base + new_size,
                             SHADOW_UNDEFINED);
        } else
            shadow_copy_range(old_base, new_base, new_size);
        
        /* If the new region is after the old region, overlap or not, compute how 
         * much of the front of the old region needs to be marked unaddressable
         * and do so.  This can include the whole old region.
         */
        if (new_base > old_base) {
            shadow_set_range(old_base,
                             /* it can overlap */
                             (new_base < old_base+old_size) ?
                             new_base : old_base+old_size,
                             SHADOW_UNADDRESSABLE);
        }
        
        /* If the new region is before the old region, overlap or not, compute how 
         * much of the end of the old region needs to be marked unaddressable
         * and do so.  This can include the whole old region.  PR 486049.
         * Note: this 'if' can't be an else of the above 'if' because there is a
         *       case where the new region is fully subsumed by the old one.
         */
        if (new_base + new_size < old_base + old_size) {
            app_pc start;
            if (new_base + new_size < old_base)     /* no overlap between regions */
                start = old_base;
            else                                    /* old & new regions overlap */
                start = new_base + new_size;
            shadow_set_range(start, old_base + old_size, SHADOW_UNADDRESSABLE);
        }
    }
    report_malloc(old_base, old_base+old_size, "realloc-old", mc);
    report_malloc(new_base, new_base+new_size, "realloc-new", mc);
    leak_handle_alloc(pt, new_base, new_size);
}

void
client_handle_alloc_failure(size_t sz, bool zeroed, bool realloc,
                            app_pc pc, dr_mcontext_t *mc)
{
    app_loc_t loc;
    pc_to_loc(&loc, pc);
#ifdef LINUX
    LOG(1, "heap allocation failed on sz="PIFX"!  heap="PFX"-"PFX"\n",
        sz, heap_start, get_brk());
# ifdef STATISTICS
    LOG(1, "\tdelayed=%u\n",  delayed_free_bytes);
    /* FIXME: if delayed frees really are a problem, should we free
     * them all here and re-try the malloc?
     */
# endif
#endif
    report_warning(&loc, mc, "heap allocation failed");
}

void
client_handle_realloc_null(app_pc pc, dr_mcontext_t *mc)
{
    /* realloc with NULL is guaranteed to be properly handled,
     * but we report a warning in case unintentional by the app.
     * Windows note: if using libc, at least for msvcr80.dll,
     * libc redirects realloc(NULL,) to malloc() so the realloc
     * does not show up at the Rtl level that we monitor.
     */
    if (options.warn_null_ptr) {
        app_loc_t loc;
        pc_to_loc(&loc, pc);
        report_warning(&loc, mc, "realloc() called with NULL pointer");
    }
}

/* Returns the value to pass to free().  Return "real_base" for no change.
 * The Windows heap param is INOUT so it can be changed as well.
 */
app_pc
client_handle_free(app_pc base, size_t size, app_pc real_base, dr_mcontext_t *mc
                   _IF_WINDOWS(app_pc *heap INOUT))
{
    report_malloc(base, base+size, "free", mc);

    if (!options.leaks_only && options.shadowing)
        shadow_set_range(base, base+size, SHADOW_UNADDRESSABLE);

    if (!options.leaks_only && options.shadowing && options.delay_frees > 0) {
        /* PR 406762: delay frees to catch more errors.  We put
         * this to-be-freed memory in a delay FIFO and leave it as
         * unaddressable.  One the FIFO fills up we substitute the
         * oldest free for this one.
         * We don't bother to free the FIFO entries at exit time; we
         * simply exclude from our leak report.
         */
        app_pc pass_to_free;
        size_t real_size;
        dr_mutex_lock(delay_free_lock);
        /* Store real base and real size: i.e., including redzones (PR 572716) */
        if (base != real_base) {
            ASSERT(base - real_base == options.redzone_size, "redzone mismatch");
            real_size = size + 2*options.redzone_size;
        } else {
            /* A pre-us alloc w/ no redzone */
            real_size = size;
        }
        rb_insert(delay_free_tree, real_base, real_size, (void *)(base == real_base));
        if (DELAY_FREE_FULL()) {
#ifdef WINDOWS
            app_pc pass_heap = delay_free_list[delay_free_head].heap;
#endif
            pass_to_free = delay_free_list[delay_free_head].addr;
            STATS_ADD(delayed_free_bytes, -(int)delay_free_list[delay_free_head].size);
            LOG(2, "delayed free queue full: freeing "PFX
                IF_WINDOWS(" heap="PFX) "\n", pass_to_free _IF_WINDOWS(pass_heap));
            delay_free_list[delay_free_head].addr = real_base;
#ifdef WINDOWS
            /* should we be doing safe_read() and safe_write()? */
            delay_free_list[delay_free_head].heap = *heap;
            *heap = pass_heap;
#endif
#ifdef STATISTICS
            delay_free_list[delay_free_head].size = size;
            STATS_ADD(delayed_free_bytes, size);
#endif
            delay_free_head++;
            if (delay_free_head >= options.delay_frees)
                delay_free_head = 0;
        } else {
            LOG(2, "delayed free queue not full: delaying %d-th free of "PFX
                IF_WINDOWS(" heap="PFX) "\n",
                delay_free_fill, real_base _IF_WINDOWS(*heap));
            ASSERT(delay_free_fill <= options.delay_frees - 1, "internal error");
            delay_free_list[delay_free_fill].addr = real_base;
#ifdef WINDOWS
            /* should we be doing safe_read() and safe_write()? */
            delay_free_list[delay_free_fill].heap = *heap;
#endif
#ifdef STATISTICS
            delay_free_list[delay_free_fill].size = size;
            STATS_ADD(delayed_free_bytes, size);
#endif
            delay_free_fill++;
            /* Rather than try to engineer a return, we continue on w/ NULL
             * which free() is guaranteed to handle
             */
            pass_to_free = NULL;
            STATS_ADD(delayed_free_bytes, (uint)size);
        }
        if (pass_to_free != NULL) {
            rb_node_t *node = rb_find(delay_free_tree, pass_to_free);
            if (node != NULL)
                rb_delete(delay_free_tree, node);
            else
                ASSERT(false, "delay_free_tree inconsistent");
        }
        dr_mutex_unlock(delay_free_lock);
        return pass_to_free;
    }
    return real_base; /* no change */
}

#ifdef WINDOWS
/* i#264: client needs to clean up any data related to allocs inside this heap */
void
client_handle_heap_destroy(void *drcontext, per_thread_t *pt, HANDLE heap)
{
    int i, num_removed = 0;
    dr_mutex_lock(delay_free_lock);
    for (i = 0; i < delay_free_fill; i++) {
        if (delay_free_list[i].heap == heap) {
            /* not worth shifting the array around: just invalidate */
            rb_node_t *node = rb_find(delay_free_tree, delay_free_list[i].addr);
            if (node != NULL)
                rb_delete(delay_free_tree, node);
            else
                ASSERT(false, "delay_free_tree inconsistent");
            delay_free_list[i].addr = NULL;
            num_removed++;
        }
    }
    dr_mutex_unlock(delay_free_lock);
    LOG(2, "removed %d delayed frees from destroyed heap "PFX"\n",
        num_removed, heap);
}
#endif

#ifdef DEBUG
void
print_free_tree(rb_node_t *node, void *data)
{
    app_pc start;
    size_t size;
    rb_node_fields(node, &start, &size, NULL);
    LOG(3, "\tfree tree entry: "PFX"-"PFX"\n", start, start+size);
}
#endif

bool
overlaps_delayed_free(byte *start, byte *end, byte **free_start, byte **free_end)
{
    bool res = false;
    rb_node_t *node;
    dr_mutex_lock(delay_free_lock);
    LOG(3, "overlaps_delayed_free "PFX"-"PFX"\n", start, end);
    DOLOG(3, { rb_iterate(delay_free_tree, print_free_tree, NULL); });
    node = rb_overlaps_node(delay_free_tree, start, end);
    if (node != NULL) {
        /* we store real base and real size, so exclude redzone since we only
         * want to report overlap with app-requested base and size
         */
        app_pc real_base;
        size_t size;
        bool has_redzone;
        rb_node_fields(node, &real_base, &size, (void **)&has_redzone);
        LOG(3, "\toverlap real base: "PFX"\n", real_base);
        if (!has_redzone ||
            (start < real_base + size - options.redzone_size &&
             end >= real_base + options.redzone_size)) {
            res = true;
            if (free_start != NULL)
                *free_start = real_base + options.redzone_size;
            /* size is the app-asked-for-size */
            if (free_end != NULL)
                *free_end = real_base + size - options.redzone_size;
        }
    }
    dr_mutex_unlock(delay_free_lock);
    return res;
}

void
client_handle_mmap(per_thread_t *pt, app_pc base, size_t size, bool anon)
{
#ifdef WINDOWS
    if (!options.leaks_only && options.shadowing) {
        if (anon) {
            if (pt->in_heap_routine == 0)
                shadow_set_range(base, base+size, SHADOW_DEFINED);
            else {
                /* FIXME PR 575260: should we do what we do on linux and leave
                 * unaddr?  I haven't yet studied what Windows Heap behavior is
                 * for very large allocations.  For now marking entire
                 * as undefined and ignoring headers.
                 */
                shadow_set_range(base, base+size, SHADOW_UNDEFINED);
            }
        } else
            mmap_walk(base, size, IF_WINDOWS_(NULL) true/*add*/);
    }
#else
    if (anon) {
        /* Kernel sets to 0 but for malloc we want to treat as undefined
         * if a single large malloc chunk or as unaddressable if a new
         * malloc arena.  For calloc, or for non-alloc, we want defined.
         * We assume that post-malloc or post-calloc will take care of
         * marking however much of the mmap has been parceled out,
         * so we leave the region as unaddressable here, which handles
         * both the extra-large headers for single large chunks and
         * new arenas gracefully and without races (xref PR 427601, PR
         * 531619).
         */
        if (pt->in_heap_routine == 0 && !options.leaks_only && options.shadowing)
            shadow_set_range(base, base+size, SHADOW_DEFINED);
        /* PR 418629: to determine stack bounds accurately we track mmaps */
        mmap_tree_add(base, size);
    } else if (!options.leaks_only && options.shadowing) {
        /* mapping a file: if an image need to walk sub-regions.
         * FIXME: on linux though the sub-regions have their own
         * mmaps: wait for those?
         */
        mmap_walk(base, size, true/*add*/);
    }
#endif
    LOG(2, "mmap %s "PFX"-"PFX"\n", anon ? "anon" : "file",
        base, base+size);
}

void
client_handle_munmap(app_pc base, size_t size, bool anon)
{
#ifdef WINDOWS
    if (!options.leaks_only && options.shadowing) {
        if (anon)
            shadow_set_range(base, base+size, SHADOW_UNADDRESSABLE);
        else
            mmap_walk(base, size, IF_WINDOWS_(NULL) false/*remove*/);
    }
#else
    /* anon not known to common/alloc.c so we see whether in the anon table */
    if (mmap_tree_remove(base, size)) {
        if (!options.leaks_only && options.shadowing)
            shadow_set_range(base, base+size, SHADOW_UNADDRESSABLE);
    } else if (!options.leaks_only && options.shadowing)
        mmap_walk(base, size, IF_WINDOWS_(NULL) false/*remove*/);
#endif
    LOG(2, "munmap %s "PFX"-"PFX"\n", anon ? "anon" : "file",
        base, base+size);
}

void
client_handle_munmap_fail(app_pc base, size_t size, bool anon)
{
#ifdef WINDOWS
    /* FIXME: need to restore shadow values by storing on pre-syscall */
    if (!options.leaks_only && options.shadowing)
        mmap_walk(base, size, IF_WINDOWS_(NULL) true/*add*/);
#else
    if (anon) {
        /* FIXME: we need to store the shadow values in pre so we
         * can restore here.  We should also work that into our
         * race handling model.  Xref malloc race handling: but
         * that relies on detecting failures ahead of time.
         */
        if (!options.leaks_only && options.shadowing)
            shadow_set_range(base, base+size, SHADOW_DEFINED);
        mmap_tree_add(base, size);
    } else if (!options.leaks_only && options.shadowing)
        mmap_walk(base, size, true/*add*/);
#endif
}

#ifdef LINUX
void
client_handle_mremap(app_pc old_base, size_t old_size, app_pc new_base, size_t new_size,
                     bool image)
{
    bool shrink = (new_size < old_size);
    if (!options.leaks_only && options.shadowing) {
        shadow_copy_range(old_base, new_base, shrink ? new_size : old_size);
        if (shrink) {
            shadow_set_range(old_base+new_size, old_base+old_size,
                             SHADOW_UNADDRESSABLE);
        } else {
            shadow_set_range(new_base+old_size, new_base+new_size,
                             image ? SHADOW_DEFINED : SHADOW_UNDEFINED);
        }
    }
    IF_DEBUG(bool found =)
        mmap_tree_remove(old_base, old_size);
    ASSERT(found, "for now assuming mremap is of anon regions only");
    mmap_tree_add(new_base, new_size);
}
#endif

#ifdef WINDOWS
void
client_handle_cbret(void *drcontext, per_thread_t *pt_parent, per_thread_t *pt_child)
{
    dr_mcontext_t mc;
    byte *sp;
    client_per_thread_t *cpt_parent = (client_per_thread_t *) pt_parent->client_data;
    if (options.leaks_only || !options.shadowing)
        return;
    dr_get_mcontext(drcontext, &mc, NULL);
    sp = (byte *) mc.esp;
    LOG(2, "cbret: marking stack "PFX"-"PFX" as unaddressable\n",
        sp, cpt_parent->pre_callback_esp);
    for (; sp < cpt_parent->pre_callback_esp; sp++)
        shadow_set_byte(sp, SHADOW_UNADDRESSABLE);
}

void
client_handle_callback(void *drcontext, per_thread_t *pt_parent, per_thread_t *pt_child,
                       bool new_depth)
{
    client_per_thread_t *cpt_parent = (client_per_thread_t *) pt_parent->client_data;
    client_per_thread_t *cpt;
    if (new_depth) {
        cpt = thread_alloc(drcontext, sizeof(*cpt), HEAPSTAT_MISC);
        memset(cpt, 0, sizeof(*cpt));
        pt_child->client_data = (void *) cpt;
    } else {
        /* client_data for most part is not shared so first clear the old one */
        cpt = (client_per_thread_t *) pt_child->client_data;
        syscall_reset_per_thread(drcontext, pt_child);
        memset(cpt, 0, sizeof(*cpt));
    }
    /* shared fields */
    cpt->shadow_regs = cpt_parent->shadow_regs;
}

void
client_handle_Ki(void *drcontext, app_pc pc, dr_mcontext_t *mc)
{
    per_thread_t *pt = (per_thread_t *) dr_get_tls_field(drcontext);
    client_per_thread_t *cpt = (client_per_thread_t *) pt->client_data;
    /* The kernel has placed some data on the stack.  We assume we're
     * on the same thread stack.  FIXME: check those assumptions by checking
     * default stack bounds.
     */
    app_pc sp = (app_pc) mc->esp;
    TEB *teb = get_TEB();
    app_pc base_esp = teb->StackBase;
    app_pc stop_esp = NULL;
    if (options.leaks_only || !options.shadowing)
        return;
    if (sp < base_esp && base_esp - sp < TYPICAL_STACK_MIN_SIZE)
        stop_esp = base_esp;
    ASSERT(ALIGNED(sp, 4), "stack not aligned");
    while ((stop_esp != NULL && sp < stop_esp) ||
           /* if not on main stack, go until non-unaddr: we could walk off
            * into an adjacent free space is the problem though.
            * should do mem query!
            */
           (stop_esp == NULL && shadow_get_byte(sp) == SHADOW_UNADDRESSABLE)) {
        shadow_set_byte(sp, SHADOW_DEFINED);
        sp++;
        if (sp - (byte *) mc->esp >= TYPICAL_STACK_MIN_SIZE) {
            ASSERT(false, "kernel-placed data on stack too large: error?");
            break; /* abort */
        }
    }
    ASSERT(ALIGNED(sp, 4), "stack not aligned");
    
    LOG(2, "Ki routine "PFX": marked stack "PFX"-"PFX" as defined\n",
        pc, mc->esp, sp);

    /* We do want to set the parent's for callback, so tls field is correct
     * since this is prior to client_handle_callback()
     */
    cpt->pre_callback_esp = sp;
}
#endif /* WINDOWS */

/***************************************************************************
 * SIGNALS
 */

void
client_pre_syscall(void *drcontext, int sysnum, per_thread_t *pt)
{
    dr_mcontext_t mc;
    if (options.leaks_only || !options.shadowing)
        return;
    dr_get_mcontext(drcontext, &mc, NULL);
#ifdef WINDOWS
    if (sysnum == sysnum_continue) {
        CONTEXT *cxt = (CONTEXT *) dr_syscall_get_param(drcontext, 0);
        if (cxt != NULL) {
            /* FIXME: what if the syscall fails? */
            byte *sp;
            register_shadow_set_dword(REG_XAX, shadow_get_byte((app_pc)&cxt->Eax));
            register_shadow_set_dword(REG_XCX, shadow_get_byte((app_pc)&cxt->Ecx));
            register_shadow_set_dword(REG_XDX, shadow_get_byte((app_pc)&cxt->Edx));
            register_shadow_set_dword(REG_XBX, shadow_get_byte((app_pc)&cxt->Ebx));
            register_shadow_set_dword(REG_XBP, shadow_get_byte((app_pc)&cxt->Ebp));
            register_shadow_set_dword(REG_XSP, shadow_get_byte((app_pc)&cxt->Esp));
            register_shadow_set_dword(REG_XSI, shadow_get_byte((app_pc)&cxt->Esi));
            register_shadow_set_dword(REG_XDI, shadow_get_byte((app_pc)&cxt->Edi));
            if (cxt->Esp < mc.esp) {
                if (mc.esp - cxt->Esp < options.stack_swap_threshold) {
                    shadow_set_range((byte *) cxt->Esp, (byte *) mc.esp, SHADOW_UNDEFINED);
                    LOG(2, "NtContinue: marked stack "PFX"-"PFX" as undefined\n",
                        cxt->Esp, mc.esp);
                }
            } else if (cxt->Esp - mc.esp < options.stack_swap_threshold) {
                shadow_set_range((byte *) mc.esp, (byte *) cxt->Esp, SHADOW_UNADDRESSABLE);
                LOG(2, "NtContinue: marked stack "PFX"-"PFX" as unaddressable\n",
                    mc.esp, cxt->Esp);
            }
        }
    } else if (sysnum == sysnum_setcontext) {
        /* FIXME PR 575434: we need to know whether the thread is in this
         * process or not, and then get its current context so we can
         * change the esp between old and new values and set the register
         * shadow values.
         */
        ASSERT(false, "NtSetContextThread NYI");
    }
#else
    client_per_thread_t *cpt = (client_per_thread_t *) pt->client_data;
    if (sysnum == SYS_rt_sigaction
        IF_X86_32(|| sysnum == SYS_sigaction || sysnum == SYS_signal)) {
        /* PR 406333: linux signal delivery.
         * For delivery: signal event doesn't help us since have to predict
         * which stack and size of frame: should intercept handler registration
         * and wait until enter a handler.  Can ignore SIG_IGN and SIG_DFL.
         */
        void *handler = NULL;
        if (sysnum == SYS_rt_sigaction) {
            /* 2nd arg is ptr to struct w/ handler as 1st field */
            safe_read((void *)pt->sysarg[1], sizeof(handler), &handler);
        }
# ifdef X86_32
        else if (sysnum == SYS_sigaction) {
            /* 2nd arg is ptr to struct w/ handler as 1st field */
            safe_read((void *)pt->sysarg[1], sizeof(handler), &handler);
        }
        else if (sysnum == SYS_signal) {
            /* 2nd arg is handler */
            handler = (void *) pt->sysarg[1];
        }
# endif
        if (handler != NULL) {
            LOGPT(2, pt, "SYS_rt_sigaction/etc.: new handler "PFX"\n", handler);
            /* We make a simplifying assumption: handler code is only used for
             * signal handling.  We could keep a counter and inc on every success
             * and dec on failure and on change to IGN/DFL and remove when it hits
             * 0 -- but might have races where a final signal comes in.  We assume
             * we can leave our instrumentation there and if it is executed
             * executed for non-signals our check for prior signal event
             * is good enough to distinguish.
             */
            if (handler != SIG_IGN && handler != SIG_DFL)
                hashtable_add(&sighand_table, (void*)handler, (void*)1);
        } else {
            LOGPT(2, pt, "SYS_rt_sigaction/etc.: bad handler\n");
        }
    }
    else if (sysnum == SYS_rt_sigreturn IF_X86_32(|| sysnum == SYS_sigreturn)) {
        /* PR 406333: linux signal delivery.
         * Should also watch for sigreturn: whether altstack or not, invalidate
         * where frame was.  Either need to record at handler entry the base of
         * the frame, or at sigreturn determine target esp.
         *
         * Will longjmp be handled naturally?  should be.
         */
        ASSERT(cpt->sigframe_top != NULL, "sigreturn with no prior signal");
        LOG(2, "at sigreturn: marking frame "PFX"-"PFX" unaddressable\n",
            mc.xsp, cpt->sigframe_top);
        shadow_set_range((app_pc)mc.xsp, cpt->sigframe_top, SHADOW_UNADDRESSABLE);
    }
    else if (sysnum == SYS_sigaltstack) {
        /* PR 406333: linux signal delivery */
        stack_t stk;
        cpt->prev_sigaltstack = cpt->sigaltstack;
        cpt->prev_sigaltsize = cpt->sigaltsize;
        if (safe_read((void *)pt->sysarg[0], sizeof(stk), &stk)) {
            if (stk.ss_flags == SS_DISABLE) {
                cpt->sigaltstack = NULL;
                cpt->sigaltsize = 0;
                /* Mark the old stack as addressable in case used as data now? */
            } else {
                /* We want the base (== highest addr) */
                cpt->sigaltstack = ((byte *) stk.ss_sp) + stk.ss_size;
                cpt->sigaltsize = stk.ss_size;
                ASSERT((cpt->sigaltstack < (byte*)mc.xsp ||
                        (ptr_int_t)cpt->sigaltstack - cpt->sigaltsize - mc.xsp >
                        options.stack_swap_threshold) &&
                       (cpt->sigaltstack > (byte*)mc.xsp ||
                        mc.xsp - ((ptr_int_t)cpt->sigaltstack + cpt->sigaltsize) >
                        options.stack_swap_threshold),
                       "sigaltstack within swap threshold of esp");
                /* We assume this memory will not be used for any other data */
                LOG(2, "marking sigaltstack "PFX"-"PFX" unaddressable\n",
                    stk.ss_sp, cpt->sigaltstack);
                shadow_set_range((app_pc)stk.ss_sp, cpt->sigaltstack,
                                 SHADOW_UNADDRESSABLE);
            }
            LOG(2, "new sigaltstack "PFX"\n", cpt->sigaltstack);
        } else {
            LOG(2, "WARNING: can't read sigaltstack param "PFX"\n", pt->sysarg[0]);
        }
    }
#endif /* WINDOWS */
}

void
client_post_syscall(void *drcontext, int sysnum, per_thread_t *pt)
{
#ifdef LINUX
    ptr_int_t result = dr_syscall_get_result(drcontext);
    client_per_thread_t *cpt = (client_per_thread_t *) pt->client_data;
    if (options.leaks_only || !options.shadowing)
        return;
    if (sysnum == SYS_rt_sigaction
        IF_X86_32(|| sysnum == SYS_sigaction || sysnum == SYS_signal)) {
        if (result != 0) {
            LOGPT(2, pt, "SYS_rt_sigaction/etc. FAILED for handler "PFX"\n",
                  pt->sysarg[1]);
            /* See notes above: if we had a counter we could remove from
             * sighand_table if there were no successfull registrations --
             * but we assume handler code is only used for signals so
             * we just leave in the table and rely on our pre-event check.
             */
        }
    }
    else if (sysnum == SYS_sigaltstack) {
        if (result != 0) {
            /* We can't query the OS since DR is hiding the real sigaltstack,
             * so we record the prev value
             */
            cpt->sigaltstack = cpt->prev_sigaltstack;
            cpt->sigaltsize = cpt->prev_sigaltsize;
            LOG(2, "sigaltstack failed, reverting to "PFX"\n", cpt->sigaltstack);
        }
    }
#endif
}

#ifdef LINUX
dr_signal_action_t
event_signal_alloc(void *drcontext, dr_siginfo_t *info)
{
    per_thread_t *pt = (per_thread_t *) dr_get_tls_field(drcontext);
    ASSERT(pt != NULL, "pt shouldn't be null");
    client_per_thread_t *cpt = (client_per_thread_t *) pt->client_data;
    cpt->signal_xsp = (app_pc) info->mcontext.xsp;
    LOG(2, "signal interrupted app at xsp="PFX"\n", cpt->signal_xsp);
    return DR_SIGNAL_DELIVER;
}

static void
at_signal_handler(void)
{
    /* PR 406333: linux signal delivery.
     * Need to know extent of frame: could record both esp in signal event
     * and record SYS_sigaltstack.
     * In handler, mark from cur esp upward as defined, until hit:
     * - Base of sigaltstack
     * - Esp at which signal happened
     * An alternative to recording esp where signal happened is to walk
     * until hit addressable memory.
     */
    void *drcontext = dr_get_current_drcontext();
    per_thread_t *pt = (per_thread_t *) dr_get_tls_field(drcontext);
    client_per_thread_t *cpt = (client_per_thread_t *) pt->client_data;
    dr_mcontext_t mc;
    byte *frame_top;
    ASSERT(!options.leaks_only && options.shadowing, "shadowing disabled");
    dr_get_mcontext(drcontext, &mc, NULL);

    /* Even if multiple signals to this thread we should get proper
     * (event,handler) pairs 
     */
    if (cpt->signal_xsp == NULL) {
        /* Could downgrade to a LOG */
        ASSERT(false, "in signal handler but not for signal?");
        return;
    }
    LOG(3, "in signal handler: alt="PFX", cur="PFX", interrupt="PFX"\n",
        cpt->sigaltstack, mc.esp, cpt->signal_xsp);
    if (cpt->sigaltstack != NULL &&
        cpt->sigaltstack > (app_pc) mc.xsp &&
        (size_t)(cpt->sigaltstack - (app_pc) mc.xsp) < cpt->sigaltsize) {
        if (cpt->sigaltstack > cpt->signal_xsp && cpt->signal_xsp < (app_pc) mc.xsp) {
            /* nested signal on alt stack */
            frame_top = cpt->signal_xsp;
        } else
            frame_top = cpt->sigaltstack;
    } else {
        ASSERT(cpt->signal_xsp > (app_pc) mc.xsp &&
               (size_t)(cpt->signal_xsp - (app_pc) mc.xsp) <
               /* nested signals could take up some space */
               10*options.stack_swap_threshold,
               "on unknown signal stack");
        frame_top = cpt->signal_xsp;
    }
    /* Assume whole frame is defined (else would need DR to identify
     * which parts are padding).
     */
    LOG(2, "in signal handler: marking frame "PFX"-"PFX" defined\n", mc.esp, frame_top);
    ASSERT((size_t)(frame_top - (app_pc) mc.xsp) < PAGE_SIZE,
           "signal frame way too big");
    shadow_set_range((app_pc) mc.xsp, frame_top, SHADOW_DEFINED);
    /* Record for sigreturn */
    cpt->sigframe_top = frame_top;
    /* Reset */
    cpt->signal_xsp = NULL;
}

void
instrument_signal_handler(void *drcontext, instrlist_t *bb, instr_t *inst,
                          app_pc pc)
{
    LOG(3, "instrumenting signal handler "PFX"\n", pc);
    dr_insert_clean_call(drcontext, bb, inst, (void *)at_signal_handler,
                         false, 0);
}
#endif /* LINUX */

/***************************************************************************
 * ADDRESSABILITY
 */

static bool
is_rawmemchr_pattern(void *drcontext, bool write, app_pc pc, app_pc next_pc,
                     app_pc addr, uint sz, instr_t *inst, bool *now_addressable OUT)
{
    /* PR 406535: glibc's rawmemchr does some bit tricks that can end
     * up using unaddressable or undefined values.  The erroneous load
     * is one of these:
     *   +0  8b 08                mov    (%eax) -> %ecx
     *   +0  8b 48 04             mov    0x4(%eax),%ecx
     *   +0  8b 48 08             mov    0x08(%eax) -> %ecx
     *   +0  8b 48 0c             mov    0x0c(%eax) -> %ecx
     * followed by the magic constant:
     *   +2  ba ff fe fe fe       mov    $0xfefefeff -> %edx 
     * followed by an add, or an xor and then an add, and then a jcc:
     *   +7  01 ca                add    %ecx %edx -> %edx   
     *   +9  73 59                jnb    $0x009041c7         
     * since the particular registers and mem sources vary, we don't do
     * raw bit comparisons and instead do high-level operand comparisons.
     * in fact, we try to also match very similar patterns in strcat,
     * strlen, strrchr, and memchr.
     *
     * strchr and strchrnul have an xor in between the load and the magic
     * constant which we also match:
     *   +0  8b 08                mov    (%eax),%ecx
     *   +2  31 d1                xor    %edx,%ecx
     *   +4  bf ff fe fe fe       mov    $0xfefefeff,%edi
     *
     * on Windows we have __from_strstr_to_strchr in intel/strchr.asm:
     *   +11  8b 0a               mov    (%edx) -> %ecx 
     *   +13  bf ff fe fe 7e      mov    $0x7efefeff -> %edi 
     *
     * xref PR 485131: propagate partial-unaddr on loads?  but would still
     * complain on the jnb.
     *
     * FIXME: share code w/ check_undefined_reg_exceptions() in readwrite.c.
     */
    instr_t next;
    app_pc dpc = next_pc;
    bool match = false;
    if (!dr_memory_is_readable(dpc, MAX_INSTR_SIZE)) /* shouldn't go off page */
        return false;
    instr_init(drcontext, &next);
    dpc = decode(drcontext, dpc, &next);
    /* We want to only allow the end of the search to be suppressed, to
     * avoid suppressing a real positive, so only unaligned addresses.
     */
    if (!ALIGNED(addr, 4) &&
        instr_get_opcode(inst) == OP_mov_ld &&
        opnd_is_reg(instr_get_dst(inst, 0)) &&
        opnd_get_size(instr_get_dst(inst, 0)) == OPSZ_PTR) {
        if (instr_valid(&next) &&
            instr_get_opcode(&next) == OP_xor &&
            opnd_is_reg(instr_get_src(&next, 0)) &&
            opnd_is_reg(instr_get_dst(&next, 0)) &&
            opnd_get_size(instr_get_dst(&next, 0)) == OPSZ_PTR) {
            /* Skip the strchr/strchnul xor */
            if (!dr_memory_is_readable(dpc, MAX_INSTR_SIZE)) /* shouldn't go off page */
                goto is_rawmemchr_pattern_done;
            instr_reset(drcontext, &next);
            dpc = decode(drcontext, dpc, &next);
        }
        if (instr_valid(&next) &&
            instr_get_opcode(&next) == OP_mov_imm &&
            (opnd_get_immed_int(instr_get_src(&next, 0)) == 0xfefefeff ||
             opnd_get_immed_int(instr_get_src(&next, 0)) == 0x7efefeff) &&
            opnd_is_reg(instr_get_dst(&next, 0))) {
            STATS_INC(strmem_unaddr_exception);
            *now_addressable = false;
            match = true;
        }
    }
 is_rawmemchr_pattern_done:
    instr_free(drcontext, &next);
    return match;
}

static bool
is_alloca_pattern(void *drcontext, bool write, app_pc pc, app_pc next_pc,
                  app_pc addr, uint sz, instr_t *inst, bool *now_addressable OUT)
{
    /* Check for alloca probes to trigger guard pages.
     * So far we've seen 3 different sequences:
         UNADDRESSABLE ACCESS: pc @0x0040db67 reading 0x0012ef80
         UNADDRESSABLE ACCESS: pc @0x0040db67 reading 0x0012ef81
         UNADDRESSABLE ACCESS: pc @0x0040db67 reading 0x0012ef82
         UNADDRESSABLE ACCESS: pc @0x0040db67 reading 0x0012ef83
         UNADDRESSABLE ACCESS: pc @0x0040db74 reading 0x0012ed48
         UNADDRESSABLE ACCESS: pc @0x0040db74 reading 0x0012ed49
         UNADDRESSABLE ACCESS: pc @0x0040db74 reading 0x0012ed4a
         UNADDRESSABLE ACCESS: pc @0x0040db74 reading 0x0012ed4b

         hello!_alloca_probe+0xc [intel\chkstk.asm @ 76]:
            76 0040db5c 81e900100000     sub     ecx,0x1000
            77 0040db62 2d00100000       sub     eax,0x1000
            79 0040db67 8501             test    [ecx],eax
            81 0040db69 3d00100000       cmp     eax,0x1000
            82 0040db6e 73ec             jnb     hello!_alloca_probe+0xc (0040db5c)
         hello!_alloca_probe+0x20 [intel\chkstk.asm @ 85]:
            85 0040db70 2bc8             sub     ecx,eax
            86 0040db72 8bc4             mov     eax,esp
            88 0040db74 8501             test    [ecx],eax
            90 0040db76 8be1             mov     esp,ecx
            92 0040db78 8b08             mov     ecx,[eax]
            93 0040db7a 8b4004           mov     eax,[eax+0x4]
            95 0040db7d 50               push    eax
            97 0040db7e c3               ret

         ntdll!_alloca_probe+0x15:
           7d61042d f7d8             neg     eax
           7d61042f 03c4             add     eax,esp
           7d610431 83c004           add     eax,0x4
           7d610434 8500             test    [eax],eax
           7d610436 94               xchg    eax,esp
           7d610437 8b00             mov     eax,[eax]
           7d610439 50               push    eax
           7d61043a c3               ret
         in this instance the probe goes 4 bytes into the stack instead
         of extending it, and then after shortening esp reads beyond TOS
         to move the retaddr to the new TOS!
           memref: read @0x7d610434 0x0007e2f0 0x4
           esp adjust esp=0x0007e2ec => 0x0007e2f0
           set range 0x0007e2ec-0x0007e2f0 => 0x0
           memref: read @0x7d610437 0x0007e2ec 0x4
           UNADDRESSABLE ACCESS: pc @0x7d610437 reading 0x0007e2ec
         though this also occurs as ntdll!_chkstk where the probe does go beyond TOS:
         depends on value of eax == amount checking/probing by

        cygwin1!alloca:
          610fc670 51               push    ecx
          610fc671 89e1             mov     ecx,esp
          610fc673 83c108           add     ecx,0x8
          610fc676 3d00100000       cmp     eax,0x1000
          610fc67b 7210             jb      cygwin1!alloca+0x1d (610fc68d)
          610fc67d 81e900100000     sub     ecx,0x1000
          610fc683 830900           or      dword ptr [ecx],0x0
          610fc686 2d00100000       sub     eax,0x1000
          610fc68b ebe9             jmp     cygwin1!alloca+0x6 (610fc676)
          610fc68d 29c1             sub     ecx,eax
          610fc68f 830900           or      dword ptr [ecx],0x0
          610fc692 89e0             mov     eax,esp
          610fc694 89cc             mov     esp,ecx
          610fc696 8b08             mov     ecx,[eax]
          610fc698 8b4004           mov     eax,[eax+0x4]
          610fc69b ffe0             jmp     eax

        gap.exe:
          00444bf2 2d00100000       sub     eax,0x1000
          00444bf7 8500             test    [eax],eax
          00444bf9 ebe9             jmp     gap+0x44be4 (00444be4)
          00444bfb cc               int     3
          0:000> U 00444be4
          00444be4 3bc8             cmp     ecx,eax
          00444be6 720a             jb      gap+0x44bf2 (00444bf2)
    */
    /* For now we do an exact pattern match but of course this
     * won't generalize well for other versions of alloca: OTOH we
     * don't want any false negatives.
     */
    instr_t next;
    app_pc dpc = next_pc;
    bool match = false;
    /* we deref pc-1 below. all these are mid-routine so should be no page boundaries. */
    if (!dr_memory_is_readable(pc-1, (dpc-(pc-1))+ MAX_INSTR_SIZE))
        return false;
    instr_init(drcontext, &next);

    if (instr_get_opcode(inst) == OP_test &&
        opnd_is_base_disp(instr_get_src(inst, 0)) &&
        (opnd_get_base(instr_get_src(inst, 0)) == REG_ECX ||
         opnd_get_base(instr_get_src(inst, 0)) == REG_EAX) &&
        opnd_get_index(instr_get_src(inst, 0)) == REG_NULL &&
        opnd_get_scale(instr_get_src(inst, 0)) == 0 &&
        opnd_get_disp(instr_get_src(inst, 0)) == 0 &&
        opnd_is_reg(instr_get_src(inst, 1)) &&
        opnd_get_reg(instr_get_src(inst, 1)) == REG_EAX) {
        instr_reset(drcontext, &next);
        dpc = decode(drcontext, dpc, &next);
        if (instr_valid(&next) &&
            ((instr_get_opcode(&next) == OP_cmp &&
              opnd_is_reg(instr_get_src(&next, 0)) &&
              opnd_get_reg(instr_get_src(&next, 0)) == REG_EAX &&
              opnd_is_immed_int(instr_get_src(&next, 1))) ||
             ((instr_get_opcode(&next) == OP_mov_ld ||
               instr_get_opcode(&next) == OP_mov_st) &&
              opnd_is_reg(instr_get_src(&next, 0)) &&
              opnd_get_reg(instr_get_src(&next, 0)) == REG_ECX &&
              opnd_is_reg(instr_get_dst(&next, 0)) &&
              opnd_get_reg(instr_get_dst(&next, 0)) == REG_ESP) ||
             (instr_get_opcode(&next) == OP_xchg &&
              opnd_is_reg(instr_get_src(&next, 0)) &&
              opnd_get_reg(instr_get_src(&next, 0)) == REG_ESP) ||
             (instr_get_opcode(&next) == OP_jmp ||
              instr_get_opcode(&next) == OP_jmp_short))) {
            match = true;
            /* this is a probe to commit the page: does not change range of
             * stack pointer
             */
            *now_addressable = false;
        }
    }
    /* ntdll!_chkstk retaddr shift */
    else if (instr_get_opcode(inst) == OP_mov_ld &&
             opnd_is_base_disp(instr_get_src(inst, 0)) &&
             opnd_get_base(instr_get_src(inst, 0)) == REG_EAX &&
             opnd_get_index(instr_get_src(inst, 0)) == REG_NULL &&
             opnd_get_scale(instr_get_src(inst, 0)) == 0 &&
             opnd_get_disp(instr_get_src(inst, 0)) == 0 &&
             opnd_is_reg(instr_get_dst(inst, 0)) &&
             opnd_get_reg(instr_get_dst(inst, 0)) == REG_EAX &&
             /* prev instr is "xchg esp, eax" */
             *(pc-1) == 0x94) {
            match = true;
            /* do NOT mark addressable as the next instr, a push, will do so */
            *now_addressable = false;
        }
    /* cygwin alloca */
    else if (instr_get_opcode(inst) == OP_or &&
             opnd_is_base_disp(instr_get_dst(inst, 0)) &&
             opnd_get_base(instr_get_dst(inst, 0)) == REG_ECX &&
             opnd_get_index(instr_get_dst(inst, 0)) == REG_NULL &&
             opnd_get_scale(instr_get_dst(inst, 0)) == 0 &&
             opnd_get_disp(instr_get_dst(inst, 0)) == 0 &&
             opnd_is_immed_int(instr_get_src(inst, 0)) &&
             opnd_get_immed_int(instr_get_src(inst, 0)) == 0) {
        /* or of memory with 0 unusual enough that we look only at that instr */
        match = true;
        /* this is a probe to commit the page: does not change range of
         * stack pointer
         */
        *now_addressable = false; /* FIXME: I used to have true here: verify ok */
    }
#ifdef STATISTICS
    if (match)
        STATS_INC(alloca_exception);
#endif
    instr_free(drcontext, &next);
    return match;
}

static bool
is_strlen_pattern(void *drcontext, bool write, app_pc pc, app_pc next_pc,
                  app_pc addr, uint sz, instr_t *inst, bool *now_addressable OUT)
{
    /* Check for intel\strlen.asm case where it reads 4 bytes for efficiency:
     * it only does so if aligned, so no danger of touching next page, and
     * though it does look at the extra bytes the string should terminate
     * in the valid bytes.  So, while ugly, technically it's an ok bug to suppress.
     *    hello!strlen+0x30 [F:\SP\vctools\crt_bld\SELF_X86\crt\src\intel\strlen.asm @ 81]:
     *       81 00405f80 8b01             mov     eax,[ecx]
     *       82 00405f82 bafffefe7e       mov     edx,0x7efefeff
     *       83 00405f87 03d0             add     edx,eax
     *       84 00405f89 83f0ff           xor     eax,0xffffffff
     *       85 00405f8c 33c2             xor     eax,edx
     *       86 00405f8e 83c104           add     ecx,0x4
     *       87 00405f91 a900010181       test    eax,0x81010100
     *       88 00405f96 74e8             jz      hello!strlen+0x30 (00405f80)
     *    hello!strlen+0x48 [F:\SP\vctools\crt_bld\SELF_X86\crt\src\intel\strlen.asm @ 90]:
     *       90 00405f98 8b41fc           mov     eax,[ecx-0x4]
     *       91 00405f9b 84c0             test    al,al
     *       92 00405f9d 7432             jz      hello!strlen+0x81 (00405fd1)
     *
     * variant:
     *    gap+0x4516e:
     *    0044516e bafffefe7e       mov     edx,0x7efefeff
     *    00445173 8b06             mov     eax,[esi]
     *    00445175 03d0             add     edx,eax
     *    00445177 83f0ff           xor     eax,0xffffffff
     *    0044517a 33c2             xor     eax,edx
     *    0044517c 8b16             mov     edx,[esi]
     *    0044517e 83c604           add     esi,0x4
     *    00445181 a900010181       test    eax,0x81010100
     */
    instr_t next;
    app_pc dpc = next_pc;
    bool match = false;
    /* we deref pc-4 below. all these are mid-routine so should be no page boundaries. */
    if (!dr_memory_is_readable(pc-4, (dpc-(pc-4))+ MAX_INSTR_SIZE))
        return false;
    instr_init(drcontext, &next);
    /* FIXME PR 406718: for this, and exceptions below, we should ensure that only
     * the final byte(s) are unaddressable, and not allow middle bytes or
     * any other real positive to slip through
     */
    if (!ALIGNED(addr, 4) &&
        instr_get_opcode(inst) == OP_mov_ld &&
        opnd_is_base_disp(instr_get_src(inst, 0)) &&
        opnd_get_base(instr_get_src(inst, 0)) == REG_ECX &&
        opnd_get_index(instr_get_src(inst, 0)) == REG_NULL &&
        opnd_get_scale(instr_get_src(inst, 0)) == 0 &&
        (opnd_get_disp(instr_get_src(inst, 0)) == 0 ||
         opnd_get_disp(instr_get_src(inst, 0)) == -4) &&
        opnd_is_reg(instr_get_dst(inst, 0)) &&
        opnd_get_reg(instr_get_dst(inst, 0)) == REG_EAX) {
        int raw = *(int *)dpc;
        instr_reset(drcontext, &next);
        dpc = decode(drcontext, dpc, &next);
        if (instr_valid(&next) &&
            (raw == 0x3274c084 /*84c0 7432*/ ||
             (instr_get_opcode(&next) == OP_mov_imm &&
              opnd_is_immed_int(instr_get_src(&next, 0)) &&
              opnd_get_immed_int(instr_get_src(&next, 0)) == 0x7efefeff &&
              opnd_is_reg(instr_get_dst(&next, 0)) &&
              opnd_get_reg(instr_get_dst(&next, 0)) == REG_EDX))) {
            match = true;
            STATS_INC(strlen_exception);
            *now_addressable = false;
        }
    }
    /* strlen variation:
     *    gap+0x4516e:
     *    0044516e bafffefe7e       mov     edx,0x7efefeff
     *    00445173 8b06             mov     eax,[esi]
     *    00445175 03d0             add     edx,eax
     *    00445177 83f0ff           xor     eax,0xffffffff
     *    0044517a 33c2             xor     eax,edx
     *    0044517c 8b16             mov     edx,[esi]
     *    0044517e 83c604           add     esi,0x4
     *    00445181 a900010181       test    eax,0x81010100
     */
    else if (!ALIGNED(addr, 4) &&
             instr_get_opcode(inst) == OP_mov_ld &&
             opnd_is_base_disp(instr_get_src(inst, 0)) &&
             opnd_get_base(instr_get_src(inst, 0)) == REG_ESI &&
             opnd_get_index(instr_get_src(inst, 0)) == REG_NULL &&
             opnd_get_scale(instr_get_src(inst, 0)) == 0 &&
             opnd_get_disp(instr_get_src(inst, 0)) == 0 &&
             opnd_is_reg(instr_get_dst(inst, 0)) &&
             (opnd_get_reg(instr_get_dst(inst, 0)) == REG_EAX ||
              opnd_get_reg(instr_get_dst(inst, 0)) == REG_EDX)) {
        int raw = *(int *)(pc-4);
        if (raw == 0x7efefeff || raw == 0xc233fff0 /*f0ff 33c2*/) {
            match = true;
            STATS_INC(strlen_exception);
            *now_addressable = false;
        }
    }
    instr_free(drcontext, &next);
    return match;
}

static bool
is_strcpy_pattern(void *drcontext, bool write, app_pc pc, app_pc next_pc,
                  app_pc addr, uint sz, instr_t *inst, bool *now_addressable OUT)
{
    instr_t next;
    app_pc dpc = next_pc;
    bool match = false;
    /* all these are mid-routine so should be no page boundaries */
    if (!dr_memory_is_readable(dpc, MAX_INSTR_SIZE))
        return false;
    instr_init(drcontext, &next);
   
    /* Check for cygwin1!strcpy case where it reads 4 bytes for efficiency:
     * it only does so if aligned, like strlen above.
     *     cygwin1!strcpy:
     *     610deb60 55               push    ebp
     *     610deb61 89e5             mov     ebp,esp
     *     610deb63 8b550c           mov     edx,[ebp+0xc]
     *     610deb66 57               push    edi
     *     610deb67 8b7d08           mov     edi,[ebp+0x8]
     *     610deb6a 89d0             mov     eax,edx
     *     610deb6c 56               push    esi
     *     610deb6d 09f8             or      eax,edi
     *     610deb6f 53               push    ebx
     *     610deb70 a803             test    al,0x3
     *     610deb72 89f9             mov     ecx,edi
     *     610deb74 753a             jnz     cygwin1!strcpy+0x50 (610debb0)
     *     610deb76 89fe             mov     esi,edi
     *     610deb78 89d3             mov     ebx,edx
     *     610deb7a eb0c             jmp     cygwin1!strcpy+0x28 (610deb88)
     *     610deb7c 8d742600         lea     esi,[esi]
     *     610deb80 890e             mov     [esi],ecx
     *     610deb82 83c304           add     ebx,0x4
     *     610deb85 83c604           add     esi,0x4
     *     610deb88 8b0b             mov     ecx,[ebx]
     *     610deb8a 89ca             mov     edx,ecx
     *     610deb8c 8d81fffefefe     lea     eax,[ecx+0xfefefeff]
     *     610deb92 f7d2             not     edx
     *     610deb94 21d0             and     eax,edx
     *     610deb96 a980808080       test    eax,0x80808080
     *     610deb9b 74e3             jz      cygwin1!strcpy+0x20 (610deb80)
     */
    if (!ALIGNED(addr, 4) &&
        instr_get_opcode(inst) == OP_mov_ld &&
        opnd_is_base_disp(instr_get_src(inst, 0)) &&
        opnd_get_base(instr_get_src(inst, 0)) == REG_EBX &&
        opnd_get_index(instr_get_src(inst, 0)) == REG_NULL &&
        opnd_get_scale(instr_get_src(inst, 0)) == 0 &&
        opnd_get_disp(instr_get_src(inst, 0)) == 0 &&
        opnd_is_reg(instr_get_dst(inst, 0)) &&
        opnd_get_reg(instr_get_dst(inst, 0)) == REG_ECX) {
        instr_reset(drcontext, &next);
        dpc = decode(drcontext, dpc, &next);
        if (instr_valid(&next)) {
            instr_reset(drcontext, &next);
            dpc = decode(drcontext, dpc, &next);
            if (instr_valid(&next) &&
                instr_get_opcode(&next) == OP_lea &&
                opnd_get_base(instr_get_src(&next, 0)) == REG_ECX &&
                opnd_get_index(instr_get_src(&next, 0)) == REG_NULL &&
                opnd_get_scale(instr_get_src(&next, 0)) == 0 &&
                opnd_get_disp(instr_get_src(&next, 0)) == 0xfefefeff &&
                opnd_is_reg(instr_get_dst(&next, 0)) &&
                opnd_get_reg(instr_get_dst(&next, 0)) == REG_EAX) {
                match = true;
                STATS_INC(strcpy_exception);
                *now_addressable = false;
            }
        }
    }
    instr_free(drcontext, &next);
    return match;
}


static bool
is_ok_unaddressable_pattern(bool write, app_loc_t *loc, app_pc addr, uint sz)
{
    void *drcontext = dr_get_current_drcontext();
    app_pc pc, dpc;
    instr_t inst;
    bool match = false, now_addressable = false;
    if (loc->type != APP_LOC_PC) /* ignore syscalls (PR 488793) */
        return false;
    /* PR 503779: be sure to not do this readability check before
     * the heap header/tls checks, else we have big perf hits!
     * Needs to be on a rare path.
     */
    if (!dr_memory_is_readable(addr, 1))
        return false;
    pc = loc_to_pc(loc);
    if (!dr_memory_is_readable(pc, 1))
        return false;
    instr_init(drcontext, &inst);
    dpc = decode(drcontext, pc, &inst);
    ASSERT(instr_valid(&inst), "unknown suspect instr");

    if (!match) {
        match = is_alloca_pattern(drcontext, write, pc, dpc, addr, sz,
                                  &inst, &now_addressable);
    }
    if (!match) {
        match = is_strlen_pattern(drcontext, write, pc, dpc, addr, sz,
                                  &inst, &now_addressable);
    }
    if (!match) {
        match = is_strcpy_pattern(drcontext, write, pc, dpc, addr, sz,
                                  &inst, &now_addressable);
    }
    if (!match) {
        match = is_rawmemchr_pattern(drcontext, write, pc, dpc, addr, sz,
                                     &inst, &now_addressable);
    }

    if (now_addressable)
        shadow_set_byte(addr, SHADOW_UNDEFINED);
    instr_free(drcontext, &inst);
    return match;
}

#ifdef LINUX
/* Until we have a private loader, we have to have exceptions for the
 * loader reading our own libraries.  Xref PR 
 */
static bool
is_loader_exception(app_loc_t *loc, app_pc addr, uint sz)
{
    /* Allow the loader to read .dynamic section of DR or DrMem libs.
     * Also allow lib itself to access its own lib.
     */
    bool res = false;
    if (is_in_client_or_DR_lib(addr)) {
        app_pc pc = loc_to_pc(loc);
        module_data_t *data = dr_lookup_module(pc);
        if (data != NULL) {
            const char *modname = dr_module_preferred_name(data);
            if (strncmp(modname, "ld-linux.so.", 12) == 0 ||
                is_in_client_or_DR_lib(pc)) {
                /* If this happens too many times we may want to go back to
                 * marking our libs as defined and give up on catching wild
                 * app writes to those regions
                 */
                STATS_INC(loader_DRlib_exception);
                res = true;
                LOG(2, "ignoring unaddr for loader accessing DR/DrMem lib\n");
            }
            dr_free_module_data(data);
        }
    }
    return res;
}
#endif /* LINUX */

bool
check_unaddressable_exceptions(bool write, app_loc_t *loc, app_pc addr, uint sz)
{
    void *drcontext = dr_get_current_drcontext();
    per_thread_t *pt = (per_thread_t *) dr_get_tls_field(drcontext);
#ifdef WINDOWS
    TEB *teb = get_TEB();
    /* We can't use teb->ProcessEnvironmentBlock b/c i#249 points it at private PEB */
    PEB *peb = get_app_PEB();
#endif
    /* It's important to handle the very-common heap-header w/o translating
     * loc's pc field which is a perf hit
     */
    if (is_in_heap_region(addr) && pt->in_heap_routine > 0) {
        /* FIXME: ideally we would know exactly which fields were header
         * fields and which ones were ok to write to, to avoid heap corruption
         * by bugs in heap routines (and avoid allowing bad reads by other
         * ntdll routines like memcpy).
         * For glibc we do know the header size, but on an alloc the block
         * is not yet in our malloc table (it is on a free).
         */
        LOG(3, "ignoring unaddressable %s by heap routine "PFX" to "PFX"\n",
            write ? "write" : "read", loc_to_print(loc), addr);
        STATS_INC(heap_header_exception);
        /* leave as unaddressable */
        return true;
    }
#ifdef WINDOWS
    /* For TLS, rather than proactively track sets and unsets, we check
     * on fault for whether set and we never mark as addressable.
     * FIXME: for performance we should proactively track so we can mark
     * as addressable.  Should just watch the API and let people who
     * bypass to set the bits themselves deal w/ the false positives instead
     * of adding checks to all writes to catch tls bitmask writes.
     */
    if ((addr >= (app_pc)&teb->TlsSlots[0] && addr < (app_pc)&teb->TlsSlots[64]) ||
        (teb->TlsExpansionSlots != NULL &&
         addr >= (app_pc)teb->TlsExpansionSlots &&
         addr < (app_pc)teb->TlsExpansionSlots +
         TLS_EXPANSION_BITMAP_SLOTS*sizeof(byte))) {
        bool tls_ok = false;
        if (addr >= (app_pc)&teb->TlsSlots[0] && addr < (app_pc)&teb->TlsSlots[64]) {
            uint slot = (addr - (app_pc)&teb->TlsSlots[0]) / sizeof(void*);
            LOG(3, "checking unaddressable TLS slot "PFX" => %d\n",
                 addr, slot);
            tls_ok = (peb->TlsBitmap->Buffer[slot/32] & (1 << (slot % 32)));
        } else {
            uint slot = (addr - (app_pc)teb->TlsExpansionSlots) / sizeof(void*);
            ASSERT(peb->TlsExpansionBitmap != NULL, "TLS mismatch");
            LOG(3, "checking unaddressable expansion TLS slot "PFX" => %d\n",
                 addr, slot);
            tls_ok = (peb->TlsExpansionBitmap->Buffer[slot/32] & (1 << (slot % 32)));
        }        
        STATS_INC(tls_exception);
        /* We leave as unaddressable since we're not tracking the unset so we
         * can't safely mark as addressable */
        return tls_ok;
    }
#else
    if (is_loader_exception(loc, addr, sz)) {
        return true;
    }
#endif
    else if (is_ok_unaddressable_pattern(write, loc, addr, sz)) {
        return true;
    }
    return false;
}

/***************************************************************************
 * HEAP REGION
 */

#ifdef WINDOWS
void
client_remove_malloc_on_destroy(HANDLE heap, byte *start, byte *end)
{
    leak_remove_malloc_on_destroy(heap, start, end);
}
#endif

void
handle_new_heap_region(app_pc start, app_pc end, dr_mcontext_t *mc)
{
    report_heap_region(true/*add*/, start, end, mc);
}

void
handle_removed_heap_region(app_pc start, app_pc end, dr_mcontext_t *mc)
{
    report_heap_region(false/*remove*/, start, end, mc);
}

/***************************************************************************
 * LEAK CHECKING
 */

void
client_exit_iter_chunk(app_pc start, app_pc end, bool pre_us, uint client_flags,
                       void *client_data)
{
    /* don't report leaks if we never scanned (could have bailed for PR 574018) */
    if (!options.leaks_only && !options.shadowing)
        return;
    if (options.count_leaks)
        leak_exit_iter_chunk(start, end, pre_us, client_flags, client_data);
}

void
client_found_leak(app_pc start, app_pc end, bool pre_us, bool reachable,
                  bool maybe_reachable, void *client_data)
{
    packed_callstack_t *pcs = (packed_callstack_t *) client_data;
    report_leak(true, start, end - start, pre_us, reachable,
                maybe_reachable, SHADOW_UNKNOWN, pcs);
}

static byte *
next_defined_dword(byte *start, byte *end)
{
    return shadow_next_dword((byte *)ALIGN_FORWARD(start, 4), end, SHADOW_DEFINED);
}

static byte *
end_of_defined_region(byte *start, byte *end)
{
    byte *res;
    if (shadow_check_range(start, end - start, SHADOW_DEFINED, &res, NULL, NULL))
        res = end;
    return res;
}

static bool
is_register_defined(void *drcontext, reg_id_t reg)
{
    return is_shadow_register_defined(get_thread_shadow_register(drcontext, reg));
}

void
check_reachability(bool at_exit)
{
    /* no point in scanning unless we have leaks-only info or full shadowing */
    if (!options.leaks_only && !options.shadowing)
        return;
    if (!options.count_leaks)
        return;
    leak_scan_for_leaks(at_exit);
}

