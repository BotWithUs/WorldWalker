#ifndef WORLDWALKER_CLI_EXECUTORTESTS_H
#define WORLDWALKER_CLI_EXECUTORTESTS_H

#include "format/ArtifactReader.h"

#include <cstddef>

// The `wwcli <artifact.wwa>` report's executor section: six numbered tests that
// drive ww::exec::Executor (and the C ABI wrapping it) against a scripted set of
// Primitive callbacks, with no game attached.
//
// Each test supplies a harness that answers reads from a simulated player and
// records what the executor did, then asserts on both the terminal status and
// the call pattern — an executor that arrives by accident, or that walks when a
// plan said it should not, fails here rather than in-game.
//
// `artifactPath` may be null; the two FFI tests are skipped when it is, because
// the C ABI opens the artifact by path rather than borrowing the reader.
// Returns the number of failed checks so the caller can turn them into a
// non-zero exit code.
namespace ww::cli
{
    std::size_t runExecutorTests(const format::ArtifactReader &reader, const char *artifactPath);
}

#endif  // WORLDWALKER_CLI_EXECUTORTESTS_H
