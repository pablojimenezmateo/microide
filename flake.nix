{
  description = "microide - a native, low-footprint keyboard-first IDE";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      lib = nixpkgs.lib;

      # Install rules are gated `if(UNIX AND NOT APPLE)` in CMakeLists.txt, so the
      # flake targets Linux only.
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f:
        lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Single source of truth: read the version straight from CMakeLists.txt's
      # `project(microide VERSION x.y.z ...)` line instead of duplicating it here.
      version =
        let
          projectLine = lib.findFirst (l: lib.hasInfix "project(microide VERSION" l) null
            (lib.splitString "\n" (builtins.readFile ./CMakeLists.txt));
          m = if projectLine == null then null
              else builtins.match ".*VERSION ([0-9.]+).*" projectLine;
        in
        if m == null then throw "flake.nix: could not parse version from CMakeLists.txt"
        else builtins.head m;
    in
    {
      packages = forAllSystems (pkgs:
        let
          # Dependency lists defined once and reused across package / devShell /
          # checks. SDL3's transitive X11/Wayland/GL/freetype come from the sdl3
          # closure; the `util` lib microide links lives in glibc.
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            pkgs.lld
            # Provides wrapProgram (postFixup) to make the host's X cursor themes
            # discoverable at runtime — see the postFixup note below.
            pkgs.makeWrapper
          ];
          buildInputs = [
            pkgs.sdl3
            pkgs.sdl3-ttf
            pkgs.pcre2
            pkgs.lua5_4
            # fontconfig gives the font-family picker real, weight-deduped family
            # names. CMake auto-detects it via pkg-config (soft dependency: absent
            # → filesystem-scan fallback), so make it explicit here rather than
            # relying on it appearing transitively in the SDL3/freetype closure.
            pkgs.fontconfig
          ];

          microide = pkgs.stdenv.mkDerivation {
            pname = "microide";
            inherit version;
            src = self;

            inherit nativeBuildInputs buildInputs;

            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              # LTO for runtime speed (the project's perf preset uses IPO). CMake
              # auto-skips ld.lld under IPO and uses the gcc LTO linker.
              "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON"
              # ccache is pointless in a clean sandbox build.
              "-DMICROIDE_USE_CCACHE=OFF"
              # SDL3_ttf + Lua stay ON (upstream defaults) for the full-featured
              # binary. Pin the Lua paths explicitly so configuration does not
              # depend on the nixpkgs pkg-config name for lua5.4.
              "-DMICROIDE_LUA_INCLUDE_DIR=${pkgs.lua5_4}/include"
              "-DMICROIDE_LUA_LIBRARY=${pkgs.lib.getLib pkgs.lua5_4}/lib/liblua.so"
            ];

            # Build only the shipped binary (skip tests/benches) for build speed.
            ninjaFlags = [ "microide" ];

            # The existing install() rules place the binary at $out/bin, assets at
            # $out/share/microide/assets, plus man page, .desktop and hicolor
            # icons. The binary resolves assets via <exe_dir>/../share/microide
            # (SDL_GetBasePath), so no patching is needed.

            # microide draws every mouse cursor with SDL_CreateSystemCursor /
            # SDL_GetDefaultCursor, i.e. it relies entirely on the host's X cursor
            # theme. The Nix-built SDL3 dlopens the Nix-built libXcursor, whose
            # compiled-in default cursor search path points into the Nix store
            # rather than /usr/share/icons. On a non-NixOS host with XCURSOR_PATH
            # unset, no named theme resolves, so SDL falls back to libX11 core
            # cursors (tiny, monochrome). Point the search path at the standard
            # host locations so the desktop's theme is discoverable; any
            # XCURSOR_PATH the user already exports still wins (prepended).
            #
            # Deliberately do NOT set XCURSOR_SIZE: on X11 libXcursor reads the
            # desktop-configured size from the X resource database (Xcursor.size),
            # and an XCURSOR_SIZE env var OVERRIDES that — pinning a fixed size
            # that ignores the desktop and HiDPI scaling (the classic "cursor is
            # tiny" symptom). Leaving it unset lets X11 honor xrdb/DPI and Wayland
            # honor the output scale. The wrapper is a one-time exec with no
            # runtime cost, and SDL_GetBasePath still resolves assets via the real
            # .microide-wrapped binary's dir.
            postFixup = ''
              wrapProgram $out/bin/microide \
                --run 'export XCURSOR_PATH="''${XCURSOR_PATH:+$XCURSOR_PATH:}$HOME/.icons:$HOME/.local/share/icons:/usr/share/icons:/usr/local/share/icons:/usr/share/pixmaps"'
            '';

            meta = with pkgs.lib; {
              description = "A native, low-footprint keyboard-first IDE";
              homepage = "https://github.com/pablojimenezmateo/microide";
              license = licenses.mit;
              platforms = systems;
              mainProgram = "microide";
            };
          };
        in
        {
          default = microide;
          inherit microide;
        });

      apps = forAllSystems (pkgs:
        let system = pkgs.stdenv.hostPlatform.system; in
        rec {
          default = microide;
          microide = {
            type = "app";
            program = "${self.packages.${system}.microide}/bin/microide";
          };
        });

      devShells = forAllSystems (pkgs:
        {
          default = pkgs.mkShell {
            # Canonical dev environment: the same build inputs as the package,
            # plus inner-loop helpers. Replaces the hand-maintained apt list.
            packages = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.lld
              pkgs.ccache
              pkgs.gdb
              pkgs.sdl3
              pkgs.sdl3-ttf
              pkgs.pcre2
              pkgs.lua5_4
              pkgs.fontconfig
            ];
          };
        });

      checks = forAllSystems (pkgs:
        let
          buildInputs = [
            pkgs.sdl3
            pkgs.sdl3-ttf
            pkgs.pcre2
            pkgs.lua5_4
            pkgs.fontconfig
          ];
        in
        {
          # `nix flake check` builds the test binary and runs ctest headless.
          # No LTO here (keep the test build fast); lld speeds the relink.
          tests = pkgs.stdenv.mkDerivation {
            pname = "microide-tests";
            inherit version;
            src = self;

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.lld
            ];
            inherit buildInputs;

            # Tools the test suite shells out to at runtime: the git CLI
            # (patch/stage/blame fixtures) and python3 (subprocess-handling
            # fixtures). bash + coreutils come from stdenv.
            nativeCheckInputs = [ pkgs.git pkgs.python3 ];

            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              "-DMICROIDE_USE_CCACHE=OFF"
              "-DMICROIDE_LUA_INCLUDE_DIR=${pkgs.lua5_4}/include"
              "-DMICROIDE_LUA_LIBRARY=${pkgs.lib.getLib pkgs.lua5_4}/lib/liblua.so"
            ];

            # Build the `microide` target too: its POST_BUILD step copies assets
            # next to the binaries (shared CMAKE_RUNTIME_OUTPUT_DIRECTORY), so the
            # test binary finds fonts/themes via <exe_dir>/assets at runtime.
            ninjaFlags = [ "microide" "microide_tests" ];

            doCheck = true;
            checkPhase = ''
              runHook preCheck
              export HOME="$TMPDIR"
              export SDL_VIDEODRIVER=dummy
              export SDL_AUDIODRIVER=dummy
              # ctest runs the test binary from the build tree, before fixupPhase
              # finalizes its RUNPATH, so make its direct dependency libs
              # discoverable: the project deps plus the stdenv C++ runtime
              # (libstdc++/libgcc). SDL3's own transitive deps resolve via their
              # libraries' embedded store RUNPATHs.
              export LD_LIBRARY_PATH="${pkgs.lib.makeLibraryPath (buildInputs ++ [ pkgs.stdenv.cc.cc.lib ])}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
              ctest --test-dir . --output-on-failure
              runHook postCheck
            '';

            # This derivation only validates; nothing to install.
            installPhase = "touch $out";
          };
        });
    };
}
