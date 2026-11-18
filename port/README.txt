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
       bin\            the game itself (hpcos.exe, hpcos_ntsc.dll, hpcos_pal.dll,
                       AMD's FSR DLLs, shadercache\)
       README.txt
       CHANGES.txt
       licenses\
2. Copy the game files from your disc, or the .xiso / .iso file, into a
   folder named "game" beside hpcos_launcher.exe. Both the North American
   (NTSC-US) and the European (PAL) disc play; the launcher tells them apart
   and shows which one it found on its Game page. A disc image
   is unpacked into the game folder the first time you start the launcher;
   the image is left as it is and can be deleted afterwards.
   The launcher then checks every game file against the disc's once (a few
   seconds, longer on a hard drive) and names any that are damaged or
   missing; "Verify game files" on its page checks them again.

The port creates these itself on the first run:
    saves\             your saved games (beside the launcher)
    bin\hpcos.toml     your settings (the launcher and PC Settings change them)
    bin\shadercache\   shaders and pipelines built on your PC
    bin\logs\runs\     a log of each start (the newest 10 are kept)

Updating from an earlier version: copy the files above over the old ones and
keep your saves\. A hpcos.toml and shadercache\ from before bin\ are moved
into bin\ by the launcher the first time it starts; the old hpcos.exe beside
the launcher is no longer used and can be deleted.


PLAYING
-------

Start the game with hpcos_launcher.exe. Opening bin\hpcos.exe directly only
offers to open the launcher. Only one copy of the game runs at a time: starting it
again brings the running game to the front.

The launcher's page has all the settings in tabs (Game, Display, Graphics,
Controls, ...): the aspect ratio, display, graphics, controls,
the enhancements and the language, then Play starts the game. Language: the
game starts in English (US or UK), French, German, Spanish or Italian, the
languages the disc carries. If you turn its page off, the
launcher starts the game straight away; hold Shift while opening it to see
the page again.

Keys that always work
  F10          key bindings, button remapping and Debug (see Settings)
  Alt+Enter    switch between windowed and borderless
  Alt+F4       quit the game

Controller
  An Xbox 360 / One / Series controller plays like the original Xbox pad.
  The original pad's White and Black buttons are LB and RB.
  To play with keyboard and mouse while a controller is connected, turn
  off "Use controller" (launcher, or PC Settings > Controls).

Keyboard and mouse (defaults; change them under F10 > Controls)
  Move                  W A S D
  Camera                mouse, or the arrow keys
  A                     Space
  B                     E
  X                     left mouse button
  Y                     right mouse button
  Black / White         R / Q
  Left / right trigger  Shift / Ctrl
  D-pad                 1 2 3 4 (up, down, left, right)
  Start / Back          Escape / Tab
  Stick clicks          C (left), middle mouse button (right)
  Pace of the keys      mouse wheel: down Slow walk < Walk < Run up (Run is
                        the default; also Controls > Movement keys)


SETTINGS
--------

Every setting is on the launcher's page, and in the game under "PC Settings",
the first row of the title screen's Options and of the pause menu. Most changes
apply at once. Key bindings, button remapping and the Debug page (the logs
folder, "Mark this moment in the log") are under F10 in the game.


TROUBLESHOOTING
---------------

The launcher says game files are damaged or missing
    Copy those files again from your disc or game dump, or put the disc
    image (.iso / .xiso) in the game folder and press "Repair" on the
    launcher's page. A damaged file can crash the game or lose sound or
    pictures where it is used.

"Failed to load default.xbe"
    The game files are missing: check that game\default.xbe exists beside
    hpcos_launcher.exe (or that the .xiso / .iso is in the game folder).

The game will not start / closes at once
    Update your graphics driver (DirectX 12 is required). The newest log in
    bin\logs\runs says what happened; send it with your report.

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
  - AMD FidelityFX frame generation DLLs (bin\amd_fidelityfx_*.dll), under
    AMD's own licence
  - code from the xemu project (the Xbox audio processor emulation), which
    stays under the GNU Lesser General Public License, version 2.1 or later
The licence texts and the list of third-party code are in the licenses folder
beside this file.

The complete source code, including that code and any changes made to it, is
available at:
    https://github.com/MiddleDucky/HP-COS-Xbox-Recomp

The Harry Potter game itself is not included and is not covered by these
licences.
