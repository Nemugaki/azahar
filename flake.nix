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
          };
        in
        {
          default = base.overrideAttrs (old: {
            version = "agentic-pica-${self.shortRev or self.dirtyShortRev or "dirty"}";
            src = source;
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
            ];
          };
        }
      );
    };
}
