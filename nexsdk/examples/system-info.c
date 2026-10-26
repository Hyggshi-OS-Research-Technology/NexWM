/* nexsdk/examples/system-info.c — what this machine is, in ten lines of code.
 *
 * Build (from a checkout, without installing anything):
 *   cc -I nexsdk/include system-info.c nexsdk/src/… -o system-info
 * Build (against an installed NexSDK):
 *   cc $(pkg-config --cflags nexsdk) system-info.c $(pkg-config --libs nexsdk) -o system-info
 */
#include <nexsdk/nexsdk.h>

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    NexSdkSystem sys;
    if (nexsdk_system_read(&sys) != 0) {
        fprintf(stderr, "cannot read the system\n");
        return 1;
    }

    char *line = nexsdk_system_summary(&sys);
    printf("%s\n\n", line ? line : "?");
    free(line);

    char *report = nexsdk_system_report(&sys);
    fputs(report ? report : "", stdout);
    free(report);

    printf("\nNexSDK %s · HDE %s · inside an HDE session: %s\n", nexsdk_version(),
           sys.hde_version[0] ? sys.hde_version : "not installed", nexsdk_in_hde_session() ? "yes" : "no");
    return 0;
}
