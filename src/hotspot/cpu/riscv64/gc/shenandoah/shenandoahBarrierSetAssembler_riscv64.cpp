/*
 * Copyright (c) 2018, 2020, Red Hat, Inc. All rights reserved.
 * Copyright (c) 2020, 2021, Huawei Technologies Co., Ltd. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "gc/shenandoah/shenandoahBarrierSetAssembler.hpp"
#include "gc/shenandoah/shenandoahForwarding.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"
#include "gc/shenandoah/shenandoahHeuristics.hpp"
#include "gc/shenandoah/shenandoahRuntime.hpp"
#include "gc/shenandoah/shenandoahThreadLocalData.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/interp_masm.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/thread.hpp"
#ifdef COMPILER1
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "gc/shenandoah/c1/shenandoahBarrierSetC1.hpp"
#endif

#define __ masm->

address ShenandoahBarrierSetAssembler::_shenandoah_lrb = NULL;

void ShenandoahBarrierSetAssembler::arraycopy_prologue(MacroAssembler* masm, DecoratorSet decorators, bool is_oop,
                                                       Register src, Register dst, Register count, RegSet saved_regs) {
  if (is_oop) {
    bool dest_uninitialized = (decorators & IS_DEST_UNINITIALIZED) != 0;
    if (ShenandoahSATBBarrier && !dest_uninitialized && !ShenandoahHeap::heap()->heuristics()->can_do_traversal_gc()) {

      Label done;

      // Avoid calling runtime if count == 0
      __ beqz(count, done);

      // Is marking active?
      Address gc_state(xthread, in_bytes(ShenandoahThreadLocalData::gc_state_offset()));
      assert_different_registers(src, dst, count, t0);

      __ lbu(t0, gc_state);
      __ andi(t0, t0, ShenandoahHeap::MARKING);
      __ beqz(t0, done);

      __ push_reg(saved_regs, sp);
      if (count == c_rarg0) {
        if (dst == c_rarg1) {
          // exactly backwards!!
          __ mv(t0, c_rarg0);
          __ mv(c_rarg0, c_rarg1);
          __ mv(c_rarg1, t0);
        } else {
          __ mv(c_rarg1, count);
          __ mv(c_rarg0, dst);
        }
      } else {
        __ mv(c_rarg0, dst);
        __ mv(c_rarg1, count);
        }
      if (UseCompressedOops) {
        __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::write_ref_array_pre_narrow_oop_entry), 2);
      } else {
        __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::write_ref_array_pre_oop_entry), 2);
      }
      __ pop_reg(saved_regs, sp);
      __ bind(done);
    }
  }
}

void ShenandoahBarrierSetAssembler::arraycopy_epilogue(MacroAssembler* masm, DecoratorSet decorators, bool is_oop,
                                                       Register start, Register count, Register scratch, RegSet saved_regs) {
  if (is_oop) {
      Label done;

      // Avoid calling runtime if count == 0
      __ beqz(count, done);

      // Is updating references?
      Address gc_state(xthread, in_bytes(ShenandoahThreadLocalData::gc_state_offset()));
      __ lbu(t0, gc_state);
      __ andi(t0, t0, ShenandoahHeap::UPDATEREFS);
      __ beqz(t0, done);

    __ push_reg(saved_regs, sp);
    assert_different_registers(start, count, scratch);
    assert_different_registers(c_rarg0, count);
    __ mv(c_rarg0, start);
    __ mv(c_rarg1, count);
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::write_ref_array_post_entry), 2);
    __ pop_reg(saved_regs, sp);

    __ bind(done);
  }
}

void ShenandoahBarrierSetAssembler::shenandoah_write_barrier_pre(MacroAssembler* masm,
                                                                 Register obj,
                                                                 Register pre_val,
                                                                 Register thread,
                                                                 Register tmp,
                                                                 bool tosca_live,
                                                                 bool expand_call) {
  if (ShenandoahSATBBarrier) {
    satb_write_barrier_pre(masm, obj, pre_val, thread, tmp, tosca_live, expand_call);
  }
}

void ShenandoahBarrierSetAssembler::satb_write_barrier_pre(MacroAssembler* masm,
                                                           Register obj,
                                                           Register pre_val,
                                                           Register thread,
                                                           Register tmp,
                                                           bool tosca_live,
                                                           bool expand_call) {
  // If expand_call is true then we expand the call_VM_leaf macro
  // directly to skip generating the check by
  // InterpreterMacroAssembler::call_VM_leaf_base that checks _last_sp.
  assert(thread == xthread, "must be");

  Label done;
  Label runtime;

  assert_different_registers(obj, pre_val, tmp, t0);
  assert(pre_val != noreg &&  tmp != noreg, "expecting a register");

  Address in_progress(thread, in_bytes(ShenandoahThreadLocalData::satb_mark_queue_active_offset()));
  Address index(thread, in_bytes(ShenandoahThreadLocalData::satb_mark_queue_index_offset()));
  Address buffer(thread, in_bytes(ShenandoahThreadLocalData::satb_mark_queue_buffer_offset()));

  // Is marking active?
  if (in_bytes(SATBMarkQueue::byte_width_of_active()) == 4) {
    __ lwu(tmp, in_progress);
  } else {
    assert(in_bytes(SATBMarkQueue::byte_width_of_active()) == 1, "Assumption");
    __ lbu(tmp, in_progress);
  }
  __ beqz(tmp, done);

  // Do we need to load the previous value?
  if (obj != noreg) {
    __ load_heap_oop(pre_val, Address(obj, 0), noreg, noreg, AS_RAW);
  }

  // Is the previous value null?
  __ beqz(pre_val, done);

  // Can we store original value in the thread's buffer?
  // Is index == 0?
  // (The index field is typed as size_t.)
  __ ld(tmp, index);                        // tmp := *index_adr
  __ beqz(tmp, runtime);                    // tmp == 0? If yes, goto runtime

  __ sub(tmp, tmp, wordSize);               // tmp := tmp - wordSize
  __ sd(tmp, index);                        // *index_adr := tmp
  __ ld(t0, buffer);
  __ add(tmp, tmp, t0);                     // tmp := tmp + *buffer_adr

  // Record the previous value
  __ sd(pre_val, Address(tmp, 0));
  __ j(done);

  __ bind(runtime);
  // save the live input values
  RegSet saved = RegSet::of(pre_val);
  if (tosca_live) saved += RegSet::of(x10);
  if (obj != noreg) saved += RegSet::of(obj);

  __ push_reg(saved, sp);

  // Calling the runtime using the regular call_VM_leaf mechanism generates
  // code (generated by InterpreterMacroAssember::call_VM_leaf_base)
  // that checks that the *(rfp+frame::interpreter_frame_last_sp) == NULL.
  //
  // If we care generating the pre-barrier without a frame (e.g. in the
  // intrinsified Reference.get() routine) then ebp might be pointing to
  // the caller frame and so this check will most likely fail at runtime.
  //
  // Expanding the call directly bypasses the generation of the check.
  // So when we do not have have a full interpreter frame on the stack
  // expand_call should be passed true.
  if (expand_call) {
    assert(pre_val != c_rarg1, "smashed arg");
    __ super_call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::write_ref_field_pre_entry), pre_val, thread);
  } else {
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::write_ref_field_pre_entry), pre_val, thread);
  }

  __ pop_reg(saved, sp);

  __ bind(done);
}

void ShenandoahBarrierSetAssembler::resolve_forward_pointer(MacroAssembler* masm, Register dst, Register tmp) {
  assert(ShenandoahLoadRefBarrier || ShenandoahCASBarrier, "Should be enabled");

  Label is_null;
  __ beqz(dst, is_null);
  resolve_forward_pointer_not_null(masm, dst, tmp);
  __ bind(is_null);
}

// IMPORTANT: This must preserve all registers, even t0 and t1, except those explicitely
// passed in.
void ShenandoahBarrierSetAssembler::resolve_forward_pointer_not_null(MacroAssembler* masm, Register dst, Register tmp) {
  assert(ShenandoahLoadRefBarrier || ShenandoahCASBarrier, "Should be enabled");
  // The below loads the mark word, checks if the lowest two bits are
  // set, and if so, clear the lowest two bits and copy the result
  // to dst. Otherwise it leaves dst alone.
  // Implementing this is surprisingly awkward. I do it here by:
  // - Inverting the mark word
  // - Test lowest two bits == 0
  // - If so, set the lowest two bits
  // - Invert the result back, and copy to dst
  RegSet saved_regs = RegSet::of(t2);
  bool borrow_reg = (tmp == noreg);
  if (borrow_reg) {
    // No free registers available. Make one useful.
    tmp = t0;
    if (tmp == dst) {
      tmp = t1;
    }
    saved_regs += RegSet::of(tmp);
  }

  assert_different_registers(tmp, dst, t2);
  __ push_reg(saved_regs, sp);

  Label done;
  __ ld(tmp, Address(dst, oopDesc::mark_offset_in_bytes()));
  __ xori(tmp, tmp, -1); // eon with 0 is equivalent to XOR with -1
  __ andi(t2, tmp, markOopDesc::lock_mask_in_place);
  __ bnez(t2, done);
  __ ori(tmp, tmp, markOopDesc::marked_value);
  __ xori(dst, tmp, -1); // eon with 0 is equivalent to XOR with -1
  __ bind(done);

  __ pop_reg(saved_regs, sp);
}

void ShenandoahBarrierSetAssembler::load_reference_barrier_not_null(MacroAssembler* masm, Register dst, Register tmp) {
  assert(ShenandoahLoadRefBarrier, "Should be enabled");
  assert(dst != t1 && load_addr.base() != t1, "need t1");

  Label done;
  __ enter();
  Address gc_state(xthread, in_bytes(ShenandoahThreadLocalData::gc_state_offset()));
  __ lbu(t1, gc_state);

  // Check for heap stability
  __ andi(t0, t1, ShenandoahHeap::HAS_FORWARDED);
  __ beqz(t0, done);

  RegSet saved_regs = RegSet::of(x10);
  if (dst != x10) {
    __ push_reg(saved_regs, sp);
    __ mv(x10, dst);
  }

  __ far_call(RuntimeAddress(CAST_FROM_FN_PTR(address, ShenandoahBarrierSetAssembler::shenandoah_lrb())));

  if (dst != x10) {
    __ mv(dst, x10);
    __ pop_reg(saved_regs, sp);
  }

  __ bind(done);
  __ leave();
}

void ShenandoahBarrierSetAssembler::storeval_barrier(MacroAssembler* masm, Register dst, Register tmp) {
  if (ShenandoahStoreValEnqueueBarrier) {
    __ push_call_clobbered_registers();

    satb_write_barrier_pre(masm, noreg, dst, xthread, tmp, true, false);

    __ pop_call_clobbered_registers();
  }
}

void ShenandoahBarrierSetAssembler::load_reference_barrier(MacroAssembler* masm, Register dst, Register tmp) {
  if (ShenandoahLoadRefBarrier) {
    Label is_null;
    __ beqz(dst, is_null);
    load_reference_barrier_not_null(masm, dst, tmp);
    __ bind(is_null);
  }
}

void ShenandoahBarrierSetAssembler::load_at(MacroAssembler* masm,
                                            DecoratorSet decorators,
                                            BasicType type,
                                            Register dst,
                                            Address src,
                                            Register tmp1,
                                            Register tmp_thread) {
  bool on_oop = type == T_OBJECT || type == T_ARRAY;
  bool on_weak = (decorators & ON_WEAK_OOP_REF) != 0;
  bool on_phantom = (decorators & ON_PHANTOM_OOP_REF) != 0;
  bool on_reference = on_weak || on_phantom;

  BarrierSetAssembler::load_at(masm, decorators, type, dst, src, tmp1, tmp_thread);

  if (on_oop) {
    load_reference_barrier(masm, dst, tmp1);
    if (ShenandoahKeepAliveBarrier && on_reference) {
      __ enter();
      satb_write_barrier_pre(masm /* masm */,
                             noreg /* obj */,
                             dst /* pre_val */,
                             xthread /* thread */,
                             tmp1 /* tmp */,
                             true /* tosca_live */,
                             true /* expand_call */);
      __ leave();
    }
  }
}

void ShenandoahBarrierSetAssembler::store_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                             Address dst, Register val, Register tmp1, Register tmp2) {
  bool on_oop = type == T_OBJECT || type == T_ARRAY;
  if (!on_oop) {
    BarrierSetAssembler::store_at(masm, decorators, type, dst, val, tmp1, tmp2);
    return;
  }

  // flatten object address if needed
  if (dst.offset() == 0) {
    if (dst.base() != x13) {
      __ mv(x13, dst.base());
    }
  } else {
    __ la(x13, dst);
  }

  shenandoah_write_barrier_pre(masm,
                               x13 /* obj */,
                               tmp2 /* pre_val */,
                               xthread /* thread */,
                               tmp1  /* tmp */,
                               val != noreg /* tosca_live */,
                               false /* expand_call */);

  if (val == noreg) {
    BarrierSetAssembler::store_at(masm, decorators, type, Address(x13, 0), noreg, noreg, noreg);
  } else {
    storeval_barrier(masm, val, tmp1);
    // G1 barrier needs uncompressed oop for region cross check.
    Register new_val = val;
    if (UseCompressedOops) {
      new_val = t1;
      __ mv(new_val, val);
    }
    BarrierSetAssembler::store_at(masm, decorators, type, Address(x13, 0), val, noreg, noreg);
  }
}

// Special Shenandoah CAS implementation that handles false negatives due
// to concurrent evacuation.  The service is more complex than a
// traditional CAS operation because the CAS operation is intended to
// succeed if the reference at addr exactly matches expected or if the
// reference at addr holds a pointer to a from-space object that has
// been relocated to the location named by expected.  There are two
// races that must be addressed:
//  a) A parallel thread may mutate the contents of addr so that it points
//     to a different object.  In this case, the CAS operation should fail.
//  b) A parallel thread may heal the contents of addr, replacing a
//     from-space pointer held in addr with the to-space pointer
//     representing the new location of the object.
// Upon entry to cmpxchg_oop, it is assured that new_val equals NULL
// or it refers to an object that is not being evacuated out of
// from-space, or it refers to the to-space version of an object that
// is being evacuated out of from-space.
//
// By default, this operation implements sequential consistency and the
// value held in the result register following execution of the
// generated code sequence is 0 to indicate failure of CAS, non-zero
// to indicate success.  Arguments support variations on this theme:
//
//  acquire: Allow relaxation of the memory ordering on CAS from
//           sequential consistency.  This can be useful when
//           sequential consistency is not required, such as when
//           another sequentially consistent operation is already
//           present in the execution stream.  If acquire, successful
//           execution has the side effect of assuring that memory
//           values updated by other threads and "released" will be
//           visible to any read operations perfomed by this thread
//           which follow this operation in program order.  This is a
//           special optimization that should not be enabled by default.
//  release: Allow relaxation of the memory ordering on CAS from
//           sequential consistency.  This can be useful when
//           sequential consistency is not required, such as when
//           another sequentially consistent operation is already
//           present in the execution stream.  If release, successful
//           completion of this operation has the side effect of
//           assuring that all writes to memory performed by this
//           thread that precede this operation in program order are
//           visible to all other threads that subsequently "acquire"
//           before reading the respective memory values.  This is a
//           special optimization that should not be enabled by default.
//  is_cae:  This turns CAS (compare and swap) into CAE (compare and
//           exchange).  This HotSpot convention is that CAE makes
//           available to the caller the "failure witness", which is
//           the value that was stored in memory which did not match
//           the expected value.  If is_cae, the result is the value
//           most recently fetched from addr rather than a boolean
//           success indicator.
//
// Clobbers t0, t1
void ShenandoahBarrierSetAssembler::cmpxchg_oop(MacroAssembler* masm,
                                                Register addr,
                                                Register expected,
                                                Register new_val,
                                                Assembler::Aqrl acquire,
                                                Assembler::Aqrl release,
                                                bool is_cae,
                                                Register result) {
  bool is_narrow = UseCompressedOops;
  Assembler::operand_size size = is_narrow ? Assembler::uint32 : Assembler::int64;

  assert_different_registers(addr, expected, new_val, t0, t1);

  Label retry, success, fail, done;

  __ bind(retry);

  // Step1: Try to CAS.
  __ cmpxchg(addr, expected, new_val, size, acquire, release, /* result */ t1);

  // If success, then we are done.
  __ beq(expected, t1, success);

  // Step2: CAS failed, check the forwared pointer.
  __ mv(t0, t1);

  if (is_narrow) {
    __ decode_heap_oop(t0, t0);
  }
  resolve_forward_pointer(masm, t0);

  __ encode_heap_oop(t0, t0);

  // Report failure when the forwarded oop was not expected.
  __ bne(t0, expected, fail);

  // Step 3: CAS again using the forwarded oop.
  __ cmpxchg(addr, t1, new_val, size, acquire, release, /* result */ t0);

  // Retry when failed.
  __ bne(t0, t1, retry);

  __ bind(success);
  if (is_cae) {
    __ mv(result, expected);
  } else {
    __ addi(result, zr, 1);
  }
  __ j(done);

  __ bind(fail);
  if (is_cae) {
    __ mv(result, t0);
  } else {
    __ mv(result, zr);
  }

  __ bind(done);
}

#undef __

#ifdef COMPILER1

#define __ ce->masm()->

void ShenandoahBarrierSetAssembler::gen_pre_barrier_stub(LIR_Assembler* ce, ShenandoahPreBarrierStub* stub) {
  ShenandoahBarrierSetC1* bs = (ShenandoahBarrierSetC1*)BarrierSet::barrier_set()->barrier_set_c1();
  // At this point we know that marking is in progress.
  // If do_load() is true then we have to emit the
  // load of the previous value; otherwise it has already
  // been loaded into _pre_val.
  __ bind(*stub->entry());

  assert(stub->pre_val()->is_register(), "Precondition.");

  Register pre_val_reg = stub->pre_val()->as_register();

  if (stub->do_load()) {
    ce->mem2reg(stub->addr(), stub->pre_val(), T_OBJECT, stub->patch_code(),
                stub->info(), false /* wide */, false /* unaligned */);
  }
  __ beqz(pre_val_reg, *stub->continuation(), /* is_far */ true);
  ce->store_parameter(stub->pre_val()->as_register(), 0);
  __ far_call(RuntimeAddress(bs->pre_barrier_c1_runtime_code_blob()->code_begin()));
  __ j(*stub->continuation());
}

void ShenandoahBarrierSetAssembler::gen_load_reference_barrier_stub(LIR_Assembler* ce,
                                                                    ShenandoahLoadReferenceBarrierStub* stub) {

  Register obj = stub->obj()->as_register();
  Register res = stub->result()->as_register();

  Label done;

  __ bind(*stub->entry());

  if (res != obj) {
    __ mv(res, obj);
  }
  // Check for null.
  if (stub->needs_null_check()) {
    __ beqz(res, done);
  }

  load_reference_barrier_not_null(ce->masm(), res, t0);

  __ bind(done);
  __ j(*stub->continuation());
}

#undef __

#define __ sasm->

void ShenandoahBarrierSetAssembler::generate_c1_pre_barrier_runtime_stub(StubAssembler* sasm) {
  __ prologue("shenandoah_pre_barrier", false);

  // arg0 : previous value of memory

  BarrierSet* bs = BarrierSet::barrier_set();

  const Register pre_val = x10;
  const Register thread = xthread;
  const Register tmp = t0;

  Address queue_index(thread, in_bytes(ShenandoahThreadLocalData::satb_mark_queue_index_offset()));
  Address buffer(thread, in_bytes(ShenandoahThreadLocalData::satb_mark_queue_buffer_offset()));

  Label done;
  Label runtime;

  // Is marking still active?
  Address gc_state(thread, in_bytes(ShenandoahThreadLocalData::gc_state_offset()));
  __ lb(tmp, gc_state);
  __ andi(tmp, tmp, ShenandoahHeap::MARKING | ShenandoahHeap::TRAVERSAL);
  __ beqz(tmp, done);

  // Can we store original value in the thread's buffer?
  __ ld(tmp, queue_index);
  __ beqz(tmp, runtime);

  __ sub(tmp, tmp, wordSize);
  __ sd(tmp, queue_index);
  __ ld(t1, buffer);
  __ add(tmp, tmp, t1);
  __ load_parameter(0, t1);
  __ sd(t1, Address(tmp, 0));
  __ j(done);

  __ bind(runtime);
  __ push_call_clobbered_registers();
  __ load_parameter(0, pre_val);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::write_ref_field_pre_entry), pre_val, thread);
  __ pop_call_clobbered_registers();
  __ bind(done);

  __ epilogue();
}

#undef __

#endif // COMPILER1

address ShenandoahBarrierSetAssembler::shenandoah_lrb() {
  assert(_shenandoah_lrb != NULL, "need load reference barrier stub");
  return _shenandoah_lrb;
}

#define __ cgen->assembler()->

// Shenandoah load reference barrier.
//
// Input:
//   x10: OOP to evacuate.  Not null.
//
// Output:
//   x10: Pointer to evacuated OOP.
//
// Trash t0, t1.  Preserve everything else.
address ShenandoahBarrierSetAssembler::generate_shenandoah_lrb(StubCodeGenerator* cgen) {

  __ align(6);
  StubCodeMark mark(cgen, "StubRoutines", "shenandoah_lrb");
  address start = __ pc();

  Label work, done;
  __ mv(t1, ShenandoahHeap::in_cset_fast_test_addr());
  __ srli(t0, x10, ShenandoahHeapRegion::region_size_bytes_shift_jint());
  __ add(t1, t1, t0);
  __ lbu(t1, Address(t1));
  __ andi(t1, t1, 1);
  __ bnez(t1, work);
  __ ret();
  __ bind(work);

  __ mv(t1, x10);
  resolve_forward_pointer_not_null(cgen->assembler(), x10, t0);
  __ bne(t1, x10, done);

  __ enter(); // required for proper stackwalking of RuntimeStub frame

  __ push_call_clobbered_registers();

  __ li(lr, (int64_t)(intptr_t)ShenandoahRuntime::load_reference_barrier_JRT);
  __ jalr(lr);
  __ mv(t0, x10);
  __ pop_call_clobbered_registers();
  __ mv(x10, t0);

  __ leave(); // required for proper stackwalking of RuntimeStub frame
  __ bind(done);
  __ ret();

  return start;
}

#undef __

void ShenandoahBarrierSetAssembler::barrier_stubs_init() {
  if (ShenandoahLoadRefBarrier) {
    int stub_code_size = 2048;
    ResourceMark rm;
    BufferBlob* bb = BufferBlob::create("shenandoah_barrier_stubs", stub_code_size);
    CodeBuffer buf(bb);
    StubCodeGenerator cgen(&buf);
    _shenandoah_lrb = generate_shenandoah_lrb(&cgen);
  }
}
