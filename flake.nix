{
  inputs.nixpkgs.url = "nixpkgs/nixos-unstable";
  outputs = { self, nixpkgs }: {
    packages.x86_64-linux = let
      pkgs = import nixpkgs {
        system = "x86_64-linux";
        config.allowUnfree = true; # cuda
      };
      inherit (pkgs) python3;
    in rec {
      nvcomp = with pkgs;
        stdenv.mkDerivation {
          pname = "nvcomp";
          version = "3.0.0";
          src = fetchurl {
            url = "https://developer.download.nvidia.com/compute/nvcomp/3.0/local_installers/nvcomp_3.0.0_x86_64_12.x.tgz";
            hash = "sha256-CZLttym7X2kyqCGeqArcZ0JP24GstEBvmBawJz+MPW0=";
          };
          sourceRoot = ".";
          nativeBuildInputs = [ autoPatchelfHook ];
          # libgcc, libstdc++
          buildInputs = [ stdenv.cc.cc.lib ];
          # from host cuda
          autoPatchelfIgnoreMissingDeps = [ "libnvidia-ml.so.1" ];
          appendRunpaths = [ "/usr/lib64" "$ORIGIN" "/run/opengl-driver/lib" ];
          installPhase = ''
            cp -r . $out
            # depends on cudart:
            rm -rf $out/bin
          '';
        };
      default = python3.pkgs.buildPythonPackage {
        name = "nyacomp";
        version = "0.1.6";
        src = ./.;
        propagatedBuildInputs = with python3.pkgs; [ torch pybind11 ];
        buildInputs = [ pkgs.cudaPackages_12.cudatoolkit nvcomp ];
        nativeBuildInputs = with pkgs; [ ninja which ];
        doCheck = false;
        passthru = { inherit nvcomp; };

        # TODO: bundle the nvcomp libs into the wheel?
        # postBuild = ...
      };
    };
  };
}
