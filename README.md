Gelida - GEnerate LIcense DAtabase, for PS Vita
===============================================

This command line utility can be used to generate/update the `license.db`, used
by VitaShell 1.76+ to insert licenses when refreshing content from PKG data files
that don't already contain a `work.bin`.

The utility accepts any kind of file containing zRIFs (`.csv`, `.xml`, `.txt`),
and, if no other parameters are passe, will create a default `license.db`, which
you can then copy to `ux0:license/license.db`.

Compilation
-----------

Either use `build.cmd` if you are on Windows and have Visual Studio 2017 installed,
or `make` for Linux, Windows/MinGW and other platforms.

Usage
-----

`gelida ZRIF_FILE [DB_FILE]`

`DB_FILE` is optional. If not specified, the default `license.db` will be used.
