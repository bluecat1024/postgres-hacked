/*-------------------------------------------------------------------------
 *
 * inval.h
 *	  POSTGRES cache invalidation dispatcher definitions.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/inval.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INVAL_H
#define INVAL_H

#include "access/htup.h"
#include "storage/relfilenode.h"
#include "utils/relcache.h"

#define INVAL_ARGV_INDEX_NOOP (int8)0
#define INVAL_ARGV_INDEX_CREATE (int8)1
#define INVAL_ARGV_INDEX_DROP (int8)2
#define INVAL_ARGV_INDEX_REBUILD (int8)3

extern PGDLLIMPORT int debug_discard_caches;

/* Index invalidation message for adaptive prepared. */
typedef struct {
	int8 indexop;
	Oid indexoid;
} AdaptiveIndexMsg;

typedef void (*SyscacheCallbackFunction) (Datum arg, int cacheid, uint32 hashvalue);
typedef void (*RelcacheCallbackFunction) (Datum arg, Oid relid);


extern void AcceptInvalidationMessages(void);

extern void AtEOXact_Inval(bool isCommit);

extern void AtEOSubXact_Inval(bool isCommit);

extern void PostPrepare_Inval(void);

extern void CommandEndInvalidationMessages(void);

extern void CacheInvalidateHeapTuple(Relation relation,
									 HeapTuple tuple,
									 HeapTuple newtuple,
									 int8 indexop,
									 Oid indexoid);

extern void CacheInvalidateCatalog(Oid catalogId);

extern void CacheInvalidateRelcache(Relation relation, int8 indexop, Oid indexoid);

extern void CacheInvalidateRelcacheAll(void);

extern void CacheInvalidateRelcacheByTuple(HeapTuple classTuple, int8 indexop, Oid indexoid);

extern void CacheInvalidateRelcacheByRelid(Oid relid);

extern void CacheInvalidateSmgr(RelFileNodeBackend rnode);

extern void CacheInvalidateRelmap(Oid databaseId);

extern void CacheRegisterSyscacheCallback(int cacheid,
										  SyscacheCallbackFunction func,
										  Datum arg);

extern void CacheRegisterRelcacheCallback(RelcacheCallbackFunction func,
										  Datum arg);

extern void CallSyscacheCallbacks(int cacheid, uint32 hashvalue);

extern void InvalidateSystemCaches(void);
extern void InvalidateSystemCachesExtended(bool debug_discard);

extern void LogLogicalInvalidations(void);
#endif							/* INVAL_H */
