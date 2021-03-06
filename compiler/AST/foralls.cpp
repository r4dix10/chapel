/*
 * Copyright 2004-2019 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "astutil.h"
#include "AstVisitor.h"
#include "DeferStmt.h"
#include "driver.h"
#include "ForLoop.h"
#include "ForallStmt.h"
#include "iterator.h"
#include "optimizations.h"
#include "passes.h"
#include "resolution.h"
#include "resolveFunction.h"
#include "stlUtil.h"
#include "stringutil.h"

const char* forallIntentTagDescription(ForallIntentTag tfiTag) {
  switch (tfiTag) {
    case TFI_DEFAULT:       return "default";
    case TFI_CONST:         return "const";
    case TFI_IN_PARENT:     return "parent-in";
    case TFI_IN:            return "in";
    case TFI_CONST_IN:      return "const in";
    case TFI_REF:           return "ref";
    case TFI_CONST_REF:     return "const ref";
    case TFI_REDUCE:        return "reduce";
    case TFI_REDUCE_OP:        return "reduce-Op";
    case TFI_REDUCE_PARENT_AS: return "parent-reduce-AS";
    case TFI_REDUCE_PARENT_OP: return "parent-reduce-Op";
    case TFI_TASK_PRIVATE:  return "task-private";
  }
  INT_ASSERT(false);
  return "";
}


/////////////////////
//                 //
// parser support  //
//                 //
/////////////////////

// These functions report a user error for an unexpected intent.

static ShadowVarSymbol* buildShadowVariable(ShadowVarPrefix prefix,
                                            const char* name, Expr* ovar)
{
  ForallIntentTag intent = TFI_DEFAULT; // dummy
  switch (prefix)
  {
    case SVP_CONST:     intent = TFI_CONST;     break;
    case SVP_IN:        intent = TFI_IN;        break;
    case SVP_CONST_IN:  intent = TFI_CONST_IN;  break;
    case SVP_REF:       intent = TFI_REF;       break;
    case SVP_CONST_REF: intent = TFI_CONST_REF; break;
    case SVP_VAR:
      // This keyword is for a TPV.
      // Whereas the user provided neither a type nor an init.
      USR_FATAL_CONT(ovar, "a task private variable '%s'"
                     "requires a type and/or initializing expression", name);
      break;
  }

  ShadowVarSymbol* result = new ShadowVarSymbol(intent, name, NULL);
  new DefExpr(result); // set result->defPoint
  return result;
}

static ShadowVarSymbol* buildTaskPrivateVariable(ShadowVarPrefix prefix,
            const char* nameString, Expr* nameExpr, Expr* type, Expr* init)
{

  // TPV - task-private variable, as we have a type and/or an initializer.
  ShadowVarSymbol* result = new ShadowVarSymbol(TFI_TASK_PRIVATE,
                                                nameString, NULL);

  switch (prefix)
  {
    case SVP_VAR:        result->qual = QUAL_VAL;       break;

    case SVP_CONST:      result->qual = QUAL_CONST_VAL;
                         result->addFlag(FLAG_CONST);   break;

    case SVP_REF:        result->qual = QUAL_REF;
                         result->addFlag(FLAG_REF_VAR); break;

    case SVP_CONST_REF:  result->qual = QUAL_CONST_REF;
                         result->addFlag(FLAG_CONST);
                         result->addFlag(FLAG_REF_VAR); break;

    case SVP_IN:
    case SVP_CONST_IN:                                  break; // error below
  }

  // Check for type or init in a wrong place.
  switch (prefix)
  {
    // One or both are fine.
    case SVP_CONST:
    case SVP_VAR:
      break;

    // Ref tpvs must have init and not type.
    case SVP_CONST_REF:
    case SVP_REF:
      if (init == NULL)
        USR_FATAL_CONT(nameExpr, "a 'ref' or 'const ref' task-private variable"
                     " '%s' must have an initializing expression", nameString);
      if (type != NULL)
        USR_FATAL_CONT(nameExpr, "a 'ref' or 'const ref' task-private variable"
                       " '%s' cannot have a type", nameString);
      break;

    // This keyword combination is not for a TPV.
    case SVP_IN:
    case SVP_CONST_IN:
      USR_FATAL_CONT(nameExpr, "an 'in' or 'const in' intent for '%s' "
                     "does not allow a type or an initializing expression",
                     nameString);
      USR_PRINT(nameExpr, "if you mean to declare a task-private variable,"
                " use 'var' or 'const'");
      break;
  }

  // We will call autoDestroy from deinitBlock() explicitly.
  result->addFlag(FLAG_NO_AUTO_DESTROY);

  new DefExpr(result, init, type); // set result->defPoint

  return result;
}

//
// The returned ShadowVarSymbol comes with a DefExpr in its defPoint.
//

ShadowVarSymbol* ShadowVarSymbol::buildForPrefix(ShadowVarPrefix prefix,
                                    Expr* nameExp, Expr* type, Expr* init)
{
  const char* nameString = toUnresolvedSymExpr(nameExp)->unresolved;

  if (type == NULL && init == NULL)
    // non-TPV forall intent
    return buildShadowVariable(prefix, nameString, nameExp);
  else
    return buildTaskPrivateVariable(prefix, nameString, nameExp, type, init);
}

ShadowVarSymbol* ShadowVarSymbol::buildFromReduceIntent(Expr* ovar,
                                                        Expr* riExpr)
{
  INT_ASSERT(riExpr != NULL);
  const char* name = toUnresolvedSymExpr(ovar)->unresolved;
  ShadowVarSymbol* result = new ShadowVarSymbol(TFI_REDUCE, name, NULL, riExpr);
  new DefExpr(result); // set result->defPoint
  return result;
}

void addForallIntent(CallExpr* call, ShadowVarSymbol* svar) {
  call->insertAtTail(svar->defPoint);
}


/////////////////////////////////////////////////////
//                                                 //
// ForallStmt pre-lowering: resolveForallHeader()  //
//                                                 //
/////////////////////////////////////////////////////

// resolveForallHeader() resolves key parts of ForallStmt:
//
//  * find the target parallel iterator (standalone or leader) and resolve it
//  * issue an error, if neither is found
//  * handle forall intents, using setupAndResolveShadowVars()
//  * partly lower by building leader+follow loop(s) as needed
//
// This happens when resolveExpr() encounters the first iterated expression
// of the ForallStmt, because:
//
//  * The iterated expressions need special treatment - adding these(),
//    adding tag=iterKind.standalone/leader/follower, etc.
//    Plain resolving is not appropriate here.
//
//  * Other things in a ForallStmt need to be set up ahead of being resolved.
//    This is a convenient time to do so.
//
// Some transformations include:
//
//  * build leader+follow loop(s) as needed
//  * iteratedExpressions() alist is left with only 1 element,
//    which is the call to the parallel iterator (standalone or leader)
//  * follower iterator(s), if needed, are invoked from within the leader loop
//  * all inductionVariables()' DefExprs are moved to the original loop body

enum ParIterFlavor {
  PIF_NONE,
  PIF_SERIAL,     // can mean "using directly the indicated iterator"
  PIF_STANDALONE,
  PIF_LEADER
};

/////////// helpers ///////////

// Given an iterator or forwarder function, find the type that it yields.
static QualifiedType fsIterYieldType(ForallStmt* fs, FnSymbol* iterFn) {
  INT_ASSERT(iterFn->isResolved());

  if (iterFn->isIterator()) {
    if (IteratorInfo* ii = iterFn->iteratorInfo) {
      return ii->getValue->getReturnQualType();
    } else {
      // We are in the midst of resolving a recursive iterator.
      USR_FATAL_CONT(fs, "the recursion pattern seen in the first iterable"
                         " in this forall loop is not supported");
      USR_PRINT(iterFn, "the corresponding iterator is here");
      USR_PRINT(iterFn, "try declaring its return type");
      USR_STOP();
      QualifiedType dummy(dtUnknown);
      return dummy;
    }

  } else {
    // An iterator forwarder, ex. "proc these() return _value.these();"
    AggregateType* retType = toAggregateType(iterFn->retType);
    INT_ASSERT(retType && retType->symbol->hasFlag(FLAG_ITERATOR_RECORD));
    FnSymbol* iterator = retType->iteratorInfo->iterator;
    INT_ASSERT(iterator->isIterator()); // 'iterator' is from an IteratorInfo
    return fsIterYieldType(fs, iterator);
  }
}

static bool isIteratorRecord(Symbol* sym) {
  return sym->type->symbol->hasFlag(FLAG_ITERATOR_RECORD);
}

static bool acceptUnmodifiedIterCall(ForallStmt* pfs, CallExpr* iterCall)
{
  return pfs->createdFromForLoop() ||
         pfs->requireSerialIterator();
}


// Like in build.cpp, here for ForallStmt.
static BlockStmt*
buildFollowLoop(VarSymbol* iter,
                VarSymbol* leadIdxCopy,
                VarSymbol* followIter,
                VarSymbol* followIdx,
                BlockStmt* loopBody,
                Expr*      ref,
                bool       fast,
                bool       zippered) {
  BlockStmt* followBlock = new BlockStmt();
  ForLoop*   followBody  = new ForLoop(followIdx, followIter, loopBody, zippered, /*forall*/ false);

  // not needed:
  //destructureIndices(followBody, indices, new SymExpr(followIdx), false);

  followBlock->insertAtTail(new DefExpr(followIter));

  followIdx->addFlag(FLAG_FOLLOWER_INDEX);

  if (fast) {

    if (zippered) {
      followBlock->insertAtTail("'move'(%S, _getIteratorZip(_toFastFollowerZip(%S, %S)))", followIter, iter, leadIdxCopy);
    } else {
      followBlock->insertAtTail("'move'(%S, _getIterator(_toFastFollower(%S, %S)))",       followIter, iter, leadIdxCopy);
    }
  } else {

    if (zippered) {
      followBlock->insertAtTail("'move'(%S, _getIteratorZip(_toFollowerZip(%S, %S)))",     followIter, iter, leadIdxCopy);
    } else {
      followBlock->insertAtTail("'move'(%S, _getIterator(_toFollower(%S, %S)))",           followIter, iter, leadIdxCopy);
    }
  }
  followBlock->insertAtTail(new DeferStmt(new CallExpr("_freeIterator", followIter)));

  ref->insertAfter(followBlock); // otherwise it wouldn't normalize
  normalize(followBlock);
  followBlock->remove();

  // followIdx has a defPoint in the non-fast case
  // and no defPoint in the fast case i.e. for fastFollowIdx.
  if (followIdx->defPoint == NULL) {
    followBlock->insertAtTail(new DefExpr(followIdx));
  } else {
    followBlock->insertAtTail(followIdx->defPoint);
  }

  followBlock->insertAtTail("{TYPE 'move'(%S, iteratorIndex(%S)) }", followIdx, followIter);

  followBlock->insertAtTail(followBody);

  return followBlock;
}

// Returns true for: .=( se, "_shape_", whatever)
static bool isSettingShape(SymExpr* se) {
  if (CallExpr* parent = toCallExpr(se->parentExpr))
    if (parent->isPrimitive(PRIM_SET_MEMBER))
      if (SymExpr* field = toSymExpr(parent->get(2)))
        if (!strcmp(field->symbol()->name, "_shape_"))
          return true;
  return false;
}

// Returns true for: iteratorIndexType(se)
static bool isIITcall(SymExpr* se) {
  if (CallExpr* parent = toCallExpr(se->parentExpr))
    if (parent->isNamed("iteratorIndexType")    ||
        parent->isNamed("iteratorIndexTypeZip") )
      return true;
  return false;
}

// The respective temp may not be needed any longer. Remove it.
static void removeOrigIterCall(SymExpr* origSE)
{
  INT_ASSERT(!origSE->inTree());

  Symbol* origSym = origSE->symbol();
  INT_ASSERT(origSym->hasFlag(FLAG_TEMP));

  // If the temp is used only to set its shape, remove it. BTW there may be
  // up to 3 shape-settings, due to a ref/value/constRef ContextCall.
  //
  // Or, the temp can be passed to iteratorIndexType/Zip() to determine
  // the input type for a reduce expr. If so, keep it.
  // Ex. associative/ferguson/plus-reduce-assoc.chpl
  //     associative/bharshbarg/domains/reduceArrOfAssocDom.chpl
  //
  // If there is another scenario, the compiler will hit INT_ASSERT() below.
  // This will alert us to look at it to make sure it is legit.

  SymExpr* defSE = origSym->getSingleDef();
  bool otherUses = false;

  for_SymbolSymExprs(se1, origSym)
    if (se1 != defSE && ! isSettingShape(se1))
      {
        INT_ASSERT(isIITcall(se1));
        otherUses = true;
      }

  if (otherUses) {
    return;  // Keep the temp.
  }

  // The temp is not needed, indeed. Remove it.

  INT_ASSERT(toCallExpr(defSE->parentExpr)->isPrimitive(PRIM_MOVE));
  defSE->parentExpr->remove();

  for_SymbolSymExprs(se2, origSym) {
    INT_ASSERT(isSettingShape(se2));
    se2->parentExpr->remove();
  }

  origSym->defPoint->remove();
}

// Replaces 'origSE' in the tree with the result.
static CallExpr* buildForallParIterCall(ForallStmt* pfs, SymExpr* origSE,
                                        FnSymbol*& origTargetRef)
{
  CallExpr* iterCall = NULL;

  if (isIteratorRecord(origSE->symbol())) {
    // Our iterable expression is an iterator call.

    if (ArgSymbol* origArg = toArgSymbol(origSE->symbol())) {
      FnSymbol* iterator = getTheIteratorFn(origArg->type);
      USR_FATAL_CONT(origSE, "a forall loop over a formal argument corresponding to a for/forall/promoted expression or an iterator call is not implemented");
      USR_PRINT(iterator, "the actual argument is here");
      USR_STOP();
    }

    CallExpr* origIterCall = toCallExpr(getDefOfTemp(origSE));
    // What to do if we do not find it?
    // For example, if the forall is over a formal that is an IR.
    INT_ASSERT(origIterCall);

    FnSymbol* origTarget = origIterCall->resolvedFunction();
    INT_ASSERT(origTarget);

    const char* targetName = origTarget->name;
    const int forallExprNameLen = strlen(astr_forallexpr);
    if (!strncmp(targetName, astr_forallexpr, forallExprNameLen)) {
      // a forall loop over a (possibly zippered) forall expression, ex.:
      //  test/reductions/deitz/test_maxloc_reduce_wmikanik_bug2.chpl
      targetName = astr(astr_loopexpr_iter, targetName + forallExprNameLen);

      // Alternatively, find the function that origTarget redirects to.
      origTarget = getTheIteratorFn(origTarget->retType);
      INT_ASSERT(origTarget->name == targetName);
    }

    if (acceptUnmodifiedIterCall(pfs, origIterCall)) {
      iterCall = origIterCall;
      iterCall->remove();
    } else {
      iterCall = origIterCall->copy();
      iterCall->baseExpr = new UnresolvedSymExpr(targetName);
      origTargetRef = origTarget;
    }

  } else {
    // Not an iterator call, so add a call to these().
    iterCall = new CallExpr("these", gMethodToken, origSE->copy());
  }

  origSE->replace(iterCall);

  return iterCall;
}

static void checkForExplicitTagArgs(CallExpr* iterCall) {
  int cnt = 0;
  for_actuals(actual, iterCall) {
    ++cnt;
    if ((actual->getValType() == gStandaloneTag->type) ||
        (isNamedExpr(actual) && toNamedExpr(actual)->name == astrTag)
    ) {
      USR_FATAL_CONT(iterCall, "user invocation of a parallel iterator should not supply tag arguments -- they are added implicitly by the compiler");
      USR_PRINT(iterCall, "actual argument %d of the iterator call", cnt);
      USR_STOP();
    }
  }
}

static ParIterFlavor findParIter(ForallStmt* pfs, CallExpr* iterCall,
                                 SymExpr* origSE, FnSymbol* origTarget)
{
  ParIterFlavor retval = PIF_NONE;

  checkForExplicitTagArgs(iterCall);

  // We are starting with a serial-iterator call.
  // Transform it to a standalone/leader call.
  NamedExpr* tag = new NamedExpr("tag", new SymExpr(gStandaloneTag));
  iterCall->insertAtTail(tag);

  // try standalone
  if (!pfs->zippered()) {
    bool gotSA = tryResolveCall(iterCall);
    if (gotSA) retval = PIF_STANDALONE;
  }

  // try leader
  if (retval == PIF_NONE) {
    tag->actual->replace(new SymExpr(gLeaderTag));
    bool gotLeader = tryResolveCall(iterCall);
    if (gotLeader) retval = PIF_LEADER;
  }

  // try serial
  if (retval == PIF_NONE && pfs->allowSerialIterator()) {
    tag->remove();
    if (origTarget != NULL) {
      retval = PIF_SERIAL;
      iterCall->baseExpr->replace(new SymExpr(origTarget));
    } else {
      // Iterating over a variable that does not have parallel .these() iters.
      INT_ASSERT(! isIteratorRecord(origSE->symbol()));
      bool gotSerial = tryResolveCall(iterCall);
      if (gotSerial) retval = PIF_SERIAL;
    }
  }

  if (retval == PIF_NONE) {
    // Cannot USR_FATAL_CONT in general: e.g. if these() is not found,
    // we do not know the type of the index variable.
    // Without which we cannot typecheck the loop body.
    if (iterCall->isNamed("these") && isTypeExpr(iterCall->get(2)))
      USR_FATAL(iterCall, "unable to iterate over type '%s'", toString(iterCall->get(2)->getValType()));
    else
      USR_FATAL(iterCall, "A%s leader iterator is not found for the iterable expression in this forall loop", pfs->zippered() ? "" : " standalone or");
  }

  return retval;
}

/////////// handleZipperedSerial /////////// 

static FnSymbol* trivialLeader          = NULL;
static Type*     trivialLeaderYieldType = NULL;

static void hzsCheckParallelIterator(ForallStmt* fs, FnSymbol* origIterFn) {
  if (isLeaderIterator(origIterFn) || isStandaloneIterator(origIterFn)) {
    USR_FATAL(fs->iteratedExpressions().head, "Support for this combination of zippered iterators is not currently implemented");
  }
}

// Return a _build_tuple of fs's index variables.
static CallExpr* hzsMakeIndices(ForallStmt* fs) {
  CallExpr* indices = new CallExpr("_build_tuple");

  for_alist(inddef, fs->inductionVariables())
    indices->insertAtTail(toDefExpr(inddef)->sym);

  // Todo detect the case where the forall loop in the source code
  // had a single index variable. We can tell that by checking whether
  // all 'inddef' vars are fed into a _build_tuple_always_allow_ref call.
  // If so, simplify the AST by having 'indices' be a single SymExpr
  // that is PRIM_MOVE'ed to from that call.

  return indices;
}

// Return a PRIM_ZIP of fs's iterables.
static CallExpr* hzsMakeIterators(ForallStmt* fs, FnSymbol* origIterFn,
                                  SymExpr* origSE)
{
  // Looks like the first of the iterables is a copy of the def of origSE.
  // So use origSE instead.
  CallExpr* iter1 = toCallExpr(fs->firstIteratedExpr());
  INT_ASSERT(iter1->resolvedFunction() == origIterFn);
  iter1->replace(origSE); // relies on origSE not inTree()

  // Move all iterables to the zip call.
  CallExpr* iterators = new CallExpr(PRIM_ZIP);
  for_alist(iter, fs->iteratedExpressions()) {
    iterators->insertAtTail(iter->remove());
  }

  return iterators;
}

// Wrap fs's loopBody in a zippered ForLoop over fs's iterables.
static void hzsBuildZipperedForLoop(ForallStmt* fs, FnSymbol* origIterFn,
                                    SymExpr* origSE)
{
  CallExpr* indices   = hzsMakeIndices(fs);
  CallExpr* iterators = hzsMakeIterators(fs, origIterFn, origSE);

  BlockStmt* origLoopBody = fs->loopBody();
  BlockStmt* newLoopBody  = new BlockStmt();
  origLoopBody->replace(newLoopBody);

  BlockStmt* forBlock = ForLoop::buildForLoop(indices, iterators,
                                              origLoopBody, false, true);
  newLoopBody->insertAtTail(forBlock);

  ForLoop* forLoop = toForLoop(origLoopBody->parentExpr);

  SymExpr* loopIterDef = forLoop->iteratorGet()->symbol()->getSingleDef();
  normalize(loopIterDef->parentExpr); // because of buildForLoop()

  // Move the index variables' DefExprs to 'forLoop'.
  while (Expr* inddef = fs->inductionVariables().tail)
    forLoop->insertAtHead(inddef->remove());

  origLoopBody->flattenAndRemove();
  forBlock->flattenAndRemove();
}

// Use 'trivialLeader' as fs's parallel iterator.
static CallExpr* hzsCallTrivialParIter(ForallStmt* fs) {
  CallExpr* result = NULL;

  if (trivialLeader == NULL) {
    result = new CallExpr("chpl_trivialLeader");
    rootModule->block->insertAtTail(result);
    resolveCallAndCallee(result, false);
    result->remove();

    trivialLeader = result->resolvedFunction();
    trivialLeaderYieldType = trivialLeader->iteratorInfo->getValue->retType;

  } else {
    result = new CallExpr(trivialLeader);
  }

  VarSymbol* trivialIdx = newTemp("chpl_trivialIdx", trivialLeaderYieldType);
  trivialIdx->addFlag(FLAG_INDEX_VAR);

  fs->inductionVariables().insertAtTail(new DefExpr(trivialIdx));
  fs->iteratedExpressions().insertAtTail(result);

  return result;
}

/*
Background:

ForallStmt lowering requires a single iterator to inline.
For a non-zippered loop, it can be standalone or serial.
For a zippered loop, it has to be a leader, with the followers
being iterated over with a zippered regular ForLoop
that we put in ForallStmt->loopBody().

The case at hand:

For a zippered loop over **serial** iterators, we do not have
such a leader, as serial iterators cannot be "followed".
So we give the ForallStmt a trivial leader, then have loopBody()
be a regular ForLoop, zippered over these serial iterators.

Retaining the ForallStmt itself means that forall intents, if any,
will be hanled by existing code.
*/
static CallExpr* handleZipperedSerial(ForallStmt* fs, FnSymbol* origIterFn,
                                      SymExpr* origSE)
{
  hzsCheckParallelIterator(fs, origIterFn);

  hzsBuildZipperedForLoop(fs, origIterFn, origSE);

  CallExpr* trivialCall = hzsCallTrivialParIter(fs);

  return trivialCall;
}

/////////// final transformations /////////// 

static void addParIdxVarsAndRestruct(ForallStmt* fs, bool gotSA) {
  if (gotSA) {
    // No need to restructure anything. Leaving it as-is for simplicity.

    VarSymbol* parIdx = parIdxVar(fs);

    // FLAG_INDEX_OF_INTEREST is needed in setConstFlagsAndCheckUponMove():
    parIdx->addFlag(FLAG_INDEX_OF_INTEREST);
    parIdx->addFlag(FLAG_INDEX_VAR);

    return;
  }

  // Keep the user loop as its own BlockStmt.
  // Make it the last thing in the new fs->loopBody().
  BlockStmt* userLoopBody = fs->loopBody();
  BlockStmt* newLoopBody = new BlockStmt();
  userLoopBody->replace(newLoopBody);
  newLoopBody->insertAtTail(userLoopBody);

  // The induction variable of the parallel loop.
  VarSymbol* parIdx = newTemp("chpl_followThis");

  // If there is only one follower, we are tempted to use
  // the original forall's induction variable as the
  // the induction variable of the follower loop.
  // Alas, this results in the autoDestroy for that variable
  // to be inserted outside the loop, and more trouble from that.
  // Ex. test/functions/ferguson/ref-pair/iterating-over-arrays.chpl

  // The induction variable of the follower loop.
  VarSymbol* followIdx = newTemp("chpl__followIdx");
  userLoopBody->insertBefore(new DefExpr(followIdx));

  AList& indvars = fs->inductionVariables();
  int idx = indvars.length;

  if (idx == 1) {
    // If only one induction var, treat as non-zippered.
    fs->setNotZippered();
    userLoopBody->insertAtHead("'move'(%S,%S)",
                               toDefExpr(indvars.head)->sym, followIdx);

  }
  else {
    for_alist_backward(def, indvars)
      userLoopBody->insertAtHead("'move'(%S,%S(%S))", toDefExpr(def)->sym,
                                 followIdx, new_IntSymbol(idx--));
  }

  // Move induction variables' DefExprs to the loop body.
  // That's where their scope is; ex. deinit them at end of each iteration.
  // Do it now, before the loop body gets cloned for and dissolves into
  // the scaffolding for fast-followers.
  //
  for_alist_backward(def, indvars)
    userLoopBody->insertAtHead(def->remove());

  // parIdx to be the index variable of the parallel loop.
  // Cf. if gotSA, the original forall's induction variable remains that.
  indvars.insertAtHead(new DefExpr(parIdx));

  // FLAG_INDEX_OF_INTEREST is needed in setConstFlagsAndCheckUponMove():
  parIdx->addFlag(FLAG_INDEX_OF_INTEREST);
  parIdx->addFlag(FLAG_INSERT_AUTO_DESTROY);

  followIdx->addFlag(FLAG_INDEX_OF_INTEREST);
  followIdx->addFlag(FLAG_INDEX_VAR);
  //followIdx->addFlag(FLAG_INSERT_AUTO_DESTROY);

  INT_ASSERT(fs->numInductionVars() == 1);
}

static void checkForNonIterator(IteratorGroup* igroup, ParIterFlavor flavor,
                                CallExpr* parCall)
{
  if ((flavor == PIF_STANDALONE && igroup->noniterSA) ||
      (flavor == PIF_LEADER     && igroup->noniterL)   )
  {
    FnSymbol* dest = parCall->resolvedFunction();
    USR_FATAL_CONT(parCall, "The iterable-expression resolves to a non-iterator function '%s' when looking for a parallel iterator", dest->name);
    USR_PRINT(dest, "The function '%s' is declared here", dest->name);
    USR_STOP();
  }
}

static void resolveParallelIteratorAndIdxVar(ForallStmt* pfs,
                                             CallExpr* iterCall,
                                             FnSymbol* origIterator,
                                             bool gotSA)
{
  // Set QualifiedType of the index variable.
  QualifiedType iType = fsIterYieldType(pfs, iterCall->resolvedFunction());

  VarSymbol* idxVar = parIdxVar(pfs);
  if (idxVar->id == breakOnResolveID) gdbShouldBreakHere();

  idxVar->type = iType.type();
  idxVar->qual = iType.getQual();
}

static Expr* rebuildIterableCall(ForallStmt* pfs,
                                 CallExpr* iterCall,
                                 Expr* origExprFlw)
{
  INT_ASSERT(iterCall == pfs->firstIteratedExpr()); // still here?

  int origLength = pfs->iteratedExpressions().length;
  if (origLength == 1) {
    INT_ASSERT(!pfs->zippered());
    // no tuple building here
    return origExprFlw;
  }

  CallExpr* result = new CallExpr("_build_tuple", origExprFlw);
  while (Expr* curr = iterCall->next)
    result->insertAtTail(curr->remove());

  // todo: remove the assert and origLength
  INT_ASSERT(result->numActuals() == origLength);
  return result;
}

static void buildLeaderLoopBody(ForallStmt* pfs, Expr* iterExpr) {
  VarSymbol* leadIdxCopy = parIdxVar(pfs);
  bool       zippered    = false;
  if (CallExpr* buildTup = toCallExpr(iterExpr)) {
    INT_ASSERT(buildTup->isNamed("_build_tuple"));
    if (buildTup->numActuals() > 1)
      zippered = true;
  }

  DefExpr*  followIdxDef = toDefExpr(pfs->loopBody()->body.head->remove());
  VarSymbol*   followIdx = toVarSymbol(followIdxDef->sym);
  BlockStmt*    userBody = toBlockStmt(pfs->loopBody()->body.tail->remove());
  INT_ASSERT(pfs->loopBody()->body.empty());

  BlockStmt* preFS           = new BlockStmt(BLOCK_SCOPELESS);
  BlockStmt* leadForLoop     = pfs->loopBody();

  VarSymbol* iterRec         = newTemp("chpl__iterLF"); // serial iter, LF case
  VarSymbol* followIter      = newTemp("chpl__followIter");
  BlockStmt* followBlock     = NULL;

  iterRec->addFlag(FLAG_NO_COPY);
  iterRec->addFlag(FLAG_EXPR_TEMP);
  iterRec->addFlag(FLAG_CHPL__ITER);
  iterRec->addFlag(FLAG_CHPL__ITER_NEWSTYLE);

  preFS->insertAtTail(new DefExpr(iterRec));
  preFS->insertAtTail(new CallExpr(PRIM_MOVE, iterRec, iterExpr));
  Expr* toNormalize = preFS->body.tail;

  followBlock = buildFollowLoop(iterRec,
                                leadIdxCopy,
                                followIter,
                                followIdx,
                                userBody,
                                pfs,
                                false,
                                zippered);

  if (fNoFastFollowers == false) {
    Symbol* T1 = newTemp();
    Symbol* T2 = newTemp();

    VarSymbol* fastFollowIdx   = newTemp("chpl__fastFollowIdx");
    VarSymbol* fastFollowIter  = newTemp("chpl__fastFollowIter");
    BlockStmt* fastFollowBlock = NULL;


    T1->addFlag(FLAG_EXPR_TEMP);
    T1->addFlag(FLAG_MAYBE_PARAM);

    T2->addFlag(FLAG_EXPR_TEMP);
    T2->addFlag(FLAG_MAYBE_PARAM);

    leadForLoop->insertAtTail(new DefExpr(T1));
    leadForLoop->insertAtTail(new DefExpr(T2));

    if (zippered == false) {
      leadForLoop->insertAtTail("'move'(%S, chpl__staticFastFollowCheck(%S))",    T1, iterRec);
      leadForLoop->insertAtTail(new CondStmt(new SymExpr(T1),
                                          new_Expr("'move'(%S, chpl__dynamicFastFollowCheck(%S))",    T2, iterRec),
                                          new_Expr("'move'(%S, %S)", T2, gFalse)));
    } else {
      leadForLoop->insertAtTail("'move'(%S, chpl__staticFastFollowCheckZip(%S))", T1, iterRec);
      leadForLoop->insertAtTail(new CondStmt(new SymExpr(T1),
                                          new_Expr("'move'(%S, chpl__dynamicFastFollowCheckZip(%S))", T2, iterRec),
                                          new_Expr("'move'(%S, %S)", T2, gFalse)));
    }

    SymbolMap map;
    map.put(followIdx, fastFollowIdx);
    BlockStmt* userBodyForFast = userBody->copy(&map);

    fastFollowBlock = buildFollowLoop(iterRec,
                                      leadIdxCopy,
                                      fastFollowIter,
                                      fastFollowIdx,
                                      userBodyForFast,
                                      pfs,
                                      true,
                                      zippered);

    leadForLoop->insertAtTail(new CondStmt(new SymExpr(T2), fastFollowBlock, followBlock));
  } else {
    leadForLoop->insertAtTail(followBlock);
  }

  pfs->insertBefore(preFS);
  normalize(toNormalize); // requires inTree()
  resolveBlockStmt(preFS);
  preFS->flattenAndRemove();
}

void static setupRecIterFields(ForallStmt* fs, CallExpr* parIterCall);

/////////// resolveForallHeader, setupRecIterFields ///////////

// Returns the next expression to resolve.
CallExpr* resolveForallHeader(ForallStmt* pfs, SymExpr* origSE)
{
  CallExpr* retval = NULL;

  if (pfs->id == breakOnResolveID) gdbShouldBreakHere();

  // We only get here for origSE==firstIteratedExpr() .
  // If at that time there are other elements in iterExprs(), we remove them.
  INT_ASSERT(origSE == pfs->firstIteratedExpr());

  FnSymbol* origTarget = NULL; //for assertions
  CallExpr* iterCall = buildForallParIterCall(pfs, origSE, origTarget);

  // So we know where iterCall is.
  INT_ASSERT(iterCall         == pfs->firstIteratedExpr());
  INT_ASSERT(origSE->inTree() == false);

  bool useOriginal = acceptUnmodifiedIterCall(pfs, iterCall);
  ParIterFlavor flavor =
    useOriginal ? PIF_SERIAL
                : findParIter(pfs, iterCall, origSE, origTarget);

  resolveCallAndCallee(iterCall, false);

  // ex. resolving the par iter failed and 'pfs' is under "if chpl__tryToken"
  if (tryFailure) return NULL;

  FnSymbol* origIterFn = iterCall->resolvedFunction();
  bool gotSA = (flavor != PIF_LEADER); // "got Single iterAtor"

  if (origTarget) {
    IteratorGroup* igroup = origTarget->iteratorGroup;
    checkForNonIterator(igroup, flavor, iterCall);

    if (origTarget == origIterFn) {
      INT_ASSERT(flavor == PIF_SERIAL);
      INT_ASSERT(pfs->allowSerialIterator());
      INT_ASSERT(origIterFn == igroup->serial);
    } else if (gotSA) {
      INT_ASSERT(origIterFn == igroup->standalone);
    } else {
      INT_ASSERT(origIterFn == igroup->leader);
    }
  }

  if (flavor == PIF_SERIAL && pfs->numIteratedExprs() > 1) {
    // numIteratedExprs() is a good number to check, right?
    INT_ASSERT(pfs->numIteratedExprs() == pfs->numInductionVars());

    retval = handleZipperedSerial(pfs, origIterFn, origSE);

    setupAndResolveShadowVars(pfs);

  } else {
    addParIdxVarsAndRestruct(pfs, gotSA);

    resolveParallelIteratorAndIdxVar(pfs, iterCall, origIterFn, gotSA);

    setupAndResolveShadowVars(pfs);

    if (gotSA) {
      if (origSE->qualType().type()->symbol->hasFlag(FLAG_ITERATOR_RECORD)) {
        removeOrigIterCall(origSE);
      }
    } else {
      buildLeaderLoopBody(pfs, rebuildIterableCall(pfs, iterCall, origSE));
    }

    INT_ASSERT(iterCall == pfs->firstIteratedExpr());        // still here?
    INT_ASSERT(iterCall == pfs->iteratedExpressions().tail); // only 1 elem

    retval = iterCall;

    setupRecIterFields(pfs, iterCall);
  }

  return retval;
}


// The fRecIter* fields:
//   fRecIterIRdef, fRecIterICdef, fRecIterGetIterator, fRecIterFreeIterator
// are used to fall back to the "old" lowering of ForallStmts
// based on the iterator record/iterator class.
// We set them up here so that calls like _getIterator can be resolved.
//
// This fallback is used only when the parallel iterator is recursive.
// Otherwise the iterator is simply inlined by
//   lowerForallStmtsInline() / lowerOneForallStmt()
//
// Recursive-ness is detected later by
//   computeRecursiveIteratorSet() / find_recursive_caller().
// Since we do not know it here, we do the work
// even in the (common) case where it will not be needed.
//
void static setupRecIterFields(ForallStmt* fs, CallExpr* parIterCall)
{
  SET_LINENO(parIterCall);

  VarSymbol* iterRec = newTemp("chpl__iterPAR"); // serial iter, PAR case
  VarSymbol* parIter = newTemp("chpl__parIter");
  VarSymbol* parIdx  = parIdxVar(fs);

  iterRec->addFlag(FLAG_NO_COPY);
  iterRec->addFlag(FLAG_CHPL__ITER);
  iterRec->addFlag(FLAG_CHPL__ITER_NEWSTYLE);
  iterRec->addFlag(FLAG_MAYBE_REF);
  iterRec->addFlag(FLAG_EXPR_TEMP);

  parIter->addFlag(FLAG_EXPR_TEMP);
  // Too late to do it here - it's needed in setConstFlagsAndCheckUponMove().
  //parIdx->addFlag(FLAG_INDEX_OF_INTEREST);
  parIdx->addFlag(FLAG_INDEX_VAR);

  BlockStmt* holder = new BlockStmt();
  fs->insertBefore(holder);  // so we can resolve it

  DefExpr*   recIterIRdef = new DefExpr(iterRec);
  DefExpr*   recIterICdef = new DefExpr(parIter);
  CallExpr*  recIterGetIterator  = new CallExpr("_getIterator", iterRec);
  CallExpr*  recIterFreeIterator = new CallExpr("_freeIterator", parIter);
  
  CallExpr* initIterRec = new CallExpr(PRIM_MOVE, iterRec, parIterCall->copy());
  CallExpr* initParIter = new CallExpr(PRIM_MOVE, parIter, recIterGetIterator);

  holder->insertAtTail(recIterIRdef);
  holder->insertAtTail(recIterICdef);
  holder->insertAtTail(initIterRec);
  holder->insertAtTail(initParIter);
  holder->insertAtTail(recIterFreeIterator);

  // This may not resolve if postponed until lowerIterators.
  resolveBlockStmt(holder);

  fs->fRecIterIRdef        = recIterIRdef;
  fs->fRecIterICdef        = recIterICdef;
  fs->fRecIterGetIterator  = recIterGetIterator;
  fs->fRecIterFreeIterator = recIterFreeIterator;

  Symbol* PS = fs->parentSymbol;
  recIterIRdef       ->remove();  insert_help(recIterIRdef,        fs, PS);
  recIterICdef       ->remove();  insert_help(recIterICdef,        fs, PS);
  recIterGetIterator ->remove();  insert_help(recIterGetIterator,  fs, PS);
  recIterFreeIterator->remove();  insert_help(recIterFreeIterator, fs, PS);

  initParIter->remove();
  // This call messes up doNotTransformForForall() in callDestructors.
  // Remove it until we need it, if at all.
  initIterRec->remove();

  INT_ASSERT(holder->body.empty());
  holder->remove();
}


///////////////////////////////
//                           //
//   ForallStmt lowering 2   //
//                           //
///////////////////////////////

// These actuals have been added to handle outer variables in LoopExpr's body.
// The leader iterator neither accepts nor handles them. So drop them.
static void removeOuterVarArgs(CallExpr* iterCall, FnSymbol* oldCallee,
                               FnSymbol* newCallee) {
  int numFormals = newCallee->numFormals();
  int numActuals = iterCall->numActuals();
  INT_ASSERT(numActuals == oldCallee->numFormals());

  if (numFormals == numActuals)
    return; // there were no outer variables, nothing to do

  std::vector<Symbol*> symbols;
  if (fVerify) collectSymbols(oldCallee->body, symbols);

  for (int xtraIdx = numFormals + 1; xtraIdx <= numActuals; xtraIdx++) {
    // Remove the next extra actual.
    iterCall->get(numFormals+1)->remove();

    if (fVerify) {
      // Ensure oldCallee did not use it.
      Symbol* xtraFormal = oldCallee->getFormal(xtraIdx);
      for_vector(Symbol, sym, symbols) INT_ASSERT(sym != xtraFormal);
    }
  }
}

//
// Handle the case where the leader iterator is chpl__loopexpr_iter.
// Not doing so confuses ReturnByRef and lowering of ForallStmts.
//
// Tests:
//   library/packages/Collection/CollectionCounter.chpl
//   library/standard/Random/deitz/test1D2D.chpl
//   reductions/deitz/test_maxloc_reduce_wmikanik_bug2.chpl
//
static void convertIteratorForLoopexpr(ForallStmt* fs) {
  if (CallExpr* iterCall = toCallExpr(fs->iteratedExpressions().head))
    if (SymExpr* calleeSE = toSymExpr(iterCall->baseExpr))
      if (FnSymbol* calleeFn = toFnSymbol(calleeSE->symbol()))
       if (! calleeFn->retType->symbol->hasFlag(FLAG_ITERATOR_RECORD))
        if (isLoopExprFun(calleeFn)) {
          // In this case, we have a _toLeader call and no side effects.
          // Just use the iterator corresponding to the iterator record.
          FnSymbol* iterator = getTheIteratorFn(calleeFn->retType);
          SET_LINENO(calleeSE);
          calleeSE->replace(new SymExpr(iterator));
          if (calleeFn->firstSymExpr() == NULL)
            calleeFn->defPoint->remove(); // not needed any more
          removeOuterVarArgs(iterCall, calleeFn, iterator);
          // Adds coercions as needed.
          if (iterCall->get(1)->getValType() != iterator->getFormal(1)->getValType())
            resolveCall(iterCall);
        }
}

// Todo: can we merge this into resolveForallHeader() ?
void resolveForallStmts2() {
  forv_Vec(ForallStmt, fs, gForallStmts) {
    if (!fs->inTree() || !fs->getFunction()->isResolved())
      continue;

    if (fs->fromReduce()) continue; // not an error

    // formerly nonLeaderParCheckInt()
    FnSymbol* parent = fs->getFunction();
    // If isTaskFun(parent), error is still reported in nonLeaderParCheckInt.
    if (parent->isIterator() && !parent->hasFlag(FLAG_INLINE_ITERATOR))
      USR_FATAL_CONT(fs, "invalid use of parallel construct in serial iterator");
    
    convertIteratorForLoopexpr(fs);
  }
}


///////////////////////////////
//                           //
//   PRIM_REDUCE lowering    //
//                           //
///////////////////////////////

// Insert a call temp. It is simpler than the full-blown normalize()
// and the caller can reference the temp.
static SymExpr* normalizeIITR(Expr* ref, Expr* iitr) {
  VarSymbol* temp = newTemp("iitr_temp");
  temp->addFlag(FLAG_TYPE_VARIABLE);
  ref->insertBefore(new DefExpr(temp));
  ref->insertBefore("'move'(%S,%E)", temp, iitr);
  return new SymExpr(temp);
}

// Given a reduce expression like "op reduce data", return op(inputType).
// inputType is the type of things being reduced - when iterating over 'data'.
// This matches the case where inputType is provided by the user.
static Expr* lowerReduceOp(Expr* ref, SymExpr* opSE, SymExpr* dataSE,
                           bool zippered)
{
  CallExpr* iit = NULL;
  if (zippered) {
    // Cf. destructZipperedIterables. 'zipcall' will be removed there.
    CallExpr* zipcall = toCallExpr(getDefOfTemp(dataSE)->copy());
    INT_ASSERT(zipcall->isPrimitive(PRIM_ZIP));
    iit = new CallExpr("iteratorIndexTypeZip");
    for_actuals(actual, zipcall)
      iit->insertAtTail(toSymExpr(actual)->symbol());
  } else {
    iit = new CallExpr("iteratorIndexType", dataSE->symbol());
  }

  ref->insertBefore(iit);
  Expr* iitR = resolveExpr(iit)->remove();
  if (!isSymExpr(iitR)) iitR = normalizeIITR(ref, iitR);

  return new CallExpr(opSE, iitR);
}

// Within resolveBlockStmt / for_exprs_postorder framework,
// we need to lower PRIM_REDUCE prior to the resolveCall()
// that gets invoked from resolveExpr(). That way the
// ForallStmt plus scaffolding that lowerPrimReduce() injects
// can come after 'retval'. resolveCall() does not support that.

void lowerPrimReduce(CallExpr* call, Expr*& retval) {
  if (call->id == breakOnResolveID) gdbShouldBreakHere();

  Expr*   callStmt = call->getStmtExpr();
  CallExpr*   noop = new CallExpr(PRIM_NOOP);
  callStmt->insertBefore(noop);

  SymExpr*   opSE = toSymExpr(call->get(1)->remove());           // 1st arg
  SymExpr* dataSE = toSymExpr(call->get(1)->remove());           // 2nd arg
  bool   zippered = toSymExpr(call->get(1))->symbol() == gTrue;  // 3rd arg
  bool  reqSerial = false; // We may need it for #11819, otherwise remove it.

  Expr* opExpr = lowerReduceOp(callStmt, opSE, dataSE, zippered);

  VarSymbol* result = newTemp("chpl_redResult");
  callStmt->insertBefore(new DefExpr(result));

  VarSymbol*       idx  = newTemp("chpl_redIdx");
  ShadowVarSymbol* svar = new ShadowVarSymbol(TFI_REDUCE, "chpl_redSVar",
                                              new SymExpr(result), opExpr);
  ForallStmt*      fs   = ForallStmt::fromReduceExpr(idx, dataSE, svar,
                                                     zippered, reqSerial);
  callStmt->insertBefore(fs);
  call->replace(new SymExpr(result));
  retval = noop;
}
