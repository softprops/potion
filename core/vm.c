//
// vm.c
// the vm execution loop
//
// (c) 2008 why the lucky stiff, the freelance professor
//
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include "potion.h"
#include "internal.h"
#include "opcodes.h"
#include "asm.h"
#include "khash.h"
#include "table.h"

extern PNTarget potion_target_x86, potion_target_ppc;

PN potion_vm_proto(Potion *P, PN cl, PN self, ...) {
  PN ary = PN_NIL;
  vPN(Proto) f = (struct PNProto *)PN_CLOSURE(cl)->data[0];
  if (PN_IS_TUPLE(f->sig)) {
    va_list args;
    va_start(args, self);
    ary = PN_TUP0();
    PN_TUPLE_EACH(f->sig, i, v, {
      if (PN_IS_STR(v))
        ary = PN_PUSH(ary, va_arg(args, PN));
    });
    va_end(args);
  }
  return potion_vm(P, (PN)f, self, ary,
    PN_CLOSURE(cl)->extra - 1, &PN_CLOSURE(cl)->data[1]);
}

PN potion_vm_class(Potion *P, PN cl, PN self) {
  if (PN_TYPE(cl) == PN_TCLOSURE) {
    vPN(Proto) proto = PN_PROTO(PN_CLOSURE(cl)->data[0]);
    PN ivars = potion_tuple_with_size(P, PN_TUPLE_LEN(proto->paths));
    PN_TUPLE_EACH(proto->paths, i, v, {
      PN_TUPLE_AT(ivars, i) = PN_TUPLE_AT(proto->values, PN_INT(v));
    });
    return potion_class(P, cl, self, ivars);
  }

  return potion_class(P, PN_NIL, self, cl);
}

#define STACK_MAX 4096
#define JUMPS_MAX 1024

void potion_vm_init(Potion *P) {
  P->targets[POTION_X86] = potion_target_x86;
  P->targets[POTION_PPC] = potion_target_ppc;
}

#define CASE_OP(name, args) case OP_##name: target->op[OP_##name]args; break;

PN_F potion_jit_proto(Potion *P, PN proto, PN target_id) {
  long regs = 0, lregs = 0, need = 0, rsp = 0, argx = 0, protoargs = 4;
  PN_SIZE pos;
  PNJumps jmps[JUMPS_MAX]; size_t offs[JUMPS_MAX]; int jmpc = 0, jmpi = 0;
  vPN(Proto) f = (struct PNProto *)proto;
  int upc = PN_TUPLE_LEN(f->upvals);
  PNAsm * volatile asmb = potion_asm_new(P);
  u8 *fn;
  PNTarget *target = &P->targets[target_id];
  target->setup(P, f, &asmb);

  if (PN_TUPLE_LEN(f->protos) > 0) {
    PN_TUPLE_EACH(f->protos, i, proto2, {
      int p2args = 3;
      vPN(Proto) f2 = (struct PNProto *)proto2;
      // TODO: i'm repeating this a lot. sad.
      if (PN_IS_TUPLE(f2->sig)) {
        PN_TUPLE_EACH(f2->sig, i, v, {
          if (PN_IS_STR(v)) p2args++;
        });
      }
      if (f2->jit == NULL)
        potion_jit_proto(P, proto2, target_id);
      if (p2args > protoargs)
        protoargs = p2args;
    });
  }

  regs = PN_INT(f->stack);
  lregs = regs + PN_TUPLE_LEN(f->locals);
  need = lregs + upc + 3;
  rsp = (need + protoargs) * sizeof(PN);

  target->stack(P, f, &asmb, rsp);
  target->registers(P, f, &asmb, need);

  // Read locals
  if (PN_IS_TUPLE(f->sig)) {
    argx = 0;
    PN_TUPLE_EACH(f->sig, i, v, {
      if (PN_IS_STR(v)) {
        PN_SIZE num = PN_GET(f->locals, v);
        target->local(P, f, &asmb, regs + num, argx);
        argx++;
      }
    });
  }

  // if CL passed in with upvals, load them
  if (upc > 0)
    target->upvals(P, f, &asmb, lregs, need, upc);

  for (pos = 0; pos < PN_FLEX_SIZE(f->asmb) / sizeof(PN_OP); pos++) {
    offs[pos] = asmb->len;
    for (jmpi = 0; jmpi < jmpc; jmpi++) {
      if (jmps[jmpi].to == pos) {
        unsigned char *asmj = asmb->ptr + jmps[jmpi].from;
        target->jmpedit(P, f, &asmb, asmj, asmb->len - (jmps[jmpi].from + 4));
      }
    }

    switch (PN_OP_AT(f->asmb, pos).code) {
      CASE_OP(MOVE, (P, f, &asmb, pos))
      CASE_OP(LOADPN, (P, f, &asmb, pos)) 
      CASE_OP(LOADK, (P, f, &asmb, pos, need))
      CASE_OP(SELF, (P, f, &asmb, pos, need))
      CASE_OP(GETLOCAL, (P, f, &asmb, pos, regs))
      CASE_OP(SETLOCAL, (P, f, &asmb, pos, regs))
      CASE_OP(GETUPVAL, (P, f, &asmb, pos, lregs))
      CASE_OP(SETUPVAL, (P, f, &asmb, pos, lregs))
      CASE_OP(NEWTUPLE, (P, f, &asmb, pos, need))
      CASE_OP(SETTUPLE, (P, f, &asmb, pos, need))
      CASE_OP(SETTABLE, (P, f, &asmb, pos, need))
      CASE_OP(NEWLICK, (P, f, &asmb, pos, need))
      CASE_OP(GETPATH, (P, f, &asmb, pos, need))
      CASE_OP(SETPATH, (P, f, &asmb, pos, need))
      CASE_OP(ADD, (P, f, &asmb, pos, need))
      CASE_OP(SUB, (P, f, &asmb, pos, need))
      CASE_OP(MULT, (P, f, &asmb, pos, need))
      CASE_OP(DIV, (P, f, &asmb, pos, need))
      CASE_OP(REM, (P, f, &asmb, pos, need))
      CASE_OP(POW, (P, f, &asmb, pos, need))
      CASE_OP(NEQ, (P, f, &asmb, pos))
      CASE_OP(EQ, (P, f, &asmb, pos))
      CASE_OP(LT, (P, f, &asmb, pos))
      CASE_OP(LTE, (P, f, &asmb, pos))
      CASE_OP(GT, (P, f, &asmb, pos))
      CASE_OP(GTE, (P, f, &asmb, pos))
      CASE_OP(BITN, (P, f, &asmb, pos, need))
      CASE_OP(BITL, (P, f, &asmb, pos, need))
      CASE_OP(BITR, (P, f, &asmb, pos, need))
      CASE_OP(DEF, (P, f, &asmb, pos, need))
      CASE_OP(BIND, (P, f, &asmb, pos, need))
      CASE_OP(MESSAGE, (P, f, &asmb, pos, need))
      CASE_OP(JMP, (P, f, &asmb, pos, jmps, offs, &jmpc))
      CASE_OP(TEST, (P, f, &asmb, pos))
      CASE_OP(NOT, (P, f, &asmb, pos))
      CASE_OP(CMP, (P, f, &asmb, pos))
      CASE_OP(TESTJMP, (P, f, &asmb, pos, jmps, offs, &jmpc))
      CASE_OP(NOTJMP, (P, f, &asmb, pos, jmps, offs, &jmpc))
      CASE_OP(NAMED, (P, f, &asmb, pos, need))
      CASE_OP(CALL, (P, f, &asmb, pos, need))
      CASE_OP(CALLSET, (P, f, &asmb, pos, need))
      CASE_OP(RETURN, (P, f, &asmb, pos))
      CASE_OP(PROTO, (P, f, &asmb, &pos, lregs, need, regs))
      CASE_OP(CLASS, (P, f, &asmb, pos, need))
    }
  }

  target->finish(P, f, &asmb);

  fn = PN_ALLOC_FUNC(asmb->len);
#ifdef JIT_DEBUG
  printf("JIT(%p): ", fn);
  long ai = 0;
  for (ai = 0; ai < asmb->len; ai++) {
    printf("%x ", asmb->ptr[ai]);
  }
  printf("\n");
#endif
  PN_MEMCPY_N(fn, asmb->ptr, u8, asmb->len);

  return f->jit = (PN_F)fn;
}

#define PN_VM_MATH(name, oper) \
  if (PN_IS_NUM(reg[op.a]) && PN_IS_NUM(reg[op.b])) \
    reg[op.a] = PN_NUM(PN_INT(reg[op.a]) oper PN_INT(reg[op.b])); \
  else \
    reg[op.a] = potion_obj_##name(P, reg[op.a], reg[op.b]);

PN potion_vm(Potion *P, PN proto, PN self, PN vargs, PN_SIZE upc, PN *upargs) {
  vPN(Proto) f = (struct PNProto *)proto;

  // these variables persist as we jump around
  PN stack[STACK_MAX];
  PN val = PN_NIL;

  // these variables change from proto to proto
  // current = upvals | locals | self | reg
  PN_SIZE pos = 0;
  long argx = 0;
  PN *args = NULL, *upvals, *locals, *reg;
  PN *current = stack;

  if (vargs != PN_NIL) args = PN_GET_TUPLE(vargs)->set;
reentry:
  if (current - stack >= STACK_MAX) {
    fprintf(stderr, "all registers used up!");
    exit(1);
  }

  upvals = current;
  locals = upvals + f->upvalsize;
  reg = locals + f->localsize + 1;

  if (pos == 0) {
    reg[-1] = reg[0] = self;
    if (upc > 0 && upargs != NULL) {
      PN_SIZE i;
      for (i = 0; i < upc; i++) {
        upvals[i] = upargs[i];
      }
    }

    if (args != NULL) {
      argx = 0;
      PN_TUPLE_EACH(f->sig, i, v, {
        if (PN_IS_STR(v)) {
          PN_SIZE num = PN_GET(f->locals, v);
          locals[num] = args[argx++];
        }
      });
    }
  }

  while (pos < PN_OP_LEN(f->asmb)) {
    PN_OP op = PN_OP_AT(f->asmb, pos);
    switch (op.code) {
      case OP_MOVE:
        reg[op.a] = reg[op.b];
      break;
      case OP_LOADK:
        reg[op.a] = PN_TUPLE_AT(f->values, op.b);
      break;
      case OP_LOADPN:
        reg[op.a] = (PN)op.b;
      break;
      case OP_SELF:
        reg[op.a] = reg[-1];
      break;
      case OP_GETLOCAL:
        if (PN_IS_REF(locals[op.b]))
          reg[op.a] = PN_DEREF(locals[op.b]);
        else
          reg[op.a] = locals[op.b];
      break;
      case OP_SETLOCAL:
        if (PN_IS_REF(locals[op.b])) {
          PN_DEREF(locals[op.b]) = reg[op.a];
          PN_TOUCH(locals[op.b]);
        } else
          locals[op.b] = reg[op.a];
      break;
      case OP_GETUPVAL:
        reg[op.a] = PN_DEREF(upvals[op.b]);
      break;
      case OP_SETUPVAL:
        PN_DEREF(upvals[op.b]) = reg[op.a];
        PN_TOUCH(upvals[op.b]);
      break;
      case OP_NEWTUPLE:
        reg[op.a] = PN_TUP0();
      break;
      case OP_SETTUPLE:
        reg[op.a] = PN_PUSH(reg[op.a], reg[op.b]);
      break;
      case OP_SETTABLE:
        potion_table_set(P, reg[op.a], reg[op.a + 1], reg[op.b]);
      break;
      case OP_NEWLICK: {
        PN attr = op.b > op.a ? reg[op.a + 1] : PN_NIL;
        PN inner = op.b > op.a + 1 ? reg[op.b] : PN_NIL;
        reg[op.a] = potion_lick(P, reg[op.a], attr, inner);
      }
      break;
      case OP_GETPATH:
        reg[op.a] = potion_obj_get(P, PN_NIL, reg[op.a], reg[op.b]);
      break;
      case OP_SETPATH:
        potion_obj_set(P, PN_NIL, reg[op.a], reg[op.a + 1], reg[op.b]);
      break;
      case OP_ADD:
        PN_VM_MATH(add, +);
      break;
      case OP_SUB:
        PN_VM_MATH(sub, -);
      break;
      case OP_MULT:
        PN_VM_MATH(mult, *);
      break;
      case OP_DIV:
        PN_VM_MATH(div, /);
      break;
      case OP_REM:
        PN_VM_MATH(rem, %);
      break;
      case OP_POW:
        reg[op.a] = PN_NUM((int)pow((double)PN_INT(reg[op.a]),
          (double)PN_INT(reg[op.b])));
      break;
      case OP_NOT:
        reg[op.a] = PN_BOOL(!PN_TEST(reg[op.a]));
      break;
      case OP_CMP:
        reg[op.a] = PN_NUM(PN_INT(reg[op.b]) - PN_INT(reg[op.a]));
      break;
      case OP_NEQ:
        reg[op.a] = PN_BOOL(reg[op.a] != reg[op.b]);
      break;
      case OP_EQ:
        reg[op.a] = PN_BOOL(reg[op.a] == reg[op.b]);
      break;
      case OP_LT:
        reg[op.a] = PN_BOOL((long)(reg[op.a]) < (long)(reg[op.b]));
      break;
      case OP_LTE:
        reg[op.a] = PN_BOOL((long)(reg[op.a]) <= (long)(reg[op.b]));
      break;
      case OP_GT:
        reg[op.a] = PN_BOOL((long)(reg[op.a]) > (long)(reg[op.b]));
      break;
      case OP_GTE:
        reg[op.a] = PN_BOOL((long)(reg[op.a]) >= (long)(reg[op.b]));
      break;
      case OP_BITN:
        reg[op.a] = PN_IS_NUM(reg[op.b]) ? PN_NUM(~PN_INT(reg[op.b])) : potion_obj_bitn(P, reg[op.b]);
      break;
      case OP_BITL:
        PN_VM_MATH(bitl, <<);
      break;
      case OP_BITR:
        PN_VM_MATH(bitr, >>);
      break;
      case OP_DEF:
        reg[op.a] = potion_def_method(P, PN_NIL, reg[op.a], reg[op.a + 1], reg[op.b]);
      break;
      case OP_BIND:
        reg[op.a] = potion_bind(P, reg[op.b], reg[op.a]);
      break;
      case OP_MESSAGE:
        reg[op.a] = potion_message(P, reg[op.b], reg[op.a]);
      break;
      case OP_JMP:
        pos += op.a;
      break;
      case OP_TEST:
        reg[op.a] = PN_BOOL(PN_TEST(reg[op.a]));
      break;
      case OP_TESTJMP:
        if (PN_TEST(reg[op.a])) pos += op.b;
      break;
      case OP_NOTJMP:
        if (!PN_TEST(reg[op.a])) pos += op.b;
      break;
      case OP_NAMED: {
        int x = potion_sig_find(P, reg[op.a], reg[op.b - 1]);
        if (x >= 0) reg[op.a + x + 2] = reg[op.b];
      }
      break;
      case OP_CALL:
        switch (PN_TYPE(reg[op.a])) {
          case PN_TVTABLE:
            reg[op.a + 1] = potion_object_new(P, PN_NIL, reg[op.a]);
            reg[op.a] = ((struct PNVtable *)reg[op.a])->ctor;
          case PN_TCLOSURE:
            if (PN_CLOSURE(reg[op.a])->method != (PN_F)potion_vm_proto) {
              reg[op.a] = potion_call(P, reg[op.a], op.b - op.a, reg + op.a + 1);
            } else if (((reg - stack) + PN_INT(f->stack) + f->upvalsize + f->localsize + 8) >= STACK_MAX) {
              int i;
              PN argt = potion_tuple_with_size(P, (op.b - op.a) - 1);
              for (i = 2; i < op.b - op.a; i++)
                PN_TUPLE_AT(argt, i - 2) = reg[op.a + i];
              reg[op.a] = potion_vm(P, PN_CLOSURE(reg[op.a])->data[0], reg[op.a + 1], argt,
                PN_CLOSURE(reg[op.a])->extra - 1, &PN_CLOSURE(reg[op.a])->data[1]);
            } else {
              self = reg[op.a + 1];
              args = &reg[op.a + 2];
              upc = PN_CLOSURE(reg[op.a])->extra - 1;
              upargs = &PN_CLOSURE(reg[op.a])->data[1];
              current = reg + PN_INT(f->stack) + 2;
              current[-2] = (PN)f;
              current[-1] = (PN)pos;

              f = PN_PROTO(PN_CLOSURE(reg[op.a])->data[0]);
              pos = 0;
              goto reentry;
            }
          break;
          
          default: {
            reg[op.a + 1] = reg[op.a];
            reg[op.a] = potion_obj_get_call(P, reg[op.a]);
            if (PN_IS_CLOSURE(reg[op.a]))
              reg[op.a] = potion_call(P, reg[op.a], op.b - op.a, &reg[op.a + 1]);
          }
          break;
        }
      break;
      case OP_CALLSET:
        reg[op.a] = potion_obj_get_callset(P, reg[op.b]);
      break;
      case OP_RETURN:
        if (current != stack) {
          val = reg[op.a];

          f = PN_PROTO(current[-2]);
          pos = (PN_SIZE)current[-1];
          op = PN_OP_AT(f->asmb, pos);

          reg = current - (PN_INT(f->stack) + 2);
          current = reg - (f->localsize + f->upvalsize + 1);
          reg[op.a] = val;
          pos++;
          goto reentry;
        } else {
          reg[0] = reg[op.a];
          goto done;
        }
      break;
      case OP_PROTO: {
        vPN(Closure) cl;
        unsigned areg = op.a;
        proto = PN_TUPLE_AT(f->protos, op.b);
        cl = (struct PNClosure *)potion_closure_new(P, (PN_F)potion_vm_proto,
          PN_PROTO(proto)->sig, PN_TUPLE_LEN(PN_PROTO(proto)->upvals) + 1);
        cl->data[0] = proto;
        PN_TUPLE_COUNT(PN_PROTO(proto)->upvals, i, {
          pos++;
          op = PN_OP_AT(f->asmb, pos);

          if (op.code == OP_GETUPVAL) {
            cl->data[i+1] = upvals[op.b];
          } else if (op.code == OP_GETLOCAL) {
            cl->data[i+1] = locals[op.b] = (PN)potion_ref(P, locals[op.b]);
          } else {
            fprintf(stderr, "** missing an upval to proto %p\n", (void *)proto);
          }
        });
        reg[areg] = (PN)cl;
      }
      break;
      case OP_CLASS:
        reg[op.a] = potion_vm_class(P, reg[op.b], reg[op.a]);
      break;
    }
    pos++;
  }

done:
  val = reg[0];
  return val;
}
