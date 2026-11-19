# Azahar Multisplit

Azahar Multisplit is a Libretro core for **2–4 locally linked Nintendo 3DS consoles** on one screen. It uses the Azahar emulator and is designed for playing on one PC with a separate controller for each player.

## Download

The [1.0 release](../../releases/tag/v1.0) provides two Windows x64 ZIPs:

| Download | Contents and installation |
| --- | --- |
| RetroArch | Extract `cores` and `info` into your RetroArch folder. |
| RetroBat | Extract into the folder containing `retrobat.exe`. The pack adds the **Nintendo 3DS Multijugador** system and Carbon artwork. The regular and multiplayer entries both read `roms/3ds`. |

If you already installed the RetroBat integration, you can also update it by extracting the RetroArch ZIP into `RetroBat/emulators/retroarch`.

## Playing

1. Select the Vulkan video driver in RetroArch, then restart RetroArch. The RetroBat integration selects it on launch. OpenGL Core and software rendering are also available.
2. Load a game with Azahar Multisplit. On **Select Players**, use the first controller’s D-pad to choose 2, 3 or 4 consoles and press **A** or **Start**.
3. Open the in-game local multiplayer menu on each console. In Mario Kart 7, create a group on P1 and select it from the other consoles.

Each controller is assigned to one column, in order from left to right. Audio comes from P1. **L3** swaps the screens for that console. Move the **right stick** to show the touchscreen cursor and use **R2** to tap or hold and drag. The cursor hides after four idle seconds by default. The mouse also works on the corresponding touchscreen.

With cursor control enabled, **R3** is mapped to the 3DS ZR button. In **Right Analog Function**, choose `touchscreen_pointer` to dedicate the stick to the cursor or `c_stick` to use the right stick as the 3DS C-Stick with ZR on R2. Reopen the game after changing this option.

The core keeps a separate Azahar data directory for every player under `Azahar-Multisplit/P1/Azahar`, `Azahar-Multisplit/P2/Azahar` and so on, within the frontend’s save directory.

## Source and credits

[Build instructions and upstream revision](BUILD.md)

The emulation engine comes from the [Azahar Emulator Project](https://github.com/azahar-emu/azahar) and the Citra project. This repository contains the Libretro wrapper and the patches used to build its embedded engine. See [LICENSE](LICENSE) and [LICENSE-AZAHAR.txt](LICENSE-AZAHAR.txt).

The optional RetroBat pack includes adapted Carbon artwork by Fabrice Caruso, based on work by Eric Hettervik and Nils Bonenberger, with the artwork credits and license in the ZIP.

---

**En español.** En RetroBat, descomprime el paquete en la carpeta donde está `retrobat.exe` y abre **Nintendo 3DS Multijugador**. Elige 2, 3 o 4 jugadores con el primer mando. Cada jugador mueve su cursor táctil con el stick derecho y toca con **R2**. El README en español dentro de cada ZIP incluye la instalación y los controles.
