// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Scheduling
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

#ifndef VERILATOR_V3SCHED_H_
#define VERILATOR_V3SCHED_H_

#include "config_build.h"
#include "verilatedos.h"
#include "V3Ast.h"

#include <functional>
#include <utility>
#include <vector>

//============================================================================

namespace V3Sched {

//============================================================================
// Throughout scheduling, we need to keep hold of AstActive nodes, together with the AstScope that
// they are under. LogicByScope is simply a vector of such pairs, with some additional convenience
// methods.
struct LogicByScope final : public std::vector<std::pair<AstScope*, AstActive*>> {
    // Add logic
    void add(AstScope* scopep, AstSenTree* senTreep, AstNode* logicp) {
        UASSERT_OBJ(!logicp->backp(), logicp, "Already linked");
        if (empty() || back().first != scopep || back().second->sensesp() != senTreep) {
            emplace_back(scopep, new AstActive{logicp->fileline(), "", senTreep});
        }
        back().second->addStmtsp(logicp);
    };

    // Create copy, with the AstActives cloned
    LogicByScope clone() const {
        LogicByScope result;
        for (const auto& pair : *this) {
            result.emplace_back(pair.first, pair.second->cloneTree(false));
        }
        return result;
    }

    // Delete actives (they should all be empty)
    void deleteActives() {
        for (const auto& pair : *this) {
            AstActive* const activep = pair.second;
            UASSERT_OBJ(!activep->stmtsp(), activep, "Leftover logic");
            if (activep->backp()) activep->unlinkFrBack();
            activep->deleteTree();
        }
        clear();
    };

    void foreachLogic(std::function<void(AstNode*)> f) const {
        for (const auto& pair : *this) {
            for (AstNode* nodep = pair.second->stmtsp(); nodep; nodep = nodep->nextp()) f(nodep);
        }
    }
};

// Logic in the design is classified based on what can trigger its execution.
// For details see the internals documentation.
struct LogicClasses final {
    LogicByScope m_static;  // Static variable initializers
    LogicByScope m_initial;  // initial blocks
    LogicByScope m_final;  // final blocks
    LogicByScope m_comb;  // Combinational logic (logic with implicit sensitivities)
    LogicByScope m_clocked;  // Clocked (or sequential) logic (logic with explictit sensitivities)
    LogicByScope m_hybrid;  // Hybrid logic (combinational logic with some explicit sensitivities)

    LogicClasses() = default;
    VL_UNCOPYABLE(LogicClasses);
    LogicClasses(LogicClasses&&) = default;
    LogicClasses& operator=(LogicClasses&&) = default;
};

// Combinational (including hybrid) logic, and clocked logic in partitioned to compute all clock
// signals in the 'act' region. For details see the internals documentation.
struct LogicRegions final {
    LogicByScope m_pre;  // AstAssignPre logic in 'act' region
    LogicByScope m_act;  // 'act' region logic
    LogicByScope m_nba;  // 'nba' region logic

    LogicRegions() = default;
    VL_UNCOPYABLE(LogicRegions);
    LogicRegions(LogicRegions&&) = default;
    LogicRegions& operator=(LogicRegions&&) = default;
};

// Combinational (including hybrid) logic is replicated into the various scheduling regions.
// For details see the internals documentation.
struct LogicReplicas final {
    LogicByScope m_ico;  // Logic replicated into the 'ico' (Input Combinational) region
    LogicByScope m_act;  // Logic replicated into the 'act' region
    LogicByScope m_nba;  // Logic replicated into the 'nba' region

    LogicReplicas() = default;
    VL_UNCOPYABLE(LogicReplicas);
    LogicReplicas(LogicReplicas&&) = default;
    LogicReplicas& operator=(LogicReplicas&&) = default;
};

// Top level entry point to scheduling
void schedule(AstNetlist*);

// Sub-steps
LogicByScope breakCycles(AstNetlist* netlistp, LogicByScope& combinationalLogic);
LogicRegions partition(LogicByScope& clockedLogic, LogicByScope& combinationalLogic,
                       LogicByScope& hybridLogic);
LogicReplicas replicateLogic(LogicRegions&);

}  // namespace V3Sched

#endif  // Guard
