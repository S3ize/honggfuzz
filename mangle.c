/*
 *
 * honggfuzz - run->dynfile->datafer mangling routines
 * -----------------------------------------
 *
 * Author:
 * Robert Swiecki <swiecki@google.com>
 *
 * Copyright 2010-2018 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include "mangle.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "input.h"
#include "libhfcommon/common.h"
#include "libhfcommon/log.h"
#include "libhfcommon/util.h"

/* Maximum reasonable block size for many types of mutations (but not for all) */
#define HF_MAX_LEN_BLOCK 512U

static inline size_t mangle_LenLeft(run_t* run, size_t off) {
    if (off >= run->dynfile->size) {
        LOG_F("Offset is too large: off:%zu >= len:%zu", off, run->dynfile->size);
    }
    return (run->dynfile->size - off - 1);
}

/* Get a random value between <1:max> with x^2 distribution */
static inline size_t mangle_getLen(size_t max) {
    if (max > _HF_INPUT_MAX_SIZE) {
        LOG_F("max (%zu) > _HF_INPUT_MAX_SIZE (%zu)", max, (size_t)_HF_INPUT_MAX_SIZE);
    }
    if (max == 0) {
        LOG_F("max == 0");
    }
    if (max == 1) {
        return 1;
    }

    const uint64_t max2 = (uint64_t)max * max;
    const uint64_t max3 = (uint64_t)max * max * max;
    const uint64_t rnd = util_rndGet(1, max2 - 1);

    uint64_t ret = rnd * rnd;
    ret /= max3;
    ret += 1;

    if (ret < 1) {
        LOG_F("ret (%" PRIu64 ") < 1, max:%zu, rnd:%" PRIu64, ret, max, rnd);
    }
    if (ret > max) {
        LOG_F("ret (%" PRIu64 ") > max (%zu), rnd:%" PRIu64, ret, max, rnd);
    }

    return (size_t)ret;
}

/* Prefer smaller values here, so use mangle_getLen() */
static inline size_t mangle_getOffSet(run_t* run) {
    return mangle_getLen(run->dynfile->size) - 1;
}

static inline void mangle_Move(run_t* run, size_t off_from, size_t off_to, size_t len) {
    if (off_from >= run->dynfile->size) {
        return;
    }
    if (off_to >= run->dynfile->size) {
        return;
    }

    size_t len_from = run->dynfile->size - off_from;
    len = HF_MIN(len, len_from);

    size_t len_to = run->dynfile->size - off_to;
    len = HF_MIN(len, len_to);

    memmove(&run->dynfile->data[off_to], &run->dynfile->data[off_from], len);
}

static inline void mangle_Overwrite(
    run_t* run, size_t off, const uint8_t* src, size_t len, bool printable) {
    if (len == 0) {
        return;
    }
    size_t maxToCopy = run->dynfile->size - off;
    if (len > maxToCopy) {
        len = maxToCopy;
    }

    memmove(&run->dynfile->data[off], src, len);
    if (printable) {
        util_turnToPrintable(&run->dynfile->data[off], len);
    }
}

static inline size_t mangle_Inflate(run_t* run, size_t off, size_t len, bool printable) {
    if (run->dynfile->size >= run->global->mutate.maxInputSz) {
        return 0;
    }
    if (len > (run->global->mutate.maxInputSz - run->dynfile->size)) {
        len = run->global->mutate.maxInputSz - run->dynfile->size;
    }

    input_setSize(run, run->dynfile->size + len);
    mangle_Move(run, off, off + len, run->dynfile->size);
    if (printable) {
        memset(&run->dynfile->data[off], ' ', len);
    }

    return len;
}

static inline void mangle_Insert(
    run_t* run, size_t off, const uint8_t* val, size_t len, bool printable) {
    len = mangle_Inflate(run, off, len, printable);
    mangle_Overwrite(run, off, val, len, printable);
}

static void mangle_MemCopyOverwrite(run_t* run, bool printable HF_ATTR_UNUSED) {
    size_t off_from = mangle_getOffSet(run);
    size_t off_to = mangle_getOffSet(run);
    size_t len = mangle_getLen(HF_MIN(HF_MAX_LEN_BLOCK, run->dynfile->size - off_from));

    mangle_Overwrite(run, off_to, &run->dynfile->data[off_from], len, printable);
}

static void mangle_MemCopyInsert(run_t* run, bool printable) {
    size_t off_to = mangle_getOffSet(run);
    size_t off_from = mangle_getOffSet(run);
    size_t len = mangle_getLen(HF_MIN(HF_MAX_LEN_BLOCK, run->dynfile->size - off_from));

    mangle_Insert(run, off_to, &run->dynfile->data[off_from], len, printable);
}

static void mangle_BytesOverwrite(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);

    uint16_t buf;
    if (printable) {
        util_rndBufPrintable((uint8_t*)&buf, sizeof(buf));
    } else {
        buf = util_rnd64();
    }

    /* Overwrite with random 1-2-byte values */
    size_t toCopy = util_rndGet(1, 2);
    mangle_Overwrite(run, off, (uint8_t*)&buf, toCopy, printable);
}

static void mangle_BytesInsert(run_t* run, bool printable) {
    uint16_t buf;
    if (printable) {
        util_rndBufPrintable((uint8_t*)&buf, sizeof(buf));
    } else {
        buf = util_rnd64();
    }

    size_t off = mangle_getOffSet(run);
    /* Insert random 1-2-byte values */
    size_t toCopy = util_rndGet(1, 2);
    mangle_Insert(run, off, (uint8_t*)&buf, toCopy, printable);
}

static void mangle_ByteRepeatOverwrite(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);
    size_t destOff = off + 1;
    size_t maxSz = run->dynfile->size - destOff;

    /* No space to repeat */
    if (!maxSz) {
        mangle_BytesOverwrite(run, printable);
        return;
    }

    size_t len = mangle_getLen(HF_MIN(HF_MAX_LEN_BLOCK, maxSz));
    memset(&run->dynfile->data[destOff], run->dynfile->data[off], len);
}

static void mangle_ByteRepeatInsert(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);
    size_t destOff = off + 1;
    size_t maxSz = run->dynfile->size - destOff;

    /* No space to repeat */
    if (!maxSz) {
        mangle_BytesInsert(run, printable);
        return;
    }

    size_t len = mangle_getLen(HF_MIN(HF_MAX_LEN_BLOCK, maxSz));
    len = mangle_Inflate(run, destOff, len, printable);
    memset(&run->dynfile->data[destOff], run->dynfile->data[off], len);
}

static void mangle_Bit(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);
    run->dynfile->data[off] ^= (uint8_t)(1U << util_rndGet(0, 7));
    if (printable) {
        util_turnToPrintable(&(run->dynfile->data[off]), 1);
    }
}

static const struct {
    const uint8_t val[8];
    const size_t size;
} mangleMagicVals[] = {
    /* 1B - No endianness */
    {"\x00\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x01\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x02\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x03\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x04\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x05\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x06\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x07\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x08\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x09\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x0A\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x0B\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x0C\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x0D\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x0E\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x0F\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x10\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x20\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x40\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x7E\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x7F\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x80\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\x81\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\xC0\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\xFE\x00\x00\x00\x00\x00\x00\x00", 1},
    {"\xFF\x00\x00\x00\x00\x00\x00\x00", 1},
    /* 2B - NE */
    {"\x00\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x01\x01\x00\x00\x00\x00\x00\x00", 2},
    {"\x80\x80\x00\x00\x00\x00\x00\x00", 2},
    {"\xFF\xFF\x00\x00\x00\x00\x00\x00", 2},
    /* 2B - BE */
    {"\x00\x01\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x02\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x03\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x04\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x05\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x06\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x07\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x08\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x09\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x0A\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x0B\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x0C\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x0D\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x0E\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x0F\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x10\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x20\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x40\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x7E\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x7F\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x80\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x81\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\xC0\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\xFE\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\xFF\x00\x00\x00\x00\x00\x00", 2},
    {"\x7E\xFF\x00\x00\x00\x00\x00\x00", 2},
    {"\x7F\xFF\x00\x00\x00\x00\x00\x00", 2},
    {"\x80\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x80\x01\x00\x00\x00\x00\x00\x00", 2},
    {"\xFF\xFE\x00\x00\x00\x00\x00\x00", 2},
    /* 2B - LE */
    {"\x00\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x01\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x02\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x03\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x04\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x05\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x06\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x07\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x08\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x09\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x0A\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x0B\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x0C\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x0D\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x0E\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x0F\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x10\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x20\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x40\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x7E\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x7F\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x80\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\x81\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\xC0\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\xFE\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\xFF\x00\x00\x00\x00\x00\x00\x00", 2},
    {"\xFF\x7E\x00\x00\x00\x00\x00\x00", 2},
    {"\xFF\x7F\x00\x00\x00\x00\x00\x00", 2},
    {"\x00\x80\x00\x00\x00\x00\x00\x00", 2},
    {"\x01\x80\x00\x00\x00\x00\x00\x00", 2},
    {"\xFE\xFF\x00\x00\x00\x00\x00\x00", 2},
    /* 4B - NE */
    {"\x00\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x01\x01\x01\x01\x00\x00\x00\x00", 4},
    {"\x80\x80\x80\x80\x00\x00\x00\x00", 4},
    {"\xFF\xFF\xFF\xFF\x00\x00\x00\x00", 4},
    /* 4B - BE */
    {"\x00\x00\x00\x01\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x02\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x03\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x04\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x05\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x06\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x07\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x08\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x09\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x0A\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x0B\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x0C\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x0D\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x0E\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x0F\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x10\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x20\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x40\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x7E\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x7F\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x80\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x81\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\xC0\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\xFE\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\xFF\x00\x00\x00\x00", 4},
    {"\x7E\xFF\xFF\xFF\x00\x00\x00\x00", 4},
    {"\x7F\xFF\xFF\xFF\x00\x00\x00\x00", 4},
    {"\x80\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x80\x00\x00\x01\x00\x00\x00\x00", 4},
    {"\xFF\xFF\xFF\xFE\x00\x00\x00\x00", 4},
    /* 4B - LE */
    {"\x00\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x01\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x02\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x03\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x04\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x05\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x06\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x07\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x08\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x09\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x0A\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x0B\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x0C\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x0D\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x0E\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x0F\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x10\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x20\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x40\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x7E\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x7F\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x80\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\x81\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\xC0\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\xFE\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\xFF\x00\x00\x00\x00\x00\x00\x00", 4},
    {"\xFF\xFF\xFF\x7E\x00\x00\x00\x00", 4},
    {"\xFF\xFF\xFF\x7F\x00\x00\x00\x00", 4},
    {"\x00\x00\x00\x80\x00\x00\x00\x00", 4},
    {"\x01\x00\x00\x80\x00\x00\x00\x00", 4},
    {"\xFE\xFF\xFF\xFF\x00\x00\x00\x00", 4},
    /* 8B - NE */
    {"\x00\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x01\x01\x01\x01\x01\x01\x01\x01", 8},
    {"\x80\x80\x80\x80\x80\x80\x80\x80", 8},
    {"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8},
    /* 8B - BE */
    {"\x00\x00\x00\x00\x00\x00\x00\x01", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x02", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x03", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x04", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x05", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x06", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x07", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x08", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x09", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x0A", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x0B", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x0C", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x0D", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x0E", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x0F", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x10", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x20", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x40", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x7E", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x7F", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x80", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x81", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\xC0", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\xFE", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\xFF", 8},
    {"\x7E\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8},
    {"\x7F\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8},
    {"\x80\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x80\x00\x00\x00\x00\x00\x00\x01", 8},
    {"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFE", 8},
    /* 8B - LE */
    {"\x00\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x01\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x02\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x03\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x04\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x05\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x06\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x07\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x08\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x09\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x0A\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x0B\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x0C\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x0D\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x0E\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x0F\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x10\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x20\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x40\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x7E\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x7F\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x80\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\x81\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\xC0\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\xFE\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\xFF\x00\x00\x00\x00\x00\x00\x00", 8},
    {"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x7E", 8},
    {"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x7F", 8},
    {"\x00\x00\x00\x00\x00\x00\x00\x80", 8},
    {"\x01\x00\x00\x00\x00\x00\x00\x80", 8},
    {"\xFE\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8},
};

static void mangle_MagicOverwrite(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);
    uint64_t choice = util_rndGet(0, ARRAYSIZE(mangleMagicVals) - 1);
    mangle_Overwrite(
        run, off, mangleMagicVals[choice].val, mangleMagicVals[choice].size, printable);
}

static void mangle_MagicInsert(run_t* run, bool printable) {
    uint64_t choice = util_rndGet(0, ARRAYSIZE(mangleMagicVals) - 1);
    size_t off = mangle_getOffSet(run);
    mangle_Insert(run, off, mangleMagicVals[choice].val, mangleMagicVals[choice].size, printable);
}

static void mangle_DictionaryOverwrite(run_t* run, bool printable) {
    if (run->global->mutate.dictionaryCnt == 0) {
        mangle_BytesOverwrite(run, printable);
        return;
    }
    size_t off = mangle_getOffSet(run);
    uint64_t choice = util_rndGet(0, run->global->mutate.dictionaryCnt - 1);
    mangle_Overwrite(run, off, run->global->mutate.dictionary[choice].val,
        run->global->mutate.dictionary[choice].len, printable);
}

static void mangle_DictionaryInsert(run_t* run, bool printable) {
    if (run->global->mutate.dictionaryCnt == 0) {
        mangle_BytesInsert(run, printable);
        return;
    }
    uint64_t choice = util_rndGet(0, run->global->mutate.dictionaryCnt - 1);
    size_t off = mangle_getOffSet(run);
    mangle_Insert(run, off, run->global->mutate.dictionary[choice].val,
        run->global->mutate.dictionary[choice].len, printable);
}

static inline const uint8_t* mangle_FeedbackDict(run_t* run, size_t* len) {
    if (!run->global->feedback.cmpFeedback) {
        return NULL;
    }
    cmpfeedback_t* cmpf = run->global->feedback.cmpFeedbackMap;
    uint32_t cnt = ATOMIC_GET(cmpf->cnt);
    if (cnt == 0) {
        return NULL;
    }
    if (cnt > ARRAYSIZE(cmpf->valArr)) {
        cnt = ARRAYSIZE(cmpf->valArr);
    }
    uint32_t choice = util_rndGet(0, cnt - 1);
    *len = (size_t)ATOMIC_GET(cmpf->valArr[choice].len);
    if (*len == 0) {
        return NULL;
    }
    return cmpf->valArr[choice].val;
}

static void mangle_ConstFeedbackInsert(run_t* run, bool printable) {
    size_t len;
    const uint8_t* val = mangle_FeedbackDict(run, &len);
    if (val == NULL) {
        mangle_BytesInsert(run, printable);
        return;
    }
    size_t off = mangle_getOffSet(run);
    mangle_Insert(run, off, val, len, printable);
}

static void mangle_ConstFeedbackOverwrite(run_t* run, bool printable) {
    size_t len;
    const uint8_t* val = mangle_FeedbackDict(run, &len);
    if (val == NULL) {
        mangle_BytesOverwrite(run, printable);
        return;
    }
    size_t off = mangle_getOffSet(run);
    mangle_Overwrite(run, off, val, len, printable);
}

static void mangle_MemSet(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);
    size_t len = mangle_getLen(HF_MIN(HF_MAX_LEN_BLOCK, run->dynfile->size - off));
    int val = printable ? (int)util_rndPrintable() : (int)util_rndGet(0, UINT8_MAX);

    memset(&run->dynfile->data[off], val, len);
}

static void mangle_RandomOverwrite(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);
    size_t len = mangle_getLen(HF_MIN(HF_MAX_LEN_BLOCK, run->dynfile->size - off));
    if (printable) {
        util_rndBufPrintable(&run->dynfile->data[off], len);
    } else {
        util_rndBuf(&run->dynfile->data[off], len);
    }
}

static void mangle_RandomInsert(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);
    size_t len = mangle_getLen(HF_MIN(HF_MAX_LEN_BLOCK, run->dynfile->size - off));

    len = mangle_Inflate(run, off, len, printable);

    if (printable) {
        util_rndBufPrintable(&run->dynfile->data[off], len);
    } else {
        util_rndBuf(&run->dynfile->data[off], len);
    }
}

static inline void mangle_AddSubWithRange(
    run_t* run, size_t off, size_t varLen, uint64_t range, bool printable) {
    int64_t delta = (int64_t)util_rndGet(0, range * 2) - (int64_t)range;

    switch (varLen) {
        case 1: {
            run->dynfile->data[off] += delta;
            break;
        }
        case 2: {
            int16_t val;
            memcpy(&val, &run->dynfile->data[off], sizeof(val));
            if (util_rnd64() & 0x1) {
                val += delta;
            } else {
                /* Foreign endianess */
                val = __builtin_bswap16(val);
                val += delta;
                val = __builtin_bswap16(val);
            }
            mangle_Overwrite(run, off, (uint8_t*)&val, varLen, printable);
            break;
        }
        case 4: {
            int32_t val;
            memcpy(&val, &run->dynfile->data[off], sizeof(val));
            if (util_rnd64() & 0x1) {
                val += delta;
            } else {
                /* Foreign endianess */
                val = __builtin_bswap32(val);
                val += delta;
                val = __builtin_bswap32(val);
            }
            mangle_Overwrite(run, off, (uint8_t*)&val, varLen, printable);
            break;
        }
        case 8: {
            int64_t val;
            memcpy(&val, &run->dynfile->data[off], sizeof(val));
            if (util_rnd64() & 0x1) {
                val += delta;
            } else {
                /* Foreign endianess */
                val = __builtin_bswap64(val);
                val += delta;
                val = __builtin_bswap64(val);
            }
            mangle_Overwrite(run, off, (uint8_t*)&val, varLen, printable);
            break;
        }
        default: {
            LOG_F("Unknown variable length size: %zu", varLen);
        }
    }
}

static void mangle_AddSub(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);

    /* 1,2,4,8 */
    size_t varLen = 1U << util_rndGet(0, 3);
    if ((run->dynfile->size - off) < varLen) {
        varLen = 1;
    }

    uint64_t range;
    switch (varLen) {
        case 1:
            range = 16;
            break;
        case 2:
            range = 4096;
            break;
        case 4:
            range = 1048576;
            break;
        case 8:
            range = 268435456;
            break;
        default:
            LOG_F("Invalid operand size: %zu", varLen);
    }

    mangle_AddSubWithRange(run, off, varLen, range, printable);
}

static void mangle_IncByte(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);
    if (printable) {
        run->dynfile->data[off] = (run->dynfile->data[off] - 32 + 1) % 95 + 32;
    } else {
        run->dynfile->data[off] += (uint8_t)1UL;
    }
}

static void mangle_DecByte(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);
    if (printable) {
        run->dynfile->data[off] = (run->dynfile->data[off] - 32 + 94) % 95 + 32;
    } else {
        run->dynfile->data[off] -= (uint8_t)1UL;
    }
}

static void mangle_NegByte(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);
    if (printable) {
        run->dynfile->data[off] = 94 - (run->dynfile->data[off] - 32) + 32;
    } else {
        run->dynfile->data[off] = ~(run->dynfile->data[off]);
    }
}

static void mangle_Expand(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);
    size_t len;
    if (util_rnd64() % 16) {
        len = mangle_getLen(HF_MIN(16, run->global->mutate.maxInputSz - off));
    } else {
        len = mangle_getLen(run->global->mutate.maxInputSz - off);
    }

    mangle_Inflate(run, off, len, printable);
}

static void mangle_Shrink(run_t* run, bool printable HF_ATTR_UNUSED) {
    if (run->dynfile->size <= 2U) {
        return;
    }

    size_t off_start = mangle_getOffSet(run);
    size_t len = mangle_LenLeft(run, off_start);
    if (len == 0) {
        return;
    }
    if (util_rnd64() % 16) {
        len = mangle_getLen(HF_MIN(16, len));
    } else {
        len = mangle_getLen(len);
    }
    size_t off_end = off_start + len;
    size_t len_to_move = run->dynfile->size - off_end;

    mangle_Move(run, off_end, off_start, len_to_move);
    input_setSize(run, run->dynfile->size - len);
}
static void mangle_ASCIINumOverwrite(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);
    size_t len = util_rndGet(2, 8);

    char buf[20];
    snprintf(buf, sizeof(buf), "%-19" PRId64, (int64_t)util_rnd64());

    mangle_Overwrite(run, off, (const uint8_t*)buf, len, printable);
}

static void mangle_ASCIINumInsert(run_t* run, bool printable) {
    size_t off = mangle_getOffSet(run);
    size_t len = util_rndGet(2, 8);

    char buf[20];
    snprintf(buf, sizeof(buf), "%-19" PRId64, (int64_t)util_rnd64());

    mangle_Insert(run, off, (const uint8_t*)buf, len, printable);
}

static void mangle_SpliceOverwrite(run_t* run, bool printable) {
    const uint8_t* buf;
    size_t sz = input_getRandomInputAsBuf(run, &buf);
    if (!sz) {
        mangle_BytesOverwrite(run, printable);
        return;
    }

    size_t remoteOff = mangle_getLen(sz) - 1;
    size_t localOff = mangle_getOffSet(run);
    size_t len = mangle_getLen(HF_MIN(sz - remoteOff, run->dynfile->size - localOff));
    mangle_Overwrite(run, localOff, &buf[remoteOff], len, printable);
}

static void mangle_SpliceInsert(run_t* run, bool printable) {
    const uint8_t* buf;
    size_t sz = input_getRandomInputAsBuf(run, &buf);
    if (!sz) {
        mangle_BytesInsert(run, printable);
        return;
    }

    size_t remoteOff = mangle_getLen(sz) - 1;
    size_t localOff = mangle_getOffSet(run);
    size_t len = mangle_getLen(HF_MIN(sz - remoteOff, run->dynfile->size - localOff));
    mangle_Insert(run, localOff, &buf[remoteOff], len, printable);
}

static void mangle_Resize(run_t* run, bool printable) {
    ssize_t oldsz = run->dynfile->size;
    ssize_t newsz = 0;

    uint64_t choice = util_rndGet(0, 32);
    switch (choice) {
        case 0: /* Set new size arbitrarily */
            newsz = (ssize_t)util_rndGet(1, run->global->mutate.maxInputSz);
            break;
        case 1 ... 4: /* Increase size by a small value */
            newsz = oldsz + (ssize_t)util_rndGet(0, 8);
            break;
        case 5: /* Increase size by a larger value */
            newsz = oldsz + (ssize_t)util_rndGet(9, 128);
            break;
        case 6 ... 9: /* Decrease size by a small value */
            newsz = oldsz - (ssize_t)util_rndGet(0, 8);
            break;
        case 10: /* Decrease size by a larger value */
            newsz = oldsz - (ssize_t)util_rndGet(9, 128);
            break;
        case 11 ... 32: /* Do nothing */
            newsz = oldsz;
            break;
        default:
            LOG_F("Illegal value from util_rndGet: %" PRIu64, choice);
            break;
    }
    if (newsz < 1) {
        newsz = 1;
    }
    if (newsz > (ssize_t)run->global->mutate.maxInputSz) {
        newsz = run->global->mutate.maxInputSz;
    }

    input_setSize(run, (size_t)newsz);
    if (newsz > oldsz) {
        if (printable) {
            memset(&run->dynfile->data[oldsz], ' ', newsz - oldsz);
        }
    }
}

void mangle_mangleContent(run_t* run, unsigned slow_factor) {
    static void (*const mangleFuncs[])(run_t * run, bool printable) = {
        /* Every *Insert or Expand expands file, so add more Shrink's */
        mangle_Shrink,
        mangle_Shrink,
        mangle_Shrink,
        mangle_Shrink,
        mangle_Expand,
        mangle_Bit,
        mangle_IncByte,
        mangle_DecByte,
        mangle_NegByte,
        mangle_AddSub,
        mangle_MemSet,
        mangle_MemCopyOverwrite,
        mangle_MemCopyInsert,
        mangle_BytesOverwrite,
        mangle_BytesInsert,
        mangle_ASCIINumOverwrite,
        mangle_ASCIINumInsert,
        mangle_ByteRepeatOverwrite,
        mangle_ByteRepeatInsert,
        mangle_MagicOverwrite,
        mangle_MagicInsert,
        mangle_DictionaryOverwrite,
        mangle_DictionaryInsert,
        mangle_ConstFeedbackOverwrite,
        mangle_ConstFeedbackInsert,
        mangle_RandomOverwrite,
        mangle_RandomInsert,
        mangle_SpliceOverwrite,
        mangle_SpliceInsert,
    };

    if (run->mutationsPerRun == 0U) {
        return;
    }
    if (run->dynfile->size == 0U) {
        mangle_Resize(run, /* printable= */ run->global->cfg.only_printable);
    }

    uint64_t changesCnt = run->global->mutate.mutationsPerRun;
    /* Give it a good shake-up, if it's a slow input */
    switch (slow_factor) {
        case 0 ... 2:
            changesCnt = util_rndGet(1, run->global->mutate.mutationsPerRun);
            break;
        case 3 ... 4:
            changesCnt = HF_MAX(run->global->mutate.mutationsPerRun, 5);
            break;
        case 5 ... 9:
            changesCnt = HF_MAX(run->global->mutate.mutationsPerRun, 7);
            break;
        default:
            changesCnt = HF_MAX(run->global->mutate.mutationsPerRun, 10);
            break;
    }

    if ((util_timeNowMillis() - ATOMIC_GET(run->global->timing.lastCovUpdate)) > 1000) {
        switch (util_rnd64() % 3) {
            case 0:
                mangle_SpliceOverwrite(run, run->global->cfg.only_printable);
                break;
            case 1:
                mangle_SpliceInsert(run, run->global->cfg.only_printable);
                break;
            default:
                break;
        }
    }

    for (uint64_t x = 0; x < changesCnt; x++) {
        uint64_t choice = util_rndGet(0, ARRAYSIZE(mangleFuncs) - 1);
        mangleFuncs[choice](run, /* printable= */ run->global->cfg.only_printable);
    }

    wmb();
}
