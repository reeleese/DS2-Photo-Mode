DS2 Photo Mode {version}
Dark Souls II: Scholar of the First Sin, Steam version
=====================================================

A photo mode for the controller, the keyboard and mouse, and the Steam Deck: a free camera with focal length, orbit and exact position and roll, poses for your character (gestures you can freeze, scrub and turn), a Body XYZ panel that moves and tilts her, framing grids, see-through panels, and a snap button that saves a clean shot (no panels, no grid) to your Steam screenshots. On the Deck the shot is the whole 1280x800 screen.

Files
-----
  photo-mode-licenses.txt
  xinput1_3.dll
  LICENSE.txt
  NOTICE.txt
  README.txt

xinput1_3.dll is the mod. photo-mode-licenses.txt holds the licences of the open-source code inside it (Dear ImGui, MIT; MinHook, BSD 2-clause).

DS2 Photo Mode is open source under the Apache-2.0 licence (LICENSE.txt, NOTICE.txt): its full source is the optional "source" file on the mod's Nexus page.


Install: Windows
----------------
1. Open the game's folder: in Steam, right-click the game > Manage > Browse local files, then open the "Game" folder (the one with DarkSoulsII.exe).
2. Copy xinput1_3.dll there. That's all: the game loads it on start.

Note: tested on Linux (Proton and Steam Deck). It should work the same on Windows, but it hasn't been tested there yet.


Install: Linux / Steam Deck (Proton)
------------------------------------
1. Copy xinput1_3.dll into the "Game" folder as above.
2. In Steam, right-click the game > Properties > General > Launch Options, and set:

       WINEDLLOVERRIDES="xinput1_3=n,b" %command%

   Without it, Proton uses its own XInput and the mod never loads. If you already have a WINEDLLOVERRIDES (for example "dinput8=n,b" for a mod loader), join them with a semicolon:

       WINEDLLOVERRIDES="dinput8=n,b;xinput1_3=n,b" %command%

Works with Seamless Co-op (tested), and next to mod loaders that use dinput8.dll (such as Mod Engine). It can't be used together with another mod that also ships an xinput1_3.dll.


Uninstall
---------
Delete xinput1_3.dll from the Game folder (and remove the launch option on Linux).


Controls (keyboard and mouse)
-----------------------------
F9                            photo mode on / off. While it's on, the game
                              doesn't see your keyboard or mouse.

Free camera:
  Mouse              aim
  W A S D            move along the view, strafe
  Q / E              down / up (roll left / right in 6DOF)
  Shift / Ctrl       faster / slower (hold)
  Mouse wheel        focal length (zoom)

Panels (bottom left):
  Up / Down arrows   pick a row
  Left / Right       change it (hold to speed up)
  Tab                cycle tabs
  Click/Enter/Space  snap (the same as A on the controller)
  R or Backspace     reset the camera

The panel's hint lines show the keys when you last used the keyboard or mouse, and the controller buttons when you last used the controller. The controller and the keyboard and mouse can be used in turn.

Everything below describes the controller buttons; use the key listed above for each.


Controls (controller)
---------------------
L3 + R3 (click both sticks)   photo mode on / off. While it's on, the game
                              doesn't see your controller.

Free camera:
  Left stick         move along the view, strafe
  Right stick        aim
  RB / LB            up / down
  RT / LT (hold)     faster / slower

Panels (bottom left):
  D-pad up/down      pick a row
  D-pad left/right   change it (hold to speed up)
  X                  cycle tabs: Camera, Camera XYZ, Pose, Body XYZ,
                     Misc
  A                  snap: saves the picture without the panels or the grid to
                     Steam screenshots (view them with Steam's screenshot
                     manager). The picture is the whole screen (1280x800 on the
                     Steam Deck); only on screens wider than 16:9 does the game
                     draw it in the middle, and the snap is that middle part.
  B                  reset the camera to where the game's camera is

Camera panel: focal length (in mm, like a full-frame lens), move speed,
  "Orbit around me".
Camera XYZ panel: the camera's exact position (X/Y/Z, metres) and angles
  (pitch/yaw/roll), a step at a time on the D-pad.
Pose panel: your character's pose.
  Pose     a gesture (the game's 20, under their in-game names, and Stretch) or
           Crouch; None gives the character back to the game.
  Frame    how far into the pose (0-100%). Change it to play to that frame and hold
           there; going back plays a gesture backwards.
  Freeze   holds the pose where it is.
  Turn     turns your character round (+-180 degrees).
  Leaving photo mode lets the pose play out and gives your character back.
Body XYZ panel: moves and tilts your character for the shot.
  X Y Z    moves her up to 5 m: X to her right, Y up, Z forward
  Pitch    tips her back (+) or forward (-), about the hips
  Yaw      turns her round
  Roll     tips her to one side
  Reset    D-pad left/right on it puts all six back to 0
  Her armour, weapon and shield move with her. Only the picture moves: your
  character doesn't really go anywhere, so she can float, lie down or lean, and leaving photo mode puts her back exactly where she stood.
Misc panel:
  Flight mode   Default, 6DOF or Seamoth (left/right cycles them)
  Grid          None, Rule of thirds or Rule of halves: thin lines over the picture
                to help framing. A snap (A) never includes the grid.

Orbit around me: the camera circles your character. Right stick and left stick left/right go round, left stick up/down moves in and out, RB/LB raise and lower.

Flight mode 6DOF (like the game Descent): everything follows the camera, with no fixed horizon. Right stick turns relative to the view, LB / RB roll left / right, and the left stick flies wherever the camera points (up and down too).

Flight mode Seamoth (like the sub in Subnautica): flies where you look and strafes, RB / LB rise / sink, the horizon stays level. It has momentum: it speeds up smoothly, glides on after you let go and slows to a stop like it's in water. Good for smooth, drifting camera moves.

Orbit and a flight mode are one at a time: turning Orbit on sets Flight mode to Default, and picking 6DOF or Seamoth turns Orbit off.


Known limits
------------
- No HUD toggle yet. For clean shots, set the game's Options > Screen Options > HUD to "Always hide" before taking photos.
- The world keeps moving while the camera is free: enemies can still attack you.
- Poses: gestures and Crouch only. No held poses (kneeling, praying, sitting at a bonfire), combat stances or idles yet. Going back on Frame from a gesture's last second or so, or on Crouch, replays the pose, so you see it play through quickly.
- Dark Souls II: Scholar of the First Sin (64-bit, DirectX 11) only. Not for the original 2014 release.
- If a game update moves things around, the panel shows "camera not found" and the mod does nothing.


Use offline
-----------
The mod doesn't change your save, items or stats, but it is code running inside the game. Use it offline (Steam offline mode, or the game's network setting off). Online play is at your own risk.


Log file
--------
%LOCALAPPDATA%\ds2-photo-mode\photo-mode.log (under Proton, inside the game's prefix: steamapps/compatdata/335300/pfx/drive_c/users/steamuser/AppData/Local/). The mod writes nothing else.


Licence and credits
-------------------
DS2 Photo Mode is by reeleese, open source under the Apache-2.0 licence (LICENSE.txt,
NOTICE.txt). Credit reeleese as the original creator when you share or build on it.
Developed alongside AI (Claude by Anthropic), directed and tested by reeleese.
