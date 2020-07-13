/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */
#include <postgres.h>
#include <access/relscan.h>
#include <access/xact.h>
#include <access/htup_details.h>
#include <executor/tuptable.h>
#include <storage/lmgr.h>
#include <storage/bufmgr.h>
#include <utils/rel.h>
#include <utils/palloc.h>

#include "scanner.h"

enum ScannerType
{
	ScannerTypeTable,
	ScannerTypeIndex,
};

/*
 * Scanner can implement both index and heap scans in a single interface.
 */
typedef struct Scanner
{
	Relation (*openscan)(InternalScannerCtx *ctx);
	ScanDesc (*beginscan)(InternalScannerCtx *ctx);
	bool (*getnext)(InternalScannerCtx *ctx);
	void (*endscan)(InternalScannerCtx *ctx);
	void (*closescan)(InternalScannerCtx *ctx);
} Scanner;

/* Functions implementing heap scans */
static Relation
table_scanner_open(InternalScannerCtx *ctx)
{
	ctx->tablerel = table_open(ctx->sctx->table, ctx->sctx->lockmode);
	return ctx->tablerel;
}

static ScanDesc
table_scanner_beginscan(InternalScannerCtx *ctx)
{
	ScannerCtx *sctx = ctx->sctx;

	ctx->scan.table_scan = table_beginscan(ctx->tablerel, SnapshotSelf, sctx->nkeys, sctx->scankey);
	return ctx->scan;
}

static bool
table_scanner_getnext(InternalScannerCtx *ctx)
{
	bool success;
#if PG12_LT
	HeapTuple tuple = heap_getnext(ctx->scan.table_scan, ForwardScanDirection);

	success = HeapTupleIsValid(tuple);

	if (success)
	{
		ctx->tinfo.tid = tuple->t_self;
		/* The tuple is managed by the heap so shouldn't free it */
		ExecStoreTuple(tuple, ctx->tinfo.slot, InvalidBuffer, false);
	}
#else
	success = table_scan_getnextslot(ctx->scan.table_scan, ForwardScanDirection, ctx->tinfo.slot);
#endif

	return success;
}

static void
table_scanner_endscan(InternalScannerCtx *ctx)
{
	table_endscan(ctx->scan.table_scan);
}

static void
table_scanner_close(InternalScannerCtx *ctx)
{
	table_close(ctx->tablerel, ctx->sctx->lockmode);
}

/* Functions implementing index scans */
static Relation
index_scanner_open(InternalScannerCtx *ctx)
{
	ctx->tablerel = table_open(ctx->sctx->table, ctx->sctx->lockmode);
	ctx->indexrel = index_open(ctx->sctx->index, ctx->sctx->lockmode);
	return ctx->indexrel;
}

static ScanDesc
index_scanner_beginscan(InternalScannerCtx *ctx)
{
	ScannerCtx *sctx = ctx->sctx;

	ctx->scan.index_scan =
		index_beginscan(ctx->tablerel, ctx->indexrel, SnapshotSelf, sctx->nkeys, sctx->norderbys);
	ctx->scan.index_scan->xs_want_itup = ctx->sctx->want_itup;
	index_rescan(ctx->scan.index_scan, sctx->scankey, sctx->nkeys, NULL, sctx->norderbys);
	return ctx->scan;
}

static bool
index_scanner_getnext(InternalScannerCtx *ctx)
{
	bool success;
#if PG12_LT
	HeapTuple tuple;

	tuple = index_getnext(ctx->scan.index_scan, ctx->sctx->scandirection);
	success = HeapTupleIsValid(tuple);

	if (success)
	{
		ctx->tinfo.tid = tuple->t_self;
		/* index_getnext() returns disk page tuples, so should not be freed */
		ExecStoreTuple(tuple, ctx->tinfo.slot, InvalidBuffer, false);
	}
#else
	success = index_getnext_slot(ctx->scan.index_scan, ctx->sctx->scandirection, ctx->tinfo.slot);
#endif

	ctx->tinfo.ituple = ctx->scan.index_scan->xs_itup;
	ctx->tinfo.ituple_desc = ctx->scan.index_scan->xs_itupdesc;
	return success;
}

static void
index_scanner_endscan(InternalScannerCtx *ctx)
{
	index_endscan(ctx->scan.index_scan);
}

static void
index_scanner_close(InternalScannerCtx *ctx)
{
	table_close(ctx->tablerel, ctx->sctx->lockmode);
	index_close(ctx->indexrel, ctx->sctx->lockmode);
}

/*
 * Two scanners by type: heap and index scanners.
 */
static Scanner scanners[] = {
	[ScannerTypeTable] = {
		.openscan = table_scanner_open,
		.beginscan = table_scanner_beginscan,
		.getnext = table_scanner_getnext,
		.endscan = table_scanner_endscan,
		.closescan = table_scanner_close,
	},
	[ScannerTypeIndex] = {
		.openscan = index_scanner_open,
		.beginscan = index_scanner_beginscan,
		.getnext = index_scanner_getnext,
		.endscan = index_scanner_endscan,
		.closescan = index_scanner_close,
	}
};

static inline Scanner *
scanner_ctx_get_scanner(ScannerCtx *ctx)
{
	if (OidIsValid(ctx->index))
		return &scanners[ScannerTypeIndex];
	else
		return &scanners[ScannerTypeTable];
}

/*
 * Perform either a heap or index scan depending on the information in the
 * ScannerCtx. ScannerCtx must be setup by caller with the proper information
 * for the scan, including filters and callbacks for found tuples.
 *
 * Return the number of tuples that where found.
 */
TSDLLEXPORT void
ts_scanner_start_scan(ScannerCtx *ctx, InternalScannerCtx *ictx)
{
	TupleDesc tuple_desc;
	Scanner *scanner;

	ictx->sctx = ctx;
	ictx->closed = false;

	scanner = scanner_ctx_get_scanner(ctx);

	scanner->openscan(ictx);
	scanner->beginscan(ictx);

	tuple_desc = RelationGetDescr(ictx->tablerel);

	ictx->tinfo.scanrel = ictx->tablerel;
	ictx->tinfo.mctx = ctx->result_mctx == NULL ? CurrentMemoryContext : ctx->result_mctx;
	ictx->tinfo.slot =
		MakeSingleTupleTableSlotCompat(tuple_desc, table_slot_callbacks(ictx->tablerel));

	/* Call pre-scan handler, if any. */
	if (ctx->prescan != NULL)
		ctx->prescan(ctx->data);
}

static inline bool
ts_scanner_limit_reached(ScannerCtx *ctx, InternalScannerCtx *ictx)
{
	return ctx->limit > 0 && ictx->tinfo.count >= ctx->limit;
}

TSDLLEXPORT void
ts_scanner_end_scan(ScannerCtx *ctx, InternalScannerCtx *ictx)
{
	Scanner *scanner = scanner_ctx_get_scanner(ictx->sctx);

	if (ictx->closed)
		return;

	/* Call post-scan handler, if any. */
	if (ictx->sctx->postscan != NULL)
		ictx->sctx->postscan(ictx->tinfo.count, ictx->sctx->data);

	scanner->endscan(ictx);
	scanner->closescan(ictx);

	ExecDropSingleTupleTableSlot(ictx->tinfo.slot);
	ictx->closed = true;
}

TSDLLEXPORT TupleInfo *
ts_scanner_next(ScannerCtx *ctx, InternalScannerCtx *ictx)
{
	Scanner *scanner = scanner_ctx_get_scanner(ctx);
	bool is_valid = ts_scanner_limit_reached(ctx, ictx) ? false : scanner->getnext(ictx);

	while (is_valid)
	{
		if (ctx->filter == NULL || ctx->filter(&ictx->tinfo, ctx->data) == SCAN_INCLUDE)
		{
			ictx->tinfo.count++;

			if (ctx->tuplock)
			{
				TM_FailureData tmfd;

#if PG12_GE
				TupleTableSlot *slot = ictx->tinfo.slot;

				PushActiveSnapshot(GetLatestSnapshot());
				ictx->tinfo.lockresult = table_tuple_lock(ictx->tablerel,
														  &(slot->tts_tid),
														  GetLatestSnapshot(),
														  slot,
														  GetCurrentCommandId(false),
														  ctx->tuplock->lockmode,
														  ctx->tuplock->waitpolicy,
														  0 /* don't follow updates */,
														  &tmfd);

				PopActiveSnapshot();
#else
				HeapTuple tuple = ExecFetchSlotTuple(ictx->tinfo.slot);
				Buffer buffer;

				ictx->tinfo.lockresult = heap_lock_tuple(ictx->tablerel,
														 tuple,
														 GetCurrentCommandId(false),
														 ctx->tuplock->lockmode,
														 ctx->tuplock->waitpolicy,
														 false,
														 &buffer,
														 &tmfd);
				/*
				 * A tuple lock pins the underlying buffer, so we need to
				 * unpin it.
				 */
				ReleaseBuffer(buffer);
#endif
			}

			/* stop at a valid tuple */
			return &ictx->tinfo;
		}
		is_valid = ts_scanner_limit_reached(ctx, ictx) ? false : scanner->getnext(ictx);
	}

	ts_scanner_end_scan(ctx, ictx);

	return NULL;
}

/*
 * Perform either a heap or index scan depending on the information in the
 * ScannerCtx. ScannerCtx must be setup by caller with the proper information
 * for the scan, including filters and callbacks for found tuples.
 *
 * Return the number of tuples that were found.
 */
TSDLLEXPORT int
ts_scanner_scan(ScannerCtx *ctx)
{
	InternalScannerCtx ictx = { 0 };
	TupleInfo *tinfo;

	for (ts_scanner_start_scan(ctx, &ictx); (tinfo = ts_scanner_next(ctx, &ictx));)
	{
		/* Call tuple_found handler. Abort the scan if the handler wants us to */
		if (ctx->tuple_found != NULL && ctx->tuple_found(tinfo, ctx->data) == SCAN_DONE)
		{
			ts_scanner_end_scan(ctx, &ictx);
			break;
		}
	}

	return ictx.tinfo.count;
}

TSDLLEXPORT bool
ts_scanner_scan_one(ScannerCtx *ctx, bool fail_if_not_found, const char *item_type)
{
	int num_found = ts_scanner_scan(ctx);

	ctx->limit = 2;

	switch (num_found)
	{
		case 0:
			if (fail_if_not_found)
			{
				elog(ERROR, "%s not found", item_type);
			}
			return false;
		case 1:
			return true;
		default:
			elog(ERROR, "more than one %s found", item_type);
			return false;
	}
}

ItemPointer
ts_scanner_get_tuple_tid(TupleInfo *ti)
{
#if PG12_GE
	return &ti->slot->tts_tid;
#else
	return &ti->tid;
#endif
}

HeapTuple
ts_scanner_fetch_heap_tuple(const TupleInfo *ti, bool materialize, bool *should_free)
{
	return ExecFetchSlotHeapTuple(ti->slot, materialize, should_free);
}

TupleDesc
ts_scanner_get_tupledesc(const TupleInfo *ti)
{
	return ti->slot->tts_tupleDescriptor;
}

void *
ts_scanner_alloc_result(const TupleInfo *ti, Size size)
{
	return MemoryContextAllocZero(ti->mctx, size);
}
