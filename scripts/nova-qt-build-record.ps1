function ConvertTo-NovaQtBuildOptions($Options) {
    @($Options | ForEach-Object {
        $value = [string]$_
        switch -Regex ($value) {
            '^-DCMAKE_INSTALL_PREFIX=' { '-DCMAKE_INSTALL_PREFIX=<qt-prefix>'; break }
            '^-DCMAKE_C_COMPILER=' { '-DCMAKE_C_COMPILER=<ucrt64-bin>/gcc.exe'; break }
            '^-DCMAKE_CXX_COMPILER=' { '-DCMAKE_CXX_COMPILER=<ucrt64-bin>/g++.exe'; break }
            '^-DCMAKE_MAKE_PROGRAM=' { '-DCMAKE_MAKE_PROGRAM=<ucrt64-bin>/ninja.exe'; break }
            default { $value }
        }
    })
}

function Get-NovaQtPackageLines([string]$Path) {
    @([IO.File]::ReadAllLines($Path) | ForEach-Object { [string]$_ })
}
