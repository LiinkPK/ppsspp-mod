PPSSPP - a fast and portable PSP emulator
=========================================

Created by Henrik Rydgård

Forked from: https://www.github.com/hrydgard/PPSSPP

Official website: https://www.ppsspp.org/

# PPSSPP — GIM/GMO Texture Dumper Mod
**Version:** v1.20.3-LiinkPK-mod  
**Base:** PPSSPP v1.20.3  
**Author:** Link García  
**Games working:** 
- Dissidia 012 (ULES01505)
- Tenchu: Shadow Assassins (ULUS10419)
- Yu-Gi-Oh! GX Tag Force (ULES000600)
- Jack And Daxter: The Lost Frontier (UCES01225)
- Digivice Ver. Portable (NPJH00126)

---

## What this mod adds

This is an unofficial build of PPSSPP with two additional texture extraction tools added to the **Developer Tools** screen (`Settings > Tools > Developer Tools > Texture Replacement`).

These tools are designed for PSP games that store textures in **GIM** and **GMO** formats such as Dissidia 012 and similar titles from the same era.

---

## Dump All Textures From Folder

Extracts textures from a folder of `.gmo` and `.gim` files on your PC (e.g. files extracted from the ISO via another tool).

**How it works:**
- You select a root folder containing `.gmo` / `.gim` files (searched recursively).
- Each file is scanned for embedded GIM image blocks by their magic bytes (`MIG.00.1PSP`).
- Each GIM block is loaded into PSP RAM and passed to PPSSPP's internal texture dumper.
- Textures that need unswizzling are detected via the `c16` field in the GIM block header (`c16 == 8` → unswizzle). Stage/environment GMOs containing geometry (`Shape` string present) are unswizzled; character GMOs are not, even if the flag is set.
- Output PNGs are saved to: `Documents\PPSSPP\PSP\TEXTURES\<GameID>\new\<filename>\`

> [!NOTE]
> - GMOs that contain only animation/skeleton data (no texture blocks) are silently skipped.
> - If a GIM block causes a memory fault it is caught and skipped; processing continues with the next file.

---

## Dump All Textures From ISO

Extracts textures directly from the currently running game's ISO filesystem without needing any external files.

**How it works:**
- Scans all files on `disc0:/PSP_GAME/USRDIR` recursively, including `.bin`, `.dat`, `.pac` and other container formats.
- Each file is scanned in 4MB chunks (with 64KB overlap) for GIM magic bytes.
- Found GIM blocks are loaded into PSP RAM and passed to the texture dumper.
- Swizzle detection uses the same `c16` field as the Folder dumper.
- Output PNGs go to the default PPSSPP texture replacement folder for the current game.

> [!NOTE]
> - This tool finds GIMs embedded inside any container format, not just standalone `.gim` files.
> - Swizzle correction via `c16` is applied, so textures that were previously exported scrambled should now export correctly.

---

## Installation

1. Make sure you already have [PPSSPP v1.20.3](https://github.com/hrydgard/ppsspp/releases/tag/v1.20.3) installed.
2. Close PPSSPP if it is running.
3. Extract `PPSSPP-LiinkPK-mod.zip` on PPSSPP's installation folder.
4. Replace the existing files, including `PPSSPPWindows64.exe`.
5. Launch PPSSPP normally.

The tools are found at:  
`Settings → Tools → Developer Tools → Texture Replacement`

---

> [!IMPORTANT]
> - This is an **unofficial mod** and is not affiliated with or endorsed by the PPSSPP project.
> - Only the Windows 64-bit build is provided.
> - The PSP firmware version reported by this build is unchanged (6.60) for game compatibility.
> - Do not submit bug reports about this build to the official PPSSPP issue tracker.
