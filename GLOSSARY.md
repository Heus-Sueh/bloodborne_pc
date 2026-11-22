# Bloodborne PC Port

A native PC port of Bloodborne (PS4) and its companion launchers, runtime, and configuration contracts.

## Directories

**Port Directory**:
The read-only or source directory holding the port's executables, launch scripts, and shared settings schema.
_Avoid_: Repo root, package dir, app dir

**Data Directory**:
The writable directory holding `bbport.ini`, `mods.json`, `patches.json`, default mods and external patches folders, and default saves.
_Avoid_: Base dir, working dir

**Config Directory**:
The per-user configuration directory holding the launcher's `settings.json`.
_Avoid_: Profile dir

**Game Directory**:
The unpacked PS4 Bloodborne directory containing `eboot.bin` and `dvdroot_ps4/`.
_Avoid_: Dump dir, ROM dir

**Saves Directory**:
The directory holding player saves, shader cache, and unlocked trophy records (`trophies.jsonl`).
_Avoid_: User dir, save folder

## Configuration & Content

**Game Setting**:
A setting persisted in `bbport.ini` and shared between the launcher and the in-game menu.
_Avoid_: INI setting, runtime toggle

**Launcher Setting**:
A setting persisted in `settings.json` that configures the launcher itself or is compiled into launch environment variables before the game starts.
_Avoid_: App setting, preference

**Mod**:
A loose-file override folder placed in the mods directory and ordered in `mods.json`.
_Avoid_: Addon, override pack

**External Patch**:
A shadPS4 XML binary patch whose enabled state is stored as a delta override in `patches.json`.
_Avoid_: XML mod, cheat
