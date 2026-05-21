# Silly_little_media_player

This works like the Audacious audio player in headless mode. Run ./player in one shell in the build dir to start the service, then run ./player-ctl in another shell (also in the build dir) to send commands to acctually use the main player.

This usues a nix shell right now, so you need the nix package manager installed.

To start the nix shell just run `sudo nix-shell` in the directory with all the code and shit. Once in the nix shell you can make a directory called "build" and run `cmake` followed by `make`. Once this is done, enter the build directory (still in your nix shell) and use ./player, and ./player-ctl to use the audio player. You will need 2 nix shells open to use this.

simply running ./player-ctl will give a list of the avalable flags for the media player.

This uses C++ 15.2.0, so you will need that or newer (although this is automatically installed to the nix shell in the config I made)

If your user account ID isn't 1000, you will have to change this in the shell.nix file to match your ID
