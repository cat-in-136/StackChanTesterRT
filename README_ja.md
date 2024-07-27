# ｽﾀｯｸﾁｬﾝテスター アールティ

[English](README.md)

ｽﾀｯｸﾁｬﾝ アールティver.用(M5Stack CoreS3 / DYNAMIXEL servo)用のテスタープログラムです。
[mongonta0716/stack-chan-tester](https://github.com/mongonta0716/stack-chan-tester) を参考にしています。

## 対応ボード

ｽﾀｯｸﾁｬﾝ アールティver. <https://github.com/rt-net/stack-chan>

## ｽﾀｯｸﾁｬﾝへのダウンロード方法

Platform IO でビルドして M5Stack CoreS3 へダウンロードします。

```sh
pio -f run --target upload
```

ダウンロードモードにしないとダウンロードに失敗します。
ダウンロードモードに入るには、リセットボタンを3秒間長押しします（緑色に点灯します）。

## 使い方

 * 仮想ボタンA（左）： サーボを正面に回転させます。
 * 仮想ボタン B（中央）：サーボをテストします： X軸（ロール軸）を-90°～90°で左右にピボット、Y軸（ピッチ軸）を-15°～10°で上下にピボット。
 * バーチャルボタンC（右）： ラムドモードで移動します。
   * 仮想ボタンC（右）：ランダムモード ランダムモード停止。
   * 仮想ボタンC（右）-ダブルタップ： バッテリーアイコンを隠す
 * 仮想ボタンA（左）-ダブルタップ： 調整モードに入り、オフセットを調べます。
   * 仮想ボタンA（左） オフセットを減らします。
   * 仮想ボタンB（中央）：X軸とY軸（ロール軸とピッチ軸）を切り替えます。
   * 仮想ボタンC（右）：オフセットを減らします： オフセットを増やします。
   * 仮想ボタンB（中央）-ダブルタップ： 調整モードを停止します。

## 依存ライブラリ

* [m5stack/M5Unified](https://github.com/m5stack/M5Unified)
* [meganetaaan/M5Stack-Avatar](https://github.com/meganetaaan/m5stack-avatar)
* [gob/gob_unifiedButton](https://github.com/GOB52/gob_unifiedButton)
* [robotis-git/Dynamixel2Arduino](https://github.com/ROBOTIS-GIT/Dynamixel2Arduino)

## LICENSE

MIT


