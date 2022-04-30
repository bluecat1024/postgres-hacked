/*-------------------------------------------------------------------------
 *
 * plancache.c
 *	  Plan cache management.
 *
 * The plan cache manager has two principal responsibilities: deciding when
 * to use a generic plan versus a custom (parameter-value-specific) plan,
 * and tracking whether cached plans need to be invalidated because of schema
 * changes in the objects they depend on.
 *
 * The logic for choosing generic or custom plans is in choose_custom_plan,
 * which see for comments.
 *
 * Cache invalidation is driven off sinval events.  Any CachedPlanSource
 * that matches the event is marked invalid, as is its generic CachedPlan
 * if it has one.  When (and if) the next demand for a cached plan occurs,
 * parse analysis and rewrite is repeated to build a new valid query tree,
 * and then planning is performed as normal.  We also force re-analysis and
 * re-planning if the active search_path is different from the previous time
 * or, if RLS is involved, if the user changes or the RLS environment changes.
 *
 * Note that if the sinval was a result of user DDL actions, parse analysis
 * could throw an error, for example if a column referenced by the query is
 * no longer present.  Another possibility is for the query's output tupdesc
 * to change (for instance "SELECT *" might expand differently than before).
 * The creator of a cached plan can specify whether it is allowable for the
 * query to change output tupdesc on replan --- if so, it's up to the
 * caller to notice changes and cope with them.
 *
 * Currently, we track exactly the dependencies of plans on relations,
 * user-defined functions, and domains.  On relcache invalidation events or
 * pg_proc or pg_type syscache invalidation events, we invalidate just those
 * plans that depend on the particular object being modified.  (Note: this
 * scheme assumes that any table modification that requires replanning will
 * generate a relcache inval event.)  We also watch for inval events on
 * certain other system catalogs, such as pg_namespace; but for them, our
 * response is just to invalidate all plans.  We expect updates on those
 * catalogs to be infrequent enough that more-detailed tracking is not worth
 * the effort.
 *
 * In addition to full-fledged query plans, we provide a facility for
 * detecting invalidations of simple scalar expressions.  This is fairly
 * bare-bones; it's the caller's responsibility to build a new expression
 * if the old one gets invalidated.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/plancache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <stdio.h>

#include "access/transam.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/resowner_private.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/plantree_traverse.h"
#include "optimizer/planmain.h"
#include "optimizer/paths.h"
#include "rewrite/rewriteManip.h"
#include "nodes/makefuncs.h"
#include "access/table.h"
#include "access/genam.h"
#include "access/amapi.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_am.h"
#include "utils/lsyscache.h"


/*
 * We must skip "overhead" operations that involve database access when the
 * cached plan's subject statement is a transaction control command.
 */
#define IsTransactionStmtPlan(plansource)  \
	((plansource)->raw_parse_tree && \
	 IsA((plansource)->raw_parse_tree->stmt, TransactionStmt))

/*
 * This is the head of the backend's list of "saved" CachedPlanSources (i.e.,
 * those that are in long-lived storage and are examined for sinval events).
 * We use a dlist instead of separate List cells so that we can guarantee
 * to save a CachedPlanSource without error.
 */
static dlist_head saved_plan_list = DLIST_STATIC_INIT(saved_plan_list);

/*
 * This is the head of the backend's list of CachedExpressions.
 */
static dlist_head cached_expression_list = DLIST_STATIC_INIT(cached_expression_list);

static void ReleaseGenericPlan(CachedPlanSource *plansource);
static List *RevalidateCachedQuery(CachedPlanSource *plansource,
								   QueryEnvironment *queryEnv);
static bool CheckCachedPlan(CachedPlanSource *plansource);
static CachedPlan *BuildCachedPlan(CachedPlanSource *plansource, List *qlist,
								   ParamListInfo boundParams, QueryEnvironment *queryEnv);
static bool choose_custom_plan(CachedPlanSource *plansource,
							   ParamListInfo boundParams);
static double cached_plan_cost(CachedPlan *plan, bool include_planner);
static Query *QueryListGetPrimaryStmt(List *stmts);
static void AcquireExecutorLocks(List *stmt_list, bool acquire);
static void AcquirePlannerLocks(List *stmt_list, bool acquire);
static void ScanQueryForLocks(Query *parsetree, bool acquire);
static bool ScanQueryWalker(Node *node, bool *acquire);
static TupleDesc PlanCacheComputeResultDesc(List *stmt_list);
static void PlanCacheRelCallback(Datum arg, Oid relid);
static void PlanCacheObjectCallback(Datum arg, int cacheid, uint32 hashvalue);
static void PlanCacheSysCallback(Datum arg, int cacheid, uint32 hashvalue);
static SeqScan *make_seqscan(List *qptlist, List *qpqual, Index scanrelid);
static SampleScan *make_samplescan(List *qptlist, List *qpqual, Index scanrelid,
								   TableSampleClause *tsc);
static IndexScan *make_indexscan(List *qptlist, List *qpqual, Index scanrelid,
								 Oid indexid, List *indexqual, List *indexqualorig,
								 List *indexorderby, List *indexorderbyorig,
								 List *indexorderbyops,
								 ScanDirection indexscandir);
static IndexOnlyScan *
make_indexonlyscan(List *qptlist,
				   List *qpqual,
				   Index scanrelid,
				   Oid indexid,
				   List *indexqual,
				   List *recheckqual,
				   List *indexorderby,
				   List *indextlist,
				   ScanDirection indexscandir);

static List *fix_indexquals_local(IndexOptInfo *index,
	List *quals, Oid tablerelid, List *matchColnos);

static void switch_running_time(CachedPlanSource *plansource);
static double get_mean_cost(CachedPlanSource *plansource, bool isMain);
static void set_running_cost(CachedPlanSource *plansource, bool isMain);
static void switch_plan_tree(PlannedStmt* stmt, CachedPlanSource *plansource);

static Node *
fix_indexqual_operand_local(Node *node, IndexOptInfo *index, int indexcol, Index tablerelid)
{
	Var		   *result;
	int			pos;
	ListCell   *indexpr_item;

	/*
	 * Remove any binary-compatible relabeling of the indexkey
	 */
	if (IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;

	Assert(indexcol >= 0 && indexcol < index->ncolumns);

	if (index->indexkeys[indexcol] != 0)
	{
		/* It's a simple index column */
		if (IsA(node, Var) &&
			((Var *) node)->varno == tablerelid &&
			((Var *) node)->varattno == index->indexkeys[indexcol])
		{
			result = (Var *) copyObject(node);
			result->varno = INDEX_VAR;
			result->varattno = indexcol + 1;
			return (Node *) result;
		}
		else {
			elog(ERROR, "index key does not match expected index column");
		}
	}

	/* It's an index expression, so find and cross-check the expression */
	indexpr_item = list_head(index->indexprs);
	for (pos = 0; pos < index->ncolumns; pos++)
	{
		if (index->indexkeys[pos] == 0)
		{
			if (indexpr_item == NULL)
				elog(ERROR, "too few entries in indexprs list");
			if (pos == indexcol)
			{
				Node	   *indexkey;

				indexkey = (Node *) lfirst(indexpr_item);
				if (indexkey && IsA(indexkey, RelabelType))
					indexkey = (Node *) ((RelabelType *) indexkey)->arg;
				if (equal(node, indexkey))
				{
					result = makeVar(INDEX_VAR, indexcol + 1,
									 exprType(lfirst(indexpr_item)), -1,
									 exprCollation(lfirst(indexpr_item)),
									 0);

					return (Node *) result;
				}
				else {
					elog(ERROR, "index key does not match expected index column");
				}
			}
			indexpr_item = lnext(index->indexprs, indexpr_item);
		}
	}

	/* Oops... */
	elog(ERROR, "index key does not match expected index column");
	return NULL;				/* keep compiler quiet */
}


static Node *
fix_indexqual_clause_local(IndexOptInfo *index, int indexcol,
					 Node *clause, List *indexcolnos, Index tablerelid)
{
	/*
	 * Replace any outer-relation variables with nestloop params.
	 *
	 * This also makes a copy of the clause, so it's safe to modify it
	 * in-place below.
	 */

	// printf("createplan.c: fix_indexqual_clause_local, table id %d\n", (int)tablerelid);

	if (IsA(clause, OpExpr))
	{
		clause = (Node *)(copyObject((OpExpr *) clause));
		OpExpr	   *op = (OpExpr *) clause;

		/* Replace the indexkey expression with an index Var. */
		linitial(op->args) = fix_indexqual_operand_local(linitial(op->args),
												   index,
												   indexcol,
												   tablerelid);
	}
	else if (IsA(clause, RowCompareExpr))
	{
		clause = (Node *)(copyObject((RowCompareExpr *) clause));
		RowCompareExpr *rc = (RowCompareExpr *) clause;
		ListCell   *lca,
				   *lcai;

		/* Replace the indexkey expressions with index Vars. */
		Assert(list_length(rc->largs) == list_length(indexcolnos));
		forboth(lca, rc->largs, lcai, indexcolnos)
		{
			lfirst(lca) = fix_indexqual_operand_local(lfirst(lca),
												index,
												lfirst_int(lcai),
												tablerelid);
		}
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		clause = (Node *)(copyObject((ScalarArrayOpExpr *) clause));
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

		/* Replace the indexkey expression with an index Var. */
		linitial(saop->args) = fix_indexqual_operand_local(linitial(saop->args),
													 index,
													 indexcol,
													 tablerelid);
	}
	else if (IsA(clause, NullTest))
	{
		clause = (Node *)(copyObject((NullTest *) clause));
		NullTest   *nt = (NullTest *) clause;

		/* Replace the indexkey expression with an index Var. */
		nt->arg = (Expr *) fix_indexqual_operand_local((Node *) nt->arg,
												 index,
												 indexcol,
												 tablerelid);
	}
	else {
		elog(ERROR, "unsupported indexqual type: %d",
			 (int) nodeTag(clause));
		// printf("createplan.c: fix_indexqual_clause_local, error\n");
	}

			 
	// printf("createplan.c: fix_indexqual_clause_local, finish\n");
	return clause;
}

static List *fix_indexquals_local(IndexOptInfo *index,
	List *quals, Index tablerelid, List *matchColnos) {
	List *fix_quals = NIL;
	int indexcol;
	List *indexcolnos;
	ListCell *lc, *lcCol;

	forboth(lc, quals, lcCol, matchColnos) {
		Node *clause = lfirst_node(Node, lc);
		indexcol = lfirst_int(lcCol);
		indexcolnos = list_make1(indexcol);
		fix_quals = lappend(fix_quals, fix_indexqual_clause_local(index,
			indexcol, clause, indexcolnos, tablerelid));
	}

	return fix_quals;
}

#define IndexCollMatchesExprColl(idxcollation, exprcollation) \
	((idxcollation) == InvalidOid || (idxcollation) == (exprcollation))

static int clause_matches_index(Node *clause, IndexOptInfo *info) {
	int idx;
	Oid	opfamily;
	Oid	expr_op;
	Oid	expr_coll;
	Oid idxcollation;

	if (IsA(clause, OpExpr)) {
		OpExpr *op = (OpExpr *)clause;
		// printf("plancache.c: clause_matches_index, op len %d\n", list_length(op->args));
		if (list_length(op->args) == 2) {
			Node *leftop = (Node *) linitial(op->args);
			Node *rightop = (Node *) lsecond(op->args);
			Oid	comm_op;
			expr_coll = op->inputcollid;
			expr_op = op->opno;
			for (idx = 0; idx < info->nkeycolumns; idx++) {
				opfamily = info->opfamily[idx];
				idxcollation = info->indexcollations[idx];
				// printf("plancache.c: clause_matches_index, %d %d %d %d\n", idxcollation, expr_coll, expr_op, opfamily);
				if (match_index_to_operand(leftop, idx, info) && !contain_volatile_functions(rightop)
					&& IndexCollMatchesExprColl(idxcollation, expr_coll) && op_in_opfamily(expr_op, opfamily)) {
					return idx;
				}
				comm_op = get_commutator(expr_op);
				if (match_index_to_operand(rightop, idx, info) && !contain_volatile_functions(leftop)
					&& IndexCollMatchesExprColl(idxcollation, expr_coll) && OidIsValid(comm_op)
					&& op_in_opfamily(comm_op, opfamily)) {
					return idx;
				}
			}
		}
	} else if (IsA(clause, RowCompareExpr)) {
		// printf("plancache.c: clause_matches_index, row compare\n");
		if (info->relam == BTREE_AM_OID) {
			RowCompareExpr *rc = (RowCompareExpr *)clause;
			Node *leftop = (Node *) linitial(rc->largs);
			Node *rightop = (Node *) linitial(rc->rargs);
			expr_op = linitial_oid(rc->opnos);
			expr_coll = linitial_oid(rc->inputcollids);
			for (idx = 0; idx < info->nkeycolumns; idx++) {
				bool exprMatch = false;
				opfamily = info->opfamily[idx];
				idxcollation = info->indexcollations[idx];
				// printf("plancache.c: clause_matches_index, %d %d %d %d\n", idxcollation, expr_coll, expr_op, opfamily);

				if (match_index_to_operand(leftop, idx, info) && !contain_volatile_functions(rightop)
					&& IndexCollMatchesExprColl(idxcollation, expr_coll)) {
					exprMatch = true;
				} else {
					expr_op = get_commutator(expr_op);
					if (match_index_to_operand(rightop, idx, info) && !contain_volatile_functions(leftop)
						&& IndexCollMatchesExprColl(idxcollation, expr_coll)) {
						exprMatch = true;
					}
				}

				if (exprMatch && OidIsValid(expr_op)) {
					switch (get_op_opfamily_strategy(expr_op, opfamily))
					{
						case BTLessStrategyNumber:
						case BTLessEqualStrategyNumber:
						case BTGreaterEqualStrategyNumber:
						case BTGreaterStrategyNumber:
							return idx;
							break;
					}
				}
			}
		}

	} else if (IsA(clause, ScalarArrayOpExpr)) {
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

		if (saop->useOr) {
			Node *leftop = (Node *) linitial(saop->args);
			Node *rightop = (Node *) lsecond(saop->args);
			expr_op = saop->opno;
			expr_coll = saop->inputcollid;

			for (idx = 0; idx < info->nkeycolumns; idx++) {
				opfamily = info->opfamily[idx];
				idxcollation = info->indexcollations[idx];
				if (match_index_to_operand(leftop, idx, info) && !contain_volatile_functions(rightop)
					&& IndexCollMatchesExprColl(idxcollation, expr_coll) && op_in_opfamily(expr_op, opfamily)) {
					return idx;
				}
			}
		}

	} else if (info->amsearchnulls && IsA(clause, NullTest)) {
		NullTest   *nt = (NullTest *) clause;
		for (idx = 0; idx < info->nkeycolumns; idx++) {
			if (!nt->argisrow &&
				match_index_to_operand((Node *) nt->arg, idx, info)) {
				return idx;
			}
		}
	}

	return -1;
}

static Plan *create_index_trigger(Plan *node, List *params, PlannedStmt* stmt) {
	if (nodeTag(node) != T_SeqScan) {
		return node;
	}
	// printf("Create index trigger called\n");
	Scan *scannode = (Scan *)node;
	IndexOptInfo *index = lfirst_node(IndexOptInfo, list_nth_cell(params, 0));
	Oid reloid = list_nth_cell(params, 1)->oid_value;
	List *relOidList = lfirst_node(List, list_nth_cell(params, 2));

	if (reloid != list_nth_cell(relOidList, scannode->scanrelid - 1)->oid_value) {
		// printf("plancache.c: relation not match, %d, %d\n", reloid, list_nth_cell(relOidList, scannode->scanrelid)->oid_value);
		return node;
	}

	index->rel->relid = scannode->scanrelid;
	if (nodeTag(node) == T_SeqScan) {
		SeqScan *seqPlanNode = (SeqScan *)(node);
		IndexScan *indexPlanNode;
		Oid indexoid = index->indexoid;
		// printf("Table Oid: %d Index Oid: %d\n", (int)reloid, (int)indexoid);

		// printf("plancache.c: Replacing SeqScan\n");
		// replace the plan info.

		// separate index qual and other qual.
		List *indexquals = NIL;
		List *seqquals = NIL;
		List *indexcolnos = NIL;
		ListCell *quallc;
		foreach(quallc, seqPlanNode->plan.qual) {
			Node *clause = lfirst_node(Node, quallc);
			int matchColno = clause_matches_index(clause, index);
			if (matchColno >= 0) {
				int innerIdx = 0;
				while (innerIdx < list_length(indexcolnos)) {
					if (list_nth_int(indexcolnos, innerIdx) > matchColno) {
						break;
					}
					innerIdx++;
				}
				indexquals = list_insert_nth(indexquals, innerIdx, clause);
				indexcolnos = list_insert_nth_int(indexcolnos, innerIdx, matchColno);
				// printf("plancache.c: Add index qual\n");
			} else {
				seqquals = lappend(seqquals, clause);
				// printf("plancache.c: Add seq qual\n");
			}
		}

		if (indexquals == NIL) {
			// printf("plancache.c: Not a match for index columns\n");
			return node;
		}

		indexPlanNode = (Plan *)make_indexscan(
			seqPlanNode->plan.targetlist,
			seqquals,
			seqPlanNode->scanrelid,
			indexoid,
			fix_indexquals_local(index,
				indexquals,
				seqPlanNode->scanrelid,
				indexcolnos),
			indexquals,
			NIL, NIL, NIL, 0
		);
		// printf("plancache.c: Replaced to IndexScan\n");

		// add seqscan to backup node.
		indexPlanNode->scan.plan.backupNode = seqPlanNode;

		stmt->has_index = true;

		return indexPlanNode;
	}
}

static Plan *create_index_sort_replacement(Plan *node, List *params, PlannedStmt *stmt) {
	if (node->type != T_Sort) {
		return node;
	}
	Sort* sortNode = (Sort*) node;
	Scan* scanNode = (Scan*) node->lefttree;

	if (nodeTag(scanNode) != T_SeqScan) {
		return node;
	}

	SeqScan *seqPlanNode = (SeqScan *)(scanNode);
	IndexOptInfo *info = lfirst_node(IndexOptInfo, list_nth_cell(params, 0));
	Oid reloid = list_nth_cell(params, 1)->oid_value;
	List *relOidList = lfirst_node(List, list_nth_cell(params, 2));

	if (info->relam != BTREE_AM_OID) {
		return node;
	}

	if (reloid != list_nth_cell(relOidList, scanNode->scanrelid - 1)->oid_value) {
		return node;
	}

	// printf("plancache.c: Start sort+seq -> index\n");

	// Check if all attributes for sort exist in the index column
	bool allExist = true;
	int i = 0;
	int j = 0;
	for (i = 0; i < sortNode->numCols; i++) {
		bool attrFound = false;
		for (j = 0; j < info->nkeycolumns; j++) {
			if ((int)sortNode->sortColIdx[i] == (int)info->indexkeys[j]) {
				attrFound = true;
				break;
			}
		}
		if (!attrFound) {
			allExist = false;
			break;
		}
	}

	// printf("plancache.c: All attributes in index columns? %d\n", allExist);

	if (allExist) {
		// Check order
		int numOfIncreasingOids = 51;
		int increasingOrderOids[] = {37, 58, 95, 97, 255, 261, 5073, 2799, 412, 418, 534, 535, 609, 645, 622, 631,
		660, 664, 672, 1095, 1552, 1122, 1132, 1322, 1332, 1502, 1587, 1222, 3364, 1203, 1754, 1786, 1806, 1864,
		1957, 2062, 2345, 2358, 2371, 2384, 2534, 2540, 2974, 3224, 3518, 3627, 3674, 2990, 3884, 3242, 2862};

		int i = 0;
		bool increasing = false;
		for (i = 0; i < numOfIncreasingOids; i++) {
			if (sortNode->sortOperators[0] == increasingOrderOids[i]) {
				increasing = true;
				break;
			}
		}
		// printf("plancache.c: Sort order increasing? %d\n", increasing);
		info->rel->relid = scanNode->scanrelid;
		List *indexquals = NIL;
		List *seqquals = NIL;
		List *indexcolnos = NIL;
		ListCell *quallc;
		foreach(quallc, seqPlanNode->plan.qual) {
			Node *clause = lfirst_node(Node, quallc);
			int matchColno = clause_matches_index(clause, info);
			if (matchColno >= 0) {
				int innerIdx = 0;
				while (innerIdx < list_length(indexcolnos)) {
					if (list_nth_int(indexcolnos, innerIdx) > matchColno) {
						break;
					}
					innerIdx++;
				}
				indexquals = list_insert_nth(indexquals, innerIdx, clause);
				indexcolnos = list_insert_nth_int(indexcolnos, innerIdx, matchColno);
				// printf("plancache.c: Add index qual\n");
			} else {
				seqquals = lappend(seqquals, clause);
				// printf("plancache.c: Add seq qual\n");
			}
		}

		if (indexquals == NIL) {
			return node;
		}

		Oid indexoid = info->indexoid;

		Plan* newNode = (Plan *)make_indexscan(
			seqPlanNode->plan.targetlist,
			seqquals,
			seqPlanNode->scanrelid,
			indexoid,
			fix_indexquals_local(info,
				indexquals,
				seqPlanNode->scanrelid,
				indexcolnos),
			indexquals,
			NIL, NIL, NIL, increasing ? 0 : -1
		);
		newNode->backupNode = node;
		stmt->has_index = true;
		// printf("plancache.c: sort+seq->index finishes\n");
		return newNode;
	}

	return node;
}

static void recursive_delete(Plan* node) {
	if (node->lefttree != NULL) {
		recursive_delete(node->lefttree);
	}
	if (node->righttree != NULL) {
		recursive_delete(node->righttree);
	}
	pfree(node);
}

static Plan *drop_index_trigger(Plan *node, List *params, PlannedStmt *stmt) {
	Oid indexoid = list_nth_cell(params, 0)->oid_value;
	if (nodeTag(node) != T_IndexScan
		&& (node->backupNode == NULL || nodeTag(node->backupNode) != T_IndexScan)) {
		return node;
	}

	Plan *savedNode = node;
	Plan *anotherNode = node->backupNode;
	if (nodeTag(node) != T_IndexScan) {
		node = node->backupNode;
		anotherNode = savedNode;
	}

	IndexScan *indexPlanNode = (IndexScan *)node;
	if (indexPlanNode->indexid != indexoid) {
		return savedNode;
	}

	stmt->has_index = false;

	if (anotherNode != NULL) {
		// printf("Drop index trigger: not null\n");
		// fflush(stdout);
		Plan* originalNode = anotherNode;
		recursive_delete(node);
		return originalNode;
	} else {
		// printf("This branch should be NEVER CALLED!!!!!\n\n\n");
		List *quals = NIL;
		ListCell *lc;
		foreach(lc, indexPlanNode->indexqualorig) {
			quals = lappend(quals, lc);
		}
		foreach(lc, indexPlanNode->scan.plan.qual) {
			quals = lappend(quals, lc);
		}

		SeqScan *seqPlanNode = make_seqscan(indexPlanNode->scan.plan.targetlist,
			quals, indexPlanNode->scan.scanrelid);

		recursive_delete(node);
		return seqPlanNode;
	}
}

/* GUC parameter */
int			plan_cache_mode;

/*
 * InitPlanCache: initialize module during InitPostgres.
 *
 * All we need to do is hook into inval.c's callback lists.
 */
void
InitPlanCache(void)
{
	CacheRegisterRelcacheCallback(PlanCacheRelCallback, (Datum) palloc(sizeof(AdaptiveIndexMsg)));
	CacheRegisterSyscacheCallback(PROCOID, PlanCacheObjectCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(TYPEOID, PlanCacheObjectCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(NAMESPACEOID, PlanCacheSysCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(OPEROID, PlanCacheSysCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(AMOPOPID, PlanCacheSysCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(FOREIGNSERVEROID, PlanCacheSysCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(FOREIGNDATAWRAPPEROID, PlanCacheSysCallback, (Datum) 0);
}

/*
 * CreateCachedPlan: initially create a plan cache entry.
 *
 * Creation of a cached plan is divided into two steps, CreateCachedPlan and
 * CompleteCachedPlan.  CreateCachedPlan should be called after running the
 * query through raw_parser, but before doing parse analysis and rewrite;
 * CompleteCachedPlan is called after that.  The reason for this arrangement
 * is that it can save one round of copying of the raw parse tree, since
 * the parser will normally scribble on the raw parse tree.  Callers would
 * otherwise need to make an extra copy of the parse tree to ensure they
 * still had a clean copy to present at plan cache creation time.
 *
 * All arguments presented to CreateCachedPlan are copied into a memory
 * context created as a child of the call-time CurrentMemoryContext, which
 * should be a reasonably short-lived working context that will go away in
 * event of an error.  This ensures that the cached plan data structure will
 * likewise disappear if an error occurs before we have fully constructed it.
 * Once constructed, the cached plan can be made longer-lived, if needed,
 * by calling SaveCachedPlan.
 *
 * raw_parse_tree: output of raw_parser(), or NULL if empty query
 * query_string: original query text
 * commandTag: command tag for query, or UNKNOWN if empty query
 */
CachedPlanSource *
CreateCachedPlan(RawStmt *raw_parse_tree,
				 const char *query_string,
				 CommandTag commandTag)
{
	// printf("plancache.c: CreateCachedPlan() begin\n");
	CachedPlanSource *plansource;
	MemoryContext source_context;
	MemoryContext oldcxt;

	Assert(query_string != NULL);	/* required as of 8.4 */

	/*
	 * Make a dedicated memory context for the CachedPlanSource and its
	 * permanent subsidiary data.  It's probably not going to be large, but
	 * just in case, allow it to grow large.  Initially it's a child of the
	 * caller's context (which we assume to be transient), so that it will be
	 * cleaned up on error.
	 */
	source_context = AllocSetContextCreate(CurrentMemoryContext,
										   "CachedPlanSource",
										   ALLOCSET_START_SMALL_SIZES);

	/*
	 * Create and fill the CachedPlanSource struct within the new context.
	 * Most fields are just left empty for the moment.
	 */
	oldcxt = MemoryContextSwitchTo(source_context);

	plansource = (CachedPlanSource *) palloc0(sizeof(CachedPlanSource));
	plansource->magic = CACHEDPLANSOURCE_MAGIC;
	plansource->raw_parse_tree = copyObject(raw_parse_tree);
	plansource->query_string = pstrdup(query_string);
	MemoryContextSetIdentifier(source_context, plansource->query_string);
	plansource->commandTag = commandTag;
	plansource->param_types = NULL;
	plansource->num_params = 0;
	plansource->parserSetup = NULL;
	plansource->parserSetupArg = NULL;
	plansource->cursor_options = 0;
	plansource->fixed_result = false;
	plansource->resultDesc = NULL;
	plansource->context = source_context;
	plansource->query_list = NIL;
	plansource->relationOids = NIL;
	plansource->invalItems = NIL;
	plansource->search_path = NULL;
	plansource->query_context = NULL;
	plansource->rewriteRoleId = InvalidOid;
	plansource->rewriteRowSecurity = false;
	plansource->dependsOnRLS = false;
	plansource->gplan = NULL;
	plansource->is_oneshot = false;
	plansource->is_complete = false;
	plansource->is_saved = false;
	plansource->is_valid = false;
	plansource->generation = 0;
	plansource->generic_cost = -1;
	plansource->total_custom_cost = 0;
	plansource->num_generic_plans = 0;
	plansource->num_custom_plans = 0;

	MemoryContextSwitchTo(oldcxt);
	// printf("plancache.c: CreateCachedPlan() end\n");
	return plansource;
}

/*
 * CreateOneShotCachedPlan: initially create a one-shot plan cache entry.
 *
 * This variant of CreateCachedPlan creates a plan cache entry that is meant
 * to be used only once.  No data copying occurs: all data structures remain
 * in the caller's memory context (which typically should get cleared after
 * completing execution).  The CachedPlanSource struct itself is also created
 * in that context.
 *
 * A one-shot plan cannot be saved or copied, since we make no effort to
 * preserve the raw parse tree unmodified.  There is also no support for
 * invalidation, so plan use must be completed in the current transaction,
 * and DDL that might invalidate the querytree_list must be avoided as well.
 *
 * raw_parse_tree: output of raw_parser(), or NULL if empty query
 * query_string: original query text
 * commandTag: command tag for query, or NULL if empty query
 */
CachedPlanSource *
CreateOneShotCachedPlan(RawStmt *raw_parse_tree,
						const char *query_string,
						CommandTag commandTag)
{
	// printf("plancache.c: CreateOneShotCachedPlan() begin\n");
	CachedPlanSource *plansource;

	Assert(query_string != NULL);	/* required as of 8.4 */

	/*
	 * Create and fill the CachedPlanSource struct within the caller's memory
	 * context.  Most fields are just left empty for the moment.
	 */
	plansource = (CachedPlanSource *) palloc0(sizeof(CachedPlanSource));
	plansource->magic = CACHEDPLANSOURCE_MAGIC;
	plansource->raw_parse_tree = raw_parse_tree;
	plansource->query_string = query_string;
	plansource->commandTag = commandTag;
	plansource->param_types = NULL;
	plansource->num_params = 0;
	plansource->parserSetup = NULL;
	plansource->parserSetupArg = NULL;
	plansource->cursor_options = 0;
	plansource->fixed_result = false;
	plansource->resultDesc = NULL;
	plansource->context = CurrentMemoryContext;
	plansource->query_list = NIL;
	plansource->relationOids = NIL;
	plansource->invalItems = NIL;
	plansource->search_path = NULL;
	plansource->query_context = NULL;
	plansource->rewriteRoleId = InvalidOid;
	plansource->rewriteRowSecurity = false;
	plansource->dependsOnRLS = false;
	plansource->gplan = NULL;
	plansource->is_oneshot = true;
	plansource->is_complete = false;
	plansource->is_saved = false;
	plansource->is_valid = false;
	plansource->generation = 0;
	plansource->generic_cost = -1;
	plansource->total_custom_cost = 0;
	plansource->num_generic_plans = 0;
	plansource->num_custom_plans = 0;

	return plansource;
}

/*
 * CompleteCachedPlan: second step of creating a plan cache entry.
 *
 * Pass in the analyzed-and-rewritten form of the query, as well as the
 * required subsidiary data about parameters and such.  All passed values will
 * be copied into the CachedPlanSource's memory, except as specified below.
 * After this is called, GetCachedPlan can be called to obtain a plan, and
 * optionally the CachedPlanSource can be saved using SaveCachedPlan.
 *
 * If querytree_context is not NULL, the querytree_list must be stored in that
 * context (but the other parameters need not be).  The querytree_list is not
 * copied, rather the given context is kept as the initial query_context of
 * the CachedPlanSource.  (It should have been created as a child of the
 * caller's working memory context, but it will now be reparented to belong
 * to the CachedPlanSource.)  The querytree_context is normally the context in
 * which the caller did raw parsing and parse analysis.  This approach saves
 * one tree copying step compared to passing NULL, but leaves lots of extra
 * cruft in the query_context, namely whatever extraneous stuff parse analysis
 * created, as well as whatever went unused from the raw parse tree.  Using
 * this option is a space-for-time tradeoff that is appropriate if the
 * CachedPlanSource is not expected to survive long.
 *
 * plancache.c cannot know how to copy the data referenced by parserSetupArg,
 * and it would often be inappropriate to do so anyway.  When using that
 * option, it is caller's responsibility that the referenced data remains
 * valid for as long as the CachedPlanSource exists.
 *
 * If the CachedPlanSource is a "oneshot" plan, then no querytree copying
 * occurs at all, and querytree_context is ignored; it is caller's
 * responsibility that the passed querytree_list is sufficiently long-lived.
 *
 * plansource: structure returned by CreateCachedPlan
 * querytree_list: analyzed-and-rewritten form of query (list of Query nodes)
 * querytree_context: memory context containing querytree_list,
 *					  or NULL to copy querytree_list into a fresh context
 * param_types: array of fixed parameter type OIDs, or NULL if none
 * num_params: number of fixed parameters
 * parserSetup: alternate method for handling query parameters
 * parserSetupArg: data to pass to parserSetup
 * cursor_options: options bitmask to pass to planner
 * fixed_result: true to disallow future changes in query's result tupdesc
 */
void
CompleteCachedPlan(CachedPlanSource *plansource,
				   List *querytree_list,
				   MemoryContext querytree_context,
				   Oid *param_types,
				   int num_params,
				   ParserSetupHook parserSetup,
				   void *parserSetupArg,
				   int cursor_options,
				   bool fixed_result)
{
	// printf("plancache.c: CompleteCachedPlan() begin\n");
	MemoryContext source_context = plansource->context;
	MemoryContext oldcxt = CurrentMemoryContext;

	/* Assert caller is doing things in a sane order */
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	Assert(!plansource->is_complete);

	/*
	 * If caller supplied a querytree_context, reparent it underneath the
	 * CachedPlanSource's context; otherwise, create a suitable context and
	 * copy the querytree_list into it.  But no data copying should be done
	 * for one-shot plans; for those, assume the passed querytree_list is
	 * sufficiently long-lived.
	 */
	if (plansource->is_oneshot)
	{
		querytree_context = CurrentMemoryContext;
	}
	else if (querytree_context != NULL)
	{
		MemoryContextSetParent(querytree_context, source_context);
		MemoryContextSwitchTo(querytree_context);
	}
	else
	{
		/* Again, it's a good bet the querytree_context can be small */
		querytree_context = AllocSetContextCreate(source_context,
												  "CachedPlanQuery",
												  ALLOCSET_START_SMALL_SIZES);
		MemoryContextSwitchTo(querytree_context);
		querytree_list = copyObject(querytree_list);
	}

	plansource->query_context = querytree_context;
	plansource->query_list = querytree_list;

	if (!plansource->is_oneshot && !IsTransactionStmtPlan(plansource))
	{
		/*
		 * Use the planner machinery to extract dependencies.  Data is saved
		 * in query_context.  (We assume that not a lot of extra cruft is
		 * created by this call.)  We can skip this for one-shot plans, and
		 * transaction control commands have no such dependencies anyway.
		 */
		extract_query_dependencies((Node *) querytree_list,
								   &plansource->relationOids,
								   &plansource->invalItems,
								   &plansource->dependsOnRLS);

		/* Update RLS info as well. */
		plansource->rewriteRoleId = GetUserId();
		plansource->rewriteRowSecurity = row_security;

		/*
		 * Also save the current search_path in the query_context.  (This
		 * should not generate much extra cruft either, since almost certainly
		 * the path is already valid.)	Again, we don't really need this for
		 * one-shot plans; and we *must* skip this for transaction control
		 * commands, because this could result in catalog accesses.
		 */
		plansource->search_path = GetOverrideSearchPath(querytree_context);
	}

	/*
	 * Save the final parameter types (or other parameter specification data)
	 * into the source_context, as well as our other parameters.  Also save
	 * the result tuple descriptor.
	 */
	MemoryContextSwitchTo(source_context);

	if (num_params > 0)
	{
		plansource->param_types = (Oid *) palloc(num_params * sizeof(Oid));
		memcpy(plansource->param_types, param_types, num_params * sizeof(Oid));
	}
	else
		plansource->param_types = NULL;
	plansource->num_params = num_params;
	plansource->parserSetup = parserSetup;
	plansource->parserSetupArg = parserSetupArg;
	plansource->cursor_options = cursor_options;
	plansource->fixed_result = fixed_result;
	plansource->resultDesc = PlanCacheComputeResultDesc(querytree_list);

	MemoryContextSwitchTo(oldcxt);

	plansource->is_complete = true;
	plansource->is_valid = true;
	// printf("plancache.c: CompleteCachedPlan() end\n");
}

/*
 * SaveCachedPlan: save a cached plan permanently
 *
 * This function moves the cached plan underneath CacheMemoryContext (making
 * it live for the life of the backend, unless explicitly dropped), and adds
 * it to the list of cached plans that are checked for invalidation when an
 * sinval event occurs.
 *
 * This is guaranteed not to throw error, except for the caller-error case
 * of trying to save a one-shot plan.  Callers typically depend on that
 * since this is called just before or just after adding a pointer to the
 * CachedPlanSource to some permanent data structure of their own.  Up until
 * this is done, a CachedPlanSource is just transient data that will go away
 * automatically on transaction abort.
 */
void
SaveCachedPlan(CachedPlanSource *plansource)
{
	// printf("plancache.c: SaveCachedPlan() begin\n");
	/* Assert caller is doing things in a sane order */
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	Assert(plansource->is_complete);
	Assert(!plansource->is_saved);

	/* This seems worth a real test, though */
	if (plansource->is_oneshot)
		elog(ERROR, "cannot save one-shot cached plan");

	/*
	 * In typical use, this function would be called before generating any
	 * plans from the CachedPlanSource.  If there is a generic plan, moving it
	 * into CacheMemoryContext would be pretty risky since it's unclear
	 * whether the caller has taken suitable care with making references
	 * long-lived.  Best thing to do seems to be to discard the plan.
	 */
	ReleaseGenericPlan(plansource);

	/*
	 * Reparent the source memory context under CacheMemoryContext so that it
	 * will live indefinitely.  The query_context follows along since it's
	 * already a child of the other one.
	 */
	MemoryContextSetParent(plansource->context, CacheMemoryContext);

	/*
	 * Add the entry to the global list of cached plans.
	 */
	dlist_push_tail(&saved_plan_list, &plansource->node);

	plansource->is_saved = true;
	// printf("plancache.c: SaveCachedPlan() end\n");
}

/*
 * DropCachedPlan: destroy a cached plan.
 *
 * Actually this only destroys the CachedPlanSource: any referenced CachedPlan
 * is released, but not destroyed until its refcount goes to zero.  That
 * handles the situation where DropCachedPlan is called while the plan is
 * still in use.
 */
void
DropCachedPlan(CachedPlanSource *plansource)
{
	// printf("plancache.c: DropCachedPlan() begin\n");
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

	/* If it's been saved, remove it from the list */
	if (plansource->is_saved)
	{
		dlist_delete(&plansource->node);
		plansource->is_saved = false;
	}

	/* Decrement generic CachedPlan's refcount and drop if no longer needed */
	ReleaseGenericPlan(plansource);

	/* Mark it no longer valid */
	plansource->magic = 0;

	/*
	 * Remove the CachedPlanSource and all subsidiary data (including the
	 * query_context if any).  But if it's a one-shot we can't free anything.
	 */
	if (!plansource->is_oneshot)
		MemoryContextDelete(plansource->context);
}

/*
 * ReleaseGenericPlan: release a CachedPlanSource's generic plan, if any.
 */
static void
ReleaseGenericPlan(CachedPlanSource *plansource)
{
	/* Be paranoid about the possibility that ReleaseCachedPlan fails */
	if (plansource->gplan)
	{
		CachedPlan *plan = plansource->gplan;

		Assert(plan->magic == CACHEDPLAN_MAGIC);
		plansource->gplan = NULL;
		ReleaseCachedPlan(plan, NULL);
	}
}

/*
 * RevalidateCachedQuery: ensure validity of analyzed-and-rewritten query tree.
 *
 * What we do here is re-acquire locks and redo parse analysis if necessary.
 * On return, the query_list is valid and we have sufficient locks to begin
 * planning.
 *
 * If any parse analysis activity is required, the caller's memory context is
 * used for that work.
 *
 * The result value is the transient analyzed-and-rewritten query tree if we
 * had to do re-analysis, and NIL otherwise.  (This is returned just to save
 * a tree copying step in a subsequent BuildCachedPlan call.)
 */
static List *
RevalidateCachedQuery(CachedPlanSource *plansource,
					  QueryEnvironment *queryEnv)
{
	// printf("plancache.c: Revalidate() begin\n");
	bool		snapshot_set;
	RawStmt    *rawtree;
	List	   *tlist;			/* transient query-tree list */
	List	   *qlist;			/* permanent query-tree list */
	TupleDesc	resultDesc;
	MemoryContext querytree_context;
	MemoryContext oldcxt;

	/*
	 * For one-shot plans, we do not support revalidation checking; it's
	 * assumed the query is parsed, planned, and executed in one transaction,
	 * so that no lock re-acquisition is necessary.  Also, there is never any
	 * need to revalidate plans for transaction control commands (and we
	 * mustn't risk any catalog accesses when handling those).
	 */
	if (plansource->is_oneshot || IsTransactionStmtPlan(plansource))
	{
		Assert(plansource->is_valid);
		return NIL;
	}

	/*
	 * If the query is currently valid, we should have a saved search_path ---
	 * check to see if that matches the current environment.  If not, we want
	 * to force replan.
	 */
	if (plansource->is_valid)
	{
		Assert(plansource->search_path != NULL);
		if (!OverrideSearchPathMatchesCurrent(plansource->search_path))
		{
			/* Invalidate the querytree and generic plan */
			plansource->is_valid = false;
			if (plansource->gplan)
				plansource->gplan->is_valid = false;
		}
	}

	/*
	 * If the query rewrite phase had a possible RLS dependency, we must redo
	 * it if either the role or the row_security setting has changed.
	 */
	if (plansource->is_valid && plansource->dependsOnRLS &&
		(plansource->rewriteRoleId != GetUserId() ||
		 plansource->rewriteRowSecurity != row_security))
		plansource->is_valid = false;

	/*
	 * If the query is currently valid, acquire locks on the referenced
	 * objects; then check again.  We need to do it this way to cover the race
	 * condition that an invalidation message arrives before we get the locks.
	 */
	if (plansource->is_valid)
	{
		AcquirePlannerLocks(plansource->query_list, true);

		/*
		 * By now, if any invalidation has happened, the inval callback
		 * functions will have marked the query invalid.
		 */
		if (plansource->is_valid)
		{
			/* Successfully revalidated and locked the query. */
			return NIL;
		}

		/* Oops, the race case happened.  Release useless locks. */
		AcquirePlannerLocks(plansource->query_list, false);
	}

	// printf("plancache.c: Revalidate() re-compile!!!\n");
	/*
	 * Discard the no-longer-useful query tree.  (Note: we don't want to do
	 * this any earlier, else we'd not have been able to release locks
	 * correctly in the race condition case.)
	 */
	plansource->is_valid = false;
	plansource->query_list = NIL;
	plansource->relationOids = NIL;
	plansource->invalItems = NIL;
	plansource->search_path = NULL;

	/*
	 * Free the query_context.  We don't really expect MemoryContextDelete to
	 * fail, but just in case, make sure the CachedPlanSource is left in a
	 * reasonably sane state.  (The generic plan won't get unlinked yet, but
	 * that's acceptable.)
	 */
	if (plansource->query_context)
	{
		MemoryContext qcxt = plansource->query_context;

		plansource->query_context = NULL;
		MemoryContextDelete(qcxt);
	}

	/* Drop the generic plan reference if any */
	ReleaseGenericPlan(plansource);

	/*
	 * Now re-do parse analysis and rewrite.  This not incidentally acquires
	 * the locks we need to do planning safely.
	 */
	Assert(plansource->is_complete);

	/*
	 * If a snapshot is already set (the normal case), we can just use that
	 * for parsing/planning.  But if it isn't, install one.  Note: no point in
	 * checking whether parse analysis requires a snapshot; utility commands
	 * don't have invalidatable plans, so we'd not get here for such a
	 * command.
	 */
	snapshot_set = false;
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot_set = true;
	}

	/*
	 * Run parse analysis and rule rewriting.  The parser tends to scribble on
	 * its input, so we must copy the raw parse tree to prevent corruption of
	 * the cache.
	 */
	rawtree = copyObject(plansource->raw_parse_tree);
	if (rawtree == NULL)
		tlist = NIL;
	else if (plansource->parserSetup != NULL)
		tlist = pg_analyze_and_rewrite_params(rawtree,
											  plansource->query_string,
											  plansource->parserSetup,
											  plansource->parserSetupArg,
											  queryEnv);
	else
		tlist = pg_analyze_and_rewrite(rawtree,
									   plansource->query_string,
									   plansource->param_types,
									   plansource->num_params,
									   queryEnv);

	/* Release snapshot if we got one */
	if (snapshot_set)
		PopActiveSnapshot();

	/*
	 * Check or update the result tupdesc.  XXX should we use a weaker
	 * condition than equalTupleDescs() here?
	 *
	 * We assume the parameter types didn't change from the first time, so no
	 * need to update that.
	 */
	resultDesc = PlanCacheComputeResultDesc(tlist);
	if (resultDesc == NULL && plansource->resultDesc == NULL)
	{
		/* OK, doesn't return tuples */
	}
	else if (resultDesc == NULL || plansource->resultDesc == NULL ||
			 !equalTupleDescs(resultDesc, plansource->resultDesc))
	{
		/* can we give a better error message? */
		if (plansource->fixed_result)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cached plan must not change result type")));
		oldcxt = MemoryContextSwitchTo(plansource->context);
		if (resultDesc)
			resultDesc = CreateTupleDescCopy(resultDesc);
		if (plansource->resultDesc)
			FreeTupleDesc(plansource->resultDesc);
		plansource->resultDesc = resultDesc;
		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * Allocate new query_context and copy the completed querytree into it.
	 * It's transient until we complete the copying and dependency extraction.
	 */
	querytree_context = AllocSetContextCreate(CurrentMemoryContext,
											  "CachedPlanQuery",
											  ALLOCSET_START_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(querytree_context);

	qlist = copyObject(tlist);

	/*
	 * Use the planner machinery to extract dependencies.  Data is saved in
	 * query_context.  (We assume that not a lot of extra cruft is created by
	 * this call.)
	 */
	extract_query_dependencies((Node *) qlist,
							   &plansource->relationOids,
							   &plansource->invalItems,
							   &plansource->dependsOnRLS);

	/* Update RLS info as well. */
	plansource->rewriteRoleId = GetUserId();
	plansource->rewriteRowSecurity = row_security;

	/*
	 * Also save the current search_path in the query_context.  (This should
	 * not generate much extra cruft either, since almost certainly the path
	 * is already valid.)
	 */
	plansource->search_path = GetOverrideSearchPath(querytree_context);

	MemoryContextSwitchTo(oldcxt);

	/* Now reparent the finished query_context and save the links */
	MemoryContextSetParent(querytree_context, plansource->context);

	plansource->query_context = querytree_context;
	plansource->query_list = qlist;

	/*
	 * Note: we do not reset generic_cost or total_custom_cost, although we
	 * could choose to do so.  If the DDL or statistics change that prompted
	 * the invalidation meant a significant change in the cost estimates, it
	 * would be better to reset those variables and start fresh; but often it
	 * doesn't, and we're better retaining our hard-won knowledge about the
	 * relative costs.
	 */

	plansource->is_valid = true;

	/* Return transient copy of querytrees for possible use in planning */
	return tlist;
}

/*
 * CheckCachedPlan: see if the CachedPlanSource's generic plan is valid.
 *
 * Caller must have already called RevalidateCachedQuery to verify that the
 * querytree is up to date.
 *
 * On a "true" return, we have acquired the locks needed to run the plan.
 * (We must do this for the "true" result to be race-condition-free.)
 */
static bool
CheckCachedPlan(CachedPlanSource *plansource)
{
	// printf("plancache.c: CheckCachedPlan() begin\n");
	CachedPlan *plan = plansource->gplan;

	/* Assert that caller checked the querytree */
	Assert(plansource->is_valid);

	/* If there's no generic plan, just say "false" */
	if (!plan)
		return false;

	Assert(plan->magic == CACHEDPLAN_MAGIC);
	/* Generic plans are never one-shot */
	Assert(!plan->is_oneshot);

	/*
	 * If plan isn't valid for current role, we can't use it.
	 */
	if (plan->is_valid && plan->dependsOnRole &&
		plan->planRoleId != GetUserId())
		plan->is_valid = false;

	/*
	 * If it appears valid, acquire locks and recheck; this is much the same
	 * logic as in RevalidateCachedQuery, but for a plan.
	 */
	if (plan->is_valid)
	{
		/*
		 * Plan must have positive refcount because it is referenced by
		 * plansource; so no need to fear it disappears under us here.
		 */
		Assert(plan->refcount > 0);

		AcquireExecutorLocks(plan->stmt_list, true);

		/*
		 * If plan was transient, check to see if TransactionXmin has
		 * advanced, and if so invalidate it.
		 */
		if (plan->is_valid &&
			TransactionIdIsValid(plan->saved_xmin) &&
			!TransactionIdEquals(plan->saved_xmin, TransactionXmin))
			plan->is_valid = false;

		/*
		 * By now, if any invalidation has happened, the inval callback
		 * functions will have marked the plan invalid.
		 */
		if (plan->is_valid)
		{
			/* Successfully revalidated and locked the query. */
			return true;
		}

		/* Oops, the race case happened.  Release useless locks. */
		AcquireExecutorLocks(plan->stmt_list, false);
	}

	/*
	 * Plan has been invalidated, so unlink it from the parent and release it.
	 */
	ReleaseGenericPlan(plansource);
	// printf("plancache.c: CheckCachedPlan() not valid!\n");
	return false;
}

static void PrintTree(Plan* root) {
	if (root != NULL) {
		// printf("Type of node: %d\n", nodeTag(root));
		if (root->lefttree) {
			// printf("Left tree:\n");
			PrintTree(root->lefttree);
		}
		if (root->righttree) {
			// printf("Right tree:\n");
			PrintTree(root->righttree);
		}
	}
}

static void SetBackNodeToNull(Plan* root) {
	if (root != NULL) {
		root->backupNode = NULL;
		if (root->lefttree) {
			SetBackNodeToNull(root->lefttree);
		}
		if (root->righttree) {
			SetBackNodeToNull(root->righttree);
		}
	}
}

/*
 * BuildCachedPlan: construct a new CachedPlan from a CachedPlanSource.
 *
 * qlist should be the result value from a previous RevalidateCachedQuery,
 * or it can be set to NIL if we need to re-copy the plansource's query_list.
 *
 * To build a generic, parameter-value-independent plan, pass NULL for
 * boundParams.  To build a custom plan, pass the actual parameter values via
 * boundParams.  For best effect, the PARAM_FLAG_CONST flag should be set on
 * each parameter value; otherwise the planner will treat the value as a
 * hint rather than a hard constant.
 *
 * Planning work is done in the caller's memory context.  The finished plan
 * is in a child memory context, which typically should get reparented
 * (unless this is a one-shot plan, in which case we don't copy the plan).
 */
static CachedPlan *
BuildCachedPlan(CachedPlanSource *plansource, List *qlist,
				ParamListInfo boundParams, QueryEnvironment *queryEnv)
{
	TimestampTz start = GetCurrentTimestamp();
	// printf("plancache.c: BuildCachedPlan\n");
	CachedPlan *plan;
	List	   *plist;
	bool		snapshot_set;
	bool		is_transient;
	MemoryContext plan_context;
	MemoryContext oldcxt = CurrentMemoryContext;
	ListCell   *lc;

	/*
	 * Normally the querytree should be valid already, but if it's not,
	 * rebuild it.
	 *
	 * NOTE: GetCachedPlan should have called RevalidateCachedQuery first, so
	 * we ought to be holding sufficient locks to prevent any invalidation.
	 * However, if we're building a custom plan after having built and
	 * rejected a generic plan, it's possible to reach here with is_valid
	 * false due to an invalidation while making the generic plan.  In theory
	 * the invalidation must be a false positive, perhaps a consequence of an
	 * sinval reset event or the debug_discard_caches code.  But for safety,
	 * let's treat it as real and redo the RevalidateCachedQuery call.
	 */
	if (!plansource->is_valid)
		qlist = RevalidateCachedQuery(plansource, queryEnv);

	/*
	 * If we don't already have a copy of the querytree list that can be
	 * scribbled on by the planner, make one.  For a one-shot plan, we assume
	 * it's okay to scribble on the original query_list.
	 */
	if (qlist == NIL)
	{
		if (!plansource->is_oneshot)
			qlist = copyObject(plansource->query_list);
		else
			qlist = plansource->query_list;
	}

	/*
	 * If a snapshot is already set (the normal case), we can just use that
	 * for planning.  But if it isn't, and we need one, install one.
	 */
	snapshot_set = false;
	if (!ActiveSnapshotSet() &&
		plansource->raw_parse_tree &&
		analyze_requires_snapshot(plansource->raw_parse_tree))
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot_set = true;
	}

	/*
	 * Generate the plan.
	 */
	plist = pg_plan_queries(qlist, plansource->query_string,
							plansource->cursor_options, boundParams);

	ListCell* l;
	foreach(l, plist) {
		PlannedStmt* stmt = lfirst_node(PlannedStmt, l);
		// printf("plancache.c: The type of the node is: %d\n", nodeTag(stmt));
		// printf("plancache.c: Set has_index to false\n");
		stmt->has_index = false;
		stmt->state = 0;
		Plan* planTree = stmt->planTree;
		PrintTree(planTree);
		SetBackNodeToNull(planTree);
	}
	/* Release snapshot if we got one */
	if (snapshot_set)
		PopActiveSnapshot();

	/*
	 * Normally we make a dedicated memory context for the CachedPlan and its
	 * subsidiary data.  (It's probably not going to be large, but just in
	 * case, allow it to grow large.  It's transient for the moment.)  But for
	 * a one-shot plan, we just leave it in the caller's memory context.
	 */
	if (!plansource->is_oneshot)
	{
		plan_context = AllocSetContextCreate(CurrentMemoryContext,
											 "CachedPlan",
											 ALLOCSET_START_SMALL_SIZES);
		MemoryContextCopyAndSetIdentifier(plan_context, plansource->query_string);

		/*
		 * Copy plan into the new context.
		 */
		MemoryContextSwitchTo(plan_context);

		plist = copyObject(plist);
	}
	else
		plan_context = CurrentMemoryContext;

	/*
	 * Create and fill the CachedPlan struct within the new context.
	 */
	plan = (CachedPlan *) palloc(sizeof(CachedPlan));
	plan->magic = CACHEDPLAN_MAGIC;
	plan->stmt_list = plist;

	/*
	 * CachedPlan is dependent on role either if RLS affected the rewrite
	 * phase or if a role dependency was injected during planning.  And it's
	 * transient if any plan is marked so.
	 */
	plan->planRoleId = GetUserId();
	plan->dependsOnRole = plansource->dependsOnRLS;
	is_transient = false;
	foreach(lc, plist)
	{
		PlannedStmt *plannedstmt = lfirst_node(PlannedStmt, lc);

		if (plannedstmt->commandType == CMD_UTILITY)
			continue;			/* Ignore utility statements */

		if (plannedstmt->transientPlan)
			is_transient = true;
		if (plannedstmt->dependsOnRole)
			plan->dependsOnRole = true;
	}
	if (is_transient)
	{
		Assert(TransactionIdIsNormal(TransactionXmin));
		plan->saved_xmin = TransactionXmin;
	}
	else
		plan->saved_xmin = InvalidTransactionId;
	plan->refcount = 0;
	plan->context = plan_context;
	plan->is_oneshot = plansource->is_oneshot;
	plan->is_saved = false;
	plan->is_valid = true;

	/* assign generation number to new plan */
	plan->generation = ++(plansource->generation);

	MemoryContextSwitchTo(oldcxt);

	TimestampTz end = GetCurrentTimestamp();
	printf("Build plan time: %d micro seconds\n", end - start);
	return plan;
}

/*
 * choose_custom_plan: choose whether to use custom or generic plan
 *
 * This defines the policy followed by GetCachedPlan.
 */
static bool
choose_custom_plan(CachedPlanSource *plansource, ParamListInfo boundParams)
{
	double		avg_custom_cost;

	return false;

	/* One-shot plans will always be considered custom */
	if (plansource->is_oneshot)
		return true;

	/* Otherwise, never any point in a custom plan if there's no parameters */
	if (boundParams == NULL)
		return false;
	/* ... nor for transaction control statements */
	if (IsTransactionStmtPlan(plansource))
		return false;

	/* Let settings force the decision */
	if (plan_cache_mode == PLAN_CACHE_MODE_FORCE_GENERIC_PLAN)
		return false;
	if (plan_cache_mode == PLAN_CACHE_MODE_FORCE_CUSTOM_PLAN)
		return true;

	/* See if caller wants to force the decision */
	if (plansource->cursor_options & CURSOR_OPT_GENERIC_PLAN)
		return false;
	if (plansource->cursor_options & CURSOR_OPT_CUSTOM_PLAN)
		return true;

	/* Generate custom plans until we have done at least 5 (arbitrary) */
	if (plansource->num_custom_plans < 5)
		return true;

	avg_custom_cost = plansource->total_custom_cost / plansource->num_custom_plans;

	/*
	 * Prefer generic plan if it's less expensive than the average custom
	 * plan.  (Because we include a charge for cost of planning in the
	 * custom-plan costs, this means the generic plan only has to be less
	 * expensive than the execution cost plus replan cost of the custom
	 * plans.)
	 *
	 * Note that if generic_cost is -1 (indicating we've not yet determined
	 * the generic plan cost), we'll always prefer generic at this point.
	 */
	if (plansource->generic_cost < avg_custom_cost)
		return false;

	return true;
}

/*
 * cached_plan_cost: calculate estimated cost of a plan
 *
 * If include_planner is true, also include the estimated cost of constructing
 * the plan.  (We must factor that into the cost of using a custom plan, but
 * we don't count it for a generic plan.)
 */
static double
cached_plan_cost(CachedPlan *plan, bool include_planner)
{
	double		result = 0;
	ListCell   *lc;

	foreach(lc, plan->stmt_list)
	{
		PlannedStmt *plannedstmt = lfirst_node(PlannedStmt, lc);

		if (plannedstmt->commandType == CMD_UTILITY)
			continue;			/* Ignore utility statements */

		result += plannedstmt->planTree->total_cost;

		if (include_planner)
		{
			/*
			 * Currently we use a very crude estimate of planning effort based
			 * on the number of relations in the finished plan's rangetable.
			 * Join planning effort actually scales much worse than linearly
			 * in the number of relations --- but only until the join collapse
			 * limits kick in.  Also, while inheritance child relations surely
			 * add to planning effort, they don't make the join situation
			 * worse.  So the actual shape of the planning cost curve versus
			 * number of relations isn't all that obvious.  It will take
			 * considerable work to arrive at a less crude estimate, and for
			 * now it's not clear that's worth doing.
			 *
			 * The other big difficulty here is that we don't have any very
			 * good model of how planning cost compares to execution costs.
			 * The current multiplier of 1000 * cpu_operator_cost is probably
			 * on the low side, but we'll try this for awhile before making a
			 * more aggressive correction.
			 *
			 * If we ever do write a more complicated estimator, it should
			 * probably live in src/backend/optimizer/ not here.
			 */
			int			nrelations = list_length(plannedstmt->rtable);

			result += 1000.0 * cpu_operator_cost * (nrelations + 1);
		}
	}

	return result;
}

/*
 * GetCachedPlan: get a cached plan from a CachedPlanSource.
 *
 * This function hides the logic that decides whether to use a generic
 * plan or a custom plan for the given parameters: the caller does not know
 * which it will get.
 *
 * On return, the plan is valid and we have sufficient locks to begin
 * execution.
 *
 * On return, the refcount of the plan has been incremented; a later
 * ReleaseCachedPlan() call is expected.  If "owner" is not NULL then
 * the refcount has been reported to that ResourceOwner (note that this
 * is only supported for "saved" CachedPlanSources).
 *
 * Note: if any replanning activity is required, the caller's memory context
 * is used for that work.
 */
CachedPlan *
GetCachedPlan(CachedPlanSource *plansource, ParamListInfo boundParams,
			  ResourceOwner owner, QueryEnvironment *queryEnv)
{
	CachedPlan *plan = NULL;
	List	   *qlist;
	bool		customplan;

	/* Assert caller is doing things in a sane order */
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	Assert(plansource->is_complete);
	/* This seems worth a real test, though */
	if (owner && !plansource->is_saved)
		elog(ERROR, "cannot apply ResourceOwner to non-saved cached plan");

	/* Make sure the querytree list is valid and we have parse-time locks */
	qlist = RevalidateCachedQuery(plansource, queryEnv);

	/* Decide whether to use a custom plan */
	customplan = choose_custom_plan(plansource, boundParams);

	if (!customplan)
	{
		if (CheckCachedPlan(plansource))
		{
			/* We want a generic plan, and we already have a valid one */

			if (plansource->num_main_execution >= 5) {
				double main_cost = get_mean_cost(plansource, true);
				double backup_cost = get_mean_cost(plansource, false);
				// printf("plancache.c: Main cost: %f, backup cost: %f\n", main_cost, backup_cost);
				if (backup_cost != (double)-1 && main_cost > backup_cost) {
					// printf("Switch!\n");
					ListCell* lc;
					foreach (lc, plansource->gplan->stmt_list) {
						PlannedStmt* stmt = lfirst_node(PlannedStmt, lc);
						switch_plan_tree(stmt, plansource);
					}
				}
			}

			plan = plansource->gplan;
			Assert(plan->magic == CACHEDPLAN_MAGIC);
		}
		else
		{
			/* Build a new generic plan */
			plan = BuildCachedPlan(plansource, qlist, NULL, queryEnv);
			/* Just make real sure plansource->gplan is clear */
			ReleaseGenericPlan(plansource);
			/* Link the new generic plan into the plansource */
			plansource->gplan = plan;
			plan->refcount++;
			/* Immediately reparent into appropriate context */
			if (plansource->is_saved)
			{
				/* saved plans all live under CacheMemoryContext */
				MemoryContextSetParent(plan->context, CacheMemoryContext);
				plan->is_saved = true;
			}
			else
			{
				/* otherwise, it should be a sibling of the plansource */
				MemoryContextSetParent(plan->context,
									   MemoryContextGetParent(plansource->context));
			}
			/* Update generic_cost whenever we make a new generic plan */
			plansource->generic_cost = cached_plan_cost(plan, false);

			/*
			 * If, based on the now-known value of generic_cost, we'd not have
			 * chosen to use a generic plan, then forget it and make a custom
			 * plan.  This is a bit of a wart but is necessary to avoid a
			 * glitch in behavior when the custom plans are consistently big
			 * winners; at some point we'll experiment with a generic plan and
			 * find it's a loser, but we don't want to actually execute that
			 * plan.
			 */
			customplan = choose_custom_plan(plansource, boundParams);

			/*
			 * If we choose to plan again, we need to re-copy the query_list,
			 * since the planner probably scribbled on it.  We can force
			 * BuildCachedPlan to do that by passing NIL.
			 */
			qlist = NIL;
		}
	}

	if (customplan)
	{
		/* Build a custom plan */
		plan = BuildCachedPlan(plansource, qlist, boundParams, queryEnv);
		/* Accumulate total costs of custom plans */
		plansource->total_custom_cost += cached_plan_cost(plan, true);

		plansource->num_custom_plans++;
	}
	else
	{
		plansource->num_generic_plans++;
	}

	Assert(plan != NULL);

	/* Flag the plan as in use by caller */
	if (owner)
		ResourceOwnerEnlargePlanCacheRefs(owner);
	plan->refcount++;
	if (owner)
		ResourceOwnerRememberPlanCacheRef(owner, plan);

	/*
	 * Saved plans should be under CacheMemoryContext so they will not go away
	 * until their reference count goes to zero.  In the generic-plan cases we
	 * already took care of that, but for a custom plan, do it as soon as we
	 * have created a reference-counted link.
	 */
	if (customplan && plansource->is_saved)
	{
		MemoryContextSetParent(plan->context, CacheMemoryContext);
		plan->is_saved = true;
	}

	return plan;
}

/*
 * ReleaseCachedPlan: release active use of a cached plan.
 *
 * This decrements the reference count, and frees the plan if the count
 * has thereby gone to zero.  If "owner" is not NULL, it is assumed that
 * the reference count is managed by that ResourceOwner.
 *
 * Note: owner == NULL is used for releasing references that are in
 * persistent data structures, such as the parent CachedPlanSource or a
 * Portal.  Transient references should be protected by a resource owner.
 */
void
ReleaseCachedPlan(CachedPlan *plan, ResourceOwner owner)
{
	Assert(plan->magic == CACHEDPLAN_MAGIC);
	if (owner)
	{
		Assert(plan->is_saved);
		ResourceOwnerForgetPlanCacheRef(owner, plan);
	}
	Assert(plan->refcount > 0);
	plan->refcount--;
	if (plan->refcount == 0)
	{
		/* Mark it no longer valid */
		plan->magic = 0;

		/* One-shot plans do not own their context, so we can't free them */
		if (!plan->is_oneshot)
			MemoryContextDelete(plan->context);
	}
}

/*
 * CachedPlanAllowsSimpleValidityCheck: can we use CachedPlanIsSimplyValid?
 *
 * This function, together with CachedPlanIsSimplyValid, provides a fast path
 * for revalidating "simple" generic plans.  The core requirement to be simple
 * is that the plan must not require taking any locks, which translates to
 * not touching any tables; this happens to match up well with an important
 * use-case in PL/pgSQL.  This function tests whether that's true, along
 * with checking some other corner cases that we'd rather not bother with
 * handling in the fast path.  (Note that it's still possible for such a plan
 * to be invalidated, for example due to a change in a function that was
 * inlined into the plan.)
 *
 * If the plan is simply valid, and "owner" is not NULL, record a refcount on
 * the plan in that resowner before returning.  It is caller's responsibility
 * to be sure that a refcount is held on any plan that's being actively used.
 *
 * This must only be called on known-valid generic plans (eg, ones just
 * returned by GetCachedPlan).  If it returns true, the caller may re-use
 * the cached plan as long as CachedPlanIsSimplyValid returns true; that
 * check is much cheaper than the full revalidation done by GetCachedPlan.
 * Nonetheless, no required checks are omitted.
 */
bool
CachedPlanAllowsSimpleValidityCheck(CachedPlanSource *plansource,
									CachedPlan *plan, ResourceOwner owner)
{
	ListCell   *lc;

	/*
	 * Sanity-check that the caller gave us a validated generic plan.  Notice
	 * that we *don't* assert plansource->is_valid as you might expect; that's
	 * because it's possible that that's already false when GetCachedPlan
	 * returns, e.g. because ResetPlanCache happened partway through.  We
	 * should accept the plan as long as plan->is_valid is true, and expect to
	 * replan after the next CachedPlanIsSimplyValid call.
	 */
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	Assert(plan->magic == CACHEDPLAN_MAGIC);
	Assert(plan->is_valid);
	Assert(plan == plansource->gplan);
	Assert(plansource->search_path != NULL);
	Assert(OverrideSearchPathMatchesCurrent(plansource->search_path));

	/* We don't support oneshot plans here. */
	if (plansource->is_oneshot)
		return false;
	Assert(!plan->is_oneshot);

	/*
	 * If the plan is dependent on RLS considerations, or it's transient,
	 * reject.  These things probably can't ever happen for table-free
	 * queries, but for safety's sake let's check.
	 */
	if (plansource->dependsOnRLS)
		return false;
	if (plan->dependsOnRole)
		return false;
	if (TransactionIdIsValid(plan->saved_xmin))
		return false;

	/*
	 * Reject if AcquirePlannerLocks would have anything to do.  This is
	 * simplistic, but there's no need to inquire any more carefully; indeed,
	 * for current callers it shouldn't even be possible to hit any of these
	 * checks.
	 */
	foreach(lc, plansource->query_list)
	{
		Query	   *query = lfirst_node(Query, lc);

		if (query->commandType == CMD_UTILITY)
			return false;
		if (query->rtable || query->cteList || query->hasSubLinks)
			return false;
	}

	/*
	 * Reject if AcquireExecutorLocks would have anything to do.  This is
	 * probably unnecessary given the previous check, but let's be safe.
	 */
	foreach(lc, plan->stmt_list)
	{
		PlannedStmt *plannedstmt = lfirst_node(PlannedStmt, lc);
		ListCell   *lc2;

		if (plannedstmt->commandType == CMD_UTILITY)
			return false;

		/*
		 * We have to grovel through the rtable because it's likely to contain
		 * an RTE_RESULT relation, rather than being totally empty.
		 */
		foreach(lc2, plannedstmt->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc2);

			if (rte->rtekind == RTE_RELATION)
				return false;
		}
	}

	/*
	 * Okay, it's simple.  Note that what we've primarily established here is
	 * that no locks need be taken before checking the plan's is_valid flag.
	 */

	/* Bump refcount if requested. */
	if (owner)
	{
		ResourceOwnerEnlargePlanCacheRefs(owner);
		plan->refcount++;
		ResourceOwnerRememberPlanCacheRef(owner, plan);
	}

	return true;
}

/*
 * CachedPlanIsSimplyValid: quick check for plan still being valid
 *
 * This function must not be used unless CachedPlanAllowsSimpleValidityCheck
 * previously said it was OK.
 *
 * If the plan is valid, and "owner" is not NULL, record a refcount on
 * the plan in that resowner before returning.  It is caller's responsibility
 * to be sure that a refcount is held on any plan that's being actively used.
 *
 * The code here is unconditionally safe as long as the only use of this
 * CachedPlanSource is in connection with the particular CachedPlan pointer
 * that's passed in.  If the plansource were being used for other purposes,
 * it's possible that its generic plan could be invalidated and regenerated
 * while the current caller wasn't looking, and then there could be a chance
 * collision of address between this caller's now-stale plan pointer and the
 * actual address of the new generic plan.  For current uses, that scenario
 * can't happen; but with a plansource shared across multiple uses, it'd be
 * advisable to also save plan->generation and verify that that still matches.
 */
bool
CachedPlanIsSimplyValid(CachedPlanSource *plansource, CachedPlan *plan,
						ResourceOwner owner)
{
	/*
	 * Careful here: since the caller doesn't necessarily hold a refcount on
	 * the plan to start with, it's possible that "plan" is a dangling
	 * pointer.  Don't dereference it until we've verified that it still
	 * matches the plansource's gplan (which is either valid or NULL).
	 */
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

	/*
	 * Has cache invalidation fired on this plan?  We can check this right
	 * away since there are no locks that we'd need to acquire first.  Note
	 * that here we *do* check plansource->is_valid, so as to force plan
	 * rebuild if that's become false.
	 */
	if (!plansource->is_valid || plan != plansource->gplan || !plan->is_valid)
		return false;

	Assert(plan->magic == CACHEDPLAN_MAGIC);

	/* Is the search_path still the same as when we made it? */
	Assert(plansource->search_path != NULL);
	if (!OverrideSearchPathMatchesCurrent(plansource->search_path))
		return false;

	/* It's still good.  Bump refcount if requested. */
	if (owner)
	{
		ResourceOwnerEnlargePlanCacheRefs(owner);
		plan->refcount++;
		ResourceOwnerRememberPlanCacheRef(owner, plan);
	}

	return true;
}

/*
 * CachedPlanSetParentContext: move a CachedPlanSource to a new memory context
 *
 * This can only be applied to unsaved plans; once saved, a plan always
 * lives underneath CacheMemoryContext.
 */
void
CachedPlanSetParentContext(CachedPlanSource *plansource,
						   MemoryContext newcontext)
{
	/* Assert caller is doing things in a sane order */
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	Assert(plansource->is_complete);

	/* These seem worth real tests, though */
	if (plansource->is_saved)
		elog(ERROR, "cannot move a saved cached plan to another context");
	if (plansource->is_oneshot)
		elog(ERROR, "cannot move a one-shot cached plan to another context");

	/* OK, let the caller keep the plan where he wishes */
	MemoryContextSetParent(plansource->context, newcontext);

	/*
	 * The query_context needs no special handling, since it's a child of
	 * plansource->context.  But if there's a generic plan, it should be
	 * maintained as a sibling of plansource->context.
	 */
	if (plansource->gplan)
	{
		Assert(plansource->gplan->magic == CACHEDPLAN_MAGIC);
		MemoryContextSetParent(plansource->gplan->context, newcontext);
	}
}

/*
 * CopyCachedPlan: make a copy of a CachedPlanSource
 *
 * This is a convenience routine that does the equivalent of
 * CreateCachedPlan + CompleteCachedPlan, using the data stored in the
 * input CachedPlanSource.  The result is therefore "unsaved" (regardless
 * of the state of the source), and we don't copy any generic plan either.
 * The result will be currently valid, or not, the same as the source.
 */
CachedPlanSource *
CopyCachedPlan(CachedPlanSource *plansource)
{
	CachedPlanSource *newsource;
	MemoryContext source_context;
	MemoryContext querytree_context;
	MemoryContext oldcxt;

	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	Assert(plansource->is_complete);

	/*
	 * One-shot plans can't be copied, because we haven't taken care that
	 * parsing/planning didn't scribble on the raw parse tree or querytrees.
	 */
	if (plansource->is_oneshot)
		elog(ERROR, "cannot copy a one-shot cached plan");

	source_context = AllocSetContextCreate(CurrentMemoryContext,
										   "CachedPlanSource",
										   ALLOCSET_START_SMALL_SIZES);

	oldcxt = MemoryContextSwitchTo(source_context);

	newsource = (CachedPlanSource *) palloc0(sizeof(CachedPlanSource));
	newsource->magic = CACHEDPLANSOURCE_MAGIC;
	newsource->raw_parse_tree = copyObject(plansource->raw_parse_tree);
	newsource->query_string = pstrdup(plansource->query_string);
	MemoryContextSetIdentifier(source_context, newsource->query_string);
	newsource->commandTag = plansource->commandTag;
	if (plansource->num_params > 0)
	{
		newsource->param_types = (Oid *)
			palloc(plansource->num_params * sizeof(Oid));
		memcpy(newsource->param_types, plansource->param_types,
			   plansource->num_params * sizeof(Oid));
	}
	else
		newsource->param_types = NULL;
	newsource->num_params = plansource->num_params;
	newsource->parserSetup = plansource->parserSetup;
	newsource->parserSetupArg = plansource->parserSetupArg;
	newsource->cursor_options = plansource->cursor_options;
	newsource->fixed_result = plansource->fixed_result;
	if (plansource->resultDesc)
		newsource->resultDesc = CreateTupleDescCopy(plansource->resultDesc);
	else
		newsource->resultDesc = NULL;
	newsource->context = source_context;

	querytree_context = AllocSetContextCreate(source_context,
											  "CachedPlanQuery",
											  ALLOCSET_START_SMALL_SIZES);
	MemoryContextSwitchTo(querytree_context);
	newsource->query_list = copyObject(plansource->query_list);
	newsource->relationOids = copyObject(plansource->relationOids);
	newsource->invalItems = copyObject(plansource->invalItems);
	if (plansource->search_path)
		newsource->search_path = CopyOverrideSearchPath(plansource->search_path);
	newsource->query_context = querytree_context;
	newsource->rewriteRoleId = plansource->rewriteRoleId;
	newsource->rewriteRowSecurity = plansource->rewriteRowSecurity;
	newsource->dependsOnRLS = plansource->dependsOnRLS;

	newsource->gplan = NULL;

	newsource->is_oneshot = false;
	newsource->is_complete = true;
	newsource->is_saved = false;
	newsource->is_valid = plansource->is_valid;
	newsource->generation = plansource->generation;

	/* We may as well copy any acquired cost knowledge */
	newsource->generic_cost = plansource->generic_cost;
	newsource->total_custom_cost = plansource->total_custom_cost;
	newsource->num_generic_plans = plansource->num_generic_plans;
	newsource->num_custom_plans = plansource->num_custom_plans;

	MemoryContextSwitchTo(oldcxt);

	return newsource;
}

/*
 * CachedPlanIsValid: test whether the rewritten querytree within a
 * CachedPlanSource is currently valid (that is, not marked as being in need
 * of revalidation).
 *
 * This result is only trustworthy (ie, free from race conditions) if
 * the caller has acquired locks on all the relations used in the plan.
 */
bool
CachedPlanIsValid(CachedPlanSource *plansource)
{
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	return plansource->is_valid;
}

/*
 * CachedPlanGetTargetList: return tlist, if any, describing plan's output
 *
 * The result is guaranteed up-to-date.  However, it is local storage
 * within the cached plan, and may disappear next time the plan is updated.
 */
List *
CachedPlanGetTargetList(CachedPlanSource *plansource,
						QueryEnvironment *queryEnv)
{
	Query	   *pstmt;

	/* Assert caller is doing things in a sane order */
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	Assert(plansource->is_complete);

	/*
	 * No work needed if statement doesn't return tuples (we assume this
	 * feature cannot be changed by an invalidation)
	 */
	if (plansource->resultDesc == NULL)
		return NIL;

	/* Make sure the querytree list is valid and we have parse-time locks */
	RevalidateCachedQuery(plansource, queryEnv);

	/* Get the primary statement and find out what it returns */
	pstmt = QueryListGetPrimaryStmt(plansource->query_list);

	return FetchStatementTargetList((Node *) pstmt);
}

/*
 * GetCachedExpression: construct a CachedExpression for an expression.
 *
 * This performs the same transformations on the expression as
 * expression_planner(), ie, convert an expression as emitted by parse
 * analysis to be ready to pass to the executor.
 *
 * The result is stashed in a private, long-lived memory context.
 * (Note that this might leak a good deal of memory in the caller's
 * context before that.)  The passed-in expr tree is not modified.
 */
CachedExpression *
GetCachedExpression(Node *expr)
{
	CachedExpression *cexpr;
	List	   *relationOids;
	List	   *invalItems;
	MemoryContext cexpr_context;
	MemoryContext oldcxt;

	/*
	 * Pass the expression through the planner, and collect dependencies.
	 * Everything built here is leaked in the caller's context; that's
	 * intentional to minimize the size of the permanent data structure.
	 */
	expr = (Node *) expression_planner_with_deps((Expr *) expr,
												 &relationOids,
												 &invalItems);

	/*
	 * Make a private memory context, and copy what we need into that.  To
	 * avoid leaking a long-lived context if we fail while copying data, we
	 * initially make the context under the caller's context.
	 */
	cexpr_context = AllocSetContextCreate(CurrentMemoryContext,
										  "CachedExpression",
										  ALLOCSET_SMALL_SIZES);

	oldcxt = MemoryContextSwitchTo(cexpr_context);

	cexpr = (CachedExpression *) palloc(sizeof(CachedExpression));
	cexpr->magic = CACHEDEXPR_MAGIC;
	cexpr->expr = copyObject(expr);
	cexpr->is_valid = true;
	cexpr->relationOids = copyObject(relationOids);
	cexpr->invalItems = copyObject(invalItems);
	cexpr->context = cexpr_context;

	MemoryContextSwitchTo(oldcxt);

	/*
	 * Reparent the expr's memory context under CacheMemoryContext so that it
	 * will live indefinitely.
	 */
	MemoryContextSetParent(cexpr_context, CacheMemoryContext);

	/*
	 * Add the entry to the global list of cached expressions.
	 */
	dlist_push_tail(&cached_expression_list, &cexpr->node);

	return cexpr;
}

/*
 * FreeCachedExpression
 *		Delete a CachedExpression.
 */
void
FreeCachedExpression(CachedExpression *cexpr)
{
	/* Sanity check */
	Assert(cexpr->magic == CACHEDEXPR_MAGIC);
	/* Unlink from global list */
	dlist_delete(&cexpr->node);
	/* Free all storage associated with CachedExpression */
	MemoryContextDelete(cexpr->context);
}

/*
 * QueryListGetPrimaryStmt
 *		Get the "primary" stmt within a list, ie, the one marked canSetTag.
 *
 * Returns NULL if no such stmt.  If multiple queries within the list are
 * marked canSetTag, returns the first one.  Neither of these cases should
 * occur in present usages of this function.
 */
static Query *
QueryListGetPrimaryStmt(List *stmts)
{
	ListCell   *lc;

	foreach(lc, stmts)
	{
		Query	   *stmt = lfirst_node(Query, lc);

		if (stmt->canSetTag)
			return stmt;
	}
	return NULL;
}

/*
 * AcquireExecutorLocks: acquire locks needed for execution of a cached plan;
 * or release them if acquire is false.
 */
static void
AcquireExecutorLocks(List *stmt_list, bool acquire)
{
	ListCell   *lc1;

	foreach(lc1, stmt_list)
	{
		PlannedStmt *plannedstmt = lfirst_node(PlannedStmt, lc1);
		ListCell   *lc2;

		if (plannedstmt->commandType == CMD_UTILITY)
		{
			/*
			 * Ignore utility statements, except those (such as EXPLAIN) that
			 * contain a parsed-but-not-planned query.  Note: it's okay to use
			 * ScanQueryForLocks, even though the query hasn't been through
			 * rule rewriting, because rewriting doesn't change the query
			 * representation.
			 */
			Query	   *query = UtilityContainsQuery(plannedstmt->utilityStmt);

			if (query)
				ScanQueryForLocks(query, acquire);
			continue;
		}

		foreach(lc2, plannedstmt->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc2);

			if (rte->rtekind != RTE_RELATION)
				continue;

			/*
			 * Acquire the appropriate type of lock on each relation OID. Note
			 * that we don't actually try to open the rel, and hence will not
			 * fail if it's been dropped entirely --- we'll just transiently
			 * acquire a non-conflicting lock.
			 */
			if (acquire)
				LockRelationOid(rte->relid, rte->rellockmode);
			else
				UnlockRelationOid(rte->relid, rte->rellockmode);
		}
	}
}

/*
 * AcquirePlannerLocks: acquire locks needed for planning of a querytree list;
 * or release them if acquire is false.
 *
 * Note that we don't actually try to open the relations, and hence will not
 * fail if one has been dropped entirely --- we'll just transiently acquire
 * a non-conflicting lock.
 */
static void
AcquirePlannerLocks(List *stmt_list, bool acquire)
{
	ListCell   *lc;

	foreach(lc, stmt_list)
	{
		Query	   *query = lfirst_node(Query, lc);

		if (query->commandType == CMD_UTILITY)
		{
			/* Ignore utility statements, unless they contain a Query */
			query = UtilityContainsQuery(query->utilityStmt);
			if (query)
				ScanQueryForLocks(query, acquire);
			continue;
		}

		ScanQueryForLocks(query, acquire);
	}
}

/*
 * ScanQueryForLocks: recursively scan one Query for AcquirePlannerLocks.
 */
static void
ScanQueryForLocks(Query *parsetree, bool acquire)
{
	ListCell   *lc;

	/* Shouldn't get called on utility commands */
	Assert(parsetree->commandType != CMD_UTILITY);

	/*
	 * First, process RTEs of the current query level.
	 */
	foreach(lc, parsetree->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		switch (rte->rtekind)
		{
			case RTE_RELATION:
				/* Acquire or release the appropriate type of lock */
				if (acquire)
					LockRelationOid(rte->relid, rte->rellockmode);
				else
					UnlockRelationOid(rte->relid, rte->rellockmode);
				break;

			case RTE_SUBQUERY:
				/* Recurse into subquery-in-FROM */
				ScanQueryForLocks(rte->subquery, acquire);
				break;

			default:
				/* ignore other types of RTEs */
				break;
		}
	}

	/* Recurse into subquery-in-WITH */
	foreach(lc, parsetree->cteList)
	{
		CommonTableExpr *cte = lfirst_node(CommonTableExpr, lc);

		ScanQueryForLocks(castNode(Query, cte->ctequery), acquire);
	}

	/*
	 * Recurse into sublink subqueries, too.  But we already did the ones in
	 * the rtable and cteList.
	 */
	if (parsetree->hasSubLinks)
	{
		query_tree_walker(parsetree, ScanQueryWalker,
						  (void *) &acquire,
						  QTW_IGNORE_RC_SUBQUERIES);
	}
}

/*
 * Walker to find sublink subqueries for ScanQueryForLocks
 */
static bool
ScanQueryWalker(Node *node, bool *acquire)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink    *sub = (SubLink *) node;

		/* Do what we came for */
		ScanQueryForLocks(castNode(Query, sub->subselect), *acquire);
		/* Fall through to process lefthand args of SubLink */
	}

	/*
	 * Do NOT recurse into Query nodes, because ScanQueryForLocks already
	 * processed subselects of subselects for us.
	 */
	return expression_tree_walker(node, ScanQueryWalker,
								  (void *) acquire);
}

/*
 * PlanCacheComputeResultDesc: given a list of analyzed-and-rewritten Queries,
 * determine the result tupledesc it will produce.  Returns NULL if the
 * execution will not return tuples.
 *
 * Note: the result is created or copied into current memory context.
 */
static TupleDesc
PlanCacheComputeResultDesc(List *stmt_list)
{
	Query	   *query;

	switch (ChoosePortalStrategy(stmt_list))
	{
		case PORTAL_ONE_SELECT:
		case PORTAL_ONE_MOD_WITH:
			query = linitial_node(Query, stmt_list);
			return ExecCleanTypeFromTL(query->targetList);

		case PORTAL_ONE_RETURNING:
			query = QueryListGetPrimaryStmt(stmt_list);
			Assert(query->returningList);
			return ExecCleanTypeFromTL(query->returningList);

		case PORTAL_UTIL_SELECT:
			query = linitial_node(Query, stmt_list);
			Assert(query->utilityStmt);
			return UtilityTupleDescriptor(query->utilityStmt);

		case PORTAL_MULTI_QUERY:
			/* will not return tuples */
			break;
	}
	return NULL;
}

/*
 * PlanCacheRelCallback
 *		Relcache inval callback function
 *
 * Invalidate all plans mentioning the given rel, or all plans mentioning
 * any rel at all if relid == InvalidOid.
 */
static void
PlanCacheRelCallback(Datum arg, Oid relid)
{
	// printf("plancache.c: PlanCacheRelCallback() begin, %d\n", (int)relid);
	AdaptiveIndexMsg *msg = (AdaptiveIndexMsg *)arg;
	// printf("plancache.c: PlanCacheRelCallback() begin, index op %d %d\n", (int)msg->indexop, (int)msg->indexoid);
	dlist_iter	iter;

	dlist_foreach(iter, &saved_plan_list)
	{
		CachedPlanSource *plansource = dlist_container(CachedPlanSource,
													   node, iter.cur);
		ListCell *lc;
		
		Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

		if (plansource->gplan && plansource->gplan->is_valid && list_member_oid(plansource->relationOids, relid)) {
			TimestampTz begin = GetCurrentTimestamp();
			bool changeHappen = false;
			MemoryContext oldCxt = MemoryContextSwitchTo(plansource->context);
			Oid indexoid = msg->indexoid;

			if (msg->indexop == INVAL_ARGV_INDEX_CREATE) {
				Relation myindex = index_open(indexoid, NoLock);

				IndexOptInfo *info = makeNode(IndexOptInfo);
				RelOptInfo *relinfo = makeNode(RelOptInfo);

				info->indexoid = indexoid;
				info->rel = relinfo;
				info->ncolumns = myindex->rd_index->indnatts;
				info->nkeycolumns = myindex->rd_index->indnkeyatts;
				// printf("Index Col Number %d\n", info->ncolumns);
				info->indexkeys = (int *) palloc(sizeof(int) * info->ncolumns);
				info->indexcollations = (Oid *) palloc(sizeof(Oid) * info->nkeycolumns);
				info->opfamily = (Oid *) palloc(sizeof(Oid) * info->nkeycolumns);
				info->relam = myindex->rd_rel->relam;
				info->amsearchnulls = myindex->rd_indam->amsearchnulls;

				int i = 0;
				for (i = 0; i < info->ncolumns; i++)
				{
					info->indexkeys[i] = myindex->rd_index->indkey.values[i];
					// printf("Index Col %d Mapping %d\n", i, info->indexkeys[i]);
				}
				for (i = 0; i < info->nkeycolumns; i++)
				{
					info->opfamily[i] = myindex->rd_opfamily[i];
					info->indexcollations[i] = myindex->rd_indcollation[i];
				}

				// printf("plancache.c: Getting index expressions\n");
				info->indexprs = RelationGetIndexExpressions(myindex);
				if (info->indexprs && relid != 1)
					ChangeVarNodes((Node *) info->indexprs, 1, relid, 0);
				// printf("plancache.c: Got index expressions\n");

				foreach(lc, plansource->gplan->stmt_list) {
					PlannedStmt *plan = lfirst_node(PlannedStmt, lc);
					if (!list_member_oid(plan->relationOids, relid)) {
						continue;
					}
					List *params = NIL;
					params = lappend(params, info);
					params = lappend_oid(params, relid);
					params = lappend(params, plan->relationOids);
					// TODO:
					// If plan has_index: continue
					// Else: do creation

					// TODO: For callback: Set plan.has_index if create a new plan
					if (plan->has_index) {
						// printf("Already has index. Do not add plan\n");
						continue;
					}
						
					plan->planTree = PreOrderPlantreeTraverse(plan->planTree, create_index_sort_replacement, params, plan);
					if (!plan->has_index) {
						plan->planTree = PreOrderPlantreeTraverse(plan->planTree, create_index_trigger, params, plan);
					}
					
					
					// Check whether plan.has_index is set to true
					// If yes, update state to 1. Switch running time and set main to 0. 
					if (plan->has_index) {
						// TODO: update state to 1. Switch running time and set main to 0. 
						plan->state = 1;
						switch_running_time(plansource);
						set_running_cost(plansource, true);
						changeHappen = true;
					}
				}

				index_close(myindex, NoLock);
				pfree(relinfo);
				pfree(info->indexkeys);
				pfree(info->indexcollations);
				pfree(info->opfamily);
				pfree(info);
				MemoryContextSwitchTo(oldCxt);
			} else if (msg->indexop == INVAL_ARGV_INDEX_DROP) {
				foreach(lc, plansource->gplan->stmt_list) {
					PlannedStmt *plan = lfirst_node(PlannedStmt, lc);
					if (!list_member_oid(plan->relationOids, relid)) {
						continue;
					}
					List *params = NIL;
					params = lappend_oid(params, indexoid);

					// If !has_index: continue
					if (!plan->has_index) {
						continue;
					}

					if (plan->state == 0) {
						switch_plan_tree(plan, plansource);
					}

					// postorder delete
					plan->planTree = PostOrderPlantreeTraverse(plan->planTree, drop_index_trigger, params, plan);
					// Set has_index to false
					// If state == 1: switch running time
					// Set backup running time to -1
					if (!plan->has_index) {
						// If state == 1: switch running time
						// TODO: Set backup running time to -1
						plan->state = 0;
						set_running_cost(plansource, false);
						changeHappen = true;
					}
				}
			}
			if (changeHappen) {
				TimestampTz end = GetCurrentTimestamp();
				printf("Substitution overhead: %d micro seconds\n", (end - begin));
			}

		}

		/* Do not need to further invalidate if is adaptive to index. */
		if (msg->indexop != INVAL_ARGV_INDEX_NOOP)
			continue;

		/* No work if it's already invalidated */
		if (!plansource->is_valid)
			continue;

		/* Never invalidate transaction control commands */
		if (IsTransactionStmtPlan(plansource))
			continue;

		/*
		 * Check the dependency list for the rewritten querytree.
		 */
		if ((relid == InvalidOid) ? plansource->relationOids != NIL :
			list_member_oid(plansource->relationOids, relid))
		{
			/* Invalidate the querytree and generic plan */
			// printf("plancache.c: Invalidate something\n");
			plansource->is_valid = false;
			if (plansource->gplan)
				plansource->gplan->is_valid = false;
		}

		/*
		 * The generic plan, if any, could have more dependencies than the
		 * querytree does, so we have to check it too.
		 */
		if (plansource->gplan && plansource->gplan->is_valid)
		{
			ListCell   *lc;

			foreach(lc, plansource->gplan->stmt_list)
			{
				PlannedStmt *plannedstmt = lfirst_node(PlannedStmt, lc);

				if (plannedstmt->commandType == CMD_UTILITY)
					continue;	/* Ignore utility statements */
				if ((relid == InvalidOid) ? plannedstmt->relationOids != NIL :
					list_member_oid(plannedstmt->relationOids, relid))
				{
					/* Invalidate the generic plan only */
					// printf("plancache.c: Invalidate something\n");
					plansource->gplan->is_valid = false;
					break;		/* out of stmt_list scan */
				}
			}
		}
	}

	/* Likewise check cached expressions */
	dlist_foreach(iter, &cached_expression_list)
	{
		CachedExpression *cexpr = dlist_container(CachedExpression,
												  node, iter.cur);

		Assert(cexpr->magic == CACHEDEXPR_MAGIC);

		/* No work if it's already invalidated */
		if (!cexpr->is_valid)
			continue;

		if ((relid == InvalidOid) ? cexpr->relationOids != NIL :
			list_member_oid(cexpr->relationOids, relid))
		{
			// printf("plancache.c: Invalidate something\n");
			cexpr->is_valid = false;
		}
	}
	// printf("plancache.c: PlanCacheRelCallback() end\n");
}

/*
 * PlanCacheObjectCallback
 *		Syscache inval callback function for PROCOID and TYPEOID caches
 *
 * Invalidate all plans mentioning the object with the specified hash value,
 * or all plans mentioning any member of this cache if hashvalue == 0.
 */
static void
PlanCacheObjectCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	// printf("plancache.c: PlanCacheObjectCallback() begin\n");
	dlist_iter	iter;
	return;

	dlist_foreach(iter, &saved_plan_list)
	{
		CachedPlanSource *plansource = dlist_container(CachedPlanSource,
													   node, iter.cur);
		ListCell   *lc;

		Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

		/* No work if it's already invalidated */
		if (!plansource->is_valid)
			continue;

		/* Never invalidate transaction control commands */
		if (IsTransactionStmtPlan(plansource))
			continue;

		/*
		 * Check the dependency list for the rewritten querytree.
		 */
		foreach(lc, plansource->invalItems)
		{
			PlanInvalItem *item = (PlanInvalItem *) lfirst(lc);

			if (item->cacheId != cacheid)
				continue;
			if (hashvalue == 0 ||
				item->hashValue == hashvalue)
			{
				/* Invalidate the querytree and generic plan */
				// printf("plancache.c: Invalidate something\n");
				plansource->is_valid = false;
				if (plansource->gplan)
					plansource->gplan->is_valid = false;
				break;
			}
		}

		/*
		 * The generic plan, if any, could have more dependencies than the
		 * querytree does, so we have to check it too.
		 */
		if (plansource->gplan && plansource->gplan->is_valid)
		{
			foreach(lc, plansource->gplan->stmt_list)
			{
				PlannedStmt *plannedstmt = lfirst_node(PlannedStmt, lc);
				ListCell   *lc3;

				if (plannedstmt->commandType == CMD_UTILITY)
					continue;	/* Ignore utility statements */
				foreach(lc3, plannedstmt->invalItems)
				{
					PlanInvalItem *item = (PlanInvalItem *) lfirst(lc3);

					if (item->cacheId != cacheid)
						continue;
					if (hashvalue == 0 ||
						item->hashValue == hashvalue)
					{
						/* Invalidate the generic plan only */
						// printf("plancache.c: Invalidate something\n");
						plansource->gplan->is_valid = false;
						break;	/* out of invalItems scan */
					}
				}
				if (!plansource->gplan->is_valid)
					break;		/* out of stmt_list scan */
			}
		}
	}

	/* Likewise check cached expressions */
	dlist_foreach(iter, &cached_expression_list)
	{
		CachedExpression *cexpr = dlist_container(CachedExpression,
												  node, iter.cur);
		ListCell   *lc;

		Assert(cexpr->magic == CACHEDEXPR_MAGIC);

		/* No work if it's already invalidated */
		if (!cexpr->is_valid)
			continue;

		foreach(lc, cexpr->invalItems)
		{
			PlanInvalItem *item = (PlanInvalItem *) lfirst(lc);

			if (item->cacheId != cacheid)
				continue;
			if (hashvalue == 0 ||
				item->hashValue == hashvalue)
			{
				// printf("plancache.c: Invalidate something\n");
				cexpr->is_valid = false;
				break;
			}
		}
	}
	// printf("plancache.c: PlanCacheObjectCallback() end\n");
}

/*
 * PlanCacheSysCallback
 *		Syscache inval callback function for other caches
 *
 * Just invalidate everything...
 */
static void
PlanCacheSysCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	// printf("plancache.c: PlanCacheSysCallback() begin\n");
	ResetPlanCache();
	// printf("plancache.c: PlanCacheSysCallback() end\n");
}

/*
 * ResetPlanCache: invalidate all cached plans.
 */
void
ResetPlanCache(void)
{
	// printf("plancache.c: ResetPlanCache() begin\n");
	dlist_iter	iter;

	dlist_foreach(iter, &saved_plan_list)
	{
		CachedPlanSource *plansource = dlist_container(CachedPlanSource,
													   node, iter.cur);
		ListCell   *lc;

		Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

		/* No work if it's already invalidated */
		if (!plansource->is_valid)
			continue;

		/*
		 * We *must not* mark transaction control statements as invalid,
		 * particularly not ROLLBACK, because they may need to be executed in
		 * aborted transactions when we can't revalidate them (cf bug #5269).
		 */
		if (IsTransactionStmtPlan(plansource))
			continue;

		/*
		 * In general there is no point in invalidating utility statements
		 * since they have no plans anyway.  So invalidate it only if it
		 * contains at least one non-utility statement, or contains a utility
		 * statement that contains a pre-analyzed query (which could have
		 * dependencies.)
		 */
		foreach(lc, plansource->query_list)
		{
			Query	   *query = lfirst_node(Query, lc);

			if (query->commandType != CMD_UTILITY ||
				UtilityContainsQuery(query->utilityStmt))
			{
				/* non-utility statement, so invalidate */
				// printf("plancache.c: Invalidate something\n");
				plansource->is_valid = false;
				if (plansource->gplan)
					plansource->gplan->is_valid = false;
				/* no need to look further */
				break;
			}
		}
	}

	/* Likewise invalidate cached expressions */
	dlist_foreach(iter, &cached_expression_list)
	{
		CachedExpression *cexpr = dlist_container(CachedExpression,
												  node, iter.cur);

		Assert(cexpr->magic == CACHEDEXPR_MAGIC);

		cexpr->is_valid = false;
	}
	// printf("plancache.c: ResetPlanCache() end\n");
}


static SeqScan *
make_seqscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid)
{
	SeqScan    *node = makeNode(SeqScan);
	Plan	   *plan = &node->plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scanrelid = scanrelid;

	return node;
}

static SampleScan *
make_samplescan(List *qptlist,
				List *qpqual,
				Index scanrelid,
				TableSampleClause *tsc)
{
	SampleScan *node = makeNode(SampleScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tablesample = tsc;

	return node;
}

static IndexScan *
make_indexscan(List *qptlist,
			   List *qpqual,
			   Index scanrelid,
			   Oid indexid,
			   List *indexqual,
			   List *indexqualorig,
			   List *indexorderby,
			   List *indexorderbyorig,
			   List *indexorderbyops,
			   ScanDirection indexscandir)
{
	IndexScan  *node = makeNode(IndexScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->indexqualorig = indexqualorig;
	node->indexorderby = indexorderby;
	node->indexorderbyorig = indexorderbyorig;
	node->indexorderbyops = indexorderbyops;
	node->indexorderdir = indexscandir;

	return node;
}

static IndexOnlyScan *
make_indexonlyscan(List *qptlist,
				   List *qpqual,
				   Index scanrelid,
				   Oid indexid,
				   List *indexqual,
				   List *recheckqual,
				   List *indexorderby,
				   List *indextlist,
				   ScanDirection indexscandir)
{
	IndexOnlyScan *node = makeNode(IndexOnlyScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->recheckqual = recheckqual;
	node->indexorderby = indexorderby;
	node->indextlist = indextlist;
	node->indexorderdir = indexscandir;

	return node;
}

static Plan* switch_tree_internal(Plan* plan) {
	if (plan == NULL) {
		return NULL;
	}

	if (plan->backupNode != NULL) {
		plan->backupNode->backupNode = plan;
		plan = plan->backupNode;
	}

	plan->lefttree = switch_tree_internal(plan->lefttree);
	plan->righttree = switch_tree_internal(plan->righttree);
	// printf("Node type is: %d\n", nodeTag(plan));
	return plan;
}

static void switch_running_time(CachedPlanSource *plansource) {
	double tmp_cost = plansource->total_main_cost;
	plansource->total_main_cost = plansource->total_backup_cost;
	plansource->total_backup_cost = tmp_cost;

	int64 tmp_num = plansource->num_main_execution;
	plansource->num_backup_execution = plansource->num_main_execution;
	plansource->num_main_execution = tmp_num;
}

static double get_mean_cost(CachedPlanSource *plansource, bool isMain) {
	double result = 0.0;

	if (!isMain && plansource->num_backup_execution == 0) {
		return (double)-1;
	}

	if (isMain) {
		if (plansource->num_main_execution != 0) {
			result = plansource->total_main_cost / (double)plansource->num_main_execution;
		}
	} else {
		result = plansource->total_backup_cost / (double)plansource->num_backup_execution;
	}

	return result;
}

static void set_running_cost(CachedPlanSource *plansource, bool isMain) {
	if (isMain) {
		plansource->total_main_cost = 0.0;
		plansource->num_main_execution = 0;
	} else {
		plansource->total_backup_cost = 0.0;
		plansource->num_backup_execution = 0;
	}
}

static void switch_plan_tree(PlannedStmt* stmt, CachedPlanSource *plansource) {
	// switch():
	//   pre-traversal. Swap (two assignments) current node and backup node if backup node is not NULL
	//   switch running mean
	//   Update state
	TimestampTz begin = GetCurrentTimestamp();
	if (!stmt->has_index) {
		return;
	}
	
	switch_running_time(plansource);

	stmt->planTree = switch_tree_internal(stmt->planTree);

	stmt->state = (stmt->state + 1) % 2;

	TimestampTz end = GetCurrentTimestamp();
	printf("Switching overhead: %d micro second\n", end - begin);
}