WolfViewer - Linux README
-=-=-=-=-=-=-=-=-=-=-=-=-

This document contains information about the WolfViewer Linux client,
the desktop viewer for the Wolf Territories Grid.

1. Introduction
2. System Requirements
3. Installing & Running
4. Troubleshooting
   4.1. 'Error creating window.'
   4.2. System hangs
   4.3. Blank window after minimizing it
   4.4. Audio
   4.5. 'Alt' key for camera controls doesn't work
   4.6. In-world streaming movie and music playback
5. Advanced Troubleshooting
   5.1. Audio
   5.2. OpenGL
6. Source code
7. Getting more help, and reporting problems


1. INTRODUCTION
-=-=-=-=-=-=-=-

WolfViewer connects to the Wolf Territories Grid, an OpenSimulator world.
Changes you make in-world are permanent. New to the grid? Create an
account at <https://www.wolf-grid.com/index.php?f=newuser>.


2. SYSTEM REQUIREMENTS
-=-=-=-=-=-=-=-=-=-=-=

Minimum requirements:
    * Internet Connection: Cable or DSL
    * A 64-bit Linux distribution.
    * PulseAudio or ALSA Linux system sound software. A recent PulseAudio
      is the recommended configuration; see README-linux-voice.txt for more
      information.
    * A video card with OpenGL 3D drivers that are recent and correctly
      configured. The graphics drivers that came with your operating system
      may not be good enough! See the TROUBLESHOOTING section if you
      encounter problems starting WolfViewer.


3. INSTALLING & RUNNING
-=-=-=-=-=-=-=-=-=-=-=-

WolfViewer can run entirely from the directory you have unpacked it
into - no installation step is required. If you wish to perform a
separate installation step anyway, run './install.sh'; it installs to
/opt/wolfviewer as root or ~/wolfviewer as a normal user, and can add a
desktop menu entry (see WOLFVIEWER_DESKTOPINSTALL.txt).

Run './wolfviewer' from the installation directory to start WolfViewer.

For in-world MOVIE and MUSIC PLAYBACK you need GStreamer installed on your
system. This is optional - it is not required for general client
functionality. Which media you can play depends on the GStreamer plugins
you have installed.

User data is stored in the hidden directory ~/.wolfviewer_x64 by default;
you may override this location with the WOLFVIEWER_X64_USER_DIR
environment variable if you wish.


4. TROUBLESHOOTING
-=-=-=-=-=-=-=-=-=

The client prints a lot of diagnostic information to the console it was
run from. Most of this is also replicated in
~/.wolfviewer_x64/logs/WolfViewer.log - this is helpful to read when
troubleshooting, especially 'WARNING' and 'ERROR' lines.

VOICE PROBLEMS?  See the separate README-linux-voice.txt file for Voice
  troubleshooting information, and <https://www.wolf-grid.com/index.php?f=voicefaq>.

SPACENAVIGATOR OR JOYSTICK PROBLEMS?  See the separate
  README-linux-joystick.txt file for configuration information.

PROBLEM 1:- WolfViewer fails to start up, with a warning on the console like:
   'Error creating window.' or
   'Unable to create window, be sure screen is set at 32-bit color' or
   'SDL: Couldn't find matching GLX visual.'
SOLUTION:- Usually this indicates that your graphics card does not meet
   the minimum requirements, or that your system's OpenGL 3D graphics driver is
   not updated and configured correctly. If you believe that your graphics
   card DOES meet the minimum requirements then you likely need to install the
   official 'non-free' NVIDIA or AMD graphics drivers; consult your Linux
   distribution's documentation for installing these official drivers.

PROBLEM 2:- My whole system seems to hang when running WolfViewer.
SOLUTION:- This is typically a hardware/driver issue. The first thing to
   do is to check that you have the most recent official drivers for your
   graphics card (see PROBLEM 1).
SOLUTION:- As a last resort, you can disable most of WolfViewer's advanced
   graphics features by editing the 'wolfviewer' script and removing the '#'
   from the line which reads '#export LL_GL_NOEXT=x'

PROBLEM 3:- After I minimize the WolfViewer window, it's just blank when
   it comes back.
SOLUTION:- Some Linux desktop 'Visual Effects' features are incompatible
   with WolfViewer. One reported solution is to use your desktop
   configuration program to disable such effects.

PROBLEM 4:- Music and sound effects are silent or very stuttery.
SOLUTION:- Make sure PulseAudio (or PipeWire's PulseAudio service) is
   running before you start WolfViewer, and that no other application holds
   the sound device exclusively.

PROBLEM 5:- Using the 'Alt' key to control the camera doesn't work or just
   moves the WolfViewer window.
SOLUTION:- Some window managers eat the Alt key for their own purposes; you
   can configure your window manager to use a different key instead (for
   example, the 'Windows' key!) which will allow the Alt key to function
   properly with mouse actions in WolfViewer and other applications.

PROBLEM 6:- In-world movie or music playback doesn't work for me.
SOLUTION:- You need to have a working installation of GStreamer; this
   is usually an optional package for most versions of Linux. If you have
   installed GStreamer and you can play some music/movies but not others
   then you need to install a wider selection of GStreamer plugins, either
   from your vendor (i.e. the 'Ugly' plugins) or an appropriate third party.


5. ADVANCED TROUBLESHOOTING
-=-=-=-=-=-=-=-=-=-=-=-=-=-

The 'wolfviewer' script which launches WolfViewer contains some
configuration options for advanced troubleshooters.

* AUDIO - Edit the 'wolfviewer' script and you will see these audio
  options: LL_BAD_OPENAL_DRIVER, LL_BAD_FMODSTUDIO_DRIVER.
  WolfViewer tries to use OpenAL, FMODSTUDIO (PULSEAUDIO, ALSA)
  audio drivers in this order; you may uncomment the corresponding LL_BAD_*
  option to skip an audio driver which you believe may be causing you trouble.

* OPENGL - For advanced troubleshooters, the LL_GL_BLACKLIST option lets
  you disable specific GL extensions, each of which is represented by a
  letter ("a"-"o"). If you can narrow down a stability problem on your system
  to just one or two GL extensions then please report details of your hardware
  (and drivers) along with the minimal LL_GL_BLACKLIST which solves your
  problems. This will help us to improve stability for your hardware while
  minimally impacting performance.
  LL_GL_BASICEXT and LL_GL_NOEXT should be commented-out for this to be useful.


6. SOURCE CODE
-=-=-=-=-=-=-=

WolfViewer is free software under the GNU Lesser General Public License
version 2.1. The source code is at
<https://github.com/intelligentwolf/wolfviewer>.

It is based on the Firestorm viewer, Copyright (C) 2010-2026 The Phoenix
Firestorm Project, Inc., itself derived from the Second Life viewer,
Copyright (C) Linden Research, Inc.


7. GETTING MORE HELP AND REPORTING PROBLEMS
-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

For general help and support with WolfViewer and the grid:
<https://www.wolf-grid.com/blog/faq.php>

For a problem that needs a person, the grid's Discord:
<https://discord.gg/S3wj882aQH>

To report a bug or request a feature:
<https://github.com/intelligentwolf/wolfviewer/issues>
