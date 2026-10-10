# Bitcoin Core at its release tag, fetched by hash, with patches/bitcoin replayed on top.
# Each patch may touch only the files allowlisted for it below, and the build asserts that:
# consensus, validation, P2P and RPC code stay upstream's.
{ pkgs }:

let
  version = "31.1";

  upstream = pkgs.fetchFromGitHub {
    owner = "bitcoin";
    repo = "bitcoin";
    tag = "v${version}";
    hash = "sha256-Hk0RBqSlJvCe5IOPCKBj0K3CyJf9U9m9DpfkpOL09X4=";
  };

  patchDir = ../patches/bitcoin;

  # By patch number, with the reason each file is needed.
  allowlist = {
    # g_local_services and g_enabled_filter_types only accumulate across inits.
    "0001" = [ "src/init.cpp" ];
    # The proxies of an earlier init can't be unset without it.
    "0002" = [ "src/netbase.cpp" "src/netbase.h" ];
  };

  allowCases = pkgs.lib.concatStringsSep "\n" (pkgs.lib.mapAttrsToList (n: files:
    ''      ${n}) echo "${pkgs.lib.concatStringsSep " " files}" ;;'') allowlist);
in
pkgs.stdenvNoCC.mkDerivation {
  pname = "bitcoin-core-src-patched";
  inherit version;
  src = upstream;

  # git runs on the builder; under cross it must not be built for the target.
  nativeBuildInputs = [ pkgs.buildPackages.git ];

  dontConfigure = true;
  dontBuild = true;
  dontFixup = true;
  # The tree must stay byte-for-byte upstream's plus our patches.
  dontUpdateAutotoolsGnuConfigScripts = true;

  patchPhase = ''
    runHook prePatch

    allowed() {
      case "$1" in
${allowCases}
      esac
    }

    # A scratch repo, so the series replays with git am and each commit can be audited.
    git init -q .
    git config user.email "build@logos.invalid"
    git config user.name "logos-bitcoin-core-nix"
    git add -A
    git commit -q -m "bitcoin v${version}"

    for p in ${patchDir}/*.patch; do
      name=$(basename "$p")
      ok=$(allowed "''${name%%-*}")
      if [ -z "$ok" ]; then
        echo "ERROR: $name has no allowlist entry in nix/bitcoin-core-src.nix" >&2
        exit 1
      fi
      git am -q --whitespace=nowarn "$p"
      for f in $(git diff --name-only HEAD~1 HEAD); do
        case " $ok " in
          *" $f "*) ;;
          *) echo "ERROR: $name touches $f, which is not on its allowlist" >&2; exit 1 ;;
        esac
      done
      echo "ok   $name: $ok"
    done

    # Scaffolding for git am only; shipping it would make the output non-deterministic.
    rm -rf .git

    runHook postPatch
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out"
    cp -R . "$out/"
    runHook postInstall
  '';

  passthru = { inherit upstream; };

  meta = {
    description = "Bitcoin Core v${version} with the patches that let it run in-process more than once";
    license = pkgs.lib.licenses.mit;
  };
}
