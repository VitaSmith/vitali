Gelida - GEnerate LIcense DAtabase
==================================

A command line utility to generate or update the `license.db` database used by
[VitaShell](https://github.com/TheOfficialFloW/VitaShell/releases) 1.76+.

The generated database, once copied over to `ux0:license/license.db`, can then
be used when invoking the _Refresh livearea_ of VitaShell, to install content
that was extracted from Sony's PKG files, regardless of whether a license was
provide during PKG extraction (provided, of course, that a corresponding
license has been imported in the database).

Compilation
-----------

Either use `build.cmd` if you are on Windows and have Visual Studio 2017
installed, or `make` for Linux, Windows/MinGW and other platforms.

Usage
-----

`gelida ZRIF_FILE [DB_FILE]`

The utility accepts any kind of __uncompressed__ file containing zRIFs (`.csv`,
`.xml`, `.txt`, ...) as well as Microsoft's `.xlsx` spreadsheets, which you
should provide as the first parameter.

Optionally you can also provide a `DB_FILE` parameter, with the name of the
database file you want to generate. If this parameter is not provide, then the
default `license.db` name will be used.
