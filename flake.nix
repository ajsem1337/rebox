{
  description = "Run dumped Xbox Series GDK titles under WineGDK + vkd3d-proton with AnyPS5-translated shaders";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    # Archived 2026-11-13; successor is xodus-gaming/{wine,xgameruntime}, but
    # that runtime is still a skeleton, so WineGDK remains the most complete.
    winegdk-src = {
      url = "github:Weather-OS/WineGDK";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, winegdk-src }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      lib = pkgs.lib;
      mingwCC = pkgs.pkgsCross.mingwW64.stdenv.cc;
      python = pkgs.python3.withPackages (p: [ p.pefile p.capstone ]);

      pyTool = name: file: pkgs.runCommand name { nativeBuildInputs = [ pkgs.makeWrapper ]; } ''
        makeWrapper ${python}/bin/python3 $out/bin/${name} --add-flags ${file} \
          --suffix PATH : ${mingwCC}/bin
      '';
    in
    {
      packages.${system} = rec {
        winegdk = pkgs.wineWow64Packages.unstable.overrideAttrs (old: {
          pname = "winegdk";
          version = "11.14-gdk-${winegdk-src.shortRev}";
          src = winegdk-src;
        });

        # The nixpkgs vkd3d-proton package is the native Linux build; Wine
        # needs the PE DLLs from the release tarball.
        vkd3d-proton-win = pkgs.stdenvNoCC.mkDerivation rec {
          pname = "vkd3d-proton-win";
          version = "3.0.1";
          src = pkgs.fetchurl {
            url = "https://github.com/HansKristian-Work/vkd3d-proton/releases/download/v${version}/vkd3d-proton-${version}.tar.zst";
            hash = "sha256-PPIxVSKvXkNgXvbTxB2tkThwQL+XGZk08/erdsqqLww=";
          };
          nativeBuildInputs = [ pkgs.zstd ];
          installPhase = "mkdir -p $out && cp -r x64 x86 $out/";
        };

        # RADV that honours requested device addresses in the lower half of
        # the VA space, so GPU-visible console memory can sit at its CPU
        # address (docs/d3d12x.md). Only the drivers this project needs.
        # Use with VK_ICD_FILENAMES=$out/share/vulkan/icd.d/radeon_icd.x86_64.json.
        # Host Vulkan layer that maps host-visible memory at identical GPU
        # and CPU addresses (layers/gpuva_identity.c).
        # Static mingw libavcodec/libavutil with only FFmpeg's XMA2 decoder,
        # linked into shims/xaudio2_9 (nixpkgs' mingw FFmpeg is marked broken).
        ffmpeg-xma2 = pkgs.stdenv.mkDerivation {
          pname = "ffmpeg-xma2-mingw";
          inherit (pkgs.ffmpeg) version src;
          nativeBuildInputs = [ mingwCC pkgs.nasm pkgs.pkg-config pkgs.perl ];
          dontAddPrefix = true;
          dontStrip = true; # native strip on mingw archives
          configurePhase = ''
            ./configure --prefix=$out --target-os=mingw32 --arch=x86_64 --enable-cross-compile \
              --cross-prefix=x86_64-w64-mingw32- --disable-everything --enable-decoder=xma2 \
              --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-swscale \
              --disable-swresample --disable-avfilter --disable-network --disable-debug \
              --enable-static --disable-shared --disable-autodetect --disable-asm
          '';
        };

        gpuva-layer = pkgs.stdenv.mkDerivation {
          name = "rebox-gpuva-layer";
          src = lib.fileset.toSource { root = ./.; fileset = ./layers; };
          buildInputs = [ pkgs.vulkan-headers ];
          buildPhase = ''
            $CC -O2 -g -Wall -fPIC -shared -fvisibility=hidden \
              -o librebox_gpuva_identity.so layers/gpuva_identity.c -lpthread
          '';
          installPhase = ''
            mkdir -p $out/lib $out/share/vulkan/explicit_layer.d
            cp librebox_gpuva_identity.so $out/lib/
            sed "s|./librebox_gpuva_identity.so|$out/lib/librebox_gpuva_identity.so|" \
              layers/VK_LAYER_REBOX_gpuva_identity.json \
              > $out/share/vulkan/explicit_layer.d/VK_LAYER_REBOX_gpuva_identity.json
          '';
        };

        radv-rebox = (pkgs.mesa.override {
          galliumDrivers = [ "llvmpipe" "radeonsi" ];
          vulkanDrivers = [ "amd" "swrast" ];
          vulkanLayers = [ ];
        }).overrideAttrs (old: {
          pname = "radv-rebox";
          patches = (old.patches or [ ]) ++ [ ./patches/mesa-radv-low-va-replay.patch ];
        });

        # Only dxgi.dll is used: vkd3d-proton needs a DXGI that enumerates
        # Vulkan adapters without a display, as in Proton.
        dxvk-win = pkgs.stdenvNoCC.mkDerivation rec {
          pname = "dxvk-win";
          version = "3.1.1";
          src = pkgs.fetchurl {
            url = "https://github.com/doitsujin/dxvk/releases/download/v${version}/dxvk-${version}.tar.gz";
            hash = "sha256-QFZbSnJKrcRDP6TgELSyORbZsfG67uZOFxhtuU9U5gg=";
          };
          installPhase = "mkdir -p $out && cp -r x64 x32 $out/";
        };

        shims = pkgs.stdenvNoCC.mkDerivation {
          name = "rebox-shims";
          src = lib.fileset.toSource {
            root = ./.;
            fileset = lib.fileset.unions [
              ./Makefile
              ./shims
              (lib.fileset.maybeMissing ./generated)
            ];
          };
          nativeBuildInputs = [ mingwCC ];
          makeFlags = [ "MINGW_CC=x86_64-w64-mingw32-gcc" "FFMPEG_XMA2=${ffmpeg-xma2}" ];
          installPhase = "mkdir -p $out && cp build/*.dll $out/";
        };

        scan-imports = pyTool "scan_imports.py" ./tools/scan_imports.py;
        anyps5-cache = pyTool "anyps5_cache.py" ./tools/anyps5_cache.py;
        # mingw gcc on PATH lets the probe find d3d12.h for GUID and vtable names.
        d3d12x-probe = pyTool "d3d12x_probe.py" ./tools/d3d12x_probe.py;

        rebox-run = pkgs.writeShellScriptBin "rebox-run" ''
          export PATH=${lib.makeBinPath [ pkgs.coreutils pkgs.gnugrep pkgs.gnused ]}:$PATH
          export WINE=''${WINE:-${winegdk}/bin/wine}
          export VKD3D_PROTON_DIR=''${VKD3D_PROTON_DIR:-${vkd3d-proton-win}/x64}
          export DXVK_DIR=''${DXVK_DIR:-${dxvk-win}/x64}
          export REBOX_SHIMS_DIR=''${REBOX_SHIMS_DIR:-${shims}}
          export REBOX_SCAN=${scan-imports}/bin/scan_imports.py
          export REBOX_GPUVA_LAYER_DIR=''${REBOX_GPUVA_LAYER_DIR:-${gpuva-layer}/share/vulkan/explicit_layer.d}
          export REBOX_RADV_ICD=''${REBOX_RADV_ICD:-${radv-rebox}/share/vulkan/icd.d/radeon_icd.x86_64.json}
          exec ${pkgs.bash}/bin/bash ${./bin/rebox-run} "$@"
        '';

        default = rebox-run;
      };

      apps.${system} = {
        default = { type = "app"; program = "${self.packages.${system}.rebox-run}/bin/rebox-run"; };
        scan-imports = { type = "app"; program = "${self.packages.${system}.scan-imports}/bin/scan_imports.py"; };
        anyps5-cache = { type = "app"; program = "${self.packages.${system}.anyps5-cache}/bin/anyps5_cache.py"; };
        d3d12x-probe = { type = "app"; program = "${self.packages.${system}.d3d12x-probe}/bin/d3d12x_probe.py"; };
      };

      # Stock Wine here so the shell doesn't need a WineGDK build; the shims
      # and the smoke test don't depend on WineGDK.
      devShells.${system}.default = pkgs.mkShell {
        packages = [ mingwCC python pkgs.gnumake pkgs.wineWow64Packages.unstable pkgs.vkd3d pkgs.vulkan-tools pkgs.vulkan-headers pkgs.vulkan-loader pkgs.directx-shader-compiler ];
        shellHook = ''
          export WINE=wine
          export VKD3D_PROTON_DIR=${self.packages.${system}.vkd3d-proton-win}/x64
          export DXVK_DIR=${self.packages.${system}.dxvk-win}/x64
          export REBOX_SHIMS_DIR=$PWD/build
          export FFMPEG_XMA2=${self.packages.${system}.ffmpeg-xma2}
          export WINE_DLL_DIR=${pkgs.wineWow64Packages.unstable}/lib/wine/x86_64-windows
          # Software Vulkan only for machines (or sandboxes) without a GPU;
          # otherwise the system's drivers (e.g. RADV) are used.
          if [ -z "''${VK_ICD_FILENAMES:-}" ] && ! ls /dev/dri/renderD* >/dev/null 2>&1; then
            export VK_ICD_FILENAMES=${pkgs.mesa}/share/vulkan/icd.d/lvp_icd.x86_64.json
          fi
        '';
      };
    };
}
