{
  description = "BetterVR - VR mod for BotW on Cemu";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};

      # Patched Cemu that exports the hook symbols BetterVR needs.
      # Stock Cemu defines DLLEXPORT as empty on native Linux, so
      # gameMeta_getTitleId, memory_getBase, osLib_registerHLEFunction
      # aren't in .dynsym and dlsym can't find them.
      cemu-bettervr = pkgs.cemu.overrideAttrs (old: {
        pname = "cemu-bettervr";
        patches = (old.patches or []) ++ [
          (pkgs.writeText "cemu-export-hooks.patch" ''
            --- a/src/Common/precompiled.h
            +++ b/src/Common/precompiled.h
            @@ -262,7 +262,7 @@
             #elif defined(__GNUC__)
                 #if BOOST_OS_WINDOWS
                     #define DLLEXPORT __attribute__((dllexport))
            -    #else
            +    #elif defined(__WINE__)
                     #define DLLEXPORT
                 #endif
             #else
            @@ -270,6 +270,10 @@
             #endif


            +#if !defined(DLLEXPORT)
            +    #define DLLEXPORT __attribute__((visibility("default")))
            +#endif
            +
             #if BOOST_OS_WINDOWS
             	#define NOEXPORT
             #elif defined(__GNUC__)
          '')
        ];
        # --export-dynamic ensures exported symbols land in .dynsym for dlsym
        cmakeFlags = (old.cmakeFlags or []) ++ [
          "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--export-dynamic"
        ];
      });
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        nativeBuildInputs = [
          pkgs.cmake
          pkgs.ninja
          pkgs.pkg-config
          pkgs.directx-shader-compiler
          pkgs.vulkan-tools
        ];

        buildInputs = [
          pkgs.openxr-loader
          pkgs.glm
          pkgs.vulkan-headers
          pkgs.vulkan-loader
          pkgs.imgui
          pkgs.implot
          pkgs.xorg.libX11
        ];

        shellHook = ''
          echo "BetterVR dev shell"
          echo "  Build: cmake --preset Linux-Debug && cmake --build cmake-build-Linux-Debug"
        '';
      };

      packages.${system} = {
        default = pkgs.stdenv.mkDerivation {
          pname = "bettervr";
          version = "0.9.16";
          src = self;

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            pkgs.directx-shader-compiler
          ];

          buildInputs = [
            pkgs.openxr-loader
            pkgs.glm
            pkgs.vulkan-headers
            pkgs.vulkan-loader
            pkgs.imgui
            pkgs.implot
            pkgs.xorg.libX11
          ];

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
          ];

          installPhase = ''
            runHook preInstall

            mkdir -p $out/lib
            mkdir -p $out/share/vulkan/implicit_layer.d
            mkdir -p $out/share/bettervr/graphicPacks

            cp lib/libBetterVR_Layer.so $out/lib/

            # Install layer manifest
            cat > $out/share/vulkan/implicit_layer.d/BetterVR_Layer.json <<EOF
            {
              "file_format_version": "1.1.2",
              "layer": {
                "name": "VK_LAYER_CREMENTIF_bettervr",
                "type": "GLOBAL",
                "library_path": "$out/lib/libBetterVR_Layer.so",
                "api_version": "1.2.0",
                "implementation_version": "1",
                "description": "BetterVR - VR mod for BotW on Cemu",
                "enable_environment": {
                  "ENABLE_BETTERVR_MOD": "1"
                },
                "disable_environment": {
                  "DISABLE_BETTERVR_MOD": "1"
                }
              }
            }
            EOF

            # Install graphic pack
            cp -r ${./resources/BreathOfTheWild_BetterVR} $out/share/bettervr/graphicPacks/BreathOfTheWild_BetterVR

            # Install launch script
            mkdir -p $out/bin
            cat > $out/bin/bettervr-launch <<SCRIPT
            #!/bin/sh
            BETTERVR_DIR="\$(dirname "\$(dirname "\$(readlink -f "\$0")")")"
            export VK_LAYER_PATH="\$BETTERVR_DIR/share/vulkan/implicit_layer.d:\$VK_LAYER_PATH"
            export ENABLE_BETTERVR_MOD=1

            # Symlink graphic pack into Cemu's directory if CEMU_DIR is set
            if [ -n "\$CEMU_DIR" ]; then
              mkdir -p "\$CEMU_DIR/graphicPacks"
              ln -sfn "\$BETTERVR_DIR/share/bettervr/graphicPacks/BreathOfTheWild_BetterVR" "\$CEMU_DIR/graphicPacks/BreathOfTheWild_BetterVR"
            fi

            echo "BetterVR layer enabled. Launch Cemu now."
            echo "  VK_LAYER_PATH=\$VK_LAYER_PATH"
            echo "  ENABLE_BETTERVR_MOD=\$ENABLE_BETTERVR_MOD"

            if [ \$# -gt 0 ]; then
              exec "\$@"
            fi
            SCRIPT
            chmod +x $out/bin/bettervr-launch

            runHook postInstall
          '';
        };

        cemu = cemu-bettervr;
      };
    };
}
