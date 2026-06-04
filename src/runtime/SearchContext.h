#ifndef WORLDWALKER_RUNTIME_SEARCHCONTEXT_H
#define WORLDWALKER_RUNTIME_SEARCHCONTEXT_H

#include "format/ArtifactReader.h"
#include "runtime/AreaSearch.h"
#include "runtime/PathAssembler.h"
#include "runtime/TileSearch.h"
#include "runtime/WorldView.h"

namespace ww::runtime
{
    // Per-query bundle of the four runtime planner components a single query or
    // executor run needs: a WorldView (clip/area cache), an AreaSearch + a
    // TileSearch (each with their own reusable scratch), and a PathAssembler
    // that stitches them. One context belongs to at most one in-flight query;
    // ContextPool hands them out and reclaims them when the query is done.
    //
    // Borrows the artifact, which must outlive the context. Not thread-safe and
    // non-movable so a borrower can hold a stable reference for the duration of
    // a query without the pool relocating it.
    struct SearchContext
    {
        explicit SearchContext(const format::ArtifactReader &reader)
            : view(reader),
              areaSearch(reader),
              tileSearch(view),
              assembler(reader, view, areaSearch, tileSearch)
        {
        }

        SearchContext(const SearchContext &) = delete;
        SearchContext &operator=(const SearchContext &) = delete;
        SearchContext(SearchContext &&) = delete;
        SearchContext &operator=(SearchContext &&) = delete;

        // No-op today: the WorldView's clip and area caches are immutable
        // functions of the borrowed artifact, so re-using them across queries
        // on the same context is sound and lets the next borrower land on a
        // warm working set instead of re-inflating every touched square. The
        // other components' scratch is overwritten on every findPath /
        // assemble call so they have no per-query state to clear either. The
        // method stays as the explicit "borrow returned to pool" hook in case
        // a future change does need to flush something here.
        void recycle()
        {
        }

        WorldView view;
        AreaSearch areaSearch;
        TileSearch tileSearch;
        PathAssembler assembler;
    };
}

#endif  // WORLDWALKER_RUNTIME_SEARCHCONTEXT_H
