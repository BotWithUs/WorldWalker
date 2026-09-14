# Artifact manifests (tracked)

This directory holds the **tracked** record of the artifacts this repo publishes.
It carries manifests, never artifacts.

## Why the binary is not here

`worldwalker.wwa` is ~10 MB of already-compressed zlib blobs. Git can neither
delta nor compress it further, so committing one bake would make it several times
the size of the entire source history, and every rebake would add another ~10 MB
to every clone, permanently. In a repo whose point is to be cloneable by
contributors, that is the wrong trade.

The artifact lives as a **release asset** on `BotWithUs/WorldWalker`. What lives
here is the sidecar that identifies it: `collisionFingerprint`, `datasetHash` and
the per-file dataset fingerprints, the bake counts, the artifact's `sha256` and
byte length, and the `releaseTag` it was published under.

That is enough to answer "which artifact is the real one and what was it built
from" from a text file in a clone, and enough for a deploy step to verify that the
binary it fetched is the one the repo means.

## How a manifest gets here

```powershell
.\scripts\bake.ps1 -ReleaseTag wwa-<yyyy-mm-dd>
```

`-ReleaseTag` stamps the tag into the sidecar and copies it here. It uploads
nothing — creating the release is a separate, deliberate step. See
`docs/artifact-delivery.md`.

The sidecar that `wwbuild` drops beside the artifact in `build\` is gitignored
along with everything else there. This copy is the one git tracks.
