{ pkgs ? import <nixpkgs> {} }: {
    default = pkgs.mkShell {
        buildInputs = with pkgs; [ 
            clang-tools
            cmake-language-server
            gcc
            cmake
            ninja
            gdb
            gtk4
            sqlitecpp
            sqlite
            pkg-config
        ];
    };
}
