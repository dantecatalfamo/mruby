/*
** sprintf.c - Kernel.#sprintf
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <limits.h>
#include <string.h>
#include <mruby/string.h>
#include <mruby/hash.h>
#include <mruby/numeric.h>
#include <mruby/presym.h>
#ifndef MRB_NO_FLOAT
#include <math.h>
#endif
#include <ctype.h>

#define BIT_DIGITS(N)   (((N)*146)/485 + 1)  /* log2(10) =~ 146/485 */
#define BITSPERDIG MRB_INT_BIT
#define EXTENDSIGN(n, l) (((~0U << (n)) >> (((n)*(l)) % BITSPERDIG)) & ~(~0U << (n)))

mrb_value mrb_str_format(mrb_state *, mrb_int, const mrb_value *, mrb_value);
#ifndef MRB_NO_FLOAT
static void fmt_setup(char*,size_t,int,int,mrb_int,mrb_int);
#endif

static char*
remove_sign_bits(char *str, int base)
{
  char *t;

  t = str;
  if (base == 16) {
    while (*t == 'f') {
      t++;
    }
  }
  else if (base == 8) {
    *t |= EXTENDSIGN(3, strlen(t));
    while (*t == '7') {
      t++;
    }
  }
  else if (base == 2) {
    while (*t == '1') {
      t++;
    }
  }

  return t;
}

static char
sign_bits(int base, const char *p)
{
  char c;

  switch (base) {
  case 16:
    if (*p == 'X') c = 'F';
    else c = 'f';
    break;
  case 8:
    c = '7'; break;
  case 2:
    c = '1'; break;
  default:
    c = '.'; break;
  }
  return c;
}

static mrb_value
mrb_fix2binstr(mrb_state *mrb, mrb_value x, int base)
{
  char buf[66], *b = buf + sizeof buf;
  mrb_int num = mrb_integer(x);
  const int mask = base -1;
  int shift;
#ifdef MRB_INT64
  uint64_t val = (uint64_t)num;
#else
  uint32_t val = (uint32_t)num;
#endif
  char d;

  switch (base) {
  case 2:
    shift = 1;
    break;
  case 8:
    shift = 3;
    break;
  case 16:
    shift = 4;
    break;
  default:
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "invalid radix %d", base);
  }
  if (num == 0) {
    return mrb_str_new_lit(mrb, "0");
  }
  *--b = '\0';
  do {
    *--b = mrb_digitmap[(int)(val & mask)];
  } while (val >>= shift);

  if (num < 0) {
    b = remove_sign_bits(b, base);
    switch (base) {
    case 16: d = 'f'; break;
    case 8:  d = '7'; break;
    case 2:  d = '1'; break;
    default: d = 0;   break;
    }

    if (d && *b != d) {
      *--b = d;
    }
  }

  return mrb_str_new_cstr(mrb, b);
}

#define FNONE  0
#define FSHARP 1
#define FMINUS 2
#define FPLUS  4
#define FZERO  8
#define FSPACE 16
#define FWIDTH 32
#define FPREC  64
#define FPREC0 128

#define CHECK(l) do {\
  while ((l) >= bsiz - blen) {\
    if (bsiz > MRB_INT_MAX/2) mrb_raise(mrb, E_ARGUMENT_ERROR, "too big specifier"); \
    bsiz*=2;\
  }\
  mrb_str_resize(mrb, result, bsiz);\
  buf = RSTRING_PTR(result);\
} while (0)

#define PUSH(s, l) do { \
  CHECK(l);\
  memcpy(&buf[blen], s, l);\
  blen += (mrb_int)(l);\
} while (0)

#define FILL(c, l) do { \
  CHECK(l);\
  memset(&buf[blen], c, l);\
  blen += (l);\
} while (0)

static void
check_next_arg(mrb_state *mrb, int posarg, int nextarg)
{
  switch (posarg) {
  case -1:
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "unnumbered(%d) mixed with numbered", nextarg);
    break;
  case -2:
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "unnumbered(%d) mixed with named", nextarg);
    break;
  default:
    break;
  }
}

static void
check_pos_arg(mrb_state *mrb, int posarg, mrb_int n)
{
  if (posarg > 0) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "numbered(%i) after unnumbered(%d)",
               n, posarg);
  }
  if (posarg == -2) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "numbered(%i) after named", n);
  }
  if (n < 1) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "invalid index - %i$", n);
  }
}

static void
check_name_arg(mrb_state *mrb, int posarg, const char *name, size_t len)
{
  if (posarg > 0) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "named%l after unnumbered(%d)",
               name, len, posarg);
  }
  if (posarg == -1) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "named%l after numbered", name, len);
  }
}

#define GETNEXTARG() (\
  check_next_arg(mrb, posarg, nextarg),\
  (posarg = nextarg++, GETNTHARG(posarg)))

#define GETARG() (!mrb_undef_p(nextvalue) ? nextvalue : GETNEXTARG())

#define GETPOSARG(n) (\
  check_pos_arg(mrb, posarg, n),\
  (posarg = -1, GETNTHARG(n)))

#define GETNTHARG(nth) \
  ((nth >= argc) ? (mrb_raise(mrb, E_ARGUMENT_ERROR, "too few arguments"), mrb_undef_value()) : argv[nth])

#define CHECKNAMEARG(name, len) (\
  check_name_arg(mrb, posarg, name, len),\
  posarg = -2)

#define GETNUM(n, val) do { \
  if (!(p = get_num(mrb, p, end, &(n)))) \
    mrb_raise(mrb, E_ARGUMENT_ERROR, #val " too big 1"); \
} while(0)

#define GETASTER(num) do { \
  mrb_value tmp_v; \
  t = p++; \
  n = 0; \
  GETNUM(n, val); \
  if (*p == '$') { \
    tmp_v = GETPOSARG(n); \
  } \
  else { \
    tmp_v = GETNEXTARG(); \
    p = t; \
  } \
  num = mrb_int(mrb, tmp_v); \
} while (0)

static const char *
get_num(mrb_state *mrb, const char *p, const char *end, mrb_int *valp)
{
  mrb_int next_n = *valp;
  for (; p < end && ISDIGIT(*p); p++) {
    if (mrb_int_mul_overflow(10, next_n, &next_n)) {
      return NULL;
    }
    if (MRB_INT_MAX - (*p - '0') < next_n) {
      return NULL;
    }
    next_n += *p - '0';
  }
  if (p >= end) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "malformed format string - %%*[0-9]");
  }
  *valp = next_n;
  return p;
}

static void
get_hash(mrb_state *mrb, mrb_value *hash, mrb_int argc, const mrb_value *argv)
{
  mrb_value tmp;

  if (!mrb_undef_p(*hash)) return;
  if (argc != 2) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "one hash required");
  }
  tmp = mrb_check_hash_type(mrb, argv[1]);
  if (mrb_nil_p(tmp)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "one hash required");
  }
  *hash = tmp;
}

/*
 *  call-seq:
 *     format(format_string [, arguments...] )   -> string
 *     sprintf(format_string [, arguments...] )  -> string
 *
 *  Returns the string resulting from applying <i>format_string</i> to
 *  any additional arguments.  Within the format string, any characters
 *  other than format sequences are copied to the result.
 *
 *  The syntax of a format sequence is follows.
 *
 *    %[flags][width][.precision]type
 *
 *  A format
 *  sequence consists of a percent sign, followed by optional flags,
 *  width, and precision indicators, then terminated with a field type
 *  character.  The field type controls how the corresponding
 *  <code>sprintf</code> argument is to be interpreted, while the flags
 *  modify that interpretation.
 *
 *  The field type characters are:
 *
 *      Field |  Integer Format
 *      ------+--------------------------------------------------------------
 *        b   | Convert argument as a binary number.
 *            | Negative numbers will be displayed as a two's complement
 *            | prefixed with '..1'.
 *        B   | Equivalent to 'b', but uses an uppercase 0B for prefix
 *            | in the alternative format by #.
 *        d   | Convert argument as a decimal number.
 *        i   | Identical to 'd'.
 *        o   | Convert argument as an octal number.
 *            | Negative numbers will be displayed as a two's complement
 *            | prefixed with '..7'.
 *        u   | Identical to 'd'.
 *        x   | Convert argument as a hexadecimal number.
 *            | Negative numbers will be displayed as a two's complement
 *            | prefixed with '..f' (representing an infinite string of
 *            | leading 'ff's).
 *        X   | Equivalent to 'x', but uses uppercase letters.
 *
 *      Field |  Float Format
 *      ------+--------------------------------------------------------------
 *        e   | Convert floating-point argument into exponential notation
 *            | with one digit before the decimal point as [-]d.dddddde[+-]dd.
 *            | The precision specifies the number of digits after the decimal
 *            | point (defaulting to six).
 *        E   | Equivalent to 'e', but uses an uppercase E to indicate
 *            | the exponent.
 *        f   | Convert floating-point argument as [-]ddd.dddddd,
 *            | where the precision specifies the number of digits after
 *            | the decimal point.
 *        g   | Convert a floating-point number using exponential form
 *            | if the exponent is less than -4 or greater than or
 *            | equal to the precision, or in dd.dddd form otherwise.
 *            | The precision specifies the number of significant digits.
 *        G   | Equivalent to 'g', but use an uppercase 'E' in exponent form.
 *        a   | Convert floating-point argument as [-]0xh.hhhhp[+-]dd,
 *            | which is consisted from optional sign, "0x", fraction part
 *            | as hexadecimal, "p", and exponential part as decimal.
 *        A   | Equivalent to 'a', but use uppercase 'X' and 'P'.
 *
 *      Field |  Other Format
 *      ------+--------------------------------------------------------------
 *        c   | Argument is the numeric code for a single character or
 *            | a single character string itself.
 *        p   | The valuing of argument.inspect.
 *        s   | Argument is a string to be substituted.  If the format
 *            | sequence contains a precision, at most that many characters
 *            | will be copied.
 *        %   | A percent sign itself will be displayed.  No argument taken.
 *
 *  The flags modifies the behavior of the formats.
 *  The flag characters are:
 *
 *    Flag     | Applies to    | Meaning
 *    ---------+---------------+-----------------------------------------
 *    space    | bBdiouxX      | Leave a space at the start of
 *             | aAeEfgG       | non-negative numbers.
 *             | (numeric fmt) | For 'o', 'x', 'X', 'b' and 'B', use
 *             |               | a minus sign with absolute value for
 *             |               | negative values.
 *    ---------+---------------+-----------------------------------------
 *    (digit)$ | all           | Specifies the absolute argument number
 *             |               | for this field.  Absolute and relative
 *             |               | argument numbers cannot be mixed in a
 *             |               | sprintf string.
 *    ---------+---------------+-----------------------------------------
 *     #       | bBoxX         | Use an alternative format.
 *             | aAeEfgG       | For the conversions 'o', increase the precision
 *             |               | until the first digit will be '0' if
 *             |               | it is not formatted as complements.
 *             |               | For the conversions 'x', 'X', 'b' and 'B'
 *             |               | on non-zero, prefix the result with "0x",
 *             |               | "0X", "0b" and "0B", respectively.
 *             |               | For 'a', 'A', 'e', 'E', 'f', 'g', and 'G',
 *             |               | force a decimal point to be added,
 *             |               | even if no digits follow.
 *             |               | For 'g' and 'G', do not remove trailing zeros.
 *    ---------+---------------+-----------------------------------------
 *    +        | bBdiouxX      | Add a leading plus sign to non-negative
 *             | aAeEfgG       | numbers.
 *             | (numeric fmt) | For 'o', 'x', 'X', 'b' and 'B', use
 *             |               | a minus sign with absolute value for
 *             |               | negative values.
 *    ---------+---------------+-----------------------------------------
 *    -        | all           | Left-justify the result of this conversion.
 *    ---------+---------------+-----------------------------------------
 *    0 (zero) | bBdiouxX      | Pad with zeros, not spaces.
 *             | aAeEfgG       | For 'o', 'x', 'X', 'b' and 'B', radix-1
 *             | (numeric fmt) | is used for negative numbers formatted as
 *             |               | complements.
 *    ---------+---------------+-----------------------------------------
 *    *        | all           | Use the next argument as the field width.
 *             |               | If negative, left-justify the result. If the
 *             |               | asterisk is followed by a number and a dollar
 *             |               | sign, use the indicated argument as the width.
 *
 *  Examples of flags:
 *
 *   # '+' and space flag specifies the sign of non-negative numbers.
 *   sprintf("%d", 123)  #=> "123"
 *   sprintf("%+d", 123) #=> "+123"
 *   sprintf("% d", 123) #=> " 123"
 *
 *   # '#' flag for 'o' increases number of digits to show '0'.
 *   # '+' and space flag changes format of negative numbers.
 *   sprintf("%o", 123)   #=> "173"
 *   sprintf("%#o", 123)  #=> "0173"
 *   sprintf("%+o", -123) #=> "-173"
 *   sprintf("%o", -123)  #=> "..7605"
 *   sprintf("%#o", -123) #=> "..7605"
 *
 *   # '#' flag for 'x' add a prefix '0x' for non-zero numbers.
 *   # '+' and space flag disables complements for negative numbers.
 *   sprintf("%x", 123)   #=> "7b"
 *   sprintf("%#x", 123)  #=> "0x7b"
 *   sprintf("%+x", -123) #=> "-7b"
 *   sprintf("%x", -123)  #=> "..f85"
 *   sprintf("%#x", -123) #=> "0x..f85"
 *   sprintf("%#x", 0)    #=> "0"
 *
 *   # '#' for 'X' uses the prefix '0X'.
 *   sprintf("%X", 123)  #=> "7B"
 *   sprintf("%#X", 123) #=> "0X7B"
 *
 *   # '#' flag for 'b' add a prefix '0b' for non-zero numbers.
 *   # '+' and space flag disables complements for negative numbers.
 *   sprintf("%b", 123)   #=> "1111011"
 *   sprintf("%#b", 123)  #=> "0b1111011"
 *   sprintf("%+b", -123) #=> "-1111011"
 *   sprintf("%b", -123)  #=> "..10000101"
 *   sprintf("%#b", -123) #=> "0b..10000101"
 *   sprintf("%#b", 0)    #=> "0"
 *
 *   # '#' for 'B' uses the prefix '0B'.
 *   sprintf("%B", 123)  #=> "1111011"
 *   sprintf("%#B", 123) #=> "0B1111011"
 *
 *   # '#' for 'e' forces to show the decimal point.
 *   sprintf("%.0e", 1)  #=> "1e+00"
 *   sprintf("%#.0e", 1) #=> "1.e+00"
 *
 *   # '#' for 'f' forces to show the decimal point.
 *   sprintf("%.0f", 1234)  #=> "1234"
 *   sprintf("%#.0f", 1234) #=> "1234."
 *
 *   # '#' for 'g' forces to show the decimal point.
 *   # It also disables stripping lowest zeros.
 *   sprintf("%g", 123.4)   #=> "123.4"
 *   sprintf("%#g", 123.4)  #=> "123.400"
 *   sprintf("%g", 123456)  #=> "123456"
 *   sprintf("%#g", 123456) #=> "123456."
 *
 *  The field width is an optional integer, followed optionally by a
 *  period and a precision.  The width specifies the minimum number of
 *  characters that will be written to the result for this field.
 *
 *  Examples of width:
 *
 *   # padding is done by spaces,       width=20
 *   # 0 or radix-1.             <------------------>
 *   sprintf("%20d", 123)   #=> "                 123"
 *   sprintf("%+20d", 123)  #=> "                +123"
 *   sprintf("%020d", 123)  #=> "00000000000000000123"
 *   sprintf("%+020d", 123) #=> "+0000000000000000123"
 *   sprintf("% 020d", 123) #=> " 0000000000000000123"
 *   sprintf("%-20d", 123)  #=> "123                 "
 *   sprintf("%-+20d", 123) #=> "+123                "
 *   sprintf("%- 20d", 123) #=> " 123                "
 *   sprintf("%020x", -123) #=> "..ffffffffffffffff85"
 *
 *  For
 *  numeric fields, the precision controls the number of decimal places
 *  displayed.  For string fields, the precision determines the maximum
 *  number of characters to be copied from the string.  (Thus, the format
 *  sequence <code>%10.10s</code> will always contribute exactly ten
 *  characters to the result.)
 *
 *  Examples of precisions:
 *
 *   # precision for 'd', 'o', 'x' and 'b' is
 *   # minimum number of digits               <------>
 *   sprintf("%20.8d", 123)  #=> "            00000123"
 *   sprintf("%20.8o", 123)  #=> "            00000173"
 *   sprintf("%20.8x", 123)  #=> "            0000007b"
 *   sprintf("%20.8b", 123)  #=> "            01111011"
 *   sprintf("%20.8d", -123) #=> "           -00000123"
 *   sprintf("%20.8o", -123) #=> "            ..777605"
 *   sprintf("%20.8x", -123) #=> "            ..ffff85"
 *   sprintf("%20.8b", -11)  #=> "            ..110101"
 *
 *   # "0x" and "0b" for '#x' and '#b' is not counted for
 *   # precision but "0" for '#o' is counted.  <------>
 *   sprintf("%#20.8d", 123)  #=> "            00000123"
 *   sprintf("%#20.8o", 123)  #=> "            00000173"
 *   sprintf("%#20.8x", 123)  #=> "          0x0000007b"
 *   sprintf("%#20.8b", 123)  #=> "          0b01111011"
 *   sprintf("%#20.8d", -123) #=> "           -00000123"
 *   sprintf("%#20.8o", -123) #=> "            ..777605"
 *   sprintf("%#20.8x", -123) #=> "          0x..ffff85"
 *   sprintf("%#20.8b", -11)  #=> "          0b..110101"
 *
 *   # precision for 'e' is number of
 *   # digits after the decimal point           <------>
 *   sprintf("%20.8e", 1234.56789) #=> "      1.23456789e+03"
 *
 *   # precision for 'f' is number of
 *   # digits after the decimal point               <------>
 *   sprintf("%20.8f", 1234.56789) #=> "       1234.56789000"
 *
 *   # precision for 'g' is number of
 *   # significant digits                          <------->
 *   sprintf("%20.8g", 1234.56789) #=> "           1234.5679"
 *
 *   #                                         <------->
 *   sprintf("%20.8g", 123456789)  #=> "       1.2345679e+08"
 *
 *   # precision for 's' is
 *   # maximum number of characters                    <------>
 *   sprintf("%20.8s", "string test") #=> "            string t"
 *
 *  Examples:
 *
 *     sprintf("%d %04x", 123, 123)               #=> "123 007b"
 *     sprintf("%08b '%4s'", 123, 123)            #=> "01111011 ' 123'"
 *     sprintf("%1$*2$s %2$d %1$s", "hello", 8)   #=> "   hello 8 hello"
 *     sprintf("%1$*2$s %2$d", "hello", -8)       #=> "hello    -8"
 *     sprintf("%+g:% g:%-g", 1.23, 1.23, 1.23)   #=> "+1.23: 1.23:1.23"
 *     sprintf("%u", -123)                        #=> "-123"
 *
 *  For more complex formatting, Ruby supports a reference by name.
 *  %<name>s style uses format style, but %{name} style doesn't.
 *
 *  Examples:
 *    sprintf("%<foo>d : %<bar>f", { :foo => 1, :bar => 2 })
 *      #=> 1 : 2.000000
 *    sprintf("%{foo}f", { :foo => 1 })
 *      # => "1f"
 */

static mrb_value
mrb_f_sprintf(mrb_state *mrb, mrb_value obj)
{
  mrb_int argc;
  const mrb_value *argv;

  mrb_get_args(mrb, "*", &argv, &argc);

  if (argc <= 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "too few arguments");
    return mrb_nil_value();
  }
  else {
    return mrb_str_format(mrb, argc - 1, argv + 1, argv[0]);
  }
}

static int
mrb_int2str(char *buf, size_t len, mrb_int n)
{
#ifdef MRB_NO_STDIO
  char *bufend = buf + len;
  char *p = bufend - 1;

  if (len < 1) return -1;

  *p -- = '\0';
  len --;

  if (n < 0) {
    if (len < 1) return -1;

    *p -- = '-';
    len --;
    n = -n;
  }

  if (n > 0) {
    for (; n > 0; len --, n /= 10) {
      if (len < 1) return -1;

      *p -- = '0' + (n % 10);
    }
    p ++;
  }
  else if (len > 0) {
    *p = '0';
    len --;
  }
  else {
    return -1;
  }

  memmove(buf, p, bufend - p);

  return bufend - p - 1;
#else
  return snprintf(buf, len, "%" MRB_PRId, n);
#endif /* MRB_NO_STDIO */
}

mrb_value
mrb_str_format(mrb_state *mrb, mrb_int argc, const mrb_value *argv, mrb_value fmt)
{
  const char *p, *end;
  char *buf;
  mrb_int blen;
  mrb_int bsiz;
  mrb_value result;
  mrb_int n;
  mrb_int width;
  mrb_int prec;
  int nextarg = 1;
  int posarg = 0;
  mrb_value nextvalue;
  mrb_value str;
  mrb_value hash = mrb_undef_value();

#define CHECK_FOR_WIDTH(f)                                              \
  if ((f) & FWIDTH) {                                                   \
    mrb_raise(mrb, E_ARGUMENT_ERROR, "width given twice");              \
    }                                                                   \
  if ((f) & FPREC0) {                                                   \
    mrb_raise(mrb, E_ARGUMENT_ERROR, "width after precision");          \
  }
#define CHECK_FOR_FLAGS(f)                                              \
  if ((f) & FWIDTH) {                                                   \
    mrb_raise(mrb, E_ARGUMENT_ERROR, "flag after width");               \
  }                                                                     \
  if ((f) & FPREC0) {                                                   \
    mrb_raise(mrb, E_ARGUMENT_ERROR, "flag after precision");           \
  }

  ++argc;
  --argv;
  mrb_to_str(mrb, fmt);
  p = RSTRING_PTR(fmt);
  end = p + RSTRING_LEN(fmt);
  blen = 0;
  bsiz = 120;
  result = mrb_str_new_capa(mrb, bsiz);
  buf = RSTRING_PTR(result);
  memset(buf, 0, bsiz);

  for (; p < end; p++) {
    const char *t;
    mrb_sym id = 0;
    int flags = FNONE;

    for (t = p; t < end && *t != '%'; t++) ;
    if (t + 1 == end) ++t;
    PUSH(p, t - p);
    if (t >= end)
      goto sprint_exit; /* end of fmt string */

    p = t + 1;    /* skip '%' */

    width = prec = -1;
    nextvalue = mrb_undef_value();

retry:
    switch (*p) {
      default:
        mrb_raisef(mrb, E_ARGUMENT_ERROR, "malformed format string - %%%c", *p);
        break;

      case ' ':
        CHECK_FOR_FLAGS(flags);
        flags |= FSPACE;
        p++;
        goto retry;

      case '#':
        CHECK_FOR_FLAGS(flags);
        flags |= FSHARP;
        p++;
        goto retry;

      case '+':
        CHECK_FOR_FLAGS(flags);
        flags |= FPLUS;
        p++;
        goto retry;

      case '-':
        CHECK_FOR_FLAGS(flags);
        flags |= FMINUS;
        p++;
        goto retry;

      case '0':
        CHECK_FOR_FLAGS(flags);
        flags |= FZERO;
        p++;
        goto retry;

      case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
        n = 0;
        GETNUM(n, width);
        if (*p == '$') {
          if (!mrb_undef_p(nextvalue)) {
            mrb_raisef(mrb, E_ARGUMENT_ERROR, "value given twice - %i$", n);
          }
          nextvalue = GETPOSARG(n);
          p++;
          goto retry;
        }
        CHECK_FOR_WIDTH(flags);
        width = n;
        flags |= FWIDTH;
        goto retry;

      case '<':
      case '{': {
        const char *start = p;
        char term = (*p == '<') ? '>' : '}';

        for (; p < end && *p != term; )
          p++;
        if (id) {
          mrb_raisef(mrb, E_ARGUMENT_ERROR, "name%l after <%n>",
                     start, p - start + 1, id);
        }
        CHECKNAMEARG(start, p - start + 1);
        get_hash(mrb, &hash, argc, argv);
        id = mrb_intern_check(mrb, start + 1, p - start - 1);
        if (id) {
          nextvalue = mrb_hash_fetch(mrb, hash, mrb_symbol_value(id), mrb_undef_value());
        }
        if (!id || mrb_undef_p(nextvalue)) {
          mrb_raisef(mrb, E_KEY_ERROR, "key%l not found", start, p - start + 1);
        }
        if (term == '}') goto format_s;
        p++;
        goto retry;
      }

      case '*':
        CHECK_FOR_WIDTH(flags);
        flags |= FWIDTH;
        GETASTER(width);
        if (width < 0) {
          flags |= FMINUS;
          width = -width;
        }
        p++;
        goto retry;

      case '.':
        if (flags & FPREC0) {
          mrb_raise(mrb, E_ARGUMENT_ERROR, "precision given twice");
        }
        flags |= FPREC|FPREC0;

        prec = 0;
        p++;
        if (*p == '*') {
          GETASTER(prec);
          if (prec < 0) {  /* ignore negative precision */
            flags &= ~FPREC;
          }
          p++;
          goto retry;
        }

        GETNUM(prec, precision);
        goto retry;

      case '\n':
      case '\0':
        p--;
        /* fallthrough */
      case '%':
        if (flags != FNONE) {
          mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid format character - %");
        }
        PUSH("%", 1);
        break;

      case 'c': {
        mrb_value val = GETARG();
        mrb_value tmp;
        char *c;

        tmp = mrb_check_string_type(mrb, val);
        if (!mrb_nil_p(tmp)) {
          if (RSTRING_LEN(tmp) != 1) {
            mrb_raise(mrb, E_ARGUMENT_ERROR, "%c requires a character");
          }
        }
        else if (mrb_integer_p(val)) {
          mrb_int n = mrb_integer(val);
#ifndef MRB_UTF8_STRING
          char buf[1];

          buf[0] = (char)n&0xff;
          tmp = mrb_str_new(mrb, buf, 1);
#else
          if (n < 0x80) {
            char buf[1];

            buf[0] = (char)n;
            tmp = mrb_str_new(mrb, buf, 1);
          }
          else {
            tmp = mrb_funcall_id(mrb, val, MRB_SYM(chr), 0);
            mrb_check_type(mrb, tmp, MRB_TT_STRING);
          }
#endif
        }
        else {
          mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid character");
        }
        c = RSTRING_PTR(tmp);
        n = RSTRING_LEN(tmp);
        if (!(flags & FWIDTH)) {
          PUSH(c, n);
        }
        else if ((flags & FMINUS)) {
          PUSH(c, n);
          if (width>0) FILL(' ', width-1);
        }
        else {
          if (width>0) FILL(' ', width-1);
          PUSH(c, n);
        }
      }
      break;

      case 's':
      case 'p':
  format_s:
      {
        mrb_value arg = GETARG();
        mrb_int len;
        mrb_int slen;

        if (*p == 'p') arg = mrb_inspect(mrb, arg);
        str = mrb_obj_as_string(mrb, arg);
        len = RSTRING_LEN(str);
        if (RSTRING(result)->flags & MRB_STR_EMBED) {
          mrb_int tmp_n = len;
          RSTRING(result)->flags &= ~MRB_STR_EMBED_LEN_MASK;
          RSTRING(result)->flags |= tmp_n << MRB_STR_EMBED_LEN_SHIFT;
        }
        else {
          RSTRING(result)->as.heap.len = blen;
        }
        if (flags&(FPREC|FWIDTH)) {
          slen = RSTRING_LEN(str);
          if (slen < 0) {
            mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid mbstring sequence");
          }
          if ((flags&FPREC) && (prec < slen)) {
            char *p = RSTRING_PTR(str) + prec;
            slen = prec;
            len = (mrb_int)(p - RSTRING_PTR(str));
          }
          /* need to adjust multi-byte string pos */
          if ((flags&FWIDTH) && (width > slen)) {
            width -= (int)slen;
            if (!(flags&FMINUS)) {
              FILL(' ', width);
            }
            PUSH(RSTRING_PTR(str), len);
            if (flags&FMINUS) {
              FILL(' ', width);
            }
            break;
          }
        }
        PUSH(RSTRING_PTR(str), len);
      }
      break;

      case 'd':
      case 'i':
      case 'o':
      case 'x':
      case 'X':
      case 'b':
      case 'B':
      case 'u': {
        mrb_value val = GETARG();
        char nbuf[69], *s;
        const char *prefix = NULL;
        int sign = 0, dots = 0;
        char sc = 0;
        mrb_int v = 0;
        int base;
        mrb_int len;

        if (flags & FSHARP) {
          switch (*p) {
            case 'o': prefix = "0"; break;
            case 'x': prefix = "0x"; break;
            case 'X': prefix = "0X"; break;
            case 'b': prefix = "0b"; break;
            case 'B': prefix = "0B"; break;
            default: break;
          }
        }

  bin_retry:
        switch (mrb_type(val)) {
#ifndef MRB_NO_FLOAT
          case MRB_TT_FLOAT:
            val = mrb_flo_to_fixnum(mrb, val);
            if (mrb_integer_p(val)) goto bin_retry;
            break;
#endif
          case MRB_TT_STRING:
            val = mrb_str_to_inum(mrb, val, 0, TRUE);
            goto bin_retry;
          case MRB_TT_INTEGER:
            v = mrb_integer(val);
            break;
          default:
            val = mrb_Integer(mrb, val);
            goto bin_retry;
        }

        switch (*p) {
          case 'o':
            base = 8; break;
          case 'x':
          case 'X':
            base = 16; break;
          case 'b':
          case 'B':
            base = 2; break;
          case 'u':
          case 'd':
          case 'i':
            sign = 1;
            /* fall through */
          default:
            base = 10; break;
        }

        if (sign) {
          if (v >= 0) {
            if (flags & FPLUS) {
              sc = '+';
              width--;
            }
            else if (flags & FSPACE) {
              sc = ' ';
              width--;
            }
          }
          else {
            sc = '-';
            width--;
          }
          mrb_assert(base == 10);
          mrb_int2str(nbuf, sizeof(nbuf)-1, v);
          s = nbuf;
          if (v < 0) s++;       /* skip minus sign */
        }
        else {
          s = nbuf;
          if (v < 0) {
            dots = 1;
            val = mrb_fix2binstr(mrb, mrb_int_value(mrb, v), base);
          }
          else {
            val = mrb_fixnum_to_str(mrb, mrb_int_value(mrb, v), base);
          }
          strncpy(++s, RSTRING_PTR(val), sizeof(nbuf)-2);
        }
        {
          size_t size;
          size = strlen(s);
          /* PARANOID: assert(size <= MRB_INT_MAX) */
          len = (mrb_int)size;
        }

        if (*p == 'X') {
          char *pp = s;
          int c;
          while ((c = (int)(unsigned char)*pp) != 0) {
            *pp = toupper(c);
            pp++;
          }
        }

        if (prefix && !prefix[1]) { /* octal */
          if (dots) {
            prefix = NULL;
          }
          else if (len == 1 && *s == '0') {
            len = 0;
            if (flags & FPREC) prec--;
          }
          else if ((flags & FPREC) && (prec > len)) {
            prefix = NULL;
          }
        }
        else if (len == 1 && *s == '0') {
          prefix = NULL;
        }

        if (prefix) {
          size_t size;
          size = strlen(prefix);
          /* PARANOID: assert(size <= MRB_INT_MAX).
           *  this check is absolutely paranoid. */
          width -= (mrb_int)size;
        }

        if ((flags & (FZERO|FMINUS|FPREC)) == FZERO) {
          prec = width;
          width = 0;
        }
        else {
          if (prec < len) {
            if (!prefix && prec == 0 && len == 1 && *s == '0') len = 0;
            prec = len;
          }
          width -= prec;
        }

        if (!(flags&FMINUS) && width > 0) {
          FILL(' ', width);
          width = 0;
        }

        if (sc) PUSH(&sc, 1);

        if (prefix) {
          int plen = (int)strlen(prefix);
          PUSH(prefix, plen);
        }
        if (dots) {
          prec -= 2;
          width -= 2;
          PUSH("..", 2);
        }

        if (prec > len) {
          CHECK(prec - len);
          if ((flags & (FMINUS|FPREC)) != FMINUS) {
            char c = '0';
            FILL(c, prec - len);
          } else if (v < 0) {
            char c = sign_bits(base, p);
            FILL(c, prec - len);
          }
        }
        PUSH(s, len);
        if (width > 0) {
          FILL(' ', width);
        }
      }
      break;

#ifndef MRB_NO_FLOAT
      case 'f':
      case 'g':
      case 'G':
      case 'e':
      case 'E':
      case 'a':
      case 'A': {
        mrb_value val = GETARG();
        double fval;
        mrb_int need = 6;
        char fbuf[64];

        fval = mrb_float(mrb_Float(mrb, val));
        if (!isfinite(fval)) {
          const char *expr;
          const mrb_int elen = 3;
          char sign = '\0';

          if (isnan(fval)) {
            expr = "NaN";
          }
          else {
            expr = "Inf";
          }
          need = elen;
          if (!isnan(fval) && fval < 0.0)
            sign = '-';
          else if (flags & (FPLUS|FSPACE))
            sign = (flags & FPLUS) ? '+' : ' ';
          if (sign)
            ++need;
          if ((flags & FWIDTH) && need < width)
            need = width;

          if (need < 0) {
            mrb_raise(mrb, E_ARGUMENT_ERROR, "width too big");
          }
          FILL(' ', need);
          if (flags & FMINUS) {
            if (sign)
              buf[blen - need--] = sign;
            memcpy(&buf[blen - need], expr, elen);
          }
          else {
            if (sign)
              buf[blen - elen - 1] = sign;
            memcpy(&buf[blen - elen], expr, elen);
          }
          break;
        }

        need = 0;
        if (*p != 'e' && *p != 'E') {
          int i;
          frexp(fval, &i);
          if (i > 0)
            need = BIT_DIGITS(i);
        }
        if (need > MRB_INT_MAX - ((flags&FPREC) ? prec : 6)) {
        too_big_width:
          mrb_raise(mrb, E_ARGUMENT_ERROR,
                    (width > prec ? "width too big" : "prec too big"));
        }
        need += (flags&FPREC) ? prec : 6;
        if ((flags&FWIDTH) && need < width)
          need = width;
        if (need > MRB_INT_MAX - 20) {
          goto too_big_width;
        }
        need += 20;

        CHECK(need);
        fmt_setup(fbuf, sizeof(fbuf), *p, flags, width, prec);
        n = mrb_float_to_cstr(mrb, &buf[blen], need, fbuf, fval);
        if (n < 0 || n >= need) {
          mrb_raise(mrb, E_RUNTIME_ERROR, "formatting error");
        }
        blen += n;
      }
      break;
#endif
    }
    flags = FNONE;
  }

  sprint_exit:
#if 0
  /* XXX - We cannot validate the number of arguments if (digit)$ style used.
   */
  if (posarg >= 0 && nextarg < argc) {
    const char *mesg = "too many arguments for format string";
    if (mrb_test(ruby_debug)) mrb_raise(mrb, E_ARGUMENT_ERROR, mesg);
    if (mrb_test(ruby_verbose)) mrb_warn(mrb, "%s", mesg);
  }
#endif
  mrb_str_resize(mrb, result, blen);

  return result;
}

#ifndef MRB_NO_FLOAT
static void
fmt_setup(char *buf, size_t size, int c, int flags, mrb_int width, mrb_int prec)
{
  char *end = buf + size;
  int n;

  *buf++ = '%';
  if (flags & FSHARP) *buf++ = '#';
  if (flags & FPLUS)  *buf++ = '+';
  if (flags & FMINUS) *buf++ = '-';
  if (flags & FZERO)  *buf++ = '0';
  if (flags & FSPACE) *buf++ = ' ';

  if (flags & FWIDTH) {
    n = mrb_int2str(buf, end - buf, width);
    buf += n;
  }

  if (flags & FPREC) {
    *buf ++ = '.';
    n = mrb_int2str(buf, end - buf, prec);
    buf += n;
  }

  *buf++ = c;
  *buf = '\0';
}
#endif

void
mrb_mruby_sprintf_gem_init(mrb_state *mrb)
{
  struct RClass *krn = mrb->kernel_module;
  mrb_define_module_function(mrb, krn, "sprintf", mrb_f_sprintf, MRB_ARGS_ANY());
  mrb_define_module_function(mrb, krn, "format",  mrb_f_sprintf, MRB_ARGS_ANY());
}

void
mrb_mruby_sprintf_gem_final(mrb_state *mrb)
{
}
