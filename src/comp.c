/* Compile byte code produced by bytecomp.el into native code.
   Copyright (C) 2019 Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

#include <config.h>

#ifdef HAVE_LIBGCCJIT

#include <stdlib.h>
#include <stdio.h>
#include <libgccjit.h>

#include "lisp.h"
#include "buffer.h"
#include "bytecode.h"
#include "atimer.h"
#include "window.h"

#define DEFAULT_SPEED 2 /* From 0 to 3 map to gcc -O */

#define COMP_DEBUG 1

#define MAX_FUN_NAME 256

/* Max number of args we are able to handle while emitting function calls.  */

#define MAX_ARGS 16

#define DISASS_FILE_NAME "emacs-asm.s"

#define CHECK_STACK					\
  eassert (stack >= stack_base && stack < stack_over)

#define PUSH_LVAL(obj)							\
  do {									\
    CHECK_STACK;							\
    gcc_jit_block_add_assignment (comp.bblock->gcc_bb,			\
				  NULL,					\
				  *stack,				\
				  gcc_jit_lvalue_as_rvalue(obj));	\
    stack++;								\
  } while (0)

#define PUSH_RVAL(obj)					\
  do {							\
    CHECK_STACK;					\
    gcc_jit_block_add_assignment (comp.bblock->gcc_bb,	\
				  NULL,			\
				  *stack,		\
				  (obj));		\
    stack++;						\
  } while (0)

/* This always happens in the first basic block.  */

#define PUSH_PARAM(obj)							\
  do {									\
    CHECK_STACK;							\
    gcc_jit_block_add_assignment (prologue_bb,				\
				  NULL,					\
				  *stack,				\
				  gcc_jit_param_as_rvalue(obj));	\
    stack++;								\
  } while (0)

#define TOS (*(stack - 1))

#define POP0

#define POP1						\
  do {							\
    stack--;						\
    CHECK_STACK;					\
    args[0] = gcc_jit_lvalue_as_rvalue (*stack);	\
  } while (0)

#define POP2						\
  do {							\
    stack--;						\
    CHECK_STACK;					\
    args[1] = gcc_jit_lvalue_as_rvalue (*stack);	\
    stack--;						\
    args[0] = gcc_jit_lvalue_as_rvalue (*stack);	\
  } while (0)

#define POP3						\
  do {							\
    stack--;						\
    CHECK_STACK;					\
    args[2] = gcc_jit_lvalue_as_rvalue (*stack);	\
    stack--;						\
    args[1] = gcc_jit_lvalue_as_rvalue (*stack);	\
    stack--;						\
    args[0] = gcc_jit_lvalue_as_rvalue (*stack);	\
  } while (0)

/* Fetch the next byte from the bytecode stream.  */

#define FETCH (bytestr_data[pc++])

/* Fetch two bytes from the bytecode stream and make a 16-bit number
   out of them.  */

#define FETCH2 (op = FETCH, op + (FETCH << 8))

#define STR(s) #s

/* With most of the ops we need to do the same stuff so this macros are meant
   to save some typing.  */

/* Generate appropriate case and emit convential calls to function. */

#define CASE_CALL_NARGS(name, nargs)					\
  case B##name:								\
  POP##nargs;								\
  res = comp_emit_call (STR(F##name), comp.lisp_obj_type, nargs, args);	\
  PUSH_LVAL (res);							\
  break

/* Emit calls to functions with prototype (ptrdiff_t nargs, Lisp_Object *args)
   This is done aggregating args into the scratch_call_area.  */

#define EMIT_SCRATCH_CALL_N(name, nargs)	\
  do {						\
    pop (nargs, &stack, args);			\
    res = comp_emit_callN (name, nargs, args);	\
    PUSH_LVAL (res);				\
  } while (0)

#define EMIT_ARITHCOMPARE(comparison)					\
  do {									\
    POP2;								\
    args[2] = gcc_jit_context_new_rvalue_from_int (comp.ctxt,		\
						   comp.int_type,	\
						   comparison);		\
    res = comp_emit_call ("arithcompare", comp.lisp_obj_type, 3, args);	\
    PUSH_LVAL (res);							\
  } while (0)


typedef struct {
  gcc_jit_block *gcc_bb;
  bool terminated;
} basic_block_t;

/* The compiler context  */

typedef struct {
  gcc_jit_context *ctxt;
  gcc_jit_type *void_type;
  gcc_jit_type *bool_type;
  gcc_jit_type *int_type;
  gcc_jit_type *unsigned_type;
  gcc_jit_type *long_type;
  gcc_jit_type *long_long_type;
  gcc_jit_type *void_ptr_type;
  gcc_jit_type *ptrdiff_type;
  gcc_jit_type *lisp_obj_type;
  gcc_jit_field *lisp_obj_as_ptr;
  gcc_jit_field *lisp_obj_as_num;
  /* libgccjit has really limited support for casting therefore this union will
     be used for the scope.  */
  gcc_jit_type *cast_union_type;
  gcc_jit_field *cast_union_as_ll;
  gcc_jit_field *cast_union_as_u;
  gcc_jit_field *cast_union_as_i;
  gcc_jit_field *cast_union_as_b;
  gcc_jit_function *func; /* Current function being compiled  */
  gcc_jit_rvalue *scratch; /* Will point to scratch_call_area  */
  gcc_jit_rvalue *most_positive_fixnum;
  gcc_jit_rvalue *most_negative_fixnum;
  gcc_jit_rvalue *one;
  gcc_jit_rvalue *inttypebits;
  gcc_jit_rvalue *lisp_int0;
  basic_block_t *bblock; /* Current basic block  */
  Lisp_Object func_hash; /* f_name -> gcc_func  */
} comp_t;

static comp_t comp;

Lisp_Object scratch_call_area[MAX_ARGS];

FILE *logfile = NULL;

/* The result of one function compilation.  */

typedef struct {
  gcc_jit_result *gcc_res;
  short min_args, max_args;
} comp_f_res_t;

void emacs_native_compile (const char *lisp_f_name, const char *c_f_name,
			   Lisp_Object func, int opt_level, bool dump_asm);


static void
bcall0 (Lisp_Object f)
{
  Ffuncall (1, &f);
}

/* Pop form the main evaluation stack and place the elements in args in reversed
   order.  */

INLINE static void
pop (unsigned n, gcc_jit_lvalue ***stack_ref, gcc_jit_rvalue *args[])
{
  gcc_jit_lvalue **stack = *stack_ref;

  while (n--)
    {
      stack--;
      args[n] = gcc_jit_lvalue_as_rvalue (*stack);
    }

  *stack_ref = stack;
}

INLINE static gcc_jit_field *
type_to_cast_field (gcc_jit_type *type)
{
  gcc_jit_field *field;

  if (type == comp.long_long_type)
    field = comp.cast_union_as_ll;
  else if (type == comp.unsigned_type)
    field = comp.cast_union_as_u;
  else if (type == comp.int_type)
    field = comp.cast_union_as_i;
  else if (type == comp.bool_type)
    field = comp.cast_union_as_b;
  else
    error ("unsopported cast\n");

  return field;
}

static gcc_jit_rvalue *
comp_cast (gcc_jit_type *new_type, gcc_jit_rvalue *obj)
{
  gcc_jit_field *orig_field =
    type_to_cast_field (gcc_jit_rvalue_get_type (obj));
  gcc_jit_field *dest_field = type_to_cast_field (new_type);

  gcc_jit_lvalue *tmp_u =
    gcc_jit_function_new_local (comp.func,
				NULL,
				comp.cast_union_type,
				"union_cast");
  gcc_jit_block_add_assignment (comp.bblock->gcc_bb,
				NULL,
				gcc_jit_lvalue_access_field (tmp_u,
							     NULL,
							     orig_field),
				obj);

  return gcc_jit_rvalue_access_field ( gcc_jit_lvalue_as_rvalue (tmp_u),
				       NULL,
				       dest_field);
}

INLINE static gcc_jit_rvalue *
comp_XLI (gcc_jit_rvalue *obj)
{
  return gcc_jit_rvalue_access_field (obj,
				      NULL,
				      comp.lisp_obj_as_num);
}

static gcc_jit_rvalue *
comp_TAGGEDP (gcc_jit_rvalue *obj, unsigned tag)
{
   /* (! (((unsigned) (XLI (a) >> (USE_LSB_TAG ? 0 : VALBITS)) \
	- (unsigned) (tag)) \
	& ((1 << GCTYPEBITS) - 1))) */

  gcc_jit_rvalue *sh_res =
    gcc_jit_context_new_binary_op (
      comp.ctxt,
      NULL,
      GCC_JIT_BINARY_OP_RSHIFT,
      comp.long_long_type,
      comp_XLI (obj),
      gcc_jit_context_new_rvalue_from_int (comp.ctxt,
					   comp.long_long_type,
					   (USE_LSB_TAG ? 0 : VALBITS)));

  gcc_jit_rvalue *minus_res =
    gcc_jit_context_new_binary_op (comp.ctxt,
				   NULL,
				   GCC_JIT_BINARY_OP_MINUS,
				   comp.unsigned_type,
				   comp_cast (comp.unsigned_type, sh_res),
				   gcc_jit_context_new_rvalue_from_int (
				     comp.ctxt,
				     comp.unsigned_type,
				     tag));

  gcc_jit_rvalue *res =
   gcc_jit_context_new_unary_op (
     comp.ctxt,
     NULL,
     GCC_JIT_UNARY_OP_LOGICAL_NEGATE,
     comp.int_type,
     gcc_jit_context_new_binary_op (comp.ctxt,
				    NULL,
				    GCC_JIT_BINARY_OP_BITWISE_AND,
				    comp.unsigned_type,
				    minus_res,
				    gcc_jit_context_new_rvalue_from_int (
				      comp.ctxt,
				      comp.unsigned_type,
				      ((1 << GCTYPEBITS) - 1))));

  return res;
}

static gcc_jit_rvalue *
comp_CONSP (gcc_jit_rvalue *obj)
{
  return comp_TAGGEDP(obj, Lisp_Cons);
}

static gcc_jit_rvalue *
comp_FIXNUMP (gcc_jit_rvalue *obj)
{
  /* (! (((unsigned) (XLI (x) >> (USE_LSB_TAG ? 0 : FIXNUM_BITS))
        - (unsigned) (Lisp_Int0 >> !USE_LSB_TAG))
	& ((1 << INTTYPEBITS) - 1)))  */

  gcc_jit_rvalue *sh_res =
    gcc_jit_context_new_binary_op (
      comp.ctxt,
      NULL,
      GCC_JIT_BINARY_OP_RSHIFT,
      comp.long_long_type,
      comp_XLI (obj),
      gcc_jit_context_new_rvalue_from_int (comp.ctxt,
					   comp.long_long_type,
					   (USE_LSB_TAG ? 0 : FIXNUM_BITS)));

  gcc_jit_rvalue *minus_res =
    gcc_jit_context_new_binary_op (comp.ctxt,
				   NULL,
				   GCC_JIT_BINARY_OP_MINUS,
				   comp.unsigned_type,
				   comp_cast (comp.unsigned_type, sh_res),
				   gcc_jit_context_new_rvalue_from_int (
				     comp.ctxt,
				     comp.unsigned_type,
				     (Lisp_Int0 >> !USE_LSB_TAG)));

  gcc_jit_rvalue *res =
   gcc_jit_context_new_unary_op (
     comp.ctxt,
     NULL,
     GCC_JIT_UNARY_OP_LOGICAL_NEGATE,
     comp.int_type,
     gcc_jit_context_new_binary_op (comp.ctxt,
				    NULL,
				    GCC_JIT_BINARY_OP_BITWISE_AND,
				    comp.unsigned_type,
				    minus_res,
				    gcc_jit_context_new_rvalue_from_int (
				      comp.ctxt,
				      comp.unsigned_type,
				      ((1 << INTTYPEBITS) - 1))));

  return res;
}

static gcc_jit_rvalue *
comp_XFIXNUM (gcc_jit_rvalue *obj)
{
  return gcc_jit_context_new_binary_op (comp.ctxt,
					NULL,
					GCC_JIT_BINARY_OP_RSHIFT,
					comp.long_long_type,
					comp_XLI (obj),
					comp.inttypebits);
}

static gcc_jit_rvalue *
comp_make_fixnum (gcc_jit_block *block, gcc_jit_rvalue *obj)
{
  gcc_jit_rvalue *tmp =
    gcc_jit_context_new_binary_op (comp.ctxt,
				   NULL,
				   GCC_JIT_BINARY_OP_LSHIFT,
				   comp.long_long_type,
				   obj,
				   comp.inttypebits);

  tmp = gcc_jit_context_new_binary_op (comp.ctxt,
				       NULL,
				       GCC_JIT_BINARY_OP_PLUS,
				       comp.long_long_type,
				       tmp,
				       comp.lisp_int0);

  gcc_jit_lvalue *res = gcc_jit_function_new_local (comp.func,
						    NULL,
						    comp.lisp_obj_type,
						    "lisp_obj_fixnum");

  gcc_jit_block_add_assignment (block,
				NULL,
				gcc_jit_lvalue_access_field (
				  res,
				  NULL,
				  comp.lisp_obj_as_num),
				tmp);

  return gcc_jit_lvalue_as_rvalue (res);
}

/* Construct fill and return a lisp object form a raw pointer.  */

static gcc_jit_rvalue *
comp_lisp_obj_as_ptr_from_ptr (basic_block_t *bblock, void *p)
{
  static unsigned i;
  char ptr_var_name[40];

  int res = snprintf (ptr_var_name, sizeof (ptr_var_name),
		      "lisp_obj_from_ptr_%u", i++);
  if (res >= sizeof (ptr_var_name))
    error ("Internal error, truncating temporary variable");

  gcc_jit_lvalue *lisp_obj = gcc_jit_function_new_local (comp.func,
							 NULL,
							 comp.lisp_obj_type,
							 ptr_var_name);
  gcc_jit_lvalue *lisp_obj_as_ptr =
    gcc_jit_lvalue_access_field (lisp_obj,
				 NULL,
				 comp.lisp_obj_as_ptr);

  gcc_jit_rvalue *void_ptr =
    gcc_jit_context_new_rvalue_from_ptr(comp.ctxt,
					comp.void_ptr_type,
					p);

  gcc_jit_block_add_assignment (bblock->gcc_bb,
				NULL,
				lisp_obj_as_ptr,
				void_ptr);
  return gcc_jit_lvalue_as_rvalue (lisp_obj);
}

static gcc_jit_function *
comp_func_declare (const char *f_name, gcc_jit_type *ret_type,
		   unsigned nargs, gcc_jit_rvalue **args,
		   enum  gcc_jit_function_kind kind, bool reusable)
{
  gcc_jit_param *param[4];
  gcc_jit_type *type[4];

  /* If args are passed types are extracted from that otherwise assume params */
  /* are all lisp objs.  */
  if (args)
    for (int i = 0; i < nargs; i++)
      type[i] = gcc_jit_rvalue_get_type (args[i]);
  else
    for (int i = 0; i < nargs; i++)
      type[i] = comp.lisp_obj_type;

  switch (nargs) {
  case 4:
    param[3] = gcc_jit_context_new_param(comp.ctxt,
					 NULL,
					 type[3],
					 "c");
    /* Fall through */
    FALLTHROUGH;
  case 3:
    param[2] = gcc_jit_context_new_param(comp.ctxt,
					 NULL,
					 type[2],
					 "c");
    /* Fall through */
    FALLTHROUGH;
  case 2:
    param[1] = gcc_jit_context_new_param(comp.ctxt,
					 NULL,
					 type[1],
					 "b");
    /* Fall through */
    FALLTHROUGH;
  case 1:
    param[0] = gcc_jit_context_new_param(comp.ctxt,
					 NULL,
					 type[0],
					 "a");
    /* Fall through */
    FALLTHROUGH;
  case 0:
    break;
  default:
    /* Argnum not supported  */
    eassert (0);
  }

  gcc_jit_function *func =
    gcc_jit_context_new_function(comp.ctxt, NULL,
				 kind,
				 comp.lisp_obj_type,
				 f_name,
				 nargs,
				 param,
				 0);

  if (reusable)
    {
      Lisp_Object value;
      Lisp_Object key = make_string (f_name, strlen (f_name));
      value = make_pointer_integer (XPL (func));

      EMACS_UINT hash = 0;
      struct Lisp_Hash_Table *ht = XHASH_TABLE (comp.func_hash);
      ptrdiff_t i = hash_lookup (ht, key, &hash);
      /* Don't want to declare the same function two times */
      eassert (i == -1);
      hash_put (ht, key, value, hash);
    }

  return func;
}

static gcc_jit_lvalue *
comp_emit_call (const char *f_name, gcc_jit_type *ret_type, unsigned nargs,
		gcc_jit_rvalue **args)
{
  Lisp_Object key = make_string (f_name, strlen (f_name));
  EMACS_UINT hash = 0;
  struct Lisp_Hash_Table *ht = XHASH_TABLE (comp.func_hash);
  ptrdiff_t i = hash_lookup (ht, key, &hash);

  if (i == -1)
    {
      comp_func_declare(f_name, ret_type, nargs, args, GCC_JIT_FUNCTION_IMPORTED,
			true);
      i = hash_lookup (ht, key, &hash);
      eassert (i != -1);
    }

  Lisp_Object value = HASH_VALUE (ht, hash_lookup (ht, key, &hash));
  gcc_jit_function *func = (gcc_jit_function *) XFIXNUMPTR (value);

  gcc_jit_lvalue *res = gcc_jit_function_new_local(comp.func,
						   NULL,
						   ret_type,
						   "res");
  gcc_jit_block_add_assignment(comp.bblock->gcc_bb, NULL,
			       res,
			       gcc_jit_context_new_call(comp.ctxt,
							NULL,
							func,
							nargs,
							args));
  return res;
}

static gcc_jit_lvalue *
comp_emit_callN (const char *f_name, unsigned nargs, gcc_jit_rvalue **args)
{
  /* Here we set all the pointers into the scratch call area.  */
  /* TODO: distinguish primitives for faster calling convention.  */

  /*
    Lisp_Object *p;
    p = scratch_call_area;

    p[0] = nargs;
    p[1] = 0x...;
    .
    .
    .
    p[n] = 0x...;
  */

  gcc_jit_lvalue *p =
    gcc_jit_function_new_local(comp.func,
			       NULL,
			       gcc_jit_type_get_pointer (comp.lisp_obj_type),
			       "p");

  gcc_jit_block_add_assignment(comp.bblock->gcc_bb, NULL,
			       p,
			       comp.scratch);

  for (int i = 0; i < nargs; i++) {
    gcc_jit_rvalue *idx =
      gcc_jit_context_new_rvalue_from_int (comp.ctxt,
					   gcc_jit_context_get_type(comp.ctxt,
								    GCC_JIT_TYPE_UNSIGNED_INT),
					   i);
    gcc_jit_block_add_assignment (comp.bblock->gcc_bb, NULL,
				  gcc_jit_context_new_array_access (comp.ctxt,
								    NULL,
								    gcc_jit_lvalue_as_rvalue(p),
								    idx),
				  args[i]);
  }

  args[0] = gcc_jit_context_new_rvalue_from_int(comp.ctxt,
						comp.ptrdiff_type,
						nargs);
  args[1] = comp.scratch;

  return comp_emit_call (f_name, comp.lisp_obj_type, 2, args);
}

static int
ucmp(const void *a, const void *b)
{
#define _I(x) *(const int*)x
  return _I(a) < _I(b) ? -1 : _I(a) > _I(b);
#undef _I
}

/* Compute and initialize all basic blocks.  */
static basic_block_t *
compute_bblocks (ptrdiff_t bytestr_length, unsigned char *bytestr_data)
{
  ptrdiff_t pc = 0;
  unsigned op;
  bool new_bb = true;
  basic_block_t *bb_map = xmalloc (bytestr_length * sizeof (basic_block_t));
  unsigned *bb_start_pc = xmalloc (bytestr_length * sizeof (unsigned));
  unsigned bb_n = 0;

  while (pc < bytestr_length)
    {
      if (new_bb)
	{
	  bb_start_pc[bb_n++] = pc;
	  new_bb = false;
	}

      op = FETCH;
      switch (op)
	{
	  /* 3 byte non branch ops */
	case Bvarref7:
	case Bvarset7:
	case Bvarbind7:
	case Bcall7:
	case Bunbind7:
	case Bpushcatch:
	case Bpushconditioncase:
	case Bstack_ref7:
	case Bstack_set2:
	  pc += 2;
	  break;
	  /* 2 byte non branch ops */
	case Bvarref6:
	case Bvarset6:
	case Bvarbind6:
	case Bcall6:
	case Bunbind6:
	case Bconstant2:
	case BlistN:
	case BconcatN:
	case BinsertN:
	case Bstack_ref6:
	case Bstack_set:
	case BdiscardN:
	  ++pc;
	  break;
	  /* Absolute branches */
	case Bgoto:
	case Bgotoifnil:
	case Bgotoifnonnil:
	case Bgotoifnilelsepop:
	case Bgotoifnonnilelsepop:
	  op = FETCH2;
	  bb_start_pc[bb_n++] = op;
	  new_bb = true;
	  break;
	  /* PC relative branches */
	case BRgoto:
	case BRgotoifnil:
	case BRgotoifnonnil:
	case BRgotoifnilelsepop:
	case BRgotoifnonnilelsepop:
	  op = FETCH - 128;
	  bb_start_pc[bb_n++] = op;
	  new_bb = true;
	  break;
	case Bsub1:
	case Badd1:
	case Bnegate:
	case Breturn:
	  new_bb = true;
	  break;
	default:
	  break;
	}
    }

  /* Sort and remove possible duplicates.  */
  qsort (bb_start_pc, bb_n, sizeof(unsigned), ucmp);
  {
    unsigned i, j;
    for (i = j = 0; i < bb_n; i++)
      if (bb_start_pc[i] != bb_start_pc[j])
	bb_start_pc[++j] = bb_start_pc[i];
    bb_n = j + 1;
  }

  basic_block_t curr_bb;
  for (int i = 0, pc = 0; pc < bytestr_length; pc++)
    {
      if (i < bb_n && pc == bb_start_pc[i])
	{
	  ++i;
	  curr_bb.gcc_bb = gcc_jit_function_new_block (comp.func, NULL);
	  curr_bb.terminated = false;
	}
      bb_map[pc] = curr_bb;
    }

  xfree (bb_start_pc);

  return bb_map;
}

/* Close current basic block emitting a conditional.  */

INLINE static void
comp_emit_cond_jump (gcc_jit_rvalue *test,
		       gcc_jit_block *then_target, gcc_jit_block *else_target)
{
  gcc_jit_block_end_with_conditional (comp.bblock->gcc_bb,
				      NULL,
				      test,
				      then_target,
				      else_target);
  comp.bblock->terminated = true;
}

/* Close current basic block emitting a comparison between two rval.  */

static gcc_jit_rvalue *
comp_emit_comp_jump (enum gcc_jit_comparison op,
		     gcc_jit_rvalue *a, gcc_jit_rvalue *b,
		     gcc_jit_block *then_target, gcc_jit_block *else_target)
{
  gcc_jit_rvalue *test = gcc_jit_context_new_comparison (comp.ctxt,
							 NULL,
							 op,
							 a, b);

  comp_emit_cond_jump (test, then_target, else_target);

  return test;
}

static comp_f_res_t
compile_f (const char *f_name, ptrdiff_t bytestr_length,
	   unsigned char *bytestr_data,
	   EMACS_INT stack_depth, Lisp_Object *vectorp,
	   ptrdiff_t vector_size, Lisp_Object args_template)
{
  gcc_jit_lvalue *res;
  comp_f_res_t comp_res = { NULL, 0, 0 };
  ptrdiff_t pc = 0;
  gcc_jit_rvalue *args[4];
  unsigned op;

  /* Meta-stack we use to flat the bytecode written for push and pop
     Emacs VM.*/
  gcc_jit_lvalue **stack_base, **stack, **stack_over;
  stack_base = stack =
    (gcc_jit_lvalue **) xmalloc (stack_depth * sizeof (gcc_jit_lvalue *));
  stack_over = stack_base + stack_depth;

  if (FIXNUMP (args_template))
    {
      ptrdiff_t at = XFIXNUM (args_template);
      bool rest = (at & 128) != 0;
      int mandatory = at & 127;
      ptrdiff_t nonrest = at >> 8;

      comp_res.min_args = mandatory;

      eassert (!rest);

      if (!rest && nonrest < SUBR_MAX_ARGS)
	comp_res.max_args = nonrest;
    }
  else if (CONSP (args_template))
    /* FIXME */
    comp_res.min_args = comp_res.max_args = XFIXNUM (Flength (args_template));

  else
    eassert (SYMBOLP (args_template) && args_template == Qnil);


  /* Current function being compiled.  */
  comp.func = comp_func_declare (f_name, comp.lisp_obj_type, comp_res.max_args,
				 NULL, GCC_JIT_FUNCTION_EXPORTED, false);

  char local_name[256];
  for (int i = 0; i < stack_depth; ++i)
    {
      snprintf (local_name, sizeof (local_name), "local_%d", i);
      stack[i] = gcc_jit_function_new_local (comp.func,
					     NULL,
					     comp.lisp_obj_type,
					     local_name);
    }

  gcc_jit_block *prologue_bb =
    gcc_jit_function_new_block (comp.func, "prologue");

  basic_block_t *bb_map = compute_bblocks (bytestr_length, bytestr_data);

  for (ptrdiff_t i = 0; i < comp_res.max_args; ++i)
    PUSH_PARAM (gcc_jit_function_get_param (comp.func, i));
  gcc_jit_block_end_with_jump (prologue_bb, NULL, bb_map[0].gcc_bb);

  gcc_jit_rvalue *nil = comp_lisp_obj_as_ptr_from_ptr (&bb_map[0], Qnil);

  comp.bblock = NULL;

  while (pc < bytestr_length)
    {
      /* If we are changing BB and the last was one wasn't terminated
	 terminate it with a fall through.  */
      if (comp.bblock && comp.bblock->gcc_bb != bb_map[pc].gcc_bb &&
	  !comp.bblock->terminated)
	{
	  gcc_jit_block_end_with_jump (comp.bblock->gcc_bb, NULL, bb_map[pc].gcc_bb);
	  comp.bblock->terminated = true;
	}
      comp.bblock = &bb_map[pc];
      op = FETCH;

      switch (op)
	{
	case Bstack_ref1:
	case Bstack_ref2:
	case Bstack_ref3:
	case Bstack_ref4:
	case Bstack_ref5:
	  {
	    PUSH_LVAL (stack_base[(stack - stack_base) - (op - Bstack_ref) - 1]);
	    break;
	  }
	case Bstack_ref6:
	  {
	    PUSH_LVAL (stack_base[(stack - stack_base) - FETCH - 1]);
	    break;
	  }
	case Bstack_ref7:
	  {
	    PUSH_LVAL (stack_base[(stack - stack_base) - FETCH2 - 1]);
	    break;
	  }

	case Bvarref7:
	  op = FETCH2;
	  goto varref;

	case Bvarref:
	case Bvarref1:
	case Bvarref2:
	case Bvarref3:
	case Bvarref4:
	case Bvarref5:
	  op -= Bvarref;
	  goto varref;

	case Bvarref6:
	  op = FETCH;
	varref:
	  {
	    args[0] = comp_lisp_obj_as_ptr_from_ptr (comp.bblock, vectorp[op]);
	    res = comp_emit_call ("Fsymbol_value", comp.lisp_obj_type, 1, args);
	    PUSH_LVAL (res);
	    break;
	  }

	case Bvarset:
	case Bvarset1:
	case Bvarset2:
	case Bvarset3:
	case Bvarset4:
	case Bvarset5:
	  op -= Bvarset;
	  goto varset;

	case Bvarset7:
	  op = FETCH2;
	  goto varset;

	case Bvarset6:
	  op = FETCH;
	varset:
	  {
	    POP1;
	    args[1] = args[0];
	    args[0] = comp_lisp_obj_as_ptr_from_ptr (comp.bblock, vectorp[op]);
	    args[2] = nil;
	    args[3] = gcc_jit_context_new_rvalue_from_int (comp.ctxt,
							   comp.int_type,
							   SET_INTERNAL_SET);
	    res = comp_emit_call ("set_internal", comp.lisp_obj_type, 4, args);
	    PUSH_LVAL (res);
	  }
	  break;

	case Bvarbind6:
	  op = FETCH;
	  goto varbind;

	case Bvarbind7:
	  op = FETCH2;
	  goto varbind;

	case Bvarbind:
	case Bvarbind1:
	case Bvarbind2:
	case Bvarbind3:
	case Bvarbind4:
	case Bvarbind5:
	  op -= Bvarbind;
	varbind:
	  {
	    args[0] = comp_lisp_obj_as_ptr_from_ptr (comp.bblock, vectorp[op]);
	    pop (1, &stack, &args[1]);
	    res = comp_emit_call ("specbind", comp.lisp_obj_type, 2, args);
	    PUSH_LVAL (res);
	    break;
	  }

	case Bcall6:
	  op = FETCH;
	  goto docall;

	case Bcall7:
	  op = FETCH2;
	  goto docall;

	case Bcall:
	case Bcall1:
	case Bcall2:
	case Bcall3:
	case Bcall4:
	case Bcall5:
	  op -= Bcall;
	docall:
	  {
	    ptrdiff_t nargs = op + 1;
	    pop (nargs, &stack, args);
	    res = comp_emit_callN ("Ffuncall", nargs, args);
	    PUSH_LVAL (res);
	    break;
	  }

	case Bunbind6:
	  op = FETCH;
	  goto dounbind;

	case Bunbind7:
	  op = FETCH2;
	  goto dounbind;

	case Bunbind:
	case Bunbind1:
	case Bunbind2:
	case Bunbind3:
	case Bunbind4:
	case Bunbind5:
	  op -= Bunbind;
	dounbind:
	  {
	    args[0] = gcc_jit_context_new_rvalue_from_int(comp.ctxt,
							  comp.ptrdiff_type,
							  op);

	    comp_emit_call ("helper_unbind_n", comp.lisp_obj_type, 1, args);
	  }
	  break;
	case Bpophandler:
	  error ("Bpophandler unsupported bytecode\n");
	  break;
	case Bpushconditioncase:
	  error ("Bpushconditioncase unsupported bytecode\n");
	  break;
	case Bpushcatch:
	  error ("Bpushcatch unsupported bytecode\n");
	  break;

	CASE_CALL_NARGS (nth, 2);
	CASE_CALL_NARGS (symbolp, 1);

	case Bconsp:
	  gcc_jit_block_add_assignment (
	    comp.bblock->gcc_bb,
	    NULL,
	    TOS,
	    comp_CONSP(gcc_jit_lvalue_as_rvalue (TOS)));
	  break;

	CASE_CALL_NARGS (stringp, 1);
	CASE_CALL_NARGS (listp, 1);
	CASE_CALL_NARGS (eq, 2);
	CASE_CALL_NARGS (memq, 1);
	CASE_CALL_NARGS (not, 1);
	CASE_CALL_NARGS (car, 1);
	CASE_CALL_NARGS (cdr, 1);
	CASE_CALL_NARGS (cons, 2);

	case BlistN:
	  op = FETCH;
	  goto make_list;

	case Blist1:
	case Blist2:
	case Blist3:
	case Blist4:
	  op = op - Blist1;
	make_list:
	  {
	    POP1;
	    args[1] = nil;
	    res = comp_emit_call ("Fcons", comp.lisp_obj_type, 2, args);
	    PUSH_LVAL (res);
	    for (int i = 0; i < op; ++i)
	      {
		POP2;
		res = comp_emit_call ("Fcons", comp.lisp_obj_type, 2, args);
		PUSH_LVAL (res);
	      }
	    break;
	  }

	CASE_CALL_NARGS (length, 1);
	CASE_CALL_NARGS (aref, 2);
	CASE_CALL_NARGS (aset, 3);
	CASE_CALL_NARGS (symbol_value, 1);
	CASE_CALL_NARGS (symbol_function, 1);
	CASE_CALL_NARGS (set, 2);
	CASE_CALL_NARGS (fset, 2);
	CASE_CALL_NARGS (get, 2);
	CASE_CALL_NARGS (substring, 3);

	case Bconcat2:
	  EMIT_SCRATCH_CALL_N ("Fconcat", 2);
	  break;
	case Bconcat3:
	  EMIT_SCRATCH_CALL_N ("Fconcat", 3);
	  break;
	case Bconcat4:
	  EMIT_SCRATCH_CALL_N ("Fconcat", 4);
	  break;
	case BconcatN:
	  op = FETCH;
	  EMIT_SCRATCH_CALL_N ("Fconcat", op);
	  break;

	case Bsub1:
	  {

	    /* (FIXNUMP (TOP) && XFIXNUM (TOP) != MOST_NEGATIVE_FIXNUM
	         ? make_fixnum (XFIXNUM (TOP) - 1)
	         : Fsub1 (TOP)) */

	    gcc_jit_block *sub1_inline_block =
		 gcc_jit_function_new_block (comp.func, "inline_sub1");
	    gcc_jit_block *sub1_fcall_block =
		 gcc_jit_function_new_block (comp.func, "fcall_sub1");

	    gcc_jit_rvalue *tos_as_num =
	      comp_XFIXNUM (gcc_jit_lvalue_as_rvalue (TOS));

	    comp_emit_cond_jump (
	      gcc_jit_context_new_binary_op (
		comp.ctxt,
		NULL,
		GCC_JIT_BINARY_OP_LOGICAL_AND,
		comp.bool_type,
		comp_cast (comp.bool_type,
			   comp_FIXNUMP (gcc_jit_lvalue_as_rvalue (TOS))),
		gcc_jit_context_new_comparison (comp.ctxt,
						NULL,
						GCC_JIT_COMPARISON_NE,
						tos_as_num,
						comp.most_negative_fixnum)),
	      sub1_inline_block,
	      sub1_fcall_block);

	    gcc_jit_rvalue *sub1_inline_res =
	      gcc_jit_context_new_binary_op (comp.ctxt,
					     NULL,
					     GCC_JIT_BINARY_OP_MINUS,
					     comp.long_long_type,
					     tos_as_num,
					     comp.one);

	    gcc_jit_block_add_assignment (sub1_inline_block,
					  NULL,
					  TOS,
					  comp_make_fixnum (sub1_inline_block,
							    sub1_inline_res));
	    basic_block_t bb_orig = *comp.bblock;

	    comp.bblock->gcc_bb = sub1_fcall_block;
	    POP1;
	    res = comp_emit_call ("Fsub1", comp.lisp_obj_type, 1, args);
	    PUSH_LVAL (res);

	    *comp.bblock = bb_orig;

	    gcc_jit_block_end_with_jump (sub1_inline_block, NULL,
					 bb_map[pc].gcc_bb);
	    gcc_jit_block_end_with_jump (sub1_fcall_block, NULL,
					 bb_map[pc].gcc_bb);
	  }

	  break;
	case Badd1:
	  {

	    /* (FIXNUMP (TOP) && XFIXNUM (TOP) != MOST_POSITIVE_FIXNUM
	         ? make_fixnum (XFIXNUM (TOP) + 1)
	         : Fadd (TOP)) */

	    gcc_jit_block *add1_inline_block =
		 gcc_jit_function_new_block (comp.func, "inline_add1");
	    gcc_jit_block *add1_fcall_block =
		 gcc_jit_function_new_block (comp.func, "fcall_add1");

	    gcc_jit_rvalue *tos_as_num =
	      comp_XFIXNUM (gcc_jit_lvalue_as_rvalue (TOS));

	    comp_emit_cond_jump (
	      gcc_jit_context_new_binary_op (
		comp.ctxt,
		NULL,
		GCC_JIT_BINARY_OP_LOGICAL_AND,
		comp.bool_type,
		comp_cast (comp.bool_type,
			   comp_FIXNUMP (gcc_jit_lvalue_as_rvalue (TOS))),
		gcc_jit_context_new_comparison (comp.ctxt,
						NULL,
						GCC_JIT_COMPARISON_NE,
						tos_as_num,
						comp.most_positive_fixnum)),
	      add1_inline_block,
	      add1_fcall_block);

	    gcc_jit_rvalue *add1_inline_res =
	      gcc_jit_context_new_binary_op (comp.ctxt,
					     NULL,
					     GCC_JIT_BINARY_OP_PLUS,
					     comp.long_long_type,
					     tos_as_num,
					     comp.one);

	    gcc_jit_block_add_assignment (add1_inline_block,
					  NULL,
					  TOS,
					  comp_make_fixnum (add1_inline_block,
							    add1_inline_res));
	    basic_block_t bb_orig = *comp.bblock;

	    comp.bblock->gcc_bb = add1_fcall_block;
	    POP1;
	    res = comp_emit_call ("Fadd1", comp.lisp_obj_type, 1, args);
	    PUSH_LVAL (res);

	    *comp.bblock = bb_orig;

	    gcc_jit_block_end_with_jump (add1_inline_block, NULL,
					 bb_map[pc].gcc_bb);
	    gcc_jit_block_end_with_jump (add1_fcall_block, NULL,
					 bb_map[pc].gcc_bb);
	  }
	  break;

	case Beqlsign:
	  EMIT_ARITHCOMPARE (ARITH_EQUAL);
	  break;

	case Bgtr:
	  EMIT_ARITHCOMPARE (ARITH_GRTR);
	  break;

	case Blss:
	  EMIT_ARITHCOMPARE (ARITH_LESS);
	  break;

	case Bleq:
	  EMIT_ARITHCOMPARE (ARITH_LESS_OR_EQUAL);
	  break;

	case Bgeq:
	  EMIT_ARITHCOMPARE (ARITH_GRTR_OR_EQUAL);
	  break;

	case Bdiff:
	  EMIT_SCRATCH_CALL_N ("Fminus", 2);
	  break;

	case Bnegate:
	  {

	    /* (FIXNUMP (TOP) && XFIXNUM (TOP) != MOST_NEGATIVE_FIXNUM
		 ? make_fixnum (- XFIXNUM (TOP))
		 : Fminus (1, &TOP)) */

	    gcc_jit_block *negate_inline_block =
		 gcc_jit_function_new_block (comp.func, "inline_negate");
	    gcc_jit_block *negate_fcall_block =
		 gcc_jit_function_new_block (comp.func, "fcall_negate");

	    gcc_jit_rvalue *tos_as_num =
	      comp_XFIXNUM (gcc_jit_lvalue_as_rvalue (TOS));

	    comp_emit_cond_jump (
	      gcc_jit_context_new_binary_op (
		comp.ctxt,
		NULL,
		GCC_JIT_BINARY_OP_LOGICAL_AND,
		comp.bool_type,
		comp_cast (comp.bool_type,
			   comp_FIXNUMP (gcc_jit_lvalue_as_rvalue (TOS))),
		gcc_jit_context_new_comparison (comp.ctxt,
						NULL,
						GCC_JIT_COMPARISON_NE,
						tos_as_num,
						comp.most_negative_fixnum)),
	      negate_inline_block,
	      negate_fcall_block);

	    gcc_jit_rvalue *negate_inline_res =
	      gcc_jit_context_new_unary_op (comp.ctxt,
					    NULL,
					    GCC_JIT_UNARY_OP_MINUS,
					    comp.long_long_type,
					    tos_as_num);

	    gcc_jit_block_add_assignment (negate_inline_block,
					  NULL,
					  TOS,
					  comp_make_fixnum (negate_inline_block,
							    negate_inline_res));
	    basic_block_t bb_orig = *comp.bblock;

	    comp.bblock->gcc_bb = negate_fcall_block;
	    EMIT_SCRATCH_CALL_N ("Fminus", 1);
	    *comp.bblock = bb_orig;

	    gcc_jit_block_end_with_jump (negate_inline_block, NULL,
					 bb_map[pc].gcc_bb);
	    gcc_jit_block_end_with_jump (negate_fcall_block, NULL,
					 bb_map[pc].gcc_bb);
	  }
	  break;
	case Bplus:
	  EMIT_SCRATCH_CALL_N ("Fplus", 2);
	  break;
	case Bmax:
	  EMIT_SCRATCH_CALL_N ("Fmax", 2);
	  break;
	case Bmin:
	  EMIT_SCRATCH_CALL_N ("Fmin", 2);
	  break;
	case Bmult:
	  EMIT_SCRATCH_CALL_N ("Ftimes", 2);
	  break;
	case Bpoint:
	  args[0] =
	    gcc_jit_context_new_rvalue_from_int (comp.ctxt,
						 comp.ptrdiff_type,
						 PT);
	  res = comp_emit_call ("make_fixed_natnum",
				comp.lisp_obj_type,
				1,
				args);
	  PUSH_LVAL (res);
	  break;

	CASE_CALL_NARGS (goto_char, 1);

	case Binsert:
	  EMIT_SCRATCH_CALL_N ("Finsert", 1);
	  break;

	case Bpoint_max:
	  args[0] =
	    gcc_jit_context_new_rvalue_from_int (comp.ctxt,
						 comp.ptrdiff_type,
						 ZV);
	  res = comp_emit_call ("make_fixed_natnum",
				comp.lisp_obj_type,
				1,
				args);
	  PUSH_LVAL (res);
	  break;

	case Bpoint_min:
	  args[0] =
	    gcc_jit_context_new_rvalue_from_int (comp.ctxt,
						 comp.ptrdiff_type,
						 BEGV);
	  res = comp_emit_call ("make_fixed_natnum",
				comp.lisp_obj_type,
				1,
				args);
	  PUSH_LVAL (res);
	  break;

	CASE_CALL_NARGS (char_after, 1);
	CASE_CALL_NARGS (following_char, 0);

	case Bpreceding_char:
	  res = comp_emit_call ("Fprevious_char", comp.lisp_obj_type, 0, args);
	  PUSH_LVAL (res);
	  break;

	CASE_CALL_NARGS (current_column, 0);

	case Bindent_to:
	  POP1;
	  args[1] = nil;
	  res = comp_emit_call ("Findent_to", comp.lisp_obj_type, 2, args);
	  PUSH_LVAL (res);
	  break;

	CASE_CALL_NARGS (eolp, 0);
	CASE_CALL_NARGS (eobp, 0);
	CASE_CALL_NARGS (bolp, 0);
	CASE_CALL_NARGS (bobp, 0);
	CASE_CALL_NARGS (current_buffer, 0);
	CASE_CALL_NARGS (set_buffer, 1);

	case Bsave_current_buffer: /* Obsolete since ??.  */
	case Bsave_current_buffer_1:
	  comp_emit_call ("record_unwind_current_buffer",
			  comp.void_type, 0, NULL);
	  break;

	case Binteractive_p:	/* Obsolete since 24.1.  */
	  PUSH_RVAL (comp_lisp_obj_as_ptr_from_ptr (comp.bblock,
						    intern ("interactive-p")));
	  res = comp_emit_call ("call0", comp.lisp_obj_type, 1, args);
	  PUSH_LVAL (res);
	  break;

	CASE_CALL_NARGS (forward_char, 1);
	CASE_CALL_NARGS (forward_word, 1);
	CASE_CALL_NARGS (skip_chars_forward, 2);
	CASE_CALL_NARGS (skip_chars_backward, 2);
	CASE_CALL_NARGS (forward_line, 1);
	CASE_CALL_NARGS (char_syntax, 1);
	CASE_CALL_NARGS (buffer_substring, 2);
	CASE_CALL_NARGS (delete_region, 2);
	CASE_CALL_NARGS (narrow_to_region, 2);
	CASE_CALL_NARGS (widen, 0);
	CASE_CALL_NARGS (end_of_line, 1);

	case Bconstant2:
	  goto do_constant;
	  break;

	case Bgoto:
	  op = FETCH2;
	  gcc_jit_block_end_with_jump (comp.bblock->gcc_bb,
				       NULL,
				       bb_map[op].gcc_bb);
	  comp.bblock->terminated = true;
	  break;

	case Bgotoifnil:
	  op = FETCH2;
	  POP1;
	  comp_emit_comp_jump (GCC_JIT_COMPARISON_EQ, args[0], nil,
			       bb_map[op].gcc_bb, bb_map[pc].gcc_bb);
	  break;

	case Bgotoifnonnil:
	  op = FETCH2;
	  POP1;
	  comp_emit_comp_jump (GCC_JIT_COMPARISON_NE, args[0], nil,
			       bb_map[op].gcc_bb, bb_map[pc].gcc_bb);
	  break;

	case Bgotoifnilelsepop:
	  op = FETCH2;
	  comp_emit_comp_jump (GCC_JIT_COMPARISON_EQ,
			       gcc_jit_lvalue_as_rvalue (TOS),
			       nil,
			       bb_map[op].gcc_bb, bb_map[pc].gcc_bb);
	  POP1;
	  break;

	case Bgotoifnonnilelsepop:
	  op = FETCH2;
	  comp_emit_comp_jump (GCC_JIT_COMPARISON_NE,
			       gcc_jit_lvalue_as_rvalue (TOS),
			       nil,
			       bb_map[op].gcc_bb, bb_map[pc].gcc_bb);
	  POP1;
	  break;

	case Breturn:
	  POP1;
	  gcc_jit_block_end_with_return(comp.bblock->gcc_bb,
					NULL,
					args[0]);
	  comp.bblock->terminated = true;
	  break;

	case Bdiscard:
	  POP1;
	  break;

	case Bdup:
	  PUSH_LVAL (TOS);
	  break;

	case Bsave_excursion:
	  res = comp_emit_call ("record_unwind_protect_excursion",
				comp.void_type, 0, args);
	  break;

	case Bsave_window_excursion: /* Obsolete since 24.1.  */
	  POP1;
	  res = comp_emit_call ("helper_save_window_excursion",
				comp.lisp_obj_type, 1, args);
	  PUSH_LVAL (res);
	  break;

	case Bsave_restriction:
	  args[0] = comp_lisp_obj_as_ptr_from_ptr (comp.bblock,
						   save_restriction_restore);
	  args[1] =
	    gcc_jit_lvalue_as_rvalue (comp_emit_call ("save_restriction_save",
						      comp.lisp_obj_type,
						      0,
						      NULL));
	  comp_emit_call ("record_unwind_protect", comp.void_ptr_type, 2, args);
	  break;

	case Bcatch:		/* Obsolete since 24.4.  */
	  POP2;
	  args[2] = args[1];
	  args[1] = comp_lisp_obj_as_ptr_from_ptr (comp.bblock, eval_sub);
	  comp_emit_call ("internal_catch", comp.void_ptr_type, 3, args);
	  break;

	case Bunwind_protect:	/* FIXME: avoid closure for lexbind.  */
	  POP1;
	  comp_emit_call ("helper_unwind_protect", comp.void_type, 1, args);
	  break;

	case Bcondition_case:		/* Obsolete since 24.4.  */
	  POP3;
	  comp_emit_call ("internal_lisp_condition_case",
			  comp.lisp_obj_type, 3, args);
	  break;

	case Btemp_output_buffer_setup: /* Obsolete since 24.1.  */
	  POP1;
	  res = comp_emit_call ("helper_temp_output_buffer_setup", comp.lisp_obj_type,
				1, args);
	  PUSH_LVAL (res);
	  break;

	case Btemp_output_buffer_show: /* Obsolete since 24.1.  */
	  POP2;
	  comp_emit_call ("temp_output_buffer_show", comp.void_type, 1,
			  &args[1]);
	  PUSH_RVAL (args[0]);
	  comp_emit_call ("helper_unbind_n", comp.lisp_obj_type, 1, args);

	  break;
	case Bunbind_all:	/* Obsolete.  Never used.  */
	  /* To unbind back to the beginning of this frame.  Not used yet,
	     but will be needed for tail-recursion elimination.  */
	  error ("Bunbind_all not supported");
	  break;

	CASE_CALL_NARGS (set_marker, 3);
	CASE_CALL_NARGS (match_beginning, 1);
	CASE_CALL_NARGS (match_end, 1);
	CASE_CALL_NARGS (upcase, 1);
	CASE_CALL_NARGS (downcase, 1);

	case Bstringeqlsign:
	  POP2;
	  res = comp_emit_call ("Fstring_equal", comp.lisp_obj_type, 2, args);
	  PUSH_LVAL (res);
	  break;

	case Bstringlss:
	  POP2;
	  res = comp_emit_call ("Fstring_lessp", comp.lisp_obj_type, 2, args);
	  PUSH_LVAL (res);
	  break;

	CASE_CALL_NARGS (equal, 2);
	CASE_CALL_NARGS (nthcdr, 2);
	CASE_CALL_NARGS (elt, 2);
	CASE_CALL_NARGS (member, 2);
	CASE_CALL_NARGS (assq, 2);
	CASE_CALL_NARGS (setcar, 2);
	CASE_CALL_NARGS (setcdr, 2);

	case Bcar_safe:
	  error ("Bcar_safe not supported");
	  break;
	case Bcdr_safe:
	  error ("Bcdr_safe not supported");
	  break;

	case Bnconc:
	  EMIT_SCRATCH_CALL_N ("Fnconc", 2);
	  break;

	case Bquo:
	  EMIT_SCRATCH_CALL_N ("Fquo", 2);
	  break;

	CASE_CALL_NARGS (rem, 2);

	case Bnumberp:
	  error ("Bnumberp not supported");
	  break;

	case Bintegerp:
	  error ("Bintegerp not supported");
	  break;

	case BRgoto:
	  op = FETCH - 128;
	  op += pc;
	  gcc_jit_block_end_with_jump (comp.bblock->gcc_bb,
				       NULL,
				       bb_map[op].gcc_bb);
	  comp.bblock->terminated = true;
	  break;

	case BRgotoifnil:
	  op = FETCH - 128;
	  op += pc;
	  POP1;
	  comp_emit_comp_jump (GCC_JIT_COMPARISON_EQ, args[0], nil,
			       bb_map[op].gcc_bb, bb_map[pc].gcc_bb);
	  break;

	case BRgotoifnonnil:
	  op = FETCH - 128;
	  op += pc;
	  POP1;
	  comp_emit_comp_jump (GCC_JIT_COMPARISON_NE, args[0], nil,
			       bb_map[op].gcc_bb, bb_map[pc].gcc_bb);
	  break;

	case BRgotoifnilelsepop:
	  op = FETCH - 128;
	  op += pc;
	  comp_emit_comp_jump (GCC_JIT_COMPARISON_EQ,
			       gcc_jit_lvalue_as_rvalue (TOS),
			       nil,
			       bb_map[op].gcc_bb, bb_map[pc].gcc_bb);
	  POP1;
	  break;

	case BRgotoifnonnilelsepop:
	  op = FETCH - 128;
	  op += pc;
	  comp_emit_comp_jump (GCC_JIT_COMPARISON_NE,
			       gcc_jit_lvalue_as_rvalue (TOS),
			       nil,
			       bb_map[op].gcc_bb, bb_map[pc].gcc_bb);
	  POP1;
	  break;

	case BinsertN:
	  error ("BinsertN not supported");
	  break;

	case Bstack_set:
	  /* stack-set-0 = discard; stack-set-1 = discard-1-preserve-tos.  */
	  op = FETCH;
	  POP1;
	  if (op > 0)
	    gcc_jit_block_add_assignment (comp.bblock->gcc_bb,
					  NULL,
					  *(stack - op),
					  args[0]);
	  break;

	case Bstack_set2:
	  error ("Bstack_set2 not supported");
	  break;
	case BdiscardN:
	  error ("BdiscardN not supported");
	  break;
	case Bswitch:
	  error ("Bswitch not supported");
	  /* The cases of Bswitch that we handle (which in theory is
	     all of them) are done in Bconstant, below.  This is done
	     due to a design issue with Bswitch -- it should have
	     taken a constant pool index inline, but instead looks for
	     a constant on the stack.  */
	  goto fail;
	  break;
	default:
	case Bconstant:
	  {
	    if (op < Bconstant || op > Bconstant + vector_size)
	      goto fail;

	    op -= Bconstant;
	  do_constant:

	    /* See the Bswitch case for commentary.  */
	    if (pc >= bytestr_length || bytestr_data[pc] != Bswitch)
	      {
		gcc_jit_rvalue *c =
		  comp_lisp_obj_as_ptr_from_ptr (comp.bblock, vectorp[op]);
		PUSH_RVAL (c);
		break;
	      }

	    /* We're compiling Bswitch instead.  */
	    ++pc;
	    break;
	  }
	}
    }

  comp_res.gcc_res = gcc_jit_context_compile(comp.ctxt);

  goto exit;

 fail:
  error ("Something went wrong");

 exit:
  xfree (stack_base);
  xfree (bb_map);
  return comp_res;
}

void
emacs_native_compile (const char *lisp_f_name, const char *c_f_name,
		      Lisp_Object func, int opt_level, bool dump_asm)
{
  Lisp_Object bytestr = AREF (func, COMPILED_BYTECODE);
  CHECK_STRING (bytestr);

  if (STRING_MULTIBYTE (bytestr))
    /* BYTESTR must have been produced by Emacs 20.2 or the earlier
       because they produced a raw 8-bit string for byte-code and now
       such a byte-code string is loaded as multibyte while raw 8-bit
       characters converted to multibyte form.  Thus, now we must
       convert them back to the originally intended unibyte form.  */
    bytestr = Fstring_as_unibyte (bytestr);

  ptrdiff_t bytestr_length = SBYTES (bytestr);

  Lisp_Object vector = AREF (func, COMPILED_CONSTANTS);
  CHECK_VECTOR (vector);
  Lisp_Object *vectorp = XVECTOR (vector)->contents;

  Lisp_Object maxdepth = AREF (func, COMPILED_STACK_DEPTH);
  CHECK_FIXNAT (maxdepth);

  /* Gcc doesn't like being interrupted.  */
  sigset_t oldset;
  block_atimers (&oldset);

  gcc_jit_context_set_int_option (comp.ctxt,
				  GCC_JIT_INT_OPTION_OPTIMIZATION_LEVEL,
				  opt_level);

  comp_f_res_t comp_res = compile_f (c_f_name, bytestr_length, SDATA (bytestr),
				     XFIXNAT (maxdepth) + 1,
				     vectorp, ASIZE (vector),
				     AREF (func, COMPILED_ARGLIST));

  union Aligned_Lisp_Subr *x = xmalloc (sizeof (union Aligned_Lisp_Subr));

  x->s.header.size = PVEC_SUBR << PSEUDOVECTOR_AREA_BITS;
  x->s.function.a0 = gcc_jit_result_get_code(comp_res.gcc_res, c_f_name);
  eassert (x->s.function.a0);
  x->s.min_args = comp_res.min_args;
  x->s.max_args = comp_res.max_args;
  x->s.symbol_name = lisp_f_name;
  defsubr(x);

  if (dump_asm)
    {
      gcc_jit_context_compile_to_file(comp.ctxt,
				      GCC_JIT_OUTPUT_KIND_ASSEMBLER,
				      DISASS_FILE_NAME);
    }
  unblock_atimers (&oldset);
}

DEFUN ("native-compile", Fnative_compile, Snative_compile,
       1, 3, 0,
       doc: /* Compile as native code function FUNC and load it.  */) /* FIXME doc */
     (Lisp_Object func, Lisp_Object speed, Lisp_Object disassemble)
{
  static char c_f_name[MAX_FUN_NAME];
  char *lisp_f_name;

  if (!SYMBOLP (func))
    error ("Not a symbol.");

  lisp_f_name = (char *) SDATA (SYMBOL_NAME (func));

  int res = snprintf (c_f_name, MAX_FUN_NAME, "Fnative_comp_%s", lisp_f_name);

  if (res >= MAX_FUN_NAME)
    error ("Function name too long");

  /* FIXME how many other characters are not allowed in C?
     This will introduce name clashs too. */
  char *c = c_f_name;
  while (*c)
    {
      if (*c == '-' ||
	  *c == '+')
	*c = '_';
      ++c;
    }

  func = indirect_function (func);
  if (!COMPILEDP (func))
    error ("Not a byte-compiled function");

  if (speed != Qnil &&
      (!FIXNUMP (speed) ||
       !(XFIXNUM (speed) >= 0 &&
	 XFIXNUM (speed) <= 3)))
    error ("opt-level must be number between 0 and 3");

  int opt_level;
  if (speed == Qnil)
    opt_level = DEFAULT_SPEED;
  else
    opt_level = XFIXNUM (speed);

  emacs_native_compile (lisp_f_name, c_f_name, func, opt_level,
			disassemble != Qnil);

  if (disassemble)
    {
      FILE *fd;
      Lisp_Object str;

      if ((fd = fopen (DISASS_FILE_NAME, "r")))
	{
	  fseek (fd , 0L, SEEK_END);
	  long int size = ftell (fd);
	  fseek (fd , 0L, SEEK_SET);
	  char *buffer = xmalloc (size + 1);
	  ptrdiff_t nread = fread (buffer, 1, size, fd);
	  if (nread > 0)
	    {
	      size = nread;
	      buffer[size] = '\0';
	      str = make_string (buffer, size);
	      fclose (fd);
	    }
	  else
	    str = empty_unibyte_string;
	  xfree (buffer);
	  return str;
	}
      else
	{
	  error ("disassemble file could not be found");
	}
    }

  return Qnil;
}

void
init_comp (void)
{
  comp.ctxt = gcc_jit_context_acquire();

  if (COMP_DEBUG > 1)
    gcc_jit_context_dump_reproducer_to_file (comp.ctxt, "comp_reproducer.c");

  comp.void_type = gcc_jit_context_get_type (comp.ctxt, GCC_JIT_TYPE_VOID);
  comp.void_ptr_type =
    gcc_jit_context_get_type (comp.ctxt, GCC_JIT_TYPE_VOID_PTR);
  comp.int_type = gcc_jit_context_get_type (comp.ctxt, GCC_JIT_TYPE_INT);
  comp.unsigned_type = gcc_jit_context_get_type (comp.ctxt,
						 GCC_JIT_TYPE_UNSIGNED_INT);
  comp.bool_type = gcc_jit_context_get_type (comp.ctxt, GCC_JIT_TYPE_BOOL);
  comp.long_type = gcc_jit_context_get_type (comp.ctxt, GCC_JIT_TYPE_LONG);
  comp.long_long_type = gcc_jit_context_get_type (comp.ctxt,
						  GCC_JIT_TYPE_LONG_LONG);

#if EMACS_INT_MAX <= LONG_MAX
  /* 32-bit builds without wide ints, 64-bit builds on Posix hosts.  */
  comp.lisp_obj_as_ptr = gcc_jit_context_new_field (comp.ctxt,
						    NULL,
						    comp.void_ptr_type,
						    "obj");
  comp.lisp_obj_as_num =  gcc_jit_context_new_field (comp.ctxt,
						     NULL,
						     comp.long_long_type,
						     "num");

#else
  /* 64-bit builds on MS-Windows, 32-bit builds with wide ints.  */
  comp.lisp_obj_as_ptr = gcc_jit_context_new_field (comp.ctxt,
						    NULL,
						    comp.long_long_type,
						    "obj");
  comp.lisp_obj_as_num = gcc_jit_context_new_field (comp.ctxt,
						    NULL,
						    comp.long_long_type,
						    "num");
#endif

  gcc_jit_field *lisp_obj_fields[2] = { comp.lisp_obj_as_ptr,
                                        comp.lisp_obj_as_num };
  comp.lisp_obj_type = gcc_jit_context_new_union_type (comp.ctxt,
						       NULL,
						       "LispObj",
						       2,
						       lisp_obj_fields);

  comp.cast_union_as_ll =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.long_long_type,  /* FIXME? */
			       "ll");
  comp.cast_union_as_u =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.unsigned_type,
			       "u");
  comp.cast_union_as_i =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.int_type,
			       "i");
  comp.cast_union_as_b =
    gcc_jit_context_new_field (comp.ctxt,
			       NULL,
			       comp.bool_type,
			       "b");

  gcc_jit_field *cast_union_fields[4] =
    { comp.cast_union_as_ll,
      comp.cast_union_as_u,
      comp.cast_union_as_i,
      comp.cast_union_as_b,};
  comp.cast_union_type = gcc_jit_context_new_union_type (comp.ctxt,
							 NULL,
							 "cast_union",
							 4,
							 cast_union_fields);
  comp.most_positive_fixnum =
    gcc_jit_context_new_rvalue_from_long (comp.ctxt,
					  comp.long_long_type, /* FIXME? */
					  MOST_POSITIVE_FIXNUM);
  comp.most_negative_fixnum =
    gcc_jit_context_new_rvalue_from_long (comp.ctxt,
					  comp.long_long_type, /* FIXME? */
					  MOST_NEGATIVE_FIXNUM);
  comp.one =
    gcc_jit_context_new_rvalue_from_int (comp.ctxt,
					 comp.long_long_type,  /* FIXME? */
					 1);
  comp.inttypebits =
    gcc_jit_context_new_rvalue_from_int (comp.ctxt,
					 comp.long_long_type,  /* FIXME? */
					 INTTYPEBITS);

  comp.lisp_int0 =
    gcc_jit_context_new_rvalue_from_int (comp.ctxt,
					 comp.long_long_type,  /* FIXME? */
					 Lisp_Int0);

  enum gcc_jit_types ptrdiff_t_gcc;
  if (sizeof (ptrdiff_t) == sizeof (int))
    ptrdiff_t_gcc = GCC_JIT_TYPE_INT;
  else if (sizeof (ptrdiff_t) == sizeof (long int))
    ptrdiff_t_gcc = GCC_JIT_TYPE_LONG;
  else if (sizeof (ptrdiff_t) == sizeof (long long int))
    ptrdiff_t_gcc = GCC_JIT_TYPE_LONG_LONG;
  else
    eassert ("ptrdiff_t size not handled.");

  comp.ptrdiff_type = gcc_jit_context_get_type(comp.ctxt, ptrdiff_t_gcc);

  comp.scratch =
    gcc_jit_lvalue_get_address(
      gcc_jit_context_new_global (comp.ctxt, NULL,
				  GCC_JIT_GLOBAL_IMPORTED,
				  comp.lisp_obj_type,
				  "scratch_call_area"),
      NULL);

  comp.func_hash = CALLN (Fmake_hash_table, QCtest, Qequal, QCweakness, Qt);

  if (COMP_DEBUG) {
    logfile = fopen ("libgccjit.log", "w");
    gcc_jit_context_set_logfile (comp.ctxt,
				 logfile,
				 0, 0);
    gcc_jit_context_set_bool_option (comp.ctxt,
				     GCC_JIT_BOOL_OPTION_DUMP_EVERYTHING,
				     1);
  }

  gcc_jit_context_set_bool_option (comp.ctxt,
				   GCC_JIT_BOOL_OPTION_KEEP_INTERMEDIATES,
				   1);
}

void
release_comp (void)
{
  if (comp.ctxt)
    gcc_jit_context_release(comp.ctxt);

  if (logfile)
    fclose (logfile);
}

void
syms_of_comp (void)
{
  defsubr (&Snative_compile);
  comp.func_hash = Qnil;
  staticpro (&comp.func_hash);
}

/******************************************************************************/
/* Helper functions called from the runtime.				      */
/* These can't be statics till shared mechanism is used to solve relocations. */
/******************************************************************************/

Lisp_Object helper_save_window_excursion (Lisp_Object v1);

void helper_unwind_protect (Lisp_Object handler);

Lisp_Object helper_temp_output_buffer_setup (Lisp_Object x);

Lisp_Object helper_unbind_n (int val);

Lisp_Object
helper_save_window_excursion (Lisp_Object v1)
{
  ptrdiff_t count1 = SPECPDL_INDEX ();
  record_unwind_protect (restore_window_configuration,
			 Fcurrent_window_configuration (Qnil));
  v1 = Fprogn (v1);
  unbind_to (count1, v1);
  return v1;
}

void helper_unwind_protect (Lisp_Object handler)
{
  /* Support for a function here is new in 24.4.  */
  record_unwind_protect (FUNCTIONP (handler) ? bcall0 : prog_ignore,
			 handler);
}

Lisp_Object
helper_temp_output_buffer_setup (Lisp_Object x)
{
  CHECK_STRING (x);
  temp_output_buffer_setup (SSDATA (x));
  return Vstandard_output;
}

Lisp_Object
helper_unbind_n (int val)
{
  return unbind_to (SPECPDL_INDEX () - val, Qnil);
}

#endif /* HAVE_LIBGCCJIT */