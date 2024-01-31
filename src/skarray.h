/* ----------------------------------------------------------------------------------------------
 * Copyright (C) 2023-2024 Jure Bagić
 *
 * This file is part of Skooma.
 * Skooma is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Skooma is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Skooma.
 * If not, see <https://www.gnu.org/licenses/>.
 * ----------------------------------------------------------------------------------------------*/

#ifndef SKARRAY_H
#define SKARRAY_H

#include "skcommon.h"

#include <errno.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>

#define ARRAY_INITIAL_SIZE 8

#define GROW_ARRAY_CAPACITY(cap, initial_size) ((cap) < (initial_size) ? (initial_size) : (cap) * 2)


/* GRAY STACK ARRAY (vm.h) */
#define GSARRAY_INIT(vm)                                                                           \
    do {                                                                                           \
        (vm)->gray_stack = NULL;                                                                   \
        (vm)->gslen = 0;                                                                           \
        (vm)->gscap = 0;                                                                           \
    } while(false)

#define GSARRAY_PUSH(vm, objref)                                                                   \
    ({                                                                                             \
        if((vm)->gscap <= (vm)->gslen) {                                                           \
            if(unlikely((vm)->gscap >= VM_GRAYSTACK_LIMIT)) {                                      \
                fprintf(                                                                           \
                    stderr,                                                                        \
                    "[%s:%d] Internal error, gray stack capacity exceeded! [gslimit -> "           \
                    "%lu]\n",                                                                      \
                    __FILE__,                                                                      \
                    __LINE__,                                                                      \
                    (VM_GRAYSTACK_LIMIT >> 1));                                                    \
                if((vm)->hooks.panic) (vm)->hooks.panic(vm);                                       \
                abort();                                                                           \
            }                                                                                      \
            Int oldcap = (vm)->gscap;                                                              \
            (vm)->gscap =                                                                          \
                MIN(GROW_ARRAY_CAPACITY((Int)oldcap, ARRAY_INITIAL_SIZE), VM_GRAYSTACK_LIMIT);     \
            (vm)->gray_stack = cast(O**, realloc((vm)->gray_stack, (vm)->gscap * sizeof(O*)));     \
        }                                                                                          \
        (vm)->gray_stack[(vm)->gslen++] = objref;                                                  \
        (vm)->gslen - 1;                                                                           \
    })

#define GSARRAY_POP(vm)                                                                            \
    ({                                                                                             \
        O* obj = NULL;                                                                             \
        if((vm)->gslen > 0) obj = (vm)->gray_stack[--(vm)->gslen];                                 \
        obj;                                                                                       \
    })

#define GSARRAY_FREE(vm)                                                                           \
    do {                                                                                           \
        free((vm)->gray_stack);                                                                    \
        GSARRAY_INIT(vm);                                                                          \
    } while(false)





/*
 * [======== GENERIC ARRAY =========]
 */


/* These are internal only macros. */
#define _ARRAY_METHOD_NAME(tname, name) tname##_##name
#define _CALL_ARRAY_METHOD(tname, name, ...)                                                       \
    _ARRAY_METHOD_NAME(tname, name)(self __VA_OPT__(, ) __VA_ARGS__)
#define _ARRAY_METHOD(tname, name, ...)                                                            \
    _ARRAY_METHOD_NAME(tname, name)                                                                \
    (tname * self __VA_OPT__(, ) __VA_ARGS__)


typedef void (*FreeFn)(void* value);

/* Create new 'name' array with 'type' elements */
#define ARRAY_NEW(name, type)                                                                      \
    typedef struct {                                                                               \
        size_t cap;                                                                                \
        size_t len;                                                                                \
        type* data;                                                                                \
        VM* vm;                                                                                    \
    } name;                                                                                        \
                                                                                                   \
    static force_inline void _ARRAY_METHOD(name, init, VM* vmachine)                               \
    {                                                                                              \
        self->cap = 0;                                                                             \
        self->len = 0;                                                                             \
        self->data = NULL;                                                                         \
        self->vm = vmachine;                                                                       \
    }                                                                                              \
                                                                                                   \
    static force_inline void _ARRAY_METHOD(name, init_cap, uint32_t cap)                           \
    {                                                                                              \
        self->data = (type*)gcrealloc(self->vm, self->data, 0, cap * sizeof(type));                \
        self->cap = cap;                                                                           \
    }                                                                                              \
                                                                                                   \
    static force_inline void _ARRAY_METHOD(name, grow)                                             \
    {                                                                                              \
        size_t old_cap = self->cap;                                                                \
        self->cap = MIN(GROW_ARRAY_CAPACITY(old_cap, ARRAY_INITIAL_SIZE), SK_BYTECODE_MAX + 1);    \
        if(unlikely(self->cap >= SK_BYTECODE_MAX + 1)) {                                           \
            fprintf(                                                                               \
                stderr,                                                                            \
                "[%s:%d] Internal error, %s capacity exceeded! [capmax -> "                        \
                "%d]\n",                                                                           \
                __FILE__,                                                                          \
                __LINE__,                                                                          \
                #name,                                                                             \
                SK_BYTECODE_MAX);                                                                  \
            abort();                                                                               \
        } else {                                                                                   \
            self->data = (type*)                                                                   \
                gcrealloc(self->vm, self->data, old_cap * sizeof(type), self->cap * sizeof(type)); \
        }                                                                                          \
    }                                                                                              \
                                                                                                   \
    static force_inline UInt _ARRAY_METHOD(name, push, type value)                                 \
    {                                                                                              \
        if(self->cap <= self->len) _CALL_ARRAY_METHOD(name, grow);                                 \
        self->data[self->len++] = value;                                                           \
        return self->len - 1;                                                                      \
    }                                                                                              \
                                                                                                   \
    static force_inline type _ARRAY_METHOD(name, pop)                                              \
    {                                                                                              \
        return self->data[--self->len];                                                            \
    }                                                                                              \
                                                                                                   \
    static force_inline type* _ARRAY_METHOD(name, index, size_t index)                             \
    {                                                                                              \
        return &self->data[index];                                                                 \
    }                                                                                              \
                                                                                                   \
    static force_inline type* _ARRAY_METHOD(name, last)                                            \
    {                                                                                              \
        return &self->data[self->len - 1];                                                         \
    }                                                                                              \
                                                                                                   \
    static force_inline type* _ARRAY_METHOD(name, first)                                           \
    {                                                                                              \
        return &self->data[0];                                                                     \
    }                                                                                              \
                                                                                                   \
    static force_inline void _ARRAY_METHOD(name, insert, size_t index, type value)                 \
    {                                                                                              \
        type* src = self->data + index;                                                            \
        type* dest = src + 1;                                                                      \
        memmove(dest, src, self->len - index);                                                     \
        self->len++;                                                                               \
        self->data[index] = value;                                                                 \
    }                                                                                              \
                                                                                                   \
    static force_inline type _ARRAY_METHOD(name, remove, size_t index)                             \
    {                                                                                              \
        if(self->len == 1) return _CALL_ARRAY_METHOD(name, pop);                                   \
        type* src = self->data + index;                                                            \
        type* dest = src - 1;                                                                      \
        type retval = *src;                                                                        \
        memmove(dest, src, self->len - index);                                                     \
        self->len--;                                                                               \
        return retval;                                                                             \
    }                                                                                              \
                                                                                                   \
    static force_inline void _ARRAY_METHOD(name, ensure, unsigned int len)                         \
    {                                                                                              \
        while(self->cap < self->len + len)                                                         \
            _CALL_ARRAY_METHOD(name, grow);                                                        \
    }                                                                                              \
                                                                                                   \
    static force_inline void _ARRAY_METHOD(name, free, FreeFn fn)                                  \
    {                                                                                              \
        if(fn != NULL)                                                                             \
            for(UInt i = 0; i < self->len; i++)                                                    \
                fn((void*)&self->data[i]);                                                         \
        if(self->data != NULL) gcfree(self->vm, self->data, self->cap, 0);                         \
    }

#endif