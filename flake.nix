{
  description = "Development shell for super-optimizer";
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs, ... }: {
    devShells.x86_64-linux.default = let
      pkgs = nixpkgs.legacyPackages.x86_64-linux;
    in pkgs.mkShell {
      name = "superopt-dev";
      buildInputs = with pkgs; [
        gcc
        gnumake
        cmake
        z3
        python3
        python3Packages.pip
        python3Packages.virtualenv
        git
      ];
      shellHook = ''
        export PATH="$PATH"
      '';
    };
  };
}
