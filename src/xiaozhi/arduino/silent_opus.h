#pragma once
// 無音Opus上り1フレーム (16kHz mono 60ms = 960samples)。
// 生成: anullsrc(16000Hz/mono/0.06s) → opusenc --framesize 60
// (ffprobeで duration_time=0.060000 の60msパケットであることを確認)。
// OGG終端のflush用3Bパケット (f8 ff fe) は含めない。
// 用途: NAT punch / server下り送出先学習用 (空ヘッダのみでは server に
// 捨てられ UDP-RX が来ないため)。音声上り自体は OpusVoiceEncoder が現地
// エンコードするため、この埋め込みは punch 専用に残す。
#include <cstddef>
#include <cstdint>

namespace xiaozhi {

// TOC 0xFB = CELT code3 + count 0x03 → 3×20ms 無音 = 60ms。
inline constexpr uint8_t kSilentOpus60ms[] = {
    0xfb, 0x03, 0xff, 0xfe, 0xff, 0xfe, 0xff, 0xfe,
};
inline constexpr size_t kSilentOpus60msLen = sizeof(kSilentOpus60ms);

}  // namespace xiaozhi
