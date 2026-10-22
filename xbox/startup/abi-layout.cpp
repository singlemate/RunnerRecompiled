#include <cstddef>
#include <cstdio>
#ifdef ME_REX_CONTEXT
#include <rex/ppc/context.h>
#else
#include "ppc_context.h"
#endif
int main(){
 printf("{\"size\":%zu,\"alignment\":%zu",sizeof(PPCContext),alignof(PPCContext));
#define FIELD(n) printf(",\"" #n "\":%zu",offsetof(PPCContext,n));
 FIELD(r3) FIELD(r0) FIELD(r1) FIELD(r31) FIELD(lr) FIELD(ctr) FIELD(xer) FIELD(cr0) FIELD(fpscr) FIELD(f0) FIELD(f31) FIELD(v0) FIELD(v127)
#ifdef ME_REX_CONTEXT
 FIELD(vscr_sat) FIELD(last_indirect_target)
#else
 FIELD(vscr)
#endif
 printf("}\n");
}
#ifdef ME_VERIFY_LAYOUT
static_assert(sizeof(PPCContext)==2688);
static_assert(offsetof(PPCContext,r1)==16);
static_assert(offsetof(PPCContext,v0)==592);
#ifdef ME_REX_CONTEXT
static_assert(offsetof(PPCContext,f0)==336);
static_assert(offsetof(PPCContext,vscr_sat)==328);
#else
static_assert(offsetof(PPCContext,f0)==328);
static_assert(offsetof(PPCContext,vscr)==2640);
#endif
#endif
