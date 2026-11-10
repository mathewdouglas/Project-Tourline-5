# Project Tourline 5 branding

`pt5-logo.png` is the original PT5 Racing Logo supplied for this project.
`pt5.ico` contains 16, 20, 24, 32, 40, 48, 64, 128 and 256 pixel versions for
Windows Explorer, the title bar, taskbar and Alt+Tab. The artwork, colours and
black background are preserved.

The executable embeds the icon and project metadata through `src/pt5.rc.in`.
The game window loads the same resource at startup; `GT5P_WINDOW_ICON=0`
disables that window override. No external image is needed at run time.

To regenerate the ICO after replacing the source PNG, run
`python scripts/make_app_icon.py` with Pillow installed. Normal builds use the
checked-in ICO directly.
