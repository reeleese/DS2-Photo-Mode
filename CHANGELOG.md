# Changelog

Each release's notes are posted to the Nexus changelog from here when it is uploaded.
Write one `## VERSION` section per release, one line per note.

## 1.3.0

Works with DS2LE PathTracing: photo mode now hooks the game's own swap chain instead of creating one of its own, so the game no longer stops at startup.
Snap saves a screenshot with DS2LE PathTracing instead of failing with "not supported".
The free camera can now fly right up to a character's skin or armour without the body being cut open: the near clip is 0.003 m instead of the game's 0.1 m while photo mode is on, and the game's own value comes back when you leave.

## 1.2.0

Hide the panels with H on the keyboard or Back / View on the controller: the camera, poses and snap keep working, and the grid stays. Press it again to bring the panels back as you left them.
The legend shows the hide key.
The start-up 'Photo mode: L3+R3' reminder is back, but only the first time and again after 30 days without using photo mode. It remembers in photo-mode.ini beside the DLL.

## 1.1.0

Keyboard and mouse support: F9 toggles photo mode, mouse aims, WASD moves, Q/E down/up, Shift/Ctrl speed, wheel focal length, arrows for rows, Tab for tabs, click/Enter/Space snaps, R resets.
Hints switch between keyboard and pad.
The start-up 'Photo mode: L3+R3' hint is gone.
