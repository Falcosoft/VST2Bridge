// smoke.c - loads a Linux / macOS CLAP bridge without Wine (CI on macOS): the file loads, exports clap_entry and the
// marker, init takes the path and the factory is there. The plugin itself is not started.
//   smoke <name>.clap | <name>.clap/Contents/MacOS/<name>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include "../clap/clap.h"

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: smoke <bridge.clap>\n"); return 99; }
    void* m = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!m) { printf("FAIL: dlopen: %s\n", dlerror()); return 1; }
    const clap_plugin_entry_t* e = (const clap_plugin_entry_t*)dlsym(m, "clap_entry");
    if (!e || !dlsym(m, "VST2BridgeClap")) { printf("FAIL: exports clap_entry and VST2BridgeClap\n"); return 1; }
    if (!clap_version_is_compatible(e->clap_version) || !e->init(argv[1])) { printf("FAIL: clap_entry init\n"); return 1; }
    if (!e->get_factory(CLAP_PLUGIN_FACTORY_ID) || e->get_factory("nothing")) { printf("FAIL: plugin factory\n"); return 1; }
    e->deinit();
    printf("ok: %s loads, clap_entry and factory are there\n", argv[1]);
    return 0;
}
