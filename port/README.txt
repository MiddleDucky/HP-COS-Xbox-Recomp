HARRY POTTER AND THE CHAMBER OF SECRETS - PC PORT
=================================================

A native Windows port of the original Xbox game, made by static recompilation.
It contains no game data: you need the files from your own copy of the Xbox disc.
This is an unofficial fan project, not affiliated with or endorsed by Warner
Bros., Electronic Arts or Eurocom.


MINIMUM SYSTEM REQUIREMENTS
---------------------------

  OS:       Windows 10 or 11, 64-bit
  CPU:      4 threads with SSE4.1 (Intel Core 2nd generation / AMD FX or newer)
  Memory:   4 GB RAM
  Graphics: DirectX 12 card (feature level 11_0) with a current driver:
            NVIDIA GeForce GTX 600 series, AMD Radeon HD 7000 series,
            Intel HD Graphics 500 series or newer
  Storage:  about 3 GB (the game files + the port)
  Input:    keyboard and mouse, or an Xbox controller (360, One or Series)

Windows "N" editions also need Microsoft's Media Feature Pack.


INSTALLING
----------

1. Put the port's files in a folder of your choice:
       hpcos_launcher.exe
       hpcos.exe
       README.txt
       CHANGES.txt
       licenses\
       shadercache\base_shaders.bin
       shadercache\base_pipelines.bin
2. Copy the game files from your disc, or the .xiso / .iso file, into a
   folder named "game" in the same folder as the executables. A disc image
   is unpacked into the game folder the first time you start the launcher;
   the image is left as it is and can be deleted afterwards.
   The launcher then checks every game file against the disc's once (a few
   seconds, longer on a hard drive) and names any that are damaged or
   missing; "Verify game files" on its page checks them again.

The port creates these itself on the first run:
    hpcos.toml     your settings (the launcher and F10 change them)
    saves\         your saved games
    shadercache\   shaders and pipelines built on your PC
    logs\runs\     a log of each start (the newest 10 are kept)


PLAYING
-------

Start the game with hpcos_launcher.exe. Opening hpcos.exe directly only offers
to open the launcher. Only one copy of the game runs at a time: starting it
again brings the running game to the front.

The launcher sets the aspect ratio (4:3 or 16:9), display, graphics,
controls and the language, then starts the game with Play. Language: the
game starts in English (US or UK), French, German, Spanish or Italian, the
languages the disc carries. If you turn its page off, the
launcher starts the game straight away; hold Shift while opening it to see
the page again.

Keys that always work
  F10          Options (see below)
  Alt+Enter    switch between windowed and borderless
  Alt+F4       quit the game

Controller
  An Xbox 360 / One / Series controller plays like the original Xbox pad.
  The original pad's White and Black buttons are LB and RB.
  To play with keyboard and mouse while a controller is connected, turn
  off "Use controller" (launcher or F10 > Controls).

Keyboard and mouse (defaults; change them under F10 > Controls)
  Move                  W A S D
  Camera                mouse, or the arrow keys
  A                     Space
  B                     E
  X                     left mouse button
  Y                     right mouse button
  Black / White         R / Q
  Left / right trigger  Shift / Ctrl
  D-pad                 no keys by default (bind them under F10 > Controls)
  Spells                1 Flipendo, 2 Diffindo, 3 Lumos, 4 Incendio,
                        5 Skurge, 6 Avifors, 7 Alohomora, 8 Expelliarmus:
                        cast straight from the key without equipping it
                        (only spells Harry has learnt)
  Start / Back          Escape / Tab
  Stick clicks          C (left), middle mouse button (right)
  Pace of the keys      mouse wheel: down Slow walk < Walk < Run up (Run is
                        the default; also F10 > Controls > Movement keys)


OPTIONS (F10)
-------------

F10 opens the options over the game; most changes apply at once and are saved
to hpcos.toml. "Defaults (this page)" resets the page you are on (Graphics,
Controls, ...) and leaves the others; the aspect ratio stays. "Show the
launcher at startup" turns the launcher's page on or off.

Graphics
  Display
    Aspect ratio         4:3, 16:9 (the game's camera widened; menus and the
                         inventory keep 4:3) or Stretch. Applies at the next
                         start.
    Display mode         Windowed, Borderless (covers the monitor) or
                         Fullscreen (switches the monitor's resolution)
    Window size /
    Resolution           the window's size, or the fullscreen mode; Desktop
                         uses the monitor's own
    Vsync                waits for the monitor's refresh (no tearing)
  Resolution
    Internal resolution  the lines the 3D is drawn at: 480 is the Xbox's own;
                         720 to 2160 sharpen it; "Follow window" matches the
                         window
    Render scale %       with "Follow window": the render size as a share of
                         the window's
    Max render height    with "Follow window": the most lines it follows the
                         window to (1080 by default)
  Anti-aliasing and filtering
    MSAA                 smooths polygon edges: off, 2x, 4x, 8x
    SMAA                 smooths edges over the finished picture; combines
                         with MSAA
    Anisotropic filtering  sharper textures at an angle: the game's own, 2x,
                         4x, 8x or 16x
  Picture
    Brightness, Contrast 50 leaves the picture as the game draws it
    FPS counter          shows the frame rate
    Draw everything in view
                         objects at the screen's edges and behind doorways
                         stay drawn (the Xbox left some out); on by default
  Post-processing
    Bloom, Bloom strength  a glow around bright parts of the 3D scene (not
                         the HUD)
    Sharpening           contrast-adaptive sharpening, off by default
    Saturation, Vibrance, Warmth
                         colour grading; 100 %, off and "as the game" leave
                         the colours alone
    Vignette, Film grain darkened corners and film grain, off by default
    Field of view (experimental)
                         80 to 130 % of the game's; 100 is the game's own

Audio
  Speakers               Stereo, or 5.1 surround: the console's own speaker
                         mix, each channel to its own speaker. Needs Windows'
                         speaker setup set to 5.1 (or a 5.1 HDMI receiver).
                         Applies at the next start.

Controls
  Keyboard and mouse
    Keyboard and mouse play as controller 1 - alongside a controller
    Mouse sensitivity, Invert mouse Y
    Movement keys        the pace W A S D move Harry at: Slow walk, Walk or
                         Run; the mouse wheel steps it in game
    Button prompts       the game's on-screen hints name your keys:
                         Follow the last input (keys after keyboard and
                         mouse, buttons after the controller), Controller
                         buttons or Keyboard keys. The button picture in the
                         corner still shows the pad's buttons.
    Key bindings         click a binding, then press a key or mouse button
                         (Escape clears it); "Default keys" restores them;
                         the spell keys are at the end of the list
  Controller
    Use controller       off: the game ignores the controller (and it does
                         not vibrate); keyboard and mouse still play
    Left / right stick deadzone %      how far a stick moves before it counts
    Left / right stick sensitivity %   the response past the deadzone
    Invert camera X / Y
    Vibration, Vibration strength %
    Button remapping     which pad button each game button reads;
                         "Default buttons" restores them

Accessibility
  Colour blindness
    Correction filter    Off, Protanopia (red-weak), Deuteranopia
                         (green-weak) or Tritanopia (blue-weak): shifts the
                         colours you confuse toward ones you can tell apart
    Strength %

Enhancements
  Hogwarts grounds
    Explore Hogwarts Grounds (experimental)
                         walk the whole grounds without the paths' fades to
                         their other end; every area is shown and the
                         destination names are hidden. The story's cutscenes
                         and lessons still play; flying practice can only be
                         repeated by turning this off. At night the flying
                         practice arch takes Harry to the field and its exit
                         brings him back, and the broom is unavailable.
  Broom
    Land the broom on any flat surface (experimental)
                         land over a flat roof or a wall's top as in a
                         landing zone (not on the lake)
  Lighting
    Per-pixel lighting   smooth light round Lumos and lamps instead of the
                         console's per-vertex patches (on by default); off
                         looks exactly as on the console

Debug
  Logs                   "Open logs folder" shows logs\runs; "Mark this
                         moment in the log" notes the time, so a problem can
                         be found in the log: send the log and the mark's
                         number with your report
  Shaders                how many pipelines are recorded and prepared ahead
  Renderer               "Capture the next frame" (F12) saves a picture of
                         the frame and how it was drawn under captures\, for
                         reporting a drawing problem


TROUBLESHOOTING
---------------

The launcher says game files are damaged or missing
    Copy those files again from your disc or game dump, or put the disc
    image (.iso / .xiso) in the game folder and press "Repair" on the
    launcher's page. A damaged file can crash the game or lose sound or
    pictures where it is used.

"Failed to load default.xbe"
    The game files are missing: check that game\default.xbe exists beside
    hpcos.exe (or that the .xiso / .iso is in the game folder).

The game will not start / closes at once
    Update your graphics driver (DirectX 12 is required). The newest log in
    logs\runs says what happened; send it with your report.

A short pause the first time you enter an area
    Shaders are being prepared for your graphics card; the loading picture
    stays up meanwhile. The port ships shaders prepared on another PC, and
    from the second start it prepares everything it has seen ahead while you
    are in the menus ("Preparing shaders" shows in the corner).


LICENCES AND SOURCE CODE
------------------------

The port's own code is under the MIT licence. It also contains:
  - the D3D12 renderer it is built on, by TGP482 and Joshhhuaaa (MIT)
  - Dear ImGui, the options panel (MIT)
  - code from the xemu project (the Xbox audio processor emulation), which
    stays under the GNU Lesser General Public License, version 2.1 or later
The licence texts and the list of third-party code are in the licenses folder
beside this file.

The complete source code, including that code and any changes made to it, is
available at:
    https://github.com/MiddleDucky/HP-COS-Xbox-Recomp

The Harry Potter game itself is not included and is not covered by these
licences.
