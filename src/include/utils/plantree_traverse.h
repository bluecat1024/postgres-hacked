#include "nodes/plannodes.h"
#include "nodes/pg_list.h"
#include "nodes/nodes.h"
#include "nodes/plannodes.h"

/* The func is expected to return the node itself, or a new node if necessary for replacement. */
Plan *PreOrderPlantreeTraverse(Plan *node, Plan* (*func)(Plan *, struct List *, PlannedStmt *), struct List *closure, PlannedStmt* stmt) {
    if (node == NULL) {
        return NULL;
    }

    Plan *nodeResult = func(node, closure, stmt);

    nodeResult->lefttree = PreOrderPlantreeTraverse(nodeResult->lefttree, func, closure, stmt);
    nodeResult->righttree = PreOrderPlantreeTraverse(nodeResult->righttree, func, closure, stmt);
    
    
    return nodeResult;
}

Plan *PostOrderPlantreeTraverse(Plan *node, Plan* (*func)(Plan *, struct List *, PlannedStmt *), struct List *closure, PlannedStmt* stmt) {
    if (node == NULL) {
        return NULL;
    }

    node->lefttree = PostOrderPlantreeTraverse(node->lefttree, func, closure, stmt);
    node->righttree = PostOrderPlantreeTraverse(node->righttree, func, closure, stmt);
    
    Plan *nodeResult = func(node, closure, stmt);

    return nodeResult;
}