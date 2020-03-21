
#include "kv_fdw.h"

#include <pthread.h>

#include "access/reloptions.h"
#include "foreign/fdwapi.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "funcapi.h"
#include "utils/rel.h"
#include "nodes/makefuncs.h"
#include "access/tuptoaster.h"
#include "catalog/pg_operator.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "commands/defrem.h"
#include "foreign/foreign.h"
#include "utils/builtins.h"


PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(kv_fdw_handler);
PG_FUNCTION_INFO_V1(kv_fdw_validator);


static SharedMem *ptr = NULL;  // in client process


#ifdef VIDARDB
static int GetAttrCount(Oid foreignTableId) {
    Relation relation = heap_open(foreignTableId, AccessShareLock);
    TupleDesc tupleDescriptor = RelationGetDescr(relation);
    int natts = tupleDescriptor->natts;
    heap_close(relation, AccessShareLock);
    return natts;
}

static bool IsColumnUsed(Oid relationId) {
    char *option = KVGetOptionValue(relationId, OPTION_STORAGE_FORMAT);
    if (option == NULL) {
        return false;
    }
    return (0 == strncmp(option, COLUMNSTORE, sizeof(COLUMNSTORE)));
}

static int32 GetBatchCapacity(Oid relationId) {
    char *capacity = KVGetOptionValue(relationId, OPTION_BATCH_CAPACITY);
    return capacity? pg_atoi(capacity, sizeof(int32), 0): BATCHCAPACITY;
}
#endif

static void GetForeignRelSize(PlannerInfo *root,
                              RelOptInfo *baserel,
                              Oid foreignTableId) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * Obtain relation size estimates for a foreign table. This is called at
     * the beginning of planning for a query that scans a foreign table. root
     * is the planner's global information about the query; baserel is the
     * planner's information about this table; and foreigntableid is the
     * pg_class OID of the foreign table. (foreigntableid could be obtained
     * from the planner data structures, but it's passed explicitly to save
     * effort.)
     *
     * This function should update baserel->rows to be the expected number of
     * rows returned by the table scan, after accounting for the filtering
     * done by the restriction quals. The initial value of baserel->rows is
     * just a constant default estimate, which should be replaced if at all
     * possible. The function may also choose to update baserel->width if it
     * can compute a better estimate of the average result row width.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    /*
     * min & max will call GetForeignRelSize & GetForeignPaths multiple times,
     * we should open & close db multiple times.
     */
    #ifdef VIDARDB
    bool useColumn = IsColumnUsed(foreignTableId);
    int attrCount = useColumn? GetAttrCount(foreignTableId): 0;
    ptr = OpenRequest(foreignTableId, ptr, useColumn, attrCount);
    #else
    ptr = OpenRequest(foreignTableId, ptr);
    #endif

    /* TODO better estimation */
    baserel->rows = CountRequest(foreignTableId, ptr);

    CloseRequest(foreignTableId, ptr);
}

static void GetForeignPaths(PlannerInfo *root,
                            RelOptInfo *baserel,
                            Oid foreignTableId) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * Create possible access paths for a scan on a foreign table. This is
     * called during query planning. The parameters are the same as for
     * GetForeignRelSize, which has already been called.
     *
     * This function must generate at least one access path (ForeignPath node)
     * for a scan on the foreign table and must call add_path to add each such
     * path to baserel->pathlist. It's recommended to use
     * create_foreignscan_path to build the ForeignPath nodes. The function
     * can generate multiple access paths, e.g., a path which has valid
     * pathkeys to represent a pre-sorted result. Each access path must
     * contain cost estimates, and can contain any FDW-private information
     * that is needed to identify the specific scan method intended.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    Cost startupCost = 0;
    Cost totalCost = startupCost + baserel->rows;

    /* Create a ForeignPath node and add it as only possible path */
    add_path(baserel,
             (Path *) create_foreignscan_path(root,
                                              baserel,
                                              NULL,  /* default pathtarget */
                                              baserel->rows,
                                              startupCost,
                                              totalCost,
                                              NIL,   /* no pathkeys */
                                              NULL,  /* no outer rel either */
                                              NULL,  /* no extra plan */
                                              NIL)); /* no fdw_private data */
}

static ForeignScan *GetForeignPlan(PlannerInfo *root,
                                   RelOptInfo *baserel,
                                   Oid foreignTableId,
                                   ForeignPath *bestPath,
                                   List *targetList,
                                   List *scanClauses,
                                   Plan *outerPlan) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * Create a ForeignScan plan node from the selected foreign access path.
     * This is called at the end of query planning. The parameters are as for
     * GetForeignRelSize, plus the selected ForeignPath (previously produced
     * by GetForeignPaths), the target list to be emitted by the plan node,
     * and the restriction clauses to be enforced by the plan node.
     *
     * This function must create and return a ForeignScan plan node; it's
     * recommended to use make_foreignscan to build the ForeignScan node.
     *
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    /*
     * We have no native ability to evaluate restriction clauses, so we just
     * put all the scan_clauses into the plan node's qual list for the
     * executor to check. So all we have to do here is strip RestrictInfo
     * nodes from the clauses and ignore pseudoconstants (which will be
     * handled elsewhere).
     */

    scanClauses = extract_actual_clauses(scanClauses, false);

    /* To accommodate min & max, we open file here */
    #ifdef VIDARDB
    bool useColumn = IsColumnUsed(foreignTableId);
    int attrCount = useColumn? GetAttrCount(foreignTableId): 0;
    ptr = OpenRequest(foreignTableId, ptr, useColumn, attrCount);
    #else
    ptr = OpenRequest(foreignTableId, ptr);
    #endif

    /* Create the ForeignScan node */
    return make_foreignscan(targetList,
                            scanClauses,
                            baserel->relid,
                            NIL, /* no expressions to evaluate */
                            NIL,
                            NIL, /* no custom tlist */
                            NIL, /* no remote quals */
                            NULL);
}

static void GetKeyBasedQual(Node *node,
                            Relation relation,
                            TableReadState *readState) {
    if (!node || !IsA(node, OpExpr)) {
        return;
    }

    OpExpr *op = (OpExpr *) node;
    if (list_length(op->args) != 2) {
        return;
    }

    Node *left = list_nth(op->args, 0);
    if (!IsA(left, Var)) {
        return;
    }

    Node *right = list_nth(op->args, 1);
    if (!IsA(right, Const)) {
        return;
    }

    Index varattno = ((Var *) left)->varattno;
    if (varattno != 1) {
        return;
    }

    /* get the name of the operator according to PG_OPERATOR OID */
    HeapTuple opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(op->opno));
    if (!HeapTupleIsValid(opertup)) {
        ereport(ERROR, (errmsg("cache lookup failed for operator %u",
                               op->opno)));
    }
    Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);
    char *oprname = NameStr(operform->oprname);
    /* TODO support more operators */
    if (strncmp(oprname, "=", NAMEDATALEN)) {
        ReleaseSysCache(opertup);
        return;
    }
    ReleaseSysCache(opertup);

    Const *constNode = ((Const *) right);
    Datum datum = constNode->constvalue;

    TypeCacheEntry *typeEntry = lookup_type_cache(constNode->consttype, 0);

    /* constant gets varlena with 4B header, same with copy uility */
    datum = ShortVarlena(datum, typeEntry->typlen, typeEntry->typstorage);

    /*
     * We can push down this qual if:
     * - The operatory is =
     * - The qual is on the key column
     */
    readState->isKeyBased = true;
    readState->key = makeStringInfo();
    TupleDesc tupleDescriptor = relation->rd_att;

    #ifdef VIDARDB
    Oid foreignTableId = RelationGetRelid(relation);
    bool useDelimiter = IsColumnUsed(foreignTableId) && (varattno > 1);
    SerializeAttribute(tupleDescriptor, varattno-1, datum, readState->key, useDelimiter);
    #else
    SerializeAttribute(tupleDescriptor, varattno-1, datum, readState->key, false);
    #endif

    return;
}

static void BeginForeignScan(ForeignScanState *scanState, int executorFlags) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * Begin executing a foreign scan. This is called during executor startup.
     * It should perform any initialization needed before the scan can start,
     * but not start executing the actual scan (that should be done upon the
     * first call to IterateForeignScan). The ForeignScanState node has
     * already been created, but its fdw_state field is still NULL.
     * Information about the table to scan is accessible through the
     * ForeignScanState node (in particular, from the underlying ForeignScan
     * plan node, which contains any FDW-private information provided by
     * GetForeignPlan). eflags contains flag bits describing the executor's
     * operating mode for this plan node.
     *
     * Note that when (eflags & EXEC_FLAG_EXPLAIN_ONLY) is true, this function
     * should not perform any externally-visible actions; it should only do
     * the minimum required to make the node state valid for
     * ExplainForeignScan and EndForeignScan.
     *
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TableReadState *readState = palloc0(sizeof(TableReadState));
    readState->isKeyBased = false;
    readState->done = false;
    readState->key = NULL;

    scanState->fdw_state = (void *) readState;

    /* must after readState is recorded, otherwise explain won't close db */
    if (executorFlags & EXEC_FLAG_EXPLAIN_ONLY) {
        return;
    }

    ListCell *lc;
    foreach (lc, scanState->ss.ps.plan->qual) {
        Expr *state = lfirst(lc);
        GetKeyBasedQual((Node *) state,
                        scanState->ss.ss_currentRelation,
                        readState);
        if (readState->isKeyBased) {
            printf("\nkey_based_qual\n");
            break;
        }
    }

    if (!readState->isKeyBased) {
        Oid relationId = RelationGetRelid(scanState->ss.ss_currentRelation);

        #ifdef VIDARDB
        bool useColumn = IsColumnUsed(relationId);
        if (useColumn) {
            int listLen = list_length(scanState->ss.ps.plan->targetlist);
            RangeQueryOptions options;
            options.batchCapacity = GetBatchCapacity(relationId);
            options.startLen = 0;
            options.limitLen = 0;

            if (listLen > 0) {
                options.attrs = palloc(listLen * sizeof(*(options.attrs)));
                int i = 0;
                ListCell *targetCell;
                foreach (targetCell, scanState->ss.ps.plan->targetlist) {
                    TargetEntry *targetEntry = lfirst(targetCell);

                    /* TODO: The attr number to RangeQuery starts from 1 for now */   
                    *(options.attrs + i) = targetEntry->resorigcol - 1;
                    i++;
                }
            }
            options.attrCount = listLen;

            readState->hasNext = RangeQueryRequest(relationId,
                                                   ptr,
                                                   &options,
                                                   &readState->buf,
                                                   &readState->bufLen);

            readState->options = options;
            readState->next = readState->buf;
        } else {
            GetIterRequest(relationId, ptr);
        }
        #else
        GetIterRequest(relationId, ptr);
        #endif
    }
}

#ifdef VIDARDB
static void DeserializeTupleByColumn(StringInfo key,
                                     StringInfo val,
                                     TupleTableSlot *tupleSlot,
                                     List *targetList) {

    Datum *values = tupleSlot->tts_values;
    bool *nulls = tupleSlot->tts_isnull;

    TupleDesc tupleDescriptor = tupleSlot->tts_tupleDescriptor;
    int count = tupleDescriptor->natts;

    /* initialize all values for this row to null */
    memset(values, 0, count * sizeof(Datum));
    memset(nulls, false, count * sizeof(bool));

    int bufLen = (count - 1 + 7) / 8;

    StringInfo buffer = makeStringInfo();
    enlargeStringInfo(buffer, bufLen);
    buffer->len = bufLen;

    memcpy(buffer->data, val->data, bufLen);

    for (int index = 1; index < count; index++) {
        int byteIndex = (index - 1) / 8;
        int bitIndex = (index - 1) % 8;
        uint8 bitmask = (1 << bitIndex);
        nulls[index] = (buffer->data[byteIndex] & bitmask)? true: false;
    }

    AttrNumber *attrs = NULL;
    int targetListLen = list_length(targetList);
    if (targetListLen > 0) {
        attrs = (AttrNumber *) palloc(targetListLen * sizeof(*attrs));
        int i = 0;
        ListCell *targetCell;
        foreach (targetCell, targetList) {
            TargetEntry *targetEntry = lfirst(targetCell);
            *(attrs + i) = targetEntry->resorigcol;
            i++;
        }
    }

    int offset = 0;
    char *current = key->data;
    for (int index = 0; index < 2; index++) {
        Form_pg_attribute attributeForm = TupleDescAttr(tupleDescriptor, index);
        bool byValue = attributeForm->attbyval;
        int typeLength = attributeForm->attlen;

        values[index] = fetch_att(current, byValue, typeLength);
        offset = att_addlength_datum(offset, typeLength, current);
        offset += 1;
        if (index == 0) {
            offset = bufLen;
        }
        current = val->data + offset;
    }

    for (int targetIdx = 0; targetIdx < targetListLen; targetIdx++) {
        AttrNumber attr = *(attrs + targetIdx);
        /* after key and first value */
        if (attr <= 2) {
            continue;
        }
        attr--;
        Form_pg_attribute attributeForm = TupleDescAttr(tupleDescriptor, attr);
        bool byValue = attributeForm->attbyval;
        int typeLength = attributeForm->attlen;

        values[attr] = fetch_att(current, byValue, typeLength);
        offset = att_addlength_datum(offset, typeLength, current);
        offset += 1;
        current = val->data + offset;
    }
}
#endif

static void DeserializeTuple(StringInfo key,
                             StringInfo val,
                             TupleTableSlot *tupleSlot) {

    Datum *values = tupleSlot->tts_values;
    bool *nulls = tupleSlot->tts_isnull;

    TupleDesc tupleDescriptor = tupleSlot->tts_tupleDescriptor;
    int count = tupleDescriptor->natts;

    /* initialize all values for this row to null */
    memset(values, 0, count * sizeof(Datum));
    memset(nulls, false, count * sizeof(bool));

    int bufLen = (count - 1 + 7) / 8;

    StringInfo buffer = makeStringInfo();
    enlargeStringInfo(buffer, bufLen);
    buffer->len = bufLen;

    memcpy(buffer->data, val->data, bufLen);

    for (int index = 1; index < count; index++) {
        int byteIndex = (index - 1) / 8;
        int bitIndex = (index - 1) % 8;
        uint8 bitmask = (1 << bitIndex);
        nulls[index] = (buffer->data[byteIndex] & bitmask)? true: false;
    }

    int offset = 0;
    char *current = key->data;
    for (int index = 0; index < count; index++) {

        if (nulls[index]) {
            if (index == 0) {
                ereport(ERROR, (errmsg("first column cannot be null!")));
            }
            continue;
        }

        Form_pg_attribute attributeForm = TupleDescAttr(tupleDescriptor, index);
        bool byValue = attributeForm->attbyval;
        int typeLength = attributeForm->attlen;
        
        values[index] = fetch_att(current, byValue, typeLength);
        offset = att_addlength_datum(offset, typeLength, current);
            
        if (index == 0) {
            offset = bufLen;
        }
        
        current = val->data + offset;
    }
}

static TupleTableSlot *IterateForeignScan(ForeignScanState *scanState) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * Fetch one row from the foreign source, returning it in a tuple table
     * slot (the node's ScanTupleSlot should be used for this purpose). Return
     * NULL if no more rows are available. The tuple table slot infrastructure
     * allows either a physical or virtual tuple to be returned; in most cases
     * the latter choice is preferable from a performance standpoint. Note
     * that this is called in a short-lived memory context that will be reset
     * between invocations. Create a memory context in BeginForeignScan if you
     * need longer-lived storage, or use the es_query_cxt of the node's
     * EState.
     *
     * The rows returned must match the column signature of the foreign table
     * being scanned. If you choose to optimize away fetching columns that are
     * not needed, you should insert nulls in those column positions.
     *
     * Note that PostgreSQL's executor doesn't care whether the rows returned
     * violate any NOT NULL constraints that were defined on the foreign table
     * columns — but the planner does care, and may optimize queries
     * incorrectly if NULL values are present in a column declared not to
     * contain them. If a NULL value is encountered when the user has declared
     * that none should be present, it may be appropriate to raise an error
     * (just as you would need to do in the case of a data type mismatch).
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TupleTableSlot *tupleSlot = scanState->ss.ss_ScanTupleSlot;
    ExecClearTuple(tupleSlot);

    TableReadState *readState = (TableReadState *) scanState->fdw_state;
    char *k = NULL, *v = NULL;
    size_t kLen = 0, vLen = 0;

    Oid relationId = RelationGetRelid(scanState->ss.ss_currentRelation);
    #ifdef VIDARDB
    bool useColumn = IsColumnUsed(relationId);
    #endif
    bool found = false;
    if (readState->isKeyBased) {
        if (!readState->done) {
            k = readState->key->data;
            kLen = readState->key->len;
            found = GetRequest(relationId, ptr, k, kLen, &v, &vLen);
            readState->done = true;
        }
    } else {
        #ifdef VIDARDB
        if (useColumn && readState->bufLen != 0) {

            char *bufEnd = readState->buf + readState->bufLen;
            if (readState->next < bufEnd) {
                memcpy(&kLen, readState->next, sizeof(kLen));
                readState->next += sizeof(kLen);
                k = readState->next;
                readState->next += kLen;

                memcpy(&vLen, readState->next, sizeof(vLen));
                readState->next += sizeof(vLen);
                v = readState->next;
                readState->next += vLen;

                found = true;
            } else if (readState->hasNext) {
                Munmap(readState->buf, readState->bufLen, __func__);
                readState->hasNext = RangeQueryRequest(relationId,
                                                       ptr,
                                                       &readState->options,
                                                       &readState->buf,
                                                       &readState->bufLen);

                readState->next = readState->buf;

                if (readState->bufLen != 0) {
                    memcpy(&kLen, readState->next, sizeof(kLen));
                    readState->next += sizeof(kLen);
                    k = readState->next;
                    readState->next += kLen;

                    memcpy(&vLen, readState->next, sizeof(vLen));
                    readState->next += sizeof(vLen);
                    v = readState->next;
                    readState->next += vLen;

                    found = true;
                }
            }
        } else {
            found = NextRequest(relationId, ptr, &k, &kLen, &v, &vLen);
        }
        #else
        found = NextRequest(relationId, ptr, &k, &kLen, &v, &vLen);
        #endif
    }

    if (found) {
        StringInfo key = makeStringInfo();
        appendBinaryStringInfo(key, k, kLen);
        StringInfo val = makeStringInfo();
        appendBinaryStringInfo(val, v, vLen);

        #ifdef VIDARDB
        if (useColumn) {
            DeserializeTupleByColumn(key, val, tupleSlot, scanState->ss.ps.plan->targetlist);
        } else {
            DeserializeTuple(key, val, tupleSlot);
        }
        #else
        DeserializeTuple(key, val, tupleSlot);
        #endif

        ExecStoreVirtualTuple(tupleSlot);
    }

    return tupleSlot;
}

static void ReScanForeignScan(ForeignScanState *scanState) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * Restart the scan from the beginning. Note that any parameters the scan
     * depends on may have changed value, so the new scan does not necessarily
     * return exactly the same rows.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));
}

static void EndForeignScan(ForeignScanState *scanState) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * End the scan and release resources. It is normally not important to
     * release palloc'd memory, but for example open files and connections to
     * remote servers should be cleaned up.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TableReadState *readState = (TableReadState *) scanState->fdw_state;

    if (readState) {
        Oid relationId = RelationGetRelid(scanState->ss.ss_currentRelation);

        #ifdef VIDARDB
        bool useColumn = IsColumnUsed(relationId);
        if (useColumn == false) {
            DelIterRequest(relationId, ptr);
        }
        #else
        DelIterRequest(relationId, ptr);
        #endif

        CloseRequest(relationId, ptr);

        pfree(readState);
    }
}

static void AddForeignUpdateTargets(Query *parsetree,
                                    RangeTblEntry *tableEntry,
                                    Relation targetRelation) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * UPDATE and DELETE operations are performed against rows previously
     * fetched by the table-scanning functions. The FDW may need extra
     * information, such as a row ID or the values of primary-key columns, to
     * ensure that it can identify the exact row to update or delete. To
     * support that, this function can add extra hidden, or "junk", target
     * columns to the list of columns that are to be retrieved from the
     * foreign table during an UPDATE or DELETE.
     *
     * To do that, add TargetEntry items to parsetree->targetList, containing
     * expressions for the extra values to be fetched. Each such entry must be
     * marked resjunk = true, and must have a distinct resname that will
     * identify it at execution time. Avoid using names matching ctidN or
     * wholerowN, as the core system can generate junk columns of these names.
     *
     * This function is called in the rewriter, not the planner, so the
     * information available is a bit different from that available to the
     * planning routines. parsetree is the parse tree for the UPDATE or DELETE
     * command, while target_rte and target_relation describe the target
     * foreign table.
     *
     * If the AddForeignUpdateTargets pointer is set to NULL, no extra target
     * expressions are added. (This will make it impossible to implement
     * DELETE operations, though UPDATE may still be feasible if the FDW
     * relies on an unchanging primary key to identify rows.)
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    Form_pg_attribute attr = &RelationGetDescr(targetRelation)->attrs[0];

    /*
     * Code adapted from redis_fdw
     *
     * In KV, we need the key name. It's the first column in the table
     * regardless of the table type. Knowing the key, we can delete it.
     */
    Var *var = makeVar(parsetree->resultRelation,
                       1,
                       attr->atttypid,
                       attr->atttypmod,
                       InvalidOid,
                       0);
    /* Wrap it in a resjunk TLE with the right name ... */
    const char *attrname = KVKEYJUNK;
    AttrNumber resno = list_length(parsetree->targetList) + 1;
    /* is this true? */
    Assert(resno == 1);
    TargetEntry *entry = makeTargetEntry((Expr *) var,
                                         resno,
                                         pstrdup(attrname),
                                         true);
    /* ... and add it to the query's targetlist */
    parsetree->targetList = lappend(parsetree->targetList, entry);
}

static List *PlanForeignModify(PlannerInfo *plannerInfo,
                               ModifyTable *plan,
                               Index resultRelation,
                               int subplanIndex) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * Perform any additional planning actions needed for an insert, update,
     * or delete on a foreign table. This function generates the FDW-private
     * information that will be attached to the ModifyTable plan node that
     * performs the update action. This private information must have the form
     * of a List, and will be delivered to BeginForeignModify during the
     * execution stage.
     *
     * root is the planner's global information about the query. plan is the
     * ModifyTable plan node, which is complete except for the fdwPrivLists
     * field. resultRelation identifies the target foreign table by its
     * rangetable index. subplan_index identifies which target of the
     * ModifyTable plan node this is, counting from zero; use this if you want
     * to index into plan->plans or other substructure of the plan node.
     *
     * If the PlanForeignModify pointer is set to NULL, no additional
     * plan-time actions are taken, and the fdw_private list delivered to
     * BeginForeignModify will be NIL.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    return NULL;
}

static void BeginForeignModify(ModifyTableState *modifyTableState,
                               ResultRelInfo *relationInfo,
                               List *fdwPrivate,
                               int subplanIndex,
                               int executorFlags) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * Begin executing a foreign table modification operation. This routine is
     * called during executor startup. It should perform any initialization
     * needed prior to the actual table modifications. Subsequently,
     * ExecForeignInsert, ExecForeignUpdate or ExecForeignDelete will be
     * called for each tuple to be inserted, updated, or deleted.
     *
     * mtstate is the overall state of the ModifyTable plan node being
     * executed; global data about the plan and execution state is available
     * via this structure. rinfo is the ResultRelInfo struct describing the
     * target foreign table. (The ri_FdwState field of ResultRelInfo is
     * available for the FDW to store any private state it needs for this
     * operation.) fdw_private contains the private data generated by
     * PlanForeignModify, if any. subplan_index identifies which target of the
     * ModifyTable plan node this is. eflags contains flag bits describing the
     * executor's operating mode for this plan node.
     *
     * Note that when (eflags & EXEC_FLAG_EXPLAIN_ONLY) is true, this function
     * should not perform any externally-visible actions; it should only do
     * the minimum required to make the node state valid for
     * ExplainForeignModify and EndForeignModify.
     *
     * If the BeginForeignModify pointer is set to NULL, no action is taken
     * during executor startup.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    if (executorFlags & EXEC_FLAG_EXPLAIN_ONLY) {
        return;
    }

    TableWriteState *writeState = palloc0(sizeof(TableWriteState));

    CmdType operation = modifyTableState->operation;
    writeState->operation = operation;

    Relation relation = relationInfo->ri_RelationDesc;

    Oid foreignTableId = RelationGetRelid(relation);
    heap_open(foreignTableId, ShareUpdateExclusiveLock);

    if (operation == CMD_INSERT) {
        #ifdef VIDARDB
        bool useColumn = IsColumnUsed(foreignTableId);
        int attrCount = useColumn? GetAttrCount(foreignTableId): 0;
        ptr = OpenRequest(foreignTableId, ptr, useColumn, attrCount);
        #else
        ptr = OpenRequest(foreignTableId, ptr);
        #endif
    }

    if (operation == CMD_DELETE) {
        /* Find the ctid resjunk column in the subplan's result */
        Plan *subplan = modifyTableState->mt_plans[subplanIndex]->plan;
        writeState->keyJunkNo =
                ExecFindJunkAttributeInTlist(subplan->targetlist, KVKEYJUNK);
        if (!AttributeNumberIsValid(writeState->keyJunkNo)) {
            ereport(ERROR, (errmsg("could not find key junk column")));
        }
    }

    relationInfo->ri_FdwState = (void *) writeState;
}

static void SerializeTuple(StringInfo key,
                           StringInfo val,
                           TupleTableSlot *tupleSlot,
                           bool useDelimiter) {

    TupleDesc tupleDescriptor = tupleSlot->tts_tupleDescriptor;
    int count = tupleDescriptor->natts;

    /* first attr must exist */
    int nullsLen = (count - 1 + 7) / 8;

    StringInfo nulls = makeStringInfo();
    enlargeStringInfo(nulls, nullsLen);
    nulls->len = nullsLen;
    memset(nulls->data, 0, nullsLen);

    val->len += nullsLen;

    for (int index = 0; index < count; index++) {

        if (tupleSlot->tts_isnull[index]) {
            if (index == 0) {
                ereport(ERROR, (errmsg("first column cannot be null!")));
            }
            int byteIndex = (index - 1) / 8;
            int bitIndex = (index - 1) % 8;
            uint8 bitmask = (1 << bitIndex);
            nulls->data[byteIndex] |= bitmask;
            continue;
        }

        Datum datum = tupleSlot->tts_values[index];

        /*The last column does not require a delimiter*/
        if (index == count - 1) {
            useDelimiter = false;
        }
        useDelimiter = (useDelimiter && index > 0);
        SerializeAttribute(tupleDescriptor, index, datum, index==0? key: val, useDelimiter);
    }

    memcpy(val->data, nulls->data, nullsLen);
}

static TupleTableSlot *ExecForeignInsert(EState *executorState,
                                         ResultRelInfo *relationInfo,
                                         TupleTableSlot *tupleSlot,
                                         TupleTableSlot *planSlot) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * Insert one tuple into the foreign table. estate is global execution
     * state for the query. rinfo is the ResultRelInfo struct describing the
     * target foreign table. slot contains the tuple to be inserted; it will
     * match the rowtype definition of the foreign table. planSlot contains
     * the tuple that was generated by the ModifyTable plan node's subplan; it
     * differs from slot in possibly containing additional "junk" columns.
     * (The planSlot is typically of little interest for INSERT cases, but is
     * provided for completeness.)
     *
     * The return value is either a slot containing the data that was actually
     * inserted (this might differ from the data supplied, for example as a
     * result of trigger actions), or NULL if no row was actually inserted
     * (again, typically as a result of triggers). The passed-in slot can be
     * re-used for this purpose.
     *
     * The data in the returned slot is used only if the INSERT query has a
     * RETURNING clause. Hence, the FDW could choose to optimize away
     * returning some or all columns depending on the contents of the
     * RETURNING clause. However, some slot must be returned to indicate
     * success, or the query's reported rowcount will be wrong.
     *
     * If the ExecForeignInsert pointer is set to NULL, attempts to insert
     * into the foreign table will fail with an error message.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TupleDesc tupleDescriptor = tupleSlot->tts_tupleDescriptor;
    if (HeapTupleHasExternal(tupleSlot->tts_tuple)) {
        /* detoast any toasted attributes */
        tupleSlot->tts_tuple = toast_flatten_tuple(tupleSlot->tts_tuple,
                                                   tupleDescriptor);
    }

    slot_getallattrs(tupleSlot);

    StringInfo key = makeStringInfo();
    StringInfo val = makeStringInfo();

    Relation relation = relationInfo->ri_RelationDesc;
    Oid foreignTableId = RelationGetRelid(relation);

    #ifdef VIDARDB
    bool useDelimiter = IsColumnUsed(foreignTableId);
    SerializeTuple(key, val, tupleSlot, useDelimiter);
    #else
    SerializeTuple(key, val, tupleSlot, false);
    #endif

    PutRequest(foreignTableId, ptr, key->data, key->len, val->data, val->len);

    return tupleSlot;
}

static TupleTableSlot *ExecForeignUpdate(EState *executorState,
                                         ResultRelInfo *relationInfo,
                                         TupleTableSlot *tupleSlot,
                                         TupleTableSlot *planSlot) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * Update one tuple in the foreign table. estate is global execution state
     * for the query. rinfo is the ResultRelInfo struct describing the target
     * foreign table. slot contains the new data for the tuple; it will match
     * the rowtype definition of the foreign table. planSlot contains the
     * tuple that was generated by the ModifyTable plan node's subplan; it
     * differs from slot in possibly containing additional "junk" columns. In
     * particular, any junk columns that were requested by
     * AddForeignUpdateTargets will be available from this slot.
     *
     * The return value is either a slot containing the row as it was actually
     * updated (this might differ from the data supplied, for example as a
     * result of trigger actions), or NULL if no row was actually updated
     * (again, typically as a result of triggers). The passed-in slot can be
     * re-used for this purpose.
     *
     * The data in the returned slot is used only if the UPDATE query has a
     * RETURNING clause. Hence, the FDW could choose to optimize away
     * returning some or all columns depending on the contents of the
     * RETURNING clause. However, some slot must be returned to indicate
     * success, or the query's reported rowcount will be wrong.
     *
     * If the ExecForeignUpdate pointer is set to NULL, attempts to update the
     * foreign table will fail with an error message.
     *
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TupleDesc tupleDescriptor = tupleSlot->tts_tupleDescriptor;
    if (HeapTupleHasExternal(tupleSlot->tts_tuple)) {
        /* detoast any toasted attributes */
        tupleSlot->tts_tuple = toast_flatten_tuple(tupleSlot->tts_tuple,
                                                   tupleDescriptor);
    }
    slot_getallattrs(tupleSlot);

    StringInfo key = makeStringInfo();
    StringInfo val = makeStringInfo();

    Relation relation = relationInfo->ri_RelationDesc;
    Oid foreignTableId = RelationGetRelid(relation);

    #ifdef VIDARDB
    bool useDelimiter = IsColumnUsed(foreignTableId);
    SerializeTuple(key, val, tupleSlot, useDelimiter);
    #else
    SerializeTuple(key, val, tupleSlot, false);
    #endif

    PutRequest(foreignTableId, ptr, key->data, key->len, val->data, val->len);

    return tupleSlot;
}

static TupleTableSlot *ExecForeignDelete(EState *executorState,
                                         ResultRelInfo *relationInfo,
                                         TupleTableSlot *tupleSlot,
                                         TupleTableSlot *planSlot) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * Delete one tuple from the foreign table. estate is global execution
     * state for the query. rinfo is the ResultRelInfo struct describing the
     * target foreign table. slot contains nothing useful upon call, but can
     * be used to hold the returned tuple. planSlot contains the tuple that
     * was generated by the ModifyTable plan node's subplan; in particular, it
     * will carry any junk columns that were requested by
     * AddForeignUpdateTargets. The junk column(s) must be used to identify
     * the tuple to be deleted.
     *
     * The return value is either a slot containing the row that was deleted,
     * or NULL if no row was deleted (typically as a result of triggers). The
     * passed-in slot can be used to hold the tuple to be returned.
     *
     * The data in the returned slot is used only if the DELETE query has a
     * RETURNING clause. Hence, the FDW could choose to optimize away
     * returning some or all columns depending on the contents of the
     * RETURNING clause. However, some slot must be returned to indicate
     * success, or the query's reported rowcount will be wrong.
     *
     * If the ExecForeignDelete pointer is set to NULL, attempts to delete
     * from the foreign table will fail with an error message.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TableWriteState *writeState = (TableWriteState *) relationInfo->ri_FdwState;

    bool isnull = true;
    ExecGetJunkAttribute(planSlot, writeState->keyJunkNo, &isnull);
    if (isnull) {
        ereport(ERROR, (errmsg("can't get junk key value")));
    }

    slot_getallattrs(planSlot);

    StringInfo key = makeStringInfo();
    StringInfo val = makeStringInfo();

    Relation relation = relationInfo->ri_RelationDesc;
    Oid foreignTableId = RelationGetRelid(relation);

    #ifdef VIDARDB
    bool useDelimiter = IsColumnUsed(foreignTableId);
    SerializeTuple(key, val, planSlot, useDelimiter);
    #else
    SerializeTuple(key, val, planSlot, false);
    #endif

    DeleteRequest(foreignTableId, ptr, key->data, key->len);

    return tupleSlot;
}

static void EndForeignModify(EState *executorState,
                             ResultRelInfo *relationInfo) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * End the table update and release resources. It is normally not
     * important to release palloc'd memory, but for example open files and
     * connections to remote servers should be cleaned up.
     *
     * If the EndForeignModify pointer is set to NULL, no action is taken
     * during executor shutdown.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    TableWriteState *writeState = (TableWriteState *) relationInfo->ri_FdwState;

    if (writeState) {
        Relation relation = relationInfo->ri_RelationDesc;
        Oid foreignTableId = RelationGetRelid(relation);

        CmdType operation = writeState->operation;
        if (operation == CMD_INSERT) {
            CloseRequest(foreignTableId, ptr);
        }

        /* CMD_UPDATE and CMD_DELETE close will be taken care of by endScan */
        heap_close(relation, ShareUpdateExclusiveLock);

        pfree(writeState);
    }
}

static void ExplainForeignScan(ForeignScanState *scanState,
                               struct ExplainState * explainState) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * Print additional EXPLAIN output for a foreign table scan. This function
     * can call ExplainPropertyText and related functions to add fields to the
     * EXPLAIN output. The flag fields in es can be used to determine what to
     * print, and the state of the ForeignScanState node can be inspected to
     * provide run-time statistics in the EXPLAIN ANALYZE case.
     *
     * If the ExplainForeignScan pointer is set to NULL, no additional
     * information is printed during EXPLAIN.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));
}

static void ExplainForeignModify(ModifyTableState *modifyTableState,
                                 ResultRelInfo *relationInfo,
                                 List *fdwPrivate,
                                 int subplanIndex,
                                 struct ExplainState *explainState) {
    printf("\n-----------------%s----------------------\n", __func__);
    /*
     * Print additional EXPLAIN output for a foreign table update. This
     * function can call ExplainPropertyText and related functions to add
     * fields to the EXPLAIN output. The flag fields in es can be used to
     * determine what to print, and the state of the ModifyTableState node can
     * be inspected to provide run-time statistics in the EXPLAIN ANALYZE
     * case. The first four arguments are the same as for BeginForeignModify.
     *
     * If the ExplainForeignModify pointer is set to NULL, no additional
     * information is printed during EXPLAIN.
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));
}

static bool AnalyzeForeignTable(Relation relation,
                                AcquireSampleRowsFunc *acquireSampleRowsFunc,
                                BlockNumber *totalPageCount) {
    printf("\n-----------------%s----------------------\n", __func__);
    /* ----
     * This function is called when ANALYZE is executed on a foreign table. If
     * the FDW can collect statistics for this foreign table, it should return
     * true, and provide a pointer to a function that will collect sample rows
     * from the table in func, plus the estimated size of the table in pages
     * in totalpages. Otherwise, return false.
     *
     * If the FDW does not support collecting statistics for any tables, the
     * AnalyzeForeignTable pointer can be set to NULL.
     *
     * If provided, the sample collection function must have the signature:
     *
     *	  int
     *	  AcquireSampleRowsFunc (Relation relation, int elevel,
     *							 HeapTuple *rows, int targrows,
     *							 double *totalrows,
     *							 double *totaldeadrows);
     *
     * A random sample of up to targrows rows should be collected from the
     * table and stored into the caller-provided rows array. The actual number
     * of rows collected must be returned. In addition, store estimates of the
     * total numbers of live and dead rows in the table into the output
     * parameters totalrows and totaldeadrows. (Set totaldeadrows to zero if
     * the FDW does not have any concept of dead rows.)
     * ----
     */

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    return false;
}

Datum kv_fdw_handler(PG_FUNCTION_ARGS) {
    printf("\n-----------------%s----------------------\n", __func__);
    FdwRoutine *fdwRoutine = makeNode(FdwRoutine);

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    /*
     * assign the handlers for the FDW
     *
     * This function might be called a number of times. In particular, it is
     * likely to be called for each INSERT statement. For an explanation, see
     * core postgres file src/optimizer/plan/createplan.c where it calls
     * GetFdwRoutineByRelId(().
     */

    /* Required by notations: S=SELECT I=INSERT U=UPDATE D=DELETE */

    /* these are required */
    fdwRoutine->GetForeignRelSize = GetForeignRelSize; /* S U D */
    fdwRoutine->GetForeignPaths = GetForeignPaths; /* S U D */
    fdwRoutine->GetForeignPlan = GetForeignPlan; /* S U D */
    fdwRoutine->BeginForeignScan = BeginForeignScan; /* S U D */
    fdwRoutine->IterateForeignScan = IterateForeignScan; /* S */
    fdwRoutine->ReScanForeignScan = ReScanForeignScan; /* S */
    fdwRoutine->EndForeignScan = EndForeignScan; /* S U D */

    /* remainder are optional - use NULL if not required */
    /* support for insert / update / delete */
    fdwRoutine->AddForeignUpdateTargets = AddForeignUpdateTargets; /* U D */
    fdwRoutine->PlanForeignModify = PlanForeignModify; /* I U D */
    fdwRoutine->BeginForeignModify = BeginForeignModify; /* I U D */
    fdwRoutine->ExecForeignInsert = ExecForeignInsert; /* I */
    fdwRoutine->ExecForeignUpdate = ExecForeignUpdate; /* U */
    fdwRoutine->ExecForeignDelete = ExecForeignDelete; /* D */
    fdwRoutine->EndForeignModify = EndForeignModify; /* I U D */

    /* support for EXPLAIN */
    fdwRoutine->ExplainForeignScan = ExplainForeignScan; /* EXPLAIN S U D */
    fdwRoutine->ExplainForeignModify = ExplainForeignModify; /* EXPLAIN I U D */

    /* support for ANALYSE */
    fdwRoutine->AnalyzeForeignTable = AnalyzeForeignTable; /* ANALYZE only */

    PG_RETURN_POINTER(fdwRoutine);
}

Datum kv_fdw_validator(PG_FUNCTION_ARGS) {
    printf("\n-----------------%s----------------------\n", __func__);
    //List *options_list = untransformRelOptions(PG_GETARG_DATUM(0));

    ereport(DEBUG1, (errmsg("entering function %s", __func__)));

    /* make sure the options are valid */

    /* no options are supported */

    /*if (list_length(options_list) > 0) {
        ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                        errmsg("invalid options"),
                        errhint("FDW does not support any options")));
    }*/

    PG_RETURN_VOID();
}
