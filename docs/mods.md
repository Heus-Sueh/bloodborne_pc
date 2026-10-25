# File Modifications (Mods)

The native launcher includes a **Modifications** manager: choose directory paths, toggle mods globally or individually, and adjust load order priorities. Changes are applied on the next launch. The lowest enabled mod has the highest priority; in case of conflicting files, the log indicates which mod overwrites earlier ones.

By default, the path `bbport/mods/` is used (or `~/.local/share/bbport/mods/` in packaged builds). Place extracted mods according to the following layout:

```text
mods/
  Mod Name/
    dvdroot_ps4/
      chr/...
      param/...
      menu/...
```

The virtual filesystem also automatically accepts:

- `Mod Name/app0/dvdroot_ps4/...` and `Mod Name/CUSA03173/dvdroot_ps4/...`;
- Redundant wrapper directories (e.g. from extracting an archive into a folder of the same name): `Mod Name/Mod Name v1.2/dvdroot_ps4/...`;
- Flat mod structures without a `dvdroot_ps4` parent folder containing game asset folders directly: `Mod Name/chr/...`, `Mod Name/parts/...` (`chr`, `parts`, `map`, `menu`, `msg`, `param`, `sfx`, `sound`, `font`, etc.).

### Case Insensitivity & Overlay Mechanics

Path casing inside mods is completely case-insensitive: `DVDROOT_PS4/Chr/C0000.chrbnd.dcx` from a Windows-authored mod seamlessly overrides `dvdroot_ps4/chr/c0000.chrbnd.dcx`. During game initialization, the launcher console displays the exact overlay summary:
```text
Mods: 12 game files replaced, 0 added
```

Original game files are never overwritten or modified. At launch, the runtime constructs an isolated, unified overlay in `out/mod-game-*`. File read operations, `stat`, and directory enumeration see the virtual overlay, while `/app0` and `/hostapp` remain strictly read-only.
