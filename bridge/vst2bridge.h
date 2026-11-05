#ifndef VST2BRIDGE_API_H
#define VST2BRIDGE_API_H

#include <stddef.h>
#include "vst2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Windows DLL exports. Pass the original plugin DLL's absolute path.
 * A uses the Windows ANSI code page. W uses UTF-16. Paths are limited to
 * MAX_PATH, including the terminator. The helper lives beside this DLL.
 * Returns NULL on failure. Close a returned proxy with effClose and keep
 * the bridge DLL loaded until all of its proxies have been closed.
 */
AEffect* VSTCALL VST2BridgeCreateA(AudioMasterCallback master, const char* plugin_path);
AEffect* VSTCALL VST2BridgeCreateW(AudioMasterCallback master, const wchar_t* plugin_path);

#ifdef __cplusplus
}
#endif
#endif
