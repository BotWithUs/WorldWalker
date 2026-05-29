# One shared mmap artifact + a bounded pool of search contexts

The Java host can run multiple game clients, each running multiple scripts on virtual threads, all in one JVM. Because the artifact is identical for every client (same cache → same world), it is memory-mapped **once per process** and shared by all clients/scripts; the OS shares the physical pages, so the cost is paid once regardless of client count.

Search scratch (open/closed sets, came-from) is served from a **bounded pool of reusable search contexts sized to real parallelism (~CPU cores)**, borrowed per query and returned. We rejected tying a context to a script or virtual thread (hundreds of mostly-idle scratch buffers) and rejected a single global lock (serializes concurrent scripts, fighting the speed goal). A virtual thread only blocks when every context is busy — exactly when the CPU is already saturated — so memory stays flat as clients/scripts scale and parallelism is capped at the hardware.
