/* Host entry of the lifted PC game: load the lifted modules, resolve imports, run the CRT DLL
   initializers and the game's entry point.

   Usage: me_pc <game Binaries folder> <folder with msvcr80.dll and msvcp80.dll> [user folder]
   The user folder (default "pc-user") receives everything the game writes. */
#include "rt.h"

#include <stdio.h>
#include <string.h>

extern const X86FnEntry x86_fns_msvcr80[], x86_fns_msvcp80[], x86_fns_game[];
extern const uint32_t x86_fns_msvcr80_count, x86_fns_msvcp80_count, x86_fns_game_count;
void rt_kernel32_register(void);
void rt_sync_register(void);
void rt_files_register(void);
void rt_user32_register(void);
void rt_d3d9_register(void);
void rt_fs_init(const char *game, const char *user);

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <game Binaries folder> <folder with msvcr80.dll/msvcp80.dll>\n", argv[0]);
        return 2;
    }
    char path[1024];
    host_install_fault_handler();
    rt_mem_init();
    rt_sync_register();
    rt_files_register();
    rt_kernel32_register();
    rt_user32_register();
    rt_d3d9_register();
    snprintf(path, sizeof path, "%s/..", argv[1]);
    rt_fs_init(path, argc > 3 ? argv[3] : "pc-user");

    snprintf(path, sizeof path, "%s/msvcr80.dll", argv[2]);
    RtModule *crt = rt_load_module(path, "msvcr80.dll", x86_fns_msvcr80, x86_fns_msvcr80_count);
    snprintf(path, sizeof path, "%s/msvcp80.dll", argv[2]);
    RtModule *cpp = rt_load_module(path, "msvcp80.dll", x86_fns_msvcp80, x86_fns_msvcp80_count);
    snprintf(path, sizeof path, "%s/MirrorsEdge.exe", argv[1]);
    RtModule *game = rt_load_module(path, "mirrorsedge.exe", x86_fns_game, x86_fns_game_count);
    rt_resolve_imports(crt);
    rt_resolve_imports(cpp);
    rt_resolve_imports(game);

    RtThread *t = rt_thread_create_main(1u << 20);
    X86Ctx *ctx = &t->ctx;

    /* DLL_PROCESS_ATTACH in load order, as the Windows loader would. */
    uint32_t args[3] = {crt->base, 1, 0};
    uint32_t r = rt_call(ctx, crt->entry, 3, args);
    rt_log("msvcr80 DllMain(PROCESS_ATTACH) = %u", r);
    args[0] = cpp->base;
    r = rt_call(ctx, cpp->entry, 3, args);
    rt_log("msvcp80 DllMain(PROCESS_ATTACH) = %u", r);

    rt_log("entering the game at %#x", game->entry);
    r = rt_call(ctx, game->entry, 0, NULL);
    rt_log("game entry returned %u", r);
    return (int)r;
}
