/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#ifndef VC4C_CONTROLFLOWGRAPH_H
#define VC4C_CONTROLFLOWGRAPH_H

#include "../Graph.h"
#include "../InstructionWalker.h"
#include "../Method.h"
#include "ControlFlowLoop.h"

namespace vc4c
{
    /*
     * A relation in the control-flow-graph represents a transition between two basic blocks.
     *
     * NOTE: This transition can be bi-directional!
     *
     */
    class CFGRelation
    {
    public:
        // this is used for only optimizations::removeConstantLoadInLoops
        bool isBackEdge = false;

        bool operator==(const CFGRelation& other) const;

        std::string getLabel() const;

        /**
         * Returns the last instruction executed in the source block (e.g. the branch) before the control flow switches
         * to the destination block.
         */
        InstructionWalker getPredecessor(BasicBlock* source) const;

        /**
         * Returns whether the branch in the given direction (source to destination) is implicit.
         */
        bool isImplicit(BasicBlock* source) const;

    private:
        // map of the source block and the predecessor within this block (empty for fall-through)
        std::map<BasicBlock*, Optional<InstructionWalker>> predecessors;

        friend class ControlFlowGraph;
    };

    using CFGNode = Node<BasicBlock*, CFGRelation, Directionality::BIDIRECTIONAL>;
    bool operator<(const CFGNode& one, const CFGNode& other);

    using CFGEdge = CFGNode::EdgeType;

    /*
     * The control-flow graph (CFG) represents the order/relation of basic-blocks by connecting basic-blocks which can
     * follow directly after one another (e.g. by branching or fall-through)
     */
    class ControlFlowGraph : public Graph<BasicBlock*, CFGNode>
    {
    public:
        /*
         * Returns the node which represents the first basic-block being executed by the QPU
         */
        CFGNode& getStartOfControlFlow();

        /*
         * Returns the single node which represents the last basic-block being executed
         *
         * NOTE: For non-kernel function there may be multiple last blocks (e.g. containing return-statements) in which
         * case this function will throw!
         */
        CFGNode& getEndOfControlFlow();

        /*
         * Finds all loops in the CFG
         */
        FastAccessList<ControlFlowLoop> findLoops(bool recursively);

        /*
         * Dump this graph as dot file
         */
        void dumpGraph(const std::string& path, bool dumpConstantLoadInstructions) const;

        void updateOnBlockInsertion(Method& method, BasicBlock& newBlock);
        void updateOnBlockRemoval(Method& method, BasicBlock& oldBlock);
        void updateOnBranchInsertion(Method& method, InstructionWalker it);
        void updateOnBranchRemoval(Method& method, BasicBlock& affectedBlock, const Local* branchTarget);

        /*
         * Creates the CFG from the basic-blocks within the given method
         */
        static std::unique_ptr<ControlFlowGraph> createCFG(Method& method);

        /*
         * Clone the CFG
         */
        std::unique_ptr<ControlFlowGraph> clone();

    private:
        explicit ControlFlowGraph(std::size_t numBlocks) : Graph(numBlocks) {}
        /*
         * This is a modified version of the Tarjan's Algorithm to find strongly connected components taken from
         * https://www.geeksforgeeks.org/tarjan-algorithm-find-strongly-connected-components/
         *
         * A recursive function that finds and prints strongly connected components using DFS traversal
         * node --> The node to be visited next
         * discoveryTimes --> Stores discovery times of visited nodes
         * lowestReachable --> earliest visited node (the node with minimum discovery time) that can be reached from
         * subtree rooted with current node stack --> To store all the connected ancestors (could be part of SCC)
         */
        ControlFlowLoop findLoopsHelper(const CFGNode* node, FastMap<const CFGNode*, int>& discoveryTimes,
            FastMap<const CFGNode*, int>& lowestReachable, FastModificationList<const CFGNode*>& stack, int& time);

        /*
         * This is similar to findLoopsHelper, but this finds also nested loops excluding one-block-loop.
         */
        FastAccessList<ControlFlowLoop> findLoopsHelperRecursively(const CFGNode* node,
            FastMap<const CFGNode*, int>& discoveryTimes, FastModificationList<const CFGNode*>& stack, int& time);

        friend class Method;
    };

} /* namespace vc4c */

#endif /* VC4C_CONTROLFLOWGRAPH_H */
