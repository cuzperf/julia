#ifndef FLISP_H
#define FLISP_H

#include <setjmp.h>
#include <stdint.h>

#include "platform.h"
#include "libsupport.h"
#include "utils.h"
#include "bitvector.h"
#include "timefuncs.h"
#include "strtod.h"
#include "dirpath.h"
#include "hashing.h"
#include "ptrhash.h"
#include "htable.h"
#include "uv.h"

//#define MEMDEBUG
//#define MEMDEBUG2

typedef uintptr_t value_t;
typedef int_t fixnum_t;
#if NBITS==64
#define T_FIXNUM T_INT64
#define labs llabs
#else
#define T_FIXNUM T_INT32
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* 前向声明 flisp 上下文结构体 */
typedef struct _fl_context_t fl_context_t;
/* cons 单元结构体，表示一对值（car 和 cdr） */
typedef struct {
    value_t car;
    value_t cdr;
} cons_t;

/* 符号结构体，表示 lisp 中的符号对象 */
typedef struct _symbol_t {
    uintptr_t flags;
    value_t binding;   // global value binding
    struct _fltype_t *type;
    uint32_t hash;
    void *dlcache;     // dlsym address
    // below fields are private
    struct _symbol_t *left;
    struct _symbol_t *right;
    JL_ATTRIBUTE_ALIGN_PTRSIZE(char name[]);
} symbol_t;

/* 生成符号结构体，用于自动生成的唯一符号（gensym） */
typedef struct {
    value_t isconst;
    value_t binding;   // global value binding
    struct _fltype_t *type;
    uint32_t id;
} gensym_t;

/* 类型标签常量：值的最低 3 位用于区分对象类型 */
#define TAG_NUM      0x0
#define TAG_CPRIM    0x1
#define TAG_FUNCTION 0x2
#define TAG_VECTOR   0x3
#define TAG_NUM1     0x4
#define TAG_CVALUE   0x5
#define TAG_SYM      0x6
#define TAG_CONS     0x7
#define UNBOUND      ((value_t)0x1) // an invalid value
#define TAG_FWD      UNBOUND
#define tag(x) ((x)&0x7)
#define ptr(x) ((void*)((x)&(~(value_t)0x7)))
#define tagptr(p,t) (((value_t)(p)) | (t))
#define fixnum(x) ((value_t)(((uintptr_t)(x))<<2))
#define numval(x)  (((fixnum_t)(x))>>2)
#if NBITS==64
#define fits_fixnum(x) (((x)>>61) == 0 || (~((x)>>61)) == 0)
#else
#define fits_fixnum(x) (((x)>>29) == 0 || (~((x)>>29)) == 0)
#endif
#define fits_bits(x,b) (((x)>>(b-1)) == 0 || (~((x)>>(b-1))) == 0)
#define uintval(x)  (((unsigned int)(x))>>3)
#define builtin(n) tagptr((((int)n)<<3), TAG_FUNCTION)
#define iscons(x)    (tag(x) == TAG_CONS)
#define issymbol(x)  (tag(x) == TAG_SYM)
#define isfixnum(x)  (((x)&3) == TAG_NUM)
#define bothfixnums(x,y) ((((x)|(y))&3) == TAG_NUM)
#define isbuiltin(x) ((tag(x) == TAG_FUNCTION) && uintval(x) <= OP_ASET)
#define isvector(x) (tag(x) == TAG_VECTOR)
#define iscvalue(x) (tag(x) == TAG_CVALUE)
#define iscprim(x)  (tag(x) == TAG_CPRIM)
#define selfevaluating(x) (tag(x)<6)
/* 判断两个值是否可直接用 == 比较（低位为偶数即无歧义） */
// comparable with ==
#define eq_comparable(a,b) (!(((a)|(b))&1))
#define eq_comparablep(a) (!((a)&1))
/* 是否为叶子值（不会引用其他值） */
// doesn't lead to other values
#define leafp(a) (((a)&3) != 3)

/* 对象转发（GC 标记）：判断是否已被移动 */
#define isforwarded(v) (((value_t*)ptr(v))[0] == TAG_FWD)
/* 获取转发后的新位置 */
#define forwardloc(v)  (((value_t*)ptr(v))[1])
/* 转发对象到新位置 */
#define forward(v,to) do { (((value_t*)ptr(v))[0] = TAG_FWD); \
                           (((value_t*)ptr(v))[1] = to); } while (0)

/* 向量操作宏：获取向量大小、设置大小、访问元素、计算增长量 */
#define vector_size(v) (((size_t*)ptr(v))[0]>>2)
#define vector_setsize(v,n) (((size_t*)ptr(v))[0] = ((n)<<2))
#define vector_elt(v,i) (((value_t*)ptr(v))[1+(i)])
#define vector_grow_amt(x) ((x)<8 ? 5 : 6*((x)>>3))
// functions ending in _ are unsafe, faster versions
/* car 和 cdr 访问宏：_ 后缀为不安全版本，不带后缀的会进行类型检查 */
#define car_(v) (((cons_t*)ptr(v))->car)
#define cdr_(v) (((cons_t*)ptr(v))->cdr)
#define car(fl_ctx, v)  (tocons(fl_ctx, (v),"car")->car)
#define cdr(fl_ctx, v)  (tocons(fl_ctx, (v),"cdr")->cdr)
/* 函数对象字段访问宏：bcode（字节码）、vals（常量值表）、env（闭包环境）、name（函数名） */
#define fn_bcode(f) (((value_t*)ptr(f))[0])
#define fn_vals(f) (((value_t*)ptr(f))[1])
#define fn_env(f) (((value_t*)ptr(f))[2])
#define fn_name(f) (((value_t*)ptr(f))[3])

/* 设置符号的全局绑定值（set：普通赋值，setc：常量赋值） */
#define set(s, v)  (((symbol_t*)ptr(s))->binding = (v))
#define setc(s, v) do { ((symbol_t*)ptr(s))->flags |= 1; \
                        ((symbol_t*)ptr(s))->binding = (v); } while (0)
/* 判断符号是否为常量或关键字 */
#define isconstant(s) ((s)->flags&0x1)
#define iskeyword(s) ((s)->flags&0x2)
/* 获取符号的绑定值 */
#define symbol_value(s) (((symbol_t*)ptr(s))->binding)
#ifdef MEMDEBUG2
/* 判断值是否由 GC 管理（在堆中分配的对象） */
#define ismanaged(ctx, v) (!issymbol(v) && !isfixnum(v) && ((v)>(N_OPCODES<<3)) && !iscbuiltin(ctx, v))
#else
#define ismanaged(ctx, v) ((((unsigned char*)ptr(v)) >= ctx->fromspace) && \
                           (((unsigned char*)ptr(v)) < ctx->fromspace + ctx->heapsize))
#endif
/* 判断是否为 gensym（由 GC 管理的符号） */
#define isgensym(ctx, x)  (issymbol(x) && ismanaged(ctx, x))

/* 判断是否为用户定义函数（非内建函数） */
#define isfunction(x) (tag(x) == TAG_FUNCTION && (x) > (N_BUILTINS<<3))
/* 闭包即用户定义函数 */
#define isclosure(x) isfunction(x)
/* 判断是否为 C 内建值 */
#define iscbuiltin(ctx, x) (iscvalue(x) && (cv_class((cvalue_t*)ptr(x))==ctx->builtintype))

/* 注册/释放 GC 根（防止局部变量被 GC 回收） */
void fl_gc_handle(fl_context_t *fl_ctx, value_t *pv) JL_NOTSAFEPOINT;
void fl_free_gc_handles(fl_context_t *fl_ctx, uint32_t n) JL_NOTSAFEPOINT;

#include "opcodes.h"

/* 遍历内建函数参数的辅助宏 */
// utility for iterating over all arguments in a builtin
// i=index, i0=start index, arg = var for each arg, args = arg array
// assumes "nargs" is the argument count
#define FOR_ARGS(i, i0, arg, args)     \
    for(i=i0; ((size_t)i)<nargs && ((arg=args[i]) || 1); i++)

/* 内建函数总数 */
#define N_BUILTINS ((int)N_OPCODES)

/* 未指定值（返回 #<unspecified>） */
#define FL_UNSPECIFIED(fl_ctx) fl_ctx->T

/* 读取、求值、打印 - REPL 主入口 */
/* read, eval, print main entry points */
value_t fl_read_sexpr(fl_context_t *fl_ctx, value_t f);
void fl_print(fl_context_t *fl_ctx, ios_t *f, value_t v);
value_t fl_toplevel_eval(fl_context_t *fl_ctx, value_t expr);
value_t fl_apply(fl_context_t *fl_ctx, value_t f, value_t l) JL_NOTSAFEPOINT;
value_t fl_applyn(fl_context_t *fl_ctx, uint32_t n, value_t f, ...) JL_NOTSAFEPOINT;

/* memory print */
void fl_print_memory(fl_context_t *fl_ctx, ios_t *f, value_t v, int max_depth);
void fl_print_mem_summary(fl_context_t *fl_ctx, ios_t *f, value_t v);

/* 对象模型操作：构造、符号、向量、比较等 */
/* object model manipulation */
value_t fl_cons(fl_context_t *fl_ctx, value_t a, value_t b) JL_NOTSAFEPOINT;
value_t fl_list2(fl_context_t *fl_ctx, value_t a, value_t b) JL_NOTSAFEPOINT;
value_t fl_listn(fl_context_t *fl_ctx, size_t n, ...) JL_NOTSAFEPOINT;
value_t symbol(fl_context_t *fl_ctx, const char *str) JL_NOTSAFEPOINT;
char *symbol_name(fl_context_t *fl_ctx, value_t v) JL_NOTSAFEPOINT;
int fl_is_keyword_name(const char *str, size_t len);
value_t alloc_vector(fl_context_t *fl_ctx, size_t n, int init);
size_t llength(value_t v);
value_t fl_compare(fl_context_t *fl_ctx, value_t a, value_t b);  // -1, 0, or 1
value_t fl_equal(fl_context_t *fl_ctx, value_t a, value_t b);    // T or nil
int equal_lispvalue(fl_context_t *fl_ctx, value_t a, value_t b);
uintptr_t hash_lispvalue(fl_context_t *fl_ctx, value_t a);
int isnumtok_base(fl_context_t *fl_ctx, char *tok, value_t *pval, int base);

/* safe casts */
cons_t *tocons(fl_context_t *fl_ctx, value_t v, const char *fname);
symbol_t *tosymbol(fl_context_t *fl_ctx, value_t v, const char *fname);
fixnum_t tofixnum(fl_context_t *fl_ctx, value_t v, const char *fname);
char *tostring(fl_context_t *fl_ctx, value_t v, const char *fname);

/* 错误处理：跨平台的 setjmp/longjmp 抽象 */
/* error handling */
#if defined(_OS_WINDOWS_)
#define fl_jmp_buf jmp_buf
#if defined(_COMPILER_GCC_)
int __attribute__ ((__nothrow__,__returns_twice__)) (jl_setjmp)(jmp_buf _Buf);
__declspec(noreturn) __attribute__ ((__nothrow__)) void (jl_longjmp)(jmp_buf _Buf, int _Value);
#else
int (jl_setjmp)(jmp_buf _Buf);
void (jl_longjmp)(jmp_buf _Buf, int _Value);
#endif
#define fl_setjmp(a) (jl_setjmp)((a))
#define fl_longjmp(a, b) (jl_longjmp)((a), (b))
#else // !_OS_WINDOWS_
#define fl_jmp_buf sigjmp_buf
#define fl_setjmp(a) sigsetjmp((a), 0)
#define fl_longjmp(a, b) siglongjmp((a), (b))
#endif

/* 异常上下文结构体：保存 setjmp 缓冲区和 flisp 执行状态，支持嵌套异常处理 */
typedef struct _ectx_t {
    fl_jmp_buf buf;        // setjmp/longjmp 缓冲区
    uint32_t sp;           // 保存的栈指针
    uint32_t frame;        // 保存的当前帧
    uint32_t ngchnd;       // 保存的 GC 句柄数
    void *rdst;            // 读取器目标
    struct _ectx_t *prev;  // 前一个异常上下文（形成链表）
} fl_exception_context_t;

/* 异常处理 try 块：保存执行上下文，设置异常跳转点 */
#define FL_TRY_EXTERN(fl_ctx)                                           \
  fl_exception_context_t _ctx; int l__tr, l__ca;                        \
  fl_savestate(fl_ctx, &_ctx); fl_ctx->exc_ctx = &_ctx;                      \
  if (!fl_setjmp(_ctx.buf))                                                \
      for (l__tr=1; l__tr; l__tr=0, (void)(fl_ctx->exc_ctx=fl_ctx->exc_ctx->prev))

/* 异常处理 catch 块：恢复执行上下文 */
#define FL_CATCH_EXTERN(fl_ctx)                                         \
    else                                                                \
        for(l__ca=1; l__ca; l__ca=0, fl_restorestate(fl_ctx, &_ctx))

#if defined(_OS_WINDOWS_)
__declspec(noreturn) void lerrorf(fl_context_t *fl_ctx, value_t e, const char *format, ...) JL_NOTSAFEPOINT;
__declspec(noreturn) void lerror(fl_context_t *fl_ctx, value_t e, const char *msg) JL_NOTSAFEPOINT;
__declspec(noreturn) void fl_raise(fl_context_t *fl_ctx, value_t e);
__declspec(noreturn) void type_error(fl_context_t *fl_ctx, const char *fname, const char *expected, value_t got);
__declspec(noreturn) void bounds_error(fl_context_t *fl_ctx, const char *fname, value_t arr, value_t ind);
#else
void lerrorf(fl_context_t *fl_ctx, value_t e, const char *format, ...) __attribute__ ((__noreturn__)) JL_NOTSAFEPOINT;
void lerror(fl_context_t *fl_ctx, value_t e, const char *msg) __attribute__((__noreturn__)) JL_NOTSAFEPOINT;
void fl_raise(fl_context_t *fl_ctx, value_t e) __attribute__ ((__noreturn__));
void type_error(fl_context_t *fl_ctx, const char *fname, const char *expected, value_t got) __attribute__ ((__noreturn__));
void bounds_error(fl_context_t *fl_ctx, const char *fname, value_t arr, value_t ind) __attribute__ ((__noreturn__));
#endif

/* 保存/恢复 flisp 执行状态（配合异常处理） */
void fl_savestate(fl_context_t *fl_ctx, fl_exception_context_t *_ctx);
void fl_restorestate(fl_context_t *fl_ctx, fl_exception_context_t *_ctx);

/* C 值虚函数表：定义了 cvalue 的打印、重定位、终结等回调 */
typedef struct {
    void (*print)(fl_context_t *fl_ctx, value_t self, ios_t *f);
    void (*relocate)(fl_context_t *fl_ctx, value_t oldv, value_t newv);
    void (*finalize)(fl_context_t *fl_ctx, value_t self);
    void (*print_traverse)(fl_context_t *fl_ctx, value_t self);
} cvtable_t;

/* 实现值接口所需的函数（对应 cvtable_t 中的回调） */
/* functions needed to implement the value interface (cvtable_t) */
value_t relocate_lispvalue(fl_context_t *fl_ctx, value_t v);
void print_traverse(fl_context_t *fl_ctx, value_t v);
void fl_print_chr(fl_context_t *fl_ctx, char c, ios_t *f);
void fl_print_str(fl_context_t *fl_ctx, const char *s, ios_t *f);
void fl_print_child(fl_context_t *fl_ctx, ios_t *f, value_t v);

/* C 值类型初始化函数指针类型 */
typedef int (*cvinitfunc_t)(fl_context_t *fl_ctx, struct _fltype_t*, value_t, void*);

/* flisp 类型描述结构体：描述 C 值的类型信息 */
typedef struct _fltype_t {
    value_t type;            // 类型的 lisp 表示
    numerictype_t numtype;   // 数值子类型
    size_t size;             // 类型实例的总大小
    size_t elsz;             // 元素大小（数组类型）
    const cvtable_t *vtable; // 虚函数表
    struct _fltype_t *eltype;  // 数组元素类型
    struct _fltype_t *artype;  // 数组类型（即 array this）
    int marked;              // GC 标记
    cvinitfunc_t init;       // 初始化函数
} fltype_t;

/* C 值结构体：包装 C 数据作为 lisp 值，支持内联存储和外部引用 */
JL_EXTENSION typedef struct {
    fltype_t *type;
    void *data;
    size_t len;            // length of *data in bytes
    union {
        value_t parent;    // optional
        char _space[1];    // variable size
    };
} cvalue_t;

/* cvalue 占用的字数（用于 GC 大小计算） */
#define CVALUE_NWORDS 4

/* C 原始类型结构体：内联存储的小型 C 值 */
typedef struct {
    fltype_t *type;
    char _space[1];
} cprim_t;

/* 函数对象结构体：包含字节码和环境 */
typedef struct {
    value_t bcode;
    value_t vals;
    value_t env;
    value_t name;
} function_t;

/* cprim 占用的字数（用于 GC 大小计算） */
#define CPRIM_NWORDS 2
/* cvalue 内联数据的最大字节数 */
#define MAX_INL_SIZE 384

/* cvalue 类型指针低位的标志位：owned（拥有数据）和 parent（有父对象） */
#define CV_OWNED_BIT  0x1
#define CV_PARENT_BIT 0x2
/* cvalue 标志检查宏 */
#define owned(cv)      ((uintptr_t)(cv)->type & CV_OWNED_BIT)
#define hasparent(cv)  ((uintptr_t)(cv)->type & CV_PARENT_BIT)
#define isinlined(cv)  ((cv)->data == &(cv)->_space[0])
/* cvalue 访问器宏：获取类型、长度、数据等 */
#define cv_class(cv)   ((fltype_t*)(((uintptr_t)(cv)->type)&~3))
#define cv_len(cv)     ((cv)->len)
#define cv_type(cv)    (cv_class(cv)->type)
#define cv_data(cv)    ((cv)->data)
#define cv_isstr(fl_ctx, cv)   (cv_class(cv)->eltype == fl_ctx->bytetype)
#define cv_isPOD(cv)   (cv_class(cv)->init != NULL)

/* 从 value_t 获取 cvalue 的数据指针和长度 */
#define cvalue_data(v) cv_data((cvalue_t*)ptr(v))
#define cvalue_len(v) cv_len((cvalue_t*)ptr(v))
#define value2c(type, v) ((type)cv_data((cvalue_t*)ptr(v)))

#define valid_numtype(v) ((v) < N_NUMTYPES)
/* cprim 访问器宏 */
#define cp_class(cp)   ((cp)->type)
#define cp_type(cp)    (cp_class(cp)->type)
#define cp_numtype(cp) (cp_class(cp)->numtype)
#define cp_data(cp)    (&(cp)->_space[0])

// WARNING: multiple evaluation!
#define cptr(v) \
    (iscprim(v) ? cp_data((cprim_t*)ptr(v)) : cv_data((cvalue_t*)ptr(v)))

/* C 类型名称别名，与 cvalue 类型名称对应 */
/* C type names corresponding to cvalues type names */
typedef int8_t   fl_int8_t;
typedef uint8_t  fl_uint8_t;
typedef int16_t  fl_int16_t;
typedef uint16_t fl_uint16_t;
typedef int32_t  fl_int32_t;
typedef uint32_t fl_uint32_t;
typedef int64_t  fl_int64_t;
typedef uint64_t fl_uint64_t;
typedef char     fl_char_t;
typedef char     char_t;
typedef ptrdiff_t fl_ptrdiff_t;
typedef size_t   fl_size_t;
typedef double   fl_double_t;
typedef float    fl_float_t;

/* 内建函数指针类型：接收上下文、参数数组和参数个数 */
typedef value_t (*builtin_t)(fl_context_t*, value_t*, uint32_t);

/* 创建 cvalue 和 cprim 对象 */
value_t cvalue(fl_context_t *fl_ctx, fltype_t *type, size_t sz) JL_NOTSAFEPOINT;
value_t cprim(fl_context_t *fl_ctx, fltype_t *type, size_t sz) JL_NOTSAFEPOINT;
/* 创建无终结器的 cvalue */
value_t cvalue_no_finalizer(fl_context_t *fl_ctx, fltype_t *type, size_t sz) JL_NOTSAFEPOINT;
/* 添加终结器、自动释放、固定对象 */
void add_finalizer(fl_context_t *fl_ctx, cvalue_t *cv);
void cv_autorelease(fl_context_t *fl_ctx, cvalue_t *cv);
void cv_pin(fl_context_t *fl_ctx, cvalue_t *cv);
/* 类型大小查询、复制、从数据创建 cvalue */
size_t ctype_sizeof(fl_context_t *fl_ctx, value_t type, int *palign);
value_t cvalue_copy(fl_context_t *fl_ctx, value_t v);
value_t cvalue_from_data(fl_context_t *fl_ctx, fltype_t *type, void *data, size_t sz);
value_t cvalue_from_ref(fl_context_t *fl_ctx, fltype_t *type, void *ptr, size_t sz, value_t parent);
value_t cbuiltin(fl_context_t *fl_ctx, const char *name, builtin_t f);
size_t cvalue_arraylen(value_t v);
value_t size_wrap(fl_context_t *fl_ctx, size_t sz);
size_t tosize(fl_context_t *fl_ctx, value_t n, const char *fname);
/* 字符串相关 cvalue 操作 */
value_t cvalue_string(fl_context_t *fl_ctx, size_t sz);
value_t cvalue_static_cstrn(fl_context_t *fl_ctx, const char *str, size_t n);
value_t cvalue_static_cstring(fl_context_t *fl_ctx, const char *str);
value_t string_from_cstr(fl_context_t *fl_ctx, char *str);
value_t string_from_cstrn(fl_context_t *fl_ctx, char *str, size_t n);
/* 类型判断谓词 */
int fl_isstring(fl_context_t *fl_ctx, value_t v) JL_NOTSAFEPOINT;
int fl_isnumber(fl_context_t *fl_ctx, value_t v) JL_NOTSAFEPOINT;
int fl_isgensym(fl_context_t *fl_ctx, value_t v) JL_NOTSAFEPOINT;
int fl_isiostream(fl_context_t *fl_ctx, value_t v) JL_NOTSAFEPOINT;
ios_t *fl_toiostream(fl_context_t *fl_ctx, value_t v, const char *fname);
value_t cvalue_compare(value_t a, value_t b);
int numeric_compare(fl_context_t *fl_ctx, value_t a, value_t b, int eq, int eqnans, char *fname);

void to_sized_ptr(fl_context_t *fl_ctx, value_t v, const char *fname, char **pdata, size_t *psz);

/* 类型系统查询和注册函数 */
fltype_t *get_type(fl_context_t *fl_ctx, value_t t);
fltype_t *get_array_type(fl_context_t *fl_ctx, value_t eltype);
fltype_t *define_opaque_type(value_t sym, size_t sz, const cvtable_t *vtab,
                             cvinitfunc_t init) JL_NOTSAFEPOINT;

/* 数值构造器：创建 double、float、uint32、uint64、宽字符等 */
value_t mk_double(fl_context_t *fl_ctx, fl_double_t n);
value_t mk_float(fl_context_t *fl_ctx, fl_float_t n);
value_t mk_uint32(fl_context_t *fl_ctx, uint32_t n);
value_t mk_uint64(fl_context_t *fl_ctx, uint64_t n);
value_t mk_wchar(fl_context_t *fl_ctx, int32_t n);
value_t return_from_uint64(fl_context_t *fl_ctx, uint64_t Uaccum);
value_t return_from_int64(fl_context_t *fl_ctx, int64_t Saccum);

/* 内建函数规格：名称和函数指针的配对 */
typedef struct {
    const char *name;
    builtin_t fptr;
} builtinspec_t;

/* 注册全局内建函数列表 */
void assign_global_builtins(fl_context_t *fl_ctx, const builtinspec_t *b) JL_NOTSAFEPOINT;

/* 内建函数声明 */
/* builtins */
value_t fl_hash(fl_context_t *fl_ctx, value_t *args, uint32_t nargs);
value_t cvalue_byte(fl_context_t *fl_ctx, value_t *args, uint32_t nargs);
value_t cvalue_wchar(fl_context_t *fl_ctx, value_t *args, uint32_t nargs);

/* flisp 初始化及加载系统镜像 */
void fl_init(fl_context_t *fl_ctx, size_t initial_heapsize) JL_NOTSAFEPOINT;
int fl_load_system_image(fl_context_t *fl_ctx, value_t ios);
int fl_load_system_image_str(fl_context_t *fl_ctx, char* str, size_t len) JL_NOTSAFEPOINT;

/* julia extensions */
JL_DLLEXPORT int jl_id_char(uint32_t wc);
JL_DLLEXPORT int jl_id_start_char(uint32_t wc);
JL_DLLEXPORT int jl_op_suffix_char(uint32_t wc);

/* flisp 运行时上下文结构体：保存所有全局状态，包括符号表、类型信息、GC 堆、栈、异常处理等 */
struct _fl_context_t {
    /* 符号表 - 哈希二叉搜索树根节点 */
    symbol_t *symtab;
    /* 基础特殊值：nil、真、假、EOF、quote */
    value_t NIL, T, F, FL_EOF, QUOTE;
    /* 基本类型的符号名称引用 */
    value_t int8sym, uint8sym, int16sym, uint16sym, int32sym, uint32sym;
    value_t int64sym, uint64sym;

    /* 更多类型符号名称：指针差、大小、字节、宽字符、浮点、字符串等 */
    value_t ptrdiffsym, sizesym, bytesym, wcharsym;
    value_t floatsym, doublesym;
    value_t stringtypesym, wcstringtypesym;
    value_t emptystringsym;

    /* 复合类型符号名称：数组、C 函数、void、指针 */
    value_t arraysym, cfunctionsym, voidsym, pointersym;

    /* 类型表（符号到 fltype_t 映射）和 dlsym 反向查找表 */
    htable_t TypeTable;
    htable_t reverse_dlsym_lookup_table;

    /* 预定义的基本类型描述符：各整数类型、指针差、大小、浮点、字节、宽字符、字符串等 */
    fltype_t *int8type, *uint8type;
    fltype_t *int16type, *uint16type;
    fltype_t *int32type, *uint32type;
    fltype_t *int64type, *uint64type;
    fltype_t *ptrdifftype, *sizetype;
    fltype_t *floattype, *doubletype;
    fltype_t *bytetype, *wchartype;
    fltype_t *stringtype, *wcstringtype;
    /* C 内建值的类型描述符 */
    fltype_t *builtintype;

    /* 相等性哈希表（用于 equal/hash 的缓存） */
    htable_t equal_eq_hashtable;

    /* 表（哈希表）类型的相关定义 */
    value_t tablesym;
    fltype_t *tabletype;
    cvtable_t table_vtable;

    /* 读取器状态：当前 token 类型、值和读取缓冲区 */
    uint32_t readtoktype;
    value_t readtokval;
    char readbuf[256];

    /* 打印器状态：已打印的 cons 表（用于环检测）、标签、格式控制等 */
    htable_t printconses;
    uint32_t printlabel;
    int print_pretty;
    int print_princ;
    fixnum_t print_length;
    fixnum_t print_level;
    fixnum_t P_LEVEL;
    int SCR_WIDTH;
    int HPOS, VPOS;

    /* IO 流类型相关：符号引用和类型描述符 */
    value_t iostreamsym, rdsym, wrsym, apsym, crsym, truncsym;
    value_t instrsym, outstrsym;
    fltype_t *iostreamtype;

    /* 内存管理：malloc 压力计数和终结器列表 */
    size_t malloc_pressure;
    cvalue_t **Finalizers;
    size_t nfinalizers;
    size_t maxfinalizers;

    /* 求值栈：大小、栈指针、当前栈帧 */
    uint32_t N_STACK;
    value_t *Stack;
    uint32_t SP;
    uint32_t curr_frame;

    /* GC 句柄栈：保护局部变量不被 GC 回收 */
#define FL_N_GC_HANDLES 8192
    value_t *GCHandleStack[FL_N_GC_HANDLES];
    uint32_t N_GCHND;

    /* 预定义的异常类型符号 */
    value_t IOError, ParseError, TypeError, ArgError, UnboundError, KeyError;
    value_t OutOfMemoryError, DivideError, BoundsError, EnumerationError;
    /* 打印控制参数符号 */
    value_t printwidthsym, printreadablysym, printprettysym, printlengthsym;
    value_t printlevelsym, builtins_table_sym;

    /* 特殊形式符号：lambda、if、try/catch、反引号、逗号等 */
    value_t LAMBDA, IF, TRYCATCH;
    value_t BACKQUOTE, COMMA, COMMAAT, COMMADOT, FUNCTION;

    /* 常用类型的符号引用 */
    value_t pairsym, symbolsym, fixnumsym, vectorsym, builtinsym, vu8sym;
    value_t definesym, defmacrosym, forsym, setqsym;
    value_t tsym, Tsym, fsym, Fsym, booleansym, nullsym, evalsym, fnsym;
    // for reading characters
    /* 字符读取用控制字符符号 */
    value_t nulsym, alarmsym, backspacesym, tabsym, linefeedsym, newlinesym;
    value_t vtabsym, pagesym, returnsym, escsym, spacesym, deletesym;

    /* 当前读取器状态（用于嵌套读取） */
    struct _fl_readstate_t *readstate;

    /* GC 堆：from-space、to-space、当前分配位置、堆限制、堆大小、cons 标记位 */
    unsigned char *fromspace;
    unsigned char *tospace;
    unsigned char *curheap;
    unsigned char *lim;
    size_t heapsize;//bytes
    uint32_t *consflags;

    // error utilities --------------------------------------------------

    // saved execution state for an unwind target
    /* 异常处理：当前异常上下文、抛出时的活跃帧、最后错误值 */
    fl_exception_context_t *exc_ctx;
    uint32_t throwing_frame;  // active frame when exception was thrown
    value_t lasterror;

    /* 生成符号（gensym）计数器及打印缓冲区 */
    uint32_t gensym_ctr;
    // two static buffers for gensym printing so there can be two
    // gensym names available at a time, mostly for compare()
    char gsname[2][16];
    int gsnameno;

    /* GC 转发链和已分配对象计数 */
    void *tochain;
    long long n_allocd;

    /* 缓存常用对象：空向量、内存不足异常值 */
    value_t the_empty_vector;
    value_t memory_exception_value;

    /* GC 是否增长堆；apply 相关临时变量（用于内核函数调用） */
    int gc_grew;
    cons_t *apply_c;
    value_t *apply_pv;
    int64_t apply_accum;
    value_t apply_func, apply_v, apply_e;

    /* Julia 扩展相关符号和扩展缓冲区 */
    value_t jl_sym;
    value_t jl_char_sym;
    // persistent buffer (avoid repeated malloc/free)
    // for julia_extensions.c: normalize
    /* 持久化缓冲区（避免重复 malloc/free），供 julia_extensions.c 使用 */
    size_t jlbuflen;
    void *jlbuf;
};

/* 检查函数参数个数是否精确匹配，不匹配时抛出 ArgError */
static inline void argcount(fl_context_t *fl_ctx, const char *fname, uint32_t nargs, uint32_t c) JL_NOTSAFEPOINT
{
    if (__unlikely(nargs != c))
        lerrorf(fl_ctx, fl_ctx->ArgError,"%s: too %s arguments", fname, nargs<c ? "few":"many");
}

#ifdef __cplusplus
}
#endif

#endif
