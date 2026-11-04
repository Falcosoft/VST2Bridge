// smoke.c - loads a Linux / macOS CLAP bridge without Wine (CI on macOS): the file loads, exports clap_entry and the
// marker, init takes the path and the factory is there. The plugin itself is not started.
//   smoke <file to load> [<path given to clap_entry.init>]
// Like a macOS host: smoke <name>.clap/Contents/MacOS/<name> <name>.clap (the host loads the binary of the bundle and
// passes the bundle's path to init).
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include "../clap/clap.h"

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: smoke <file> [<init path>]\n"); return 99; }
    const char* path = argc > 2 ? argv[2] : argv[1];
    void* m = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!m) { printf("FAIL: dlopen: %s\n", dlerror()); return 1; }
    const clap_plugin_entry_t* e = (const clap_plugin_entry_t*)dlsym(m, "clap_entry");
    if (!e || !dlsym(m, "VST2BridgeClap")) { printf("FAIL: exports clap_entry and VST2BridgeClap\n"); return 1; }
    if (!clap_version_is_compatible(e->clap_version) || !e->init(path)) { printf("FAIL: clap_entry init\n"); return 1; }
    if (!e->get_factory(CLAP_PLUGIN_FACTORY_ID) || e->get_factory("nothing")) { printf("FAIL: plugin factory\n"); return 1; }
    e->deinit();
    printf("ok: %s loads, init(%s), clap_entry and factory are there\n", argv[1], path);
    return 0;
}
