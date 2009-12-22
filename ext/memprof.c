#define _GNU_SOURCE
#include <err.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sysexits.h>
#include <sys/mman.h>
#include <err.h>

#include <ruby.h>
#include <ruby/st.h>
#include <ruby/intern.h>
#include <node.h>

#include "arch.h"
#include "bin_api.h"

size_t pagesize;
void *text_segment = NULL;
unsigned long text_segment_len = 0;

/*
   trampoline specific stuff
 */
struct tramp_st2_entry *tramp_table = NULL;
size_t tramp_size = 0;

/*
   inline trampoline specific stuff
 */
size_t inline_tramp_size = 0;
struct inline_tramp_st2_entry *inline_tramp_table = NULL;

/*
 * bleak_house stuff
 */
static VALUE eUnsupported;
static int track_objs = 0;
static st_table *objs = NULL;

struct obj_track {
  VALUE obj;
  char *source;
  int line;
};

static VALUE
newobj_tramp()
{
  VALUE ret = rb_newobj();
  struct obj_track *tracker = NULL;

  if (track_objs) {
    tracker = malloc(sizeof(*tracker));

    if (tracker) {
#if 0
      while (RUBY_VM_VALID_CONTROL_FRAME_P(cfp, end_cfp)) {
        rb_iseq_t *iseq = cfp->iseq;
        if (cfp->iseq && cfp->pc) {
          tracker->line = rb_vm_get_sourceline(cfp);
          tracker->source = strdup(RSTRING_PTR(iseq->filename));
        }

        cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(cfp);
      }
      if (ruby_current_node && ruby_current_node->nd_file && *ruby_current_node->nd_file) {
        tracker->source = strdup(ruby_current_node->nd_file);
        tracker->line = nd_line(ruby_current_node);
      } else if (ruby_sourcefile) {
        tracker->source = strdup(ruby_sourcefile);
        tracker->line = ruby_sourceline;
      } else {
        tracker->source = strdup("__null__");
        tracker->line = 0;
      }
#endif
      tracker->source = strdup("im lazy");
      tracker->line = 420;
      tracker->obj = ret;
      st_insert(objs, (st_data_t)ret, (st_data_t)tracker);
    } else {
      fprintf(stderr, "Warning, unable to allocate a tracker. You are running dangerously low on RAM!\n");
    }
  }

  return ret;
}

static void
freelist_tramp(unsigned long rval)
{
  struct obj_track *tracker = NULL;
  if (track_objs) {
    st_delete(objs, (st_data_t *) &rval, (st_data_t *) &tracker);
    if (tracker) {
      free(tracker->source);
      free(tracker);
    }
  }
}

static int
objs_free(st_data_t key, st_data_t record, st_data_t arg)
{
  struct obj_track *tracker = (struct obj_track *)record;
  free(tracker->source);
  free(tracker);
  return ST_DELETE;
}

static int
objs_tabulate(st_data_t key, st_data_t record, st_data_t arg)
{
  st_table *table = (st_table *)arg;
  struct obj_track *tracker = (struct obj_track *)record;
  char *source_key = NULL;
  unsigned long count = 0;
  char *type = NULL;

  switch (TYPE(tracker->obj)) {
    case T_NONE:
      type = "__none__"; break;
    case T_UNDEF:
      type = "__undef__"; break;
    case T_NODE:
      type = "__node__"; break;
    default:
      if (RBASIC(tracker->obj)->klass) {
        type = (char*) rb_obj_classname(tracker->obj);
      } else {
        type = "__unknown__";
      }
  }

  asprintf(&source_key, "%s:%d:%s", tracker->source, tracker->line, type);
  st_lookup(table, (st_data_t)source_key, (st_data_t *)&count);
  if (st_insert(table, (st_data_t)source_key, ++count)) {
    free(source_key);
  }

  return ST_CONTINUE;
}

struct results {
  char **entries;
  unsigned long num_entries;
};

static int
objs_to_array(st_data_t key, st_data_t record, st_data_t arg)
{
  struct results *res = (struct results *)arg;
  unsigned long count = (unsigned long)record;
  char *source = (char *)key;
  
  asprintf(&(res->entries[res->num_entries++]), "%7li %s", count, source);

  free(source);
  return ST_DELETE;
}

static VALUE
memprof_start(VALUE self)
{
  if (track_objs == 1)
    return Qfalse;

  track_objs = 1;
  return Qtrue;
}

static VALUE
memprof_stop(VALUE self)
{
  if (track_objs == 0)
    return Qfalse;

  track_objs = 0;
  st_foreach(objs, objs_free, (st_data_t)0);
  return Qtrue;
}

static int
memprof_strcmp(const void *obj1, const void *obj2)
{
  char *str1 = *(char **)obj1;
  char *str2 = *(char **)obj2;
  return strcmp(str2, str1);
}

static VALUE
memprof_stats(int argc, VALUE *argv, VALUE self)
{
  st_table *tmp_table;
  struct results res;
  int i;
  VALUE str;
  FILE *out = NULL;

  if (!track_objs)
    rb_raise(rb_eRuntimeError, "object tracking disabled, call Memprof.start first");

  rb_scan_args(argc, argv, "01", &str);

  if (RTEST(str)) {
    out = fopen(StringValueCStr(str), "w");
    if (!out)
      rb_raise(rb_eArgError, "unable to open output file");
  }

  track_objs = 0;

  tmp_table = st_init_strtable();
  st_foreach(objs, objs_tabulate, (st_data_t)tmp_table);

  res.num_entries = 0;
  res.entries = malloc(sizeof(char*) * tmp_table->num_entries);

  st_foreach(tmp_table, objs_to_array, (st_data_t)&res);
  st_free_table(tmp_table);

  qsort(res.entries, res.num_entries, sizeof(char*), &memprof_strcmp);

  for (i=0; i < res.num_entries; i++) {
    fprintf(out ? out : stderr, "%s\n", res.entries[i]);
    free(res.entries[i]);
  }
  free(res.entries);

  if (out)
    fclose(out);

  track_objs = 1;
  return Qnil;
}

static VALUE
memprof_stats_bang(int argc, VALUE *argv, VALUE self)
{
  memprof_stats(argc, argv, self);
  st_foreach(objs, objs_free, (st_data_t)0);
  return Qnil;
}

static VALUE
memprof_track(int argc, VALUE *argv, VALUE self)
{
  if (!rb_block_given_p())
    rb_raise(rb_eArgError, "block required");

  memprof_start(self);
  rb_yield(Qnil);
  memprof_stats(argc, argv, self);
  memprof_stop(self);
  return Qnil;
}

#include <yajl/yajl_gen.h>
#include <stdarg.h>
#include "ruby/re.h"

yajl_gen_status
yajl_gen_cstr(yajl_gen gen, const char * str)
{
  if (!str || str[0] == 0)
    return yajl_gen_null(gen);
  else
    return yajl_gen_string(gen, (unsigned char *)str, strlen(str));
}

yajl_gen_status
yajl_gen_format(yajl_gen gen, char *format, ...)
{
  va_list args;
  char *str;
  yajl_gen_status ret;

  va_start(args, format);
  vasprintf(&str, format, args);
  va_end(args);

  ret = yajl_gen_string(gen, (unsigned char *)str, strlen(str));
  free(str);
  return ret;
}

yajl_gen_status
yajl_gen_value(yajl_gen gen, VALUE obj)
{
  if (FIXNUM_P(obj))
    return yajl_gen_integer(gen, FIX2INT(obj));
  else if (NIL_P(obj) || obj == Qundef)
    return yajl_gen_null(gen);
  else if (obj == Qtrue)
    return yajl_gen_bool(gen, 1);
  else if (obj == Qfalse)
    return yajl_gen_bool(gen, 0);
  else if (SYMBOL_P(obj))
    return yajl_gen_format(gen, ":%s", rb_id2name(SYM2ID(obj)));
  else
    return yajl_gen_format(gen, "0x%x", obj);
}

static int
each_hash_entry(st_data_t key, st_data_t record, st_data_t arg)
{
  yajl_gen gen = (yajl_gen)arg;
  VALUE k = (VALUE)key;
  VALUE v = (VALUE)record;

  yajl_gen_value(gen, k);
  yajl_gen_value(gen, v);

  return ST_CONTINUE;
}

static int
each_ivar(st_data_t key, st_data_t record, st_data_t arg)
{
  yajl_gen gen = (yajl_gen)arg;
  ID id = (ID)key;
  VALUE val = (VALUE)record;

  yajl_gen_cstr(gen, rb_id2name(id));
  yajl_gen_value(gen, val);

  return ST_CONTINUE;
}

char *
nd_type_str(VALUE obj)
{
  switch(nd_type(obj)) {
    #define ND(type) case NODE_##type: return #type;
    ND(METHOD);      ND(FBODY);      ND(CFUNC);      ND(SCOPE);
    ND(BLOCK);       ND(IF);         ND(CASE);       ND(WHEN);
    ND(OPT_N);       ND(WHILE);      ND(UNTIL);      ND(ITER);
    ND(FOR);         ND(BREAK);      ND(NEXT);       ND(REDO);
    ND(RETRY);       ND(BEGIN);      ND(RESCUE);     ND(RESBODY);
    ND(ENSURE);      ND(AND);        ND(OR);         ND(MASGN);
    ND(LASGN);       ND(DASGN);      ND(DASGN_CURR);
    ND(GASGN);       ND(IASGN);      ND(IASGN2);     ND(CDECL);
    ND(CVASGN);      ND(CVDECL);     ND(OP_ASGN1);   ND(OP_ASGN2);
    ND(OP_ASGN_AND); ND(OP_ASGN_OR); ND(CALL);       ND(FCALL);
    ND(VCALL);       ND(SUPER);      ND(ZSUPER);     ND(ARRAY);
    ND(ZARRAY);      ND(VALUES);     ND(HASH);       ND(RETURN);
    ND(YIELD);       ND(LVAR);       ND(DVAR);       ND(GVAR);
    ND(IVAR);        ND(CONST);      ND(CVAR);       ND(NTH_REF);
    ND(BACK_REF);    ND(MATCH);      ND(MATCH2);     ND(MATCH3);
    ND(LIT);         ND(STR);        ND(DSTR);       ND(XSTR);
    ND(DXSTR);       ND(EVSTR);      ND(DREGX);      ND(DREGX_ONCE);
    ND(ARGS);        ND(ARGS_AUX);   ND(OPT_ARG);    ND(POSTARG);
    ND(ARGSCAT);     ND(ARGSPUSH);   ND(SPLAT);      ND(TO_ARY);
    ND(BLOCK_ARG);   ND(BLOCK_PASS); ND(DEFN);       ND(DEFS);
    ND(ALIAS);       ND(VALIAS);     ND(UNDEF);      ND(CLASS);
    ND(MODULE);      ND(SCLASS);     ND(COLON2);     ND(COLON3);
    ND(DOT2);        ND(DOT3);       ND(FLIP2);      ND(FLIP3);
    ND(ATTRSET);     ND(SELF);       ND(NIL);        ND(TRUE);
    ND(FALSE);       ND(ERRINFO);    ND(DEFINED);    ND(POSTEXE);
    ND(ALLOCA);      ND(BMETHOD);    ND(MEMO);       ND(IFUNC);
    ND(DSYM);        ND(ATTRASGN);   ND(PRELUDE);    ND(LAMBDA);
    ND(OPTBLOCK);    ND(LAST);
    default:
      return "unknown";
  }
}

static VALUE (*rb_classname)(VALUE);

/* TODO
 *  look for FL_EXIVAR flag and print ivars
 *  print more detail about Proc/struct BLOCK in T_DATA if freefunc == blk_free
 *  add Memprof.dump_all for full heap dump
 *  print details on different types of nodes (nd_next, nd_lit, nd_nth, etc)
 */

void
obj_dump(VALUE obj, yajl_gen gen)
{
  int type;
  yajl_gen_map_open(gen);

  yajl_gen_cstr(gen, "address");
  yajl_gen_value(gen, obj);

  struct obj_track *tracker = NULL;
  if (st_lookup(objs, (st_data_t)obj, (st_data_t *)&tracker)) {
    yajl_gen_cstr(gen, "source");
    yajl_gen_format(gen, "%s:%d", tracker->source, tracker->line);
  }

  yajl_gen_cstr(gen, "type");
  switch (type=BUILTIN_TYPE(obj)) {
    case T_DATA:
      yajl_gen_cstr(gen, "data");

      if (RBASIC(obj)->klass) {
        yajl_gen_cstr(gen, "class");
        yajl_gen_value(gen, RBASIC(obj)->klass);

        yajl_gen_cstr(gen, "class_name");
        VALUE name = rb_classname(RBASIC(obj)->klass);
        if (RTEST(name))
          yajl_gen_cstr(gen, RSTRING_PTR(name));
        else
          yajl_gen_cstr(gen, 0);
      }
      break;

    case T_FILE:
      yajl_gen_cstr(gen, "file");
      break;

    case T_FLOAT:
      yajl_gen_cstr(gen, "float");

      yajl_gen_cstr(gen, "data");
      yajl_gen_double(gen, RFLOAT_VALUE(obj));
      break;

    case T_BIGNUM:
      yajl_gen_cstr(gen, "bignum");

      yajl_gen_cstr(gen, "negative");
      yajl_gen_bool(gen, RBIGNUM_SIGN(obj) == 0);

      yajl_gen_cstr(gen, "length");
      yajl_gen_integer(gen, RBIGNUM_LEN(obj));

      yajl_gen_cstr(gen, "data");
      yajl_gen_string(gen, (void *)RBIGNUM_DIGITS(obj), RBIGNUM_LEN(obj));
      break;

    case T_MATCH:
      yajl_gen_cstr(gen, "match");

      yajl_gen_cstr(gen, "data");
      yajl_gen_value(gen, RMATCH(obj)->str);
      break;

    case T_REGEXP:
      yajl_gen_cstr(gen, "regexp");

      yajl_gen_cstr(gen, "length");
      yajl_gen_integer(gen, RREGEXP_SRC_LEN(obj));

      yajl_gen_cstr(gen, "data");
      yajl_gen_cstr(gen, RREGEXP_SRC_PTR(obj));
      break;

    case T_NODE:
      yajl_gen_cstr(gen, "node");

      yajl_gen_cstr(gen, "node_type");
      yajl_gen_cstr(gen, nd_type_str(obj));

      yajl_gen_cstr(gen, "file");
      yajl_gen_cstr(gen, RNODE(obj)->nd_file);

      yajl_gen_cstr(gen, "line");
      yajl_gen_integer(gen, nd_line(obj));

      yajl_gen_cstr(gen, "node_code");
      yajl_gen_integer(gen, nd_type(obj));

      switch (nd_type(obj)) {
        case NODE_SCOPE:
          break;
      }
      break;

    case T_STRING:
      yajl_gen_cstr(gen, "string");

      yajl_gen_cstr(gen, "length");
      yajl_gen_integer(gen, RSTRING_LEN(obj));

      if (FL_TEST(obj, ELTS_SHARED|FL_USER3)) {
        yajl_gen_cstr(gen, "shared");
        yajl_gen_value(gen, RSTRING(obj)->as.heap.aux.shared);

        yajl_gen_cstr(gen, "flags");
        yajl_gen_array_open(gen);
        if (FL_TEST(obj, ELTS_SHARED))
          yajl_gen_cstr(gen, "elts_shared");
        if (FL_TEST(obj, FL_USER3))
          yajl_gen_cstr(gen, "str_assoc");
        yajl_gen_array_close(gen);
      } else {
        yajl_gen_cstr(gen, "data");
        yajl_gen_string(gen, (unsigned char *)RSTRING_PTR(obj), RSTRING_LEN(obj));
      }
      break;

    case T_CLASS:
    case T_MODULE:
    case T_ICLASS:
      yajl_gen_cstr(gen, type==T_CLASS ? "class" : type==T_MODULE ? "module" : "iclass");

      yajl_gen_cstr(gen, "name");
      VALUE name = rb_classname(obj);
      if (RTEST(name))
        yajl_gen_cstr(gen, RSTRING_PTR(name));
      else
        yajl_gen_cstr(gen, 0);

      yajl_gen_cstr(gen, "super");
      yajl_gen_value(gen, RCLASS_SUPER(obj));

      yajl_gen_cstr(gen, "super_name");
      VALUE super_name = rb_classname(RCLASS_SUPER(obj));
      if (RTEST(super_name))
        yajl_gen_cstr(gen, RSTRING_PTR(super_name));
      else
        yajl_gen_cstr(gen, 0);

      if (FL_TEST(obj, FL_SINGLETON)) {
        yajl_gen_cstr(gen, "singleton");
        yajl_gen_bool(gen, 1);
      }

      if (RCLASS_IV_TBL(obj) && RCLASS_IV_TBL(obj)->num_entries) {
        yajl_gen_cstr(gen, "ivars");
        yajl_gen_map_open(gen);
        st_foreach(RCLASS_IV_TBL(obj), each_ivar, (st_data_t)gen);
        yajl_gen_map_close(gen);
      }

      if (type != T_ICLASS && RCLASS(obj)->m_tbl && RCLASS(obj)->m_tbl->num_entries) {
        yajl_gen_cstr(gen, "methods");
        yajl_gen_map_open(gen);
        st_foreach(RCLASS(obj)->m_tbl, each_ivar, (st_data_t)gen);
        yajl_gen_map_close(gen);
      }
      break;

    case T_OBJECT:
      yajl_gen_cstr(gen, "object");

      yajl_gen_cstr(gen, "class");
      yajl_gen_value(gen, RBASIC(obj)->klass);

      yajl_gen_cstr(gen, "class_name");
      yajl_gen_cstr(gen, rb_obj_classname(obj));

      struct RClass *klass = RCLASS(obj);

      if (RCLASS_IV_TBL(klass) && RCLASS_IV_TBL(klass)->num_entries) {
        yajl_gen_cstr(gen, "ivars");
        yajl_gen_map_open(gen);
        st_foreach(RCLASS_IV_TBL(klass), each_ivar, (st_data_t)gen);
        yajl_gen_map_close(gen);
      }
      break;

    case T_ARRAY:
      yajl_gen_cstr(gen, "array");

      struct RArray *ary = RARRAY(obj);

      yajl_gen_cstr(gen, "length");
      yajl_gen_integer(gen, RARRAY_LEN(ary));

      if (FL_TEST(obj, ELTS_SHARED)) {
        yajl_gen_cstr(gen, "shared");
        yajl_gen_value(gen, ary->as.heap.aux.shared);
      } else if (RARRAY_LEN(ary)) {
        yajl_gen_cstr(gen, "data");
        yajl_gen_array_open(gen);
        int i;
        for(i=0; i < RARRAY_LEN(ary); i++)
          yajl_gen_value(gen, RARRAY_PTR(ary)[i]);
        yajl_gen_array_close(gen);
      }
      break;

    case T_HASH:
      yajl_gen_cstr(gen, "hash");

      struct RHash *hash = RHASH(obj);

      yajl_gen_cstr(gen, "length");
      if (hash->ntbl)
        yajl_gen_integer(gen, RHASH_SIZE(hash));
      else
        yajl_gen_integer(gen, 0);

      yajl_gen_cstr(gen, "default");
      yajl_gen_value(gen, hash->ifnone);

      if (hash->ntbl && RHASH_SIZE(hash)) {
        yajl_gen_cstr(gen, "data");
        yajl_gen_map_open(gen);
        st_foreach(hash->ntbl, each_hash_entry, (st_data_t)gen);
        yajl_gen_map_close(gen);
      }
      break;

    case T_RATIONAL:
      yajl_gen_cstr(gen, "rational");
      struct RRational *rat = RRATIONAL(obj);
      yajl_gen_cstr(gen, "num");
      yajl_gen_value(gen, rat->num);
      yajl_gen_cstr(gen, "den");
      yajl_gen_value(gen, rat->den);
      break;

    case T_COMPLEX:
      yajl_gen_cstr(gen, "complex");
      struct RComplex *comp = RCOMPLEX(obj);
      yajl_gen_cstr(gen, "real");
      yajl_gen_value(gen, comp->real);
      yajl_gen_cstr(gen, "imag");
      yajl_gen_value(gen, comp->imag);
      break;

    default:
      yajl_gen_cstr(gen, "unknown");
  }

  yajl_gen_cstr(gen, "code");
  yajl_gen_integer(gen, BUILTIN_TYPE(obj));

  yajl_gen_map_close(gen);
}

static int
objs_each_dump(st_data_t key, st_data_t record, st_data_t arg)
{
  obj_dump((VALUE)key, (yajl_gen)arg);
  return ST_CONTINUE;
}

void
json_print(void *ctx, const char * str, unsigned int len)
{
  FILE *out = (FILE *)ctx;
  fwrite(str, sizeof(char), len, out ? out : stdout);
}

static VALUE
memprof_dump(int argc, VALUE *argv, VALUE self)
{
  VALUE str;
  FILE *out = NULL;

  if (!track_objs)
    rb_raise(rb_eRuntimeError, "object tracking disabled, call Memprof.start first");

  rb_scan_args(argc, argv, "01", &str);

  if (RTEST(str)) {
    out = fopen(StringValueCStr(str), "w");
    if (!out)
      rb_raise(rb_eArgError, "unable to open output file");
  }

  yajl_gen_config conf = { .beautify = 1, .indentString = "  " };
  yajl_gen gen = yajl_gen_alloc2((yajl_print_t)&json_print, &conf, NULL, (void*)out);

  track_objs = 0;

  yajl_gen_array_open(gen);
  st_foreach(objs, objs_each_dump, (st_data_t)gen);
  yajl_gen_array_close(gen);
  yajl_gen_free(gen);

  if (out)
    fclose(out);

  track_objs = 1;

  return Qnil;
}

static VALUE
memprof_dump_all(int argc, VALUE *argv, VALUE self)
{
  int sizeof_RVALUE = bin_type_size("RVALUE");
  char *heaps = *(char**)bin_find_symbol("heaps",0);
  int heaps_used = *(int*)bin_find_symbol("heaps_used",0);
  int sizeof_heaps_slot = bin_type_size("heaps_slot");
  int offset_limit = bin_type_member_offset("heaps_slot", "limit");
  int offset_slot = bin_type_member_offset("heaps_slot", "slot");
  char *p, *pend;
  int i, limit;

  if (sizeof_RVALUE < 0 || sizeof_heaps_slot < 0)
    rb_raise(eUnsupported, "could not find internal heap");

  VALUE str;
  FILE *out = NULL;

  rb_scan_args(argc, argv, "01", &str);

  if (RTEST(str)) {
    out = fopen(StringValueCStr(str), "w");
    if (!out)
      rb_raise(rb_eArgError, "unable to open output file");
  }

  yajl_gen_config conf = { .beautify = 1, .indentString = "  " };
  yajl_gen gen = yajl_gen_alloc2((yajl_print_t)&json_print, &conf, NULL, (void*)out);

  track_objs = 0;

  yajl_gen_array_open(gen);

  for (i=0; i < heaps_used; i++) {
    p = *(char**)(heaps + (i * sizeof_heaps_slot) + offset_slot);
    limit = *(int*)(heaps + (i * sizeof_heaps_slot) + offset_limit);
    pend = p + (sizeof_RVALUE * limit);

    while (p < pend) {
      if (RBASIC(p)->flags)
        obj_dump((VALUE)p, gen);

      p += sizeof_RVALUE;
    }
  }

  yajl_gen_array_close(gen);
  yajl_gen_free(gen);

  if (out)
    fclose(out);

  track_objs = 1;

  return Qnil;
}

static void
create_tramp_table()
{
  int i = 0;
  void *region = NULL;
   size_t tramp_sz = 0, inline_tramp_sz = 0;

  void *ent = arch_get_st2_tramp(&tramp_sz);
  void *inline_ent = arch_get_inline_st2_tramp(&inline_tramp_sz);

  if ((region = bin_allocate_page()) == MAP_FAILED) {
    fprintf(stderr, "Failed to allocate memory for stage 1 trampolines.\n");
    return;
  }

  tramp_table = region;
  inline_tramp_table = region + pagesize/2;

  for (i = 0; i < (pagesize/2)/tramp_sz; i++) {
    memcpy(tramp_table + i, ent, tramp_sz);
  }

  for (i = 0; i < (pagesize/2)/inline_tramp_sz; i++) {
    memcpy(inline_tramp_table + i, inline_ent, inline_tramp_sz);
  }
}

void
update_callqs(int entry, void *trampee, void *tramp)
{
  unsigned char *byte = text_segment;
  size_t count = 0;

  for(; count < text_segment_len; byte++, count++) {
    arch_insert_st1_tramp(byte, trampee, tramp);
  }
}

#define FREELIST_INLINES (3)

static void
hook_freelist(int entry)
{
  size_t sizes[FREELIST_INLINES], i = 0;
  void *freelist_inliners[FREELIST_INLINES];
  void *freelist = NULL;
  unsigned char *byte = NULL;

  freelist_inliners[0] = bin_find_symbol("gc_sweep", &sizes[0]);
  /* sometimes gc_sweep gets inlined in garbage_collect */
  if (!freelist_inliners[0]) {
    freelist_inliners[0] = bin_find_symbol("garbage_collect", &sizes[0]);
    if (!freelist_inliners[0]) {
      /* couldn't find garbage_collect either. */
      fprintf(stderr, "Couldn't find gc_sweep or garbage_collect!\n");
      return;
    }
  }

  freelist_inliners[1] = bin_find_symbol("finalize_list", &sizes[1]);
  if (!freelist_inliners[1]) {
    fprintf(stderr, "Couldn't find finalize_list!\n");
    /* XXX continue or exit? */
  }

  freelist_inliners[2] = bin_find_symbol("rb_gc_force_recycle", &sizes[2]);
  if (!freelist_inliners[2]) {
    fprintf(stderr, "Couldn't find rb_gc_force_recycle!\n");
    /* XXX continue or exit? */
  }

  /* XXX this needs to change for 19 */
  freelist = bin_find_symbol("freelist", NULL);
  if (!freelist) {
    fprintf(stderr, "Couldn't find freelist!\n");
    return;
  }

  /* start the search for places to insert the inline tramp */

  byte = freelist_inliners[i];

  while (i < FREELIST_INLINES) {
    if (arch_insert_inline_st2_tramp(byte, freelist, freelist_tramp,
        &inline_tramp_table[entry]) == 0) {
      /* insert occurred, so increment internal counters for the tramp table */
      entry++;
      inline_tramp_size++;
    }

    /* if we've looked at all the bytes in this function... */
    if (((void *)byte - freelist_inliners[i]) >= sizes[i]) {
      /* move on to the next function */
      i++;
      byte = freelist_inliners[i];
    }
    byte++;
  }
}

static void
insert_tramp(char *trampee, void *tramp)
{
  void *trampee_addr = bin_find_symbol(trampee, NULL);
  int entry = tramp_size;
  int inline_ent = inline_tramp_size;

  if (trampee_addr == NULL) {
    if (strcmp("add_freelist", trampee) == 0) {
      /* XXX super hack */
      inline_tramp_size++;
      hook_freelist(inline_ent);
    } else {
      return;
    }
  } else {
    tramp_table[tramp_size].addr = tramp;
    bin_update_image(entry, trampee_addr, &tramp_table[tramp_size]);
    tramp_size++;
  }
}

void
Init_memprof()
{
  VALUE memprof = rb_define_module("Memprof");
  eUnsupported = rb_define_class_under(memprof, "Unsupported", rb_eStandardError);
  rb_define_singleton_method(memprof, "start", memprof_start, 0);
  rb_define_singleton_method(memprof, "stop", memprof_stop, 0);
  rb_define_singleton_method(memprof, "stats", memprof_stats, -1);
  rb_define_singleton_method(memprof, "stats!", memprof_stats_bang, -1);
  rb_define_singleton_method(memprof, "track", memprof_track, -1);
  rb_define_singleton_method(memprof, "dump", memprof_dump, -1);
  rb_define_singleton_method(memprof, "dump_all", memprof_dump_all, -1);

  pagesize = getpagesize();
  objs = st_init_numtable();
  bin_init();
  create_tramp_table();

#if defined(HAVE_MACH)
  insert_tramp("_rb_newobj", newobj_tramp);
#elif defined(HAVE_ELF)
  insert_tramp("rb_newobj", newobj_tramp);
  insert_tramp("add_freelist", freelist_tramp);
#endif

  rb_classname = bin_find_symbol("classname", 0);

  if (getenv("MEMPROF"))
    track_objs = 1;

  return;
}
