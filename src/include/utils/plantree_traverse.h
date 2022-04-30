#include "nodes/plannodes.h"
#include "nodes/pg_list.h"
#include "nodes/nodes.h"

/* The func is expected to return the node itself, or a new node if necessary for replacement. */
Plan *PreOrderPlantreeTraverse(Plan *node, Plan* (*func)(Plan *, struct List *), struct List *closure) {
    if (node == NULL) {
        return NULL;
    }

    node->lefttree = PreOrderPlantreeTraverse(node->lefttree, func, closure);
    node->righttree = PreOrderPlantreeTraverse(node->righttree, func, closure);
    
    Plan *nodeResult = func(node, closure);
    return nodeResult;
}