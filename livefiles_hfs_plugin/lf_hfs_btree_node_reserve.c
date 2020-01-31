//
//  lf_hfs_btree_node_reserve.c
//  livefiles_hfs
//
//  Created by Yakov Ben Zaken on 22/03/2018.
//

#include <stdio.h>
#include "lf_hfs_btrees_private.h"
#include "lf_hfs_utils.h"
#include "lf_hfs_vfsutils.h"

/*
 * B-tree Node Reserve
 *
 * BTReserveSpace
 * BTReleaseReserve
 * BTUpdateReserve
 *
 * Each kernel thread can have it's own reserve of b-tree
 * nodes. This reserve info is kept in a hash table.
 *
 * Don't forget to call BTReleaseReserve when you're finished
 * or you will leave stale node reserves in the hash.
 */


/*
 * BE CAREFUL WHEN INCREASING THE SIZE OF THIS STRUCT!
 *
 * It must remain equal in size to the opaque cat_cookie_t
 * struct (in hfs_catalog.h).
 */
struct nreserve {
    LIST_ENTRY(nreserve) nr_hash;  /* hash chain */
    int  nr_nodecnt;               /* count of nodes held in reserve */
    int  nr_newnodes;              /* nodes that were allocated */
    struct    vnode *nr_btvp;        /* b-tree file vnode */
    void  *nr_tag;                 /* unique tag (per thread) */
};

#define NR_GET_TAG()    (pthread_self())

#define    NR_CACHE 17

#define NR_HASH(btvp, tag) \
(&nr_hashtbl[((((intptr_t)(btvp)) >> 8) ^ ((intptr_t)(tag) >> 4)) & nr_hashmask])

LIST_HEAD(nodereserve, nreserve) *nr_hashtbl;

u_long nr_hashmask;

pthread_mutex_t  nr_mutex;

/* Internal Node Reserve Hash Routines (private) */
static void nr_insert (struct vnode *, struct nreserve *nrp, int);
static void nr_delete (struct vnode *, struct nreserve *nrp, int *);
static void nr_update (struct vnode *, int);


/*
 * BTReserveSetup - initialize the node reserve hash table
 */
void BTReserveSetup(void)
{
    if (sizeof(struct nreserve) != sizeof(cat_cookie_t))
    {
        LFHFS_LOG(LEVEL_ERROR,"BTReserveSetup: nreserve size != opaque struct size");
        hfs_assert(0);
    }

    nr_hashtbl = hashinit(NR_CACHE, &nr_hashmask);

    lf_lck_mtx_init(&nr_mutex);
}


/*
 * BTReserveSpace - obtain a node reserve (for current thread)
 *
 * Used by the Catalog Layer (hfs_catalog.c) to reserve space.
 *
 * When data is NULL, we only insure that there's enough space
 * but it is not reserved (assumes you keep the b-tree lock).
 */
int
BTReserveSpace(FCB *file, int operations, void* data)
{
    BTreeControlBlock *btree;
    int rsrvNodes, availNodes, totalNodes;
    int height;
    int inserts, deletes;
    u_int32_t clumpsize;
    int err = 0;

    btree = (BTreeControlBlockPtr)file->fcbBTCBPtr;
    clumpsize = file->ff_clumpsize;

    REQUIRE_FILE_LOCK(btree->fileRefNum, true);

    /*
     * The node reserve is based on the number of b-tree
     * operations (insert/deletes) and the height of the
     * tree.
     */
    height = btree->treeDepth;
    if (height < 2)
        height = 2;  /* prevent underflow in rsrvNodes calculation */
    inserts = operations & 0xffff;
    deletes = operations >> 16;

    /*
     * Allow for at least one root split.
     *
     * Each delete operation can propogate a big key up the
     * index. This can cause a split at each level up.
     *
     * Each insert operation can cause a local split and a
     * split at each level up.
     */
    rsrvNodes = 1 + (deletes * (height - 2)) + (inserts * (height - 1));

    availNodes = btree->freeNodes - btree->reservedNodes;

    if (rsrvNodes > availNodes) {
        u_int32_t reqblks, freeblks, rsrvblks;
        uint32_t bt_rsrv;
        struct hfsmount *hfsmp;

        /*
         * For UNIX conformance, we try and reserve the MIN of either 5% of
         * total file blocks or 10MB worth of blocks, for growing existing
         * files.  On non-HFS filesystems, creating a new directory entry may
         * not cause additional disk space to be allocated, but on HFS, creating
         * a new entry could cause the b-tree to grow.  As a result, we take
         * some precautions here to prevent that on configurations that try to
         * satisfy conformance.
         */
        hfsmp = VTOVCB(btree->fileRefNum);
        rsrvblks = (uint32_t)(((u_int64_t)hfsmp->allocLimit * 5) / 100);
        if (hfsmp->blockSize > HFS_BT_MAXRESERVE) {
            bt_rsrv = 1;
        }
        else {
            bt_rsrv = (HFS_BT_MAXRESERVE / hfsmp->blockSize);
        }
        rsrvblks = MIN(rsrvblks, bt_rsrv);

        freeblks = hfs_freeblks(hfsmp, 0);
        if (freeblks <= rsrvblks) {
            /* When running low, disallow adding new items. */
            if ((inserts > 0) && (deletes == 0)) {
                return (ENOSPC);
            }
            freeblks = 0;
        } else {
            freeblks -= rsrvblks;
        }
        reqblks = clumpsize / hfsmp->blockSize;

        if (reqblks > freeblks) {
            reqblks = ((rsrvNodes - availNodes) * btree->nodeSize) / hfsmp->blockSize;
            /* When running low, disallow adding new items. */
            if ((reqblks > freeblks) && (inserts > 0) && (deletes == 0)) {
                return (ENOSPC);
            }
            file->ff_clumpsize = freeblks * hfsmp->blockSize;
        }
        totalNodes = rsrvNodes + btree->totalNodes - availNodes;

        /* See if we also need a map node */
        if (totalNodes > (int)CalcMapBits(btree)) {
            ++totalNodes;
        }
        if ((err = ExtendBTree(btree, totalNodes))) {
            goto out;
        }
    }
    /* Save this reserve if this is a persistent request. */
    if (data) {
        btree->reservedNodes += rsrvNodes;
        nr_insert(btree->fileRefNum, (struct nreserve *)data, rsrvNodes);
    }
out:
    /* Put clump size back if it was changed. */
    if (file->ff_clumpsize != clumpsize)
        file->ff_clumpsize = clumpsize;

    return (err);
}


/*
 * BTReleaseReserve - release the node reserve held by current thread
 *
 * Used by the Catalog Layer (hfs_catalog.c) to relinquish reserved space.
 */
int
BTReleaseReserve(FCB *file, void* data)
{
    BTreeControlBlock *btree;
    int nodecnt;

    btree = (BTreeControlBlockPtr)file->fcbBTCBPtr;

    REQUIRE_FILE_LOCK(btree->fileRefNum, true);

    nr_delete(btree->fileRefNum, (struct nreserve *)data, &nodecnt);

    if (nodecnt)
        btree->reservedNodes -= nodecnt;

    return (0);
}

/*
 * BTUpdateReserve - update a node reserve for allocations that occurred.
 */
void
BTUpdateReserve(BTreeControlBlockPtr btreePtr, int nodes)
{
    nr_update(btreePtr->fileRefNum, nodes);
}


/*----------------------------------------------------------------------------*/
/* Node Reserve Hash Functions (private) */


int nrinserts = 0;
int nrdeletes = 0;

/*
 * Insert a new node reserve.
 */
static void
nr_insert(struct vnode * btvp, struct nreserve *nrp, int nodecnt)
{
    struct nodereserve *nrhead;
    struct nreserve *tmp_nrp;
    void * tag = NR_GET_TAG();

    /*
     * Check the cache - there may already be a reserve
     */
    lf_lck_mtx_lock(&nr_mutex);
    nrhead = NR_HASH(btvp, tag);
    for (tmp_nrp = nrhead->lh_first; tmp_nrp;
         tmp_nrp = tmp_nrp->nr_hash.le_next) {
        if ((tmp_nrp->nr_tag == tag) && (tmp_nrp->nr_btvp == btvp)) {
            nrp->nr_tag = 0;
            tmp_nrp->nr_nodecnt += nodecnt;
            lf_lck_mtx_unlock(&nr_mutex);
            return;
        }
    }

    nrp->nr_nodecnt = nodecnt;
    nrp->nr_newnodes = 0;
    nrp->nr_btvp = btvp;
    nrp->nr_tag = tag;
    LIST_INSERT_HEAD(nrhead, nrp, nr_hash);
    ++nrinserts;
    lf_lck_mtx_unlock(&nr_mutex);
}

/*
 * Delete a node reserve.
 */
static void
nr_delete(struct vnode * btvp, struct nreserve *nrp, int *nodecnt)
{
    void * tag = NR_GET_TAG();

    lf_lck_mtx_lock(&nr_mutex);
    if (nrp->nr_tag) {
        if ((nrp->nr_tag != tag) || (nrp->nr_btvp != btvp))
        {
            LFHFS_LOG(LEVEL_ERROR,"nr_delete: invalid NR (%p)", nrp);
            hfs_assert(0);
        }
        LIST_REMOVE(nrp, nr_hash);
        *nodecnt = nrp->nr_nodecnt;
        bzero(nrp, sizeof(struct nreserve));
        ++nrdeletes;
    } else {
        *nodecnt = 0;
    }
    lf_lck_mtx_unlock(&nr_mutex);
}


/*
 * Update a node reserve for any allocations that occurred.
 */
static void
nr_update(struct vnode * btvp, int nodecnt)
{
    struct nodereserve *nrhead;
    struct nreserve *nrp;
    void* tag = NR_GET_TAG();

    lf_lck_mtx_lock(&nr_mutex);

    nrhead = NR_HASH(btvp, tag);
    for (nrp = nrhead->lh_first; nrp; nrp = nrp->nr_hash.le_next) {
        if ((nrp->nr_tag == tag) && (nrp->nr_btvp == btvp)) {
            nrp->nr_newnodes += nodecnt;
            break;
        }
    }
    lf_lck_mtx_unlock(&nr_mutex);
}
