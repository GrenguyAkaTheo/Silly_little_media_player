{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  nativeBuildInputs = with pkgs; [
    cmake
    pkg-config
  ];

  buildInputs = with pkgs; [
    gcc
    ffmpeg
    alsa-lib
    libpulseaudio
    qt6.qtbase
  ];

  shellHook = ''
    export LD_LIBRARY_PATH="${pkgs.libpulseaudio}/lib:${pkgs.alsa-lib}/lib:$LD_LIBRARY_PATH"

    export PULSE_SERVER="unix:/run/user/1000/pulse/native"

    export SET_XDG_RUNTIME_DIR=1
    export GIO_EXTRA_MODULES=""
  '';
}
