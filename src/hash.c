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
 *
 *
 * xxHash Library
 * Copyright (c) 2012-2021 Yann Collet
 * All rights reserved.
 *
 * BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 *  - Redistributions in binary form must reproduce the above copyright notice, this
 *    list of conditions and the following disclaimer in the documentation and/or
 *    other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ----------------------------------------------------------------------------------------------*/

#include "common.h"
#include "hash.h"
#include "skmath.h"
#include "xxhash.h"

#define HASH_INF 314159

/* Hash double */
Hash dblhash(double dbl)
{
    if(sk_isinf(dbl) || sk_isnan(dbl))
        return (dbl > 0) ? cast_hash(HASH_INF) : cast_hash(-HASH_INF);
    union {
        double value;
        uint32_t ints[2];
    } bitcast;
    bitcast.value = (dbl) + 1.0;
    return bitcast.ints[0] + bitcast.ints[1];
}

/* Hash string (xxHash), strings get special hash. */
Hash stringhash(const char* str, size_t len, unsigned long seed)
{
    return XXH64(str, len, seed);
}

/* Hash pointer (for objects except strings) */
Hash ptrhash(const void* ptr)
{
    uintptr_t x = (uintptr_t)ptr;
    // https://github.com/python/cpython/blob/3375dfed400494ba5cc1b744d52f6fb8b7796059/Include/internal/pycore_pyhash.h#L10
    x = (x >> 4) | (x << (8 * sizeof(uintptr_t) - 4));
    return cast_hash(x);
}
