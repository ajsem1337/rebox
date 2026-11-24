// operator new and delete for everything in rebox_anyps5.dll (AnyPS5, SPIRV-Tools, glslang) from
// mimalloc: per-thread heaps, so d3d12_xs's compile workers don't queue on the process heap's lock
// as they do in msvcrt's malloc under Wine. A block freed on another thread goes back to its owner.
#include <mimalloc-new-delete.h>
