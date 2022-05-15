// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Scheduling - partitioning
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2022 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
//
// V3SchedPartition (and in particular V3Sched::partition) partitions all
// logic into two regions, the 'act' region contains all logic that might
// compute a clock via an update even that falls into the SystemVerilog Active
// scheduling region (that is: blocking and continuous assignments in
// particular). All other logic is assigned to the 'nba' region.
//
// To achieve this, we build a dependency graph of all logic in the design,
// and trace back from every AstSenItem through all logic that might (via an
// Active region update) feed into triggering that AstSenItem. Any such logic
// is then assigned to the 'act' region, and all other logic is assigned to
// the 'nba' region.
//
// For later practical purposes, AstAssignPre logic that would be assigned to
// the 'act' region is returned separately. Nevertheless, this logic is part of
// the 'act' region.
//
// For more details, please see the internals documentation.
//
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3Ast.h"
#include "V3Error.h"
#include "V3Global.h"
#include "V3Sched.h"
#include "V3Graph.h"
#include "V3EmitV.h"

#include <tuple>
#include <unordered_map>
#include <vector>

namespace V3Sched {

namespace {

class SchedSenVertex final : public V3GraphVertex {
    const AstSenItem* const m_senItemp;

public:
    SchedSenVertex(V3Graph* graphp, const AstSenItem* senItemp)
        : V3GraphVertex{graphp}
        , m_senItemp{senItemp} {}
    string name() const override {
        std::ostringstream os;
        V3EmitV::verilogForTree(const_cast<AstSenItem*>(m_senItemp), os);
        return os.str();
    }
    string dotShape() const override { return "doubleoctagon"; }
    string dotColor() const override { return "red"; }
};

class SchedLogicVertex final : public V3GraphVertex {
    AstScope* const m_scopep;
    AstSenTree* const m_senTreep;
    AstNode* const m_logicp;

public:
    SchedLogicVertex(V3Graph* graphp, AstScope* scopep, AstSenTree* senTreep, AstNode* logicp)
        : V3GraphVertex{graphp}
        , m_scopep{scopep}
        , m_senTreep{senTreep}
        , m_logicp{logicp} {}
    AstScope* scopep() const { return m_scopep; }
    AstSenTree* senTreep() const { return m_senTreep; }
    AstNode* logicp() const { return m_logicp; }

    string name() const override {
        return m_logicp->typeName() + ("\n" + m_logicp->fileline()->ascii());
    };
    string dotShape() const override { return "rectangle"; }
};

class SchedVarVertex final : public V3GraphVertex {
    const AstVarScope* const m_vscp;

public:
    SchedVarVertex(V3Graph* graphp, AstVarScope* vscp)
        : V3GraphVertex{graphp}
        , m_vscp{vscp} {}
    string name() const override { return m_vscp->name(); }
    string dotShape() const override {
        return m_vscp->scopep()->isTop() && m_vscp->varp()->isNonOutput() ? "invhouse" : "ellipse";
    }
    string dotColor() const override {
        return m_vscp->scopep()->isTop() && m_vscp->varp()->isNonOutput() ? "green" : "black";
    }
};

class SchedGraphBuilder final : public VNVisitor {
    // NODE STATE
    // AstVarScope::user1() -> SchedVarVertex
    // AstSenItem::user1p() -> SchedSenVertex
    // AstVarScope::user2() -> bool: Read of this AstVarScope triggers this logic.
    //                         Used only for hybrid logic.
    const VNUser1InUse m_user1InUse;
    const VNUser2InUse m_user2InUse;

    // STATE
    V3Graph* const m_graphp = new V3Graph;  // The dataflow graph being built
    // The vertices associated with a unique AstSenItem
    std::unordered_map<VNRef<AstSenItem>, SchedSenVertex*> m_senVertices;
    AstScope* m_scopep = nullptr;  // AstScope of the current AstActive
    AstSenTree* m_senTreep = nullptr;  // AstSenTree of the current AstActive
    // Predicate for whether a read of the given variable triggers this block
    std::function<bool(AstVarScope*)> m_readTriggersThisLogic;

    VL_DEBUG_FUNC;

    SchedVarVertex* getVarVertex(AstVarScope* vscp) const {
        if (!vscp->user1p()) vscp->user1p(new SchedVarVertex{m_graphp, vscp});
        return vscp->user1u().to<SchedVarVertex*>();
    }

    SchedSenVertex* getSenVertex(AstSenItem* senItemp) {
        if (!senItemp->user1p()) {
            // There is a unique SchedSenVertex for each globally unique AstSenItem. Multiple
            // AstSenTree might use the same AstSenItem (e.g.: posedge clk1 or rst, posedge clk2 or
            // rst), so we use a hash map to get the unique SchedSenVertex. (Note: This creates
            // separate vertices for ET_CHANGED and ET_HYBRID over the same expression, but that is
            // OK for now).
            auto it = m_senVertices.find(*senItemp);

            // If it does not exist, create it
            if (it == m_senVertices.end()) {
                // Create the vertex
                SchedSenVertex* const vtxp = new SchedSenVertex{m_graphp, senItemp};

                // Connect up the variable references
                senItemp->sensp()->foreach<AstVarRef>([&](AstVarRef* refp) {
                    SchedVarVertex* const varVtxp = getVarVertex(refp->varScopep());
                    new V3GraphEdge{m_graphp, varVtxp, vtxp, 1};
                });

                // Store back to hash map so we can find it next time
                it = m_senVertices.emplace(*senItemp, vtxp).first;
            }

            // Cache sensitivity vertex
            senItemp->user1p(it->second);
        }
        return senItemp->user1u().to<SchedSenVertex*>();
    }

    void visitLogic(AstNode* nodep) {
        UASSERT_OBJ(m_senTreep, nodep, "Should be under AstActive");

        SchedLogicVertex* const logicVtxp
            = new SchedLogicVertex{m_graphp, m_scopep, m_senTreep, nodep};

        // Clocked or hybrid logic has explicit sensitivity, so add edge from sensitivity vertex
        if (!m_senTreep->hasCombo()) {
            m_senTreep->foreach<AstSenItem>([=](AstSenItem* senItemp) {
                if (senItemp->isIllegal()) return;
                UASSERT_OBJ(senItemp->isClocked() || senItemp->isHybrid(), nodep,
                            "Non-clocked SenItem under clocked SenTree");
                V3GraphVertex* const eventVtxp = getSenVertex(senItemp);
                new V3GraphEdge{m_graphp, eventVtxp, logicVtxp, 10};
            });
        }

        // Add edges based on references
        nodep->foreach<AstVarRef>([=](const AstVarRef* vrefp) {
            AstVarScope* const vscp = vrefp->varScopep();
            SchedVarVertex* const varVtxp = getVarVertex(vscp);
            if (vrefp->access().isReadOrRW() && m_readTriggersThisLogic(vscp)) {
                new V3GraphEdge{m_graphp, varVtxp, logicVtxp, 10};
            }
            if (vrefp->access().isWriteOrRW()) {
                new V3GraphEdge{m_graphp, logicVtxp, varVtxp, 10};
            }
        });

        // If the logic calls a DPI import, it might fire the DPI Export trigger
        if (AstVarScope* const dpiExporTrigger = v3Global.rootp()->dpiExportTriggerp()) {
            nodep->foreach<AstCCall>([=](const AstCCall* callp) {
                if (!callp->funcp()->dpiImportWrapper()) return;
                SchedVarVertex* const varVtxp = getVarVertex(dpiExporTrigger);
                new V3GraphEdge{m_graphp, logicVtxp, varVtxp, 10};
            });
        }
    }

    // VISIT methods
    virtual void visit(AstActive* nodep) override {
        AstSenTree* const senTreep = nodep->sensesp();
        UASSERT_OBJ(senTreep->hasClocked() || senTreep->hasCombo() || senTreep->hasHybrid(), nodep,
                    "Unhandled");
        UASSERT_OBJ(!m_senTreep, nodep, "Should not nest");

        // Mark explicit sensitivities as not triggering these blocks
        if (senTreep->hasHybrid()) {
            AstNode::user2ClearTree();
            senTreep->foreach<AstVarRef>([](const AstVarRef* refp) {  //
                refp->varScopep()->user2(true);
            });
        }

        m_senTreep = senTreep;
        iterateChildrenConst(nodep);
        m_senTreep = nullptr;
    }

    virtual void visit(AstNodeProcedure* nodep) override { visitLogic(nodep); }
    virtual void visit(AstNodeAssign* nodep) override { visitLogic(nodep); }
    virtual void visit(AstCoverToggle* nodep) override { visitLogic(nodep); }
    virtual void visit(AstAlwaysPublic* nodep) override { visitLogic(nodep); }

    // Pre and Post logic are handled separately
    virtual void visit(AstAssignPre* nodep) override {}
    virtual void visit(AstAssignPost* nodep) override {}
    virtual void visit(AstAlwaysPost* nodep) override {}

    // Ignore
    virtual void visit(AstInitialStatic* nodep) override {  // LCOV_EXCL_START
        nodep->v3fatalSrc("Should not need ordering");
    }
    virtual void visit(AstInitial* nodep) override {
        nodep->v3fatalSrc("Should not need ordering");
    }
    virtual void visit(AstFinal* nodep) override {
        nodep->v3fatalSrc("Should not need ordering");
    }  // LCOV_EXCL_STOP

    virtual void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

    SchedGraphBuilder(const LogicByScope& clockedLogic, const LogicByScope& combinationalLogic,
                      const LogicByScope& hybridLogic) {
        // Build the data flow graph
        const auto iter = [this](const LogicByScope& lbs) {
            for (const auto& pair : lbs) {
                m_scopep = pair.first;
                iterate(pair.second);
                m_scopep = nullptr;
            }
        };
        // Clocked logic is never triggered by reads
        m_readTriggersThisLogic = [](AstVarScope*) { return false; };
        iter(clockedLogic);
        // Combinational logic is always triggered by reads
        m_readTriggersThisLogic = [](AstVarScope*) { return true; };
        iter(combinationalLogic);
        // Hybrid logic is triggered by all reads, except for reads of the explicit sensitivities
        m_readTriggersThisLogic = [](AstVarScope* vscp) { return !vscp->user2(); };
        iter(hybridLogic);
    }

public:
    // Build the dataflow graph for partitioning
    static std::unique_ptr<V3Graph> build(const LogicByScope& clockedLogic,
                                          const LogicByScope& combinationalLogic,
                                          const LogicByScope& hybridLogic) {
        SchedGraphBuilder visitor{clockedLogic, combinationalLogic, hybridLogic};
        return std::unique_ptr<V3Graph>{visitor.m_graphp};
    }
};

void colorActiveRegion(const V3Graph& graph) {
    // Work queue for depth first traversal
    std::vector<V3GraphVertex*> queue{};

    // Trace from all SchedSenVertex
    for (V3GraphVertex* vtxp = graph.verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
        if (const auto activeEventVtxp = dynamic_cast<SchedSenVertex*>(vtxp)) {
            queue.push_back(activeEventVtxp);
        }
    }

    // Depth first traversal
    while (!queue.empty()) {
        // Pop next work item
        V3GraphVertex& vtx = *queue.back();
        queue.pop_back();
        // If not first encounter, move on
        if (vtx.color() != 0) continue;

        // Mark vertex as being in active region
        vtx.color(1);

        // Enqueue all parent vertices that feed this vertex.
        for (V3GraphEdge* edgep = vtx.inBeginp(); edgep; edgep = edgep->inNextp()) {
            queue.push_back(edgep->fromp());
        }

        // If this is a logic vertex, also enqueue all variable vertices that are driven from this
        // logic. This will ensure that if a variable is set in the active region, then all
        // settings of that variable will be in the active region.
        if (dynamic_cast<SchedLogicVertex*>(&vtx)) {
            for (V3GraphEdge* edgep = vtx.outBeginp(); edgep; edgep = edgep->outNextp()) {
                UASSERT(dynamic_cast<SchedVarVertex*>(edgep->top()), "Should be var vertex");
                queue.push_back(edgep->top());
            }
        }
    }
}

}  // namespace

LogicRegions partition(LogicByScope& clockedLogic, LogicByScope& combinationalLogic,
                       LogicByScope& hybridLogic) {
    UINFO(2, __FUNCTION__ << ": " << endl);

    // Build the graph
    const std::unique_ptr<V3Graph> graphp
        = SchedGraphBuilder::build(clockedLogic, combinationalLogic, hybridLogic);
    if (debug() > 6) graphp->dumpDotFilePrefixed("sched");

    // Partition into Active and NBA regions
    colorActiveRegion(*(graphp.get()));
    if (debug() > 6) graphp->dumpDotFilePrefixed("sched-partitioned", true);

    LogicRegions result;

    for (V3GraphVertex* vtxp = graphp->verticesBeginp(); vtxp; vtxp = vtxp->verticesNextp()) {
        if (const auto lvtxp = dynamic_cast<SchedLogicVertex*>(vtxp)) {
            LogicByScope& lbs = lvtxp->color() ? result.m_act : result.m_nba;
            AstNode* const logicp = lvtxp->logicp();
            logicp->unlinkFrBack();
            lbs.add(lvtxp->scopep(), lvtxp->senTreep(), logicp);
        }
    }

    // Partition the Pre logic
    {
        const VNUser1InUse user1InUse;  // AstVarScope::user1() -> bool: read in Active region
        const VNUser2InUse user2InUse;  // AstVarScope::user2() -> bool: writen in Active region

        const auto markVars = [](AstNode* nodep) {
            nodep->foreach<AstNodeVarRef>([](const AstNodeVarRef* vrefp) {
                AstVarScope* const vscp = vrefp->varScopep();
                if (vrefp->access().isReadOrRW()) vscp->user1(true);
                if (vrefp->access().isWriteOrRW()) vscp->user2(true);
            });
        };

        for (const auto& pair : result.m_act) {
            AstActive* const activep = pair.second;
            markVars(activep->sensesp());
            markVars(activep);
        }

        // AstAssignPre, AstAssignPost and AstAlwaysPost should only appear under a clocked
        // AstActive, and should be the only thing left at this point.
        for (const auto& pair : clockedLogic) {
            AstScope* const scopep = pair.first;
            AstActive* const activep = pair.second;
            for (AstNode *nodep = activep->stmtsp(), *nextp; nodep; nodep = nextp) {
                nextp = nodep->nextp();
                if (AstAssignPre* const logicp = VN_CAST(nodep, AssignPre)) {
                    bool toActiveRegion = false;
                    logicp->foreach<AstNodeVarRef>([&](const AstNodeVarRef* vrefp) {
                        AstVarScope* const vscp = vrefp->varScopep();
                        if (vrefp->access().isReadOnly()) {
                            // Variable only read in Pre, and is written in active region
                            if (vscp->user2()) toActiveRegion = true;
                        } else {
                            // Variable written in Pre, and referenced in active region
                            if (vscp->user1() || vscp->user2()) toActiveRegion = true;
                        }
                    });
                    LogicByScope& lbs = toActiveRegion ? result.m_pre : result.m_nba;
                    logicp->unlinkFrBack();
                    lbs.add(scopep, activep->sensesp(), logicp);
                } else {
                    UASSERT_OBJ(VN_IS(nodep, AssignPost) || VN_IS(nodep, AlwaysPost), nodep,
                                "Unexpected node type " << nodep->typeName());
                    nodep->unlinkFrBack();
                    result.m_nba.add(scopep, activep->sensesp(), nodep);
                }
            }
        }
    }

    // Clean up remains of inputs
    clockedLogic.deleteActives();
    combinationalLogic.deleteActives();
    hybridLogic.deleteActives();

    return result;
}

}  // namespace V3Sched
