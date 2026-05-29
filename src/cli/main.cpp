#include "c_api/worldwalker_c.h"

#include <cstdio>

// wwcli — WorldWalker dev harness (queries + benchmarks).
// Scaffold entry point: exercises the C ABI to confirm the shared library
// links and resolves. Real query/benchmark commands land in Phase 6.
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    std::printf("wwcli - WorldWalker dev harness (scaffold)\n");
    std::printf("artifact format version: %u\n", (unsigned)WW_ARTIFACT_FORMAT_VERSION);

    ww_artifact *artifact = ww_artifact_open("nonexistent.wwa");
    if (artifact == nullptr)
    {
        std::printf("ww_artifact_open returned NULL as expected: %s\n", ww_last_error());
    }
    else
    {
        ww_artifact_close(artifact);
    }
    return 0;
}
