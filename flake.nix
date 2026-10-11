{
  description = "Azahar emulator and development shell";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    {
      self,
      nixpkgs,
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
      gdb3ds =
        pkgs:
        assert pkgs.lib.versions.major pkgs.gdb.version == "17";
        (pkgs.gdb.override {
          python3 = pkgs.python3.withPackages (ps: [ ps.protobuf ]);
        }).overrideAttrs
          (old: {
            pname = "gdb-3ds";
            patches = (old.patches or [ ]) ++ [ ./nix/gdb-17-3ds.patch ];
            postInstall = (old.postInstall or "") + ''
              $out/bin/gdb -q -nx -batch -ex "set architecture arm" -ex "set osabi 3DS" -ex "show osabi" 2>&1 \
                | grep -F 'The current OS ABI is "3DS".'
              $out/bin/gdb -q -nx -batch -ex "python import google.protobuf"
            '';
          });

      # Project packaging policy. Change these when a feature should be gated.
      features = {
        discordRpc = true;
        gamemode = true;
      };
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          inherit (pkgs) lib;
          source = lib.cleanSourceWith {
            src = ./.;
            filter =
              path: type:
              let
                relative = lib.removePrefix "${toString ./.}/" (toString path);
              in
              lib.cleanSourceFilter path type
              && !(builtins.elem relative [
                ".idea"
                "build"
                "cmake-build-debug"
              ]);
          };
          base = pkgs.azahar.override {
            useDiscordRichPresence = features.discordRpc;
            enableGamemode = features.gamemode;
            # Upstream master links FFmpeg directly and requires 7.x exactly (libavutil 59);
            # the nixpkgs package still targets the 2126 releases, which use 6.x.
            ffmpeg_6-headless = pkgs.ffmpeg_7-headless;
          };
        in
        {
          gdb-3ds = gdb3ds pkgs;
          default = base.overrideAttrs (old: {
            version = "agentic-pica-${self.shortRev or self.dirtyShortRev or "dirty"}";
            src = source;
            # nixpkgs carries patches against upstream release sources; quartzcore.patch is
            # macOS-only and does not apply to this tree's src/citra_qt/CMakeLists.txt.
            patches = builtins.filter (p: baseNameOf (toString p) != "quartzcore.patch") (
              old.patches or [ ]
            );
            postPatch = (old.postPatch or "") + ''
              echo "${self.rev or self.dirtyRev or "0000000000000000000000000000000000000000"}" > GIT-COMMIT
              echo "agentic-pica-${self.shortRev or self.dirtyShortRev or "dirty"}" > GIT-TAG
            '';
            cmakeFlags =
              builtins.filter (
                flag:
                !(lib.hasInfix "USE_DISCORD_PRESENCE" flag)
                && !(lib.hasInfix "ENABLE_DISCORD_RPC" flag)
                && !(lib.hasInfix "ENABLE_GAMEMODE" flag)
              ) old.cmakeFlags
              ++ [
                (lib.cmakeBool "ENABLE_DISCORD_RPC" features.discordRpc)
                (lib.cmakeBool "ENABLE_GAMEMODE" features.gamemode)
              ];
          });
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.default ];
            packages = with pkgs; [
              cmake
              ninja
              pkg-config
              (gdb3ds pkgs)
            ];
          };
        }
      );
    };
}
