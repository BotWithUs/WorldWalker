# Scope: overworld and static dungeons; instanced/procedural content is out

v1 models the persistent world and fixed (non-procedural) dungeons and caves. Instanced and procedurally-generated content is explicitly **out of scope**: player-owned houses, Dungeoneering/Daemonheim floors, and boss/minigame/quest instances (Fight Kiln, etc.). A static baked artifact cannot represent layouts that are generated per-session or customised per-player. For such content WorldWalker routes the player to the instance *entrance* and stops; navigation inside is the consumer's responsibility.

This boundary is recorded because it is a deliberate "no," not an oversight — a future reader will otherwise wonder why the walker can route to a dungeon mouth but not through Daemonheim. Extending to instances would require blending the static artifact with live consumer-supplied layout overlays, which we are intentionally not doing in v1.
