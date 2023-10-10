#include "compiler.h"
#include "hashtable.h"
#include "mem.h"
#include "object.h"
#include "skconf.h"
#include "value.h"
#ifdef DEBUG
    #include "debug.h"
#endif
#ifdef DEBUG_LOG_GC
    #include "debug.h"
    #include <stdio.h>
#endif

#include <errno.h>
#include <stdlib.h>

#define OBJ_MARK(obj)      ((obj)->otype |= (uint32_t)1)
#define OBJ_IS_MARKED(obj) ((uint32_t)(obj)->otype & (uint32_t)1)
#define OBJ_UNMARK(obj)    ((obj)->otype &= (uint32_t)~1)

// Make extern for 'mark_c_roots' function in 'compiler.c'.
void mark_obj(VM* vm, Obj* obj)
{
    if(obj == NULL || OBJ_IS_MARKED(obj)) {
        return;
    }

    OBJ_MARK(obj);
    if((obj->otype & ~1) & (OBJ_STRING | OBJ_NATIVE)) {
#ifdef DEBUG_LOG_GC
        printf("%p blacken ", (void*)obj);
        Value_print(OBJ_VAL(obj));
        printf("\n");
#endif
        return;
    }

#ifdef DEBUG_LOG_GC
    printf("%p mark ", (void*)obj);
    Value_print(OBJ_VAL(obj));
    printf("\n");
#endif
    Array_ObjRef_push(&vm->gray_stack, obj);
}

SK_INTERNAL(force_inline void) mark_value(VM* vm, Value value)
{
    if(IS_OBJ(value)) {
        mark_obj(vm, AS_OBJ(value));
    }
}


SK_INTERNAL(force_inline void) mark_globals(VM* vm)
{
    for(UInt i = 0; i < vm->global_ids.cap; i++) {
        Entry* entry = &vm->global_ids.entries[i];

        if(entry->key.type != VAL_EMPTY) {
            mark_obj(vm, AS_OBJ(entry->key));
            UInt idx = (UInt)AS_NUMBER(entry->value);
            mark_value(vm, vm->global_vals.data[idx].value);
        }
    }

    for(UInt i = 0; i < vm->global_vals.len; i++) {
        Value val = vm->global_vals.data[i].value;
        if(IS_OBJ(val)) {
            assert(OBJ_IS_MARKED(AS_OBJ(val)));
        }
    }
}

SK_INTERNAL(force_inline void) mark_stack(VM* vm)
{
    for(Value* local = vm->stack; local < vm->sp; local++) {
        mark_value(vm, *local);
    }
}

SK_INTERNAL(force_inline void) mark_frames(VM* vm)
{
    for(Int i = 0; i < vm->fc; i++) {
        if(vm->frames[i].closure == NULL) {
            mark_obj(vm, (Obj*)vm->frames[i].fn);
        } else {
            mark_obj(vm, (Obj*)vm->frames[i].closure);
        }
    }
}

SK_INTERNAL(force_inline void) mark_upvalues(VM* vm)
{
    for(ObjUpvalue* upval = vm->open_upvals; upval != NULL; upval = upval->next) {
        mark_obj(vm, (Obj*)upval);
    }
}

SK_INTERNAL(force_inline void) mark_black(VM* vm, Obj* obj)
{
#ifdef DEBUG_LOG_GC
    printf("%p blacken ", (void*)obj);
    Value_print(OBJ_VAL(obj));
    printf("\n");
#endif
    switch(obj->otype & ~1) {
        case OBJ_UPVAL:
            mark_value(vm, ((ObjUpvalue*)obj)->closed);
            break;
        case OBJ_FUNCTION: {
            ObjFunction* fn = (ObjFunction*)obj;
            mark_obj(vm, (Obj*)fn->name);
            for(UInt i = 0; i < fn->chunk.constants.len; i++) {
                mark_value(vm, fn->chunk.constants.data[i]);
            }
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)obj;
            mark_obj(vm, (Obj*)closure->fn);
            for(UInt i = 0; i < closure->fn->upvalc; i++) {
                mark_obj(vm, (Obj*)closure->upvals[i]);
            }
            break;
        }
        default:
            unreachable;
    }
}

SK_INTERNAL(force_inline void) mark_vm_roots(VM* vm)
{
    mark_stack(vm);
    mark_frames(vm);
    mark_upvalues(vm);
    mark_globals(vm);
}

SK_INTERNAL(force_inline void) remove_weak_refs(VM* vm)
{
    for(UInt i = 0; i < vm->strings.cap; i++) {
        Entry* entry = &vm->strings.entries[i];
        if(entry->key.type == VAL_OBJ && !OBJ_IS_MARKED(AS_OBJ(entry->key))) {
            HashTable_remove(&vm->strings, entry->key);
        }
    }
}

SK_INTERNAL(force_inline void) sweep(Roots* roots)
{
    Obj* previous = NULL;
    Obj* current  = roots->vm->objects;

    while(current != NULL) {
        if(OBJ_IS_MARKED(current)) {
            OBJ_UNMARK(current);
            previous = current;
            current  = current->next;
        } else {
            Obj* unreached = current;
            current        = current->next;
            if(previous != NULL) {
                previous->next = current;
            } else {
                roots->vm->objects = current;
            }

            Obj_free(roots, unreached);
        }
    }
}

unused static void gc(Roots* roots)
{
    Compiler* c  = roots->c;
    VM*       vm = roots->vm;

#ifdef DEBUG_LOG_GC
    printf("--> GC start\n");
#endif

    mark_vm_roots(vm);
#ifdef THREADED_CODE
    static const void* jmptable[] = {&&mark, &&skip};

    goto* jmptable[runtime];
mark:
    mark_c_roots(vm, c);
skip:
#else
    mark_c_roots(vm, c);
#endif
    // Blacken gray objects
    while(vm->gray_stack.len > 0) {
        mark_black(vm, Array_ObjRef_pop(&vm->gray_stack));
    }

    remove_weak_refs(vm);
    sweep(roots);

#ifdef DEBUG_LOG_GC
    printf("--> GC end\n");
#endif
}

/* Allocator that can trigger gc. */
void* gc_reallocate(unused void* roots, void* ptr, unused size_t oldc, size_t newc)
{
    if(newc > oldc) {
#ifdef DEBUG_STRESS_GC
        gc((Roots*)roots);
#endif
    }
    return reallocate(ptr, newc);
}

/* Allocator that never triggers gc. */
void* reallocate(void* ptr, size_t newc)
{
    if(newc == 0) {
        free(ptr);
        return NULL;
    }

    void* alloc = realloc(ptr, newc);

    if(alloc == NULL) {
        fprintf(stderr, "Internal error, allocation failure!\n");
        exit(errno);
    }

    return alloc;
}
