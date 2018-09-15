//
// Created by nomaddo on 18/06/02.
//

#include <algorithm>

#include "Scheduler.h"
#include "../Graph.h"
#include "../analysis/DataDependencyGraph.h"
#include "../InstructionWalker.h"
#include "../Values.h"

using namespace vc4c;

using IL = intermediate::IntermediateInstruction;

class DAGNodeBase
{
    DAGNodeBase() = default;
};


// template <typename Key, typename Relation, Directionality Direction, typename Base>
class DAGNode : public Node<IL*, int, Directionality::DIRECTED, DAGNodeBase>
{

};

class InstructionDAG : public Graph<IL*, DAGNode>
{
    std::set<DAGNode> roots;

    void updateRoots()
    {
        roots = std::set<DAGNode>();
        std::function<bool(const DAGNode&)> func = [&](const DAGNode& node) -> bool {
            roots.emplace(node);
            return true;
        };
        forAllSources(func);
    }

public:
    std::set<DAGNode> getRoots() const
    {
        return roots;
    }

    std::set<intermediate::IntermediateInstruction*> getPairCandiate ()
    {
        auto roots = getRoots();
        auto set = std::set<intermediate::IntermediateInstruction*>();
        for (auto& ila : roots)
            for (auto& ilb : roots)
            {
                auto a = ila.key;
                auto b = ilb.key;

                if (a != b && dynamic_cast<intermediate::Operation*>(a) && dynamic_cast<intermediate::Operation*>(b))
                {
                    auto aop = dynamic_cast<intermediate::Operation*>(a);
                    auto bop = dynamic_cast<intermediate::Operation*>(b);
                    auto combined = new intermediate::CombinedOperation(aop, bop);
                    if (combined->canBeCombined)
                    {
                        set.emplace(combined);
                    } else
                    {
                        delete(combined);
                    }
                }
            }

        return set;
    }

    InstructionDAG(Method & method, BasicBlock & bb)
    {
        auto walker = bb.begin();
        auto instrs = std::vector<IL*>();

        while (walker != bb.end())
        {
            if (dynamic_cast<intermediate::Nop*>(walker.get())) {
                walker.nextInBlock();
                continue;
            }

            IL* copied = walker.get()->copyFor(method, "");
            instrs.push_back(copied);
            walker.nextInBlock();
        }

        std::reverse(instrs.begin(), instrs.end());

        auto defs = std::unordered_map<Value, IL*>();

        for (auto il : instrs)
        {
            for (auto v : il->getArguments())
            {
                auto it = defs.find(v);
                if (it != defs.end())
                {
                    this->getOrCreateNode(il).addEdge(&this->getOrCreateNode(it->second), 0);
                }
            }

            il->getOutput().ifPresent([&](const Value &v) -> bool {
                defs[v] = il;
                return true;
            });
        }

        updateRoots();
    }
};

void vc4c::optimizations::Scheduler::doScheduling(vc4c::Method &method, const vc4c::Configuration & config) {
    for (BasicBlock & bb : method)
    {
        auto dag = new InstructionDAG(method, bb);
        bb.reset();

        auto pairCandidate = dag->getPairCandiate();
        if (! pairCandidate.empty()) {
            auto il = * pairCandidate.begin();
            bb.end().emplace(il);

            if (auto combined = dynamic_cast<intermediate::CombinedOperation*>(il)) {
                dag->eraseNode(const_cast<IL *>(combined->op1.get()));
                dag->eraseNode(const_cast<IL *>(combined->op2.get()));
            }
        }

        auto instr = dag->getRoots().begin()->key;
        bb.end().emplace(instr);
        dag->eraseNode(instr);
    }
}
