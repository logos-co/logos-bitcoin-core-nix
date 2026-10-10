{
  description = "Logos bitcoin-core-nix: Bitcoin Core's node, patched to run in-process and built as libbitcoind_c.";

  inputs = {
    # logos-nix roots the family's nixpkgs pin and owns the mingw cross package set.
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
  };

  outputs = { self, nixpkgs, logos-nix, ... }:
    let
      lib = nixpkgs.lib;
      nativeSystems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];

      # Editing the harness rebuilds only the harness.
      shimSrc = lib.fileset.toSource { root = ./shim; fileset = ./shim; };
      testSrc = lib.fileset.toSource { root = ./test; fileset = ./test; };

      perSystem = pkgs:
        let
          coreSrc = import ./nix/bitcoin-core-src.nix { inherit pkgs; };
          libevent = import ./nix/libevent.nix { inherit pkgs coreSrc; };
          libbitcoind_c = import ./nix/bitcoind-c.nix { inherit pkgs coreSrc libevent shimSrc; };
          harness = import ./nix/harness.nix { inherit pkgs libbitcoind_c testSrc; };
        in
        {
          packages = {
            default = libbitcoind_c;
            inherit libbitcoind_c libevent;
            bitcoin-core-src = coreSrc;
            bitcoind-c-test = harness.bin;
          };
          checks = {
            harness = harness.check;
          };
        };

      native = lib.genAttrs nativeSystems (system: perSystem (import nixpkgs { inherit system; }));

      # x86_64-windows: a pseudo-system that cross builds from x86_64-linux, as across the family.
      windows = perSystem (logos-nix.lib.mkWindowsPkgs { buildSystem = "x86_64-linux"; });
    in
    {
      packages = lib.mapAttrs (_: s: s.packages) native // { x86_64-windows = windows.packages; };
      checks = lib.mapAttrs (_: s: s.checks) native;
    };
}
