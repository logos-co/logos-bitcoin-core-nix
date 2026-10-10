# The C test harness, and a check that runs it against the built library: three regtest
# cycles and a regtest -> signet -> regtest switch, all offline. Tor runs by hand (README).
# On Windows the output is a runnable directory: the .exe beside the DLLs it imports.
{ pkgs, libbitcoind_c, testSrc }:

let
  inherit (pkgs) lib stdenv;
  isWin = stdenv.hostPlatform.isWindows;
  exe = "bitcoind-c-test${stdenv.hostPlatform.extensions.executable}";
  objdump = "${stdenv.cc.bintools.bintools}/bin/${stdenv.cc.targetPrefix}objdump";

  bin = stdenv.mkDerivation {
    pname = "bitcoind-c-test";
    inherit (libbitcoind_c) version;
    src = testSrc;
    dontConfigure = true;
    buildPhase = ''
      runHook preBuild
      $CC -std=c11 -O2 -Wall -Wextra -o ${exe} bitcoind_c_test.c \
        -I${libbitcoind_c}/include -L${libbitcoind_c}/lib -lbitcoind_c \
        ${if isWin then "-lws2_32" else "-Wl,-rpath,${libbitcoind_c}/lib"}
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      install -Dm0755 ${exe} "$out/bin/${exe}"
    '' + lib.optionalString isWin ''
      install -m0755 ${libbitcoind_c}/lib/libbitcoind_c.dll "$out/bin/"
      # Whatever else the two import beyond Windows itself ships beside them.
      for dll in $(${objdump} -p "$out/bin/${exe}" "$out/bin/libbitcoind_c.dll" | awk '/DLL Name:/ { print $3 }' | sort -u); do
        [ -e "$out/bin/$dll" ] && continue
        for dir in ${lib.concatMapStringsSep " " (p: "${p}/bin ${p}/lib") ([ stdenv.cc.cc.lib pkgs.windows.mcfgthreads ])}; do
          found=$(find "$dir" -maxdepth 3 -iname "$dll" 2>/dev/null | head -1)
          if [ -n "$found" ]; then install -m0755 "$found" "$out/bin/"; break; fi
        done
      done
      echo "bundled: $(ls "$out/bin" | tr '\n' ' ')"
    '' + ''
      runHook postInstall
    '';
    meta.mainProgram = exe;
  };
in
{
  inherit bin;

  check = pkgs.runCommand "bitcoind-c-test-check" {
    # Loopback only; -connect=0 keeps every node off the network.
    __darwinAllowLocalNetworking = true;
  } ''
    set -o pipefail
    export HOME="$TMPDIR"
    ${bin}/bin/${exe} "$TMPDIR/work" | tee "$out"
  '';
}
