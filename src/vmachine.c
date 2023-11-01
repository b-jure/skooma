#include "array.h"
#include "chunk.h"
#include "common.h"
#include "compiler.h"
#include "core.h"
#include "debug.h"
#include "err.h"
#include "hash.h"
#include "mem.h"
#include "object.h"
#include "skconf.h"
#include "skmath.h"
#include "value.h"
#include "vmachine.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define stack_peek(top) (*((vm)->sp - ((top) + 1)))
#define stack_reset(vm) (vm)->sp = (vm)->stack
#define stack_size(vm)  ((vm)->sp - (vm)->stack)

Int runtime = 0;

void VM_error(VM* vm, const char* errfmt, ...)
{
    fputs("\n======= runtime error =======\n", stderr);
    va_list ap;
    va_start(ap, errfmt);
    vfprintf(stderr, errfmt, ap);
    va_end(ap);
    putc('\n', stderr);

    for(Int i = vm->fc - 1; i >= 0; i--) {
        CallFrame* frame = &vm->frames[i];
        Chunk*     chunk = &frame->fn->chunk;
        UInt       line  = Chunk_getline(chunk, frame->ip - chunk->code.data - 1);

        fprintf(stderr, "[line: %u] in ", line);

        if(frame->fn->name != NULL) {
            fprintf(stderr, "%s()\n", frame->fn->name->storage);
        } else {
            fprintf(stderr, "script\n");
        }
    }

    stack_reset(vm);
}

void VM_push(VM* vm, Value val)
{
    if(likely(vm->sp - vm->stack < VM_STACK_MAX)) {
        *vm->sp++ = val;
    } else {
        RUNTIME_INTERNAL_STACK_OVERFLOW_ERR(vm, VM_STACK_MAX);
        exit(EXIT_FAILURE);
    }
}

Value VM_pop(VM* vm)
{
    return *--vm->sp;
}

SK_INTERNAL(force_inline void) VM_popn(VM* vm, UInt n)
{
    vm->sp -= n;
}

SK_INTERNAL(force_inline Byte) concat_bool(bool boolean, char** dest)
{
    if(boolean) {
        *dest = "true";
        return sizeof("true") - 1;
    }
    *dest = "false";
    return sizeof("false") - 1;
}

SK_INTERNAL(force_inline Byte) concat_nil(char** str)
{
    *str = "nil";
    return sizeof("nil") - 1;
}

SK_INTERNAL(ObjString*) concatenate(VM* vm, Value a, Value b)
{
    ObjString* left  = AS_STRING(a);
    ObjString* right = AS_STRING(b);

    size_t length = left->len + right->len;
    char   buffer[length + 1];
    memcpy(buffer, left->storage, left->len);
    memcpy(buffer + left->len, right->storage, right->len);
    buffer[length] = '\0';

    ObjString* string = ObjString_from(vm, buffer, length);

    VM_pop(vm); // GC
    VM_pop(vm); // GC

    return string;
}

SK_INTERNAL(force_inline ObjBoundMethod*)
VM_bind_method(VM* vm, ObjClass* cclass, Value name, Value receiver)
{
    Value method;
    if(unlikely(!HashTable_get(&cclass->methods, name, &method))) {
        RUNTIME_INSTANCE_PROPERTY_ERR(vm, AS_CSTRING(name), cclass->name->storage);
        return NULL;
    }

    ObjBoundMethod* bound_method = ObjBoundMethod_new(vm, receiver, AS_OBJ(method)); // GC
    return bound_method;
}

SK_INTERNAL(force_inline void)
VM_define_native(VM* vm, const char* name, NativeFn native, UInt arity)
{
    VM_push(vm, OBJ_VAL(ObjString_from(vm, name, strlen(name))));
    VM_push(vm, OBJ_VAL(ObjNative_new(vm, AS_STRING(stack_peek(0)), native, arity)));

    UInt idx = GARRAY_PUSH(vm, ((Global){vm->stack[1], false})); // GC
    HashTable_insert(vm, &vm->globids, vm->stack[0],
                     NUMBER_VAL((double)idx)); // GC

    VM_pop(vm);
    VM_pop(vm);
}

void Config_init(Config* config)
{
    config->reallocate        = reallocate;
    config->userdata          = NULL;
    config->gc_init_heap_size = 10 * (1 << 20); // 10 MiB
    config->gc_min_heap_size  = (1 << 20);      // 1 MiB
    config->gc_grow_factor    = GC_HEAP_GROW_FACTOR;
}

VM* VM_new(Config* config)
{
    AllocatorFn allocate = reallocate;
    void*       userdata = NULL;

    if(config != NULL) {
        userdata = config->userdata;
        allocate = config->reallocate ? config->reallocate : reallocate;
    }

    VM* vm = allocate(NULL, sizeof(VM), userdata);

    if(config != NULL) {
        memcpy(&vm->config, config, sizeof(Config));
        // Make sure we have a valid allocator
        vm->config.reallocate = allocate;
    } else {
        Config_init(&vm->config);
    }

    vm->fc           = 0;
    vm->objects      = NULL;
    vm->open_upvals  = NULL;
    vm->gc_allocated = 0;
    vm->gc_next      = (1 << 20); // 1 MiB
    vm->gc_flags     = 0;
    stack_reset(vm);

    HashTable_init(&vm->loaded);        // Loaded scripts (filenames)
    HashTable_init(&vm->globids);       // Global variable identifiers (GC)
    GARRAY_INIT(vm);                    // Global values array (GC)
    HashTable_init(&vm->strings);       // Interned strings table (Weak_refs)
    Array_ObjRef_init(&vm->gray_stack); // Gray stack (NO GC)

    for(UInt i = 0; i < SS_SIZE; i++) {
        vm->statics[i] = NULL;
        vm->statics[i] = ObjString_from(vm, static_str[i].name, static_str[i].len);
    }

    // Native function definitions
    VM_define_native(vm, "clock", native_clock, 0);         // GC
    VM_define_native(vm, "isfield", native_isfield, 2);     // GC
    VM_define_native(vm, "printl", native_printl, 1);       // GC
    VM_define_native(vm, "print", native_print, 1);         // GC
    VM_define_native(vm, "tostr", native_tostr, 1);         // GC
    VM_define_native(vm, "isstr", native_isstr, 1);         // GC
    VM_define_native(vm, "strlen", native_strlen, 1);       // GC
    VM_define_native(vm, "strpat", native_strpat, 2);       // GC
    VM_define_native(vm, "strsub", native_strsub, 3);       // GC
    VM_define_native(vm, "gcfactor", native_gcfactor, 1);   // GC
    VM_define_native(vm, "gcmode", native_gcmode, 1);       // GC
    VM_define_native(vm, "gccollect", native_gccollect, 0); // GC
    VM_define_native(vm, "gcleft", native_gcleft, 0);       // GC
    VM_define_native(vm, "gcusage", native_gcusage, 0);     // GC
    VM_define_native(vm, "gcnext", native_gcnext, 0);       // GC
    VM_define_native(vm, "gcset", native_gcset, 1);         // GC
    VM_define_native(vm, "gcisauto", native_gcisauto, 0);   // GC
    VM_define_native(vm, "assert", native_assert, 1);       // GC
    VM_define_native(vm, "assertf", native_assertf, 2);     // GC
    VM_define_native(vm, "error", native_error, 1);         // GC
    return vm;
}

/**
 * 'obj' is either a function or closure.
 * 'argc' is argument count.
 * 'init' marks if the function is invoked as class initializer
 * 'debug' is the receiver in case call is invoked on method otherwise NULL.
 **/
#define CALL_FN_OR_CLOSURE(vm, obj, argc, init, debug)                                             \
    ({                                                                                             \
        bool ret;                                                                                  \
        if(Obj_type(obj) == OBJ_CLOSURE) {                                                         \
            ObjClosure* closure = (ObjClosure*)(obj);                                              \
            ret                 = VM_call_fn(vm, closure, closure->fn, argc, init, debug);         \
        } else {                                                                                   \
            ObjFunction* fn = (ObjFunction*)obj;                                                   \
            ret             = VM_call_fn(vm, NULL, fn, argc, init, debug);                         \
        }                                                                                          \
        ret;                                                                                       \
    })

SK_INTERNAL(bool)
VM_call_fn(VM* vm, ObjClosure* closure, ObjFunction* fn, Int argc, bool init, ObjClass* debug)
{
    if(unlikely(!init && vm->statics[SS_INIT] == fn->name)) {
        RUNTIME_EXPLICIT_INIT_CALL(vm);
        return false;
    } else if(unlikely((Int)fn->arity != argc)) {
        if(debug != NULL) {
            ObjString* debugstr = Obj_to_str(vm, (Obj*)debug);
            VM_push(vm, OBJ_VAL(debugstr));
            RUNTIME_INSTANCE_ARGC_ERR(
                vm,
                debugstr->storage,
                Obj_to_str(vm, (Obj*)fn)->storage,
                fn->arity,
                argc);
            VM_pop(vm);
        } else {
            RUNTIME_ARGC_ERR(vm, Obj_to_str(vm, (Obj*)fn)->storage, fn->arity, argc);
        }
        return false;
    } else if(unlikely(vm->fc == VM_FRAMES_MAX)) {
        RUNTIME_INTERNAL_FRAME_LIMIT_ERR(vm, VM_FRAMES_MAX);
        return false;
    }

    CallFrame* frame = &vm->frames[vm->fc++];
    frame->closure   = closure;
    frame->fn        = fn;
    frame->ip        = fn->chunk.code.data;
    frame->sp        = vm->sp - argc - 1;

    return true;
}

SK_INTERNAL(force_inline bool) VM_call_native(VM* vm, ObjNative* native, Int argc)
{
    if(unlikely(native->arity != argc)) {
        RUNTIME_ARGC_ERR(vm, Obj_to_str(vm, (Obj*)native)->storage, native->arity, argc);
        return false;
    }

    if(likely(native->fn(vm, vm->sp - argc))) {
        vm->sp -= argc;
        return true;
    } else {
        VM_error(vm, AS_CSTRING(vm->sp[-argc - 1]));
        return false;
    }
}

SK_INTERNAL(force_inline bool) VM_call_instance(VM* vm, ObjClass* cclass, Int argc)
{
    vm->sp[-argc - 1] = OBJ_VAL(ObjInstance_new(vm, cclass));
    Obj* init         = cclass->overloaded;
    if(init != NULL) {
        return CALL_FN_OR_CLOSURE(vm, init, argc, true, cclass);
    } else if(unlikely(argc != 0)) {
        RUNTIME_INSTANCE_INIT_ARGC_ERR(vm, Obj_to_str(vm, (Obj*)cclass)->storage, argc);
        return false;
    }
    return true;
}

SK_INTERNAL(force_inline bool) VM_call_val(VM* vm, Value fnval, Int argc)
{
    if(IS_OBJ(fnval)) {
        switch(OBJ_TYPE(fnval)) {
            case OBJ_BOUND_METHOD: {
                ObjBoundMethod* bound = AS_BOUND_METHOD(fnval);
                vm->sp[-argc - 1]     = bound->receiver; // class instance (self)
                return CALL_FN_OR_CLOSURE(
                    vm,
                    bound->method,
                    argc,
                    false,
                    AS_INSTANCE(bound->receiver)->cclass);
            }
            case OBJ_FUNCTION:
                return VM_call_fn(vm, NULL, AS_FUNCTION(fnval), argc, false, NULL);
            case OBJ_CLOSURE: {
                ObjClosure* closure = AS_CLOSURE(fnval);
                return VM_call_fn(vm, closure, closure->fn, argc, false, NULL);
            }
            case OBJ_CLASS:
                return VM_call_instance(vm, AS_CLASS(fnval), argc);
            case OBJ_NATIVE:
                return VM_call_native(vm, AS_NATIVE(fnval), argc);
            default:
                break;
        }
    }


    RUNTIME_NONCALLABLE_ERR(vm, Value_to_str(vm, fnval)->storage);
    return false;
}

SK_INTERNAL(force_inline bool)
VM_invoke_from_class(VM* vm, ObjClass* cclass, Value method_name, Int argc)
{
    Value method;
    if(unlikely(!HashTable_get(&cclass->methods, method_name, &method))) {
        RUNTIME_INSTANCE_PROPERTY_ERR(vm, AS_CSTRING(method_name), cclass->name->storage);
        return false;
    }
    return VM_call_val(vm, method, argc);
}

SK_INTERNAL(force_inline bool) VM_invoke_index(VM* vm, Value name, Int argc)
{
    Value receiver = stack_peek(argc);

    if(unlikely(!IS_INSTANCE(receiver))) {
        RUNTIME_INSTANCE_ERR(vm, Value_to_str(vm, receiver)->storage);
        return false;
    }

    ObjInstance* instance = AS_INSTANCE(receiver);

    Value value;
    if(HashTable_get(&instance->fields, name, &value)) {
        vm->sp[-argc - 1] = value;
        vm->sp--; // Remember we have additional argument on stack ('name')
        return VM_call_val(vm, value, argc - 1);
    }

    if(unlikely(!HashTable_get(&instance->cclass->methods, name, &value))) {
        // Convert hash into string in case index is
        // not a string or a positive integer (whose fractional part is 0)
        char optional[30] = {0};
        optional[0]       = '\0';
        if(IS_BOOL(name) || (IS_NUMBER(name) && skfloor(AS_NUMBER(name)))) {
            dbl_to_str_generic(Value_hash(name), optional, 30);
        }
        RUNTIME_INSTANCE_PROPERTY_ERR(
            vm,
            optional[0] != '\0' ? optional : Value_to_str(vm, name)->storage,
            instance->cclass->name->storage);
        return false;
    }

    VM_pop(vm); // pop the name
    return VM_call_val(vm, value, argc - 1);
}

SK_INTERNAL(force_inline bool) VM_invoke(VM* vm, Value name, Int argc)
{
    Value receiver = stack_peek(argc);

    if(unlikely(!IS_INSTANCE(receiver))) {
        RUNTIME_INSTANCE_ERR(vm, Value_to_str(vm, receiver)->storage);
        return false;
    }

    ObjInstance* instance = AS_INSTANCE(receiver);

    Value value;
    if(HashTable_get(&instance->fields, name, &value)) {
        vm->sp[-argc - 1] = value;
        return VM_call_val(vm, value, argc);
    }

    return VM_invoke_from_class(vm, instance->cclass, name, argc);
}


// Bit confusing but keep in mind that pp is a double pointer to 'ObjUpValue'.
// Think about it in a way that 'pp' holds the memory location of 'UpValue->next'
// excluding the first iteration where it holds list head.
// So the 'next' field is what we are holding onto, then when we dereference
// the 'pp' we automatically dereference the 'next' field of the previous 'UpValue'.
// This basically inserts new ObjUpvalue into the vm->open_upvals singly linked list
// (in reverse stack order head:high -> tail:low) or it returns already
// inserted/existing Upvalue.
SK_INTERNAL(force_inline ObjUpvalue*) VM_capture_upval(VM* vm, Value* var_ref)
{
    ObjUpvalue** pp = &vm->open_upvals;

    while(*pp != NULL && (*pp)->location > var_ref) {
        pp = &(*pp)->next;
    }

    // If pointers are the same we already captured
    if(*pp != NULL && (*pp)->location == var_ref) {
        return *pp;
    }

    ObjUpvalue* upval = ObjUpvalue_new(vm, var_ref);
    upval->next       = *pp;
    *pp               = upval;

    return upval;
}

SK_INTERNAL(force_inline void) VM_close_upval(VM* vm, Value* last)
{
    while(vm->open_upvals != NULL && vm->open_upvals->location >= last) {
        // This is where closing happens, stack values
        // get new 'location' (this 'Obj').
        // Meaning when GC triggers they will get marked
        // because open_upvals is considered as a root.
        ObjUpvalue* upval = vm->open_upvals;
        upval->closed     = *upval->location;
        upval->location   = &upval->closed;
        vm->open_upvals   = upval->next;
    }
}

/* Unescape strings before printing them when error occurs. */
SK_INTERNAL(force_inline ObjString*) unescape(VM* vm, ObjString* string)
{
    Array_Byte new;
    Array_Byte_init(&new);
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

    ObjString* unescaped = ObjString_from(vm, (void*)new.data, new.len);
    Array_Byte_free(&new, NULL);
    return unescaped;
}

/**
 * Searches the entire table for the matching index in order to
 * provide more precise runtime error output.
 * It is okay if the lookup is slow, this only gets called when runtime error occurs.
 **/
SK_INTERNAL(force_inline ObjString*) VM_find_glob_name(VM* vm, UInt idx)
{
    for(UInt i = 0; i < vm->globids.cap; i++) {
        Entry* entry = &vm->globids.entries[i];
        if(!IS_EMPTY(entry->key) && AS_NUMBER(entry->value) == idx) {
            return (ObjString*)AS_OBJ(entry->key);
        }
    }
    unreachable;
}

SK_INTERNAL(InterpretResult) VM_run(VM* vm)
{
#define READ_BYTE()      (*ip++)
#define READ_BYTEL()     (ip += 3, GET_BYTES3(ip - 3))
#define READ_CONSTANT()  frame->fn->chunk.constants[READ_BYTE()]
#define READ_CONSTANTL() frame->fn->chunk.constants[READ_BYTEL()]
#define READ_STRING()    AS_STRING(READ_CONSTANT())
#define READ_STRINGL()   AS_STRING(READ_CONSTANTL())
#define BINARY_OP(value_type, op)                                                                  \
    do {                                                                                           \
        if(unlikely(!IS_NUMBER(stack_peek(0)) || !IS_NUMBER(stack_peek(1)))) {                     \
            frame->ip = ip;                                                                        \
            VM_error(vm, "Operands must be numbers (operator '" #op "').");                        \
            return INTERPRET_RUNTIME_ERROR;                                                        \
        }                                                                                          \
        double b = AS_NUMBER(VM_pop(vm));                                                          \
        double a = AS_NUMBER(VM_pop(vm));                                                          \
        VM_push(vm, value_type(a op b));                                                           \
    } while(false)

#define CONCAT_OR_ADD(vm)                                                                          \
    do {                                                                                           \
        Value b = stack_peek(0);                                                                   \
        Value a = stack_peek(1);                                                                   \
        if(IS_NUMBER(b) && IS_NUMBER(a)) {                                                         \
            double b = AS_NUMBER(VM_pop(vm));                                                      \
            double a = AS_NUMBER(VM_pop(vm));                                                      \
            VM_push(vm, NUMBER_VAL((a + b)));                                                      \
        } else if(IS_STRING(b) && IS_STRING(a)) {                                                  \
            VM_push(vm, OBJ_VAL(concatenate(vm, a, b)));                                           \
        } else {                                                                                   \
            ObjString* astr = Value_to_str(vm, a);                                                 \
            VM_push(vm, OBJ_VAL(astr));                                                            \
            ObjString* bstr = Value_to_str(vm, b);                                                 \
            VM_push(vm, OBJ_VAL(bstr));                                                            \
            ObjString* unescaped_b = unescape(vm, bstr);                                           \
            VM_pop(vm); /* pop 'bstr' */                                                           \
            VM_push(vm, OBJ_VAL(unescaped_b));                                                     \
            ObjString* unescaped_a = unescape(vm, astr);                                           \
            VM_pop(vm); /* pop 'astr' */                                                           \
            VM_push(vm, OBJ_VAL(unescaped_a));                                                     \
            VM_error(                                                                              \
                vm,                                                                                \
                "Only two numbers can be added together or two strings "                           \
                "concatenated.\nThis is invalid: ...\"%s\" + \"%s\"...\nTry "                      \
                "instead: "                                                                        \
                "...\"%s%s%s\" + "                                                                 \
                "\"%s%s%s\"...",                                                                   \
                unescaped_a->storage,                                                              \
                unescaped_b->storage,                                                              \
                IS_STRING(a) ? "" : "tostr(",                                                      \
                unescaped_a->storage,                                                              \
                IS_STRING(a) ? "" : ")",                                                           \
                IS_STRING(b) ? "" : "tostr(",                                                      \
                unescaped_b->storage,                                                              \
                IS_STRING(b) ? "" : ")");                                                          \
            VM_pop(vm);                                                                            \
            VM_pop(vm);                                                                            \
            return INTERPRET_RUNTIME_ERROR;                                                        \
        }                                                                                          \
    } while(false)

    // Flag for gc to skip marking compiler roots ('mem.c' -> 'gc()')
    runtime = 1;
    // First frame is 'global' frame, implicit
    // function that contains all other code and/or functions
    register CallFrame* frame = &vm->frames[vm->fc - 1];
    // Keep instruction pointer in a local variable to encourage
    // compiler to keep it in a register.
    register Byte* ip = frame->ip;

#ifdef DEBUG_TRACE_EXECUTION
    printf("\n=== vmachine ===\n");
#endif
    while(true) {
#ifdef SK_PRECOMPUTED_GOTO
    #define OP_TABLE
    #include "jmptable.h"
    #undef OP_TABLE
#else
    #define DISPATCH(x) switch(x)
    #define CASE(label) case label:
    #define BREAK       break
#endif
#ifdef DEBUG_TRACE_EXECUTION
    #undef BREAK
    #define BREAK continue
        printf("           ");
        for(Value* ptr = vm->stack; ptr < vm->sp; ptr++) {
            printf("[");
            Value_print(*ptr);
            printf("]");
        }
        printf("\n");
        Instruction_debug(&frame->fn->chunk, (UInt)(ip - frame->fn->chunk.code.data));
#endif
        DISPATCH(READ_BYTE())
        {
            CASE(OP_TRUE)
            {
                VM_push(vm, BOOL_VAL(true));
                BREAK;
            }
            CASE(OP_FALSE)
            {
                VM_push(vm, BOOL_VAL(false));
                BREAK;
            }
            CASE(OP_NIL)
            {
                VM_push(vm, NIL_VAL);
                BREAK;
            }
            CASE(OP_NEG)
            {
                Value val = stack_peek(0);
                if(unlikely(!IS_NUMBER(val))) {
                    frame->ip = ip;
                    RUNTIME_UNARY_NEGATION_ERR(vm, Value_to_str(vm, val)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                }
                AS_NUMBER_REF(vm->sp - 1) = NUMBER_VAL(-AS_NUMBER(val));
                BREAK;
            }
            CASE(OP_ADD)
            {
                frame->ip = ip;
                CONCAT_OR_ADD(vm);
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
            CASE(OP_DIV)
            {
                BINARY_OP(NUMBER_VAL, /);
                BREAK;
            }
            CASE(OP_NOT)
            {
                *(vm->sp - 1) = BOOL_VAL(ISFALSEY(stack_peek(0)));
                BREAK;
            }
            CASE(OP_NOT_EQUAL)
            {
                Value a = VM_pop(vm);
                Value b = VM_pop(vm);
                VM_push(vm, BOOL_VAL(!Value_eq(b, a)));
                BREAK;
            }
            CASE(OP_EQUAL)
            {
                Value b = VM_pop(vm);
                Value a = VM_pop(vm);
                VM_push(vm, BOOL_VAL(Value_eq(a, b)));
                BREAK;
            }
            CASE(OP_EQ)
            {
                Value b = VM_pop(vm);
                Value a = stack_peek(0);
                VM_push(vm, BOOL_VAL(Value_eq(a, b)));
                BREAK;
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
                VM_pop(vm);
                BREAK;
            }
            CASE(OP_POPN)
            {
                VM_popn(vm, READ_BYTEL());
                BREAK;
            }
            CASE(OP_CONST)
            {
                VM_push(vm, READ_CONSTANT());
                BREAK;
            }
            CASE(OP_CONSTL)
            {
                VM_push(vm, READ_CONSTANTL());
                BREAK;
            }
            CASE(OP_DEFINE_GLOBAL)
            {
                Byte idx = READ_BYTE();
                //
                vm->globvals[idx].value = stack_peek(0);
                VM_pop(vm);
                BREAK;
            }
            CASE(OP_DEFINE_GLOBALL)
            {
                UInt idx = READ_BYTEL();
                //
                vm->globvals[idx].value = stack_peek(0);
                VM_pop(vm);
                BREAK;
            }
            CASE(OP_GET_GLOBAL)
            {
                Byte    idx    = READ_BYTE();
                Global* global = &vm->globvals[idx];

                if(unlikely(IS_UNDEFINED(global->value) || IS_DECLARED(global->value))) {
                    frame->ip = ip;
                    RUNTIME_GLOBAL_UNDEFINED_ERR(vm, VM_find_glob_name(vm, idx)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                }

                VM_push(vm, global->value);
                BREAK;
            }
            CASE(OP_GET_GLOBALL)
            {
                UInt    idx    = READ_BYTEL();
                Global* global = &vm->globvals[idx];

                if(unlikely(IS_UNDEFINED(global->value) || IS_DECLARED(global->value))) {
                    frame->ip = ip;
                    RUNTIME_GLOBAL_UNDEFINED_ERR(vm, VM_find_glob_name(vm, idx)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                }

                VM_push(vm, global->value);
                BREAK;
            }
            CASE(OP_SET_GLOBAL)
            {
                Byte    idx    = READ_BYTE();
                Global* global = &vm->globvals[idx];

                if(unlikely(IS_UNDEFINED(global->value) || IS_DECLARED(global->value))) {
                    frame->ip = ip;
                    RUNTIME_GLOBAL_UNDEFINED_ERR(vm, VM_find_glob_name(vm, idx)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                } else if(unlikely(GLOB_CHECK(global, GLOB_FIXED_BIT))) {
                    frame->ip       = ip;
                    ObjString* name = VM_find_glob_name(vm, idx);
                    RUNTIME_GLOBAL_FIXED_ERR(vm, name->len, name->storage);
                    return INTERPRET_RUNTIME_ERROR;
                }

                global->value = stack_peek(0);
                BREAK;
            }
            CASE(OP_SET_GLOBALL)
            {
                UInt    idx    = READ_BYTEL();
                Global* global = &vm->globvals[idx];

                if(unlikely(IS_UNDEFINED(global->value) || IS_DECLARED(global->value))) {
                    frame->ip = ip;
                    RUNTIME_GLOBAL_UNDEFINED_ERR(vm, VM_find_glob_name(vm, idx)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                } else if(unlikely(GLOB_CHECK(global, GLOB_FIXED_BIT))) {
                    frame->ip       = ip;
                    ObjString* name = VM_find_glob_name(vm, idx);
                    RUNTIME_GLOBAL_FIXED_ERR(vm, name->len, name->storage);
                    return INTERPRET_RUNTIME_ERROR;
                }

                global->value = stack_peek(0);
                BREAK;
            }
            CASE(OP_GET_LOCAL)
            {
                Byte slot = READ_BYTE();
                VM_push(vm, frame->sp[slot]);
                BREAK;
            }
            CASE(OP_GET_LOCALL)
            {
                UInt slot = READ_BYTEL();
                VM_push(vm, frame->sp[slot]);
                BREAK;
            }
            CASE(OP_SET_LOCAL)
            {
                Byte slot       = READ_BYTE();
                frame->sp[slot] = stack_peek(0);
                BREAK;
            }
            CASE(OP_SET_LOCALL)
            {
                UInt slot       = READ_BYTEL();
                frame->sp[slot] = stack_peek(0);
                BREAK;
            }
            CASE(OP_JMP_IF_FALSE)
            {
                UInt skip_offset = READ_BYTEL();
                //
                ip += ((Byte)ISFALSEY(stack_peek(0)) * skip_offset);
                BREAK;
            }
            CASE(OP_JMP_IF_FALSE_POP)
            {
                UInt skip_offset = READ_BYTEL();
                if(ISFALSEY(stack_peek(0))) {
                    ip += skip_offset;
                }
                VM_pop(vm);
                BREAK;
            }
            CASE(OP_JMP_IF_FALSE_OR_POP)
            {
                UInt skip_offset = READ_BYTEL();
                if(ISFALSEY(stack_peek(0))) {
                    ip += skip_offset;
                } else {
                    VM_pop(vm);
                }
                BREAK;
            }
            CASE(OP_JMP_IF_FALSE_AND_POP)
            {
                UInt skip_offset = READ_BYTEL();
                if(ISFALSEY(stack_peek(0))) {
                    ip += skip_offset;
                    VM_pop(vm);
                }
                BREAK;
            }
            CASE(OP_JMP)
            {
                UInt skip_offset = READ_BYTEL();
                //
                ip += skip_offset;
                BREAK;
            }
            CASE(OP_JMP_AND_POP)
            {
                UInt skip_offset = READ_BYTEL();
                //
                ip += skip_offset;
                VM_pop(vm);
                BREAK;
            }
            CASE(OP_LOOP)
            {
                UInt offset = READ_BYTEL();
                //
                ip -= offset;
                BREAK;
            }
            CASE(OP_CALL)
            {
                Int argc  = READ_BYTE();
                frame->ip = ip;
                if(unlikely(!VM_call_val(vm, stack_peek(argc), argc))) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm->frames[vm->fc - 1];
                ip    = frame->ip;
                BREAK;
            }
            CASE(OP_CALLL)
            {
                Int argc  = READ_BYTEL();
                frame->ip = ip;
                if(unlikely(!VM_call_val(vm, stack_peek(argc), argc))) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm->frames[vm->fc - 1];
                ip    = frame->ip;
                BREAK;
            }
            CASE(OP_CLOSURE)
            {
                ObjFunction* fn      = AS_FUNCTION(READ_CONSTANTL());
                ObjClosure*  closure = ObjClosure_new(vm, fn);
                VM_push(vm, OBJ_VAL(closure));

                for(UInt i = 0; i < closure->upvalc; i++) {
                    Byte local = READ_BYTE();
                    UInt idx   = READ_BYTEL();

                    if(local) {
                        closure->upvals[i] = VM_capture_upval(vm, frame->sp + idx);
                    } else {
                        closure->upvals[i] = frame->closure->upvals[idx];
                    }
                }
                BREAK;
            }
            CASE(OP_GET_UPVALUE)
            {
                UInt idx = READ_BYTEL();
                VM_push(vm, *frame->closure->upvals[idx]->location);
                BREAK;
            }
            CASE(OP_SET_UPVALUE)
            {
                UInt idx = READ_BYTEL();
                //
                *frame->closure->upvals[idx]->location = stack_peek(0);
                BREAK;
            }
            CASE(OP_CLOSE_UPVAL)
            {
                VM_close_upval(vm, vm->sp - 1);
                VM_pop(vm);
                BREAK;
            }
            CASE(OP_CLOSE_UPVALN)
            {
                UInt last = READ_BYTEL();
                VM_close_upval(vm, vm->sp - last);
                VM_popn(vm, last);
                BREAK;
            }
            CASE(OP_CLASS)
            {
                VM_push(vm, OBJ_VAL(ObjClass_new(vm, READ_STRING())));
                BREAK;
            }
            CASE(OP_CLASSL)
            {
                VM_push(vm, OBJ_VAL(ObjClass_new(vm, READ_STRINGL())));
                BREAK;
            }
            CASE(OP_SET_PROPERTY)
            {
                Value      val           = stack_peek(1);
                ObjString* property_name = READ_STRING();

                if(unlikely(!IS_INSTANCE(val))) {
                    frame->ip = ip;
                    RUNTIME_INSTANCE_ERR(vm, Value_to_str(vm, val)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjInstance* instance = AS_INSTANCE(val);
                HashTable_insert(vm, &instance->fields, OBJ_VAL(property_name), stack_peek(0));
                Value ret = VM_pop(vm); // Get assigned value
                VM_pop(vm);             // Pop instance
                VM_push(vm, ret);       // Push back the assigned value
                BREAK;
            }
            CASE(OP_SET_PROPERTYL)
            {
                Value      val           = stack_peek(1);
                ObjString* property_name = READ_STRINGL();

                if(unlikely(!IS_INSTANCE(val))) {
                    frame->ip = ip;
                    RUNTIME_INSTANCE_ERR(vm, Value_to_str(vm, val)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjInstance* instance = AS_INSTANCE(val);
                HashTable_insert(vm, &instance->fields, OBJ_VAL(property_name), stack_peek(0));
                Value ret = VM_pop(vm); // Get assigned value
                VM_pop(vm);             // Pop instance
                VM_push(vm, ret);       // Push back the assigned value
                BREAK;
            }
            CASE(OP_GET_PROPERTY)
            {
                Value receiver = stack_peek(0);
                if(unlikely(!IS_INSTANCE(receiver))) {
                    frame->ip = ip;
                    RUNTIME_INSTANCE_ERR(vm, Value_to_str(vm, receiver)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance* instance      = AS_INSTANCE(receiver);
                Value        property_name = OBJ_VAL(READ_STRING());
                Value        property;
                if(HashTable_get(&instance->fields, property_name, &property)) {
                    VM_pop(vm);
                    VM_push(vm, property);
                    BREAK;
                }
                frame->ip = ip;
                ObjBoundMethod* bound =
                    VM_bind_method(vm, instance->cclass, property_name, receiver);
                if(unlikely(bound == NULL)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                VM_pop(vm);                  // pop instance
                VM_push(vm, OBJ_VAL(bound)); // Push bound method
                BREAK;
            }
            CASE(OP_GET_PROPERTYL)
            {
                Value receiver = stack_peek(0);
                if(unlikely(!IS_INSTANCE(receiver))) {
                    frame->ip = ip;
                    RUNTIME_INSTANCE_ERR(vm, Value_to_str(vm, receiver)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance* instance      = AS_INSTANCE(receiver);
                Value        property_name = OBJ_VAL(READ_STRINGL());
                Value        property;
                if(HashTable_get(&instance->fields, property_name, &property)) {
                    VM_pop(vm);
                    VM_push(vm, property);
                    BREAK;
                }
                frame->ip = ip;
                ObjBoundMethod* bound =
                    VM_bind_method(vm, instance->cclass, property_name, receiver);
                if(unlikely(bound == NULL)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                VM_pop(vm);                  // pop instance
                VM_push(vm, OBJ_VAL(bound)); // Push bound method
                BREAK;
            }
            CASE(OP_INDEX)
            {
                Value receiver = stack_peek(1);
                Value key      = stack_peek(0);

                if(unlikely(!IS_INSTANCE(receiver))) {
                    frame->ip = ip;
                    RUNTIME_INDEX_RECEIVER_ERR(vm);
                    return INTERPRET_RUNTIME_ERROR;
                } else if(unlikely(IS_NIL(key))) {
                    frame->ip = ip;
                    RUNTIME_INDEX_NIL_ERR(vm);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value        value;
                ObjInstance* instance = AS_INSTANCE(receiver);

                if(HashTable_get(&instance->fields, key, &value)) {
                    VM_popn(vm, 2);     // Pop key and receiver
                    VM_push(vm, value); // Push the field value
                    BREAK;
                }

                frame->ip = ip;

                ObjBoundMethod* bound = VM_bind_method(vm, instance->cclass, key, receiver);
                if(unlikely(bound == NULL)) {
                    return INTERPRET_RUNTIME_ERROR;
                }

                VM_popn(vm, 2);              // Pop key and receiver
                VM_push(vm, OBJ_VAL(bound)); // Push bound method
                BREAK;
            }
            CASE(OP_SET_INDEX)
            {
                Value receiver = stack_peek(2);
                Value property = stack_peek(1);
                Value field    = stack_peek(0);

                if(unlikely(!IS_INSTANCE(receiver))) {
                    frame->ip = ip;
                    RUNTIME_INDEX_RECEIVER_ERR(vm);
                    return INTERPRET_RUNTIME_ERROR;
                } else if(unlikely(IS_NIL(property))) {
                    frame->ip = ip;
                    RUNTIME_INDEX_NIL_ERR(vm);
                    return INTERPRET_RUNTIME_ERROR;
                }

                HashTable_insert(vm, &AS_INSTANCE(receiver)->fields, property, field);
                VM_popn(vm, 3);
                VM_push(vm, field);
                BREAK;
            }
            CASE(OP_INVOKE_INDEX)
            {
                Int argc  = READ_BYTEL();
                frame->ip = ip;
                if(unlikely(!VM_invoke_index(vm, stack_peek(argc), argc + 1))) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm->frames[vm->fc - 1];
                ip    = frame->ip;
                BREAK;
            }
            CASE(OP_METHOD)
            {
                Value      method      = stack_peek(0); // Function or closure
                ObjClass*  cclass      = AS_CLASS(stack_peek(1));
                ObjString* method_name = READ_STRING();

                HashTable_insert(vm, &cclass->methods, OBJ_VAL(method_name),
                                 method); // GC

                VM_pop(vm); // pop the method (function/closure)
                BREAK;
            }
            CASE(OP_METHODL)
            {
                Value      method      = stack_peek(0); // Function or closure
                ObjClass*  cclass      = AS_CLASS(stack_peek(1));
                ObjString* method_name = READ_STRINGL();

                HashTable_insert(vm, &cclass->methods, OBJ_VAL(method_name),
                                 method); // GC

                VM_pop(vm); // pop the method (function/closure)
                BREAK;
            }
            CASE(OP_INVOKE)
            {
                ObjString* method_name = READ_STRING();
                Int        argc        = READ_BYTEL();
                frame->ip              = ip;
                if(unlikely(!VM_invoke(vm, OBJ_VAL(method_name), argc))) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm->frames[vm->fc - 1];
                ip    = frame->ip;
                BREAK;
            }
            CASE(OP_INVOKEL)
            {
                ObjString* method_name = READ_STRINGL();
                Int        argc        = READ_BYTEL();
                frame->ip              = ip;
                if(unlikely(!VM_invoke(vm, OBJ_VAL(method_name), argc))) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm->frames[vm->fc - 1];
                ip    = frame->ip;
                BREAK;
            }
            CASE(OP_OVERLOAD)
            {
                // Note: Do not pop anything, next instruction is OP_METHOD
                ObjClass* cclass = AS_CLASS(stack_peek(1));
                // Right now the only thing that can be overloaded
                // is class initializer, so this is not useful,
                // but if the operator overloading gets implemented
                // this will actually be index into the array of
                // overload-able methods/operators.
                Byte opn = READ_BYTE();
                UNUSED(opn);

                cclass->overloaded = AS_OBJ(stack_peek(0));
                BREAK;
            }
            CASE(OP_INHERIT)
            {
                // Safety: It is safe to cast 'subclass' as ObjClass,
                // compiler emits OP_INHERIT only when compiling a class.
                ObjClass* subclass   = AS_CLASS(stack_peek(0));
                Value     superclass = stack_peek(1);

                if(unlikely(!IS_CLASS(superclass))) {
                    frame->ip = ip;
                    RUNTIME_INHERIT_ERR(
                        vm,
                        Obj_to_str(vm, (Obj*)subclass)->storage, // no need to VM_push
                                                                 // classes already have names
                        Value_to_str(vm, superclass)->storage);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // Impl all the methods
                HashTable_into(vm, &AS_CLASS(superclass)->methods, &subclass->methods);

                // Update the overloaded methods/operators cache
                // (only initializer method overload-able for now)
                subclass->overloaded = AS_CLASS(superclass)->overloaded;

                VM_pop(vm);
                BREAK;
            }
            CASE(OP_GET_SUPER)
            {
                Value     name        = OBJ_VAL(READ_STRING());
                ObjClass* superclass  = AS_CLASS(VM_pop(vm));
                frame->ip             = ip;
                ObjBoundMethod* bound = VM_bind_method(vm, superclass, name, stack_peek(0));
                if(unlikely(bound == NULL)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                vm->sp[-1] = OBJ_VAL(bound); // Replace instance with bound method
                BREAK;
            }
            CASE(OP_GET_SUPERL)
            {
                Value     name        = OBJ_VAL(READ_STRINGL());
                ObjClass* superclass  = AS_CLASS(VM_pop(vm));
                frame->ip             = ip;
                ObjBoundMethod* bound = VM_bind_method(vm, superclass, name, stack_peek(0));
                if(unlikely(bound == NULL)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                vm->sp[-1] = OBJ_VAL(bound); // Replace instance with bound method
                BREAK;
            }
            CASE(OP_INVOKE_SUPER)
            {
                ObjClass* superclass  = AS_CLASS(VM_pop(vm));
                Value     method_name = OBJ_VAL(READ_STRING());
                Int       argc        = READ_BYTEL();
                frame->ip             = ip;

                if(unlikely(!VM_invoke_from_class(vm, superclass, method_name, argc))) {
                    return INTERPRET_RUNTIME_ERROR;
                }

                frame = &vm->frames[vm->fc - 1];
                ip    = frame->ip;
                BREAK;
            }
            CASE(OP_INVOKE_SUPERL)
            {
                ObjClass* superclass  = AS_CLASS(VM_pop(vm));
                Value     method_name = OBJ_VAL(READ_STRINGL());
                Int       argc        = READ_BYTEL();
                frame->ip             = ip;

                if(unlikely(!VM_invoke_from_class(vm, superclass, method_name, argc))) {
                    return INTERPRET_RUNTIME_ERROR;
                }

                frame = &vm->frames[vm->fc - 1];
                ip    = frame->ip;
                BREAK;
            }
            CASE(OP_RET)
            {
                // @FIX: repl
                Value retval = VM_pop(vm);
                VM_close_upval(vm, frame->sp);
                vm->fc--;
                if(vm->fc == 0) {
                    VM_pop(vm);
                    return INTERPRET_OK;
                }
                vm->sp = frame->sp;
                VM_push(vm, retval);
                frame = &vm->frames[vm->fc - 1];
                ip    = frame->ip;
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
#undef VM_CONCAT_OR_ADD
}

InterpretResult VM_interpret(VM* vm, const char* source)
{
    ObjFunction* fn = compile(vm, source);

    if(fn == NULL) {
        return INTERPRET_COMPILE_ERROR;
    }

    VM_push(vm, OBJ_VAL(fn));
    VM_call_fn(vm, NULL, fn, 0, false, NULL);

    return VM_run(vm);
}

void VM_free(VM* vm)
{
    HashTable_free(vm, &vm->loaded);
    HashTable_free(vm, &vm->globids);
    GARRAY_FREE(vm);
    HashTable_free(vm, &vm->strings);
    Array_ObjRef_free(&vm->gray_stack, NULL);

    Obj* next;
    for(Obj* head = vm->objects; head != NULL; head = next) {
        next = Obj_next(head);
        Obj_free(vm, head);
    }

    vm->config.reallocate(vm, 0, vm->config.userdata);
}
