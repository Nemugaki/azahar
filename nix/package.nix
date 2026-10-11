# Package for this tree. Derived from nixpkgs' pkgs/by-name/az/azahar, which tracks the 2126
# release series; this fork follows upstream master, whose dependencies move ahead of it.
{
  lib,
  stdenv,
  src,
  version,
  rev,
  boost,
  catch2_3,
  cmake,
  cryptopp,
  cpp-jwt,
  doxygen,
  dynarmic,
  enet,
  fmt,
  ffmpeg_7-headless,
  glslang,
  gtk3,
  gsettings-desktop-schemas,
  httplib,
  inih,
  libGL,
  libunwind,
  libusb1,
  libx11,
  libxcb,
  libxext,
  nlohmann_json,
  oaknut,
  openal,
  openssl,
  pipewire,
  pkg-config,
  portaudio,
  python3,
  qt6,
  rapidjson,
  robin-map,
  SDL2,
  soundtouch,
  vulkan-headers,
  vulkan-memory-allocator,
  xbyak,
  cubeb,
  gamemode,
  enableQtTranslations ? true,
  enableCubeb ? true,
  useDiscordRichPresence ? true,
  enableSSE42 ? true, # Disable if your hardware doesn't support SSE 4.2 (mainly CPUs before 2011)
  enableGamemode ? lib.meta.availableOn stdenv.hostPlatform gamemode,
}:
let
  inherit (lib)
    optionals
    optionalString
    cmakeBool
    getLib
    ;
in
stdenv.mkDerivation {
  pname = "azahar";
  inherit version src;

  strictDeps = true;

  nativeBuildInputs = [
    cmake
    doxygen
    python3
    pkg-config
    qt6.wrapQtAppsHook
  ];

  buildInputs = [
    boost
    catch2_3
    cryptopp
    cpp-jwt
    dynarmic
    enet
    fmt
    # Upstream links FFmpeg directly and requires 7.x exactly (libavutil 59)
    ffmpeg_7-headless
    glslang
    httplib
    inih
    libGL
    libunwind
    libusb1
    nlohmann_json
    openal
    openssl
    portaudio
    robin-map
    qt6.qtbase
    qt6.qtmultimedia
    qt6.qttools
    qt6.qtwayland
    soundtouch
    SDL2
    vulkan-headers
    vulkan-memory-allocator
    pipewire
    libx11
    libxcb
    libxext
    # zstd (zstd_seekable is not packaged in nixpkgs) and spirv-tools/headers
    # (https://github.com/azahar-emu/azahar/issues/1283) come from the bundled externals.
  ]
  ++ optionals stdenv.hostPlatform.isx86_64 [ xbyak ]
  ++ optionals stdenv.hostPlatform.isAarch64 [ oaknut ]
  ++ optionals enableCubeb [ cubeb ]
  ++ optionals useDiscordRichPresence [ rapidjson ];

  postPatch = ''
    # The flake source already contains the submodules
    substituteInPlace CMakeLists.txt \
      --replace-fail "check_submodules_present()" ""
    echo "${rev}" > GIT-COMMIT
    echo "${version}" > GIT-TAG
  ''
  + optionalString enableGamemode ''
    substituteInPlace externals/gamemode/include/gamemode_client.h \
      --replace-fail "libgamemode.so.0" "${getLib gamemode}/lib/libgamemode.so.0"
  '';

  cmakeFlags = [
    (cmakeBool "USE_SYSTEM_LIBS" true)
    (cmakeBool "DISABLE_SYSTEM_LODEPNG" true)
    (cmakeBool "DISABLE_SYSTEM_VMA" true)
    (cmakeBool "DISABLE_SYSTEM_ZSTD" true)
    (cmakeBool "DISABLE_SYSTEM_SPIRV_HEADERS" true)
    (cmakeBool "ENABLE_QT_TRANSLATION" enableQtTranslations)
    (cmakeBool "ENABLE_CUBEB" enableCubeb)
    (cmakeBool "ENABLE_DISCORD_RPC" useDiscordRichPresence)
    (cmakeBool "ENABLE_GAMEMODE" enableGamemode)
    (cmakeBool "ENABLE_SSE42" enableSSE42)
  ];

  preFixup = ''
    qtWrapperArgs+=(
      --prefix XDG_DATA_DIRS : "${gsettings-desktop-schemas}/share/gsettings-schemas/${gsettings-desktop-schemas.name}"
      --prefix XDG_DATA_DIRS : "${gtk3}/share/gsettings-schemas/${gtk3.name}"
    )
  '';

  meta = {
    description = "Azahar 3DS emulator with the agentic-pica RPC and render debugger";
    homepage = "https://github.com/Nemugaki/azahar";
    # Upstream relicensed to GPLv3 (LICENSE.txt); some files remain GPLv2-or-later.
    license = lib.licenses.gpl3Plus;
    mainProgram = "azahar";
    platforms = lib.platforms.linux;
  };
}
