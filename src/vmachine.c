#include "array.h"
#include "chunk.h"
#include "common.h"
#include "corelib.h"
#include "debug.h"
#include "err.h"
#include "hash.h"
#include "mem.h"
#include "object.h"
#include "parser.h"
#include "skconf.h"
#include "skmath.h"
#include "skooma.h"
#include "value.h"
#include "vmachine.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>



volatile Int runtime = 0; // VM is running?



void runerror(VM* vm, const char* errfmt, ...)
{
    fputs("\nSkooma: [runtime error]\nSkooma: ", stderr);
    va_list ap;
    va_start(ap, errfmt);
    vfprintf(stderr, errfmt, ap);
    va_end(ap);
    putc('\n', stderr);
    for(Int i = vm->fc - 1; i >= 0; i--) {
        CallFrame* frame = &vm->frames[i];
        Chunk*     chunk = &frame->closure->fn->chunk;
        UInt       line  = Chunk_getline(chunk, frame->ip - chunk->code.data - 1);
        Value      _;
        bool       loaded = false;
        if(HashTable_get(&vm->loaded, OBJ_VAL(FFN(frame)->name), &_)) {
            vm->script = OBJ_VAL(FFN(frame)->name);
            loaded     = true;
        }
        fprintf(stderr, "Skooma: ['%s' on line %u] in ", AS_CSTRING(vm->script), line);
        if(loaded) fprintf(stderr, "script\n");
        else fprintf(stderr, "%s()\n", FFN(frame)->name->storage);
    }
}

void push(VM* vm, Value val)
{
    if(likely(vm->sp - vm->stack < (UInt)VM_STACK_MAX)) *vm->sp++ = val;
    else {
        fprintf(stderr, "VM stack overflow. Limit [%u].\n", (UInt)VM_STACK_MAX);
        _cleanupvm(&vm);
        exit(EXIT_FAILURE);
    }
}

static OString* concatenate(VM* vm, Value a, Value b)
{
    OString* left   = AS_STRING(a);
    OString* right  = AS_STRING(b);
    size_t   length = left->len + right->len;
    char     buffer[length + 1];
    memcpy(buffer, left->storage, left->len);
    memcpy(buffer + left->len, right->storage, right->len);
    buffer[length]  = '\0';
    OString* string = OString_new(vm, buffer, length);
    popn(vm, 2);
    return string;
}

static force_inline OBoundMethod*
bindmethod(VM* vm, OClass* oclass, Value name, Value receiver)
{
    Value method;
    if(unlikely(!HashTable_get(&oclass->methods, name, &method))) {
        vm->sp[-1] =
            OBJ_VAL(UNDEFINED_PROPERTY_ERR(vm, AS_CSTRING(name), oclass->name->storage));
        return NULL;
    }
    OBoundMethod* bound_method = OBoundMethod_new(vm, receiver, AS_CLOSURE(method));
    return bound_method;
}

static force_inline void
VM_define_native(VM* vm, const char* name, CFunction native, UInt arity, bool isva)
{
    push(vm, OBJ_VAL(OString_new(vm, name, strlen(name))));
    push(vm, OBJ_VAL(ONative_new(vm, AS_STRING(*stackpeek(0)), native, arity, isva)));
    UInt idx = GARRAY_PUSH(vm, ((Variable){.value = vm->stack[1], .flags = 0x00}));
    HashTable_insert(vm, &vm->globids, vm->stack[0], NUMBER_VAL((double)idx));
    popn(vm, 2);
}

void Config_init(Config* config)
{
    config->reallocate        = reallocate;
    config->userdata          = NULL;
    config->load_script       = NULL;
    config->rename_script     = NULL;
    config->gc_init_heap_size = 10 * (1 << 20); // 10 MiB
    config->gc_min_heap_size  = (1 << 20); // 1 MiB
    config->gc_grow_factor    = GC_HEAP_GROW_FACTOR;
}


static bool callfn(VM* vm, OClosure* callee, Int argc, Int retcnt)
{
    OString*   err;
    OFunction* fn = callee->fn;
    if(unlikely(!fn->isva && (Int)fn->arity != argc)) {
        err = FN_ARGC_ERR(vm, fn->arity, argc);
    } else if(unlikely(fn->isva && (Int)fn->arity > argc)) {
        err = FN_VA_ARGC_ERR(vm, fn->arity, argc);
    } else if(unlikely(vm->fc == VM_FRAMES_MAX)) {
        err = FRAME_LIMIT_ERR(vm, VM_FRAMES_MAX);
    } else goto ok;
    vm->sp[-argc - 1] = OBJ_VAL(err);
    return false;
ok:;
    CallFrame* frame = &vm->frames[vm->fc++];
    frame->vacnt     = argc - fn->arity;
    frame->retcnt    = retcnt;
    frame->closure   = callee;
    frame->ip        = fn->chunk.code.data;
    frame->callee    = vm->sp - argc - 1;
    return true;
}

static force_inline void moveresults(VM* vm, Value* nativefn, Int got, Int expect)
{
    Value* retstart = vm->sp - got; // start of return values
    if(expect == 0) expect = got; // all results (MULRET)
    if(got > expect) got = expect; // remove extra results
    memcpy(nativefn, retstart, got); // Safety: 'retstart' >= 'nativefn'
    for(int i = got; i < expect; i++) // replace missing values with nil
        nativefn[i] = NIL_VAL;
    vm->sp = nativefn + expect;
}

static force_inline bool callnative(VM* vm, ONative* native, Int argc, Int retcnt)
{
    OString* err;
    if(unlikely(!sk_ensurestack(vm, retcnt))) {
        err = RETCNT_STACK_OVERFLOW(vm, native->name->storage);
    } else if(unlikely(native->isva && native->arity > argc)) {
        err = FN_VA_ARGC_ERR(vm, native->arity, argc);
    } else if(unlikely(!native->isva && native->arity != argc)) {
        err = FN_ARGC_ERR(vm, native->arity, argc);
    } else goto callnative;
    vm->sp[-argc - 1] = OBJ_VAL(err);
    return false;
callnative:;
    int values = native->fn(vm); // perform the call
    moveresults(vm, vm->sp - argc - 1, values, retcnt);
    return values;
}

bool callv(VM* vm, Value callee, Int argc, Int retcnt)
{
    if(IS_OBJ(callee)) {
        switch(OBJ_TYPE(callee)) {
            case OBJ_BOUND_METHOD: {
                OBoundMethod* bound = AS_BOUND_METHOD(callee);
                vm->sp[-argc - 1]   = bound->receiver; // class instance (self)
                return callfn(vm, bound->method, argc, retcnt);
            }
            case OBJ_CLOSURE:
            case OBJ_FUNCTION:
                return callfn(vm, AS_CLOSURE(callee), argc, retcnt);
            case OBJ_CLASS: {
                OClass* oclass    = AS_CLASS(callee);
                vm->sp[-argc - 1] = OBJ_VAL(OInstance_new(vm, oclass));
                OClosure* init    = oclass->overloaded;
                if(init != NULL) return callfn(vm, init, argc, 1);
                else if(unlikely(argc != 0)) {
                    FN_ARGC_ERR(vm, 0, argc);
                    return false;
                } else return true;
            }
            case OBJ_NATIVE: // A C function ?
                return callnative(vm, AS_NATIVE(callee), argc, retcnt);
            default:
                break;
        }
    }
    vm->sp[-argc - 1] = OBJ_VAL(NONCALLABLE_ERR(vm, vtostr(vm, callee)->storage));
    return false;
}

static force_inline bool
invokefrom(VM* vm, OClass* oclass, Value methodname, Int argc, Int retcnt)
{
    Value method;
    if(unlikely(!HashTable_get(&oclass->methods, methodname, &method))) {
        UNDEFINED_PROPERTY_ERR(vm, AS_CSTRING(methodname), oclass->name->storage);
        return false;
    }
    return callv(vm, method, argc, retcnt);
}

static force_inline bool invokeindex(VM* vm, Value name, Int argc, Int retcnt)
{
    Value receiver = *stackpeek(argc);
    if(unlikely(!IS_INSTANCE(receiver))) {
        NOT_INSTANCE_ERR(vm, vtostr(vm, receiver)->storage);
        return false;
    }
    OInstance* instance = AS_INSTANCE(receiver);
    Value      value;
    if(HashTable_get(&instance->fields, name, &value)) {
        vm->sp[-argc - 1] = value;
        vm->sp--; // additional argument on stack ('name')
        return callv(vm, value, argc - 1, retcnt);
    }
    if(unlikely(!HashTable_get(&instance->oclass->methods, name, &value))) {
        UNDEFINED_PROPERTY_ERR(
            vm,
            vtostr(vm, name)->storage,
            instance->oclass->name->storage);
        return false;
    }
    pop(vm); // pop the name
    return callv(vm, value, argc - 1, retcnt);
}

static force_inline bool invoke(VM* vm, Value name, Int argc, Int retcnt)
{
    Value receiver = *stackpeek(argc);
    if(unlikely(!IS_INSTANCE(receiver))) {
        vm->sp[-argc - 1] = OBJ_VAL(NOT_INSTANCE_ERR(vm, vtostr(vm, receiver)->storage));
        return false;
    }
    OInstance* instance = AS_INSTANCE(receiver);
    Value      value;
    if(HashTable_get(&instance->fields, name, &value)) {
        vm->sp[-argc - 1] = value;
        return callv(vm, value, argc, retcnt);
    }
    return invokefrom(vm, instance->oclass, name, argc, retcnt);
}

static force_inline OUpvalue* captureupval(VM* vm, Value* valp)
{
    OUpvalue** upvalpp = &vm->open_upvals;
    while(*upvalpp != NULL && (*upvalpp)->location > valp)
        upvalpp = &(*upvalpp)->next;
    if(*upvalpp != NULL && (*upvalpp)->location == valp) return *upvalpp;
    OUpvalue* upvalp = OUpvalue_new(vm, valp);
    upvalp->next     = *upvalpp;
    *upvalpp         = upvalp;
    return upvalp;
}

void closeupval(VM* vm, Value* last)
{
    while(vm->open_upvals != NULL && vm->open_upvals->location >= last) {
        OUpvalue* upvalp     = vm->open_upvals;
        upvalp->closed.value = *upvalp->location;
        upvalp->location     = &upvalp->closed.value;
        vm->open_upvals      = upvalp->next;
    }
}

/* Unescape strings before printing them when ERROR occurs. */
static OString* unescape(VM* vm, OString* string)
{
    Array_Byte new;
    Array_Byte_init(&new, vm);
    Array_Byte_init_cap(&new, string->len + 1);
    for(UInt i = 0; i < string->len; i++) {
        switch(string->storage[i]) {
            case '\n':
                Array_Byte_push(&new, '\\');
                Array_Byte_push(&new, 'n');
                break;
            case '\0':
                Array_Byte_push(&new, '\\');
                Array_Byte_push(&new, '0');
                break;
            case '\a':
                Array_Byte_push(&new, '\\');
                Array_Byte_push(&new, 'a');
                break;
            case '\b':
                Array_Byte_push(&new, '\\');
                Array_Byte_push(&new, 'b');
                break;
            case '\33':
                Array_Byte_push(&new, '\\');
                Array_Byte_push(&new, 'e');
                break;
            case '\f':
                Array_Byte_push(&new, '\\');
                Array_Byte_push(&new, 'f');
                break;
            case '\r':
                Array_Byte_push(&new, '\\');
                Array_Byte_push(&new, 'r');
                break;
            case '\t':
                Array_Byte_push(&new, '\\');
                Array_Byte_push(&new, 't');
                break;
            case '\v':
                Array_Byte_push(&new, '\\');
                Array_Byte_push(&new, 'v');
                break;
            default:
                Array_Byte_push(&new, string->storage[i]);
        }
    }
    OString* unescaped = OString_new(vm, (void*)new.data, new.len);
    push(vm, OBJ_VAL(unescaped));
    Array_Byte_free(&new, NULL);
    pop(vm);
    return unescaped;
}

/**
 * Searches the entire table for the matching index in order to
 * provide more descriptive runtime error.
 * It is okay if the lookup is slow, this only gets called when runtime error
 * occurs (which is follows the end of the program execution).
 **/
static force_inline OString* globalname(VM* vm, UInt idx)
{
    for(UInt i = 0; i < vm->globids.cap; i++) {
        Entry* entry = &vm->globids.entries[i];
        if(!IS_EMPTY(entry->key) && AS_NUMBER(entry->value) == idx)
            return (OString*)AS_OBJ(entry->key);
    }
    unreachable;
}

static sdebug void dumpstack(VM* vm, CallFrame* frame, Byte* ip)
{
    printf("           ");
    for(Value* ptr = vm->stack; ptr < vm->sp; ptr++) {
        printf("[");
        vprint(*ptr);
        printf("]");
    }
    printf("\n");
    Instruction_debug(&FFN(frame)->chunk, (UInt)(ip - FFN(frame)->chunk.code.data));
}

static InterpretResult run(VM* vm)
{
#define throwerr(vm)    runerror(vm, vtostr(vm, *stackpeek(0))->storage)
#define READ_BYTE()     (*ip++)
#define READ_BYTEL()    (ip += 3, GET_BYTES3(ip - 3))
#define READ_CONSTANT() FFN(frame)->chunk.constants.data[READ_BYTEL()]
#define READ_STRING()   AS_STRING(READ_CONSTANT())
#define BINARY_OP(value_type, op)                                                        \
    do {                                                                                 \
        if(unlikely(!IS_NUMBER(*stackpeek(0)) || !IS_NUMBER(*stackpeek(1)))) {           \
            frame->ip = ip;                                                              \
            BINARYOP_ERR(vm, op);                                                        \
            return INTERPRET_RUNTIME_ERROR;                                              \
        }                                                                                \
        double b = AS_NUMBER(pop(vm));                                                   \
        double a = AS_NUMBER(pop(vm));                                                   \
        push(vm, value_type(a op b));                                                    \
    } while(false)

    runtime = 1;
    // cache these hopefully in a register
    register CallFrame* frame = &vm->frames[vm->fc - 1];
    register Byte*      ip    = frame->ip;
#ifdef DEBUG_TRACE_EXECUTION
    printf("\n=== VM - execution ===\n");
#endif
    while(true) {
#ifdef S_PRECOMPUTED_GOTO
    #define OP_TABLE
    #include "jmptable.h"
    #undef OP_TABLE
    #ifdef DEBUG_TRACE_EXECUTION
        #undef BREAK
        #define BREAK                                                                    \
            dumpstack(vm, frame, ip);                                                    \
            DISPATCH(READ_BYTE())
    #endif
#else
    #define DISPATCH(x) switch(x)
    #define CASE(label) case label:
    #ifdef DEBUG_TRACE_EXECUTION
        #define BREAK                                                                    \
            dumpstack(vm, frame, ip);                                                    \
            break
    #else
        #define BREAK break
    #endif
#endif
        DISPATCH(READ_BYTE())
        {
            CASE(OP_TRUE)
            {
                push(vm, BOOL_VAL(true));
                BREAK;
            }
            CASE(OP_FALSE)
            {
                push(vm, BOOL_VAL(false));
                BREAK;
            }
            CASE(OP_NIL)
            {
                push(vm, NIL_VAL);
                BREAK;
            }
            CASE(OP_NILN)
            {
                pushn(vm, READ_BYTEL(), NIL_VAL);
                BREAK;
            }
            CASE(OP_NEG)
            {
                Value val = *stackpeek(0);
                if(unlikely(!IS_NUMBER(val))) {
                    frame->ip = ip;
                    UNARYNEG_ERR(vm, vtostr(vm, val)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                }
                AS_NUMBER_REF(vm->sp - 1) = NUMBER_VAL(-AS_NUMBER(val));
                BREAK;
            }
            CASE(OP_ADD)
            {
                Value b = *stackpeek(0);
                Value a = *stackpeek(1);
                if(IS_NUMBER(b) && IS_NUMBER(a)) {
                    double b = AS_NUMBER(pop(vm));
                    double a = AS_NUMBER(pop(vm));
                    push(vm, NUMBER_VAL((a + b)));
                } else if(IS_STRING(b) && IS_STRING(a)) {
                    push(vm, OBJ_VAL(concatenate(vm, a, b)));
                } else {
                    frame->ip = ip;
                    ADD_OPERATOR_ERR(vm, a, b);
                    return INTERPRET_RUNTIME_ERROR;
                }
                BREAK;
            }
            CASE(OP_SUB)
            {
                BINARY_OP(NUMBER_VAL, -);
                BREAK;
            }
            CASE(OP_MUL)
            {
                BINARY_OP(NUMBER_VAL, *);
                BREAK;
            }
            CASE(OP_MOD)
            {
                TODO("Implement OP_MOD");
                BREAK;
            }
            CASE(OP_POW)
            {
                TODO("Implement OP_POW");
                BREAK;
            }
            CASE(OP_DIV)
            {
                BINARY_OP(NUMBER_VAL, /);
                BREAK;
            }
            CASE(OP_NOT)
            {
                *(vm->sp - 1) = BOOL_VAL(ISFALSEY(*stackpeek(0)));
                BREAK;
            }
            CASE(OP_VALIST)
            {
                OFunction* fn    = FFN(frame);
                UInt       vacnt = READ_BYTEL();
                if(vacnt == 0) vacnt = frame->vacnt;
                for(UInt i = 1; i <= vacnt; i++) {
                    Value* next = frame->callee + fn->arity + i;
                    push(vm, *next);
                }
                BREAK;
            }
            CASE(OP_NOT_EQUAL)
            {
                Value b = pop(vm);
                Value a = pop(vm);
                push(vm, BOOL_VAL(!veq(a, b)));
                BREAK;
            }
            {
                Value a, b;
                CASE(OP_EQUAL)
                {
                    b = pop(vm);
                    a = pop(vm);
                    goto op_equal_fin;
                }
                CASE(OP_EQ)
                {
                    b = pop(vm);
                    a = *stackpeek(0);
                op_equal_fin:
                    push(vm, BOOL_VAL(veq(a, b)));
                    BREAK;
                }
            }
            CASE(OP_GREATER)
            {
                BINARY_OP(BOOL_VAL, >);
                BREAK;
            }
            CASE(OP_GREATER_EQUAL)
            {
                BINARY_OP(BOOL_VAL, >=);
                BREAK;
            }
            CASE(OP_LESS)
            {
                BINARY_OP(BOOL_VAL, <);
                BREAK;
            }
            CASE(OP_LESS_EQUAL)
            {
                BINARY_OP(BOOL_VAL, <=);
                BREAK;
            }
            CASE(OP_POP)
            {
                pop(vm);
                BREAK;
            }
            CASE(OP_POPN)
            {
                popn(vm, READ_BYTEL());
                BREAK;
            }
            CASE(OP_CONST)
            {
                push(vm, READ_CONSTANT());
                BREAK;
            }
            CASE(OP_CALL)
            {
                Int retcnt = READ_BYTEL();
                Int argc   = vm->sp - Array_VRef_pop(&vm->callstart);
                frame->ip  = ip;
                if(unlikely(!callv(vm, *stackpeek(argc), argc, retcnt))) {
                    throwerr(vm);
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm->frames[vm->fc - 1];
                ip    = frame->ip;
                BREAK;
            }
            CASE(OP_METHOD)
            {
                Value   methodname = READ_CONSTANT();
                Value   method     = *stackpeek(0); // OFunction or OClosure
                OClass* oclass     = AS_CLASS(*stackpeek(1));
                HashTable_insert(vm, &oclass->methods, methodname, method);
                pop(vm); // pop method
                BREAK;
            }
            CASE(OP_INVOKE)
            {
                Value methodname = READ_CONSTANT();
                Int   retcnt     = READ_BYTEL();
                Int   argc       = vm->sp - Array_VRef_pop(&vm->callstart);
                frame->ip        = ip;
                if(unlikely(!invoke(vm, methodname, argc, retcnt))) {
                    throwerr(vm);
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm->frames[vm->fc - 1];
                ip    = frame->ip;
                BREAK;
            }
            CASE(OP_GET_SUPER)
            {
                Value   methodname = READ_CONSTANT();
                OClass* superclass = AS_CLASS(pop(vm));
                frame->ip          = ip;
                OBoundMethod* bound =
                    bindmethod(vm, superclass, methodname, *stackpeek(0));
                if(unlikely(bound == NULL)) {
                    throwerr(vm);
                    return INTERPRET_RUNTIME_ERROR;
                }
                vm->sp[-1] = OBJ_VAL(bound);
                BREAK;
            }
            CASE(OP_INVOKE_SUPER)
            {
                Value methodname = READ_CONSTANT();
                ASSERT(IS_CLASS(*stackpeek(0)), "superclass must be class.");
                OClass* superclass = AS_CLASS(pop(vm));
                UInt    argc       = vm->sp - Array_VRef_pop(&vm->callstart);
                Int     retcnt     = READ_BYTEL();
                frame->ip          = ip;
                if(unlikely(!invokefrom(vm, superclass, methodname, argc, retcnt))) {
                    throwerr(vm);
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm->frames[vm->fc - 1];
                ip    = frame->ip;
                BREAK;
            }
            CASE(OP_SET_PROPERTY)
            {
                Value property_name = READ_CONSTANT();
                Value receiver      = *stackpeek(1);
                if(unlikely(!IS_INSTANCE(receiver))) {
                    frame->ip = ip;
                    NOT_INSTANCE_ERR(vm, vtostr(vm, receiver)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                }
                HashTable_insert(
                    vm,
                    &AS_INSTANCE(receiver)->fields,
                    property_name,
                    *stackpeek(0));
                popn(vm, 2);
                BREAK;
            }
            CASE(OP_GET_PROPERTY)
            {
                Value property_name = READ_CONSTANT();
                Value receiver      = *stackpeek(0);
                if(unlikely(!IS_INSTANCE(receiver))) {
                    frame->ip = ip;
                    NOT_INSTANCE_ERR(vm, vtostr(vm, receiver)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                }
                OInstance* instance = AS_INSTANCE(receiver);
                Value      property;
                if(HashTable_get(&instance->fields, property_name, &property)) {
                    *(vm->sp - 1) = property;
                    BREAK;
                }
                frame->ip = ip;
                OBoundMethod* bound =
                    bindmethod(vm, instance->oclass, property_name, receiver);
                if(unlikely(bound == NULL)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                *(vm->sp - 1) = OBJ_VAL(bound);
                BREAK;
            }
            {
                // Short and long instructions have identical code
                Int bcp; // Bytecode parameter
                CASE(OP_DEFINE_GLOBAL)
                {
                    bcp = READ_BYTE();
                    goto define_global_fin;
                }
                CASE(OP_DEFINE_GLOBALL)
                {
                    bcp = READ_BYTEL();
                    goto define_global_fin;
                }
            define_global_fin:;
                {
                    if(unlikely(!veq(vm->globvals[bcp].value, EMPTY_VAL))) {
                        frame->ip = ip;
                        GLOBALVAR_REDEFINITION_ERR(vm, globalname(vm, bcp)->storage);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    vm->globvals[bcp].value = *stackpeek(0);
                    pop(vm);
                    BREAK;
                }
                CASE(OP_GET_GLOBAL)
                {
                    bcp = READ_BYTE();
                    goto get_global_fin;
                }
                CASE(OP_GET_GLOBALL)
                {
                    bcp = READ_BYTEL();
                    goto get_global_fin;
                }
            get_global_fin:;
                {
                    Variable* global = &vm->globvals[bcp];
                    if(unlikely(IS_UNDEFINED(global->value))) {
                        frame->ip = ip;
                        UNDEFINED_GLOBAL_ERR(vm, globalname(vm, bcp)->storage);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, global->value);
                    BREAK;
                }
                CASE(OP_SET_GLOBAL)
                {
                    bcp = READ_BYTE();
                    goto set_global_fin;
                }
                CASE(OP_SET_GLOBALL)
                {
                    bcp = READ_BYTEL();
                    goto set_global_fin;
                }
            set_global_fin:;
                {
                    Variable* global = &vm->globvals[bcp];
                    if(unlikely(IS_UNDEFINED(global->value))) {
                        frame->ip = ip;
                        UNDEFINED_GLOBAL_ERR(vm, globalname(vm, bcp)->storage);
                        return INTERPRET_RUNTIME_ERROR;
                    } else if(unlikely(VAR_CHECK(global, VAR_FIXED_BIT))) {
                        frame->ip     = ip;
                        OString* name = globalname(vm, bcp);
                        VARIABLE_FIXED_ERR(vm, name->len, name->storage);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    global->value = pop(vm);
                    BREAK;
                }
                CASE(OP_GET_LOCAL)
                {
                    bcp = READ_BYTE();
                    goto get_local_fin;
                }
                CASE(OP_GET_LOCALL)
                {
                    bcp = READ_BYTEL();
                    goto get_local_fin;
                }
            get_local_fin:;
                {
                    push(vm, frame->callee[bcp]);
                    BREAK;
                }
                CASE(OP_SET_LOCAL)
                {
                    bcp = READ_BYTE();
                    goto set_local_fin;
                }
                CASE(OP_SET_LOCALL)
                {
                    bcp = READ_BYTEL();
                    goto set_local_fin;
                }
            set_local_fin:;
                {
                    frame->callee[bcp] = pop(vm);
                    BREAK;
                }
                CASE(OP_TOPRET) // return from script
                {
                    HashTable_insert(
                        vm,
                        &vm->loaded,
                        OBJ_VAL(FFN(frame)->name),
                        TRUE_VAL);
                    goto ret_fin;
                }
                CASE(OP_RET) // function return
                {
                ret_fin:;
                    Int retcnt = vm->sp - Array_VRef_pop(&vm->retstart);
                    Int pushc;
                    if(frame->retcnt == 0) {
                        pushc         = 0;
                        frame->retcnt = retcnt;
                    } else pushc = frame->retcnt - retcnt;
                    if(pushc < 0) popn(vm, sabs(pushc));
                    else pushn(vm, pushc, NIL_VAL);
                    ASSERT(vm->temp.len == 0, "Temporary array must be empty.");
                    for(Int returns = frame->retcnt; returns--;) {
                        Array_Value_push(&vm->temp, *stackpeek(0));
                        pop(vm);
                    }
                    closeupval(vm, frame->callee);
                    vm->fc--;
                    if(vm->fc == 0) { // end of main script
                        popn(vm, vm->sp - vm->stack);
                        return INTERPRET_OK;
                    }
                    vm->sp = frame->callee;
                    while(vm->temp.len > 0)
                        push(vm, Array_Value_pop(&vm->temp));
                    ASSERT(vm->temp.len == 0, "Temporary array must be empty.");
                    frame = &vm->frames[vm->fc - 1];
                    ip    = frame->ip;
                    BREAK;
                }
            }
            CASE(OP_JMP_IF_FALSE)
            {
                UInt skip_offset  = READ_BYTEL();
                ip               += ((Byte)ISFALSEY(*stackpeek(0)) * skip_offset);
                BREAK;
            }
            CASE(OP_JMP_IF_FALSE_POP)
            {
                UInt skip_offset  = READ_BYTEL();
                ip               += ISFALSEY(*stackpeek(0)) * skip_offset;
                pop(vm);
                BREAK;
            }
            CASE(OP_JMP_IF_FALSE_OR_POP)
            {
                UInt skip_offset = READ_BYTEL();
                if(ISFALSEY(*stackpeek(0))) ip += skip_offset;
                else pop(vm);
                BREAK;
            }
            CASE(OP_JMP_IF_FALSE_AND_POP)
            {
                UInt skip_offset = READ_BYTEL();
                if(ISFALSEY(*stackpeek(0))) {
                    ip += skip_offset;
                    pop(vm);
                }
                BREAK;
            }
            CASE(OP_JMP)
            {
                UInt skip_offset  = READ_BYTEL();
                ip               += skip_offset;
                BREAK;
            }
            CASE(OP_JMP_AND_POP)
            {
                UInt skip_offset  = READ_BYTEL();
                ip               += skip_offset;
                pop(vm);
                BREAK;
            }
            CASE(OP_LOOP)
            {
                UInt offset  = READ_BYTEL();
                ip          -= offset;
                BREAK;
            }
            CASE(OP_CLOSURE)
            {
                OFunction* fn      = AS_FUNCTION(READ_CONSTANT());
                OClosure*  closure = OClosure_new(vm, fn);
                push(vm, OBJ_VAL(closure));
                for(UInt i = 0; i < closure->upvalc; i++) {
                    Byte local = READ_BYTE();
                    Byte flags = READ_BYTE();
                    UInt idx   = READ_BYTEL();
                    if(local) closure->upvals[i] = captureupval(vm, frame->callee + idx);
                    else closure->upvals[i] = frame->closure->upvals[idx];
                    closure->upvals[i]->closed.flags = flags;
                }
                BREAK;
            }
            CASE(OP_GET_UPVALUE)
            {
                UInt idx = READ_BYTEL();
                push(vm, *frame->closure->upvals[idx]->location);
                BREAK;
            }
            CASE(OP_SET_UPVALUE)
            {
                UInt      idx   = READ_BYTEL();
                OUpvalue* upval = frame->closure->upvals[idx];
                if(unlikely(VAR_CHECK(&upval->closed, VAR_FIXED_BIT))) {
                    frame->ip      = ip;
                    OString* gname = globalname(vm, idx);
                    VARIABLE_FIXED_ERR(vm, gname->len, gname->storage);
                    runerror(vm, "Can't assign to a variable declared as 'fixed'.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                *upval->location = pop(vm);
                BREAK;
            }
            CASE(OP_CLOSE_UPVAL)
            {
                closeupval(vm, vm->sp - 1);
                pop(vm);
                BREAK;
            }
            CASE(OP_CLOSE_UPVALN)
            {
                UInt last = READ_BYTEL();
                closeupval(vm, vm->sp - last);
                popn(vm, last);
                BREAK;
            }
            CASE(OP_CLASS)
            {
                push(vm, OBJ_VAL(OClass_new(vm, READ_STRING())));
                BREAK;
            }
            CASE(OP_INDEX)
            {
                Value receiver = *stackpeek(1);
                Value key      = *stackpeek(0);
                if(unlikely(!IS_INSTANCE(receiver))) {
                    frame->ip = ip;
                    INDEX_RECEIVER_ERR(vm, vtostr(vm, receiver)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                } else if(unlikely(!IS_STRING(key))) {
                    frame->ip = ip;
                    INVALID_INDEX_ERR(vm);
                    return INTERPRET_RUNTIME_ERROR;
                }
                // @TODO: Fix this up when overloading gets implemented
                Value      value;
                OInstance* instance = AS_INSTANCE(receiver);
                if(HashTable_get(&instance->fields, key, &value)) {
                    popn(vm, 2); // Pop key and receiver
                    push(vm, value); // Push the field value
                    BREAK;
                }
                frame->ip           = ip;
                OBoundMethod* bound = bindmethod(vm, instance->oclass, key, receiver);
                if(unlikely(bound == NULL)) return INTERPRET_RUNTIME_ERROR;
                popn(vm, 2); // Pop key and receiver
                push(vm, OBJ_VAL(bound)); // Push bound method
                BREAK;
            }
            CASE(OP_SET_INDEX)
            {
                Value receiver = *stackpeek(2);
                Value property = *stackpeek(1);
                Value field    = *stackpeek(0);
                if(unlikely(!IS_INSTANCE(receiver))) {
                    frame->ip = ip;
                    INDEX_RECEIVER_ERR(vm, vtostr(vm, receiver)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                } else if(unlikely(!IS_STRING(property))) {
                    frame->ip = ip;
                    INVALID_INDEX_ERR(vm);
                    return INTERPRET_RUNTIME_ERROR;
                }
                // @TODO: Fix this up when overloading gets implemented
                HashTable_insert(vm, &AS_INSTANCE(receiver)->fields, property, field);
                popn(vm, 3);
                push(vm, field);
                BREAK;
            }
            CASE(OP_INVOKE_INDEX)
            {
                Int retcnt = READ_BYTEL();
                Int argc   = vm->sp - Array_VRef_pop(&vm->callstart);
                frame->ip  = ip;
                if(unlikely(!invokeindex(vm, *stackpeek(argc), argc + 1, retcnt)))
                    return INTERPRET_RUNTIME_ERROR;
                frame = &vm->frames[vm->fc - 1];
                ip    = frame->ip;
                BREAK;
            }
            CASE(OP_OVERLOAD)
            {
                OClass* oclass = AS_CLASS(*stackpeek(1));
                // Right now the only thing that can be overloaded
                // is class initializer, so this parameter is not useful,
                // but if the operator overloading gets implemented
                // this will actually be index into the array of
                // overload-able methods/operators.
                Byte opn = READ_BYTE();
                UNUSED(opn);
                oclass->overloaded = AS_CLOSURE(*stackpeek(0));
                ASSERT(*ip == OP_METHOD, "Expected 'OP_METHOD'.");
                BREAK;
            }
            CASE(OP_INHERIT)
            {
                ASSERT(IS_CLASS(*stackpeek(0)), "subclass must be class.");
                OClass* subclass   = AS_CLASS(*stackpeek(0));
                Value   superclass = *stackpeek(1);
                if(unlikely(!IS_CLASS(superclass))) {
                    frame->ip = ip;
                    INHERIT_ERR(
                        vm,
                        otostr(vm, (O*)subclass)->storage,
                        vtostr(vm, superclass)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                }
                HashTable_into(vm, &AS_CLASS(superclass)->methods, &subclass->methods);
                subclass->overloaded = AS_CLASS(superclass)->overloaded;
                pop(vm); // pop subclass
                BREAK;
            }
            CASE(OP_FOREACH_PREP)
            {
                Int vars = READ_BYTEL();
                memcpy(vm->sp, stackpeek(2), 3 * sizeof(Value));
                vm->sp    += 3;
                frame->ip  = ip;
                if(unlikely(!callv(vm, *stackpeek(2), 2, vars)))
                    return INTERPRET_RUNTIME_ERROR;
                frame = &vm->frames[vm->fc - 1];
                ip    = frame->ip;
                BREAK;
            }
            CASE(OP_FOREACH)
            {
                Int vars         = READ_BYTEL();
                *stackpeek(vars) = *stackpeek(vars - 1); // cntlvar
                ASSERT(*ip == OP_JMP, "Expect 'OP_JMP'.");
                if(!IS_NIL(*stackpeek(vars))) ip += 4;
                BREAK;
            }
            CASE(OP_CALLSTART)
            {
                Array_VRef_push(&vm->callstart, vm->sp);
                BREAK;
            }
            CASE(OP_RETSTART)
            {
                Array_VRef_push(&vm->retstart, vm->sp);
                BREAK;
            }
        }
    }

    unreachable;

#undef READ_BYTE
#undef READ_BYTEL
#undef READ_CONSTANT
#undef READ_CONSTANTL
#undef READ_STRING
#undef READ_STRINGL
#undef DISPATCH
#undef CASE
#undef BREAK
#undef VM_BINARY_OP
}



InterpretResult interpret(VM* vm, const char* source, const char* path)
{
    Value     name    = OBJ_VAL(OString_new(vm, path, strlen(path)));
    OClosure* closure = compile(vm, source, name);
    if(closure == NULL) return INTERPRET_COMPILE_ERROR;
    callfn(vm, closure, 0, 1);
    return run(vm);
}

void _cleanupvm(VM** vmp)
{
    _cleanup_function((*vmp)->F);
    sk_destroy(vmp);
}
