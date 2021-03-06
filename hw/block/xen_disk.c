/*
 *  xen paravirt block device backend
 *
 *  (c) Gerd Hoffmann <kraxel@redhat.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 *  Contributions after 2012-01-13 are licensed under the terms of the
 *  GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include <sys/uio.h>

#include "hw/hw.h"
#include "hw/xen/xen_backend.h"
#include "xen_blkif.h"
#include "sysemu/blockdev.h"
#include "sysemu/iothread.h"
#include "sysemu/block-backend.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qstring.h"
#include "trace.h"

/* ------------------------------------------------------------- */

#define BLOCK_SIZE  512
#define IOCB_COUNT  (BLKIF_MAX_SEGMENTS_PER_REQUEST + 2)

struct ioreq {
    blkif_request_t     req;
    int16_t             status;

    /* parsed request */
    off_t               start;
    QEMUIOVector        v;
    int                 presync;

    /* grant mapping */
    uint32_t            refs[BLKIF_MAX_SEGMENTS_PER_REQUEST];
    void                *page[BLKIF_MAX_SEGMENTS_PER_REQUEST];
    void                *pages;

    /* aio status */
    int                 aio_inflight;
    int                 aio_errors;

    struct XenBlkDev    *blkdev;
    QLIST_ENTRY(ioreq)   list;
    BlockAcctCookie     acct;
};

#define MAX_RING_PAGE_ORDER 4

struct XenBlkDev {
    struct XenDevice    xendev;  /* must be first */
    char                *params;
    char                *mode;
    char                *type;
    char                *dev;
    char                *devtype;
    bool                directiosafe;
    const char          *fileproto;
    const char          *filename;
    const char          *devicename;
    const char          *nodename;
    unsigned int        ring_ref[1 << MAX_RING_PAGE_ORDER];
    unsigned int        nr_ring_ref;
    void                *sring;
    int64_t             file_blk;
    int64_t             file_size;
    int                 protocol;
    blkif_back_rings_t  rings;
    int                 more_work;
    unsigned int        errcount;

    /* request lists */
    QLIST_HEAD(inflight_head, ioreq) inflight;
    QLIST_HEAD(freelist_head, ioreq) freelist;
    int                 requests_total;
    int                 requests_inflight;
    unsigned int        max_requests;

    gboolean            feature_discard;

    /* qemu block driver */
    DriveInfo           *dinfo;
    BlockBackend        *blk;
    QEMUBH              *bh;
};

/* Threshold of in-flight requests above which we will start using
 * blk_io_plug()/blk_io_unplug() to batch requests */
#define IO_PLUG_THRESHOLD 1
static int blk_send_response(struct ioreq *ioreq);
/* ------------------------------------------------------------- */

static void ioreq_reset(struct ioreq *ioreq)
{
    memset(&ioreq->req, 0, sizeof(ioreq->req));
    ioreq->status = 0;
    ioreq->start = 0;
    ioreq->presync = 0;

    memset(ioreq->refs, 0, sizeof(ioreq->refs));
    memset(ioreq->page, 0, sizeof(ioreq->page));

    ioreq->aio_inflight = 0;
    ioreq->aio_errors = 0;

    ioreq->blkdev = NULL;
    memset(&ioreq->list, 0, sizeof(ioreq->list));
    memset(&ioreq->acct, 0, sizeof(ioreq->acct));

    qemu_iovec_reset(&ioreq->v);
}

static struct ioreq *ioreq_start(struct XenBlkDev *blkdev)
{
    struct ioreq *ioreq = NULL;

    if (QLIST_EMPTY(&blkdev->freelist)) {
        if (blkdev->requests_total >= blkdev->max_requests) {
            goto out;
        }
        /* allocate new struct */
        ioreq = g_malloc0(sizeof(*ioreq));
        ioreq->blkdev = blkdev;
        /* We cannot need more pages per ioreq than this, and we do re-use ioreqs,
         * so allocate the memory once here, to be freed in blk_free() when the
         * ioreq is freed. */
        ioreq->pages = qemu_memalign(XC_PAGE_SIZE, BLKIF_MAX_SEGMENTS_PER_REQUEST * XC_PAGE_SIZE);
        blkdev->requests_total++;
        qemu_iovec_init(&ioreq->v, BLKIF_MAX_SEGMENTS_PER_REQUEST);
    } else {
        /* get one from freelist */
        ioreq = QLIST_FIRST(&blkdev->freelist);
        QLIST_REMOVE(ioreq, list);
    }
    QLIST_INSERT_HEAD(&blkdev->inflight, ioreq, list);
    blkdev->requests_inflight++;

out:
    return ioreq;
}

static void ioreq_finish(struct ioreq *ioreq)
{
    struct XenBlkDev *blkdev = ioreq->blkdev;

    QLIST_REMOVE(ioreq, list);
    blkdev->requests_inflight--;
}

static void ioreq_release(struct ioreq *ioreq)
{
    struct XenBlkDev *blkdev = ioreq->blkdev;

    QLIST_REMOVE(ioreq, list);
    ioreq_reset(ioreq);
    ioreq->blkdev = blkdev;
    QLIST_INSERT_HEAD(&blkdev->freelist, ioreq, list);
    blkdev->requests_inflight--;
}

/* Avoid log flooding of errors by turning them
 * raising the required log level if we have had too
 * many consecutive ones. Avoids flooding logs
 * when the ring goes berserk */
#define ERT(a) ( ((a->errcount) < 16)?0:3 )

/*
 * translate request into iovec + start offset
 * do sanity checks along the way
 */
static int ioreq_parse(struct ioreq *ioreq)
{
    struct XenBlkDev *blkdev = ioreq->blkdev;
    uintptr_t mem;
    size_t len;
    int i;

    xen_pv_printf(&blkdev->xendev, 3,
                  "op %d, nr %d, handle %d, id %" PRId64 ", sector %" PRId64 "\n",
                  ioreq->req.operation, ioreq->req.nr_segments,
                  ioreq->req.handle, ioreq->req.id, ioreq->req.sector_number);
    switch (ioreq->req.operation) {
    case BLKIF_OP_READ:
        break;
    case BLKIF_OP_FLUSH_DISKCACHE:
        ioreq->presync = 1;
        if (!ioreq->req.nr_segments) {
            goto out;
        }
        /* fall through */
    case BLKIF_OP_WRITE:
        break;
    case BLKIF_OP_DISCARD:
        goto out;
    default:
        xen_pv_printf(&blkdev->xendev, ERT(blkdev), "error: unknown operation (%d)\n",
                      ioreq->req.operation);
        goto err;
    };

    if (ioreq->req.operation != BLKIF_OP_READ && blkdev->mode[0] != 'w') {
        xen_pv_printf(&blkdev->xendev, ERT(blkdev), "error: write req for ro device\n");
        goto err;
    }

    ioreq->start = ioreq->req.sector_number * blkdev->file_blk;
    for (i = 0; i < ioreq->req.nr_segments; i++) {
        if (i == BLKIF_MAX_SEGMENTS_PER_REQUEST) {
            xen_pv_printf(&blkdev->xendev, ERT(blkdev), "error: nr_segments too big\n");
            goto err;
        }
        if (ioreq->req.seg[i].first_sect > ioreq->req.seg[i].last_sect) {
            xen_pv_printf(&blkdev->xendev, ERT(blkdev), "error: first > last sector\n");
            goto err;
        }
        if (ioreq->req.seg[i].last_sect * BLOCK_SIZE >= XC_PAGE_SIZE) {
            xen_pv_printf(&blkdev->xendev, ERT(blkdev), "error: page crossing\n");
            goto err;
        }

        ioreq->refs[i]   = ioreq->req.seg[i].gref;

        mem = ioreq->req.seg[i].first_sect * blkdev->file_blk;
        len = (ioreq->req.seg[i].last_sect - ioreq->req.seg[i].first_sect + 1) * blkdev->file_blk;
        qemu_iovec_add(&ioreq->v, (void*)mem, len);
    }
    if (ioreq->start + ioreq->v.size > blkdev->file_size) {
        xen_pv_printf(&blkdev->xendev, ERT(blkdev), "error: access beyond end of file\n");
        goto err;
    }
out:
    blkdev->errcount = 0;
    return 0;

err:
    blkdev->errcount++;
    ioreq->status = BLKIF_RSP_ERROR;
    return -1;
}

static void ioreq_free_copy_buffers(struct ioreq *ioreq)
{
    int i;

    for (i = 0; i < ioreq->v.niov; i++) {
        ioreq->page[i] = NULL;
    }
}

static int ioreq_init_copy_buffers(struct ioreq *ioreq)
{
    int i;

    if (ioreq->v.niov == 0) {
        return 0;
    }

    for (i = 0; i < ioreq->v.niov; i++) {
        ioreq->page[i] = ioreq->pages + i * XC_PAGE_SIZE;
        ioreq->v.iov[i].iov_base = ioreq->page[i];
    }

    return 0;
}

static int ioreq_grant_copy(struct ioreq *ioreq)
{
    struct XenBlkDev *blkdev = ioreq->blkdev;
    struct XenDevice *xendev = &blkdev->xendev;
    XenGrantCopySegment segs[BLKIF_MAX_SEGMENTS_PER_REQUEST];
    int i, count, rc;
    int64_t file_blk = ioreq->blkdev->file_blk;
    bool to_domain = (ioreq->req.operation == BLKIF_OP_READ);

    if (ioreq->v.niov == 0) {
        return 0;
    }

    count = ioreq->v.niov;

    for (i = 0; i < count; i++) {
        if (to_domain) {
            segs[i].dest.foreign.ref = ioreq->refs[i];
            segs[i].dest.foreign.offset = ioreq->req.seg[i].first_sect * file_blk;
            segs[i].source.virt = ioreq->v.iov[i].iov_base;
        } else {
            segs[i].source.foreign.ref = ioreq->refs[i];
            segs[i].source.foreign.offset = ioreq->req.seg[i].first_sect * file_blk;
            segs[i].dest.virt = ioreq->v.iov[i].iov_base;
        }
        segs[i].len = (ioreq->req.seg[i].last_sect
                       - ioreq->req.seg[i].first_sect + 1) * file_blk;
    }

    rc = xen_be_copy_grant_refs(xendev, to_domain, segs, count);

    if (rc) {
        xen_pv_printf(&ioreq->blkdev->xendev, 0,
                      "failed to copy data %d (%d)\n", rc, errno);
        ioreq->aio_errors++;
        return -1;
    }

    return rc;
}

static int ioreq_runio_qemu_aio(struct ioreq *ioreq);

static void qemu_aio_complete(void *opaque, int ret)
{
    struct ioreq *ioreq = opaque;
    struct XenBlkDev *blkdev = ioreq->blkdev;

    aio_context_acquire(blk_get_aio_context(blkdev->blk));

    if (ret != 0) {
        xen_pv_printf(&blkdev->xendev, 0, "%s I/O error\n",
                      ioreq->req.operation == BLKIF_OP_READ ? "read" : "write");
        ioreq->aio_errors++;
    }

    ioreq->aio_inflight--;
    if (ioreq->presync) {
        ioreq->presync = 0;
        ioreq_runio_qemu_aio(ioreq);
        goto done;
    }
    if (ioreq->aio_inflight > 0) {
        goto done;
    }

    switch (ioreq->req.operation) {
    case BLKIF_OP_READ:
        /* in case of failure ioreq->aio_errors is increased */
        if (ret == 0) {
            ioreq_grant_copy(ioreq);
        }
        ioreq_free_copy_buffers(ioreq);
        break;
    case BLKIF_OP_WRITE:
    case BLKIF_OP_FLUSH_DISKCACHE:
        if (!ioreq->req.nr_segments) {
            break;
        }
        ioreq_free_copy_buffers(ioreq);
        break;
    default:
        break;
    }

    ioreq->status = ioreq->aio_errors ? BLKIF_RSP_ERROR : BLKIF_RSP_OKAY;
    ioreq_finish(ioreq);

    switch (ioreq->req.operation) {
    case BLKIF_OP_WRITE:
    case BLKIF_OP_FLUSH_DISKCACHE:
        if (!ioreq->req.nr_segments) {
            break;
        }
    case BLKIF_OP_READ:
        if (ioreq->status == BLKIF_RSP_OKAY) {
            block_acct_done(blk_get_stats(blkdev->blk), &ioreq->acct);
        } else {
            block_acct_failed(blk_get_stats(blkdev->blk), &ioreq->acct);
        }
        break;
    case BLKIF_OP_DISCARD:
    default:
        break;
    }
    /* If the I/O ring is full, the guest cannot send any more
     * requests until some responses are sent. So, whenever an
     * ioreq completes, just send everything we have ready.
     * In particular, if this was a read, not sending it at once
     * just adds read latency for the guest.
     */
    if (blk_send_response(ioreq)) {
        xen_pv_send_notify(&blkdev->xendev);
    }
    ioreq_release(ioreq);
    qemu_bh_schedule(blkdev->bh);

done:
    aio_context_release(blk_get_aio_context(blkdev->blk));
}

static bool blk_split_discard(struct ioreq *ioreq, blkif_sector_t sector_number,
                              uint64_t nr_sectors)
{
    struct XenBlkDev *blkdev = ioreq->blkdev;
    int64_t byte_offset;
    int byte_chunk;
    uint64_t byte_remaining, limit;
    uint64_t sec_start = sector_number;
    uint64_t sec_count = nr_sectors;

    /* Wrap around, or overflowing byte limit? */
    if (sec_start + sec_count < sec_count ||
        sec_start + sec_count > INT64_MAX >> BDRV_SECTOR_BITS) {
        return false;
    }

    limit = BDRV_REQUEST_MAX_SECTORS << BDRV_SECTOR_BITS;
    byte_offset = sec_start << BDRV_SECTOR_BITS;
    byte_remaining = sec_count << BDRV_SECTOR_BITS;

    do {
        byte_chunk = byte_remaining > limit ? limit : byte_remaining;
        ioreq->aio_inflight++;
        blk_aio_pdiscard(blkdev->blk, byte_offset, byte_chunk,
                         qemu_aio_complete, ioreq);
        byte_remaining -= byte_chunk;
        byte_offset += byte_chunk;
    } while (byte_remaining > 0);

    return true;
}

static int ioreq_runio_qemu_aio(struct ioreq *ioreq)
{
    struct XenBlkDev *blkdev = ioreq->blkdev;

    ioreq_init_copy_buffers(ioreq);
    if (ioreq->req.nr_segments &&
        (ioreq->req.operation == BLKIF_OP_WRITE ||
         ioreq->req.operation == BLKIF_OP_FLUSH_DISKCACHE) &&
        ioreq_grant_copy(ioreq)) {
        ioreq_free_copy_buffers(ioreq);
        goto err;
    }

    ioreq->aio_inflight++;
    if (ioreq->presync) {
        blk_aio_flush(ioreq->blkdev->blk, qemu_aio_complete, ioreq);
        return 0;
    }

    switch (ioreq->req.operation) {
    case BLKIF_OP_READ:
        block_acct_start(blk_get_stats(blkdev->blk), &ioreq->acct,
                         ioreq->v.size, BLOCK_ACCT_READ);
        ioreq->aio_inflight++;
        blk_aio_preadv(blkdev->blk, ioreq->start, &ioreq->v, 0,
                       qemu_aio_complete, ioreq);
        break;
    case BLKIF_OP_WRITE:
    case BLKIF_OP_FLUSH_DISKCACHE:
        if (!ioreq->req.nr_segments) {
            break;
        }

        block_acct_start(blk_get_stats(blkdev->blk), &ioreq->acct,
                         ioreq->v.size,
                         ioreq->req.operation == BLKIF_OP_WRITE ?
                         BLOCK_ACCT_WRITE : BLOCK_ACCT_FLUSH);
        ioreq->aio_inflight++;
        blk_aio_pwritev(blkdev->blk, ioreq->start, &ioreq->v, 0,
                        qemu_aio_complete, ioreq);
        break;
    case BLKIF_OP_DISCARD:
    {
        struct blkif_request_discard *req = (void *)&ioreq->req;
        if (!blk_split_discard(ioreq, req->sector_number, req->nr_sectors)) {
            goto err;
        }
        break;
    }
    default:
        /* unknown operation (shouldn't happen -- parse catches this) */
        goto err;
    }

    qemu_aio_complete(ioreq, 0);

    return 0;

err:
    ioreq_finish(ioreq);
    ioreq->status = BLKIF_RSP_ERROR;
    return -1;
}

static int blk_send_response(struct ioreq *ioreq)
{
    struct XenBlkDev  *blkdev = ioreq->blkdev;
    int               send_notify   = 0;
    int               have_requests = 0;
    blkif_response_t  *resp;

    /* Place on the response ring for the relevant domain. */
    switch (blkdev->protocol) {
    case BLKIF_PROTOCOL_NATIVE:
        resp = (blkif_response_t *) RING_GET_RESPONSE(&blkdev->rings.native,
                                 blkdev->rings.native.rsp_prod_pvt);
        break;
    case BLKIF_PROTOCOL_X86_32:
        resp = (blkif_response_t *) RING_GET_RESPONSE(&blkdev->rings.x86_32_part,
                                 blkdev->rings.x86_32_part.rsp_prod_pvt);
        break;
    case BLKIF_PROTOCOL_X86_64:
        resp = (blkif_response_t *) RING_GET_RESPONSE(&blkdev->rings.x86_64_part,
                                 blkdev->rings.x86_64_part.rsp_prod_pvt);
        break;
    default:
        return 0;
    }

    resp->id        = ioreq->req.id;
    resp->operation = ioreq->req.operation;
    resp->status    = ioreq->status;

    blkdev->rings.common.rsp_prod_pvt++;

    RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&blkdev->rings.common, send_notify);
    if (blkdev->rings.common.rsp_prod_pvt == blkdev->rings.common.req_cons) {
        /*
         * Tail check for pending requests. Allows frontend to avoid
         * notifications if requests are already in flight (lower
         * overheads and promotes batching).
         */
        RING_FINAL_CHECK_FOR_REQUESTS(&blkdev->rings.common, have_requests);
    } else if (RING_HAS_UNCONSUMED_REQUESTS(&blkdev->rings.common)) {
        have_requests = 1;
    }

    if (have_requests) {
        blkdev->more_work++;
    }
    return send_notify;
}

static int blk_get_request(struct XenBlkDev *blkdev, struct ioreq *ioreq, RING_IDX rc)
{
    switch (blkdev->protocol) {
    case BLKIF_PROTOCOL_NATIVE:
        memcpy(&ioreq->req, RING_GET_REQUEST(&blkdev->rings.native, rc),
               sizeof(ioreq->req));
        break;
    case BLKIF_PROTOCOL_X86_32:
        blkif_get_x86_32_req(&ioreq->req,
                             RING_GET_REQUEST(&blkdev->rings.x86_32_part, rc));
        break;
    case BLKIF_PROTOCOL_X86_64:
        blkif_get_x86_64_req(&ioreq->req,
                             RING_GET_REQUEST(&blkdev->rings.x86_64_part, rc));
        break;
    }
    /* Prevent the compiler from accessing the on-ring fields instead. */
    barrier();
    return 0;
}

static void blk_handle_requests(struct XenBlkDev *blkdev)
{
    RING_IDX rc, rp;
    struct ioreq *ioreq;
    int inflight_atstart = blkdev->requests_inflight;
    int batched = 0;

    blkdev->more_work = 0;

    rc = blkdev->rings.common.req_cons;
    rp = blkdev->rings.common.sring->req_prod;
    xen_rmb(); /* Ensure we see queued requests up to 'rp'. */

    /* If there was more than one ioreq in flight when we got here, this
     * is an indication that there the bottleneck is below us, so it's worth
     * beginning to batch up I/O requests rather than submitting them
     * immediately. The maximum number of requests we're willing to batch
     * is the number already in flight, so it can grow up to max_requests
     * when the bottleneck is below us */
    if (inflight_atstart > IO_PLUG_THRESHOLD) {
        blk_io_plug(blkdev->blk);
    }
    while (rc != rp) {
        /* pull request from ring */
        if (RING_REQUEST_CONS_OVERFLOW(&blkdev->rings.common, rc)) {
            break;
        }
        ioreq = ioreq_start(blkdev);
        if (ioreq == NULL) {
            blkdev->more_work++;
            break;
        }
        blk_get_request(blkdev, ioreq, rc);
        blkdev->rings.common.req_cons = ++rc;

        /* parse them */
        if (ioreq_parse(ioreq) != 0) {

            switch (ioreq->req.operation) {
            case BLKIF_OP_READ:
                block_acct_invalid(blk_get_stats(blkdev->blk),
                                   BLOCK_ACCT_READ);
                break;
            case BLKIF_OP_WRITE:
                block_acct_invalid(blk_get_stats(blkdev->blk),
                                   BLOCK_ACCT_WRITE);
                break;
            case BLKIF_OP_FLUSH_DISKCACHE:
                block_acct_invalid(blk_get_stats(blkdev->blk),
                                   BLOCK_ACCT_FLUSH);
            default:
                break;
            };

            if (blk_send_response(ioreq)) {
                xen_pv_send_notify(&blkdev->xendev);
            }
            ioreq_release(ioreq);
            continue;
        }

        if (inflight_atstart > IO_PLUG_THRESHOLD && batched >= inflight_atstart) {
            blk_io_unplug(blkdev->blk);
        }
        ioreq_runio_qemu_aio(ioreq);
        if (inflight_atstart > IO_PLUG_THRESHOLD) {
            if (batched >= inflight_atstart) {
                blk_io_plug(blkdev->blk);
                batched=0;
            } else {
                batched++;
            }
        }
    }
    if (inflight_atstart > IO_PLUG_THRESHOLD) {
        blk_io_unplug(blkdev->blk);
    }

    if (blkdev->more_work && blkdev->requests_inflight < blkdev->max_requests) {
        qemu_bh_schedule(blkdev->bh);
    }
}

/* ------------------------------------------------------------- */

static void blk_bh(void *opaque)
{
    struct XenBlkDev *blkdev = opaque;

    aio_context_acquire(blk_get_aio_context(blkdev->blk));
    blk_handle_requests(blkdev);
    aio_context_release(blk_get_aio_context(blkdev->blk));
}

static void blk_alloc(struct XenDevice *xendev)
{
    struct XenBlkDev *blkdev = container_of(xendev, struct XenBlkDev, xendev);

    trace_xen_disk_alloc(xendev->name);

    QLIST_INIT(&blkdev->inflight);
    QLIST_INIT(&blkdev->freelist);
}

static void blk_parse_discard(struct XenBlkDev *blkdev)
{
    int enable;

    blkdev->feature_discard = true;

    if (xenstore_read_be_int(&blkdev->xendev, "discard-enable", &enable) == 0) {
        blkdev->feature_discard = !!enable;
    }

    if (blkdev->feature_discard) {
        xenstore_write_be_int(&blkdev->xendev, "feature-discard", 1);
    }
}

static int blk_init(struct XenDevice *xendev)
{
    struct XenBlkDev *blkdev = container_of(xendev, struct XenBlkDev, xendev);
    int info = 0;
    char *directiosafe = NULL;

    trace_xen_disk_init(xendev->name);

    /* read xenstore entries */
    if (blkdev->params == NULL) {
        char *h = NULL;
        blkdev->params = xenstore_read_be_str(&blkdev->xendev, "params");
        if (blkdev->params != NULL) {
            h = strchr(blkdev->params, ':');
        }
        if (h != NULL) {
            blkdev->fileproto = blkdev->params;
            blkdev->filename  = h+1;
            *h = 0;
        } else {
            blkdev->fileproto = "<unset>";
            blkdev->filename  = blkdev->params;
        }
    }
    if (xendev->blocknode) {
        /* override "params" */
        blkdev->devicename = xendev->devicename;
        blkdev->nodename = xendev->blocknode;
        blkdev->filename = NULL;
    }
    if (!strcmp("aio", blkdev->fileproto)) {
        blkdev->fileproto = "raw";
    }
    if (!strcmp("vhd", blkdev->fileproto)) {
        blkdev->fileproto = "vpc";
    }
    if (blkdev->mode == NULL) {
        blkdev->mode = xenstore_read_be_str(&blkdev->xendev, "mode");
    }
    if (blkdev->type == NULL) {
        blkdev->type = xenstore_read_be_str(&blkdev->xendev, "type");
    }
    if (blkdev->dev == NULL) {
        blkdev->dev = xenstore_read_be_str(&blkdev->xendev, "dev");
    }
    if (blkdev->devtype == NULL) {
        blkdev->devtype = xenstore_read_be_str(&blkdev->xendev, "device-type");
    }
    directiosafe = xenstore_read_be_str(&blkdev->xendev, "direct-io-safe");
    blkdev->directiosafe = (directiosafe && atoi(directiosafe));

    /* do we have all we need? */
    if (blkdev->params == NULL ||
        blkdev->mode == NULL   ||
        blkdev->type == NULL   ||
        blkdev->dev == NULL) {
        goto out_error;
    }

    /* read-only ? */
    if (strcmp(blkdev->mode, "w")) {
        info  |= VDISK_READONLY;
    }

    /* cdrom ? */
    if (blkdev->devtype && !strcmp(blkdev->devtype, "cdrom")) {
        info  |= VDISK_CDROM;
    }

    blkdev->file_blk  = BLOCK_SIZE;

    /* fill info
     * blk_connect supplies sector-size and sectors
     */
    xenstore_write_be_int(&blkdev->xendev, "feature-flush-cache", 1);
    xenstore_write_be_int(&blkdev->xendev, "info", info);

    xenstore_write_be_int(&blkdev->xendev, "max-ring-page-order",
                          MAX_RING_PAGE_ORDER);

    blk_parse_discard(blkdev);

    g_free(directiosafe);
    return 0;

out_error:
    g_free(blkdev->params);
    blkdev->params = NULL;
    g_free(blkdev->mode);
    blkdev->mode = NULL;
    g_free(blkdev->type);
    blkdev->type = NULL;
    g_free(blkdev->dev);
    blkdev->dev = NULL;
    g_free(blkdev->devtype);
    blkdev->devtype = NULL;
    g_free(directiosafe);
    blkdev->directiosafe = false;
    return -1;
}

static int blk_connect(struct XenDevice *xendev)
{
    struct XenBlkDev *blkdev = container_of(xendev, struct XenBlkDev, xendev);
    int index, qflags;
    bool readonly = true;
    bool writethrough = true;
    int order, ring_ref;
    unsigned int ring_size, max_grants;
    unsigned int i;
    AioContext *ctx = NULL;

    trace_xen_disk_connect(xendev->name);

    /* read-only ? */
    if (blkdev->directiosafe) {
        qflags = BDRV_O_NOCACHE | BDRV_O_NATIVE_AIO;
    } else {
        qflags = 0;
        writethrough = false;
    }
    if (strcmp(blkdev->mode, "w") == 0) {
        qflags |= BDRV_O_RDWR;
        readonly = false;
    }
    if (blkdev->feature_discard) {
        qflags |= BDRV_O_UNMAP;
    }

    /* init qemu block driver */
    index = (blkdev->xendev.dev - 202 * 256) / 16;
    blkdev->dinfo = drive_get(IF_XEN, 0, index);
    if (!blkdev->dinfo) {
        Error *local_err = NULL;
        QDict *options = NULL;

        if (strcmp(blkdev->fileproto, "<unset>")) {
            options = qdict_new();
            qdict_put_str(options, "driver", blkdev->fileproto);
        }
        if (blkdev->nodename) {
            xen_pv_printf(&blkdev->xendev, 2, "looking up blockdev node %s\n", blkdev->nodename);
            options = NULL;
        }

        /* setup via xenbus -> create new block driver instance */
        xen_pv_printf(&blkdev->xendev, 2, "create new bdrv (xenbus setup)\n");
        blkdev->blk = blk_new_open(blkdev->filename, blkdev->nodename, options,
                                   qflags, &local_err);
        if (!blkdev->blk) {
            xen_pv_printf(&blkdev->xendev, 0, "error: %s\n",
                          error_get_pretty(local_err));
            error_free(local_err);
            return -1;
        }
        /* Acquire the AIO context as soon as we know what it is */
        ctx = blk_get_aio_context(blkdev->blk);
        aio_context_acquire(ctx);
        blk_set_enable_write_cache(blkdev->blk, !writethrough);
    } else {
        /* setup via qemu cmdline -> already setup for us */
        xen_pv_printf(&blkdev->xendev, 2,
                      "get configured bdrv (cmdline setup)\n");
        blkdev->blk = blk_by_legacy_dinfo(blkdev->dinfo);

        /* Acquire the AIO context as soon as we know what it is */
        ctx = blk_get_aio_context(blkdev->blk);
        aio_context_acquire(ctx);
        if (blk_is_read_only(blkdev->blk) && !readonly) {
            xen_pv_printf(&blkdev->xendev, 0, "Unexpected read-only drive");
            blkdev->blk = NULL;
            goto error_ctx_release;
        }
        /* blkdev->blk is not create by us, we get a reference
         * so we can blk_unref() unconditionally */
        blk_ref(blkdev->blk);
    }
    blk_attach_dev_legacy(blkdev->blk, blkdev);
    blkdev->file_size = blk_getlength(blkdev->blk);
    if (blkdev->file_size < 0) {
        BlockDriverState *bs = blk_bs(blkdev->blk);
        const char *drv_name = bs ? bdrv_get_format_name(bs) : NULL;
        xen_pv_printf(&blkdev->xendev, 1, "blk_getlength: %d (%s) | drv %s\n",
                      (int)blkdev->file_size, strerror(-blkdev->file_size),
                      drv_name ?: "-");
        blkdev->file_size = 0;
    }

    xen_pv_printf(xendev, 1, "type \"%s\", fileproto \"%s\", filename \"%s\","
                  " size %" PRId64 " (%" PRId64 " MB)\n",
                  blkdev->type, blkdev->fileproto, blkdev->filename,
                  blkdev->file_size, blkdev->file_size >> 20);

    /* Fill in number of sector size and number of sectors */
    xenstore_write_be_int(&blkdev->xendev, "sector-size", blkdev->file_blk);
    xenstore_write_be_int64(&blkdev->xendev, "sectors",
                            blkdev->file_size / blkdev->file_blk);

    if (xenstore_read_fe_int(&blkdev->xendev, "ring-page-order",
                             &order) == -1) {
        blkdev->nr_ring_ref = 1;

        if (xenstore_read_fe_int(&blkdev->xendev, "ring-ref",
                                 &ring_ref) == -1) {
            goto error_ctx_release;
        }
        blkdev->ring_ref[0] = ring_ref;

    } else if (order >= 0 && order <= MAX_RING_PAGE_ORDER) {
        blkdev->nr_ring_ref = 1 << order;

        for (i = 0; i < blkdev->nr_ring_ref; i++) {
            char *key;

            key = g_strdup_printf("ring-ref%u", i);
            if (!key) {
                goto error_ctx_release;
            }

            if (xenstore_read_fe_int(&blkdev->xendev, key,
                                     &ring_ref) == -1) {
                g_free(key);
                goto error_ctx_release;
            }
            blkdev->ring_ref[i] = ring_ref;

            g_free(key);
        }
    } else {
        xen_pv_printf(xendev, 0, "invalid ring-page-order: %d\n",
                      order);
        goto error_ctx_release;
    }

    if (xenstore_read_fe_int(&blkdev->xendev, "event-channel",
                             &blkdev->xendev.remote_port) == -1) {
        goto error_ctx_release;
    }

    if (!blkdev->xendev.protocol) {
        blkdev->protocol = BLKIF_PROTOCOL_NATIVE;
    } else if (strcmp(blkdev->xendev.protocol, XEN_IO_PROTO_ABI_NATIVE) == 0) {
        blkdev->protocol = BLKIF_PROTOCOL_NATIVE;
    } else if (strcmp(blkdev->xendev.protocol, XEN_IO_PROTO_ABI_X86_32) == 0) {
        blkdev->protocol = BLKIF_PROTOCOL_X86_32;
    } else if (strcmp(blkdev->xendev.protocol, XEN_IO_PROTO_ABI_X86_64) == 0) {
        blkdev->protocol = BLKIF_PROTOCOL_X86_64;
    } else {
        blkdev->protocol = BLKIF_PROTOCOL_NATIVE;
    }

    ring_size = XC_PAGE_SIZE * blkdev->nr_ring_ref;
    switch (blkdev->protocol) {
    case BLKIF_PROTOCOL_NATIVE:
    {
        blkdev->max_requests = __CONST_RING_SIZE(blkif, ring_size);
        break;
    }
    case BLKIF_PROTOCOL_X86_32:
    {
        blkdev->max_requests = __CONST_RING_SIZE(blkif_x86_32, ring_size);
        break;
    }
    case BLKIF_PROTOCOL_X86_64:
    {
        blkdev->max_requests = __CONST_RING_SIZE(blkif_x86_64, ring_size);
        break;
    }
    default:
        goto error_ctx_release;
    }

    /* Add on the number needed for the ring pages */
    max_grants = blkdev->nr_ring_ref;

    xen_be_set_max_grant_refs(xendev, max_grants);

    blkdev->sring = xen_be_map_grant_refs(xendev, blkdev->ring_ref,
                                          blkdev->nr_ring_ref,
                                          PROT_READ | PROT_WRITE);
    if (!blkdev->sring) {
        goto error_ctx_release;
    }

    switch (blkdev->protocol) {
    case BLKIF_PROTOCOL_NATIVE:
    {
        blkif_sring_t *sring_native = blkdev->sring;
        BACK_RING_INIT(&blkdev->rings.native, sring_native, ring_size);
        break;
    }
    case BLKIF_PROTOCOL_X86_32:
    {
        blkif_x86_32_sring_t *sring_x86_32 = blkdev->sring;

        BACK_RING_INIT(&blkdev->rings.x86_32_part, sring_x86_32, ring_size);
        break;
    }
    case BLKIF_PROTOCOL_X86_64:
    {
        blkif_x86_64_sring_t *sring_x86_64 = blkdev->sring;

        BACK_RING_INIT(&blkdev->rings.x86_64_part, sring_x86_64, ring_size);
        break;
    }
    }

    blkdev->bh = aio_bh_new(blk_get_aio_context(blkdev->blk), blk_bh, blkdev);

    xen_be_bind_evtchn(&blkdev->xendev);

    xen_pv_printf(&blkdev->xendev, 1, "ok: proto %s, nr-ring-ref %u, "
                  "remote port %d, local port %d\n",
                  blkdev->xendev.protocol, blkdev->nr_ring_ref,
                  blkdev->xendev.remote_port, blkdev->xendev.local_port);
    aio_context_release(ctx);
    return 0;

error_ctx_release:
    aio_context_release(ctx);
    return -1;
}

static void blk_disconnect(struct XenDevice *xendev)
{
    struct XenBlkDev *blkdev = container_of(xendev, struct XenBlkDev, xendev);

    trace_xen_disk_disconnect(xendev->name);

    if (blkdev->blk) {
        AioContext *ctx = blk_get_aio_context(blkdev->blk);
        BlockDriverState *bs = blk_bs(blkdev->blk);

        do {
            blk_handle_requests(blkdev);
        } while (blkdev->more_work);

        aio_context_acquire(ctx);

        if (bs) {
            /* Take steps to ensure that all I/O has finished. This code
             * is modelled on bdrv_set_aio_context()
             */
            aio_disable_external(ctx);
            bdrv_parent_drained_begin(bs, NULL);
            bdrv_drain(bs); /* ensure there are no in-flight requests */

            while (aio_poll(ctx, false)) {
                /* wait for all bottom halves to execute */
            }
            bdrv_parent_drained_end(bs, NULL);
        }

        blk_detach_dev(blkdev->blk, blkdev);
        blk_unref(blkdev->blk);
        blkdev->blk = NULL;
        qemu_bh_delete(blkdev->bh);
        aio_context_release(ctx);
    }
    xen_pv_unbind_evtchn(&blkdev->xendev);

    if (blkdev->sring) {
        xen_be_unmap_grant_refs(xendev, blkdev->sring,
                                blkdev->nr_ring_ref);
        blkdev->sring = NULL;
    }
}

static int blk_free(struct XenDevice *xendev)
{
    struct XenBlkDev *blkdev = container_of(xendev, struct XenBlkDev, xendev);
    struct ioreq *ioreq;

    trace_xen_disk_free(xendev->name);

    blk_disconnect(xendev);

    while (!QLIST_EMPTY(&blkdev->freelist)) {
        ioreq = QLIST_FIRST(&blkdev->freelist);
        QLIST_REMOVE(ioreq, list);
        qemu_iovec_destroy(&ioreq->v);
        qemu_vfree(ioreq->pages);
        g_free(ioreq);
    }

    g_free(blkdev->params);
    g_free(blkdev->mode);
    g_free(blkdev->type);
    g_free(blkdev->dev);
    g_free(blkdev->devtype);
    return 0;
}

static void blk_event(struct XenDevice *xendev)
{
    struct XenBlkDev *blkdev = container_of(xendev, struct XenBlkDev, xendev);

    qemu_bh_schedule(blkdev->bh);
}

struct XenDevOps xen_blkdev_ops = {
    .flags      = DEVOPS_FLAG_NEED_GNTDEV,
    .size       = sizeof(struct XenBlkDev),
    .alloc      = blk_alloc,
    .init       = blk_init,
    .initialise = blk_connect,
    .disconnect = blk_disconnect,
    .event      = blk_event,
    .free       = blk_free,
};
