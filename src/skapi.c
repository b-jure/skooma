#include "object.h"
#include "skconf.h"
#include "skooma.h"
#include "stdarg.h"
#include "value.h"
#include "vmachine.h"

#include <time.h>




#define stklast(vm) cast_intptr(vm->stack + VM_STACK_MAX - 1)

/* Increment stack pointer */
#define incsp(vm)                                                                        \
    do {                                                                                 \
        (vm)->sp++;                                                                      \
        sk_checkapi(vm, vm->sp - vm->stack <= VM_STACK_MAX, "stack overflow.");          \
    } while(0)

/* Decrement stack pointer */
#define decsp(vm)                                                                        \
    do {                                                                                 \
        (vm)->sp--;                                                                      \
        sk_checkapi(vm, vm->stack <= (vm)->sp, "stack underflow.");                      \
    } while(0)



/* Get stack value at 'idx'. */
static force_inline Value* idx2val(const VM* vm, int idx)
{
    Value* fn = vm->frames[vm->fc - 1]->callee;
    if(idx >= 0) {
        sk_checkapi(vm, idx < vm->sp - 1 - fn, "index too big.");
        return (fn + 1 + idx);
    } else { // idx is negative
        sk_checkapi(vm, -idx <= (vm->sp - fn), "Invalid index.");
        return (vm->sp + idx);
    }
}


/* Local strlen implementation. */
static force_inline size_t skstrlen(const char* str)
{
    size_t         len = 0;
    unsigned char* c   = (unsigned char*)str;
    while(*c++)
        len++;
    return len;
}


/* Shifts values on stack either to the left or right (once).
 * 0 direction is a left shift, anything else is a right shift. */
static force_inline void stackshift(VM* vm, Value* val, int direction)
{
    uintptr_t shift = vm->sp - (val + 1);
    if(direction == 0 && shift > 0) memcpy(val, val + 1, shift);
    else if(shift > 0) memcpy(val + 1, val, shift);
}



/* Ensure the stack has enough space. */
SK_API int sk_ensurestack(VM* vm, int n)
{
    sk_checkapi(vm, n >= 0, "negative 'n'.");
    return (((vm->sp - vm->stack) + n) < VM_STACK_MAX);
}







/*
 * CREATE/DESTROY the VM.
 */

/* Create the VM initialized with the 'cfg'.
 * If 'cfg' is NULL then the VM initializes per default settings. */
SK_API VM* sk_create(Config* cfg)
{
    AllocatorFn allocate = reallocate;
    void*       userdata = NULL;
    if(cfg != NULL) {
        userdata = cfg->userdata;
        allocate = cfg->reallocate ? cfg->reallocate : reallocate;
    }
    VM* vm = allocate(NULL, sizeof(VM), userdata);
    memset(&vm->config, 0, sizeof(Config));
    if(cfg != NULL) {
        memcpy(&vm->config, cfg, sizeof(Config));
        vm->config.reallocate = allocate;
    } else Config_init(&vm->config);
    srand(time(0));
    vm->seed         = rand();
    vm->fc           = 0;
    vm->objects      = NULL;
    vm->F            = NULL;
    vm->open_upvals  = NULL;
    vm->script       = NIL_VAL;
    vm->gc_allocated = 0;
    vm->gc_next      = (1 << 20); // 1 MiB
    vm->gc_flags     = 0;
    vm->sp           = vm->stack;
    HashTable_init(&vm->loaded); // Loaded scripts and their functions
    HashTable_init(&vm->globids); // Global variable identifiers
    GARRAY_INIT(vm); // Global values array
    GSARRAY_INIT(vm); // Gray stack array (no GC)
    Array_Value_init(&vm->temp, vm); // Temp values storage (return values)
    Array_VRef_init(&vm->callstart, vm);
    Array_VRef_init(&vm->retstart, vm);
    HashTable_init(&vm->strings); // Interned strings table (Weak_refs)
    memset(vm->statics, 0, sizeof(vm->statics));
    for(UInt i = 0; i < SS_SIZE; i++)
        vm->statics[i] = OString_new(vm, static_str[i].name, static_str[i].len);
    // @REFACTOR?: Maybe make the native functions private and only
    //             callable inside class instances?
    //             Upside: less branching resulting in more straightforward code.
    //             Downside: slower function call (maybe not so bad because
    //             of removal of type checking inside the native functions, needs
    //             testing)
    //
    // @TODO?: Change NativeFn signature to accept variable amount of arguments.
    //         Upside: More expressive and flexible functions.
    //         Downside: va_list parsing resulting in slower function call
    //         processing
    // VM_define_native(vm, "clock", native_clock, 0, false); // GC
    // VM_define_native(vm, "isfield", native_isfield, 2, false); // GC
    // VM_define_native(vm, "printl", native_printl, 1, false); // GC
    // VM_define_native(vm, "print", native_print, 1, false); // GC
    // VM_define_native(vm, "tostr", native_tostr, 1, false); // GC
    // VM_define_native(vm, "isstr", native_isstr, 1, false); // GC
    // VM_define_native(vm, "strlen", native_strlen, 1, false); // GC
    // VM_define_native(vm, "strpat", native_strpat, 2, false); // GC
    // VM_define_native(vm, "strsub", native_strsub, 3, false); // GC
    // VM_define_native(vm, "strbyte", native_strbyte, 2, false); // GC
    // VM_define_native(vm, "strlower", native_strlower, 1, false); // GC
    // VM_define_native(vm, "strupper", native_strupper, 1, false); // GC
    // VM_define_native(vm, "strrev", native_strrev, 1, false); // GC
    // VM_define_native(vm, "strconcat", native_strconcat, 2, false); // GC
    // VM_define_native(vm, "byte", native_byte, 1, false); // GC
    // VM_define_native(vm, "gcfactor", native_gcfactor, 1, false); // GC
    // VM_define_native(vm, "gcmode", native_gcmode, 1, false); // GC
    // VM_define_native(vm, "gccollect", native_gccollect, 0, false); // GC
    // VM_define_native(vm, "gcleft", native_gcleft, 0, false); // GC
    // VM_define_native(vm, "gcusage", native_gcusage, 0, false); // GC
    // VM_define_native(vm, "gcnext", native_gcnext, 0, false); // GC
    // VM_define_native(vm, "gcset", native_gcset, 1, false); // GC
    // VM_define_native(vm, "gcisauto", native_gcisauto, 0, false); // GC
    // VM_define_native(vm, "assert", native_assert, 1, false); // GC
    // VM_define_native(vm, "assertf", native_assertf, 2, false); // GC
    // VM_define_native(vm, "error", native_error, 1, false); // GC
    // VM_define_native(vm, "typeof", native_typeof, 1, false); // GC
    // VM_define_native(vm, "loadscript", native_loadscript, 1, false); // GC
    return vm;
}


/* Free the VM allocation, the pointer to VM will be nulled out. */
SK_API void sk_destroy(VM** vmp)
{
    if(likely(vmp)) { // non-null pointer ?
        sk_lock(*vmp);
        if(*vmp == NULL) return;
        VM* vm = *vmp;
        HashTable_free(vm, &vm->loaded);
        HashTable_free(vm, &vm->globids);
        GARRAY_FREE(vm);
        GSARRAY_FREE(vm);
        Array_Value_free(&vm->temp, NULL);
        Array_VRef_free(&vm->callstart, NULL);
        Array_VRef_free(&vm->retstart, NULL);
        HashTable_free(vm, &vm->strings);
        O* next;
        for(O* head = vm->objects; head != NULL; head = next) {
            next = onext(head);
            ofree(vm, head);
        }
        FREE(vm, vm);
        *vmp = NULL;
    }
}






/*
 * CHECK/GET VALUE TYPE.
 */

/*
 * If the hardware supports 'find first set' (has the instruction)
 * bit operation then enable this define
 */
#if __has_builtin(__builtin_ctz)
    /* Create type bitmask from the value.
     * First least significant set bit acts as a type tag.
     * bit 0 is set -> number
     * bit 1 is set -> string
     * bit 2 is set -> callable
     * bit 3 is set -> bool
     * bit 4 is set -> nil
     * bit 5 is set -> instance
     * bit 6 is set -> class */
    #define val2tbmask(value)                                                            \
        cast_uint(                                                                       \
            0 | (IS_NUMBER(value) * 1) | (IS_STRING(value) * 2) |                        \
            ((IS_FUNCTION(value) | IS_BOUND_METHOD(value) | IS_CLOSURE(value) |          \
              IS_NATIVE(value)) *                                                        \
             4) |                                                                        \
            IS_BOOL(value) * 8 | IS_NIL(value) * 16 | IS_INSTANCE(value) * 32 |          \
            IS_CLASS(value) * 64)
#endif


/* Auxiliary to sk_type */
static int val2type(const VM* vm, Value* value)
{
/* If the hardware supports 'find first set' and
 * compiler supports threaded code then use that. */
#if defined(S_PRECOMPUTED_GOTO) && __has_builtin(__builtin_ctz)
    static const int typetable[] = {
        SK_TNUMBER,
        SK_TSTRING,
        SK_TFUNCTION,
        SK_TBOOL,
        SK_TNIL,
        SK_TINSTANCE,
        SK_TCLASS,
    };
    // https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html#index-_005f_005fbuiltin_005fctz
    unsigned char bitidx = __builtin_ctz(val2tbmask(*value));
    return typetable[bitidx];

/* Otherwise use this as fallback */
#else
    if(IS_NUMBER(value)) return SK_TNUMBER;
    else if(IS_STRING(value)) return SK_TSTRING;
    else if(
        IS_FUNCTION(value) || IS_BOUND_METHOD(value) || IS_CLOSURE(value) ||
        IS_NATIVE(value))
        return SK_TFUNCTION;
    else if(IS_BOOL(value)) return SK_TBOOL;
    else if(IS_NIL(value)) return SK_TNIL;
    else if(IS_INSTANCE(value)) return SK_TINSTANCE;
    else if(IS_CLASS(value)) return SK_TCLASS;
#endif
    unreachable;
}

/* Return type of the value on the stack at 'idx'. */
SK_API int sk_type(const VM* vm, int idx)
{
    Value* value = idx2val(vm, idx);
    return val2type(vm, value);
}

/* Return type name of the value on the stack at 'idx'.
 * This returned pointer is 'const' indicating the
 * memory it points to should not be modified. */
SK_API const char* sk_typename(const VM* vm, int idx)
{
    Value* value = idx2val(vm, idx);
    int    type  = val2type(vm, value);
    return vm->statics[type]->storage;
}

/* Check if the value on the stack at 'idx' is nil. */
SK_API int sk_isnil(const VM* vm, int idx)
{
    return IS_NIL(*idx2val(vm, idx));
}

/* Check if the value on the stack at 'idx' is number. */
SK_API int sk_isnumber(const VM* vm, int idx)
{
    return IS_NUMBER(*idx2val(vm, idx));
}

/* Check if the value on the stack at 'idx' is string. */
SK_API int sk_isstring(const VM* vm, int idx)
{
    return IS_STRING(*idx2val(vm, idx));
}

/* Check if the value on the stack at 'idx' is bool. */
SK_API int sk_isbool(const VM* vm, int idx)
{
    return IS_BOOL(*idx2val(vm, idx));
}

/* Check if the value on the stack at 'idx' is class. */
SK_API int sk_isclass(const VM* vm, int idx)
{
    return IS_CLASS(*idx2val(vm, idx));
}

/* Check if the value on the stack at 'idx' is instance. */
SK_API int sk_isinstance(const VM* vm, int idx)
{
    return IS_INSTANCE(*idx2val(vm, idx));
}





/*
 * PUSH from C -> stack
 */

/* Push value on the stack */
#define pushval(vm, val)                                                                 \
    do {                                                                                 \
        *(vm)->sp = val;                                                                 \
        incsp(vm);                                                                       \
    } while(0)

/* Push object on the stack */
#define pusho(vm, o) pushval(vm, OBJ_VAL(o))

/* Push string object */
#define pushostring(vm, string) pusho(vm, (O*)(string))

/* Push closure object */
#define pushoclosure(vm, closure) pusho(vm, (O*)(closure))

/* Push class */
#define pushoclass(vm, class) pusho(vm, (O*)(class))

/* Push instance */
#define pushoinst(vm, inst) pusho(vm, (O*)(inst))

/* Push nil literal */
#define pushnil(vm) pushval(vm, NIL_VAL)

/* Push formatted cstring */
#define pushfstr(vm, fmt, argp)                                                          \
    if(fmt) pushostring(vm, OString_fmt_from(vm, fmt, argp));                            \
    else pushnil(vm);

/* Push string */
#define pushstr(vm, ptr, len)                                                            \
    if(ptr) pushostring(vm, OString_new(vm, ptr, len));                                  \
    else pushnil(vm);

/* Push cstring */
#define pushcstr(vm, ptr) pushstr(vm, ptr, skstrlen(ptr))

/* Push true literal */
#define pushtrue(vm) pushval(vm, TRUE_VAL)

/* Push false literal */
#define pushfalse(vm) pushval(vm, FALSE_VAL)

/* Push bool */
#define pushbool(vm, b) pushval(vm, BOOL_VAL(b))

/* Push number */
#define pushnum(vm, n) pushval(vm, NUMBER_VAL(n))



/* Push nil on the stack */
SK_API void sk_pushnil(VM* vm)
{
    sk_lock(vm);
    pushnil(vm);
    sk_unlock(vm);
}

/* Push number on the stack */
SK_API void sk_pushnumber(VM* vm, sk_number number)
{
    sk_lock(vm);
    pushnum(vm, number);
    sk_unlock(vm);
}

/* Push string on the stack */
SK_API void sk_pushstring(VM* vm, const char* str, size_t len)
{
    sk_lock(vm);
    pushstr(vm, str, len);
    sk_unlock(vm);
}

/* Push cstring on the stack */
SK_API void sk_pushcstring(VM* vm, const char* str)
{
    sk_lock(vm);
    pushstr(vm, str, skstrlen(str));
    sk_unlock(vm);
}

/* Push formatted cstring on the stack */
SK_API const char* sk_pushfstring(VM* vm, const char* fmt, ...)
{
    const char* str;
    va_list     argp;
    sk_lock(vm);
    va_start(argp, fmt);
    pushfstr(vm, fmt, argp);
    va_end(argp);
    sk_unlock(vm);
    return str;
}

/* Push boolean on the stack */
SK_API void sk_pushbool(VM* vm, int boolean)
{
    sk_lock(vm);
    sk_checkapi(vm, boolean == 0 || boolean == 1, "invalid boolean.");
    pushbool(vm, boolean);
    sk_unlock(vm);
}


/* Auxiliary to sk_hasmethod */
static force_inline int getmethod(VM* vm, OClass* class, const char* method)
{
    Value m;
    pushstr(vm, method, skstrlen(method));
    if(HashTable_get(&class->methods, *stackpeek(0), &m)) {
        *stackpeek(0) = OBJ_VAL(AS_CLOSURE(m));
        return 1;
    }
    vm->sp--; // pop method name
    return 0;
}

/* Push class method of an instance at idx on top of the stack.
 * If class instance has a method with 'name' then the method will be
 * pushed on top of the stack and this function will return 1, otherwise
 * nothing will be pushed on the stack and this function will return 0. */
SK_API int sk_pushmethod(VM* vm, int idx, const char* method)
{
    sk_lock(vm);
    Value val = *idx2val(vm, idx);
    sk_checkapi(vm, IS_INSTANCE(val), "expected instance.");
    int res = getmethod(vm, AS_INSTANCE(val)->oclass, method);
    sk_unlock(vm);
    return res;
}


/* Auxiliary to sk_pushglobal */
static force_inline int getglobal(VM* vm, const char* name)
{
    Value gval;
    pushstr(vm, name, skstrlen(name));
    if(HashTable_get(&vm->globids, *stackpeek(0), &gval)) {
        int idx       = (int)AS_NUMBER(gval);
        *stackpeek(0) = vm->globvals[idx].value;
        return 1;
    }
    vm->sp--; // pop global name
    return 0;
}

/* Push global value on top of the stack.
 * In case global value was found, it will be on top of the stack,
 * and this function will return 1, otherwise nothing will be pushed
 * on the stack and the function will return 0. */
SK_API int sk_pushglobal(VM* vm, const char* name)
{
    sk_lock(vm);
    int res = getglobal(vm, name);
    sk_unlock(vm);
    return res;
}


/* Push value from the stack located at 'idx', on top of the stack */
SK_API void sk_push(VM* vm, int idx)
{
    sk_lock(vm);
    pushval(vm, *idx2val(vm, idx));
    sk_unlock(vm);
}








/*
 * GET from STACK -> C
 */

/* Get boolean value (int 1/0) from the stack at 'idx'.
 * If the value at 'idx' is not a boolean, then the flag
 * if provided 'isbool' is set as 0, otherwise flag is set to 1. */
SK_API int sk_getbool(const VM* vm, int idx, int* isbool)
{
    int   bval;
    Value val = *idx2val(vm, idx);
    int   is  = tobool(val, &bval);
    if(isbool) *isbool = is;
    return bval;
}

/* Get number value (sk_number) from the stack at 'idx'.
 * If the value at 'idx' is not a number, then the flag
 * if provided 'isnum' is set as 0, otherwise flag is set to 1. */
SK_API sk_number sk_getnumber(const VM* vm, int idx, int* isnum)
{
    sk_number nval = 0.0;
    Value     val  = *idx2val(vm, idx);
    int       is   = tonumber(val, &nval);
    if(isnum) *isnum = is;
    return nval;
}

/* Get string value from the stack at 'idx'.
 * Returns NULL (0) if the value is not a string.
 * Otherwise it returns pointer to the start of the string.
 * Returned pointer is 'const' indicating that user should not
 * modify the contents the pointer points to. */
SK_API const char* sk_getstring(const VM* vm, int idx)
{
    Value val = *idx2val(vm, idx);
    return IS_STRING(val) ? AS_CSTRING(val) : 0;
}

/* Return the length of the value at 'idx'. */
SK_API size_t sk_rawlen(const VM* vm, int idx)
{
    size_t len;
    Value* val  = idx2val(vm, idx);
    int    type = val2type(vm, val);
    switch(type) {
        case SK_TSTRING:
            len = AS_STRING(*val)->len;
            break;
        case SK_TCLASS:
            len = AS_CLASS(*val)->methods.len;
            break;
        default:
            len = 0;
            break;
    }
    return len;
}

/* Return the number of values currently on the stack
 * relative to the current function */
SK_API int sk_gettop(const VM* vm)
{
    return cast_int(vm->sp - (vm->frames[vm->fc - 1]->callee + 1));
}








/*
 * STACK MANIPULATION
 */

/* Sets the new stack top relative to the current function */
SK_API void sk_settop(VM* vm, int idx)
{
    sk_lock(vm);
    Value* fn = vm->frames[vm->fc - 1]->callee;
    if(idx >= 0) {
        sk_checkapi(vm, idx < ((Value*)stklast(vm) - fn), "index too big.");
        intptr_t diff = ((fn + 1) + idx) - vm->sp;
        for(; diff > 0; diff--)
            *vm->sp++ = NIL_VAL;
    } else { // index negative
        sk_checkapi(vm, -idx <= (vm->sp - fn), "invalid index.");
        vm->sp += (idx + 1);
    }
    sk_unlock(vm);
}

/* Remove the value from the stack at 'idx' and shift the
 * stack to the left to fill the gap */
SK_API void sk_remove(VM* vm, int idx)
{
    sk_lock(vm);
    Value* val = idx2val(vm, idx);
    stackshift(vm, val, 0);
    decsp(vm);
    sk_unlock(vm);
}

/* Insert the value on top of the stack at the 'idx' and
 * shift the stack to the right to make space for the new value */
SK_API void sk_insert(VM* vm, int idx)
{
    sk_lock(vm);
    Value* top = vm->sp - 1;
    Value* val = idx2val(vm, idx);
    stackshift(vm, val, 1);
    *val = *top;
    incsp(vm);
    sk_unlock(vm);
}

/* Pop the value on top of the stack and then replace the
 * value at 'idx' with the popped value. */
SK_API void sk_replace(VM* vm, int idx)
{
    sk_lock(vm);
    Value* top = vm->sp - 1;
    Value* val = idx2val(vm, idx);
    if(top != val) *val = *top;
    decsp(vm);
    sk_unlock(vm);
}



/* Call the value on the stack located at the 'idx'. */
SK_API int sk_vcall(VM* vm, int idx, int argc, int retcnt)
{
    sk_lock(vm);
    Value callee = *idx2val(vm, idx);
    int   res    = callv(vm, callee, argc, retcnt);
    if(!callv(vm, callee, argc, retcnt)) {
    }
    sk_unlock(vm);
    return res;
}
