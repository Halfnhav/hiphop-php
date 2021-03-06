/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/service-requests-x64.h"

#include "hphp/runtime/vm/jit/code-gen-helpers-x64.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/translator-x64.h"
#include "hphp/runtime/vm/jit/translator-x64-internal.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/x64-util.h"
#include "hphp/runtime/vm/srckey.h"
#include "hphp/util/asm-x64.h"
#include "hphp/util/ringbuffer.h"

namespace HPHP { namespace JIT { namespace X64 {

using Transl::ConditionCode;
using Transl::CodeCursor;
using Transl::TCA;

TRACE_SET_MOD(servicereq);

// An intentionally funny-looking-in-core-dumps constant for uninitialized
// instruction pointers.
constexpr uint64_t kUninitializedRIP = 0xba5eba11acc01ade;

namespace {

void emitBindJ(Asm& a, Asm& astubs,
               ConditionCode cc, SrcKey dest, ServiceRequest req) {
  prepareForSmash(a, cc == Transl::CC_None ? kJmpLen : kJmpccLen);
  TCA toSmash = a.frontier();
  if (a.base() == astubs.base()) {
    emitJmpOrJcc(a, cc, toSmash);
  }

  tx64->setJmpTransID(toSmash);

  TCA sr = (req == JIT::REQ_BIND_JMP
            ? emitEphemeralServiceReq(astubs, tx64->getFreeStub(), req, toSmash,
                                      dest.offset())
            : emitServiceReq(astubs, req, toSmash, dest.offset()));

  if (a.base() == astubs.base()) {
    CodeCursor cursor(a, toSmash);
    emitJmpOrJcc(a, cc, sr);
  } else {
    emitJmpOrJcc(a, cc, sr);
  }
}

/*
 * NativeImpl is a special operation in the sense that it must be the
 * only opcode in a function body, and also functions as the return.
 *
 * if emitSavedRIPReturn is false, it returns the amount by which
 * rVmSp should be adjusted, otherwise, it emits code to perform
 * the adjustment (this allows us to combine updates to rVmSp)
 */
int32_t emitNativeImpl(CodeBlock& mainCode, const Func* func,
                       bool emitSavedRIPReturn) {
  BuiltinFunction builtinFuncPtr = func->builtinFuncPtr();
  if (false) { // typecheck
    ActRec* ar = nullptr;
    builtinFuncPtr(ar);
  }

  TRACE(2, "calling builtin preClass %p func %p\n", func->preClass(),
    builtinFuncPtr);
  /*
   * Call the native implementation. This will free the locals for us in the
   * normal case. In the case where an exception is thrown, the VM unwinder
   * will handle it for us.
   */
  Asm a { mainCode };
  a.   mov_reg64_reg64(rVmFp, argNumToRegName[0]);
  if (tx64->fixupMap().eagerRecord(func)) {
    emitEagerSyncPoint(a, func->getEntry(), 0);
  }
  emitCall(a, (TCA)builtinFuncPtr);

  /*
   * We're sometimes calling this while curFunc() isn't really the
   * builtin---make sure to properly record the sync point as if we
   * are inside the builtin.
   *
   * The assumption here is that for builtins, the generated func
   * contains only a single opcode (NativeImpl), and there are no
   * non-argument locals.
   */
  assert(func->numIterators() == 0 && func->isBuiltin());
  assert(func->numLocals() == func->numParams());
  assert(toOp(*func->getEntry()) == OpNativeImpl);
  assert(instrLen((Op*)func->getEntry()) == func->past() - func->base());
  Offset pcOffset = 0;  // NativeImpl is the only instruction in the func
  Offset stackOff = func->numLocals(); // Builtin stubs have no
                                       // non-arg locals
  tx64->fixupMap().recordSyncPoint(mainCode.frontier(), pcOffset, stackOff);

  if (emitSavedRIPReturn) {
    // push the return address to get ready to ret.
    a.   push  (rVmFp[AROFF(m_savedRip)]);
  }

  /*
   * The native implementation already put the return value on the
   * stack for us, and handled cleaning up the arguments.  We have to
   * update the frame pointer and the stack pointer, and load the
   * return value into the return register so the trace we are
   * returning to has it where it expects.
   *
   * TODO(#1273094): we should probably modify the actual builtins to
   * return values via registers (rax:edx) using the C ABI and do a
   * reg-to-reg move.
   */
  int nLocalCells = func->numSlotsInFrame();
  if (emitSavedRIPReturn) {
    a. add_imm64_reg64(sizeof(ActRec) + cellsToBytes(nLocalCells-1), rVmSp);
  }
  a.   load_reg64_disp_reg64(rVmFp, AROFF(m_savedRbp), rVmFp);

  emitRB(a, Trace::RBTypeFuncExit, func->fullName()->data());
  if (emitSavedRIPReturn) {
    a. ret();
    translator_not_reached(a);
    return 0;
  }
  return sizeof(ActRec) + cellsToBytes(nLocalCells-1);
}

void emitBindCallHelper(CodeBlock& mainCode, CodeBlock& stubsCode,
                        SrcKey srcKey,
                        const Func* funcd,
                        int numArgs) {
  // Whatever prologue we're branching to will check at runtime that we
  // went to the right Func*, correcting if necessary. We treat the first
  // Func we encounter as a decent prediction. Make space to burn in a
  // TCA.
  ReqBindCall* req = tx64->globalData().alloc<ReqBindCall>();

  Asm a { mainCode };
  prepareForSmash(a, kCallLen);
  TCA toSmash = mainCode.frontier();
  a.    call(stubsCode.frontier());

  Asm astubs { stubsCode };
  astubs.    mov_reg64_reg64(rStashedAR, serviceReqArgRegs[1]);
  emitPopRetIntoActRec(astubs);
  emitServiceReq(astubs, JIT::REQ_BIND_CALL, req);

  TRACE(1, "will bind static call: tca %p, funcd %p, astubs %p\n",
        toSmash, funcd, stubsCode.frontier());
  req->m_toSmash = toSmash;
  req->m_nArgs = numArgs;
  req->m_sourceInstr = srcKey;
  req->m_isImmutable = (bool)funcd;
}

bool isNativeImplCall(const Func* funcd, int numArgs) {
  return funcd && funcd->info() && numArgs == funcd->numParams();
}

} // anonymous namespace

void emitBindJcc(Asm& a, Asm& astubs, Transl::ConditionCode cc,
                 SrcKey dest, ServiceRequest req /* = REQ_BIND_JCC */) {
  emitBindJ(a, astubs, cc, dest, req);
}

void emitBindJmp(Asm& a, Asm& astubs,
                 SrcKey dest, ServiceRequest req /* = REQ_BIND_JMP */) {
  emitBindJ(a, astubs, Transl::CC_None, dest, req);
}

int32_t emitBindCall(CodeBlock& mainCode, CodeBlock& stubsCode,
                     SrcKey srcKey, const Func* funcd, int numArgs) {
  // If this is a call to a builtin and we don't need any argument
  // munging, we can skip the prologue system and do it inline.
  if (isNativeImplCall(funcd, numArgs)) {
    StoreImmPatcher patchIP(mainCode, (uint64_t)mainCode.frontier(), reg::rax,
                            cellsToBytes(numArgs) + AROFF(m_savedRip),
                            rVmSp);
    assert(funcd->numLocals() == funcd->numParams());
    assert(funcd->numIterators() == 0);
    Asm a { mainCode };
    emitLea(a, rVmSp, cellsToBytes(numArgs), rVmFp);
    emitCheckSurpriseFlagsEnter(mainCode, stubsCode, true, tx64->fixupMap(),
                                Fixup(0, numArgs));
    // rVmSp is already correctly adjusted, because there's no locals
    // other than the arguments passed.
    auto retval = emitNativeImpl(mainCode, funcd,
                                 false /* don't jump to return */);
    patchIP.patch(uint64_t(mainCode.frontier()));
    return retval;
  }

  Asm a { mainCode };
  if (debug) {
    a.    storeq (kUninitializedRIP,
                  rVmSp[cellsToBytes(numArgs) + AROFF(m_savedRip)]);
  }
  // Stash callee's rVmFp into rStashedAR for the callee's prologue
  emitLea(a, rVmSp, cellsToBytes(numArgs), rStashedAR);
  emitBindCallHelper(mainCode, stubsCode, srcKey, funcd, numArgs);
  return 0;
}


}}}
