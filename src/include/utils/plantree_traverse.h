#include "nodes/plannodes.h"
#include "nodes/pg_list.h"
#include "nodes/nodes.h"

/* The func is expected to return the node itself, or a new node if necessary for replacement. */
Plan *PreOrderPlantreeTraverse(Plan *node, Plan* (*func)(Plan *, struct List *), struct List *closure) {
    if (node == NULL) {
        return NULL;
    }

    Plan *nodeResult = func(node, closure);

    nodeResult->lefttree = PreOrderPlantreeTraverse(nodeResult->lefttree, func, closure);
    nodeResult->righttree = PreOrderPlantreeTraverse(nodeResult->righttree, func, closure);
    
    
    return nodeResult;
}

Plan *PostOrderPlantreeTraverse(Plan *node, Plan* (*func)(Plan *, struct List *), struct List *closure) {
    if (node == NULL) {
        return NULL;
    }

    node->lefttree = PostOrderPlantreeTraverse(node->lefttree, func, closure);
    node->righttree = PostOrderPlantreeTraverse(node->righttree, func, closure);
    
    Plan *nodeResult = func(node, closure);

    return nodeResult;
}