/* Parse C expressions for cpplib.
   Copyright (C) 1987, 1992, 1994, 1995, 1997, 1998, 1999, 2000, 2001,
   2002 Free Software Foundation.
   Contributed by Per Bothner, 1994.

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

#include "config.h"
#include "system.h"
#include "cpplib.h"
#include "cpphash.h"

typedef struct cpp_num cpp_num;

#define PART_PRECISION (sizeof (cpp_num_part) * CHAR_BIT)
#define HALF_MASK (~(cpp_num_part) 0 >> (PART_PRECISION / 2))
#define LOW_PART(num_part) (num_part & HALF_MASK)
#define HIGH_PART(num_part) (num_part >> (PART_PRECISION / 2))

/* A preprocessing number.  Code assumes that any unused high bits of
   the double integer are set to zero.  */
struct cpp_num
{
  cpp_num_part high;
  cpp_num_part low;
  bool unsignedp;  /* True if value should be treated as unsigned.  */
  bool overflow;   /* True if the most recent calculation overflowed.  */
};

struct op
{
  cpp_num value;		     /* The value logically "right" of op.  */
  enum cpp_ttype op;
};

/* Some simple utility routines on double integers.  */
#define num_zerop(num) ((num.low | num.high) == 0)
#define num_eq(num1, num2) (num1.low == num2.low && num1.high == num2.high)
static bool num_positive PARAMS ((cpp_num, size_t));
static bool num_greater_eq PARAMS ((cpp_num, cpp_num, size_t));
static cpp_num num_trim PARAMS ((cpp_num, size_t));
static cpp_num num_part_mul PARAMS ((cpp_num_part, cpp_num_part));

static cpp_num num_unary_op PARAMS ((cpp_reader *, cpp_num, enum cpp_ttype));
static cpp_num num_binary_op PARAMS ((cpp_reader *, cpp_num, cpp_num,
				      enum cpp_ttype));
static cpp_num num_negate PARAMS ((cpp_num, size_t));
static cpp_num num_bitwise_op PARAMS ((cpp_reader *, cpp_num, cpp_num,
				       enum cpp_ttype));
static cpp_num num_inequality_op PARAMS ((cpp_reader *, cpp_num, cpp_num,
					  enum cpp_ttype));
static cpp_num num_equality_op PARAMS ((cpp_reader *, cpp_num, cpp_num,
					enum cpp_ttype));
static cpp_num num_mul PARAMS ((cpp_reader *, cpp_num, cpp_num,
				enum cpp_ttype));
static cpp_num num_div_op PARAMS ((cpp_reader *, cpp_num, cpp_num,
				   enum cpp_ttype));
static cpp_num num_lshift PARAMS ((cpp_num, size_t, size_t));
static cpp_num num_rshift PARAMS ((cpp_num, size_t, size_t));

static cpp_num append_digit PARAMS ((cpp_num, int, int, size_t));
static cpp_num interpret_number PARAMS ((cpp_reader *, const cpp_token *));
static cpp_num parse_defined PARAMS ((cpp_reader *));
static cpp_num eval_token PARAMS ((cpp_reader *, const cpp_token *));
static struct op *reduce PARAMS ((cpp_reader *, struct op *, enum cpp_ttype));

/* Token type abuse.  There is no "error" token, but we can't get
   comments in #if, so we can abuse that token type.  Similarly,
   create unary plus and minus operators.  */
#define CPP_ERROR CPP_COMMENT
#define CPP_UPLUS (CPP_LAST_CPP_OP + 1)
#define CPP_UMINUS (CPP_LAST_CPP_OP + 2)

/* With -O2, gcc appears to produce nice code, moving the error
   message load and subsequent jump completely out of the main path.  */
#define SYNTAX_ERROR(msgid) \
  do { cpp_error (pfile, DL_ERROR, msgid); goto syntax_error; } while(0)
#define SYNTAX_ERROR2(msgid, arg) \
  do { cpp_error (pfile, DL_ERROR, msgid, arg); goto syntax_error; } while(0)

struct suffix
{
  const unsigned char s[4];
  const unsigned char u;
  const unsigned char l;
};

static const struct suffix vsuf_1[] = {
  { "u", 1, 0 }, { "U", 1, 0 },
  { "l", 0, 1 }, { "L", 0, 1 }
};

static const struct suffix vsuf_2[] = {
  { "ul", 1, 1 }, { "UL", 1, 1 }, { "uL", 1, 1 }, { "Ul", 1, 1 },
  { "lu", 1, 1 }, { "LU", 1, 1 }, { "Lu", 1, 1 }, { "lU", 1, 1 },
  { "ll", 0, 2 }, { "LL", 0, 2 }
};

static const struct suffix vsuf_3[] = {
  { "ull", 1, 2 }, { "ULL", 1, 2 }, { "uLL", 1, 2 }, { "Ull", 1, 2 },
  { "llu", 1, 2 }, { "LLU", 1, 2 }, { "LLu", 1, 2 }, { "llU", 1, 2 }
};

/* Append DIGIT to NUM, a number of PRECISION bits being read in base
   BASE.  */
static cpp_num
append_digit (num, digit, base, precision)
     cpp_num num;
     int digit, base;
     size_t precision;
{
  cpp_num result;
  unsigned int shift = 3 + (base == 16);
  bool overflow;
  cpp_num_part add_high, add_low;

  /* Multiply by 8 or 16.  Catching this overflow here means we don't
     need to worry about add_high overflowing.  */
  overflow = num.high >> (PART_PRECISION - shift);
  result.high = num.high << shift;
  result.low = num.low << shift;
  result.high |= num.low >> (PART_PRECISION - shift);

  if (base == 10)
    {
      add_low = num.low << 1;
      add_high = (num.high << 1) + (num.low >> (PART_PRECISION - 1));
    }
  else
    add_high = add_low = 0;

  if (add_low + digit < add_low)
    add_high++;
  add_low += digit;
    
  if (result.low + add_low < result.low)
    add_high++;
  if (result.high + add_high < result.high)
    overflow = true;

  result.low += add_low;
  result.high += add_high;

  /* The above code catches overflow of a cpp_num type.  This catches
     overflow of the (possibly shorter) target precision.  */
  num.low = result.low;
  num.high = result.high;
  result = num_trim (result, precision);
  if (!num_eq (result, num))
    overflow = true;

  result.unsignedp = num.unsignedp;
  result.overflow = overflow;
  return result;
}

/* Parse and convert what is presumably an integer in TOK.  Accepts
   decimal, hex, or octal with or without size suffixes.  Returned op
   is CPP_ERROR on error, otherwise it is a CPP_NUMBER.  */
static cpp_num
interpret_number (pfile, tok)
     cpp_reader *pfile;
     const cpp_token *tok;
{
  cpp_num result;
  cpp_num_part max;
  const uchar *p = tok->val.str.text;
  const uchar *end;
  const struct suffix *sufftab;
  size_t precision;
  unsigned int i, nsuff, base, c;
  bool overflow, big_digit;

  result.low = 0;
  result.high = 0;
  result.unsignedp = 0;
  result.overflow = 0;

  /* Common case of a single digit.  */
  end = p + tok->val.str.len;
  if (tok->val.str.len == 1 && (unsigned int) (p[0] - '0') <= 9)
    {
      result.low = p[0] - '0';
      return result;
    }

  base = 10;
  if (p[0] == '0')
    {
      if (end - p >= 3 && (p[1] == 'x' || p[1] == 'X'))
	{
	  p += 2;
	  base = 16;
	}
      else
	{
	  p += 1;
	  base = 8;
	}
    }

  c = 0;
  overflow = big_digit = false;
  precision = CPP_OPTION (pfile, precision);

  /* We can add a digit to numbers less than this without needing
     double integers.  9 is the maximum digit for octal and decimal;
     for hex it is annihilated by the division anyway.  */
  max = ~(cpp_num_part) 0;
  if (precision < PART_PRECISION)
    max >>= PART_PRECISION - precision;
  max = (max - 9) / base + 1;

  for(; p < end; p++)
    {
      c = *p;

      if (ISDIGIT (c) || (base == 16 && ISXDIGIT (c)))
	c = hex_value (c);
      else
	break;

      if (c >= base)
	big_digit = true;

      /* Strict inequality for when max is set to zero.  */
      if (result.low < max)
	result.low = result.low * base + c;
      else
	{
	  result = append_digit (result, c, base, precision);
	  overflow |= result.overflow;
	  max = 0;
	}
    }

  if (p < end)
    {
      /* Check for a floating point constant.  Note that float constants
	 with an exponent or suffix but no decimal point are technically
	 invalid (C99 6.4.4.2) but accepted elsewhere.  */
      if ((c == '.' || c == 'F' || c == 'f')
	  || (base == 10 && (c == 'E' || c == 'e')
	      && p+1 < end && (p[1] == '+' || p[1] == '-'))
	  || (base == 16 && (c == 'P' || c == 'p')
	      && p+1 < end && (p[1] == '+' || p[1] == '-')))
	SYNTAX_ERROR ("floating point numbers are not valid in #if");

      /* Determine the suffix. l means long, and u means unsigned.
	 See the suffix tables, above.  */
      switch (end - p)
	{
	case 1: sufftab = vsuf_1; nsuff = ARRAY_SIZE (vsuf_1); break;
	case 2: sufftab = vsuf_2; nsuff = ARRAY_SIZE (vsuf_2); break;
	case 3: sufftab = vsuf_3; nsuff = ARRAY_SIZE (vsuf_3); break;
	default: goto invalid_suffix;
	}

      for (i = 0; i < nsuff; i++)
	if (memcmp (p, sufftab[i].s, end - p) == 0)
	  break;
      if (i == nsuff)
	goto invalid_suffix;
      result.unsignedp = sufftab[i].u;

      if (CPP_WTRADITIONAL (pfile)
	  && sufftab[i].u
	  && ! cpp_sys_macro_p (pfile))
	cpp_error (pfile, DL_WARNING, "traditional C rejects the `U' suffix");
      if (sufftab[i].l == 2 && CPP_OPTION (pfile, pedantic)
	  && ! CPP_OPTION (pfile, c99))
	cpp_error (pfile, DL_PEDWARN,
		   "too many 'l' suffixes in integer constant");
    }

  if (big_digit)
    cpp_error (pfile, DL_PEDWARN,
	       "integer constant contains digits beyond the radix");

  if (overflow)
    cpp_error (pfile, DL_PEDWARN, "integer constant too large for its type");
  /* If too big to be signed, consider it unsigned.  */
  else if (!result.unsignedp && !num_positive (result, precision))
    {
      if (base == 10)
	cpp_error (pfile, DL_WARNING,
		   "integer constant is so large that it is unsigned");
      result.unsignedp = 1;
    }

  return result;

 invalid_suffix:
  cpp_error (pfile, DL_ERROR, "invalid suffix '%.*s' on integer constant",
	     (int) (end - p), p);
 syntax_error:
  return result;
}

/* Handle meeting "defined" in a preprocessor expression.  */
static cpp_num
parse_defined (pfile)
     cpp_reader *pfile;
{
  cpp_num result;
  int paren = 0;
  cpp_hashnode *node = 0;
  const cpp_token *token;
  cpp_context *initial_context = pfile->context;

  /* Don't expand macros.  */
  pfile->state.prevent_expansion++;

  token = cpp_get_token (pfile);
  if (token->type == CPP_OPEN_PAREN)
    {
      paren = 1;
      token = cpp_get_token (pfile);
    }

  if (token->type == CPP_NAME)
    {
      node = token->val.node;
      if (paren && cpp_get_token (pfile)->type != CPP_CLOSE_PAREN)
	{
	  cpp_error (pfile, DL_ERROR, "missing ')' after \"defined\"");
	  node = 0;
	}
    }
  else
    {
      cpp_error (pfile, DL_ERROR,
		 "operator \"defined\" requires an identifier");
      if (token->flags & NAMED_OP)
	{
	  cpp_token op;

	  op.flags = 0;
	  op.type = token->type;
	  cpp_error (pfile, DL_ERROR,
		     "(\"%s\" is an alternative token for \"%s\" in C++)",
		     cpp_token_as_text (pfile, token),
		     cpp_token_as_text (pfile, &op));
	}
    }

  if (node)
    {
      if (pfile->context != initial_context)
	cpp_error (pfile, DL_WARNING,
		   "this use of \"defined\" may not be portable");

      /* A possible controlling macro of the form #if !defined ().
	 _cpp_parse_expr checks there was no other junk on the line.  */
      pfile->mi_ind_cmacro = node;
    }

  pfile->state.prevent_expansion--;

  result.unsignedp = 0;
  result.high = 0;
  result.overflow = 0;
  result.low = node && node->type == NT_MACRO;
  return result;
}

/* Convert a token into a CPP_NUMBER (an interpreted preprocessing
   number or character constant, or the result of the "defined" or "#"
   operators), or CPP_ERROR on error.  */
static cpp_num
eval_token (pfile, token)
     cpp_reader *pfile;
     const cpp_token *token;
{
  cpp_num result;
  unsigned int temp;
  int unsignedp = 0;

  switch (token->type)
    {
    case CPP_NUMBER:
      return interpret_number (pfile, token);

    case CPP_WCHAR:
    case CPP_CHAR:
      {
	cppchar_t cc = cpp_interpret_charconst (pfile, token,
						&temp, &unsignedp);

	result.high = 0;
	result.low = cc;
	/* Sign-extend the result if necessary.  */
	if (!unsignedp && (cppchar_signed_t) cc < 0)
	  {
	    if (PART_PRECISION > BITS_PER_CPPCHAR_T)
	      result.low |= ~(~(cpp_num_part) 0
			      >> (PART_PRECISION - BITS_PER_CPPCHAR_T));
	    result.high = ~(cpp_num_part) 0;
	    result = num_trim (result, CPP_OPTION (pfile, precision));
	  }
      }
      break;

    case CPP_NAME:
      if (token->val.node == pfile->spec_nodes.n_defined)
	return parse_defined (pfile);
      else if (CPP_OPTION (pfile, cplusplus)
	       && (token->val.node == pfile->spec_nodes.n_true
		   || token->val.node == pfile->spec_nodes.n_false))
	{
	  result.high = 0;
	  result.low = (token->val.node == pfile->spec_nodes.n_true);

	  /* Warn about use of true or false in #if when pedantic
	     and stdbool.h has not been included.  */
	  if (CPP_PEDANTIC (pfile)
	      && ! cpp_defined (pfile, DSC("__bool_true_false_are_defined")))
	    cpp_error (pfile, DL_PEDWARN,
		       "ISO C++ does not permit \"%s\" in #if",
		       NODE_NAME (token->val.node));
	}
      else
	{
	  result.high = 0;
	  result.low = 0;
	  if (CPP_OPTION (pfile, warn_undef) && !pfile->state.skip_eval)
	    cpp_error (pfile, DL_WARNING, "\"%s\" is not defined",
		       NODE_NAME (token->val.node));
	}
      break;

    default: /* CPP_HASH */
      _cpp_test_assertion (pfile, &temp);
      result.high = 0;
      result.low = temp;
    }

  result.unsignedp = unsignedp;
  result.overflow = 0;
  return result;
}

/* Operator precedence and flags table.

After an operator is returned from the lexer, if it has priority less
than the operator on the top of the stack, we reduce the stack by one
operator and repeat the test.  Since equal priorities do not reduce,
this is naturally right-associative.

We handle left-associative operators by decrementing the priority of
just-lexed operators by one, but retaining the priority of operators
already on the stack.

The remaining cases are '(' and ')'.  We handle '(' by skipping the
reduction phase completely.  ')' is given lower priority than
everything else, including '(', effectively forcing a reduction of the
parenthesised expression.  If there is a matching '(', the routine
reduce() exits immediately.  If the normal exit route sees a ')', then
there cannot have been a matching '(' and an error message is output.

The parser assumes all shifted operators require a left operand unless
the flag NO_L_OPERAND is set.  These semantics are automatic; any
extra semantics need to be handled with operator-specific code.  */

/* Flags.  */
#define NO_L_OPERAND	(1 << 0)
#define LEFT_ASSOC	(1 << 1)

/* Arity. */
#define UNARY		(1 << 0)
#define BINARY		(1 << 1)
#define OTHER		(1 << 2)

typedef cpp_num (*binary_handler) PARAMS ((cpp_reader *, cpp_num, cpp_num,
					   enum cpp_ttype));
/* Operator to priority map.  Must be in the same order as the first
   N entries of enum cpp_ttype.  */
static const struct operator
{
  uchar prio;
  uchar flags;
  uchar arity;
  binary_handler handler;
} optab[] =
{
  /* EQ */		{0, 0, OTHER, NULL},	/* Shouldn't happen.  */
  /* NOT */		{16, NO_L_OPERAND, UNARY, NULL},
  /* GREATER */		{12, LEFT_ASSOC, BINARY, num_inequality_op},
  /* LESS */		{12, LEFT_ASSOC, BINARY, num_inequality_op},
  /* PLUS */		{14, LEFT_ASSOC, BINARY, num_binary_op},
  /* MINUS */		{14, LEFT_ASSOC, BINARY, num_binary_op},
  /* MULT */		{15, LEFT_ASSOC, BINARY, num_mul},
  /* DIV */		{15, LEFT_ASSOC, BINARY, num_div_op},
  /* MOD */		{15, LEFT_ASSOC, BINARY, num_div_op},
  /* AND */		{9, LEFT_ASSOC, BINARY, num_bitwise_op},
  /* OR */		{7, LEFT_ASSOC, BINARY, num_bitwise_op},
  /* XOR */		{8, LEFT_ASSOC, BINARY, num_bitwise_op},
  /* RSHIFT */		{13, LEFT_ASSOC, BINARY, num_binary_op},
  /* LSHIFT */		{13, LEFT_ASSOC, BINARY, num_binary_op},

  /* MIN */		{10, LEFT_ASSOC, BINARY, num_binary_op},
  /* MAX */		{10, LEFT_ASSOC, BINARY, num_binary_op},

  /* COMPL */		{16, NO_L_OPERAND, UNARY, NULL},
  /* AND_AND */		{6, LEFT_ASSOC, OTHER, NULL},
  /* OR_OR */		{5, LEFT_ASSOC, OTHER, NULL},
  /* QUERY */		{3, 0, OTHER, NULL},
  /* COLON */		{4, LEFT_ASSOC, OTHER, NULL},
  /* COMMA */		{2, LEFT_ASSOC, BINARY, num_binary_op},
  /* OPEN_PAREN */	{1, NO_L_OPERAND, OTHER, NULL},
  /* CLOSE_PAREN */	{0, 0, OTHER, NULL},
  /* EOF */		{0, 0, OTHER, NULL},
  /* EQ_EQ */		{11, LEFT_ASSOC, BINARY, num_equality_op},
  /* NOT_EQ */		{11, LEFT_ASSOC, BINARY, num_equality_op},
  /* GREATER_EQ */	{12, LEFT_ASSOC, BINARY, num_inequality_op},
  /* LESS_EQ */		{12, LEFT_ASSOC, BINARY, num_inequality_op},
  /* UPLUS */		{16, NO_L_OPERAND, UNARY, NULL},
  /* UMINUS */		{16, NO_L_OPERAND, UNARY, NULL}
};

/* Parse and evaluate a C expression, reading from PFILE.
   Returns the truth value of the expression.

   The implementation is an operator precedence parser, i.e. a
   bottom-up parser, using a stack for not-yet-reduced tokens.

   The stack base is op_stack, and the current stack pointer is 'top'.
   There is a stack element for each operator (only), and the most
   recently pushed operator is 'top->op'.  An operand (value) is
   stored in the 'value' field of the stack element of the operator
   that precedes it.  */
bool
_cpp_parse_expr (pfile)
     cpp_reader *pfile;
{
  struct op *top = pfile->op_stack;
  const cpp_token *token = NULL, *prev_token;
  unsigned int lex_count;
  bool saw_leading_not, want_value = true;

  pfile->state.skip_eval = 0;

  /* Set up detection of #if ! defined().  */
  pfile->mi_ind_cmacro = 0;
  saw_leading_not = false;
  lex_count = 0;

  /* Lowest priority operator prevents further reductions.  */
  top->op = CPP_EOF;

  for (;;)
    {
      struct op op;

      prev_token = token;
      token = cpp_get_token (pfile);
      lex_count++;
      op.op = token->type;

      switch (op.op)
	{
	  /* These tokens convert into values.  */
	case CPP_NUMBER:
	case CPP_CHAR:
	case CPP_WCHAR:
	case CPP_NAME:
	case CPP_HASH:
	  if (!want_value)
	    SYNTAX_ERROR2 ("missing binary operator before token \"%s\"",
			   cpp_token_as_text (pfile, token));
	  want_value = false;
	  top->value = eval_token (pfile, token);
	  continue;

	case CPP_NOT:
	  saw_leading_not = lex_count == 1;
	  break;
	case CPP_PLUS:
	  if (want_value)
	    op.op = CPP_UPLUS;
	  break;
	case CPP_MINUS:
	  if (want_value)
	    op.op = CPP_UMINUS;
	  break;
	case CPP_OTHER:
	  if (ISGRAPH (token->val.c))
	    SYNTAX_ERROR2 ("invalid character '%c' in #if", token->val.c);
	  else
	    SYNTAX_ERROR2 ("invalid character '\\%03o' in #if", token->val.c);

	default:
	  if ((int) op.op <= (int) CPP_EQ || (int) op.op >= (int) CPP_PLUS_EQ)
	    SYNTAX_ERROR2 ("token \"%s\" is not valid in #if expressions",
			   cpp_token_as_text (pfile, token));
	  break;
	}

      /* Check we have a value or operator as appropriate.  */
      if (optab[op.op].flags & NO_L_OPERAND)
	{
	  if (!want_value)
	    SYNTAX_ERROR2 ("missing binary operator before token \"%s\"",
			   cpp_token_as_text (pfile, token));
	}
      else if (want_value)
	{
	  /* Ordering here is subtle and intended to favour the
	     missing parenthesis diagnostics over alternatives.  */
	  if (op.op == CPP_CLOSE_PAREN)
	    {
	      if (top->op == CPP_OPEN_PAREN)
		SYNTAX_ERROR ("void expression between '(' and ')'");
	    }
	  else if (top->op == CPP_EOF)
	    SYNTAX_ERROR ("#if with no expression");
	  if (top->op != CPP_EOF && top->op != CPP_OPEN_PAREN)
	    SYNTAX_ERROR2 ("operator '%s' has no right operand",
			   cpp_token_as_text (pfile, prev_token));
	}

      top = reduce (pfile, top, op.op);
      if (!top)
	goto syntax_error;

      if (op.op == CPP_EOF)
	break;

      switch (op.op)
	{
	case CPP_CLOSE_PAREN:
	  continue;
	case CPP_OR_OR:
	  if (!num_zerop (top->value))
	    pfile->state.skip_eval++;
	  break;
	case CPP_AND_AND:
	case CPP_QUERY:
	  if (num_zerop (top->value))
	    pfile->state.skip_eval++;
	  break;
	case CPP_COLON:
	  if (top->op != CPP_QUERY)
	    SYNTAX_ERROR (" ':' without preceding '?'");
	  if (!num_zerop (top[-1].value)) /* Was '?' condition true?  */
	    pfile->state.skip_eval++;
	  else
	    pfile->state.skip_eval--;
	default:
	  break;
	}

      want_value = true;

      /* Check for and handle stack overflow.  */
      if (++top == pfile->op_limit)
	top = _cpp_expand_op_stack (pfile);

      top->op = op.op;
    }

  /* The controlling macro expression is only valid if we called lex 3
     times: <!> <defined expression> and <EOF>.  push_conditional ()
     checks that we are at top-of-file.  */
  if (pfile->mi_ind_cmacro && !(saw_leading_not && lex_count == 3))
    pfile->mi_ind_cmacro = 0;

  if (top != pfile->op_stack)
    {
      cpp_error (pfile, DL_ICE, "unbalanced stack in #if");
    syntax_error:
      return false;  /* Return false on syntax error.  */
    }

  return !num_zerop (top->value);
}

/* Reduce the operator / value stack if possible, in preparation for
   pushing operator OP.  Returns NULL on error, otherwise the top of
   the stack.  */
static struct op *
reduce (pfile, top, op)
     cpp_reader *pfile;
     struct op *top;
     enum cpp_ttype op;
{
  unsigned int prio;

  if (top->op <= CPP_EQ || top->op > CPP_LAST_CPP_OP + 2)
    {
    bad_op:
      cpp_error (pfile, DL_ICE, "impossible operator '%u'", top->op);
      return 0;
    }

  if (op == CPP_OPEN_PAREN)
    return top;

  /* Decrement the priority of left-associative operators to force a
     reduction with operators of otherwise equal priority.  */
  prio = optab[op].prio - ((optab[op].flags & LEFT_ASSOC) != 0);
  while (prio < optab[top->op].prio)
    {
      if (optab[top->op].arity == UNARY)
	{
	  if (!pfile->state.skip_eval)
	    top[-1].value = num_unary_op (pfile, top->value, top->op);
	  top--;
	}
      else if (optab[top->op].arity == BINARY)
	{
	  if (!pfile->state.skip_eval)
	    top[-1].value = (* (binary_handler) optab[top->op].handler)
	      (pfile, top[-1].value, top->value, top->op);
	  top--;
	}
      /* Anything changing skip_eval has to be handled here.  */
      else switch (top--->op)
	{
	case CPP_OR_OR:
	  if (!num_zerop (top->value))
	    pfile->state.skip_eval--;
	  top->value.low = !num_zerop (top->value) || !num_zerop (top[1].value);
	  top->value.high = 0;
	  top->value.unsignedp = false;
	  top->value.overflow = false;
	  break;

	case CPP_AND_AND:
	  if (num_zerop (top->value))
	    pfile->state.skip_eval--;
	  top->value.low = !num_zerop (top->value) && !num_zerop (top[1].value);
	  top->value.high = 0;
	  top->value.unsignedp = false;
	  top->value.overflow = false;
	  break;

	case CPP_OPEN_PAREN:
	  if (op != CPP_CLOSE_PAREN)
	    {
	      cpp_error (pfile, DL_ERROR, "missing ')' in expression");
	      return 0;
	    }
	  top->value = top[1].value;
	  return top;

	case CPP_COLON:
	  top--;
	  if (!num_zerop (top->value))
	    {
	      pfile->state.skip_eval--;
	      top->value = top[1].value;
	    }
	  else
	    top->value = top[2].value;
	  top->value.unsignedp = (top[1].value.unsignedp
				  || top[2].value.unsignedp);
	  break;

	case CPP_QUERY:
	  cpp_error (pfile, DL_ERROR, "'?' without following ':'");
	  return 0;

	default:
	  goto bad_op;
	}

      if (top->value.overflow && !pfile->state.skip_eval)
	cpp_error (pfile, DL_PEDWARN,
		   "integer overflow in preprocessor expression");
    }

  if (op == CPP_CLOSE_PAREN)
    {
      cpp_error (pfile, DL_ERROR, "missing '(' in expression");
      return 0;
    }

  return top;
}

/* Returns the position of the old top of stack after expansion.  */
struct op *
_cpp_expand_op_stack (pfile)
     cpp_reader *pfile;
{
  size_t old_size = (size_t) (pfile->op_limit - pfile->op_stack);
  size_t new_size = old_size * 2 + 20;

  pfile->op_stack = (struct op *) xrealloc (pfile->op_stack,
					    new_size * sizeof (struct op));
  pfile->op_limit = pfile->op_stack + new_size;

  return pfile->op_stack + old_size;
}

/* Clears the unused high order bits of the number pointed to by PNUM.  */
static cpp_num
num_trim (num, precision)
     cpp_num num;
     size_t precision;
{
  if (precision > PART_PRECISION)
    {
      precision -= PART_PRECISION;
      if (precision < PART_PRECISION)
	num.high &= ((cpp_num_part) 1 << precision) - 1;
    }
  else
    {
      if (precision < PART_PRECISION)
	num.low &= ((cpp_num_part) 1 << precision) - 1;
      num.high = 0;
    }

  return num;
}

/* True iff A (presumed signed) >= 0.  */
static bool
num_positive (num, precision)
     cpp_num num;
     size_t precision;
{
  if (precision > PART_PRECISION)
    {
      precision -= PART_PRECISION;
      return (num.high & (cpp_num_part) 1 << (precision - 1)) == 0;
    }

  return (num.low & (cpp_num_part) 1 << (precision - 1)) == 0;
}

/* Returns the negative of NUM.  */
static cpp_num
num_negate (num, precision)
     cpp_num num;
     size_t precision;
{
  cpp_num copy;

  copy = num;
  num.high = ~num.high;
  num.low = ~num.low;
  if (++num.low == 0)
    num.high++;
  num = num_trim (num, precision);
  num.overflow = (!num.unsignedp && num_eq (num, copy) && !num_zerop (num));

  return num;
}

/* Returns true if A >= B.  */
static bool
num_greater_eq (pa, pb, precision)
     cpp_num pa, pb;
     size_t precision;
{
  bool unsignedp;

  unsignedp = pa.unsignedp || pb.unsignedp;

  if (!unsignedp)
    {
      /* Both numbers have signed type.  If they are of different
       sign, the answer is the sign of A.  */
      unsignedp = num_positive (pa, precision);

      if (unsignedp != num_positive (pb, precision))
	return unsignedp;

      /* Otherwise we can do an unsigned comparison.  */
    }

  return (pa.high > pb.high) || (pa.high == pb.high && pa.low >= pb.low);
}

/* Returns LHS OP RHS, where OP is a bit-wise operation.  */
static cpp_num
num_bitwise_op (pfile, lhs, rhs, op)
     cpp_reader *pfile ATTRIBUTE_UNUSED;
     cpp_num lhs, rhs;
     enum cpp_ttype op;
{
  lhs.overflow = false;
  lhs.unsignedp = lhs.unsignedp || rhs.unsignedp;

  /* As excess precision is zeroed, there is no need to num_trim () as
     these operations cannot introduce a set bit there.  */
  if (op == CPP_AND)
    {
      lhs.low &= rhs.low;
      lhs.high &= rhs.high;
    }
  else if (op == CPP_OR)
    {
      lhs.low |= rhs.low;
      lhs.high |= rhs.high;
    }
  else
    {
      lhs.low ^= rhs.low;
      lhs.high ^= rhs.high;
    }

  return lhs;
}

/* Returns LHS OP RHS, where OP is an inequality.  */
static cpp_num
num_inequality_op (pfile, lhs, rhs, op)
     cpp_reader *pfile;
     cpp_num lhs, rhs;
     enum cpp_ttype op;
{
  bool gte = num_greater_eq (lhs, rhs, CPP_OPTION (pfile, precision));

  if (op == CPP_GREATER_EQ)
    lhs.low = gte;
  else if (op == CPP_LESS)
    lhs.low = !gte;
  else if (op == CPP_GREATER)
    lhs.low = gte && !num_eq (lhs, rhs);
  else /* CPP_LESS_EQ.  */
    lhs.low = !gte || num_eq (lhs, rhs);

  lhs.high = 0;
  lhs.overflow = false;
  lhs.unsignedp = false;
  return lhs;
}

/* Returns LHS OP RHS, where OP is == or !=.  */
static cpp_num
num_equality_op (pfile, lhs, rhs, op)
     cpp_reader *pfile ATTRIBUTE_UNUSED;
     cpp_num lhs, rhs;
     enum cpp_ttype op;
{
  lhs.low = num_eq (lhs, rhs);
  if (op == CPP_NOT_EQ)
    lhs.low = !lhs.low;
  lhs.high = 0;
  lhs.overflow = false;
  lhs.unsignedp = false;
  return lhs;
}

/* Shift NUM, of width PRECISION, right by N bits.  */
static cpp_num
num_rshift (num, precision, n)
     cpp_num num;
     size_t precision, n;
{
  cpp_num_part sign_mask;

  if (num.unsignedp || num_positive (num, precision))
    sign_mask = 0;
  else
    sign_mask = ~(cpp_num_part) 0;

  if (n >= precision)
    num.high = num.low = sign_mask;
  else
    {
      /* Sign-extend.  */
      if (precision < PART_PRECISION)
	num.high = sign_mask, num.low |= sign_mask << precision;
      else if (precision < 2 * PART_PRECISION)
	num.high |= sign_mask << (precision - PART_PRECISION);

      if (n >= PART_PRECISION)
	{
	  n -= PART_PRECISION;
	  num.low = num.high;
	  num.high = sign_mask;
	}

      if (n)
	{
	  num.low = (num.low >> n) | (num.high << (PART_PRECISION - n));
	  num.high = (num.high >> n) | (sign_mask << (PART_PRECISION - n));
	}
    }

  num = num_trim (num, precision);
  num.overflow = false;
  return num;
}

/* Shift NUM, of width PRECISION, left by N bits.  */
static cpp_num
num_lshift (num, precision, n)
     cpp_num num;
     size_t precision, n;
{
  if (n >= precision)
    {
      num.overflow = !num.unsignedp && !num_zerop (num);
      num.high = num.low = 0;
    }
  else
    {
      cpp_num orig, maybe_orig;
      size_t m = n;

      orig = num;
      if (m >= PART_PRECISION)
	{
	  m -= PART_PRECISION;
	  num.high = num.low;
	  num.low = 0;
	}
      if (m)
	{
	  num.high = (num.high << m) | (num.low >> (PART_PRECISION - m));
	  num.low <<= m;
	}
      num = num_trim (num, precision);

      if (num.unsignedp)
	num.overflow = false;
      else
	{
	  maybe_orig = num_rshift (num, precision, n);
	  num.overflow = !num_eq (orig, maybe_orig);
	}
    }

  return num;
}

/* The four unary operators: +, -, ! and ~.  */
static cpp_num
num_unary_op (pfile, num, op)
     cpp_reader *pfile;
     cpp_num num;
     enum cpp_ttype op;
{
  switch (op)
    {
    case CPP_UPLUS:
      if (CPP_WTRADITIONAL (pfile))
	cpp_error (pfile, DL_WARNING,
		   "traditional C rejects the unary plus operator");
      num.overflow = false;
      break;

    case CPP_UMINUS:
      num = num_negate (num, CPP_OPTION (pfile, precision));
      break;

    case CPP_COMPL:
      num.high = ~num.high;
      num.low = ~num.low;
      num = num_trim (num, CPP_OPTION (pfile, precision));
      num.overflow = false;
      break;

    default: /* case CPP_NOT: */
      num.low = num_zerop (num);
      num.high = 0;
      num.overflow = false;
      num.unsignedp = false;
      break;
    }

  return num;
}

/* The various binary operators.  */
static cpp_num
num_binary_op (pfile, lhs, rhs, op)
     cpp_reader *pfile;
     cpp_num lhs, rhs;
     enum cpp_ttype op;
{
  cpp_num result;
  size_t precision = CPP_OPTION (pfile, precision);
  bool gte;
  size_t n;

  switch (op)
    {
      /* Shifts.  */
    case CPP_LSHIFT:
    case CPP_RSHIFT:
      if (!rhs.unsignedp && !num_positive (rhs, precision))
	{
	  /* A negative shift is a positive shift the other way.  */
	  if (op == CPP_LSHIFT)
	    op = CPP_RSHIFT;
	  else
	    op = CPP_LSHIFT;
	  rhs = num_negate (rhs, precision);
	}
      if (rhs.high)
	n = ~0;			/* Maximal.  */
      else
	n = rhs.low;
      if (op == CPP_LSHIFT)
	lhs = num_lshift (lhs, precision, n);
      else
	lhs = num_rshift (lhs, precision, n);
      break;

      /* Min / Max.  */
    case CPP_MIN:
    case CPP_MAX:
      {
	bool unsignedp = lhs.unsignedp || rhs.unsignedp;

	gte = num_greater_eq (lhs, rhs, precision);
	if (op == CPP_MIN)
	  gte = !gte;
	if (!gte)
	  lhs = rhs;
	lhs.unsignedp = unsignedp;
      }
      break;

      /* Arithmetic.  */
    case CPP_MINUS:
      rhs = num_negate (rhs, precision);
    case CPP_PLUS:
      result.low = lhs.low + rhs.low;
      result.high = lhs.high + rhs.high;
      if (result.low < lhs.low)
	result.high++;

      result = num_trim (result, precision);
      result.unsignedp = lhs.unsignedp || rhs.unsignedp;
      if (result.unsignedp)
	result.overflow = false;
      else
	{
	  bool lhsp = num_positive (lhs, precision);
	  result.overflow = (lhsp == num_positive (rhs, precision)
			     && lhsp != num_positive (result, precision));
	}
      return result;

      /* Comma.  */
    default: /* case CPP_COMMA: */
      if (CPP_PEDANTIC (pfile))
	cpp_error (pfile, DL_PEDWARN,
		   "comma operator in operand of #if");
      lhs = rhs;
      break;
    }

  return lhs;
}

/* Multiplies two unsigned cpp_num_parts to give a cpp_num.  This
   cannot overflow.  */
static cpp_num
num_part_mul (lhs, rhs)
     cpp_num_part lhs, rhs;
{
  cpp_num result;
  cpp_num_part middle[2], temp;

  result.low = LOW_PART (lhs) * LOW_PART (rhs);
  result.high = HIGH_PART (lhs) * HIGH_PART (rhs);

  middle[0] = LOW_PART (lhs) * HIGH_PART (rhs);
  middle[1] = HIGH_PART (lhs) * LOW_PART (rhs);

  temp = result.low;
  result.low += LOW_PART (middle[0]) << (PART_PRECISION / 2);
  if (result.low < temp)
    result.high++;

  temp = result.low;
  result.low += LOW_PART (middle[1]) << (PART_PRECISION / 2);
  if (result.low < temp)
    result.high++;

  result.high += HIGH_PART (middle[0]);
  result.high += HIGH_PART (middle[1]);

  return result;
}

/* Multiply two preprocessing numbers.  */
static cpp_num
num_mul (pfile, lhs, rhs, op)
     cpp_reader *pfile;
     cpp_num lhs, rhs;
     enum cpp_ttype op ATTRIBUTE_UNUSED;
{
  cpp_num result, temp;
  bool unsignedp = lhs.unsignedp || rhs.unsignedp;
  bool overflow, negate = false;
  size_t precision = CPP_OPTION (pfile, precision);

  /* Prepare for unsigned multiplication.  */
  if (!unsignedp)
    {
      if (!num_positive (lhs, precision))
	negate = !negate, lhs = num_negate (lhs, precision);
      if (!num_positive (rhs, precision))
	negate = !negate, rhs = num_negate (rhs, precision);
    }

  overflow = lhs.high && rhs.high;
  result = num_part_mul (lhs.low, rhs.low);

  temp = num_part_mul (lhs.high, rhs.low);
  result.high += temp.low;
  if (temp.high)
    overflow = true;

  temp = num_part_mul (lhs.low, rhs.high);
  result.high += temp.low;
  if (temp.high)
    overflow = true;

  temp.low = result.low, temp.high = result.high;
  result = num_trim (result, precision);
  if (!num_eq (result, temp))
    overflow = true;

  if (negate)
    result = num_negate (result, precision);

  if (unsignedp)
    result.overflow = false;
  else
    result.overflow = overflow || (num_positive (result, precision) ^ !negate
				   && !num_zerop (result));
  result.unsignedp = unsignedp;

  return result;
}

/* Divide two preprocessing numbers, returning the answer or the
   remainder depending upon OP.  */
static cpp_num
num_div_op (pfile, lhs, rhs, op)
     cpp_reader *pfile;
     cpp_num lhs, rhs;
     enum cpp_ttype op;
{
  cpp_num result, sub;
  cpp_num_part mask;
  bool unsignedp = lhs.unsignedp || rhs.unsignedp;
  bool negate = false, lhs_neg = false;
  size_t i, precision = CPP_OPTION (pfile, precision);

  /* Prepare for unsigned division.  */
  if (!unsignedp)
    {
      if (!num_positive (lhs, precision))
	negate = !negate, lhs_neg = true, lhs = num_negate (lhs, precision);
      if (!num_positive (rhs, precision))
	negate = !negate, rhs = num_negate (rhs, precision);
    }

  /* Find the high bit.  */
  if (rhs.high)
    {
      i = precision - 1;
      mask = (cpp_num_part) 1 << (i - PART_PRECISION);
      for (; ; i--, mask >>= 1)
	if (rhs.high & mask)
	  break;
    }
  else if (rhs.low)
    {
      if (precision > PART_PRECISION)
	i = precision - PART_PRECISION - 1;
      else
	i = precision - 1;
      mask = (cpp_num_part) 1 << i;
      for (; ; i--, mask >>= 1)
	if (rhs.low & mask)
	  break;
    }
  else
    {
      cpp_error (pfile, DL_ERROR, "division by zero in #if");
      return lhs;
    }

  /* First non-zero bit of RHS is bit I.  Do naive division by
     shifting the RHS fully left, and subtracting from LHS if LHS is
     at least as big, and then repeating but with one less shift.
     This is not very efficient, but is easy to understand.  */

  rhs.unsignedp = true;
  lhs.unsignedp = true;
  i = precision - i - 1;
  sub = num_lshift (rhs, precision, i);

  result.high = result.low = 0;
  for (;;)
    {
      if (num_greater_eq (lhs, sub, precision))
	{
	  lhs = num_binary_op (pfile, lhs, sub, CPP_MINUS);
	  if (i >= PART_PRECISION)
	    result.high |= (cpp_num_part) 1 << (i - PART_PRECISION);
	  else
	    result.low |= (cpp_num_part) 1 << i;
	}
      if (i-- == 0)
	break;
      sub.low = (sub.low >> 1) | (sub.high << (PART_PRECISION - 1));
      sub.high >>= 1;
    }

  /* We divide so that the remainder has the sign of the LHS.  */
  if (op == CPP_DIV)
    {
      result.unsignedp = unsignedp;
      if (unsignedp)
	result.overflow = false;
      else
	{
	  if (negate)
	    result = num_negate (result, precision);
	  result.overflow = num_positive (result, precision) ^ !negate;
	}

      return result;
    }

  /* CPP_MOD.  */
  lhs.unsignedp = unsignedp;
  lhs.overflow = false;
  if (lhs_neg)
    lhs = num_negate (lhs, precision);

  return lhs;
}
