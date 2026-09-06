#ifndef WORLDWALKER_RUNTIME_SEARCHCONTEXT_H
#define WORLDWALKER_RUNTIME_SEARCHCONTEXT_H

#include "format/ArtifactReader.h"
#include "runtime/AreaSearch.h"
#include "runtime/InstanceMap.h"
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
            // The view resolves collision through this context's own instance
            // map for its whole life; the map itself is empty (inactive) until a
            // query installs a descriptor grid, which is the static-scene case.
            view.setInstance(&instance);
        }

        SearchContext(const SearchContext &) = delete;
        SearchContext &operator=(const SearchContext &) = delete;
        SearchContext(SearchContext &&) = delete;
        SearchContext &operator=(SearchContext &&) = delete;

        // Drops the dynamic-region descriptor grid, and trims the view's caches
        // back under budget when a long borrow blew past it.
        //
        // The WorldView's clip and area caches are immutable functions of the
        // borrowed artifact, so re-using them across queries on the same context
        // is sound and lets the next borrower land on a warm working set instead
        // of re-inflating every touched square — and because the instance
        // redirect resolves to source coordinates *before* the square lookup,
        // those caches are keyed in source space and stay valid across a scene
        // change too. The other components' scratch is overwritten on every
        // findPath / assemble call, so they have no per-query state either.
        //
        // The instance map is the one thing that must not survive the borrow.
        // The next query may be an ordinary overworld walk, and resolving it
        // through the previous borrower's descriptor grid would answer with
        // another scene's collision — plausible wrong tiles rather than a
        // failure, which is the worst shape a pathfinding bug can take.
        void recycle()
        {
            instance.clear();
            view.trimCache();
        }

        // The scene's dynamic-region grid, empty in a static scene. Installed on
        // `view` at construction and refreshed per query / per (re-)plan.
        //
        // Declared FIRST on purpose: `view` holds a pointer to it, and members
        // are destroyed in reverse declaration order, so this ordering means the
        // pointee outlives the pointer rather than the other way round.
        InstanceMap instance;
        WorldView view;
        AreaSearch areaSearch;
        TileSearch tileSearch;
        PathAssembler assembler;
    };
}

#endif  // WORLDWALKER_RUNTIME_SEARCHCONTEXT_H
