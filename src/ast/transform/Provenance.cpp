/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2017, The Souffle Developers. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Provenance.cpp
 *
 * Implements AstTransformer for adding provenance information via extra columns
 *
 ***********************************************************************/

#include "ast/transform/Provenance.h"
#include "RelationTag.h"
#include "ast/Aggregator.h"
#include "ast/Argument.h"
#include "ast/Atom.h"
#include "ast/Attribute.h"
#include "ast/BinaryConstraint.h"
#include "ast/Clause.h"
#include "ast/Constant.h"
#include "ast/Functor.h"
#include "ast/IntrinsicFunctor.h"
#include "ast/Literal.h"
#include "ast/Negation.h"
#include "ast/Node.h"
#include "ast/NumericConstant.h"
#include "ast/Program.h"
#include "ast/ProvenanceNegation.h"
#include "ast/QualifiedName.h"
#include "ast/Relation.h"
#include "ast/StringConstant.h"
#include "ast/TranslationUnit.h"
#include "ast/UnnamedVariable.h"
#include "ast/Variable.h"
#include "ast/utility/NodeMapper.h"
#include "ast/utility/Utils.h"
#include "souffle/BinaryConstraintOps.h"
#include "souffle/utility/ContainerUtil.h"
#include "souffle/utility/MiscUtil.h"
#include "souffle/utility/StreamUtil.h"
#include "souffle/utility/StringUtil.h"
#include "souffle/utility/tinyformat.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace souffle {

/**
 * Helper functions
 */
inline AstQualifiedName makeRelationName(
        const AstQualifiedName& orig, const std::string& type, int num = -1) {
    AstQualifiedName newName(toString(orig));
    newName.append(type);
    if (num != -1) {
        newName.append((const std::string&)std::to_string(num));
    }

    return newName;
}

std::unique_ptr<AstRelation> makeInfoRelation(
        AstClause& originalClause, size_t originalClauseNum, AstTranslationUnit& translationUnit) {
    AstQualifiedName name =
            makeRelationName(originalClause.getHead()->getQualifiedName(), "@info", originalClauseNum);

    // initialise info relation
    auto infoRelation = new AstRelation();
    infoRelation->setQualifiedName(name);
    // set qualifier to INFO_RELATION
    infoRelation->setRepresentation(RelationRepresentation::INFO);

    // create new clause containing a single fact
    auto infoClause = new AstClause();
    auto infoClauseHead = new AstAtom();
    infoClauseHead->setQualifiedName(name);

    // (darth_tytus): Can this be unsigned?
    infoRelation->addAttribute(mk<AstAttribute>("clause_num", AstQualifiedName("number")));
    infoClauseHead->addArgument(mk<AstNumericConstant>(originalClauseNum));

    // add head relation as meta info
    std::vector<std::string> headVariables;

    // a method to stringify an AstArgument, translating functors and aggregates
    // keep a global counter of functor and aggregate numbers, which increment for each unique
    // functor/aggregate
    int functorNumber = 0;
    int aggregateNumber = 0;
    auto getArgInfo = [&](AstArgument* arg) -> std::string {
        if (auto* var = dynamic_cast<AstVariable*>(arg)) {
            return toString(*var);
        } else if (auto* constant = dynamic_cast<AstConstant*>(arg)) {
            return toString(*constant);
        }
        if (isA<AstUnnamedVariable>(arg)) {
            return "_";
        }
        if (isA<AstFunctor>(arg)) {
            return tfm::format("functor_%d", functorNumber++);
        }
        if (isA<AstAggregator>(arg)) {
            return tfm::format("agg_%d", aggregateNumber++);
        }

        fatal("Unhandled argument type");
    };

    // add head arguments
    for (auto& arg : originalClause.getHead()->getArguments()) {
        headVariables.push_back(getArgInfo(arg));
    }

    // join variables in the head with commas
    std::stringstream headVariableString;
    headVariableString << join(headVariables, ",");

    // add an attribute to infoRelation for the head of clause
    infoRelation->addAttribute(
            mk<AstAttribute>(std::string("head_vars"), AstQualifiedName("symbol")));
    infoClauseHead->addArgument(mk<AstStringConstant>(toString(join(headVariables, ","))));

    // visit all body literals and add to info clause head
    for (size_t i = 0; i < originalClause.getBodyLiterals().size(); i++) {
        auto lit = originalClause.getBodyLiterals()[i];

        const AstAtom* atom = nullptr;
        if (isA<AstAtom>(lit)) {
            atom = static_cast<AstAtom*>(lit);
        } else if (isA<AstNegation>(lit)) {
            atom = static_cast<AstNegation*>(lit)->getAtom();
        } else if (isA<AstProvenanceNegation>(lit)) {
            atom = static_cast<AstProvenanceNegation*>(lit)->getAtom();
        }

        // add an attribute for atoms and binary constraints
        if (atom != nullptr || isA<AstBinaryConstraint>(lit)) {
            infoRelation->addAttribute(mk<AstAttribute>(
                    std::string("rel_") + std::to_string(i), AstQualifiedName("symbol")));
        }

        if (atom != nullptr) {
            std::string relName = toString(atom->getQualifiedName());

            // for an atom, add its name and variables (converting aggregates to variables)
            if (isA<AstAtom>(lit)) {
                std::string atomDescription = relName;

                for (auto& arg : atom->getArguments()) {
                    atomDescription.append("," + getArgInfo(arg));
                }

                infoClauseHead->addArgument(mk<AstStringConstant>(atomDescription));
                // for a negation, add a marker with the relation name
            } else if (isA<AstNegation>(lit)) {
                infoClauseHead->addArgument(mk<AstStringConstant>("!" + relName));
            }
        }
    }

    // visit all body constraints and add to info clause head
    for (size_t i = 0; i < originalClause.getBodyLiterals().size(); i++) {
        auto lit = originalClause.getBodyLiterals()[i];

        if (auto con = dynamic_cast<AstBinaryConstraint*>(lit)) {
            // for a constraint, add the constraint symbol and LHS and RHS
            std::string constraintDescription = toBinaryConstraintSymbol(con->getOperator());

            constraintDescription.append("," + getArgInfo(con->getLHS()));
            constraintDescription.append("," + getArgInfo(con->getRHS()));

            infoClauseHead->addArgument(mk<AstStringConstant>(constraintDescription));
        }
    }

    infoRelation->addAttribute(mk<AstAttribute>("clause_repr", AstQualifiedName("symbol")));
    infoClauseHead->addArgument(mk<AstStringConstant>(toString(originalClause)));

    // set clause head and add clause to info relation
    infoClause->setHead(std::unique_ptr<AstAtom>(infoClauseHead));
    translationUnit.getProgram()->addClause(std::unique_ptr<AstClause>(infoClause));

    return std::unique_ptr<AstRelation>(infoRelation);
}

/** Transform eqrel relations to explicitly define equivalence relations */
void transformEqrelRelation(AstProgram& program, AstRelation& rel) {
    assert(rel.getRepresentation() == RelationRepresentation::EQREL &&
            "attempting to transform non-eqrel relation");
    assert(rel.getArity() == 2 && "eqrel relation not binary");

    rel.setRepresentation(RelationRepresentation::BTREE);

    // transitivity
    // transitive clause: A(x, z) :- A(x, y), A(y, z).
    auto transitiveClause = new AstClause();
    auto transitiveClauseHead = new AstAtom(rel.getQualifiedName());
    transitiveClauseHead->addArgument(mk<AstVariable>("x"));
    transitiveClauseHead->addArgument(mk<AstVariable>("z"));

    auto transitiveClauseBody = new AstAtom(rel.getQualifiedName());
    transitiveClauseBody->addArgument(mk<AstVariable>("x"));
    transitiveClauseBody->addArgument(mk<AstVariable>("y"));

    auto transitiveClauseBody2 = new AstAtom(rel.getQualifiedName());
    transitiveClauseBody2->addArgument(mk<AstVariable>("y"));
    transitiveClauseBody2->addArgument(mk<AstVariable>("z"));

    transitiveClause->setHead(std::unique_ptr<AstAtom>(transitiveClauseHead));
    transitiveClause->addToBody(std::unique_ptr<AstLiteral>(transitiveClauseBody));
    transitiveClause->addToBody(std::unique_ptr<AstLiteral>(transitiveClauseBody2));
    program.addClause(std::unique_ptr<AstClause>(transitiveClause));

    // symmetric
    // symmetric clause: A(x, y) :- A(y, x).
    auto symClause = new AstClause();
    auto symClauseHead = new AstAtom(rel.getQualifiedName());
    symClauseHead->addArgument(mk<AstVariable>("x"));
    symClauseHead->addArgument(mk<AstVariable>("y"));

    auto symClauseBody = new AstAtom(rel.getQualifiedName());
    symClauseBody->addArgument(mk<AstVariable>("y"));
    symClauseBody->addArgument(mk<AstVariable>("x"));

    symClause->setHead(std::unique_ptr<AstAtom>(symClauseHead));
    symClause->addToBody(std::unique_ptr<AstLiteral>(symClauseBody));
    program.addClause(std::unique_ptr<AstClause>(symClause));

    // reflexivity
    // reflexive clause: A(x, x) :- A(x, _).
    auto reflexiveClause = new AstClause();
    auto reflexiveClauseHead = new AstAtom(rel.getQualifiedName());
    reflexiveClauseHead->addArgument(mk<AstVariable>("x"));
    reflexiveClauseHead->addArgument(mk<AstVariable>("x"));

    auto reflexiveClauseBody = new AstAtom(rel.getQualifiedName());
    reflexiveClauseBody->addArgument(mk<AstVariable>("x"));
    reflexiveClauseBody->addArgument(mk<AstUnnamedVariable>());

    reflexiveClause->setHead(std::unique_ptr<AstAtom>(reflexiveClauseHead));
    reflexiveClause->addToBody(std::unique_ptr<AstLiteral>(reflexiveClauseBody));
    program.addClause(std::unique_ptr<AstClause>(reflexiveClause));
}

namespace {
std::unique_ptr<AstArgument> getNextLevelNumber(const std::vector<AstArgument*>& levels) {
    if (levels.empty()) return mk<AstNumericConstant>(0);

    auto max = levels.size() == 1
                       ? std::unique_ptr<AstArgument>(levels[0])
                       : mk<AstIntrinsicFunctor>("max",
                                 map(levels, [](auto&& x) { return std::unique_ptr<AstArgument>(x); }));

    return mk<AstIntrinsicFunctor>("+", std::move(max), mk<AstNumericConstant>(1));
}
}  // namespace

bool ProvenanceTransformer::transformMaxHeight(AstTranslationUnit& translationUnit) {
    auto program = translationUnit.getProgram();

    for (auto relation : program->getRelations()) {
        if (relation->getRepresentation() == RelationRepresentation::EQREL) {
            // Explicitly expand eqrel relation
            transformEqrelRelation(*program, *relation);
        }
    }

    for (auto relation : program->getRelations()) {
        // generate info relations for each clause
        // do this before all other transformations so that we record
        // the original rule without any instrumentation
        for (auto clause : getClauses(*program, *relation)) {
            if (!isFact(*clause)) {
                // add info relation
                program->addRelation(
                        makeInfoRelation(*clause, getClauseNum(program, clause), translationUnit));
            }
        }

        relation->addAttribute(
                mk<AstAttribute>(std::string("@rule_number"), AstQualifiedName("number")));
        relation->addAttribute(
                mk<AstAttribute>(std::string("@level_number"), AstQualifiedName("number")));

        for (auto clause : getClauses(*program, *relation)) {
            size_t clauseNum = getClauseNum(program, clause);

            // mapper to add two provenance columns to atoms
            struct M : public AstNodeMapper {
                using AstNodeMapper::operator();

                std::unique_ptr<AstNode> operator()(std::unique_ptr<AstNode> node) const override {
                    // add provenance columns
                    if (auto atom = dynamic_cast<AstAtom*>(node.get())) {
                        atom->addArgument(mk<AstUnnamedVariable>());
                        atom->addArgument(mk<AstUnnamedVariable>());
                    } else if (auto neg = dynamic_cast<AstNegation*>(node.get())) {
                        auto atom = neg->getAtom();
                        atom->addArgument(mk<AstUnnamedVariable>());
                        atom->addArgument(mk<AstUnnamedVariable>());
                    }

                    // otherwise - apply mapper recursively
                    node->apply(*this);
                    return node;
                }
            };

            // add unnamed vars to each atom nested in arguments of head
            clause->getHead()->apply(M());

            // if fact, level number is 0
            if (isFact(*clause)) {
                clause->getHead()->addArgument(mk<AstNumericConstant>(0));
                clause->getHead()->addArgument(mk<AstNumericConstant>(0));
            } else {
                std::vector<AstArgument*> bodyLevels;

                for (size_t i = 0; i < clause->getBodyLiterals().size(); i++) {
                    auto lit = clause->getBodyLiterals()[i];

                    // add unnamed vars to each atom nested in arguments of lit
                    lit->apply(M());

                    // add two provenance columns to lit; first is rule num, second is level num
                    if (auto atom = dynamic_cast<AstAtom*>(lit)) {
                        atom->addArgument(mk<AstUnnamedVariable>());
                        atom->addArgument(mk<AstVariable>("@level_num_" + std::to_string(i)));
                        bodyLevels.push_back(new AstVariable("@level_num_" + std::to_string(i)));
                    }
                }

                // add two provenance columns to head lit
                clause->getHead()->addArgument(mk<AstNumericConstant>(clauseNum));
                clause->getHead()->addArgument(getNextLevelNumber(bodyLevels));
            }
        }
    }
    return true;
}

bool ProvenanceTransformer::transform(AstTranslationUnit& translationUnit) {
    return ProvenanceTransformer::transformMaxHeight(translationUnit);
}

}  // end of namespace souffle
