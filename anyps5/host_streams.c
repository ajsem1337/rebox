/* AnyPS5's Windows build routes stdout/stderr through these (defined in its guest
 * libc, which rebox_anyps5 doesn't build): the host's own streams. */
#include <stdio.h>

FILE *HostStandardOutputFile_nid_no_patch(void)
{
    return stdout;
}

FILE *HostStandardErrorFile_nid_no_patch(void)
{
    return stderr;
}
