# TamaPoke shop item art v3

This directory contains the 120 shop-item illustrations used by the firmware.

- `source/`: 96x96 working PNG files.
- `firmware_32/`: the exact 32x32, 15-color pixel art rendered on the device.
- `OPENMOJI_LICENSE.txt`: license for the OpenMoji source silhouettes.

Food, toys, medicine, equipment and props start from recognizable OpenMoji
object silhouettes, then receive TamaPoke-specific cropping, color reduction,
status badges and pixel treatment. Travel landmarks and unsupported objects are
drawn specifically for this project. `tools/build_shop_item_art.py` regenerates
the PNG files, `shop_icons.h`, and both preview sheets in one run.

The firmware stores each icon as a 16-color RGB565 palette plus packed 4-bit
pixels. Palette index zero is transparent, so the same art works on shop cards,
item details, inventory cards and action selectors without a square backdrop.
