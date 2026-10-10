# libbitcoind_c: Bitcoin Core's node, run in-process, as a shared library with a C ABI.
# Native on Linux and macOS; on Windows a cross build with logos-nix's MinGW (mcf, UCRT).
{ pkgs, coreSrc, libevent, shimSrc }:

let
  inherit (pkgs) lib stdenv;
  isDarwin = stdenv.hostPlatform.isDarwin;
  isWin = stdenv.hostPlatform.isWindows;
  ext = stdenv.hostPlatform.extensions.sharedLibrary;
  # Header-only (multi_index, signals2), so the build platform's copy serves a cross build
  # too; nixpkgs' mingw Boost does not build.
  boost = if isWin then pkgs.buildPackages.boost else pkgs.boost;
  objdump = "${stdenv.cc.bintools.bintools}/bin/${stdenv.cc.targetPrefix}objdump";
in
stdenv.mkDerivation {
  pname = "bitcoind_c";
  version = coreSrc.version;

  src = shimSrc;

  nativeBuildInputs = [ pkgs.buildPackages.cmake pkgs.buildPackages.ninja ];
  buildInputs = [ boost libevent ];

  cmakeFlags = [
    "-DBITCOIN_SRC=${coreSrc}"
    # Header paths reach __FILE__ through asserts in inline code.
    "-DLOGOS_EXTRA_PREFIX_MAPS=${boost.dev}=/boost;${libevent}=/libevent"
  ];

  installPhase = ''
    runHook preInstall
    cmake --install . --component bitcoind_c --prefix "$out"
    # Core is MIT; the subtrees and libevent carry their own notices. lib/ is where module
    # staging looks.
    install -Dm0644 ${coreSrc}/COPYING "$out/lib/COPYING.bitcoin"
    {
      for f in src/leveldb/LICENSE src/secp256k1/COPYING src/crc32c/LICENSE src/minisketch/LICENSE; do
        echo "==> $f"; cat "${coreSrc}/$f"; echo
      done
      echo "==> libevent LICENSE"; cat ${libevent}/share/licenses/libevent/LICENSE
    } > "$out/lib/NOTICES.bitcoind_c"
    runHook postInstall
  '';

  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck
  '' + (if isWin then ''
    pe=$(${objdump} -p "$out/lib/libbitcoind_c.dll")
    # Exactly the functions the header declares, as ci/check-exports.sh asserts on Unix.
    want=$(grep -o -E 'BITCOIND_[a-z_]+\(' "$out/include/bitcoind_c.h" | tr -d '(' | sort -u)
    got=$(echo "$pe" | awk '/^\[Ordinal\/Name Pointer\] Table/ { t = 1; next } t && /^\t\[/ { print $NF } t && /^$/ { t = 0 }' | sort -u)
    [ "$got" = "$want" ] || { echo "exports of libbitcoind_c.dll differ from bitcoind_c.h:" >&2; diff <(echo "$want") <(echo "$got") >&2; exit 1; }
    echo "exports: $(echo "$got" | wc -l) BITCOIND_* functions and nothing else"
    if echo "$pe" | grep -qi 'DLL Name: msvcrt.dll'; then
      echo "libbitcoind_c.dll imports msvcrt.dll beside the UCRT" >&2; exit 1
    fi
    echo "imports: $(echo "$pe" | awk '/DLL Name:/ { print $3 }' | sort -fu | tr '\n' ' ')"
  '' else ''
    bash ${../ci/check-exports.sh} "$out/lib/libbitcoind_c${ext}" "$out/include/bitcoind_c.h" \
      ${if isDarwin then "${pkgs.cctools}/bin/nm" else "nm"}
  '') + ''
    runHook postInstallCheck
  '';

  # strip would invalidate the linker's ad-hoc signature on macOS.
  dontStrip = isDarwin;

  passthru = { inherit coreSrc libevent; };

  meta = {
    description = "Bitcoin Core's node, run in-process behind a C ABI";
    license = with lib.licenses; [ mit bsd3 ];
  };
}
