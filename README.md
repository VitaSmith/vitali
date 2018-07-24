Vitali - Vita License database updater
======================================

A command line utility to generate or update the `license.db` database used by
[VitaShell](https://github.com/TheOfficialFloW/VitaShell/releases) 1.76+.

The generated database, once copied over to `ux0:license/license.db`, can then
be used when invoking the _Refresh livearea_ of VitaShell, to install content
that was extracted from Sony's PKG files, regardless of whether a license was
provide during PKG extraction (provided, of course, that a corresponding
license has been imported in the database).

Compilation
-----------

Either use `build.cmd` or the `.sln` file if you are on Windows and have
Visual Studio 2017 installed, or `make` on Linux, Windows/MinGW, or 
`make -f Makefile.vita` for the Vita version.

Usage
-----

`vitali [ZRIF_URI] [DB_FILE]`

If no parameter is provided, Vitali tries to download the latest zRIF data
from the internet, and create/update a `license.db` file in the current
directory (or in `ux0:license/license.db` if using the Vita version).

If a single parameter is provided, Vitali uses it as the source of the zRIF
data. It can be either a local file or a URL.

If a second parameter is provided, it will be used as the name of the
database to process instead of the default `license.db`.

The application is designed to accept any kind of __uncompressed__ file
containing zRIFs (`.csv`, `.xml`, `.txt`, ...) as well as Microsoft's
`.xlsx` spreadsheets.
