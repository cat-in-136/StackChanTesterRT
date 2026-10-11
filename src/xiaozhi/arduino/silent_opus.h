#pragma once
// 無音Opus上り1フレーム (16kHz mono 60ms = 960samples)。
// 生成: anullsrc(16000Hz/mono/0.06s) → opusenc --framesize 60
// (ffprobeで duration_time=0.060000 の60msパケットであることを確認)。
// OGG終端のflush用3Bパケット (f8 ff fe) は含めない。
// 同梱libopusはデコーダ専用ビルドのため現地エンコード不可、埋め込みで持つ。
// 用途: mic-less の NAT punch / server下り送出先学習用 (空ヘッダのみでは
// serverに捨てられ UDP-RX が来ないため)。
#include <cstddef>
#include <cstdint>

namespace xiaozhi {

// TOC 0xFB = CELT code3 + count 0x03 → 3×20ms 無音 = 60ms。
inline constexpr uint8_t kSilentOpus60ms[] = {
    0xfb, 0x03, 0xff, 0xfe, 0xff, 0xfe, 0xff, 0xfe,
};
inline constexpr size_t kSilentOpus60msLen = sizeof(kSilentOpus60ms);

}  // namespace xiaozhi
