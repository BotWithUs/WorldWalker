# Scope: overworld and static dungeons; instanced/procedural content is out

> **Partially superseded by [ADR 0011](0011-instance-collision-via-chunk-remap.md).**
> Pathing *inside* a dynamic region is now supported, by remapping each instance
> tile onto the static chunk it was copied from. The "route to the entrance and
> stop" rule below still governs routing *into* an instance from the overworld,
> and the paragraph about not blending in live overlays is the decision ADR 0011
> reverses — read that one for the current position.

v1 models the persistent world and fixed (non-procedural) dungeons and caves. Instanced and procedurally-generated content is explicitly **out of scope**: player-owned houses, Dungeoneering/Daemonheim floors, and boss/minigame/quest instances (Fight Kiln, etc.). A static baked artifact cannot represent layouts that are generated per-session or customised per-player. For such content WorldWalker routes the player to the instance *entrance* and stops; navigation inside is the consumer's responsibility.

This boundary is recorded because it is a deliberate "no," not an oversight — a future reader will otherwise wonder why the walker can route to a dungeon mouth but not through Daemonheim. Extending to instances would require blending the static artifact with live consumer-supplied layout overlays, which we are intentionally not doing in v1.
