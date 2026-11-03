/* Tables a generated module exports to the runtime (pc/tools/gen_module.py). */
#ifndef X86RT_MODULE_H
#define X86RT_MODULE_H

#include "x86rt.h"

typedef struct X86FnEntry {
    uint32_t addr; /* guest address of the function entry */
    X86Fn fn;
} X86FnEntry;

#endif
