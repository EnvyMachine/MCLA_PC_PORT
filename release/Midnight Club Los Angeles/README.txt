Midnight Club: Los Angeles - Complete Edition, native PC port (version 0.4)
==========================================================================

First time: the game itself is not included. Copy the files from your own Xbox 360 disc
of Midnight Club: Los Angeles - Complete Edition into the game\ folder next to the exe,
so that game\default.xex exists.

Start: double-click "Midnight Club Los Angeles.exe" (or the desktop shortcut).
It starts in fullscreen; to start in a window, edit MCLA.ini.

Fullscreen and window
---------------------
- F11 or Alt+Enter switches between fullscreen and a window.
- In fullscreen, move the mouse to the top middle of the screen: a bar slides down with
  Minimize, Exit full screen and Close.
- In a window, right-click the title bar for "Full screen".
- The mouse cursor hides after 2 seconds without movement.

Controls
--------
An Xbox / XInput controller works as on the console. Keyboard (player 1):

  W / S           accelerate (RT) / brake-reverse (LT)
  A / D           steer (left stick)
  Arrow keys      D-pad
  Enter           Start          Tab        Back
  Space           A              Backspace  B
  Left Shift      X              Left Ctrl  Y
  Q / E           LB / RB
  I J K L         right stick (camera)
  Z / C           left / right stick click

Arcade races: pause > RB (MODES) > ARCADE. Controller layout: pause > SETTINGS > OPTIONS.

Files in this folder
--------------------
  game\           the game data (your Xbox 360 disc contents)
  userdata\       saves - back this folder up to keep your career
  MCLA.ini        settings (fullscreen, volume, ...)
  mcla.log        written each run; send it along with any crash or problem
  shader_cache\   compiled graphics shaders (rebuilt automatically if deleted)
  ffmpeg\         audio decoder libraries
  MCLA.pdb        debug symbols (lets crash reports name the failing code)

The whole folder can be moved anywhere; it needs nothing outside it.

Changes in 0.4.1
----------------
- Fixed the crash at the end of a race / when entering the garage.
- Minimap: the streets no longer turn into one dark blob around the player while driving.
- The full-screen speed blur is off (MCLA_MOTION_BLUR=1 in MCLA.ini brings it back).

Known issues
------------
- At night, car headlights can light the road as hard-edged bright patches instead of
  soft pools.
- The race start grid is the heaviest scene and can drop a frame now and then.
- Entering a new part of the city can cause a short hitch while its textures load, and
  objects with new shaders can pop in a moment late the first time they are seen.
- Finishing a race (results screen) and career missions are not fully tested yet.
- Online features (Xbox LIVE) are not available.
