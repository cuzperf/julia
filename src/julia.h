// This file is a part of Julia. License is MIT: https://julialang.org/license

#ifndef JULIA_H
#define JULIA_H

#if defined(JL_LIBRARY_EXPORTS_INTERNAL) || defined(JL_LIBRARY_EXPORTS_CODEGEN)
#define JL_LIBRARY_EXPORTS
#endif
#ifdef JL_LIBRARY_EXPORTS
// Generated file, needs to be searched in include paths so that the builddir
// retains priority
#include <jl_internal_funcs.inc>
#undef jl_setjmp
#undef jl_longjmp
#undef jl_egal
#undef jl_genericmemory_owner
#endif
#include "jl_exported_data.inc"

#include "julia_fasttls.h"
#include "libsupport.h"
#include <stdint.h>
#include <string.h>

#include "htable.h"
#include "arraylist.h"
#include "analyzer_annotations.h"
#include "jloptions.h"

#include <setjmp.h>
#ifndef _OS_WINDOWS_
    #define jl_jmp_buf sigjmp_buf
#else
    #include "win32_ucontext.h"
    #define jl_jmp_buf jmp_buf
#endif

/* ======= 平台相关的原子操作大小定义 ======= */
// Define the largest size (bytes) of a properly aligned object that the
// processor family (MAX_ATOMIC_SIZE) and compiler (MAX_POINTERATOMIC_SIZE)
// typically supports without a lock (assumed to be at least a pointer size)
// with MAX_POINTERATOMIC_SIZE >= MAX_ATOMIC_SIZE.
#ifdef _P64
#define MAX_ATOMIC_SIZE 16
#define MAX_POINTERATOMIC_SIZE 16
#else
#define MAX_ATOMIC_SIZE 8
#define MAX_POINTERATOMIC_SIZE 8
#endif

#ifdef _P64
#define NWORDS(sz) (((sz)+7)>>3)
#else
#define NWORDS(sz) (((sz)+3)>>2)
#endif

/* ======= 编译器属性宏 ======= */
#if defined(__GNUC__)
#  define JL_NORETURN __attribute__ ((noreturn))
#  define JL_CONST_FUNC __attribute__((const))
#  define JL_USED_FUNC __attribute__((used))
#else
#  define JL_NORETURN
#  define JL_CONST_FUNC
#  define JL_USED_FUNC
#endif

// 通过成员指针获取包含该成员的结构体指针
#define container_of(ptr, type, member) \
    ((type *) ((char *)(ptr) - offsetof(type, member)))

typedef struct _jl_taggedvalue_t jl_taggedvalue_t;
typedef struct _jl_tls_states_t *jl_ptls_t;

#ifdef JL_LIBRARY_EXPORTS
#include "uv.h"
#endif
#include "gc-interface.h"
#include "julia_atomics.h"
#include "julia_assert.h"

/* ======= JL_DATA_TYPE 标记宏 ======= */
// 该宏用于标记所有继承自 jl_value_t 的子类型，
// 表示这些类型的公共字段隐藏在指针之前。
// the common fields are hidden before the pointer, but the following macro is
// used to indicate which types below are subtypes of jl_value_t
#define JL_DATA_TYPE
typedef struct _jl_value_t jl_value_t;
#include "julia_threads.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======= 核心数据类型 ======= */
// core data types ------------------------------------------------------------

// 标记值的位字段结构：包含 GC 标记、镜像标记和类型标签
struct _jl_taggedvalue_bits {
    uintptr_t gc:2;
    uintptr_t in_image:2;
#ifdef _P64
    uintptr_t tag:60;
#else
    uintptr_t tag:28;
#endif
};

// 标记值 —— 所有 Julia 值的类型标签头部
// 位于每个 Julia 对象之前，包含类型指针、GC 信息
JL_EXTENSION struct _jl_taggedvalue_t {
    union {
        uintptr_t header;
        jl_taggedvalue_t *next;
        jl_value_t *type; // 16-byte aligned
        struct _jl_taggedvalue_bits bits;
    };
    // jl_value_t value;
};

// 将 uintptr_t 类型标签转换为 jl_value_t* 类型对象
static inline jl_value_t *jl_to_typeof(uintptr_t t) JL_GLOBALLY_ROOTED JL_NOTSAFEPOINT;
#ifdef __clang_gcanalyzer__
JL_DLLEXPORT jl_taggedvalue_t *_jl_astaggedvalue(jl_value_t *v JL_PROPAGATES_ROOT) JL_NOTSAFEPOINT;
// 获取值对应的 taggedvalue 指针（位于值指针之前）
#define jl_astaggedvalue(v) _jl_astaggedvalue((jl_value_t*)(v))
jl_value_t *_jl_valueof(jl_taggedvalue_t *tv JL_PROPAGATES_ROOT) JL_NOTSAFEPOINT;
// 将 taggedvalue 指针转换为值指针
#define jl_valueof(v) _jl_valueof((jl_taggedvalue_t*)(v))
JL_DLLEXPORT jl_value_t *_jl_typeof(jl_value_t *v JL_PROPAGATES_ROOT) JL_NOTSAFEPOINT;
// 获取值的类型对象
#define jl_typeof(v) (_jl_typeof((jl_value_t*)(v)))
// 获取值的类型标签（低位掩码后的 header 值）
#define jl_typetagof(v) ((uintptr_t)_jl_typeof((jl_value_t*)(v)))
#else
// 获取值对应的 taggedvalue 指针（位于值指针之前）
#define jl_astaggedvalue(v)                                             \
    ((jl_taggedvalue_t*)((char*)(v) - sizeof(jl_taggedvalue_t)))
// 将 taggedvalue 指针转换为值指针
#define jl_valueof(v)                                                   \
    ((jl_value_t*)((char*)(v) + sizeof(jl_taggedvalue_t)))
// 获取值的类型对象
#define jl_typeof(v)                                                    \
    jl_to_typeof(jl_typetagof(v))
// 获取值的类型标签（低位掩码后的 header 值）
#define jl_typetagof(v)                                                 \
    ((jl_astaggedvalue(v)->header) & ~(uintptr_t)15)
#endif
// 设置值的类型（不要在已初始化的值上调用）
static inline void jl_set_typeof(void *v, void *t) JL_NOTSAFEPOINT
{
    // Do not call this on a value that is already initialized.
    jl_taggedvalue_t *tag = jl_astaggedvalue(v);
    jl_atomic_store_relaxed((_Atomic(jl_value_t*)*)&tag->type, (jl_value_t*)t);
}
// 通过指针比较检查值是否为指定类型
#define jl_typeis(v,t) (jl_typeof(v)==(jl_value_t*)(t))
// 通过标签比较检查值是否为指定类型
#define jl_typetagis(v,t) (jl_typetagof(v)==(uintptr_t)(t))
// 设置值的类型标签（包含 GC 标记）
#define jl_set_typetagof(v,t,gc) (jl_set_typeof((v), (void*)(((uintptr_t)(t) << 4) | (gc))))

// 符号（Symbol）—— 内部化的字符串（哈希共享），存储为侵入式二叉树
// 字符串数据以 NUL 结尾，挂在结构体末尾之后
// Symbols are interned strings (hash-consed) stored as an invasive binary tree.
// The string data is nul-terminated and hangs off the end of the struct.
typedef struct _jl_sym_t {
    JL_DATA_TYPE
    _Atomic(struct _jl_sym_t*) left;
    _Atomic(struct _jl_sym_t*) right;
    uintptr_t hash;    // precomputed hash value
    // JL_ATTRIBUTE_ALIGN_PTRSIZE(char name[]);
} jl_sym_t;

// SSA 值 —— 用于优化代码分析和生成的编号 SSA 值，`id` 是唯一的较小编号
// A numbered SSA value, for optimized code analysis and generation
// the `id` is a unique, small number
typedef struct _jl_ssavalue_t {
    JL_DATA_TYPE
    ssize_t id;
} jl_ssavalue_t;

// SimpleVector —— 不可变指针数组，数据存储在变长结构体末尾
// A SimpleVector is an immutable pointer array
// Data is stored at the end of this variable-length struct.
typedef struct {
    JL_DATA_TYPE
    size_t length;
    // pointer size aligned
    // jl_value_t *data[];
} jl_svec_t;

// 泛型内存 —— 通用内存块，支持嵌入式数据、GC 管理、malloc 或字符串拥有等多种数据管理方式
JL_EXTENSION typedef struct _jl_genericmemory_t {
    JL_DATA_TYPE
    size_t length;
    void *ptr;
    // followed by padding and inline data, or owner pointer
#ifdef _P64
    // union {
    //     jl_value_t *owner;
    //     T inl[];
    // };
#else
    //
    // jl_value_t *owner;
    // size_t padding[1];
    // T inl[];
#endif
} jl_genericmemory_t;

// 泛型内存引用 —— 指向泛型内存中某个位置的指针/偏移对
JL_EXTENSION typedef struct {
    JL_DATA_TYPE
    // 泛型内存引用 —— 指向泛型内存中某个位置的指针/偏移对
    void *ptr_or_offset;
    jl_genericmemory_t *mem;
} jl_genericmemoryref_t;

// 数组类型 —— Julia 数组，基于 GenericMemory 实现
JL_EXTENSION typedef struct {
    JL_DATA_TYPE
    jl_genericmemoryref_t ref;
    size_t dimsize[]; // length for 1-D, otherwise length is mem->length
} jl_array_t;


typedef struct _jl_datatype_t jl_tupletype_t;
struct _jl_code_instance_t;
typedef struct _jl_method_instance_t jl_method_instance_t;
typedef struct _jl_globalref_t jl_globalref_t;
typedef struct _jl_typemap_entry_t jl_typemap_entry_t;


// TypeMap —— 隐式定义的类型，包含 TypeMapLevel、TypeMapEntry 或 Nothing 节点
// 形成近似树形结构，用于灵活的方法分发表查询
// TypeMap is an implicitly defined type
// that can consist of any of the following nodes:
//   typedef TypeMap Union{TypeMapLevel, TypeMapEntry, Nothing}
// it forms a roughly tree-shaped structure, consisting of nodes of TypeMapLevels
// which split the tree when possible, for example based on the key into the tuple type at `offs`
// when key is a leaftype, (but only when the tree has enough entries for this to be
// more efficient than storing them sorted linearly)
// otherwise the leaf entries are stored sorted, linearly
typedef jl_value_t jl_typemap_t;

// 函数调用指针类型 —— Julia 函数调用的标准 ABI
typedef jl_value_t *(jl_call_t)(jl_value_t*, jl_value_t**, uint32_t, struct _jl_code_instance_t*);
typedef jl_call_t *jl_callptr_t;

// "speccall" 调用约定签名 —— 编译后的 Julia 函数的特殊 ABI
// "speccall" calling convention signatures.
// This describes some of the special ABI used by compiled julia functions.
extern jl_call_t jl_fptr_args;
JL_DLLEXPORT extern const jl_callptr_t jl_fptr_args_addr;
// fptr_args 调用约定函数指针
typedef jl_value_t *(*jl_fptr_args_t)(jl_value_t*, jl_value_t**, uint32_t);

extern jl_call_t jl_fptr_const_return;
JL_DLLEXPORT extern const jl_callptr_t jl_fptr_const_return_addr;

extern jl_call_t jl_fptr_sparam;
JL_DLLEXPORT extern const jl_callptr_t jl_fptr_sparam_addr;
// fptr_sparam 调用约定函数指针（带静态参数）
typedef jl_value_t *(*jl_fptr_sparam_t)(jl_value_t*, jl_value_t**, uint32_t, jl_svec_t*);

extern jl_call_t jl_fptr_interpret_call;
JL_DLLEXPORT extern const jl_callptr_t jl_fptr_interpret_call_addr;

JL_DLLEXPORT extern const jl_callptr_t jl_f_opaque_closure_call_addr;

JL_DLLEXPORT extern const jl_callptr_t jl_fptr_wait_for_compiled_addr;

// 源码位置区间 —— 表示行或列区间 [first, second)
typedef struct _jl_locspan_t {
    int32_t first;
    int32_t second;
} jl_locspan_t;

// 代码位置结构 —— 包含位置索引和 PC 值
struct jl_codeloc_t {
    int32_t loc;
    int32_t to;
    int32_t pc;
};

// 源码字节表头部 —— 用于压缩的调试信息行号表
// In a compressed jl_debuginfo_t linetable string, this header is followed by
// (with byte_offset subtracted from all raw byte positions):
//
// bytespans: (byte_encl+span_encl)*nlocs bytes
// line_starts: byte_encl*rest bytes
typedef struct _jl_sourcebytetable_header_t {
    // (>0) minimum byte
    int32_t byte_offset;
    // (>0) minimum line, where line_starts[0] is the byte position of this
    // line's first character
    int32_t line_offset;
    // (>=0) number of (byte, len) bytespans
    int32_t nlocs;
    // (1,2,4) compressed length
    uint8_t byte_encl;
    // (0,1,2,4) compressed length
    uint8_t span_encl;
} jl_sourcebytetable_header_t;
// packed size
#define SBT_HEADER_SIZE 14

// 调试信息 —— 包含函数定义、行号表和边信息
typedef struct _jl_debuginfo_t {
    jl_value_t *def;
    jl_value_t *linetable; // debuginfo, compressed string, or nothing
    jl_svec_t *edges; // Memory{DebugInfo}
    jl_value_t *codelocs; // String // Memory{UInt8} // compressed info
} jl_debuginfo_t;

// 纯度覆盖 —— 用于覆盖函数副作用分析的结果（对应 base/expr.jl 中的 EffectsOverride）
// the following mirrors `struct EffectsOverride` in `base/expr.jl`
typedef union __jl_purity_overrides_t {
    struct {
        uint16_t ipo_consistent          : 1;
        uint16_t ipo_effect_free         : 1;
        uint16_t ipo_nothrow             : 1;
        uint16_t ipo_terminates_globally : 1;
        // Weaker form of `terminates` that asserts
        // that any control flow syntactically in the method
        // is guaranteed to terminate, but does not make
        // assertions about any called functions.
        uint16_t ipo_terminates_locally  : 1;
        uint16_t ipo_notaskstate         : 1;
        uint16_t ipo_inaccessiblememonly : 1;
        uint16_t ipo_noub                : 1;
        uint16_t ipo_noub_if_noinbounds  : 1;
        uint16_t ipo_consistent_overlay  : 1;
        uint16_t ipo_nortcall            : 1;
    } overrides;
    uint16_t bits;
} _jl_purity_overrides_t;

#define NUM_EFFECTS_OVERRIDES 11
#define NUM_IR_FLAGS 3

// 代码信息 —— 描述单个函数体的所有 IR 信息
// This type describes a single function body
typedef struct _jl_code_info_t {
    JL_DATA_TYPE
    // ssavalue-indexed arrays of properties:
    jl_array_t *code;  // Any array of statements
    jl_debuginfo_t *debuginfo; // Table of edge data for each statement
    jl_value_t *ssavaluetypes; // types of ssa values (or count of them)
    jl_array_t *ssaflags; // 32 bits flags associated with each statement:
        // 1 << 0 = inbounds region
        // 1 << 1 = callsite inline region
        // 1 << 2 = callsite noinline region
        // 1 << 3-14 = purity
        // 1 << 16+ = reserved for inference
    // miscellaneous data:
    jl_array_t *slotnames; // names of local variables
    jl_array_t *slotflags;  // local var bit flags
    // the following is a deprecated property (not preserved by compression)
    jl_value_t *slottypes; // inferred types of slots
    // more inferred data:
    jl_value_t *rettype; // return type relevant for fptr
    jl_method_instance_t *parent; // context (after inference, otherwise nothing)
    // the following are required to cache the method correctly
    jl_value_t *edges; // forward edge info (svec preferred, but tolerates Array{Any} and nothing token)
    size_t min_world;
    size_t max_world;

    // These may be used by generated functions to further constrain the resulting inputs.
    jl_value_t *method_for_inference_limit_heuristics; // optional method used during inference
    size_t nargs;

    // various boolean properties:
    uint8_t propagate_inbounds;
    uint8_t has_fcall;
    uint8_t has_image_globalref;
    uint8_t nospecializeinfer;
    uint8_t isva;
    // uint8 settings
    uint8_t inlining; // 0 = default; 1 = @inline; 2 = @noinline
    uint8_t constprop; // 0 = use heuristic; 1 = aggressive; 2 = none
    _jl_purity_overrides_t purity;
    // uint16 settings
    uint16_t inlining_cost;
} jl_code_info_t;

// 方法定义 —— 描述一个方法的定义，存储该方法所有特化共享的数据
// 读写需要 `writelock` 或独占所有权：roots, root_blocks, nroots_sysimg, ccallable
// 以下字段在构造时设置一次，读取无需锁：所有其他字段
// This type describes a single method definition, and stores data
// shared by the specializations of a function.
//
// Reading or writing requires `writelock` or exclusive ownership:
//   roots, root_blocks, nroots_sysimg, ccallable
// No lock is required to read these fields, set once on construction:
//   all other fields
typedef struct _jl_method_t {
    JL_DATA_TYPE
    jl_sym_t *name;  // for error reporting
    struct _jl_module_t *module;
    jl_sym_t *file;
    int32_t line;
    _Atomic(uint8_t) dispatch_status; // bits defined in staticdata.jl
    _Atomic(jl_genericmemory_t*) interferences; // set of intersecting methods not more specific
    _Atomic(size_t) primary_world;

    // method's type signature. redundant with TypeMapEntry->specTypes
    jl_value_t *sig;

    // table of all jl_method_instance_t specializations we have
    _Atomic(jl_value_t*) specializations; // allocated as [hashable, ..., NULL, linear, ....], or a single item
    _Atomic(jl_genericmemory_t*) speckeyset; // index lookup by hash into specializations

    jl_value_t *slot_syms; // compacted list of slot names (String)
    jl_value_t *external_mt; // reference to the method table this method is part of, null if part of the internal table
    jl_value_t *source;  // original code template (jl_code_info_t, but may be compressed), null for builtins
    jl_debuginfo_t *debuginfo;  // fixed linetable from the source argument, null if not available
    _Atomic(jl_method_instance_t*) unspecialized;  // unspecialized executable method instance, or null
    jl_value_t *generator;  // executable code-generating function if available
    jl_array_t *roots;  // pointers in generated code (shared to reduce memory), or null
    // Identify roots by module-of-origin. We only track the module for roots added during incremental compilation.
    // May be NULL if no external roots have been added, otherwise it's a Vector{UInt64}
    jl_array_t *root_blocks;   // RLE (build_id.lo, offset) pairs (even/odd indexing)
    int32_t nroots_sysimg;     // # of roots stored in the system image
    jl_svec_t *ccallable; // svec(rettype, sig) if a ccallable entry point is requested for this

    // cache of specializations of this method for invoke(), i.e.
    // cases where this method was called even though it was not necessarily
    // the most specific for the argument types.
    _Atomic(jl_typemap_t*) invokes;

    // A function that compares two specializations of this method, returning
    // `true` if the first signature is to be considered "smaller" than the
    // second for purposes of recursion analysis. Set to NULL to use
    // the default recursion relation.
    jl_value_t *recursion_relation;

    uint32_t nargs;
    uint32_t called;        // bit flags: whether each of the first 8 arguments is called
    uint32_t nospecialize;  // bit flags: which arguments should not be specialized
    uint32_t nkw;           // # of leading arguments that are actually keyword arguments
                            // of another method.
    // various boolean properties
    uint8_t isva;
    uint8_t is_for_opaque_closure;
    uint8_t nospecializeinfer;
    // bit flags, 0x01 = scanned
    // 0x02 = added to module scanned list (either from scanning or inference edge)
    // 0x04 = Source was invalidated since jl_require_world
    _Atomic(uint8_t) did_scan_source;

    // uint8 settings
    uint8_t constprop;      // 0x00 = use heuristic; 0x01 = aggressive; 0x02 = none
    uint8_t max_varargs;    // 0xFF = use heuristic; otherwise, max # of args to expand
                            // varargs when specializing.

    // Override the conclusions of inter-procedural effect analysis,
    // forcing the conclusion to always true.
    _jl_purity_overrides_t purity;

// hidden fields:
    jl_mutex_t writelock;
} jl_method_t;

// 方法实例 —— 缓存方法特定类型签名的特化数据，作为调用特定 Method 的特定参数类型的唯一字典键
// 读写需要 def.method->writelock 或独占所有权：backedges
// 读写需要关联 jl_methcache_t 的 writelock：cache_with_orig
// 以下字段在构造时设置一次，读取无需锁：def, specTypes, sparam_vals
// This type is a placeholder to cache data for a specType signature specialization of a Method
// and can be used as a unique dictionary key representation of a call to a particular Method
// with a particular set of argument types
//
// Reading or writing requires `def.method->writelock` or exclusive ownership:
//   backedges
// Reading or writing requires the associated jl_methcache_t's `writelock`:
//   cache_with_orig
// No lock is required to read these fields, set once on construction:
//   def, specTypes, sparam_vals
struct _jl_method_instance_t {
    JL_DATA_TYPE
    union {
        jl_value_t *value; // generic accessor
        struct _jl_module_t *module; // this is a toplevel thunk
        jl_method_t *method; // method this is specialized from
    } def; // pointer back to the context for this code
    jl_value_t *specTypes;  // argument types this was specialized for
    jl_svec_t *sparam_vals; // static parameter values, indexed by def.method->sig
    // list of code-instances which call this method-instance; `invoke` records (invokesig, caller) pairs
    jl_array_t *backedges;
    _Atomic(struct _jl_code_instance_t*) cache;
    uint8_t cache_with_orig; // !cache_with_specTypes

    // flags for this method instance
    //   bit 0: generated by an explicit `precompile(...)`
    //   bit 1: dispatched
    //   bit 2: The ->backedges field is currently being walked higher up the stack - entries may be deleted, but not moved
    //   bit 3: The ->backedges field was modified and should be compacted when clearing bit 2
    _Atomic(uint8_t) flags;
    _Atomic(uint8_t) dispatch_status; // bits defined in staticdata.jl
    _Atomic(uint8_t) precompile; // if set, this will be added to the output system image
};
// 方法实例标志掩码
#define JL_MI_FLAGS_MASK_PRECOMPILED    0x01
#define JL_MI_FLAGS_MASK_DISPATCHED     0x02

// 不透明闭包 —— 带有捕获变量的可调用对象
// OpaqueClosure
typedef struct _jl_opaque_closure_t {
    JL_DATA_TYPE
    jl_value_t *captures;
    size_t world;
    jl_method_t *source;
    jl_fptr_args_t invoke; // n.b. despite the similar name, this is not an invoke ABI (jl_call_t / julia.call2), but rather the fptr1 (jl_fptr_args_t / julia.call) ABI
    void *specptr; // n.b. despite the similarity in field name, this is not arbitrary private data for jlcall, but rather the codegen ABI for specsig, and is mandatory if specsig is valid
} jl_opaque_closure_t;

// 代码实例 —— 表示一个可执行操作
// 拥有对象独占所有权时写入的字段：def, owner, rettype, exctype, rettype_const, analysis_results, time_infer_total, time_infer_self
// This type represents an executable operation
//
// No lock is required to read these fields, which are set while we have
// exclusive ownership of the CodeInstance:
//   def, owner, rettype, exctype, rettype_const, analysis_results,
//   time_infer_total, time_infer_self

// 代码实例标志位
// flags bits for CodeInstance
#define JL_CI_FLAGS_SPECPTR_SPECIALIZED      0b0001
#define JL_CI_FLAGS_INVOKE_MATCHES_SPECPTR   0b0010
#define JL_CI_FLAGS_FROM_IMAGE               0b0100
#define JL_CI_FLAGS_NATIVE_CACHE_VALID       0b1000

typedef struct _jl_code_instance_t {
    JL_DATA_TYPE
    jl_value_t *def; // MethodInstance or ABIOverride
    jl_value_t *owner; // Compiler token this belongs to, `jl_nothing` is reserved for native
    _Atomic(struct _jl_code_instance_t*) next; // pointer to the next cache entry

    // world range for which this object is valid to use
    _Atomic(size_t) min_world;
    _Atomic(size_t) max_world;

    // inference state cache
    jl_value_t *rettype; // return type for fptr
    jl_value_t *exctype; // thrown type for fptr
    jl_value_t *rettype_const; // inferred constant return value, or null

    // Inferred result. When part of the runtime cache, either
    // - A jl_code_info_t (may be compressed as a String) containing the inferred IR
    // - jl_nothing, indicating that inference was completed, but the result was
    //               deleted to save space.
    // - UInt8, indicating that inference recorded the estimated inlining cost, but deleted the result to save space
    // - NULL, indicating that inference was not yet completed or did not succeed
    _Atomic(jl_value_t *) inferred;
    _Atomic(jl_debuginfo_t *) debuginfo; // stored information about edges from this object (set once, with a happens-before both source and invoke)
    _Atomic(jl_svec_t *) edges; // forward edge info

    // purity results
    jl_value_t *analysis_results; // Analysis results about this code (IPO-safe)
    // see also encode_effects() and decode_effects() in `base/compiler/effects.jl`,
    _Atomic(uint32_t) ipo_purity_bits;
    // purity_flags:
    //     uint8_t consistent          : 3;
    //     uint8_t effect_free         : 2;
    //     uint8_t nothrow             : 1;
    //     uint8_t terminates          : 1;
    //     uint8_t notaskstate         : 1;
    //     uint8_t inaccessiblememonly : 2;
    //     uint8_t noub                : 2;
    //     uint8_t nonoverlayed        : 2;

    // compilation state cache
    // these time fields have units of seconds (60 ns minimum resolution and 18 hour maximum saturates to Infinity) and are stored in Float16 format
    uint16_t time_infer_total; // total cost of computing `inferred` originally
    uint16_t time_infer_cache_saved; // adjustment to total cost, reflecting how much time was saved by having caches, to give a stable real cost without caches for comparisons
    uint16_t time_infer_self; // self cost of julia inference for `inferred` (included in time_infer_total)
    _Atomic(uint16_t) time_compile; // self cost of llvm compilation (e.g. of computing `invoke`)
    //TODO: uint8_t absolute_max; // whether true max world is unknown
    _Atomic(uint8_t) flags; // & 0b001 == specptr is a specialized function signature for specTypes->rettype
                            // & 0b010 == invokeptr matches specptr
                            // & 0b100 == From image
                            // & 0b1000 == native_cache_valid
    _Atomic(jl_callptr_t) invoke; // jlcall entry point usually, but if this codeinst belongs to an OC Method, then this is an jl_fptr_args_t fptr1 instead, unless it is not, because it is a special token object instead
    union _jl_generic_specptr_t {
        _Atomic(void*) fptr;
        _Atomic(jl_fptr_args_t) fptr1;
        // 2 constant
        _Atomic(jl_fptr_sparam_t) fptr3;
        // 4 interpreter
    } specptr; // private data for `jlcall entry point
} jl_code_instance_t;

// ABI 覆盖 —— 可用作 CodeInstance 的 ->def 字段来覆盖 ABI
// May be used as the ->def field of a CodeInstance to override the ABI
typedef struct _jl_abi_override_t {
    JL_DATA_TYPE
    jl_value_t *abi;
    jl_method_instance_t *def;
} jl_abi_override_t;

typedef struct {
    JL_DATA_TYPE
    jl_sym_t *JL_NONNULL name;
    jl_value_t *JL_NONNULL lb;   // lower bound
    jl_value_t *JL_NONNULL ub;   // upper bound
} jl_tvar_t;

// UnionAll 类型 —— 对某个变量所有值的迭代并集，写法：`body where lb<:var<:ub`
// UnionAll type (iterated union over all values of a variable in certain bounds)
// written `body where lb<:var<:ub`
typedef struct {
    JL_DATA_TYPE
    jl_tvar_t *JL_NONNULL var;
    jl_value_t *JL_NONNULL body;
} jl_unionall_t;

// 类型名称 —— 描述 DataType 的"名称"部分，表示类型的语法结构
// 存储类型不同实例化之间共享的所有数据，包括 DataType 对象的哈希共享分配缓存
// represents the "name" part of a DataType, describing the syntactic structure
// of a type and storing all data common to different instantiations of the type,
// including a cache for hash-consed allocation of DataType objects.
typedef struct {
    JL_DATA_TYPE
    jl_sym_t *name;
    struct _jl_module_t *module;
    jl_sym_t *singletonname; // sometimes used for debug printing
    jl_svec_t *names;  // field names
    const uint32_t *atomicfields; // if any fields are atomic, we record them here
    const uint32_t *constfields; // if any fields are const, we record them here
    // `wrapper` is either the only instantiation of the type (if no parameters)
    // or a UnionAll accepting parameters to make an instantiation.
    jl_value_t *wrapper;
    _Atomic(jl_value_t*) Typeofwrapper;  // cache for Type{wrapper}
    _Atomic(jl_svec_t*) cache;        // sorted array
    _Atomic(jl_svec_t*) linearcache;  // unsorted array
    jl_array_t *partial;     // incomplete instantiations of this type
    intptr_t hash;
    _Atomic(int32_t) max_args;  // max # of non-vararg arguments in a signature with this type as the function
    int32_t n_uninitialized;
    // type properties
    uint8_t abstract:1;
    uint8_t mutabl:1;
    uint8_t mayinlinealloc:1;
    uint8_t _unused:5;
    _Atomic(uint8_t) cache_entry_count; // (approximate counter of TypeMapEntry for heuristics)
    uint8_t max_methods; // override for inference's max_methods setting (0 = no additional limit or relaxation)
    uint8_t constprop_heustic; // override for inference's constprop heuristic
} jl_typename_t;

// 并集类型 —— 表示 `Union{a, b}`
typedef struct {
    JL_DATA_TYPE
    jl_value_t *JL_NONNULL a;
    jl_value_t *JL_NONNULL b;
} jl_uniontype_t;

// 交集类型 —— 内部使用的两种类型的"meet"，是 Union 的对偶
// `Intersect{a, b}` 表示 `a ∩ b`，在子类型算法中临时创建
// 用于表示无法用现有单一类型精确表达的最大下界
// Internal-use-only "meet" of two types, dual to Union: `Intersect{a, b}`
// denotes `a ∩ b`. It is created transiently inside the subtyping algorithm to
// represent a greatest-lower-bound that cannot be expressed precisely as a
// single existing type, and never escapes into user-visible types.
typedef struct {
    JL_DATA_TYPE
    jl_value_t *JL_NONNULL a;
    jl_value_t *JL_NONNULL b;
} jl_intersecttype_t;

// 类型相等 —— 包装一个类型的 Type{T} 等价表示
typedef struct {
    JL_DATA_TYPE
    jl_value_t *JL_NONNULL T;
} jl_typeeq_t;

// 字段描述符（8 位）—— 描述结构体字段的偏移、大小和是否为指针
// in little-endian, isptr is always the first bit, avoiding the need for a branch in computing isptr
typedef struct {
    uint8_t isptr:1;
    uint8_t size:7;
    uint8_t offset;   // offset relative to data start, excluding type tag
} jl_fielddesc8_t;

// 字段描述符（16 位）
typedef struct {
    uint16_t isptr:1;
    uint16_t size:15;
    uint16_t offset;   // offset relative to data start, excluding type tag
} jl_fielddesc16_t;

// 字段描述符（32 位）
typedef struct {
    uint32_t isptr:1;
    uint32_t size:31;
    uint32_t offset;   // offset relative to data start, excluding type tag
} jl_fielddesc32_t;

// 字段描述符类型枚举
typedef enum {
    JL_FIELDDESC_8 = 0,
    JL_FIELDDESC_16 = 1,
    JL_FIELDDESC_32 = 2,
    JL_FIELDDESC_FOREIGN = 3,
} jl_fielddesc_type_t;

// 数据类型布局 —— 描述 DataType 的内存布局，包括大小、字段数、指针数、对齐等
typedef struct {
    uint32_t size;
    uint32_t nfields;
    uint32_t npointers; // number of pointers embedded inside
    int32_t first_ptr; // index of the first pointer (or -1)
    uint16_t alignment; // strictest alignment over all fields
    struct { // combine these fields into a struct so that we can take addressof them
        uint16_t haspadding : 1; // has internal undefined bytes
        uint16_t fielddesc_type : 2; // jl_fielddesc_type_t
        // metadata bit only for GenericMemory eltype layout
        uint16_t arrayelem_isboxed : 1;
        uint16_t arrayelem_isunion : 1;
        uint16_t arrayelem_isatomic : 1;
        uint16_t arrayelem_islocked : 1;
        // If set, this type's egality can be determined entirely by comparing
        // the non-padding bits of this datatype.
        uint16_t isbitsegal : 1;
        uint16_t padding : 8;
    } flags;
    // union {
    //     jl_fielddesc8_t field8[nfields];
    //     jl_fielddesc16_t field16[nfields];
    //     jl_fielddesc32_t field32[nfields];
    // };
    // union { // offsets relative to data start in words
    //     uint8_t ptr8[npointers];
    //     uint16_t ptr16[npointers];
    //     uint32_t ptr32[npointers];
    // };
} jl_datatype_layout_t;

// 数据类型 —— Julia 类型的核心表示，包含类型名称、父类型、参数、字段类型、单例实例和布局信息
typedef struct _jl_datatype_t {
    JL_DATA_TYPE
    jl_typename_t *name;
    struct _jl_datatype_t *super;
    jl_svec_t *parameters;
    jl_svec_t *types;
    jl_value_t *instance;  // for singletons
    const jl_datatype_layout_t *layout;
    // memoized properties (set on construction)
    uint32_t hash;
    uint16_t hasfreetypevars:1; // majority part of isconcrete computation
    uint16_t isconcretetype:1; // whether this type can have instances
    uint16_t isdispatchtuple:1; // aka isleaftupletype
    uint16_t isbitstype:1; // relevant query for C-api and type-parameters
    uint16_t zeroinit:1; // if one or more fields requires zero-initialization
    uint16_t has_concrete_subtype:1; // If clear, no value will have this datatype
    uint16_t maybe_subtype_of_cache:1; // Computational bit for has_concrete_supertype. See description in jltypes.c.
    uint16_t isprimitivetype:1; // whether this is declared with 'primitive type' keyword (sized, no fields, and immutable)
    uint16_t ismutationfree:1; // whether any mutable memory is reachable through this type (in the type or via fields)
    uint16_t isidentityfree:1; // whether this type or any object reachable through its fields has non-content-based identity
    uint16_t smalltag:6; // whether this type has a small-tag optimization
} jl_datatype_t;

// 可变参数类型 —— 表示 Vararg{T, N}
typedef struct _jl_vararg_t {
    JL_DATA_TYPE
    jl_value_t *T;
    jl_value_t *N;
} jl_vararg_t;

// 弱引用 —— 不阻止 GC 回收的引用
typedef struct _jl_weakref_t {
    JL_DATA_TYPE
    jl_value_t *value;
} jl_weakref_t;

// N.B: Needs to be synced with runtime_internals.jl
// We track essentially three levels of binding strength:
//
// 1. Implicit Bindings (Weakest)
//   These binding kinds depend solely on the set of using'd packages and are not explicitly
//   declared:
//
//      PARTITION_KIND_IMPLICIT_CONST
//      PARTITION_KIND_IMPLICIT_GLOBAL
//      PARTITION_KIND_GUARD
//      PARTITION_KIND_FAILED
//
// 2. Weakly Declared Bindings (Weak)
//    The binding was declared using `global`. It is treated as a mutable, `Any` type global
//    for almost all purposes, except that it receives slightly worse optimizations, since it
//    may be replaced.
//
//      PARTITION_KIND_DECLARED
//
// 3. Strong Declared Bindings (Strong)
//    All other bindings are explicitly declared using a keyword or global assignment.
//   These are considered strongest:
//
//      PARTITION_KIND_CONST
//      PARTITION_KIND_CONST_IMPORT
//      PARTITION_KIND_EXPLICIT
//      PARTITION_KIND_IMPORTED
//      PARTITION_KIND_GLOBAL
//      PARTITION_KIND_UNDEF_CONST
//
// The runtime supports syntactic invalidation (by raising the world age and changing the partition type
// in the new world age) from any partition kind to any other.
//
// However, not all transitions are allowed syntactically. We have the following rules for SYNTACTIC invalidation:
// 1. It is always syntactically permissible to replace a weaker binding by a stronger binding
// 2. Implicit bindings can be syntactically changed to other implicit bindings by changing the `using` set.
// 3. Finally, we syntactically permit replacing one PARTITION_KIND_CONST(_IMPORT) by another of a different value.
//
// We may make this list more permissive in the future.
//
// Finally, PARTITION_KIND_BACKDATED_CONST is a special case, and the only case where we may replace an
// existing partition by a different partition kind in the same world age. As such, it needs special
// support in inference. Any partition kind that may be replaced by a PARTITION_KIND_BACKDATED_CONST
// must be inferred accordingly. PARTITION_KIND_BACKDATED_CONST is intended as a temporary compatibility
// measure. The following kinds may be replaced by PARTITION_KIND_BACKDATED_CONST:
//  - PARTITION_KIND_GUARD
//  - PARTITION_KIND_FAILED
//  - PARTITION_KIND_DECLARED
// 绑定分区类型枚举 —— 描述模块中绑定的类型和强度
enum jl_partition_kind {
    // Constant: This binding partition is a constant declared using `const _ = ...`
    //  ->restriction holds the constant value
    PARTITION_KIND_CONST        = 0x0,
    // Import Constant: This binding partition is a constant declared using `import A`
    //  ->restriction holds the constant value
    PARTITION_KIND_CONST_IMPORT = 0x1,
    // Global: This binding partition is a global variable. It was declared either using
    // `global x::T` or implicitly through a syntactic global assignment.
    //  -> restriction holds the type restriction
    PARTITION_KIND_GLOBAL       = 0x2,
    // Implicit: The binding was a global, implicitly imported from a `using`'d module.
    //  ->restriction holds the ultimately imported global binding
    PARTITION_KIND_IMPLICIT_GLOBAL     = 0x3,
    // Implicit: The binding was a constant, implicitly imported from a `using`'d module.
    //  ->restriction holds the ultimately imported constant value
    PARTITION_KIND_IMPLICIT_CONST     = 0x4,
    // Explicit: The binding was explicitly `using`'d by name
    //  ->restriction holds the imported binding
    PARTITION_KIND_EXPLICIT     = 0x5,
    // Imported: The binding was explicitly `import`'d by name
    //  ->restriction holds the imported binding
    PARTITION_KIND_IMPORTED     = 0x6,
    // Failed: We attempted to import the binding, but the import was ambiguous
    //  ->restriction is NULL.
    PARTITION_KIND_FAILED       = 0x7,
    // Declared: The binding was declared using `global` or similar. This acts in most ways like
    // PARTITION_KIND_GLOBAL with an `Any` restriction, except that it may be redefined to a stronger
    // binding like `const` or an explicit import.
    //  ->restriction is NULL.
    PARTITION_KIND_DECLARED     = 0x8,
    // Guard: The binding was looked at, but no global or import was resolved at the time
    //  ->restriction is NULL.
    PARTITION_KIND_GUARD        = 0x9,
    // Undef Constant: This binding partition is a constant declared using `const`, but
    // without a value.
    //  ->restriction is NULL
    PARTITION_KIND_UNDEF_CONST  = 0xa,
    // Backated constant. A constant that was backdated for compatibility. In all other
    // ways equivalent to PARTITION_KIND_CONST, but prints a warning on access
    PARTITION_KIND_BACKDATED_CONST = 0xb,

    // This is not a real binding kind, but can be used to ask for a re-resolution
    // of the implicit binding kind
    PARTITION_FAKE_KIND_IMPLICIT_RECOMPUTE = 0xc,
    PARTITION_FAKE_KIND_CYCLE = 0xd
};

// 绑定分区类型掩码和标志掩码
static const uint8_t PARTITION_MASK_KIND = 0x0f;
static const uint8_t PARTITION_MASK_FLAG = 0xf0;

//// These are flags that get anded into the above
//
// _EXPORTED: This binding partition is exported. In the world ranges covered by this partitions,
// other modules that `using` this module, may implicit import this binding.
// 绑定分区标志位常量
static const uint8_t PARTITION_FLAG_EXPORTED       = 0x10;
// _DEPRECATED: This binding partition is deprecated. It is considered weak for the purposes of
// implicit import resolution.
static const uint8_t PARTITION_FLAG_DEPRECATED     = 0x20;
// _DEPWARN: This binding partition will print a deprecation warning on access. Note that _DEPWARN
// implies _DEPRECATED. However, the reverse is not true. Such bindings are usually used for functions,
// where calling the function itself will provide a (better) deprecation warning/error.
static const uint8_t PARTITION_FLAG_DEPWARN        = 0x40;
// _IMPLICITLY_EXPORTED: This binding partition is implicitly exported via @reexport. Unlike _EXPORTED,
// this flag is set during implicit resolution and can be removed if the resolution changes.
static const uint8_t PARTITION_FLAG_IMPLICITLY_EXPORTED = 0x80;

#if defined(_COMPILER_MICROSOFT_)
#define JL_ALIGNED_ATTR(alignment) \
    __declspec(align(alignment))
#else
#define JL_ALIGNED_ATTR(alignment) \
    __attribute__((aligned(alignment)))
#endif

// 绑定分区 —— 表示变量绑定的一个分区（按世界年龄分段）
// 包含约束（常量值、类型限制或导入的绑定）以及世界年龄范围
typedef struct JL_ALIGNED_ATTR(8) _jl_binding_partition_t {
    JL_DATA_TYPE
    /* union {
     *   // For ->kind == PARTITION_KIND_GLOBAL
     *   jl_value_t *type_restriction;
     *   // For ->kind in (PARTITION_KIND_CONST(_IMPORT), PARTITION_KIND_IMPLICIT_CONST)
     *   jl_value_t *constval;
     *   // For ->kind in (PARTITION_KIND_IMPLICIT_GLOBAL, PARTITION_KIND_EXPLICIT, PARTITION_KIND_IMPORT)
     *   jl_binding_t *imported;
     * } restriction;
     */
    jl_value_t *restriction;
    _Atomic(size_t) min_world;
    _Atomic(size_t) max_world;
    _Atomic(struct _jl_binding_partition_t *) next;
    size_t kind;
} jl_binding_partition_t;

// 获取绑定分区的类型（取低 4 位）
STATIC_INLINE enum jl_partition_kind jl_binding_kind(jl_binding_partition_t *bpart) JL_NOTSAFEPOINT
{
    return (enum jl_partition_kind)(bpart->kind & 0xf);
}

// 绑定标志枚举
enum jl_binding_flags {
    BINDING_FLAG_DID_PRINT_BACKDATE_ADMONITION        = 0x1,
    BINDING_FLAG_DID_PRINT_IMPLICIT_IMPORT_ADMONITION = 0x2,
    // `export` is tracked in partitions, but sets this as well
    BINDING_FLAG_PUBLICP                              = 0x4,
    // Set if any methods defined in this module implicitly reference
    // this binding. If not, invalidation is optimized.
    BINDING_FLAG_ANY_IMPLICIT_EDGES                   = 0x8
};

// 模块绑定 —— 连接变量名和其值的对象
typedef struct _jl_binding_t {
    JL_DATA_TYPE
    jl_globalref_t *globalref;  // cached GlobalRef for this binding
    _Atomic(jl_value_t*) value;
    _Atomic(jl_binding_partition_t*) partitions;
    jl_array_t *backedges;
    _Atomic(uint8_t) flags;
} jl_binding_t;

typedef struct {
    uint64_t hi;
    uint64_t lo;
} jl_uuid_t;

// 模块 —— Julia 的命名空间和变量作用域
// Reading or writing requires `lock`:
//   scanned_methods, usings
// Reading or writing requires `Base.require_lock`:
//   uuid
// Reading or writing requires `world_counter_lock`:
//   usings_backedges (TODO)
// No lock is required to read these fields, set once on construction:
//   name, parent, file, line, build_id, uuid, nospecialize, optlevel, compile,
//   infer, iistopmod, max_methods
typedef struct _jl_module_t {
    JL_DATA_TYPE
    jl_sym_t *name;
    struct _jl_module_t *parent;
    _Atomic(jl_svec_t*) bindings;
    _Atomic(jl_genericmemory_t*) bindingkeyset; // index lookup by name into bindings
    jl_sym_t *file;
    int32_t line;
    jl_value_t *usings_backedges;
    jl_value_t *scanned_methods;
    // hidden fields:
    arraylist_t usings; /* arraylist of struct jl_module_using */  // modules with all bindings potentially imported
    jl_uuid_t build_id;
    jl_uuid_t uuid;
    _Atomic(uint32_t) counter;
    int32_t nospecialize;  // global bit flags: initialization for new methods
    int8_t optlevel;
    int8_t compile;
    int8_t infer;
    uint8_t istopmod;
    int8_t max_methods;
    // If cleared no binding partition in this module has PARTITION_FLAG_EXPORTED and min_world > jl_require_world.
    _Atomic(int8_t) export_set_changed_since_require_world;
    // Set if this module has any reexport usings (used to bypass fast-path in implicit resolution)
    _Atomic(int8_t) has_reexports;
    jl_mutex_t lock;
    intptr_t hash;
} jl_module_t;

// 模块 using 记录 —— 表示模块对另一个模块的 using 关系及其世界年龄范围
struct _jl_module_using {
    jl_module_t *mod;
    size_t min_world;
    size_t max_world;
    size_t flags;
};

// Flags for _jl_module_using.flags
static const uint8_t JL_MODULE_USING_REEXPORT = 0x1;

// 全局引用 —— 由模块和符号名组成的对 (GlobalRef)
struct _jl_globalref_t {
    JL_DATA_TYPE
    jl_module_t *mod;
    jl_sym_t *name;
    jl_binding_t *binding;
};

// 类型映射条目 —— 类型到值映射的一个条目（侵入式链表节点）
// one Type-to-Value entry
struct _jl_typemap_entry_t {
    JL_DATA_TYPE
    _Atomic(struct _jl_typemap_entry_t*) next; // invasive linked list
    jl_tupletype_t *sig; // the type signature for this entry
    jl_tupletype_t *simplesig; // a simple signature for fast rejection
    jl_svec_t *guardsigs;
    _Atomic(size_t) min_world;
    _Atomic(size_t) max_world;
    union {
        jl_value_t *value; // generic accessor
        jl_method_instance_t *linfo; // [nullable] for guard entries
        jl_method_t *method;
    } func;
    // memoized properties of sig:
    int8_t isleafsig; // isleaftype(sig) & !any(isType, sig) : unsorted and very fast
    int8_t issimplesig; // all(isleaftype | isAny | isType | isVararg, sig) : sorted and fast
    int8_t va; // isVararg(sig)
};

// 类型映射层级 —— TypeMap 树中的一层（按给定偏移量进行类型拆分）
// one level in a TypeMap tree (each level splits on a type at a given offset)
typedef struct _jl_typemap_level_t {
    JL_DATA_TYPE
    // these vectors contains vectors of more levels in their intended visit order
    // with an index that gives the functionality of a sorted dict.
    // next split may be on Type{T} as LeafTypes then TypeName's parents up to Any
    // next split may be on LeafType
    // next split may be on TypeName
    _Atomic(jl_genericmemory_t*) arg1; // contains LeafType (in a map of non-abstract TypeName)
    _Atomic(jl_genericmemory_t*) targ; // contains Type{LeafType} (in a map of non-abstract TypeName)
    _Atomic(jl_genericmemory_t*) name1; // a map for a map for TypeName, for parents up to (excluding) Any
    _Atomic(jl_genericmemory_t*) tname; // a map for Type{TypeName}, for parents up to (including) Any
    // next a linear list of things too complicated at this level for analysis (no more levels)
    _Atomic(jl_typemap_entry_t*) linear;
    // finally, start a new level if the type at offs is Any
    _Atomic(jl_typemap_t*) any;
} jl_typemap_level_t;

// 方法缓存 —— 方法分派的哈希表缓存
typedef struct _jl_methcache_t {
    JL_DATA_TYPE
    // hash map from dispatchtuple type to a linked-list of TypeMapEntry
    // entry.sig == type for all entries in the linked-list
    _Atomic(jl_genericmemory_t*) leafcache;

    // cache for querying everything else (anything that didn't seem profitable to put into leafcache)
    _Atomic(jl_typemap_t*) cache;

    jl_mutex_t writelock;
} jl_methcache_t;

// 方法表 —— 包含全局方法表
// contains global MethodTable
typedef struct _jl_methtable_t {
    JL_DATA_TYPE
    // full set of entries
    _Atomic(jl_typemap_t*) defs;
    jl_methcache_t *cache;
    jl_sym_t *name; // sometimes used for debug printing
    jl_module_t *module; // sometimes used for debug printing
    jl_genericmemory_t *backedges; // IdDict{top typenames, Vector{uncovered (sig => caller::CodeInstance)}}
} jl_methtable_t;

// 表达式 —— 抽象语法树节点，包含头部符号和参数数组
typedef struct {
    JL_DATA_TYPE
    jl_sym_t *head;
    jl_array_t *args;
} jl_expr_t;

typedef struct {
    JL_DATA_TYPE
    jl_tupletype_t *spec_types;
    jl_svec_t *sparams;
    jl_method_t *method;
    // A bool on the julia side, but can be temporarily 0x2 as a sentinel
    // during construction.
    uint8_t fully_covers;
} jl_method_match_t;

/* ======= 常量与类型对象 ======= */
// constants and type objects -------------------------------------------------

// 小类型标签宏 —— 所有可以快速识别的基本类型
#define JL_SMALL_TYPEOF(XX) \
    /* kinds */ \
    XX(typeofbottom) \
    XX(datatype) \
    XX(unionall) \
    XX(uniontype) \
    /* type parameter objects */ \
    XX(vararg) \
    XX(tvar) \
    XX(symbol) \
    XX(module) \
    /* special GC objects */ \
    XX(simplevector) \
    XX(string) \
    XX(task) \
    /* bits types with special allocators */ \
    XX(bool) \
    XX(nothing) \
    XX(char) \
    /*XX(float16)*/ \
    /*XX(float32)*/ \
    /*XX(float64)*/ \
    /*XX(bfloat16)*/ \
    XX(int16) \
    XX(int32) \
    XX(int64) \
    XX(int8) \
    XX(uint16) \
    XX(uint32) \
    XX(uint64) \
    XX(uint8) \
    XX(addrspacecore) \
    XX(intrinsic) \
    /* AST objects */ \
    XX(argument) \
    /* XX(newvarnode) */ \
    XX(slotnumber) \
    XX(ssavalue) \
    XX(gotoifnot) \
    XX(returnnode) \
    XX(enternode) \
    XX(pinode) \
    XX(phinode) \
    XX(phicnode) \
    XX(upsilonnode) \
    XX(globalref) \
    XX(gotonode) \
    XX(quotenode) \
    XX(typeeq) \
    /* Add new tags here to keep existing builds ABI stable - we don't guarantee ABI \
       stability, but it'll help PkgEval to not break it unnecessarily */ \
    /* end of JL_SMALL_TYPEOF */
// 小类型标签的枚举常量
enum jl_small_typeof_tags {
    jl_null_tag = 0,
#define XX(name) jl_##name##_tag,
    JL_SMALL_TYPEOF(XX)
#undef XX
    jl_tags_count,
    jl_bitstags_first = jl_char_tag, // n.b. bool is not considered a bitstype, since it can be compared by pointer
    jl_max_tags = 64
};
extern JL_DLLIMPORT jl_datatype_t *jl_small_typeof[(jl_max_tags << 4) / sizeof(jl_datatype_t*)];
#ifndef JL_LIBRARY_EXPORTS_INTERNAL
static inline jl_value_t *jl_to_typeof(uintptr_t t)
{
    if (t < (jl_max_tags << 4))
        return (jl_value_t*)jl_small_typeof[t / sizeof(*jl_small_typeof)];
    return (jl_value_t*)t;
}
#else
extern JL_HIDDEN jl_datatype_t *ijl_small_typeof[(jl_max_tags << 4) / sizeof(jl_datatype_t*)];
static inline jl_value_t *jl_to_typeof(uintptr_t t)
{
    if (t < (jl_max_tags << 4))
        return (jl_value_t*)ijl_small_typeof[t / sizeof(*ijl_small_typeof)];
    return (jl_value_t*)t;
}
#endif


#define jl_tuple_type jl_anytuple_type

#if !defined(JL_LIBRARY_EXPORTS_INTERNAL) || defined(__clang_analyzer__)
#define XX(name, type) extern JL_DLLIMPORT type jl_##name JL_GLOBALLY_ROOTED;
JL_EXPORTED_DATA_POINTERS(XX)
#undef XX

#define XX(name, type) extern JL_DLLIMPORT type jl_##name JL_GLOBALLY_ROOTED;
JL_CONST_GLOBAL_VARS(XX)
#undef XX

#else
// Struct definitions for global data access (internal hidden copies)
struct jl_sysimg_global {
#define XX(name, type) type name JL_GLOBALLY_ROOTED;
    JL_EXPORTED_DATA_POINTERS(XX)
#undef XX
};

struct jl_const_globals {
#define XX(name, type) type jl##name JL_GLOBALLY_ROOTED;
    JL_CONST_GLOBAL_VARS(XX)
#undef XX
};

extern JL_HIDDEN struct jl_sysimg_global sysimg_global;
extern JL_HIDDEN struct jl_const_globals const_globals;
#include <jl_data_globals_defs.inc>
#endif

#define XX(name, type) extern JL_DLLIMPORT type name;
JL_EXPORTED_DATA_SYMBOLS(XX)
#undef XX

/* ======= 垃圾回收（GC） ======= */
// gc -------------------------------------------------------------------------

// GC 帧 —— 用于在栈上注册 GC 根，防止被回收
struct _jl_gcframe_t {
    size_t nroots;
    struct _jl_gcframe_t *prev;
    // actual roots go here
};

// NOTE: it is the caller's responsibility to make sure arguments are
// rooted such that the gc can see them on the stack.
// `foo(f(), g())` is not safe,
// since the result of `f()` is not rooted during the call to `g()`,
// and the arguments to foo are not gc-protected during the call to foo.
// foo can't do anything about it, so the caller must do:
// jl_value_t *x=NULL, *y=NULL; JL_GC_PUSH2(&x, &y);
// x = f(); y = g(); foo(x, y)

// 获取当前任务的 GC 栈指针
#define jl_pgcstack (jl_current_task->gcstack)

// GC 推送编码宏 —— 将根数量和类型编码到帧头中
#define JL_GC_ENCODE_PUSHARGS(n)   (((size_t)(n))<<2)
#define JL_GC_ENCODE_PUSH(n)       ((((size_t)(n))<<2)|1)
#define JL_GC_DECODE_NROOTS(n)     (n >> 2)

#ifdef __clang_gcanalyzer__

// When running with the analyzer make these real function calls, that are
// easier to detect in the analyzer
extern void JL_GC_PUSH1(void *) JL_NOTSAFEPOINT;
extern void JL_GC_PUSH2(void *, void *) JL_NOTSAFEPOINT;
extern void JL_GC_PUSH3(void *, void *, void *)  JL_NOTSAFEPOINT;
extern void JL_GC_PUSH4(void *, void *, void *, void *)  JL_NOTSAFEPOINT;
extern void JL_GC_PUSH5(void *, void *, void *, void *, void *)  JL_NOTSAFEPOINT;
extern void JL_GC_PUSH6(void *, void *, void *, void *, void *, void *)  JL_NOTSAFEPOINT;
extern void JL_GC_PUSH7(void *, void *, void *, void *, void *, void *, void *)  JL_NOTSAFEPOINT;
extern void JL_GC_PUSH8(void *, void *, void *, void *, void *, void *, void *, void *)  JL_NOTSAFEPOINT;
extern void JL_GC_PUSH9(void *, void *, void *, void *, void *, void *, void *, void *, void *)  JL_NOTSAFEPOINT;
extern void _JL_GC_PUSHARGS(jl_value_t **, size_t) JL_NOTSAFEPOINT;
// This is necessary, because otherwise the analyzer considers this undefined
// behavior and terminates the exploration
#define JL_GC_PUSHARGS(rts_var, n)     \
  rts_var = (jl_value_t **)alloca(sizeof(void*) * (n)); \
  memset(rts_var, 0, sizeof(void*) * (n)); \
  _JL_GC_PUSHARGS(rts_var, (n));

extern void JL_GC_POP() JL_NOTSAFEPOINT;

#else

// GC 推送宏 —— 将变量注册为 GC 根（1 个参数）
#define JL_GC_PUSH1(arg1)                                                                               \
  void *__gc_stkf[] = {(void*)JL_GC_ENCODE_PUSH(1), jl_pgcstack, arg1};                                 \
  jl_pgcstack = (jl_gcframe_t*)__gc_stkf;

// GC 推送宏（2 个参数）
#define JL_GC_PUSH2(arg1, arg2)                                                                         \
  void *__gc_stkf[] = {(void*)JL_GC_ENCODE_PUSH(2), jl_pgcstack, arg1, arg2};                           \
  jl_pgcstack = (jl_gcframe_t*)__gc_stkf;

#define JL_GC_PUSH3(arg1, arg2, arg3)                                                                   \
  void *__gc_stkf[] = {(void*)JL_GC_ENCODE_PUSH(3), jl_pgcstack, arg1, arg2, arg3};                     \
  jl_pgcstack = (jl_gcframe_t*)__gc_stkf;

#define JL_GC_PUSH4(arg1, arg2, arg3, arg4)                                                             \
  void *__gc_stkf[] = {(void*)JL_GC_ENCODE_PUSH(4), jl_pgcstack, arg1, arg2, arg3, arg4};               \
  jl_pgcstack = (jl_gcframe_t*)__gc_stkf;

#define JL_GC_PUSH5(arg1, arg2, arg3, arg4, arg5)                                                       \
  void *__gc_stkf[] = {(void*)JL_GC_ENCODE_PUSH(5), jl_pgcstack, arg1, arg2, arg3, arg4, arg5};         \
  jl_pgcstack = (jl_gcframe_t*)__gc_stkf;

#define JL_GC_PUSH6(arg1, arg2, arg3, arg4, arg5, arg6)                                                 \
  void *__gc_stkf[] = {(void*)JL_GC_ENCODE_PUSH(6), jl_pgcstack, arg1, arg2, arg3, arg4, arg5, arg6};   \
  jl_pgcstack = (jl_gcframe_t*)__gc_stkf;

#define JL_GC_PUSH7(arg1, arg2, arg3, arg4, arg5, arg6, arg7)                                           \
  void *__gc_stkf[] = {(void*)JL_GC_ENCODE_PUSH(7), jl_pgcstack, arg1, arg2, arg3, arg4, arg5, arg6, arg7}; \
  jl_pgcstack = (jl_gcframe_t*)__gc_stkf;

#define JL_GC_PUSH8(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8)                                     \
  void *__gc_stkf[] = {(void*)JL_GC_ENCODE_PUSH(8), jl_pgcstack, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8}; \
  jl_pgcstack = (jl_gcframe_t*)__gc_stkf;

#define JL_GC_PUSH9(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9)                               \
  void *__gc_stkf[] = {(void*)JL_GC_ENCODE_PUSH(9), jl_pgcstack, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9}; \
  jl_pgcstack = (jl_gcframe_t*)__gc_stkf;


// 推送可变数量的 GC 根（使用 alloca 分配栈空间）
#define JL_GC_PUSHARGS(rts_var,n)                                                                       \
  rts_var = ((jl_value_t**)alloca(((n)+2)*sizeof(jl_value_t*)))+2;                                      \
  ((void**)rts_var)[-2] = (void*)JL_GC_ENCODE_PUSHARGS(n);                                              \
  ((void**)rts_var)[-1] = jl_pgcstack;                                                                  \
  memset((void*)rts_var, 0, (n)*sizeof(jl_value_t*));                                                   \
  jl_pgcstack = (jl_gcframe_t*)&(((void**)rts_var)[-2])

// 弹出 GC 帧
#define JL_GC_POP() (jl_pgcstack = jl_pgcstack->prev)

#endif

// 添加 GC 终结器
// 调试打印
JL_DLLEXPORT void jl_gc_add_finalizer(jl_value_t *v, jl_value_t *f) JL_NOTSAFEPOINT;
// 添加指针级终结器
JL_DLLEXPORT void jl_gc_add_ptr_finalizer(jl_ptls_t ptls, jl_value_t *v, void *f) JL_NOTSAFEPOINT;
// 添加静默期回调
JL_DLLEXPORT void jl_gc_add_quiescent(jl_ptls_t ptls, void **v, void *f) JL_NOTSAFEPOINT;
// 强制执行终结器
JL_DLLEXPORT void jl_finalize(jl_value_t *o);
// 分配任务栈空间
JL_DLLEXPORT void *jl_malloc_stack(size_t *bufsz, struct _jl_task_t *owner) JL_NOTSAFEPOINT;
// 释放任务栈空间
JL_DLLEXPORT void jl_free_stack(void *stkbuf, size_t bufsz);

// Allocates a new weak-reference, assigns its value and increments Julia allocation
// counters. If thread-local allocators are used, then this function should allocate in the
// thread-local allocator of the current thread.
// 分配新的弱引用
JL_DLLEXPORT jl_weakref_t *jl_gc_new_weakref(jl_value_t *value);

// GC 安全点函数
JL_DLLEXPORT void jl_gc_safepoint(void);
// 暂停指定线程
JL_DLLEXPORT int jl_safepoint_suspend_thread(int tid, int waitstate);
// 暂停所有线程
JL_DLLEXPORT void jl_safepoint_suspend_all_threads(struct _jl_task_t *ct);
// 恢复所有线程
JL_DLLEXPORT void jl_safepoint_resume_all_threads(struct _jl_task_t *ct);
// 恢复指定线程
JL_DLLEXPORT int jl_safepoint_resume_thread(int tid) JL_NOTSAFEPOINT;

void *mtarraylist_get(small_arraylist_t *_a, size_t idx) JL_NOTSAFEPOINT;
size_t mtarraylist_length(small_arraylist_t *_a) JL_NOTSAFEPOINT;
void mtarraylist_add(small_arraylist_t *_a, void *elt, size_t idx) JL_NOTSAFEPOINT;
void mtarraylist_push(small_arraylist_t *_a, void *elt) JL_NOTSAFEPOINT;

/* ======= 对象访问器 ======= */
// object accessors -----------------------------------------------------------

// SimpleVector 长度和数据访问
#define jl_svec_len(t)              (((jl_svec_t*)(t))->length)
#define jl_svec_set_len_unsafe(t,n) (((jl_svec_t*)(t))->length=(n))
#define jl_svec_data(t) ((jl_value_t**)((char*)(t) + sizeof(jl_svec_t)))

#ifdef __clang_gcanalyzer__
STATIC_INLINE jl_value_t *jl_svecref(void *t JL_PROPAGATES_ROOT, size_t i) JL_PROPAGATES_ROOT_INDEXED(0, 1) JL_NOTSAFEPOINT;
STATIC_INLINE jl_value_t *jl_svecset(
    void *t JL_PROPAGATES_ROOT,
    size_t i, void *x JL_ROOTED_BY_ARG_INDEXED(0, 1)) JL_NOTSAFEPOINT;
#else
STATIC_INLINE jl_value_t *jl_svecref(void *t JL_PROPAGATES_ROOT, size_t i) JL_PROPAGATES_ROOT_INDEXED(0, 1) JL_NOTSAFEPOINT
{
    assert(jl_typetagis(t,jl_simplevector_tag << 4));
    assert(i < jl_svec_len(t));
    // while svec is supposedly immutable, in practice we sometimes publish it first
    // and set the values lazily
    return jl_atomic_load_relaxed((_Atomic(jl_value_t*)*)jl_svec_data(t) + i);
}
STATIC_INLINE jl_value_t *jl_svecset(
    void *t JL_PROPAGATES_ROOT,
    size_t i, void *x JL_ROOTED_BY_ARG_INDEXED(0, 1)) JL_NOTSAFEPOINT
{
    assert(jl_typetagis(t,jl_simplevector_tag << 4));
    assert(i < jl_svec_len(t));
    // while svec is supposedly immutable, in practice we sometimes publish it
    // first and set the values lazily. Those users occasionally might need to
    // instead use jl_atomic_store_release here.
    jl_gc_wb(t, x);
    jl_atomic_store_relaxed((_Atomic(jl_value_t*)*)jl_svec_data(t) + i, (jl_value_t*)x);
    return (jl_value_t*)x;
}
#endif

// GenericMemory 的 owner 字段访问宏
#define jl_genericmemory_data_owner_field(a) (*(jl_value_t**)((jl_genericmemory_t*)(a) + 1))

// DataType 参数访问宏
#define jl_nparams(t)  jl_svec_len(((jl_datatype_t*)(t))->parameters)
#define jl_tparam0(t)  jl_svecref(((jl_datatype_t*)(t))->parameters, 0)
#define jl_tparam1(t)  jl_svecref(((jl_datatype_t*)(t))->parameters, 1)
#define jl_tparam2(t)  jl_svecref(((jl_datatype_t*)(t))->parameters, 2)
#define jl_tparam(t,i) jl_svecref(((jl_datatype_t*)(t))->parameters, i)
// 数组数据访问宏
#define jl_array_data(a,t) ((t*)((jl_array_t*)(a))->ref.ptr_or_offset)
#define jl_array_data_(a) ((void*)((jl_array_t*)(a))->ref.ptr_or_offset)
#define jl_array_dim(a,i) (((jl_array_t*)(a))->dimsize[i])
#define jl_array_dim0(a)  (((jl_array_t*)(a))->dimsize[0])
#define jl_array_nrows(a) (((jl_array_t*)(a))->dimsize[0])
#define jl_array_ndims(a) (*(size_t*)jl_tparam1(jl_typetagof(a)))
#define jl_array_maxsize(a) (((jl_array_t*)(a))->ref.mem->length)
#define jl_array_len(a)   (jl_array_ndims(a) == 1 ? jl_array_nrows(a) : jl_array_maxsize(a))

// 获取 GC 栈指针
JL_DLLEXPORT JL_CONST_FUNC jl_gcframe_t **(jl_get_pgcstack)(void) JL_GLOBALLY_ROOTED JL_NOTSAFEPOINT;
// 获取当前任务
#define jl_current_task (container_of(jl_get_pgcstack(), jl_task_t, gcstack))

STATIC_INLINE jl_value_t *jl_genericmemory_owner(jl_genericmemory_t *m JL_PROPAGATES_ROOT) JL_NOTSAFEPOINT;

/* ======= 写屏障 ======= */
// write barriers

#ifndef WITH_THIRD_PARTY_HEAP
#include "gc-wb-stock.h"
#else
// Pick the appropriate third-party implementation
#ifdef WITH_THIRD_PARTY_HEAP
#if WITH_THIRD_PARTY_HEAP == 1 // MMTk
#include "gc-wb-mmtk.h"
#endif
#endif
#endif

// Utility for doing a basic write with the appropriate write barrier.
// `parent` is the GC-tracked owner, `field` is an lvalue (e.g. obj->member),
// and `val` is the new value to store. The barrier is emitted *before* the
// store and receives the new value being stored.
// 带写屏障的基本写操作：`parent` 是 GC 跟踪的所有者，`field` 是左值，`val` 是新值
#define jl_gc_write(parent, field, val) do { \
    void *_jl_write_val = (void*)(val); \
    jl_gc_wb((parent), _jl_write_val); \
    (field) = (__typeof__(field))_jl_write_val; \
} while (0)

// Atomic variant: `field` must be an _Atomic lvalue, `order` is relaxed or release.
// 原子写屏障变体 —— `field` 必须是 _Atomic 左值
#define jl_gc_write_atomic(parent, field, val, order) do { \
    __typeof__(jl_atomic_load_relaxed(&(field))) _jl_write_val = (__typeof__(jl_atomic_load_relaxed(&(field))))(val); \
    jl_gc_wb((parent), (const void *)_jl_write_val); \
    jl_atomic_store_##order(&(field), _jl_write_val); \
} while (0)

/*
  how - allocation style
  0 = data is inlined
  1 = owns the gc-managed data, exclusively (will free it)
  2 = malloc-allocated pointer (does not own it)
  3 = has a pointer to the String object that owns the data pointer (m must be isbits)
*/
// GenericMemory 数据管理方式常量
// 0 = 内联数据  1 = GC 管理  2 = malloc 分配  3 = 字符串拥有
#define JL_GENERICMEMORY_INLINED     0
#define JL_GENERICMEMORY_GCMANAGED   1
#define JL_GENERICMEMORY_MALLOCD     2
#define JL_GENERICMEMORY_STRINGOWNED 3

// 判断 GenericMemory 的数据管理方式
STATIC_INLINE int jl_genericmemory_how(jl_genericmemory_t *m) JL_NOTSAFEPOINT
{
    if (m->ptr == (void*)((char*)m + 16)) // JL_SMALL_BYTE_ALIGNMENT (from julia_internal.h)
        return JL_GENERICMEMORY_INLINED;
    jl_value_t *owner = jl_genericmemory_data_owner_field(m);
    if (owner == (jl_value_t*)m)
        return JL_GENERICMEMORY_GCMANAGED;
    if (owner == NULL)
        return JL_GENERICMEMORY_MALLOCD;
    return JL_GENERICMEMORY_STRINGOWNED;
}

STATIC_INLINE jl_value_t *jl_genericmemory_owner(jl_genericmemory_t *m JL_PROPAGATES_ROOT) JL_NOTSAFEPOINT
{
    return (jl_value_t*)m;
}

JL_DLLEXPORT char *jl_genericmemory_typetagdata(jl_genericmemory_t *m) JL_NOTSAFEPOINT;

#ifdef __clang_gcanalyzer__
jl_value_t **jl_genericmemory_ptr_data(jl_genericmemory_t *m JL_PROPAGATES_ROOT) JL_NOTSAFEPOINT;
// 读取 GenericMemory 中的指针元素
STATIC_INLINE jl_value_t *jl_genericmemory_ptr_ref(void *m JL_PROPAGATES_ROOT, size_t i) JL_PROPAGATES_ROOT_INDEXED(0, 1) JL_NOTSAFEPOINT;
// 设置 GenericMemory 中的指针元素（带写屏障）
STATIC_INLINE jl_value_t *jl_genericmemory_ptr_set(
    void *m, size_t i,
    void *x JL_ROOTED_BY_ARG_INDEXED(0, 1)) JL_NOTSAFEPOINT;
#else
// 获取 GenericMemory 的指针数据数组
#define jl_genericmemory_ptr_data(a)  ((jl_value_t**)((jl_genericmemory_t*)(a))->ptr)
STATIC_INLINE jl_value_t *jl_genericmemory_ptr_ref(void *m JL_PROPAGATES_ROOT, size_t i) JL_PROPAGATES_ROOT_INDEXED(0, 1) JL_NOTSAFEPOINT
{
    jl_genericmemory_t *m_ = (jl_genericmemory_t*)m;
    assert(((jl_datatype_t*)jl_typetagof(m_))->layout->flags.arrayelem_isboxed);
    assert(i < m_->length);
    return jl_atomic_load_relaxed(((_Atomic(jl_value_t*)*)(m_->ptr)) + i);
}
STATIC_INLINE jl_value_t *jl_genericmemory_ptr_set(
    void *m, size_t i,
    void *x JL_ROOTED_BY_ARG_INDEXED(0, 1)) JL_NOTSAFEPOINT
{
    jl_genericmemory_t *m_ = (jl_genericmemory_t*)m;
    assert(((jl_datatype_t*)jl_typetagof(m_))->layout->flags.arrayelem_isboxed);
    assert(i < m_->length);
    jl_gc_write_atomic(m, ((_Atomic(jl_value_t*)*)(m_->ptr))[i], (jl_value_t*)x, release);
    return (jl_value_t*)x;
}
#endif

// 读取 uint8 GenericMemory 元素
STATIC_INLINE uint8_t jl_memory_uint8_ref(void *m, size_t i) JL_NOTSAFEPOINT
{
    jl_genericmemory_t *m_ = (jl_genericmemory_t*)m;
    assert(jl_typetagis(m_, jl_memory_uint8_type));
    assert(i < m_->length);
    return ((uint8_t*)m_->ptr)[i];
}
// 设置 uint8 GenericMemory 元素
STATIC_INLINE void jl_memory_uint8_set(void *m, size_t i, uint8_t x) JL_NOTSAFEPOINT
{
    jl_genericmemory_t *m_ = (jl_genericmemory_t*)m;
    assert(jl_typetagis(m_, jl_memory_uint8_type));
    assert(i < m_->length);
    ((uint8_t*)m_->ptr)[i] = x;
}

// 获取数组的所有者
STATIC_INLINE jl_value_t *jl_array_owner(jl_array_t *a JL_PROPAGATES_ROOT) JL_NOTSAFEPOINT
{
    return jl_genericmemory_owner(a->ref.mem);
}

#ifdef __clang_gcanalyzer__
jl_value_t **jl_array_ptr_data(jl_array_t *a JL_PROPAGATES_ROOT) JL_NOTSAFEPOINT;
// 读取数组中的指针元素
STATIC_INLINE jl_value_t *jl_array_ptr_ref(void *a JL_PROPAGATES_ROOT, size_t i) JL_PROPAGATES_ROOT_INDEXED(0, 1) JL_NOTSAFEPOINT;
// 设置数组中的指针元素（带写屏障）
STATIC_INLINE jl_value_t *jl_array_ptr_set(
    void *a, size_t i,
    void *x JL_ROOTED_BY_ARG_INDEXED(0, 1)) JL_NOTSAFEPOINT;
#else
// 获取数组的指针数据数组
#define jl_array_ptr_data(a) (jl_array_data(a, jl_value_t*))
STATIC_INLINE jl_value_t *jl_array_ptr_ref(void *a JL_PROPAGATES_ROOT, size_t i) JL_PROPAGATES_ROOT_INDEXED(0, 1) JL_NOTSAFEPOINT
{
    assert(((jl_datatype_t*)jl_typetagof(((jl_array_t*)a)->ref.mem))->layout->flags.arrayelem_isboxed);
    assert(i < jl_array_len(a));
    return jl_atomic_load_relaxed(jl_array_data(a, _Atomic(jl_value_t*)) + i);
}
STATIC_INLINE jl_value_t *jl_array_ptr_set(
    void *a, size_t i,
    void *x JL_ROOTED_BY_ARG_INDEXED(0, 1)) JL_NOTSAFEPOINT
{
    assert(((jl_datatype_t*)jl_typetagof(((jl_array_t*)a)->ref.mem))->layout->flags.arrayelem_isboxed);
    assert(i < jl_array_len(a));
    jl_gc_write_atomic(jl_array_owner((jl_array_t*)a), jl_array_data(a, _Atomic(jl_value_t*))[i], (jl_value_t*)x, release);
    return (jl_value_t*)x;
}
#endif

// 读取 uint8 数组元素
STATIC_INLINE uint8_t jl_array_uint8_ref(void *a, size_t i) JL_NOTSAFEPOINT
{
    assert(jl_typetagis(a, jl_array_uint8_type));
    assert(i < jl_array_len(a));
    return jl_array_data(a, uint8_t)[i];
}
// 设置 uint8 数组元素
STATIC_INLINE void jl_array_uint8_set(void *a, size_t i, uint8_t x) JL_NOTSAFEPOINT
{
    assert(jl_typetagis(a, jl_array_uint8_type));
    assert(i < jl_array_len(a));
    jl_array_data(a, uint8_t)[i] = x;
}
// 设置 uint32 数组元素（也接受 int32 类型）
STATIC_INLINE void jl_array_uint32_set(void *a, size_t i, uint32_t x) JL_NOTSAFEPOINT
{
    assert(i < jl_array_len(a));
    assert(jl_typetagis(a, jl_array_uint32_type) || jl_typetagis(a, jl_array_int32_type));
    jl_array_data(a, uint32_t)[i] = x;
}

// 表达式参数访问宏
#define jl_exprarg(e,n) jl_array_ptr_ref(((jl_expr_t*)(e))->args, n)
#define jl_exprargset(e, n, v) jl_array_ptr_set(((jl_expr_t*)(e))->args, n, v)
#define jl_expr_nargs(e) jl_array_nrows(((jl_expr_t*)(e))->args)

// 结构体字段访问宏
#define jl_fieldref(s,i) jl_get_nth_field(((jl_value_t*)(s)),i)
#define jl_fieldref_noalloc(s,i) jl_get_nth_field_noalloc(((jl_value_t*)(s)),i)
#define jl_nfields(v)    jl_datatype_nfields(jl_typeof(v))

// 以下 AST 内部数据访问宏不使用 jl_fieldref 以避免分配
// Not using jl_fieldref to avoid allocations
#define jl_linenode_line(x) (((intptr_t*)(x))[0])
#define jl_linenode_file(x) (((jl_value_t**)(x))[1])
#define jl_slot_number(x) (((intptr_t*)(x))[0])
#define jl_typedslot_get_type(x) (((jl_value_t**)(x))[1])
#define jl_gotonode_label(x) (((intptr_t*)(x))[0])
#define jl_gotoifnot_cond(x) (((jl_value_t**)(x))[0])
#define jl_gotoifnot_label(x) (((intptr_t*)(x))[1])
#define jl_enternode_catch_dest(x) (((intptr_t*)(x))[0])
#define jl_enternode_scope(x) (((jl_value_t**)(x))[1])
#define jl_globalref_mod(s) (*(jl_module_t**)(s))
#define jl_globalref_name(s) (((jl_sym_t**)(s))[1])
#define jl_quotenode_value(x) (((jl_value_t**)x)[0])
#define jl_returnnode_value(x) (((jl_value_t**)x)[0])

// 获取数据类型的数据指针
// get a pointer to the data in a datatype
#define jl_data_ptr(v)  ((jl_value_t**)v)

// 字符串数据访问宏
#define jl_string_data(s) ((char*)s + sizeof(void*))
#define jl_string_len(s)  (*(size_t*)s)

// 获取通用函数名称
#define jl_gf_name(f) (((jl_datatype_t*)jl_typeof(f))->name->singletonname)

// struct type info
// 计算字段类型
JL_DLLEXPORT jl_svec_t *jl_compute_fieldtypes(jl_datatype_t *st JL_PROPAGATES_ROOT, void *stack, int cacheable);
// 获取字段类型（可能触发惰性计算）
#define jl_get_fieldtypes(st) ((st)->types ? (st)->types : jl_compute_fieldtypes((st), NULL, 0))
// 获取字段名称数组
STATIC_INLINE jl_svec_t *jl_field_names(jl_datatype_t *st) JL_NOTSAFEPOINT
{
    return st->name->names;
}
STATIC_INLINE jl_value_t *jl_field_type(jl_datatype_t *st JL_PROPAGATES_ROOT, size_t i)
{
    return jl_svecref(jl_get_fieldtypes(st), i);
}
// 获取具体化字段类型（假设 st->types 已计算）
STATIC_INLINE jl_value_t *jl_field_type_concrete(jl_datatype_t *st JL_PROPAGATES_ROOT, size_t i) JL_NOTSAFEPOINT
{
    assert(st->types);
    return jl_svecref(st->types, i);
}

// 检查布局是否为不透明类型（无字段但有指针）
STATIC_INLINE int jl_is_layout_opaque(const jl_datatype_layout_t *l) JL_NOTSAFEPOINT
{
    return l->nfields == 0 && l->npointers > 0;
}

// 解包 UnionAll 类型
JL_DLLEXPORT jl_value_t *jl_unwrap_unionall(jl_value_t *v JL_PROPAGATES_ROOT) JL_NOTSAFEPOINT;

#define jl_inlinedatatype_layout(t) (((jl_datatype_t*)t)->layout)
// 获取数据类型布局（处理不透明布局的退化情况）
STATIC_INLINE const jl_datatype_layout_t *jl_datatype_layout(jl_datatype_t *t) JL_NOTSAFEPOINT
{
    if (t->layout == NULL || jl_is_layout_opaque(t->layout)) // e.g. GenericMemory
        t = (jl_datatype_t*)jl_unwrap_unionall(t->name->wrapper);
    return t->layout;
}
// DataType 大小、对齐、位数和字段数
#define jl_datatype_size(t)    (jl_datatype_layout((jl_datatype_t*)(t))->size)
#define jl_datatype_align(t)   (jl_datatype_layout((jl_datatype_t*)(t))->alignment)
#define jl_datatype_nbits(t)   ((jl_datatype_layout((jl_datatype_t*)(t))->size)*8)
#define jl_datatype_nfields(t) (jl_datatype_layout((jl_datatype_t*)(t))->nfields)

// 获取符号名称字符串
JL_DLLEXPORT void *jl_symbol_name(jl_sym_t *s);
// 内联版本的符号名称获取（带强类型检查）
// inline version with strong type check to detect typos in a `->name` chain
STATIC_INLINE char *jl_symbol_name_(jl_sym_t *s) JL_NOTSAFEPOINT
{
    return (char*)s + LLT_ALIGN(sizeof(jl_sym_t), sizeof(void*));
}
#define jl_symbol_name(s) jl_symbol_name_(s)

// 获取模块调试名称
STATIC_INLINE const char *jl_module_debug_name(jl_module_t *mod) JL_NOTSAFEPOINT
{
    if (!mod)
        return "<null>";
    return jl_symbol_name(mod->name);
}

// 获取字段描述符大小
static inline uint32_t jl_fielddesc_size(int8_t fielddesc_type) JL_NOTSAFEPOINT
{
    switch ((jl_fielddesc_type_t)fielddesc_type) {
    case JL_FIELDDESC_8:
        return sizeof(jl_fielddesc8_t);
    case JL_FIELDDESC_16:
        return sizeof(jl_fielddesc16_t);
    case JL_FIELDDESC_32:
        return sizeof(jl_fielddesc32_t);
    case JL_FIELDDESC_FOREIGN:
        break;
    }
    assert(0 && "foreign field descriptors do not have inline layout entries");
    return 0;
}

// 获取字段描述符指针大小
static inline uint32_t jl_fielddesc_ptr_size(int8_t fielddesc_type) JL_NOTSAFEPOINT
{
    switch ((jl_fielddesc_type_t)fielddesc_type) {
    case JL_FIELDDESC_8:
        return sizeof(uint8_t);
    case JL_FIELDDESC_16:
        return sizeof(uint16_t);
    case JL_FIELDDESC_32:
        return sizeof(uint32_t);
    case JL_FIELDDESC_FOREIGN:
        break;
    }
    assert(0 && "foreign field descriptors do not have inline pointer tables");
    return 0;
}

#define jl_dt_layout_fields(d) ((const char*)(d) + sizeof(jl_datatype_layout_t))
static inline const char *jl_dt_layout_ptrs(const jl_datatype_layout_t *l) JL_NOTSAFEPOINT
{
    assert(l->flags.fielddesc_type != JL_FIELDDESC_FOREIGN);
    return jl_dt_layout_fields(l) + jl_fielddesc_size(l->flags.fielddesc_type) * l->nfields;
}

// 字段访问器宏 —— 生成获取字段偏移/大小的代码
#define DEFINE_FIELD_ACCESSORS(f)                                             \
    static inline uint32_t jl_field_##f(jl_datatype_t *st,                    \
                                        int i) JL_NOTSAFEPOINT                \
    {                                                                         \
        const jl_datatype_layout_t *ly = jl_datatype_layout(st);              \
        assert(i >= 0 && (size_t)i < ly->nfields);                            \
        if (ly->flags.fielddesc_type == JL_FIELDDESC_8) {                     \
            return ((const jl_fielddesc8_t*)jl_dt_layout_fields(ly))[i].f;    \
        }                                                                     \
        else if (ly->flags.fielddesc_type == JL_FIELDDESC_16) {               \
            return ((const jl_fielddesc16_t*)jl_dt_layout_fields(ly))[i].f;   \
        }                                                                     \
        else {                                                                \
            assert(ly->flags.fielddesc_type == JL_FIELDDESC_32);              \
            return ((const jl_fielddesc32_t*)jl_dt_layout_fields(ly))[i].f;   \
        }                                                                     \
    }                                                                         \

// 生成字段偏移和大小访问器
DEFINE_FIELD_ACCESSORS(offset)
DEFINE_FIELD_ACCESSORS(size)
#undef DEFINE_FIELD_ACCESSORS

// 判断字段是否为指针
static inline int jl_field_isptr(jl_datatype_t *st, int i) JL_NOTSAFEPOINT
{
    const jl_datatype_layout_t *ly = jl_datatype_layout(st);
    assert(i >= 0 && (size_t)i < ly->nfields);
    assert(ly->flags.fielddesc_type != JL_FIELDDESC_FOREIGN);
    return ((const jl_fielddesc8_t*)(jl_dt_layout_fields(ly) + jl_fielddesc_size(ly->flags.fielddesc_type) * i))->isptr;
}

// 获取指针偏移
static inline uint32_t jl_ptr_offset(jl_datatype_t *st, int i) JL_NOTSAFEPOINT
{
    const jl_datatype_layout_t *ly = st->layout; // NOT jl_datatype_layout(st)
    assert(i >= 0 && (size_t)i < ly->npointers);
    const void *ptrs = jl_dt_layout_ptrs(ly);
    if (ly->flags.fielddesc_type == JL_FIELDDESC_8) {
        return ((const uint8_t*)ptrs)[i];
    }
    else if (ly->flags.fielddesc_type == JL_FIELDDESC_16) {
        return ((const uint16_t*)ptrs)[i];
    }
    else {
        assert(ly->flags.fielddesc_type == JL_FIELDDESC_32);
        return ((const uint32_t*)ptrs)[i];
    }
}

// 判断字段是否为原子字段
static inline int jl_field_isatomic(jl_datatype_t *st, int i) JL_NOTSAFEPOINT
{
    const uint32_t *atomicfields = st->name->atomicfields;
    if (atomicfields != NULL) {
        if (atomicfields[i / 32] & (1 << (i % 32)))
            return 1;
    }
    return 0;
}

// 判断字段是否为 const 字段
static inline int jl_field_isconst(jl_datatype_t *st, int i) JL_NOTSAFEPOINT
{
    jl_typename_t *tn = st->name;
    if (!tn->mutabl)
        return 1;
    const uint32_t *constfields = tn->constfields;
    if (constfields != NULL) {
        if (constfields[i / 32] & (1 << (i % 32)))
            return 1;
    }
    return 0;
}


/* ======= 基本类型谓词 ======= */
// 以下是各种 Julia 类型检查的宏定义
// basic predicates -----------------------------------------------------------
#define jl_is_nothing(v)     (((jl_value_t*)(v)) == ((jl_value_t*)jl_nothing))
#define jl_is_tuple(v)       (((jl_datatype_t*)jl_typeof(v))->name == jl_tuple_typename)
#define jl_is_namedtuple(v)  (((jl_datatype_t*)jl_typeof(v))->name == jl_namedtuple_typename)
#define jl_is_svec(v)        jl_typetagis(v,jl_simplevector_tag<<4)
#define jl_is_simplevector(v) jl_is_svec(v)
#define jl_is_datatype(v)    jl_typetagis(v,jl_datatype_tag<<4)
#define jl_is_mutable(t)     (((jl_datatype_t*)t)->name->mutabl)
#define jl_is_mutable_datatype(t) (jl_is_datatype(t) && (((jl_datatype_t*)t)->name->mutabl))
#define jl_is_immutable(t)   (!((jl_datatype_t*)t)->name->mutabl)
#define jl_may_be_immutable_datatype(t) (jl_is_datatype(t) && (!((jl_datatype_t*)t)->name->mutabl))
#define jl_is_uniontype(v)   jl_typetagis(v,jl_uniontype_tag<<4)
#define jl_is_intersecttype(v) jl_typetagis(v,jl_intersect_type)
#define jl_is_typeeq(v)      jl_typetagis(v,jl_typeeq_tag<<4)
#define jl_is_typevar(v)     jl_typetagis(v,jl_tvar_tag<<4)
#define jl_is_unionall(v)    jl_typetagis(v,jl_unionall_tag<<4)
#define jl_is_vararg(v)      jl_typetagis(v,jl_vararg_tag<<4)
#define jl_is_typename(v)    jl_typetagis(v,jl_typename_type)
#define jl_is_int8(v)        jl_typetagis(v,jl_int8_tag<<4)
#define jl_is_int16(v)       jl_typetagis(v,jl_int16_tag<<4)
#define jl_is_int32(v)       jl_typetagis(v,jl_int32_tag<<4)
#define jl_is_int64(v)       jl_typetagis(v,jl_int64_tag<<4)
#define jl_is_uint8(v)       jl_typetagis(v,jl_uint8_tag<<4)
#define jl_is_uint16(v)      jl_typetagis(v,jl_uint16_tag<<4)
#define jl_is_uint32(v)      jl_typetagis(v,jl_uint32_tag<<4)
#define jl_is_uint64(v)      jl_typetagis(v,jl_uint64_tag<<4)
#define jl_is_bool(v)        jl_typetagis(v,jl_bool_tag<<4)
#define jl_is_symbol(v)      jl_typetagis(v,jl_symbol_tag<<4)
#define jl_is_ssavalue(v)    jl_typetagis(v,jl_ssavalue_tag<<4)
#define jl_is_slotnumber(v)  jl_typetagis(v,jl_slotnumber_tag<<4)
#define jl_is_expr(v)        jl_typetagis(v,jl_expr_type)
#define jl_is_binding(v)     jl_typetagis(v,jl_binding_type)
#define jl_is_binding_partition(v) jl_typetagis(v,jl_binding_partition_type)
#define jl_is_globalref(v)   jl_typetagis(v,jl_globalref_tag<<4)
#define jl_is_gotonode(v)    jl_typetagis(v,jl_gotonode_tag<<4)
#define jl_is_gotoifnot(v)   jl_typetagis(v,jl_gotoifnot_tag<<4)
#define jl_is_returnnode(v)  jl_typetagis(v,jl_returnnode_tag<<4)
#define jl_is_enternode(v)   jl_typetagis(v,jl_enternode_tag<<4)
#define jl_is_argument(v)    jl_typetagis(v,jl_argument_tag<<4)
#define jl_is_pinode(v)      jl_typetagis(v,jl_pinode_tag<<4)
#define jl_is_phinode(v)     jl_typetagis(v,jl_phinode_tag<<4)
#define jl_is_phicnode(v)    jl_typetagis(v,jl_phicnode_tag<<4)
#define jl_is_upsilonnode(v) jl_typetagis(v,jl_upsilonnode_tag<<4)
#define jl_is_quotenode(v)   jl_typetagis(v,jl_quotenode_tag<<4)
#define jl_is_newvarnode(v)  jl_typetagis(v,jl_newvarnode_type)
#define jl_is_linenode(v)    jl_typetagis(v,jl_linenumbernode_type)
#define jl_is_linenumbernode(v) jl_typetagis(v,jl_linenumbernode_type)
#define jl_is_method_instance(v) jl_typetagis(v,jl_method_instance_type)
#define jl_is_code_instance(v) jl_typetagis(v,jl_code_instance_type)
#define jl_is_code_info(v)   jl_typetagis(v,jl_code_info_type)
#define jl_is_method(v)      jl_typetagis(v,jl_method_type)
#define jl_is_module(v)      jl_typetagis(v,jl_module_tag<<4)
#define jl_is_mtable(v)      jl_typetagis(v,jl_methtable_type)
#define jl_is_mcache(v)      jl_typetagis(v,jl_methcache_type)
#define jl_is_task(v)        jl_typetagis(v,jl_task_tag<<4)
#define jl_is_string(v)      jl_typetagis(v,jl_string_tag<<4)
#define jl_is_cpointer(v)    jl_is_cpointer_type(jl_typeof(v))
#define jl_is_pointer(v)     jl_is_cpointer_type(jl_typeof(v))
#define jl_is_uint8pointer(v)jl_typetagis(v,jl_uint8pointer_type)
#define jl_is_llvmpointer(v) (((jl_datatype_t*)jl_typeof(v))->name == jl_llvmpointer_typename)
#define jl_is_intrinsic(v)   jl_typetagis(v,jl_intrinsic_tag<<4)
#define jl_is_addrspacecore(v) jl_typetagis(v,jl_addrspacecore_tag<<4)
#define jl_is_abioverride(v) jl_typetagis(v,jl_abioverride_type)
// GenericMemory 类型属性检查
#define jl_genericmemory_isbitsunion(a) (((jl_datatype_t*)jl_typetagof(a))->layout->flags.arrayelem_isunion)
#define jl_genericmemory_isatomic(a) (((jl_datatype_t*)jl_typetagof(a))->layout->flags.arrayelem_isatomic)
#define jl_genericmemory_islocked(a) (((jl_datatype_t*)jl_typetagof(a))->layout->flags.arrayelem_islocked)
#define jl_is_array_any(v)    jl_typetagis(v,jl_array_any_type)
#define jl_is_debuginfo(v)    jl_typetagis(v,jl_debuginfo_type)

// 子类型检查
JL_DLLEXPORT int jl_subtype(jl_value_t *a, jl_value_t *b);

int is_leaf_bound(jl_value_t *v) JL_NOTSAFEPOINT;

// 判断是否为类型"种类"
STATIC_INLINE int jl_is_kind(jl_value_t *v) JL_NOTSAFEPOINT
{
    return (v==(jl_value_t*)jl_uniontype_type || v==(jl_value_t*)jl_datatype_type ||
            v==(jl_value_t*)jl_unionall_type || v==(jl_value_t*)jl_typeeq_type ||
            v==(jl_value_t*)jl_typeofbottom_type);
}

// 判断标签是否为种类标签
STATIC_INLINE int jl_is_kindtag(uintptr_t t) JL_NOTSAFEPOINT
{
    t >>= 4;
    return (t==(uintptr_t)jl_uniontype_tag || t==(uintptr_t)jl_datatype_tag ||
            t==(uintptr_t)jl_unionall_tag || t==(uintptr_t)jl_typeeq_tag ||
            t==(uintptr_t)jl_typeofbottom_tag);
}

// 判断是否为类型
STATIC_INLINE int jl_is_type(jl_value_t *v) JL_NOTSAFEPOINT
{
    return jl_is_kindtag(jl_typetagof(v));
}

// 判断是否为原始类型（primitive type）
STATIC_INLINE int jl_is_primitivetype(void *v) JL_NOTSAFEPOINT
{
    return (jl_is_datatype(v) && ((jl_datatype_t*)(v))->isprimitivetype);
}

// 判断是否为结构体类型
STATIC_INLINE int jl_is_structtype(void *v) JL_NOTSAFEPOINT
{
    return (jl_is_datatype(v) &&
            !((jl_datatype_t*)(v))->name->abstract &&
            !((jl_datatype_t*)(v))->isprimitivetype);
}

// 判断是否为 bits 类型（对应 Julia 中的 isbitstype）
STATIC_INLINE int jl_isbits(void *t) JL_NOTSAFEPOINT // corresponding to isbitstype() in julia
{
    return jl_is_datatype(t) && ((jl_datatype_t*)t)->isbitstype;
}

// 判断是否为单例数据类型
STATIC_INLINE int jl_is_datatype_singleton(jl_datatype_t *d) JL_NOTSAFEPOINT
{
    return d->instance != NULL && d->layout->size == 0 && d->layout->npointers == 0;
}

// 判断是否为抽象类型
STATIC_INLINE int jl_is_abstracttype(void *v) JL_NOTSAFEPOINT
{
    return jl_is_datatype(v) && ((jl_datatype_t*)(v))->name->abstract;
}

// 判断是否为数组类型
// 判断是否为数组对象
STATIC_INLINE int jl_is_array_type(void *t) JL_NOTSAFEPOINT
{
    return jl_is_datatype(t) &&
           ((jl_datatype_t*)(t))->name == jl_array_typename;
}

STATIC_INLINE int jl_is_array(void *v) JL_NOTSAFEPOINT
{
    jl_value_t *t = jl_typeof(v);
    return jl_is_array_type(t);
}

// 判断是否为 GenericMemory 类型
// 判断是否为 GenericMemory 对象
STATIC_INLINE int jl_is_genericmemory_type(void *t) JL_NOTSAFEPOINT
{
    return (jl_is_datatype(t) &&
            ((jl_datatype_t*)(t))->name == jl_genericmemory_typename);
}

STATIC_INLINE int jl_is_genericmemory(void *v) JL_NOTSAFEPOINT
{
    jl_value_t *t = jl_typeof(v);
    return jl_is_genericmemory_type(t);
}

// 判断是否为 GenericMemoryRef 类型
// 判断是否为 GenericMemoryRef 对象
STATIC_INLINE int jl_is_genericmemoryref_type(void *t) JL_NOTSAFEPOINT
{
    return (jl_is_datatype(t) &&
            ((jl_datatype_t*)(t))->name == jl_genericmemoryref_typename);
}

STATIC_INLINE int jl_is_genericmemoryref(void *v) JL_NOTSAFEPOINT
{
    jl_value_t *t = jl_typeof(v);
    return jl_is_genericmemoryref_type(t);
}

// 判断是否为地址空间类型
// 判断是否为地址空间对象
STATIC_INLINE int jl_is_addrspace_type(void *t) JL_NOTSAFEPOINT
{
    return (jl_is_datatype(t) &&
            ((jl_datatype_t*)(t))->name == jl_addrspace_typename);
}

STATIC_INLINE int jl_is_addrspace(void *v) JL_NOTSAFEPOINT
{
    jl_value_t *t = jl_typeof(v);
    return jl_is_addrspace_type(t);
}


// 判断是否为不透明闭包类型
// 判断是否为不透明闭包对象
STATIC_INLINE int jl_is_opaque_closure_type(void *t) JL_NOTSAFEPOINT
{
    return (jl_is_datatype(t) &&
            ((jl_datatype_t*)(t))->name == jl_opaque_closure_typename);
}

STATIC_INLINE int jl_is_opaque_closure(void *v) JL_NOTSAFEPOINT
{
    jl_value_t *t = jl_typeof(v);
    return jl_is_opaque_closure_type(t);
}

// 判断是否为 C 指针类型
STATIC_INLINE int jl_is_cpointer_type(jl_value_t *t) JL_NOTSAFEPOINT
{
    return (jl_is_datatype(t) &&
            ((jl_datatype_t*)(t))->name == ((jl_datatype_t*)jl_pointer_type->body)->name);
}

// 判断是否为 LLVM 指针类型
STATIC_INLINE int jl_is_llvmpointer_type(jl_value_t *t) JL_NOTSAFEPOINT
{
    return (jl_is_datatype(t) &&
            ((jl_datatype_t*)(t))->name == jl_llvmpointer_typename);
}

// 判断是否为抽象引用类型
STATIC_INLINE int jl_is_abstract_ref_type(jl_value_t *t) JL_NOTSAFEPOINT
{
    return (jl_is_datatype(t) &&
            ((jl_datatype_t*)(t))->name == ((jl_datatype_t*)jl_ref_type->body)->name);
}

// 判断是否为元组类型
STATIC_INLINE int jl_is_tuple_type(void *t) JL_NOTSAFEPOINT
{
    return (jl_is_datatype(t) &&
            ((jl_datatype_t*)(t))->name == jl_tuple_typename);
}

// 判断是否为命名元组类型
STATIC_INLINE int jl_is_namedtuple_type(void *t) JL_NOTSAFEPOINT
{
    return (jl_is_datatype(t) &&
            ((jl_datatype_t*)(t))->name == jl_namedtuple_typename);
}

// 判断是否为 VecElement 类型
STATIC_INLINE int jl_is_vecelement_type(jl_value_t* t) JL_NOTSAFEPOINT
{
    return (jl_is_datatype(t) &&
            ((jl_datatype_t*)(t))->name == jl_vecelement_typename);
}

// 获取 TypeEq 的 T 字段
STATIC_INLINE jl_value_t *jl_typeeq_T(jl_value_t *v JL_PROPAGATES_ROOT) JL_NOTSAFEPOINT
{
    assert(jl_is_typeeq(v));
    return ((jl_typeeq_t*)v)->T;
}

// 判断 GenericMemory 是否需要零初始化
STATIC_INLINE int jl_is_genericmemory_zeroinit(jl_genericmemory_t *m) JL_NOTSAFEPOINT
{
    return ((jl_datatype_t*)jl_typeof(m))->zeroinit;
}

/* ======= 对象相等性与哈希 ======= */
// object identity
// 值相等性判断（egal）
JL_DLLEXPORT int jl_egal(const jl_value_t *a JL_MAYBE_UNROOTED, const jl_value_t *b JL_MAYBE_UNROOTED) JL_NOTSAFEPOINT;
// bits 类型的相等性判断
JL_DLLEXPORT int jl_egal__bits(const jl_value_t *a JL_MAYBE_UNROOTED, const jl_value_t *b JL_MAYBE_UNROOTED, jl_datatype_t *dt) JL_NOTSAFEPOINT;
// 位标签级别的相等性判断
JL_DLLEXPORT int jl_egal__bitstag(const jl_value_t *a JL_MAYBE_UNROOTED, const jl_value_t *b JL_MAYBE_UNROOTED, uintptr_t dtag) JL_NOTSAFEPOINT;
// 解包后的相等性判断
JL_DLLEXPORT int jl_egal__unboxed(const jl_value_t *a JL_MAYBE_UNROOTED, const jl_value_t *b JL_MAYBE_UNROOTED, uintptr_t dtag) JL_NOTSAFEPOINT;
// 对象哈希
JL_DLLEXPORT uintptr_t jl_object_id(jl_value_t *v) JL_NOTSAFEPOINT;
// 类型哈希
JL_DLLEXPORT uintptr_t jl_type_hash(jl_value_t *v) JL_NOTSAFEPOINT;
// 类型缓存哈希
JL_DLLEXPORT uintptr_t jl_type_cache_hash(jl_value_t *v) JL_NOTSAFEPOINT;

// 解包后相等性判断的内部实现
// 快速相等性判断（先比较指针，再按标签分派）
STATIC_INLINE int jl_egal__unboxed_(const jl_value_t *a JL_MAYBE_UNROOTED, const jl_value_t *b JL_MAYBE_UNROOTED, uintptr_t dtag) JL_NOTSAFEPOINT
{
    if (dtag < jl_max_tags << 4) {
        if (dtag == jl_symbol_tag << 4 || dtag == jl_bool_tag << 4 || dtag == jl_nothing_tag << 4)
            return 0;
    }
    else if (((jl_datatype_t*)dtag)->name->mutabl)
        return 0;
    return jl_egal__bitstag(a, b, dtag);
}

STATIC_INLINE int jl_egal_(const jl_value_t *a JL_MAYBE_UNROOTED, const jl_value_t *b JL_MAYBE_UNROOTED) JL_NOTSAFEPOINT
{
    if (a == b)
        return 1;
    uintptr_t dtag = jl_typetagof(a);
    if (dtag != jl_typetagof(b))
        return 0;
    return jl_egal__unboxed_(a, b, dtag);
}
#define jl_egal(a, b) jl_egal_((a), (b))

/* ======= 类型谓词与基本操作 ======= */
// type predicates and basic operations
// 检查类型是否包含自由类型变量
JL_DLLEXPORT int jl_has_free_typevars(jl_value_t *v) JL_NOTSAFEPOINT;
// 检查类型变量是否在类型中出现
JL_DLLEXPORT int jl_has_typevar(jl_value_t *t, jl_tvar_t *v) JL_NOTSAFEPOINT;
// 检查类型变量是否来自 UnionAll
JL_DLLEXPORT int jl_has_typevar_from_unionall(jl_value_t *t, jl_unionall_t *ua);
// 获取子类型环境大小
// 子类型环境计算
JL_DLLEXPORT int jl_subtype_env_size(jl_value_t *t) JL_NOTSAFEPOINT;
JL_DLLEXPORT int jl_subtype_env(jl_value_t *x, jl_value_t *y, jl_value_t **env, int envsz);
// isa 检查
JL_DLLEXPORT int jl_isa(jl_value_t *a, jl_value_t *t);
// 类型相等性检查
JL_DLLEXPORT int jl_types_equal(jl_value_t *a, jl_value_t *b);
// 是否为非破损子类型
JL_DLLEXPORT int jl_is_not_broken_subtype(jl_value_t *a, jl_value_t *b);
// 类型并集
JL_DLLEXPORT jl_value_t *jl_type_union(jl_value_t **ts, size_t n);
// 类型交集
JL_DLLEXPORT jl_value_t *jl_type_intersection(jl_value_t *a, jl_value_t *b);
// 判断交集是否为空
JL_DLLEXPORT int jl_has_empty_intersection(jl_value_t *x, jl_value_t *y);
// 创建 UnionAll 类型
JL_DLLEXPORT jl_value_t *jl_type_unionall(jl_tvar_t *v, jl_value_t *body);
// 获取类型名称字符串
JL_DLLEXPORT const char *jl_typename_str(jl_value_t *v) JL_NOTSAFEPOINT;
// 获取 typeof 字符串
JL_DLLEXPORT const char *jl_typeof_str(jl_value_t *v) JL_NOTSAFEPOINT;
// 判断类型 a 是否比 b 更具体
JL_DLLEXPORT int jl_type_morespecific(jl_value_t *a, jl_value_t *b);
// 判断方法 ma 是否比 mb 更具体
JL_DLLEXPORT int jl_method_morespecific(jl_method_t *ma, jl_method_t *mb);

// 判断是否为分派元组类型
STATIC_INLINE int jl_is_dispatch_tupletype(jl_value_t *v) JL_NOTSAFEPOINT
{
    return jl_is_datatype(v) && ((jl_datatype_t*)v)->isdispatchtuple;
}

// 判断是否为具体类型
STATIC_INLINE int jl_is_concrete_type(jl_value_t *v) JL_NOTSAFEPOINT
{
    return jl_is_datatype(v) && ((jl_datatype_t*)v)->isconcretetype;
}

// 判断类型签名是否可编译
JL_DLLEXPORT int jl_isa_compileable_sig(jl_tupletype_t *type, jl_svec_t *sparams, jl_method_t *definition);

/* ======= 类型构造器 ======= */
// type constructors
// 创建新的类型名称
JL_DLLEXPORT jl_typename_t *jl_new_typename_in(jl_sym_t *name, jl_module_t *inmodule, int abstract, int mutabl);
// 创建新的类型变量
JL_DLLEXPORT jl_tvar_t *jl_new_typevar(jl_sym_t *name, jl_value_t *lb, jl_value_t *ub);
// 实例化 UnionAll
JL_DLLEXPORT jl_value_t *jl_instantiate_unionall(jl_unionall_t *u, jl_value_t *p);
// 应用类型构造器（带参数列表）
JL_DLLEXPORT jl_value_t *jl_apply_type(jl_value_t *tc, jl_value_t **params, size_t n);
// 应用类型构造器（1 个参数）
JL_DLLEXPORT jl_value_t *jl_apply_type1(jl_value_t *tc, jl_value_t *p1);
// 应用类型构造器（2 个参数）
JL_DLLEXPORT jl_value_t *jl_apply_type2(jl_value_t *tc, jl_value_t *p1, jl_value_t *p2);
// 应用类型构造器（3 个参数）
JL_DLLEXPORT jl_value_t *jl_apply_type3(jl_value_t *tc, jl_value_t *p1, jl_value_t *p2, jl_value_t *p3);
// 应用修改类型
JL_DLLEXPORT jl_datatype_t *jl_apply_modify_type(jl_value_t *dt);
// 应用比较交换类型
JL_DLLEXPORT jl_datatype_t *jl_apply_cmpswap_type(jl_value_t *dt);
// 应用元组类型
JL_DLLEXPORT jl_value_t *jl_apply_tuple_type(jl_svec_t *params, int check); // if uncertain, set check=1
// 应用元组类型（可变参数版本）
JL_DLLEXPORT jl_value_t *jl_apply_tuple_type_v(jl_value_t **p, size_t np);
// 创建新的 DataType
JL_DLLEXPORT jl_datatype_t *jl_new_datatype(jl_sym_t *name,
                                            jl_module_t *module,
                                            jl_datatype_t *super,
                                            jl_svec_t *parameters,
                                            jl_svec_t *fnames,
                                            jl_svec_t *ftypes,
                                            jl_svec_t *fattrs,
                                            int abstract, int mutabl,
                                            int ninitialized);
// 创建新的原始类型
JL_DLLEXPORT jl_datatype_t *jl_new_primitivetype(jl_value_t *name,
                                                 jl_module_t *module,
                                                 jl_datatype_t *super,
                                                 jl_svec_t *parameters, size_t nbits);

/* ======= 构造器 ======= */
// constructors
// 从 bits 数据创建新的值
JL_DLLEXPORT jl_value_t *jl_new_bits(jl_value_t *bt, const void *src);
// 原子地创建 bits 值
JL_DLLEXPORT jl_value_t *jl_atomic_new_bits(jl_value_t *dt, const char *src);
// 原子地存储 bits 值
JL_DLLEXPORT void jl_atomic_store_bits(char *dst, const jl_value_t *src, int nb) JL_NOTSAFEPOINT;
// 原子地交换 bits 值
JL_DLLEXPORT jl_value_t *jl_atomic_swap_bits(jl_value_t *dt, char *dst, const jl_value_t *src, int nb);
// 原子地比较并交换 bits 值（布尔版本）
JL_DLLEXPORT int jl_atomic_bool_cmpswap_bits(char *dst, const jl_value_t *expected, const jl_value_t *src, int nb) JL_NOTSAFEPOINT;
// 原子地比较并交换 bits 值
JL_DLLEXPORT int jl_atomic_cmpswap_bits(jl_datatype_t *dt, jl_value_t *y, char *dst, const jl_value_t *expected, const jl_value_t *src, int nb) JL_NOTSAFEPOINT;
// 原子地一次性存储 bits 值
JL_DLLEXPORT int jl_atomic_storeonce_bits(jl_datatype_t *dt, char *dst, const jl_value_t *src, int nb) JL_NOTSAFEPOINT;
// 创建新的结构体实例（变参）
JL_DLLEXPORT jl_value_t *jl_new_struct(jl_datatype_t *type, ...) JL_ROOTED_VARARGS;
// 创建新的结构体实例（参数数组）
JL_DLLEXPORT jl_value_t *jl_new_structv(jl_datatype_t *type, jl_value_t **args, uint32_t na);
// 从元组创建新的结构体实例
JL_DLLEXPORT jl_value_t *jl_new_structt(jl_datatype_t *type, jl_value_t *tup);
// 创建未初始化的结构体实例
JL_DLLEXPORT jl_value_t *jl_new_struct_uninit(jl_datatype_t *type);
// 创建未初始化的方法实例
JL_DLLEXPORT jl_method_instance_t *jl_new_method_instance_uninit(void);
// 创建 SimpleVector
JL_DLLEXPORT jl_svec_t *jl_svec(size_t n, ...) JL_MAYBE_UNROOTED JL_ROOTED_VARARGS;
JL_DLLEXPORT jl_svec_t *jl_svec1(
    void *a JL_ROOTED_BY_RETURN);
JL_DLLEXPORT jl_svec_t *jl_svec2(
    void *a JL_ROOTED_BY_RETURN,
    void *b JL_ROOTED_BY_RETURN);
JL_DLLEXPORT jl_svec_t *jl_svec3(
    void *a JL_ROOTED_BY_RETURN,
    void *b JL_ROOTED_BY_RETURN,
    void *c JL_ROOTED_BY_RETURN);
// 分配 SimpleVector（零初始化）
JL_DLLEXPORT jl_svec_t *jl_alloc_svec(size_t n);
// 分配 SimpleVector（未初始化）
JL_DLLEXPORT jl_svec_t *jl_alloc_svec_uninit(size_t n);
// 复制 SimpleVector
JL_DLLEXPORT jl_svec_t *jl_svec_copy(jl_svec_t *a);
// 填充 SimpleVector
JL_DLLEXPORT jl_svec_t *jl_svec_fill(size_t n, jl_value_t *x);
// 创建或查找符号
JL_DLLEXPORT jl_sym_t *jl_symbol(const char *str) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_sym_t *jl_symbol_lookup(const char *str) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_sym_t *jl_symbol_n(const char *str, size_t len) JL_NOTSAFEPOINT;
// 生成唯一符号
JL_DLLEXPORT jl_sym_t *jl_gensym(void);
JL_DLLEXPORT jl_sym_t *jl_tagged_gensym(const char *str, size_t len);
JL_DLLEXPORT jl_sym_t *jl_get_root_symbol(void);
// 获取绑定的值
JL_DLLEXPORT jl_value_t *jl_get_binding_value(jl_binding_t *b JL_PROPAGATES_ROOT);
JL_DLLEXPORT jl_value_t *jl_get_binding_value_in_world(jl_binding_t *b JL_PROPAGATES_ROOT, size_t world);
// 获取绑定的常量值（如果存在）
JL_DLLEXPORT jl_value_t *jl_get_latest_binding_value_if_const(jl_binding_t *b JL_PROPAGATES_ROOT);
JL_DLLEXPORT jl_value_t *jl_get_latest_binding_value_if_resolved_debug_only(jl_binding_t *b JL_PROPAGATES_ROOT) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_value_t *jl_get_latest_binding_value_if_resolved_and_const_debug_only(jl_binding_t *b JL_PROPAGATES_ROOT) JL_NOTSAFEPOINT;
// 声明 const 通用函数
JL_DLLEXPORT jl_value_t *jl_declare_const_gf(jl_module_t *mod, jl_sym_t *name);
// 定义方法
JL_DLLEXPORT jl_method_t *jl_method_def(jl_svec_t *argdata, jl_methtable_t *mt, jl_code_info_t *f, jl_module_t *module);
// 获取分阶段的代码信息
JL_DLLEXPORT jl_code_info_t *jl_code_for_staged(jl_method_instance_t *linfo JL_PROPAGATES_ROOT, size_t world, jl_code_instance_t **cache JL_OUT_ROOTED_BY_ARG(0));
// 复制代码信息
JL_DLLEXPORT jl_code_info_t *jl_copy_code_info(jl_code_info_t *src);
// 获取世界计数器
JL_DLLEXPORT size_t jl_get_world_counter(void) JL_NOTSAFEPOINT;
// 获取线程本地世界年龄
JL_DLLEXPORT size_t jl_get_tls_world_age(void) JL_NOTSAFEPOINT;
// 丢弃所有缓存
JL_DLLEXPORT void jl_drop_all_caches(void);
/* ======= 装箱/拆箱函数 ======= */
// 装箱函数 —— 在各种 Julia 值和 C 类型之间转换
JL_DLLEXPORT jl_value_t *jl_box_bool(int8_t x) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_value_t *jl_box_int8(int8_t x) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_value_t *jl_box_uint8(uint8_t x) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_value_t *jl_box_int16(int16_t x);
JL_DLLEXPORT jl_value_t *jl_box_uint16(uint16_t x);
JL_DLLEXPORT jl_value_t *jl_box_int32(int32_t x);
JL_DLLEXPORT jl_value_t *jl_box_uint32(uint32_t x);
JL_DLLEXPORT jl_value_t *jl_box_char(uint32_t x);
JL_DLLEXPORT jl_value_t *jl_box_int64(int64_t x);
JL_DLLEXPORT jl_value_t *jl_box_uint64(uint64_t x);
JL_DLLEXPORT jl_value_t *jl_box_float32(float x);
JL_DLLEXPORT jl_value_t *jl_box_float64(double x);
JL_DLLEXPORT jl_value_t *jl_box_voidpointer(void *x);
JL_DLLEXPORT jl_value_t *jl_box_uint8pointer(uint8_t *x);
JL_DLLEXPORT jl_value_t *jl_box_ssavalue(size_t x);
JL_DLLEXPORT jl_value_t *jl_box_slotnumber(size_t x);
// 拆箱函数
JL_DLLEXPORT int8_t jl_unbox_bool(jl_value_t *v) JL_NOTSAFEPOINT;
JL_DLLEXPORT int8_t jl_unbox_int8(jl_value_t *v) JL_NOTSAFEPOINT;
JL_DLLEXPORT uint8_t jl_unbox_uint8(jl_value_t *v) JL_NOTSAFEPOINT;
JL_DLLEXPORT int16_t jl_unbox_int16(jl_value_t *v) JL_NOTSAFEPOINT;
JL_DLLEXPORT uint16_t jl_unbox_uint16(jl_value_t *v) JL_NOTSAFEPOINT;
JL_DLLEXPORT int32_t jl_unbox_int32(jl_value_t *v) JL_NOTSAFEPOINT;
JL_DLLEXPORT uint32_t jl_unbox_uint32(jl_value_t *v) JL_NOTSAFEPOINT;
JL_DLLEXPORT int64_t jl_unbox_int64(jl_value_t *v) JL_NOTSAFEPOINT;
JL_DLLEXPORT uint64_t jl_unbox_uint64(jl_value_t *v) JL_NOTSAFEPOINT;
JL_DLLEXPORT float jl_unbox_float32(jl_value_t *v) JL_NOTSAFEPOINT;
JL_DLLEXPORT double jl_unbox_float64(jl_value_t *v) JL_NOTSAFEPOINT;
JL_DLLEXPORT void *jl_unbox_voidpointer(jl_value_t *v) JL_NOTSAFEPOINT;
JL_DLLEXPORT uint8_t *jl_unbox_uint8pointer(jl_value_t *v) JL_NOTSAFEPOINT;

// 获取对象大小
JL_DLLEXPORT int jl_get_size(jl_value_t *val, size_t *pnt);

#ifdef _P64
// 平台相关的 long 装箱/拆箱宏
#define jl_box_long(x)   jl_box_int64(x)
#define jl_box_ulong(x)  jl_box_uint64(x)
#define jl_unbox_long(x) jl_unbox_int64(x)
#define jl_unbox_ulong(x) jl_unbox_uint64(x)
#define jl_is_long(x)    jl_is_int64(x)
#define jl_is_ulong(x)   jl_is_uint64(x)
#define jl_long_type     jl_int64_type
#define jl_ulong_type    jl_uint64_type
#else
#define jl_box_long(x)   jl_box_int32(x)
#define jl_box_ulong(x)  jl_box_uint32(x)
#define jl_unbox_long(x) jl_unbox_int32(x)
#define jl_unbox_ulong(x) jl_unbox_uint32(x)
#define jl_is_long(x)    jl_is_int32(x)
#define jl_is_ulong(x)   jl_is_uint32(x)
#define jl_long_type     jl_int32_type
#define jl_ulong_type    jl_uint32_type
#endif

// 结构体字段操作函数
// structs
// 获取字段索引
JL_DLLEXPORT int         jl_field_index(jl_datatype_t *t, jl_sym_t *fld, int err);
// 获取第 i 个字段值
JL_DLLEXPORT jl_value_t *jl_get_nth_field(jl_value_t *v, size_t i);
// Like jl_get_nth_field above, but asserts if it needs to allocate
// 获取第 i 个字段值（不分配内存）
JL_DLLEXPORT jl_value_t *jl_get_nth_field_noalloc(jl_value_t *v JL_PROPAGATES_ROOT, size_t i) JL_NOTSAFEPOINT;
// 获取第 i 个字段值（带检查）
JL_DLLEXPORT jl_value_t *jl_get_nth_field_checked(jl_value_t *v, size_t i);
// 设置第 i 个字段值
JL_DLLEXPORT void        jl_set_nth_field(jl_value_t *v, size_t i, jl_value_t *rhs);
// 判断字段是否已定义
JL_DLLEXPORT int         jl_field_isdefined(jl_value_t *v, size_t i) JL_NOTSAFEPOINT;
// 判断字段是否已定义（带检查）
JL_DLLEXPORT int         jl_field_isdefined_checked(jl_value_t *v, size_t i);
// 通过名称获取字段值
JL_DLLEXPORT jl_value_t *jl_get_field(jl_value_t *o, const char *fld);
// 获取值的指针
JL_DLLEXPORT jl_value_t *jl_value_ptr(jl_value_t *a);
int jl_uniontype_size(jl_value_t *ty, size_t *sz);
JL_DLLEXPORT int jl_islayout_inline(jl_value_t *eltype, size_t *fsz, size_t *al);

// 数组操作函数
// arrays
// 从指针创建一维数组
// 从指针创建多维数组
JL_DLLEXPORT jl_array_t *jl_ptr_to_array_1d(jl_value_t *atype, void *data,
                                            size_t nel, int own_buffer);
JL_DLLEXPORT jl_array_t *jl_ptr_to_array(jl_value_t *atype, void *data,
                                         jl_value_t *dims, int own_buffer);

// 分配一维数组
JL_DLLEXPORT jl_array_t *jl_alloc_array_1d(jl_value_t *atype, size_t nr);
// 分配二维数组
JL_DLLEXPORT jl_array_t *jl_alloc_array_2d(jl_value_t *atype, size_t nr, size_t nc);
// 分配三维数组
JL_DLLEXPORT jl_array_t *jl_alloc_array_3d(jl_value_t *atype, size_t nr, size_t nc, size_t z);
// 分配 N 维数组
JL_DLLEXPORT jl_array_t *jl_alloc_array_nd(jl_value_t *atype, size_t *dims, size_t ndims);
// 从 C 字符串创建数组
JL_DLLEXPORT jl_array_t *jl_pchar_to_array(const char *str, size_t len);
// 从 C 字符串创建字符串
JL_DLLEXPORT jl_value_t *jl_pchar_to_string(const char *str, size_t len);
// 从 C 字符串创建 Julia 字符串
JL_DLLEXPORT jl_value_t *jl_cstr_to_string(const char *str);
// 分配字符串
JL_DLLEXPORT jl_value_t *jl_alloc_string(size_t len);
// 从数组创建字符串
JL_DLLEXPORT jl_value_t *jl_array_to_string(jl_array_t *a);
// 分配 Any 类型向量
JL_DLLEXPORT jl_array_t *jl_alloc_vec_any(size_t n);
// 数组尾部增长
JL_DLLEXPORT void jl_array_grow_end(jl_array_t *a, size_t inc);
// 数组尾部删除
JL_DLLEXPORT void jl_array_del_end(jl_array_t *a, size_t dec);
// 一维指针数组尾部添加元素
JL_DLLEXPORT void jl_array_ptr_1d_push(jl_array_t *a, jl_value_t *item);
// 一维指针数组尾部追加另一个数组
JL_DLLEXPORT void jl_array_ptr_1d_append(jl_array_t *a, jl_array_t *a2);
// 应用数组类型构造器
JL_DLLEXPORT jl_value_t *jl_apply_array_type(jl_value_t *type, size_t dim);
// property access
// 获取数组数据指针
JL_DLLEXPORT void *jl_array_ptr(jl_array_t *a);
// 获取数组元素类型
JL_DLLEXPORT void *jl_array_eltype(jl_value_t *a);
JL_DLLEXPORT int jl_array_rank(jl_value_t *a);

// GenericMemory 操作函数
// genericmemory
// 创建新的 GenericMemory
JL_DLLEXPORT jl_genericmemory_t *jl_new_genericmemory(jl_value_t *mtype, jl_value_t *dim);
// 从指针创建 GenericMemory
JL_DLLEXPORT jl_genericmemory_t *jl_ptr_to_genericmemory(jl_value_t *mtype, void *data,
                                           size_t nel, int own_buffer);
// 分配 GenericMemory
JL_DLLEXPORT jl_genericmemory_t *jl_alloc_genericmemory(jl_value_t *mtype, size_t nel);
// 从 C 字符串创建 GenericMemory
JL_DLLEXPORT jl_genericmemory_t *jl_pchar_to_memory(const char *str, size_t len);
// 分配 GenericMemory（不检查）
JL_DLLEXPORT jl_genericmemory_t *jl_alloc_genericmemory_unchecked(jl_ptls_t ptls, size_t nbytes, jl_datatype_t *mtype);
// 从 GenericMemory 创建字符串
JL_DLLEXPORT jl_value_t *jl_genericmemory_to_string(jl_genericmemory_t *m, size_t len);
// 分配 Any 类型 GenericMemory
JL_DLLEXPORT jl_genericmemory_t *jl_alloc_memory_any(size_t n);
// 创建 GenericMemoryRef（0 索引）
JL_DLLEXPORT jl_value_t *jl_genericmemoryref(jl_genericmemory_t *m, size_t i);  // 0-indexed

// 创建新的内存引用
JL_DLLEXPORT jl_genericmemoryref_t *jl_new_memoryref(jl_value_t *typ, jl_genericmemory_t *mem, void *data);
// 读取内存引用值
JL_DLLEXPORT jl_value_t *jl_memoryrefget(jl_genericmemoryref_t m JL_PROPAGATES_ROOT, int isatomic);
// 读取指针内存引用值
JL_DLLEXPORT jl_value_t *jl_ptrmemoryrefget(jl_genericmemoryref_t m JL_PROPAGATES_ROOT) JL_NOTSAFEPOINT;
// 检查内存引用是否已赋值
JL_DLLEXPORT jl_value_t *jl_memoryref_isassigned(jl_genericmemoryref_t m, int isatomic) JL_GLOBALLY_ROOTED;
// 偏移内存引用索引
JL_DLLEXPORT jl_genericmemoryref_t jl_memoryrefindex(jl_genericmemoryref_t m JL_PROPAGATES_ROOT, size_t idx) JL_NOTSAFEPOINT;
// 设置内存引用值
JL_DLLEXPORT void jl_memoryrefset(jl_genericmemoryref_t m, jl_value_t *v JL_ROOTED_BY_ARG(0) JL_MAYBE_UNROOTED, int isatomic);
// 取消设置内存引用值
JL_DLLEXPORT void jl_memoryrefunset(jl_genericmemoryref_t m, int isatomic);
// 原子交换内存引用值
JL_DLLEXPORT jl_value_t *jl_memoryrefswap(jl_genericmemoryref_t m, jl_value_t *v, int isatomic);
// 原子修改内存引用值
JL_DLLEXPORT jl_value_t *jl_memoryrefmodify(jl_genericmemoryref_t m, jl_value_t *op, jl_value_t *v, int isatomic);
// 原子替换内存引用值
JL_DLLEXPORT jl_value_t *jl_memoryrefreplace(jl_genericmemoryref_t m, jl_value_t *expected, jl_value_t *v, int isatomic);
// 原子一次性设置内存引用值
JL_DLLEXPORT jl_value_t *jl_memoryrefsetonce(jl_genericmemoryref_t m, jl_value_t *v, int isatomic);

// 字符串操作
// strings
// 获取字符串的 C 指针
JL_DLLEXPORT const char *jl_string_ptr(jl_value_t *s);

// 模块与全局变量操作
// modules and global variables
// 创建新模块
JL_DLLEXPORT jl_module_t *jl_new_module(jl_sym_t *name, jl_module_t *parent);
// 设置/获取模块 nospecialize 标志
JL_DLLEXPORT void jl_set_module_nospecialize(jl_module_t *self, int on);
// 设置/获取模块优化级别
JL_DLLEXPORT void jl_set_module_optlevel(jl_module_t *self, int lvl);
JL_DLLEXPORT int jl_get_module_optlevel(jl_module_t *m);
// 设置/获取模块编译设置
JL_DLLEXPORT void jl_set_module_compile(jl_module_t *self, int value);
JL_DLLEXPORT int jl_get_module_compile(jl_module_t *m);
// 设置/获取模块推断设置
JL_DLLEXPORT void jl_set_module_infer(jl_module_t *self, int value);
JL_DLLEXPORT int jl_get_module_infer(jl_module_t *m);
// 设置/获取模块最大方法数
JL_DLLEXPORT void jl_set_module_max_methods(jl_module_t *self, int value);
JL_DLLEXPORT int jl_get_module_max_methods(jl_module_t *m);
// 获取模块的 using backedge 列表
JL_DLLEXPORT jl_value_t *jl_get_module_usings_backedges(jl_module_t *m);
JL_DLLEXPORT jl_value_t *jl_get_module_scanned_methods(jl_module_t *m);
JL_DLLEXPORT jl_value_t *jl_get_module_binding_or_nothing(jl_module_t *m, jl_sym_t *s);

// 获取变量绑定（用于读取）
// get binding for reading
// 获取变量绑定
JL_DLLEXPORT jl_binding_t *jl_get_binding(jl_module_t *m JL_PROPAGATES_ROOT, jl_sym_t *var);
// 获取模块的 GlobalRef
JL_DLLEXPORT jl_value_t *jl_module_globalref(jl_module_t *m JL_PROPAGATES_ROOT, jl_sym_t *var);
// 获取绑定的类型限制
JL_DLLEXPORT jl_value_t *jl_get_binding_type(jl_module_t *m, jl_sym_t *var);
// 获取变量绑定（用于赋值）
// get binding for assignment
// 检查绑定当前是否可写
JL_DLLEXPORT void jl_check_binding_currently_writable(jl_binding_t *b, jl_module_t *m, jl_sym_t *s);
// 获取绑定（用于写入）
JL_DLLEXPORT jl_binding_t *jl_get_binding_wr(jl_module_t *m JL_PROPAGATES_ROOT, jl_sym_t *var);
// 获取已存在的强通用函数
JL_DLLEXPORT jl_value_t *jl_get_existing_strong_gf(jl_binding_t *b JL_PROPAGATES_ROOT, size_t new_world);
// 检查变量是否已绑定
JL_DLLEXPORT int jl_boundp(jl_module_t *m, jl_sym_t *var, int allow_import);
// 检查变量是否为 const
JL_DLLEXPORT int jl_is_const(jl_module_t *m, jl_sym_t *var);
// 检查 GlobalRef 是否为 const
JL_DLLEXPORT int jl_globalref_is_const(jl_globalref_t *gr);
// 获取全局变量
JL_DLLEXPORT jl_value_t *jl_get_global(jl_module_t *m JL_PROPAGATES_ROOT, jl_sym_t *var);
// 设置全局变量
JL_DLLEXPORT void jl_set_global(jl_module_t *m, jl_sym_t *var, jl_value_t *val JL_ROOTED_BY_ARG(0));
// 设置 const 全局变量
JL_DLLEXPORT void jl_set_const(jl_module_t *m, jl_sym_t *var, jl_value_t *val JL_ROOTED_BY_ARG(0));
void jl_set_initial_const(jl_module_t *m, jl_sym_t *var, jl_value_t *val JL_ROOTED_BY_ARG(0), int exported);
JL_DLLEXPORT void jl_checked_assignment(jl_binding_t *b, jl_module_t *mod, jl_sym_t *var, jl_value_t *rhs JL_MAYBE_UNROOTED);
JL_DLLEXPORT jl_value_t *jl_checked_swap(jl_binding_t *b, jl_module_t *mod, jl_sym_t *var, jl_value_t *rhs JL_MAYBE_UNROOTED);
JL_DLLEXPORT jl_value_t *jl_checked_replace(jl_binding_t *b, jl_module_t *mod, jl_sym_t *var, jl_value_t *expected, jl_value_t *rhs);
JL_DLLEXPORT jl_value_t *jl_checked_modify(jl_binding_t *b, jl_module_t *mod, jl_sym_t *var, jl_value_t *op, jl_value_t *rhs);
JL_DLLEXPORT jl_value_t *jl_checked_assignonce(jl_binding_t *b, jl_module_t *mod, jl_sym_t *var, jl_value_t *rhs JL_MAYBE_UNROOTED);
// 声明常量值绑定分区
JL_DLLEXPORT jl_binding_partition_t *jl_declare_constant_val(jl_binding_t *b, jl_module_t *mod, jl_sym_t *var, jl_value_t *val JL_ROOTED_BY_ARG(1) JL_MAYBE_UNROOTED);
JL_DLLEXPORT jl_binding_partition_t *jl_declare_constant_val2(jl_binding_t *b, jl_module_t *mod, jl_sym_t *var, jl_value_t *val JL_ROOTED_BY_ARG(1) JL_MAYBE_UNROOTED, enum jl_partition_kind);
// 模块导入绑定
JL_DLLEXPORT void jl_module_import(jl_task_t *ct, jl_module_t *to, jl_module_t *from, jl_sym_t *asname, jl_sym_t *s, int explici);
JL_DLLEXPORT void jl_import_module(jl_task_t *ct, jl_module_t *m, jl_module_t *import, jl_sym_t *asname);
// 模块使用另一模块
JL_DLLEXPORT void jl_module_using(jl_module_t *to, jl_module_t *from, size_t flags);
// 设置模块的 public 绑定
JL_DLLEXPORT void jl_module_public(jl_module_t *from, jl_value_t **symbols, size_t nsymbols, int exported);
JL_DLLEXPORT int jl_is_imported(jl_module_t *m, jl_sym_t *s);
JL_DLLEXPORT int jl_module_exports_p(jl_module_t *m, jl_sym_t *var);

// 相等性哈希表操作
// eq hash tables
// 向相等性哈希表插入键值对
JL_DLLEXPORT jl_genericmemory_t *jl_eqtable_put(jl_genericmemory_t *h, jl_value_t *key, jl_value_t *val JL_ROOTED_BY_ARG(0), int *inserted);
// 从相等性哈希表查询值
JL_DLLEXPORT jl_value_t *jl_eqtable_get(jl_genericmemory_t *h JL_PROPAGATES_ROOT, jl_value_t *key, jl_value_t *deflt) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_value_t *jl_eqtable_pop(jl_genericmemory_t *h, jl_value_t *key, jl_value_t *deflt, int *found);
jl_value_t *jl_eqtable_getkey(jl_genericmemory_t *h JL_PROPAGATES_ROOT, jl_value_t *key, jl_value_t *deflt) JL_NOTSAFEPOINT;

// 系统信息查询
// system information
// 获取/设置 errno
JL_DLLEXPORT int jl_errno(void) JL_NOTSAFEPOINT;
JL_DLLEXPORT void jl_set_errno(int e) JL_NOTSAFEPOINT;
JL_DLLEXPORT int32_t jl_stat(const char *path, char *statbuf) JL_NOTSAFEPOINT;
// 获取 CPU 线程数
JL_DLLEXPORT int jl_cpu_threads(void) JL_NOTSAFEPOINT;
// 获取有效线程数
JL_DLLEXPORT int jl_effective_threads(void) JL_NOTSAFEPOINT;
JL_DLLEXPORT const char *jl_precompile_jobserver_create(int ntokens) JL_NOTSAFEPOINT;
JL_DLLEXPORT int jl_precompile_jobserver_active(void) JL_NOTSAFEPOINT;
JL_DLLEXPORT void jl_precompile_jobserver_destroy(void) JL_NOTSAFEPOINT;
JL_DLLEXPORT int jl_precompile_jobserver_acquire(void) JL_NOTSAFEPOINT;
JL_DLLEXPORT void jl_precompile_jobserver_release(void) JL_NOTSAFEPOINT;
JL_DLLEXPORT long jl_getpagesize(void) JL_NOTSAFEPOINT;
JL_DLLEXPORT long jl_getallocationgranularity(void) JL_NOTSAFEPOINT;
JL_DLLEXPORT long jl_gethugepagesize(void) JL_NOTSAFEPOINT;
JL_DLLEXPORT int jl_is_debugbuild(void) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_sym_t *jl_get_UNAME(void) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_sym_t *jl_get_ARCH(void) JL_NOTSAFEPOINT;
JL_DLLIMPORT jl_value_t *jl_get_libllvm(void) JL_NOTSAFEPOINT;
extern int jl_n_markthreads;
extern int jl_n_sweepthreads;

// 线程池 ID 常量
#define JL_THREADPOOL_ID_INTERACTIVE 0
#define JL_THREADPOOL_ID_DEFAULT 1

// 抛出常见异常的函数
// throwing common exceptions
// 格式化创建异常值
JL_DLLEXPORT jl_value_t *jl_vexceptionf(jl_datatype_t *exception_type,
                                        const char *fmt, va_list args);
// 抛出错误异常
JL_DLLEXPORT void JL_NORETURN jl_error(const char *str);
JL_DLLEXPORT void JL_NORETURN jl_errorf(const char *fmt, ...);
// 格式化抛出异常
JL_DLLEXPORT void JL_NORETURN jl_exceptionf(jl_datatype_t *ty,
                                            const char *fmt, ...);
// 参数数量错误
JL_DLLEXPORT void JL_NORETURN jl_too_few_args(const char *fname, int min);
JL_DLLEXPORT void JL_NORETURN jl_too_many_args(const char *fname, int max);
// 类型错误
JL_DLLEXPORT void JL_NORETURN jl_type_error(const char *fname,
                                            jl_value_t *expected JL_MAYBE_UNROOTED,
                                            jl_value_t *got JL_MAYBE_UNROOTED);
JL_DLLEXPORT void JL_NORETURN jl_type_error_rt(const char *fname,
                                               const char *context,
                                               jl_value_t *ty JL_MAYBE_UNROOTED,
                                               jl_value_t *got JL_MAYBE_UNROOTED);
JL_DLLEXPORT void JL_NORETURN jl_type_error_global(const char *fname,
                                               jl_module_t *mod, jl_sym_t *sym,
                                               jl_value_t *ty JL_MAYBE_UNROOTED,
                                               jl_value_t *got JL_MAYBE_UNROOTED);
// 未定义变量错误
JL_DLLEXPORT void JL_NORETURN jl_undefined_var_error(jl_sym_t *var, jl_value_t *scope JL_MAYBE_UNROOTED);
JL_DLLEXPORT void JL_NORETURN jl_has_no_field_error(jl_datatype_t *t, jl_sym_t *var);
JL_DLLEXPORT void JL_NORETURN jl_argument_error(char *str);
JL_DLLEXPORT void JL_NORETURN jl_atomic_error(char *str);
// 越界错误
JL_DLLEXPORT void JL_NORETURN jl_bounds_error(jl_value_t *v JL_MAYBE_UNROOTED,
                                              jl_value_t *t JL_MAYBE_UNROOTED);
JL_DLLEXPORT void JL_NORETURN jl_bounds_error_v(jl_value_t *v JL_MAYBE_UNROOTED,
                                                jl_value_t **idxs, size_t nidxs);
JL_DLLEXPORT void JL_NORETURN jl_bounds_error_int(jl_value_t *v JL_MAYBE_UNROOTED,
                                                  size_t i);
JL_DLLEXPORT void JL_NORETURN jl_bounds_error_tuple_int(jl_value_t **v,
                                                        size_t nv, size_t i);
JL_DLLEXPORT void JL_NORETURN jl_bounds_error_unboxed_int(void *v, jl_value_t *vt, size_t i);
JL_DLLEXPORT void JL_NORETURN jl_bounds_error_ints(jl_value_t *v JL_MAYBE_UNROOTED,
                                                   size_t *idxs, size_t nidxs);

// 参数数量检查宏
#define JL_NARGS(fname, min, max)                               \
    if (nargs < min) jl_too_few_args(#fname, min);              \
    else if (nargs > max) jl_too_many_args(#fname, max);

#define JL_NARGSV(fname, min)                           \
    if (nargs < min) jl_too_few_args(#fname, min);

// 类型检查宏
#define JL_TYPECHK(fname, type, v)                                 \
    if (!jl_is_##type(v)) {                                        \
        jl_type_error(#fname, (jl_value_t*)jl_##type##_type, (v)); \
    }
#define JL_TYPECHKS(fname, type, v)                                     \
    if (!jl_is_##type(v)) {                                             \
        jl_type_error(fname, (jl_value_t*)jl_##type##_type, (v));       \
    }

// Julia 初始化函数
// initialization functions
typedef enum {
    JL_IMAGE_CWD = 0,
    JL_IMAGE_JULIA_HOME = 1,
    JL_IMAGE_IN_MEMORY = 2
} JL_IMAGE_SEARCH;

typedef enum {
    JL_IMAGE_KIND_NONE = 0,
    JL_IMAGE_KIND_JI,
    JL_IMAGE_KIND_SO,
} jl_image_kind_t;

// 加载但未解析的 .ji 或 .so 镜像文件
// A loaded, but unparsed .ji or .so image file
typedef struct {
    jl_image_kind_t kind;
    const void *pointers; // jl_image_pointers_t *
    const char *data;
    size_t size;
    uint64_t base;
    uint32_t checksum;
} jl_image_buf_t;

struct _jl_image_t;
typedef struct _jl_image_t jl_image_t;

// 获取 Julia 库目录
JL_DLLIMPORT const char *jl_get_libdir(void);
// 初始化 Julia 运行时
JL_DLLEXPORT void jl_init(void);
// 使用镜像文件初始化 Julia
JL_DLLEXPORT void jl_init_with_image_file(const char *julia_bindir,
                                          const char *image_path);
// 使用镜像句柄初始化 Julia
JL_DLLEXPORT void jl_init_with_image_handle(void *handle);
JL_DLLEXPORT const char *jl_get_default_sysimg_path(void);
// 检查 Julia 是否已初始化
JL_DLLEXPORT int jl_is_initialized(void);
// 注册退出钩子
JL_DLLEXPORT void jl_atexit_hook(int status);
JL_DLLEXPORT void jl_task_wait_empty(void);
JL_DLLEXPORT void jl_postoutput_hook(void);
// 退出 Julia
JL_DLLEXPORT void JL_NORETURN jl_exit(int status);
JL_DLLEXPORT void JL_NORETURN jl_raise(int signo);
JL_DLLEXPORT const char *jl_pathname_for_handle(void *handle) JL_NOTSAFEPOINT;
JL_DLLEXPORT const char *jl_pathname_for_symbol(void *symbol) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_gcframe_t **jl_adopt_thread(void);

// 反序列化验证头部
JL_DLLEXPORT int jl_deserialize_verify_header(ios_t *s);
// 预加载系统镜像
JL_DLLEXPORT jl_image_buf_t jl_preload_sysimg(const char *fname);
JL_DLLEXPORT jl_image_buf_t jl_set_sysimg_so(void *handle);
// 创建系统镜像
JL_DLLEXPORT void jl_create_system_image(void **, jl_array_t *worklist, bool_t emit_split, ios_t **s, ios_t **z, jl_array_t **udeps JL_REQUIRE_ROOTED_SLOT, int64_t *srctextpos, jl_array_t *module_init_order);
// 恢复系统镜像
JL_DLLEXPORT void jl_restore_system_image(jl_image_t *image, jl_image_buf_t buf);
JL_DLLEXPORT jl_value_t *jl_restore_incremental(const char *fname, jl_array_t *depmods, int complete, const char *pkgimage);
JL_DLLEXPORT jl_value_t *jl_object_top_module(jl_value_t* v) JL_NOTSAFEPOINT;

JL_DLLEXPORT void jl_set_newly_inferred(jl_value_t *newly_inferred);
JL_DLLEXPORT void jl_finalize_precompile_inferred(int8_t cleanup_keep_ir);
JL_DLLEXPORT jl_array_t* jl_compute_new_ext(void);
JL_DLLEXPORT void jl_push_newly_inferred(jl_value_t *ci);
JL_DLLEXPORT void jl_set_inference_entrance_backtraces(jl_value_t *inference_entrance_backtraces);
JL_DLLEXPORT void jl_push_inference_entrance_backtraces(jl_value_t *ci);
JL_DLLEXPORT void jl_write_compiler_output(void);

// 解析与降阶函数
// parsing
// 完整解析代码字符串
JL_DLLEXPORT jl_value_t *jl_parse_all(const char *text, size_t text_len,
                                      const char *filename, size_t filename_len, size_t lineno);
JL_DLLEXPORT jl_value_t *jl_parse_string(const char *text, size_t text_len,
                                         int offset, int greedy);
// 代码降阶
// lowering
// 降阶表达式为 IR
JL_DLLEXPORT jl_value_t *jl_lower(jl_value_t *expr, jl_module_t *inmodule,
                                  const char *file, int line, size_t world,
                                  bool_t warn);
// deprecated; use jl_parse_all
JL_DLLEXPORT jl_value_t *jl_parse_input_line(const char *text, size_t text_len,
                                             const char *filename, size_t filename_len);

// 动态链接库加载函数
// external libraries
enum JL_RTLD_CONSTANT {
     JL_RTLD_LOCAL=1U,
     JL_RTLD_GLOBAL=2U,
     JL_RTLD_LAZY=4U,
     JL_RTLD_NOW=8U,
     /* Linux/glibc and MacOS X: */
     JL_RTLD_NODELETE=16U,
     JL_RTLD_NOLOAD=32U,
     /* Linux/glibc: */
     JL_RTLD_DEEPBIND=64U,
     /* MacOS X 10.5+: */
     JL_RTLD_FIRST=128U
};
#define JL_RTLD_DEFAULT (JL_RTLD_LAZY | JL_RTLD_DEEPBIND)

// 动态库句柄类型
typedef void *jl_libhandle; // compatible with dlopen (void*) / LoadLibrary (HMODULE)
// 加载动态链接库
JL_DLLEXPORT jl_libhandle jl_load_dynamic_library(const char *fname, unsigned flags, int throw_err);
// 打开动态库
JL_DLLEXPORT jl_libhandle jl_dlopen(const char *filename, unsigned flags) JL_NOTSAFEPOINT;
// 关闭动态库
JL_DLLEXPORT int jl_dlclose(jl_libhandle handle) JL_NOTSAFEPOINT;
// 查找动态库符号
JL_DLLEXPORT int jl_dlsym(jl_libhandle handle, const char *symbol, void ** value, int throw_err, int search_deps) JL_NOTSAFEPOINT;

// 求值函数
// evaluation
// 顶层求值
JL_DLLEXPORT jl_value_t *jl_toplevel_eval(jl_module_t *m, jl_value_t *v);
JL_DLLEXPORT jl_value_t *jl_toplevel_eval_in(jl_module_t *m, jl_value_t *ex);
// 代码加载（解析+求值）
// code loading (parsing + evaluation)
// 求值字符串
JL_DLLEXPORT jl_value_t *jl_eval_string(const char *str); // embedding interface
// 从字符串加载代码
// 加载文件
JL_DLLEXPORT jl_value_t *jl_load_file_string(const char *text, size_t len,
                                             char *filename, jl_module_t *module);
JL_DLLEXPORT jl_value_t *jl_load(jl_module_t *module, const char *fname);

// 获取相对于当前模块的 Base 模块
JL_DLLEXPORT jl_module_t *jl_base_relative_to(jl_module_t *m JL_PROPAGATES_ROOT);

// 追踪函数
// tracing
// 注册新方法追踪回调
JL_DLLEXPORT void jl_register_newmeth_tracer(void (*callback)(jl_method_t *tracee));

// AST 操作
// AST access
// 复制 AST
JL_DLLEXPORT jl_value_t *jl_copy_ast(jl_value_t *expr JL_MAYBE_UNROOTED);

// IR 表示操作
// IR representation
// 压缩 IR
JL_DLLEXPORT jl_value_t *jl_compress_ir(jl_method_t *m, jl_code_info_t *code);
// 解压缩 IR
JL_DLLEXPORT jl_code_info_t *jl_uncompress_ir(jl_method_t *m, jl_code_instance_t *metadata, jl_value_t *data);
JL_DLLEXPORT uint8_t jl_ir_flag_inlining(jl_value_t *data) JL_NOTSAFEPOINT;
JL_DLLEXPORT uint8_t jl_ir_flag_has_fcall(jl_value_t *data) JL_NOTSAFEPOINT;
JL_DLLEXPORT uint8_t jl_ir_flag_has_image_globalref(jl_value_t *data) JL_NOTSAFEPOINT;
JL_DLLEXPORT uint16_t jl_ir_inlining_cost(jl_value_t *data) JL_NOTSAFEPOINT;
JL_DLLEXPORT ssize_t jl_ir_nslots(jl_value_t *data) JL_NOTSAFEPOINT;
JL_DLLEXPORT uint8_t jl_ir_slotflag(jl_value_t *data, size_t i) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_value_t *jl_compress_argnames(jl_array_t *syms);
JL_DLLEXPORT jl_array_t *jl_uncompress_argnames(jl_value_t *syms);
JL_DLLEXPORT jl_value_t *jl_uncompress_argname_n(jl_value_t *syms, size_t i);
JL_DLLEXPORT struct jl_codeloc_t jl_uncompress1_codeloc(jl_debuginfo_t *di, size_t pc) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_value_t *jl_compress_codelocs(int32_t firstloc, jl_value_t *codelocs, size_t nstmts);
JL_DLLEXPORT jl_value_t *jl_uncompress_codelocs(jl_debuginfo_t *di, size_t nstmts);
JL_DLLEXPORT jl_locspan_t jl_cdi_bytespan(jl_debuginfo_t *di, int32_t pc) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_locspan_t jl_cdi_byte_to_xy(jl_debuginfo_t *di, int32_t b) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_locspan_t jl_cdi_firstxy(jl_debuginfo_t *di, int32_t pc) JL_NOTSAFEPOINT;
JL_DLLEXPORT int32_t jl_cdi_external_firstline(jl_debuginfo_t *di) JL_NOTSAFEPOINT;
JL_DLLEXPORT int32_t jl_cdi_firstline_all(jl_debuginfo_t *di) JL_NOTSAFEPOINT;
JL_DLLEXPORT const char *jl_cdi_file(jl_debuginfo_t *di) JL_NOTSAFEPOINT;
JL_DLLEXPORT uint8_t jl_encode_inlining_cost(uint16_t inlining_cost) JL_NOTSAFEPOINT;
JL_DLLEXPORT uint16_t jl_decode_inlining_cost(uint8_t inlining_cost) JL_NOTSAFEPOINT;

// 运算符信息查询
JL_DLLEXPORT int jl_is_operator(const char *sym);
JL_DLLEXPORT int jl_is_unary_operator(const char *sym);
JL_DLLEXPORT int jl_is_unary_and_binary_operator(const char *sym);
JL_DLLEXPORT int jl_is_syntactic_operator(const char *sym);
JL_DLLEXPORT int jl_operator_precedence(const char *sym);

STATIC_INLINE int jl_vinfo_sa(uint8_t vi)
{
    return (vi&16)!=0;
}

STATIC_INLINE int jl_vinfo_usedundef(uint8_t vi)
{
    return (vi&32)!=0;
}

/* ======= 调用 Julia 函数 ======= */
// calling into julia ---------------------------------------------------------

// 通用函数调用
JL_DLLEXPORT jl_value_t *jl_apply_generic(jl_value_t *F, jl_value_t **args, uint32_t nargs);
// 调用特定方法实例
JL_DLLEXPORT jl_value_t *jl_invoke(jl_value_t *F, jl_value_t **args, uint32_t nargs, jl_method_instance_t *meth);
// 调用不透明闭包方法实例
JL_DLLEXPORT jl_value_t *jl_invoke_oc(jl_value_t *F, jl_value_t **args, uint32_t nargs, jl_method_instance_t *meth);
// 调用 API 层函数
JL_DLLEXPORT int32_t jl_invoke_api(jl_code_instance_t *linfo);

// 简化版函数调用
STATIC_INLINE jl_value_t *jl_apply(jl_value_t **args, uint32_t nargs)
{
    return jl_apply_generic(args[0], &args[1], nargs - 1);
}

// 调用 Julia 函数（参数数组）
JL_DLLEXPORT jl_value_t *jl_call(jl_value_t *f JL_MAYBE_UNROOTED, jl_value_t **args, uint32_t nargs);
// 调用无参数 Julia 函数
JL_DLLEXPORT jl_value_t *jl_call0(jl_value_t *f JL_MAYBE_UNROOTED);
JL_DLLEXPORT jl_value_t *jl_call1(jl_value_t *f JL_MAYBE_UNROOTED, jl_value_t *a JL_MAYBE_UNROOTED);
JL_DLLEXPORT jl_value_t *jl_call2(jl_value_t *f JL_MAYBE_UNROOTED, jl_value_t *a JL_MAYBE_UNROOTED, jl_value_t *b JL_MAYBE_UNROOTED);
JL_DLLEXPORT jl_value_t *jl_call3(jl_value_t *f JL_MAYBE_UNROOTED, jl_value_t *a JL_MAYBE_UNROOTED,
                                  jl_value_t *b JL_MAYBE_UNROOTED, jl_value_t *c JL_MAYBE_UNROOTED);
JL_DLLEXPORT jl_value_t *jl_call4(jl_value_t *f JL_MAYBE_UNROOTED, jl_value_t *a JL_MAYBE_UNROOTED,
                                  jl_value_t *b JL_MAYBE_UNROOTED, jl_value_t *c JL_MAYBE_UNROOTED,
                                  jl_value_t *d JL_MAYBE_UNROOTED);

/* ======= 异步信号处理 ======= */
// async signal handling ------------------------------------------------------

// 安装 SIGINT 信号处理器
JL_DLLEXPORT void jl_install_sigint_handler(void);
// 开始信号原子区
JL_DLLEXPORT void jl_sigatomic_begin(void);
// 结束信号原子区
JL_DLLEXPORT void jl_sigatomic_end(void);

/* ======= 任务与异常 ======= */
// tasks and exceptions -------------------------------------------------------

// 异常处理器的描述信息
// info describing an exception handler
// 异常处理器结构 —— 保存 setjmp 上下文、GC 栈和作用域
struct _jl_handler_t {
    jl_jmp_buf eh_ctx;
    jl_gcframe_t *gcstack;
    jl_value_t *scope;
    struct _jl_handler_t *prev;
    size_t locks_len;
    jl_timing_block_t *timing_stack;
    size_t world_age;
    sig_atomic_t defer_signal;
    int8_t gc_state;
};

// 任务状态常量
#define JL_TASK_STATE_RUNNABLE 0
#define JL_TASK_STATE_DONE     1
#define JL_TASK_STATE_FAILED   2

// 创建新任务
JL_DLLEXPORT jl_task_t *jl_new_task(jl_value_t*, jl_value_t*, size_t);
// 切换到指定任务
JL_DLLEXPORT void jl_switchto(jl_task_t **pt);
JL_DLLEXPORT int jl_set_task_tid(jl_task_t *task, int16_t tid) JL_NOTSAFEPOINT;
JL_DLLEXPORT int jl_set_task_threadpoolid(jl_task_t *task, int8_t tpid) JL_NOTSAFEPOINT;
// 抛出异常
JL_DLLEXPORT void JL_NORETURN jl_throw(jl_value_t *e JL_MAYBE_UNROOTED);
// 重新抛出异常
JL_DLLEXPORT void JL_NORETURN jl_rethrow(void);
JL_DLLEXPORT void JL_NORETURN jl_rethrow_other(jl_value_t *e JL_MAYBE_UNROOTED);
JL_DLLEXPORT void JL_NORETURN jl_no_exc_handler(jl_value_t *e, jl_task_t *ct);


#ifdef __cplusplus
}
#endif
#include "julia_locks.h"   // requires jl_task_t definition
#ifdef __cplusplus
extern "C" {
#endif

// 返回当前正在处理的异常
// Return the exception currently being handled, or `jl_nothing`.
//
// The catch scope is determined dynamically so this works in functions called
// from a catch block.  The returned value is gc rooted until we exit the
// enclosing JL_CATCH.
// FIXME: Teach the static analyzer about this rather than using
// JL_GLOBALLY_ROOTED which is far too optimistic.
JL_DLLEXPORT jl_value_t *jl_current_exception(jl_task_t *ct) JL_GLOBALLY_ROOTED JL_NOTSAFEPOINT;
// 检查是否有异常发生
JL_DLLEXPORT jl_value_t *jl_exception_occurred(void);
// 清除异常状态
JL_DLLEXPORT void jl_exception_clear(void) JL_NOTSAFEPOINT;

// 进入异常处理器
JL_DLLEXPORT void jl_enter_handler(jl_task_t *ct, jl_handler_t *eh) JL_NOTSAFEPOINT ;
// 恢复异常处理状态
JL_DLLEXPORT void jl_eh_restore_state(jl_task_t *ct, jl_handler_t *eh);
JL_DLLEXPORT void jl_eh_restore_state_noexcept(jl_task_t *ct, jl_handler_t *eh) JL_NOTSAFEPOINT;
JL_DLLEXPORT void jl_pop_handler(jl_task_t *ct, int n) JL_NOTSAFEPOINT;
JL_DLLEXPORT void jl_pop_handler_noexcept(jl_task_t *ct, int n) JL_NOTSAFEPOINT;
JL_DLLEXPORT size_t jl_excstack_state(jl_task_t *ct) JL_NOTSAFEPOINT;
JL_DLLEXPORT void jl_restore_excstack(jl_task_t *ct, size_t state) JL_NOTSAFEPOINT;

#if defined(_OS_WINDOWS_)
#if defined(_COMPILER_GCC_)
JL_DLLEXPORT int __attribute__ ((__nothrow__,__returns_twice__)) (jl_setjmp)(jmp_buf _Buf);
__declspec(noreturn) __attribute__ ((__nothrow__)) void (jl_longjmp)(jmp_buf _Buf, int _Value);
JL_DLLEXPORT int __attribute__ ((__nothrow__,__returns_twice__)) (ijl_setjmp)(jmp_buf _Buf);
__declspec(noreturn) __attribute__ ((__nothrow__)) void (ijl_longjmp)(jmp_buf _Buf, int _Value);
#else
JL_DLLEXPORT int (jl_setjmp)(jmp_buf _Buf);
void (jl_longjmp)(jmp_buf _Buf, int _Value);
JL_DLLEXPORT int (ijl_setjmp)(jmp_buf _Buf);
void (ijl_longjmp)(jmp_buf _Buf, int _Value);
#endif
#ifdef JL_LIBRARY_EXPORTS
#define jl_setjmp_f ijl_setjmp
#define jl_setjmp_name "ijl_setjmp"
#define jl_setjmp(a,b) ijl_setjmp(a)
#define jl_longjmp(a,b) ijl_longjmp(a,b)
#else
#define jl_setjmp_f jl_setjmp
#define jl_setjmp_name "jl_setjmp"
#define jl_setjmp(a,b) jl_setjmp(a)
#define jl_longjmp(a,b) jl_longjmp(a,b)
#endif
#elif defined(_OS_EMSCRIPTEN_)
#define jl_setjmp(a,b) setjmp(a)
#define jl_longjmp(a,b) longjmp(a,b)
#define jl_setjmp_f    setjmp
#define jl_setjmp_name "setjmp"
#else
// determine actual entry point name
#if defined(sigsetjmp)
#define jl_setjmp_f    __sigsetjmp
#define jl_setjmp_name "__sigsetjmp"
#else
#define jl_setjmp_f    sigsetjmp
#define jl_setjmp_name "sigsetjmp"
#endif
#define jl_setjmp(a,b) sigsetjmp(a,b)
#if defined(__GLIBC__)
// Route jl_longjmp through a function pointer so we can bypass the sanitizers' longjmp
// interceptor (which mishandles Julia's task stacks) when a sanitizer is active.
typedef void (*siglongjmp_func_t)(jmp_buf _Buf, int _Value) JL_NOTSAFEPOINT;
extern siglongjmp_func_t real_siglongjmp;
#define jl_longjmp(a,b) real_siglongjmp(a,b)
#else
#define jl_longjmp(a,b) siglongjmp(a,b)
#endif
#endif


#ifdef __clang_gcanalyzer__

extern int had_exception;

// The analyzer assumes that the TRY block always executes to completion.
// This can lead to both false positives and false negatives, since it doesn't model the fact that throwing always leaves the try block early.
// JL_TRY/JL_CATCH 异常处理宏
#define JL_TRY                                                      \
    int i__try, i__catch; jl_handler_t __eh; jl_task_t *__eh_ct;    \
    __eh_ct = jl_current_task;                                      \
    size_t __excstack_state = jl_excstack_state(__eh_ct);           \
    jl_enter_handler(__eh_ct, &__eh);                               \
    __eh_ct->eh = &__eh;                                            \
    for (i__try=1; i__try; i__try=0)

#define JL_CATCH                                                    \
    if (!had_exception)                                             \
        jl_eh_restore_state_noexcept(__eh_ct, &__eh);               \
    else                                                            \
        for (i__catch=1, jl_eh_restore_state(__eh_ct, &__eh); i__catch; i__catch=0, /* CATCH BLOCK; */ jl_restore_excstack(__eh_ct, __excstack_state))

#else

#define JL_TRY                                                      \
    int i__try, i__catch; jl_handler_t __eh; jl_task_t *__eh_ct;    \
    __eh_ct = jl_current_task;                                      \
    size_t __excstack_state = jl_excstack_state(__eh_ct);           \
    jl_enter_handler(__eh_ct, &__eh);                               \
    if (!jl_setjmp(__eh.eh_ctx, 0))                                 \
        for (i__try=1, __eh_ct->eh = &__eh; i__try; i__try=0, /* TRY BLOCK; */ jl_eh_restore_state_noexcept(__eh_ct, &__eh))

#define JL_CATCH                                                    \
    else                                                            \
        for (i__catch=1, jl_eh_restore_state(__eh_ct, &__eh); i__catch; i__catch=0, /* CATCH BLOCK; */ jl_restore_excstack(__eh_ct, __excstack_state))

#endif

/* ======= I/O 系统 ======= */
// I/O system -----------------------------------------------------------------

struct uv_loop_s;
struct uv_handle_s;
struct uv_stream_s;
#ifdef _OS_WINDOWS_
typedef HANDLE jl_uv_os_fd_t;
#else
typedef int jl_uv_os_fd_t;
#endif
#define JL_STREAM struct uv_stream_s
#define JL_STDOUT jl_uv_stdout
#define JL_STDERR jl_uv_stderr
#define JL_STDIN  jl_uv_stdin

JL_DLLEXPORT int jl_process_events(void);

JL_DLLEXPORT struct uv_loop_s *jl_global_event_loop(void) JL_NOTSAFEPOINT;

JL_DLLEXPORT void jl_close_uv(struct uv_handle_s *handle);

JL_DLLEXPORT jl_array_t *jl_take_buffer(ios_t *s);

typedef struct {
    void *data;
    struct uv_loop_s *loop;
    int type; // enum uv_handle_type
    jl_uv_os_fd_t file;
} jl_uv_file_t;

#ifdef __GNUC__
#  ifdef __MINGW32__
#define _JL_FORMAT_ATTR(str, arg) \
    __attribute__((format(__MINGW_PRINTF_FORMAT, str, arg)))
#  else
#define _JL_FORMAT_ATTR(str, arg) \
    __attribute__((format(printf, str, arg)))
#  endif
#else
#define _JL_FORMAT_ATTR(str, arg)
#endif

// 打印到 UV 流
JL_DLLEXPORT void jl_uv_puts(struct uv_stream_s *stream, const char *str, size_t n);
// 格式化输出到 UV 流
JL_DLLEXPORT int jl_printf(struct uv_stream_s *s, const char *format, ...)
    _JL_FORMAT_ATTR(2, 3);
JL_DLLEXPORT int jl_vprintf(struct uv_stream_s *s, const char *format, va_list args)
    _JL_FORMAT_ATTR(2, 0);
// 安全打印（不含 GC 安全点）
JL_DLLEXPORT void jl_safe_printf(const char *str, ...) JL_NOTSAFEPOINT
    _JL_FORMAT_ATTR(1, 2);
JL_DLLEXPORT void jl_safe_fprintf(ios_t *s, const char *str, ...) JL_NOTSAFEPOINT
    _JL_FORMAT_ATTR(2, 3);

extern JL_DLLEXPORT JL_STREAM *JL_STDIN;
extern JL_DLLEXPORT JL_STREAM *JL_STDOUT;
extern JL_DLLEXPORT JL_STREAM *JL_STDERR;

JL_DLLEXPORT JL_STREAM *jl_stdout_stream(void);
JL_DLLEXPORT JL_STREAM *jl_stdin_stream(void);
JL_DLLEXPORT JL_STREAM *jl_stderr_stream(void);
JL_DLLEXPORT int jl_termios_size(void);

// 打印与显示函数
// showing and std streams
JL_DLLEXPORT void jl_flush_cstdio(void) JL_NOTSAFEPOINT;
JL_DLLEXPORT jl_value_t *jl_stderr_obj(void) JL_NOTSAFEPOINT;
// 静态显示 Julia 值
JL_DLLEXPORT size_t jl_static_show(JL_STREAM *out, jl_value_t *v) JL_NOTSAFEPOINT;
JL_DLLEXPORT size_t jl_safe_static_show(JL_STREAM *out, jl_value_t *v) JL_NOTSAFEPOINT;
JL_DLLEXPORT size_t jl_static_show_func_sig(JL_STREAM *s, jl_value_t *type) JL_NOTSAFEPOINT;
// 打印回溯信息
JL_DLLEXPORT void jl_print_backtrace(void) JL_NOTSAFEPOINT;
JL_DLLEXPORT void jl_fprint_backtrace(ios_t *s) JL_NOTSAFEPOINT;
JL_DLLEXPORT void jlbacktrace(void) JL_NOTSAFEPOINT; // deprecated
// Mainly for debugging, use `void*` so that no type cast is needed in C++.
JL_DLLEXPORT void jl_(void *jl_value) JL_NOTSAFEPOINT;
// Mainly for debugging, a high-verbosity version of `jl_`
JL_DLLEXPORT void jl__(void *jl_value) JL_NOTSAFEPOINT;
// Mainly for debugging, print memory layout and type info of a jl_value_t
// 打印 jl_value_t 的内存布局和类型信息（用于调试）
JL_DLLEXPORT void jl_m(void *jl_value) JL_NOTSAFEPOINT;

/* ======= Julia 选项 ======= */
// julia options -----------------------------------------------------------

// 获取 jl_options 结构体大小
JL_DLLEXPORT ssize_t jl_sizeof_jl_options(void);

// Parse an argc/argv pair to extract general julia options, passing back out
// any arguments that should be passed on to the script.
// 解析命令行参数
JL_DLLEXPORT void jl_parse_opts(int *argcp, char ***argvp);
JL_DLLEXPORT char *jl_format_filename(const char *output_pattern) JL_NOTSAFEPOINT;

uint64_t parse_heap_size_option(const char *optarg, const char *option_name, int allow_pct);

// Set julia-level ARGS array according to the arguments provided in
// argc/argv
// 设置 ARGS 数组
JL_DLLEXPORT jl_value_t *jl_set_ARGS(int argc, char **argv);

// 检查是否正在生成输出
JL_DLLEXPORT int jl_generating_output(void) JL_NOTSAFEPOINT;

// Settings for code_coverage and malloc_log
// NOTE: if these numbers change, test/cmdlineargs.jl will have to be updated
// 日志级别常量
#define JL_LOG_NONE 0
#define JL_LOG_USER 1
#define JL_LOG_ALL  2
#define JL_LOG_PATH 3

// 选项常量：边界检查
#define JL_OPTIONS_CHECK_BOUNDS_DEFAULT 0
#define JL_OPTIONS_CHECK_BOUNDS_ON 1
#define JL_OPTIONS_CHECK_BOUNDS_OFF 2

// 选项常量：编译模式
#define JL_OPTIONS_COMPILE_DEFAULT 1
#define JL_OPTIONS_COMPILE_OFF 0
#define JL_OPTIONS_COMPILE_ON  1
#define JL_OPTIONS_COMPILE_ALL 2
#define JL_OPTIONS_COMPILE_MIN 3

// 选项常量：颜色输出
#define JL_OPTIONS_COLOR_AUTO 0
#define JL_OPTIONS_COLOR_ON 1
#define JL_OPTIONS_COLOR_OFF 2

#define JL_OPTIONS_HISTORYFILE_ON 1
#define JL_OPTIONS_HISTORYFILE_OFF 0

#define JL_OPTIONS_STARTUPFILE_ON 1
#define JL_OPTIONS_STARTUPFILE_OFF 2

// 日志级别值
#define JL_LOGLEVEL_BELOWMIN -1000001
#define JL_LOGLEVEL_DEBUG    -1000
#define JL_LOGLEVEL_INFO      0
#define JL_LOGLEVEL_WARN      1000
#define JL_LOGLEVEL_ERROR     2000
#define JL_LOGLEVEL_ABOVEMAX  1000001

// 选项常量：弃用警告
#define JL_OPTIONS_DEPWARN_OFF 0
#define JL_OPTIONS_DEPWARN_ON 1
#define JL_OPTIONS_DEPWARN_ERROR 2

#define JL_OPTIONS_WARN_OVERWRITE_OFF 0
#define JL_OPTIONS_WARN_OVERWRITE_ON 1

#define JL_OPTIONS_WARN_SCOPE_OFF 0
#define JL_OPTIONS_WARN_SCOPE_ON 1

// 选项常量：Polly 优化
#define JL_OPTIONS_POLLY_ON 1
#define JL_OPTIONS_POLLY_OFF 0

// 选项常量：快速数学
#define JL_OPTIONS_FAST_MATH_ON 1
#define JL_OPTIONS_FAST_MATH_OFF 2
#define JL_OPTIONS_FAST_MATH_DEFAULT 0

#define JL_OPTIONS_HANDLE_SIGNALS_ON 1
#define JL_OPTIONS_HANDLE_SIGNALS_OFF 0

#define JL_OPTIONS_USE_EXPERIMENTAL_FEATURES_YES 1
#define JL_OPTIONS_USE_EXPERIMENTAL_FEATURES_NO 0

#define JL_OPTIONS_USE_SYSIMAGE_NATIVE_CODE_YES 1
#define JL_OPTIONS_USE_SYSIMAGE_NATIVE_CODE_NO 0

#define JL_OPTIONS_USE_COMPILED_MODULES_STRICT 3
#define JL_OPTIONS_USE_COMPILED_MODULES_EXISTING 2
#define JL_OPTIONS_USE_COMPILED_MODULES_YES 1
#define JL_OPTIONS_USE_COMPILED_MODULES_NO 0

#define JL_OPTIONS_USE_PKGIMAGES_EXISTING 2
#define JL_OPTIONS_USE_PKGIMAGES_YES 1
#define JL_OPTIONS_USE_PKGIMAGES_NO 0

// 选项常量：裁剪模式
#define JL_TRIM_NO 0
#define JL_TRIM_SAFE 1
#define JL_TRIM_UNSAFE 2
#define JL_TRIM_UNSAFE_WARN 3

#define JL_OPTIONS_TASK_METRICS_OFF 0
#define JL_OPTIONS_TASK_METRICS_ON 1

// 版本信息函数
// Version information
#include <julia_version.h> // Generated file

// 获取 Julia 主版本号
JL_DLLEXPORT extern int jl_ver_major(void);
JL_DLLEXPORT extern int jl_ver_minor(void);
JL_DLLEXPORT extern int jl_ver_patch(void);
JL_DLLEXPORT extern int jl_ver_is_release(void);
JL_DLLEXPORT extern const char *jl_ver_string(void);

// 可空类型表示
// nullable struct representations
typedef struct {
    uint8_t hasvalue;
    double value;
} jl_nullable_float64_t;

typedef struct {
    uint8_t hasvalue;
    float value;
} jl_nullable_float32_t;

// 获取根任务
#define jl_root_task (jl_current_task->ptls->root_task)
JL_DLLEXPORT jl_task_t *jl_get_current_task(void) JL_GLOBALLY_ROOTED JL_NOTSAFEPOINT;

// 获取模块中的函数
STATIC_INLINE jl_value_t *jl_get_function(jl_module_t *m, const char *name)
{
    return (jl_value_t*)jl_get_global(m, jl_symbol(name));
}

// TODO: we need to pin the task while using this (set pure bit)
JL_DLLEXPORT jl_jmp_buf *jl_get_safe_restore(void) JL_NOTSAFEPOINT;
JL_DLLEXPORT void jl_set_safe_restore(jl_jmp_buf *) JL_NOTSAFEPOINT;

/* ======= 代码生成接口 ======= */
// codegen interface ----------------------------------------------------------
// The root propagation here doesn't have to be literal, but callers should
// ensure that the return value outlives the MethodInstance
// 代码生成参数 —— 控制编译器的行为设置
// Must be kept in sync with `base/reflection.jl` (CodegenParams)
typedef struct {
    int track_allocations;  // can we track allocations?
    int code_coverage;      // can we measure coverage?
    int prefer_specsig;     // are specialized function signatures preferred?

    // controls the emission of debug-info. mirrors the clang options
    int gnu_pubnames;       // can we emit the gnu pubnames debuginfo
    int debug_info_kind;    // Enum for line-table-only, line-directives-only,
                            // limited, standalone
    int debug_info_level;   // equivalent to the -g level from the cli
    int safepoint_on_entry; // Emit a safepoint on entry to each function
    int gcstack_arg; // Pass the ptls value as an argument with swiftself

    int use_jlplt; // Whether to use the Julia PLT mechanism or emit symbols directly
    int force_emit_all; // Force emission of code for const return functions

    // These options control the sanitizer passes and are used to AOT compile instrumented sysimages
    int sanitize_memory;
    int sanitize_thread;
    int sanitize_address;

    int unique_names;   // Emit globally unique names
} jl_cgparams_t;
extern JL_DLLEXPORT int jl_default_debug_info_kind;
extern JL_DLLEXPORT jl_cgparams_t jl_default_cgparams;

typedef struct {
    int emit_metadata;
} jl_emission_params_t;

#ifdef __cplusplus
}
#endif

#endif

