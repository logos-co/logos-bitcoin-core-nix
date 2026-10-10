# libevent as Core's own depends builds it (depends/packages/libevent.mk): 2.1.12-stable at
# depends' hash, Core's three patches, static and position-independent so it links into the
# shared library. nixpkgs' libevent is shared-only, and its mingw build does not compile.
{ pkgs, coreSrc }:

let
  isWin = pkgs.stdenv.hostPlatform.isWindows;
  patchDir = "${coreSrc}/depends/patches/libevent";
in
pkgs.stdenv.mkDerivation {
  pname = "libevent-static";
  version = "2.1.12-stable";

  src = pkgs.fetchurl {
    url = "https://github.com/libevent/libevent/releases/download/release-2.1.12-stable/libevent-2.1.12-stable.tar.gz";
    hash = "sha256-kubeG+nsF2Qo/SNnZ35hzv/C7hyxGQNQN6J9NGsEA7s=";
  };

  patches = [
    "${patchDir}/cmake_fixups.patch"
    "${patchDir}/netbsd_fixup.patch"
    "${patchDir}/winver_fixup.patch"
  ];

  nativeBuildInputs = [ pkgs.buildPackages.cmake ];

  cmakeFlags = [
    "-DEVENT__DISABLE_BENCHMARK=ON"
    "-DEVENT__DISABLE_OPENSSL=ON"
    "-DEVENT__DISABLE_SAMPLES=ON"
    "-DEVENT__DISABLE_REGRESS=ON"
    "-DEVENT__DISABLE_TESTS=ON"
    "-DEVENT__LIBRARY_TYPE=STATIC"
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
  ];

  # depends' flags.
  env.NIX_CFLAGS_COMPILE = toString ([ "-D_GNU_SOURCE" ]
    ++ pkgs.lib.optionals isWin [ "-D_WIN32_WINNT=0x0A00" ]);

  # What depends deletes after staging: compat headers, the all-in-one archive, pkg-config.
  postInstall = ''
    rm -rf "$out/bin" "$out/lib/pkgconfig" "$out"/include/ev*.h "$out"/include/event2/*_compat.h
    rm -f "$out/lib/libevent.a"
    install -Dm0644 ../LICENSE "$out/share/licenses/libevent/LICENSE"
  '';

  meta = {
    description = "libevent 2.1.12, static, as Bitcoin Core's depends builds it";
    license = pkgs.lib.licenses.bsd3;
  };
}
