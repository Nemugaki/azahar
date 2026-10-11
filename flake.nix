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
      # nix/gdb-17-3ds.patch targets gdb 17. When nixpkgs moves to another major version the
      # patch has to be ported; until then gdb-3ds fails with this message and the dev shell
      # falls back to plain gdb instead of breaking evaluation.
      gdbPatchMajor = "17";
      gdbPatchApplies = pkgs: pkgs.lib.versions.major pkgs.gdb.version == gdbPatchMajor;
      gdb3ds =
        pkgs:
        let
          gdb =
            (pkgs.gdb.override { python3 = pkgs.python3.withPackages (ps: [ ps.protobuf ]); }).overrideAttrs
              (old: {
                pname = "gdb-3ds";
                patches = (old.patches or [ ]) ++ [ ./nix/gdb-17-3ds.patch ];
                postInstall = (old.postInstall or "") + ''
                  $out/bin/gdb -q -nx -batch -ex "set architecture arm" -ex "set osabi 3DS" -ex "show osabi" 2>&1 \
                    | grep -F 'The current OS ABI is "3DS".'
                  $out/bin/gdb -q -nx -batch -ex "python import google.protobuf"
                '';
              });
        in
        pkgs.lib.throwIfNot (gdbPatchApplies pkgs)
          "gdb-3ds: nix/gdb-${gdbPatchMajor}-3ds.patch must be ported to gdb ${pkgs.gdb.version}"
          gdb;

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
        in
        {
          gdb-3ds = gdb3ds pkgs;
          default = pkgs.callPackage ./nix/package.nix {
            src = source;
            version = "agentic-pica-${self.shortRev or self.dirtyShortRev or "dirty"}";
            rev = self.rev or self.dirtyRev or "0000000000000000000000000000000000000000";
            useDiscordRichPresence = features.discordRpc;
            enableGamemode = features.gamemode;
          };
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          inherit (pkgs) lib;
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.default ];
            packages = with pkgs; [
              cmake
              ninja
              pkg-config
              (
                if gdbPatchApplies pkgs then
                  gdb3ds pkgs
                else
                  lib.warn "gdb-3ds unavailable for gdb ${pkgs.gdb.version}; using plain gdb" pkgs.gdb
              )
            ];
          };
        }
      );
    };
}
