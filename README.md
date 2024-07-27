# StackChanTesterRT

[Japanese](README_ja.md)

A stack-chan tester program for stackchan RT ver. (M5Stack CoreS3 / DYNAMIXEL servo),
inspired by [mongonta0716/stack-chan-tester](https://github.com/mongonta0716/stack-chan-tester).

## Supported board

Stack-chan RT ver. <https://github.com/rt-net/stack-chan>

## How to download into the stack-chan

Build and download into the M5Stack CoreS3 with Platform IO.

```sh
pio -f run --target upload
```

Remember to enter the download mode, otherwise the download will fail.
To enter the download mode, press and hold the reset button 3sec (emit green light).

## Usage

 * Virtual Button A (Left): Rotates servos to face front.
 * Virtual Button B (Center): Test servos: Pivot left/right at -90° to 90° on X axis (roll axis); pivot up/down at -15° to 10° on Y axis (pitcha axis).
 * Virtual Button C (Right): Move in ramdom mode.
   * Virtual Button C (Right): Stop random mode.
   * Virtual Button C (Right) - double tap: Hide battery icon.
 * Virtual Button A (Left) - double tap: Enter the mode to adjust and examine the offset.
   * Virtual Button A (Left): Decrease offset.
   * Virtual Button B (Center): toggles between X and Y axis (roll and pitch axis).
   * Virtual Button C (Right): Increase offset.
   * Virtual Button B (Center) - double tap: Stop adjust mode.

## Requirement

* [m5stack/M5Unified](https://github.com/m5stack/M5Unified)
* [meganetaaan/M5Stack-Avatar](https://github.com/meganetaaan/m5stack-avatar)
* [gob/gob_unifiedButton](https://github.com/GOB52/gob_unifiedButton)
* [robotis-git/Dynamixel2Arduino](https://github.com/ROBOTIS-GIT/Dynamixel2Arduino)

## LICENSE

MIT


