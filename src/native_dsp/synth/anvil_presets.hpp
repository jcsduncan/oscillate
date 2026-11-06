#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Anvil presets (ADR-0060): only values that differ from the table defaults
// (which are "Warehouse Rumble"), plus each preset's drawn body: pitch in Hz,
// amplitude and drive 0..1, x over the hit's length. Weighted to techno and
// house, as the musician asked.
namespace oscillate::native_dsp::synth::anvil_presets {

using V = std::pair<std::string_view, double>;

// A drawn point; for the pitch curve, value is in Hz.
struct Point {
  double x = 0.0;
  double value = 0.0;
  double tension = 0.0;
};
struct Body {
  std::span<const Point> pitch;
  std::span<const Point> amp;
  std::span<const Point> drive;
};

// Drive over the hit. A real kick's knock (400 Hz to 1.5 kHz) comes from the
// attack driven hard while its pitch is still high; its body stays clean.
inline constexpr std::array<Point, 4> drive_attack{{{0, 1, 0}, {0.04, 0.5, 0}, {0.1, 0.1, 0}, {1, 0.05, 0}}};
inline constexpr std::array<Point, 4> drive_attack_long{{{0, 1, 0}, {0.05, 0.6, 0}, {0.15, 0.12, 0}, {1, 0.05, 0}}};
inline constexpr std::array<Point, 3> drive_front{{{0, 0.9, -0.6}, {0.3, 0.35, 0}, {1, 0.3, 0}}};
inline constexpr std::array<Point, 2> drive_flat{{{0, 0.8, 0}, {1, 0.8, 0}}};
inline constexpr std::array<Point, 2> drive_soft{{{0, 0.5, 0}, {1, 0.2, 0}}};
inline constexpr std::array<Point, 2> drive_none{{{0, 0, 0}, {1, 0, 0}}};

// Each body follows an analog kick: the pitch falls exponentially from its
// start to the tail, f(t) = tail + (start - tail) e^(-t/tau), and the level
// holds a few milliseconds, then decays exponentially and fades out by the
// hit's end. A level held then cut, or a pitch still gliding after 30 ms,
// sounds like a tom. Points are sampled from those laws, so they edit like
// any drawn curve; segments between them are straight (pitch in log Hz).
// 240 Hz falling to 49 Hz (tau 5 ms); decay tau 150 ms over 480 ms.
inline constexpr std::array<Point, 15> warehouse_pitch{{{0, 240, 0}, {0.0016, 213.395, 0}, {0.0031, 190.496, 0}, {0.0052, 164.847, 0}, {0.0083, 134.822, 0}, {0.0125, 106.528, 0}, {0.0187, 80.5721, 0}, {0.0271, 63.1863, 0}, {0.0396, 53.2728, 0}, {0.0563, 49.8627, 0}, {0.0833, 49.0641, 0}, {0.125, 49.0012, 0}, {0.1875, 49, 0}, {0.2708, 49, 0}, {1, 49, 0}}};
inline constexpr std::array<Point, 9> warehouse_amp{{{0, 1, 0}, {0.0063, 1, 0}, {0.0844, 0.7788, 0}, {0.1938, 0.5488, 0}, {0.3187, 0.3679, 0}, {0.5062, 0.2019, 0}, {0.7562, 0.0907, 0}, {0.92, 0.0537, 0}, {1, 0, 0}}};
// 260 Hz falling to 53 Hz (tau 4 ms); decay tau 90 ms over 360 ms.
inline constexpr std::array<Point, 15> berlin_pitch{{{0, 260, 0}, {0.0021, 224.609, 0}, {0.0042, 195.269, 0}, {0.0069, 163.799, 0}, {0.0111, 129.151, 0}, {0.0167, 99.1879, 0}, {0.025, 74.8176, 0}, {0.0361, 61.0263, 0}, {0.0528, 54.7909, 0}, {0.075, 53.2424, 0}, {0.1111, 53.0094, 0}, {0.1667, 53.0001, 0}, {0.25, 53, 0}, {0.3611, 53, 0}, {1, 53, 0}}};
inline constexpr std::array<Point, 10> berlin_amp{{{0, 1, 0}, {0.0083, 1, 0}, {0.0708, 0.7788, 0}, {0.1583, 0.5488, 0}, {0.2583, 0.3679, 0}, {0.4083, 0.2019, 0}, {0.6083, 0.0907, 0}, {0.8833, 0.0302, 0}, {0.92, 0.0261, 0}, {1, 0, 0}}};
// 280 Hz falling to 52 Hz (tau 4 ms); decay tau 140 ms over 420 ms.
inline constexpr std::array<Point, 15> hammer_pitch{{{0, 280, 0}, {0.0018, 241.019, 0}, {0.0036, 208.702, 0}, {0.006, 174.04, 0}, {0.0095, 135.876, 0}, {0.0143, 102.874, 0}, {0.0214, 76.031, 0}, {0.031, 60.8405, 0}, {0.0452, 53.9726, 0}, {0.0643, 52.267, 0}, {0.0952, 52.0104, 0}, {0.1429, 52.0001, 0}, {0.2143, 52, 0}, {0.3095, 52, 0}, {1, 52, 0}}};
inline constexpr std::array<Point, 9> hammer_amp{{{0, 1, 0}, {0.0071, 1, 0}, {0.0905, 0.7788, 0}, {0.2071, 0.5488, 0}, {0.3405, 0.3679, 0}, {0.5405, 0.2019, 0}, {0.8071, 0.0907, 0}, {0.92, 0.0647, 0}, {1, 0, 0}}};
// 200 Hz falling to 48 Hz (tau 5 ms); decay tau 180 ms over 560 ms.
inline constexpr std::array<Point, 15> hypnotic_pitch{{{0, 200, 0}, {0.0013, 178.828, 0}, {0.0027, 160.604, 0}, {0.0045, 140.193, 0}, {0.0071, 116.298, 0}, {0.0107, 93.7815, 0}, {0.0161, 73.1254, 0}, {0.0232, 59.2896, 0}, {0.0339, 51.4004, 0}, {0.0482, 48.6865, 0}, {0.0714, 48.051, 0}, {0.1071, 48.0009, 0}, {0.1607, 48, 0}, {0.2321, 48, 0}, {1, 48, 0}}};
inline constexpr std::array<Point, 9> hypnotic_amp{{{0, 1, 0}, {0.0054, 1, 0}, {0.0857, 0.7788, 0}, {0.1982, 0.5488, 0}, {0.3268, 0.3679, 0}, {0.5196, 0.2019, 0}, {0.7768, 0.0907, 0}, {0.92, 0.0581, 0}, {1, 0, 0}}};
// 250 Hz falling to 52 Hz (tau 4.5 ms); decay tau 120 ms over 420 ms.
inline constexpr std::array<Point, 15> peak_pitch{{{0, 250, 0}, {0.0018, 219.603, 0}, {0.0036, 193.873, 0}, {0.006, 165.603, 0}, {0.0095, 133.4, 0}, {0.0143, 104.192, 0}, {0.0214, 78.7964, 0}, {0.031, 63.0163, 0}, {0.0452, 54.9039, 0}, {0.0643, 52.4908, 0}, {0.0952, 52.0273, 0}, {0.1429, 52.0003, 0}, {0.2143, 52, 0}, {0.3095, 52, 0}, {1, 52, 0}}};
inline constexpr std::array<Point, 9> peak_amp{{{0, 1, 0}, {0.0071, 1, 0}, {0.0786, 0.7788, 0}, {0.1786, 0.5488, 0}, {0.2929, 0.3679, 0}, {0.4643, 0.2019, 0}, {0.6929, 0.0907, 0}, {0.92, 0.041, 0}, {1, 0, 0}}};
// 170 Hz falling to 46 Hz (tau 6 ms); decay tau 220 ms over 620 ms.
inline constexpr std::array<Point, 15> dub_pitch{{{0, 170, 0}, {0.0012, 155.43, 0}, {0.0024, 142.571, 0}, {0.004, 127.746, 0}, {0.0065, 109.664, 0}, {0.0097, 91.6171, 0}, {0.0145, 73.6681, 0}, {0.021, 60.2053, 0}, {0.0306, 51.2258, 0}, {0.0435, 47.3775, 0}, {0.0645, 46.1578, 0}, {0.0968, 46.0056, 0}, {0.1452, 46, 0}, {0.2097, 46, 0}, {1, 46, 0}}};
inline constexpr std::array<Point, 9> dub_amp{{{0, 1, 0}, {0.0048, 1, 0}, {0.0935, 0.7788, 0}, {0.2177, 0.5488, 0}, {0.3597, 0.3679, 0}, {0.5726, 0.2019, 0}, {0.8565, 0.0907, 0}, {0.92, 0.0758, 0}, {1, 0, 0}}};
// 230 Hz falling to 49 Hz (tau 5 ms); decay tau 140 ms over 460 ms.
inline constexpr std::array<Point, 15> rolling_pitch{{{0, 230, 0}, {0.0016, 204.788, 0}, {0.0033, 183.088, 0}, {0.0054, 158.782, 0}, {0.0087, 130.328, 0}, {0.013, 103.516, 0}, {0.0196, 78.9191, 0}, {0.0283, 62.4435, 0}, {0.0413, 53.0491, 0}, {0.0587, 49.8175, 0}, {0.087, 49.0607, 0}, {0.1304, 49.0011, 0}, {0.1957, 49, 0}, {0.2826, 49, 0}, {1, 49, 0}}};
inline constexpr std::array<Point, 9> rolling_amp{{{0, 1, 0}, {0.0065, 1, 0}, {0.0826, 0.7788, 0}, {0.1891, 0.5488, 0}, {0.3109, 0.3679, 0}, {0.4935, 0.2019, 0}, {0.737, 0.0907, 0}, {0.92, 0.0497, 0}, {1, 0, 0}}};
// 260 Hz falling to 55 Hz (tau 4 ms); decay tau 70 ms over 300 ms.
inline constexpr std::array<Point, 15> groove_pitch{{{0, 260, 0}, {0.0025, 224.951, 0}, {0.005, 195.894, 0}, {0.0083, 164.729, 0}, {0.0133, 130.415, 0}, {0.02, 100.742, 0}, {0.03, 76.6068, 0}, {0.0433, 62.9487, 0}, {0.0633, 56.7736, 0}, {0.09, 55.24, 0}, {0.1333, 55.0093, 0}, {0.2, 55.0001, 0}, {0.3, 55, 0}, {0.4333, 55, 0}, {1, 55, 0}}};
inline constexpr std::array<Point, 10> groove_amp{{{0, 1, 0}, {0.01, 1, 0}, {0.0683, 0.7788, 0}, {0.15, 0.5488, 0}, {0.2433, 0.3679, 0}, {0.3833, 0.2019, 0}, {0.57, 0.0907, 0}, {0.8267, 0.0302, 0}, {0.92, 0.0202, 0}, {1, 0, 0}}};
// 280 Hz falling to 48 Hz (tau 4 ms); decay tau 140 ms over 420 ms.
inline constexpr std::array<Point, 15> bunker_pitch{{{0, 280, 0}, {0.0018, 240.335, 0}, {0.0036, 207.451, 0}, {0.006, 172.181, 0}, {0.0095, 133.348, 0}, {0.0143, 99.7662, 0}, {0.0214, 72.4526, 0}, {0.031, 56.9956, 0}, {0.0452, 50.0072, 0}, {0.0643, 48.2716, 0}, {0.0952, 48.0105, 0}, {0.1429, 48.0001, 0}, {0.2143, 48, 0}, {0.3095, 48, 0}, {1, 48, 0}}};
inline constexpr std::array<Point, 9> bunker_amp{{{0, 1, 0}, {0.0071, 1, 0}, {0.0905, 0.7788, 0}, {0.2071, 0.5488, 0}, {0.3405, 0.3679, 0}, {0.5405, 0.2019, 0}, {0.8071, 0.0907, 0}, {0.92, 0.0647, 0}, {1, 0, 0}}};
// 210 Hz falling to 48 Hz (tau 5 ms); decay tau 170 ms over 520 ms.
inline constexpr std::array<Point, 15> warehouse_sub_pitch{{{0, 210, 0}, {0.0014, 187.435, 0}, {0.0029, 168.013, 0}, {0.0048, 146.258, 0}, {0.0077, 120.791, 0}, {0.0115, 96.7935, 0}, {0.0173, 74.7784, 0}, {0.025, 60.0323, 0}, {0.0365, 51.6241, 0}, {0.0519, 48.7317, 0}, {0.0769, 48.0543, 0}, {0.1154, 48.001, 0}, {0.1731, 48, 0}, {0.25, 48, 0}, {1, 48, 0}}};
inline constexpr std::array<Point, 9> warehouse_sub_amp{{{0, 1, 0}, {0.0058, 1, 0}, {0.0875, 0.7788, 0}, {0.2019, 0.5488, 0}, {0.3327, 0.3679, 0}, {0.5288, 0.2019, 0}, {0.7904, 0.0907, 0}, {0.92, 0.061, 0}, {1, 0, 0}}};
// 200 Hz falling to 51 Hz (tau 4 ms); decay tau 75 ms over 320 ms.
inline constexpr std::array<Point, 15> thump_pitch{{{0, 200, 0}, {0.0023, 174.525, 0}, {0.0047, 153.406, 0}, {0.0078, 130.754, 0}, {0.0125, 105.814, 0}, {0.0187, 84.2464, 0}, {0.0281, 66.7045, 0}, {0.0406, 56.7774, 0}, {0.0594, 52.2891, 0}, {0.0844, 51.1745, 0}, {0.125, 51.0068, 0}, {0.1875, 51, 0}, {0.2812, 51, 0}, {0.4062, 51, 0}, {1, 51, 0}}};
inline constexpr std::array<Point, 10> thump_amp{{{0, 1, 0}, {0.0094, 1, 0}, {0.068, 0.7788, 0}, {0.15, 0.5488, 0}, {0.2437, 0.3679, 0}, {0.3844, 0.2019, 0}, {0.5719, 0.0907, 0}, {0.8297, 0.0302, 0}, {0.92, 0.0205, 0}, {1, 0, 0}}};
// 150 Hz falling to 48 Hz (tau 6 ms); decay tau 130 ms over 450 ms.
inline constexpr std::array<Point, 15> round_pitch{{{0, 150, 0}, {0.0017, 138.015, 0}, {0.0033, 127.438, 0}, {0.0056, 115.243, 0}, {0.0089, 100.368, 0}, {0.0133, 85.5237, 0}, {0.02, 70.7593, 0}, {0.0289, 59.685, 0}, {0.0422, 52.2987, 0}, {0.06, 49.1331, 0}, {0.0889, 48.1298, 0}, {0.1333, 48.0046, 0}, {0.2, 48, 0}, {0.2889, 48, 0}, {1, 48, 0}}};
inline constexpr std::array<Point, 9> round_amp{{{0, 1, 0}, {0.0067, 1, 0}, {0.0789, 0.7788, 0}, {0.18, 0.5488, 0}, {0.2956, 0.3679, 0}, {0.4689, 0.2019, 0}, {0.7, 0.0907, 0}, {0.92, 0.0424, 0}, {1, 0, 0}}};
// 210 Hz falling to 53 Hz (tau 4.5 ms); decay tau 120 ms over 420 ms.
inline constexpr std::array<Point, 15> nine_pitch{{{0, 210, 0}, {0.0018, 185.898, 0}, {0.0036, 165.495, 0}, {0.006, 143.079, 0}, {0.0095, 117.545, 0}, {0.0143, 94.3848, 0}, {0.0214, 74.2476, 0}, {0.031, 61.7352, 0}, {0.0452, 55.3026, 0}, {0.0643, 53.3892, 0}, {0.0952, 53.0217, 0}, {0.1429, 53.0003, 0}, {0.2143, 53, 0}, {0.3095, 53, 0}, {1, 53, 0}}};
inline constexpr std::array<Point, 9> nine_amp{{{0, 1, 0}, {0.0071, 1, 0}, {0.0786, 0.7788, 0}, {0.1786, 0.5488, 0}, {0.2929, 0.3679, 0}, {0.4643, 0.2019, 0}, {0.6929, 0.0907, 0}, {0.92, 0.041, 0}, {1, 0, 0}}};
// 200 Hz falling to 54 Hz (tau 3 ms); decay tau 55 ms over 260 ms.
inline constexpr std::array<Point, 15> tick_pitch{{{0, 200, 0}, {0.0029, 167.705, 0}, {0.0058, 142.554, 0}, {0.0096, 117.451, 0}, {0.0154, 92.4852, 0}, {0.0231, 73.759, 0}, {0.0346, 61.2689, 0}, {0.05, 55.9161, 0}, {0.0731, 54.2593, 0}, {0.1038, 54.018, 0}, {0.1538, 54.0002, 0}, {0.2308, 54, 0}, {0.3462, 54, 0}, {0.5, 54, 0}, {1, 54, 0}}};
inline constexpr std::array<Point, 10> tick_amp{{{0, 1, 0}, {0.0115, 1, 0}, {0.0644, 0.7788, 0}, {0.1385, 0.5488, 0}, {0.2231, 0.3679, 0}, {0.35, 0.2019, 0}, {0.5192, 0.0907, 0}, {0.7519, 0.0302, 0}, {0.92, 0.0136, 0}, {1, 0, 0}}};
// 180 Hz falling to 50 Hz (tau 5 ms); decay tau 110 ms over 400 ms.
inline constexpr std::array<Point, 15> disco_pitch{{{0, 180, 0}, {0.0019, 161.892, 0}, {0.0037, 146.306, 0}, {0.0063, 128.849, 0}, {0.01, 108.413, 0}, {0.015, 89.1552, 0}, {0.0225, 71.4889, 0}, {0.0325, 59.6556, 0}, {0.0475, 52.9082, 0}, {0.0675, 50.5872, 0}, {0.1, 50.0436, 0}, {0.15, 50.0008, 0}, {0.225, 50, 0}, {0.325, 50, 0}, {1, 50, 0}}};
inline constexpr std::array<Point, 9> disco_amp{{{0, 1, 0}, {0.0075, 1, 0}, {0.0762, 0.7788, 0}, {0.1725, 0.5488, 0}, {0.2825, 0.3679, 0}, {0.4475, 0.2019, 0}, {0.6675, 0.0907, 0}, {0.92, 0.0362, 0}, {1, 0, 0}}};
// 230 Hz falling to 56 Hz (tau 3.5 ms); decay tau 70 ms over 300 ms.
inline constexpr std::array<Point, 15> garage_pitch{{{0, 230, 0}, {0.0025, 196.439, 0}, {0.005, 169.35, 0}, {0.0083, 141.18, 0}, {0.0133, 111.49, 0}, {0.02, 87.3361, 0}, {0.03, 69.2982, 0}, {0.0433, 60.2409, 0}, {0.0633, 56.7637, 0}, {0.09, 56.0777, 0}, {0.1333, 56.0019, 0}, {0.2, 56, 0}, {0.3, 56, 0}, {0.4333, 56, 0}, {1, 56, 0}}};
inline constexpr std::array<Point, 10> garage_amp{{{0, 1, 0}, {0.01, 1, 0}, {0.0683, 0.7788, 0}, {0.15, 0.5488, 0}, {0.2433, 0.3679, 0}, {0.3833, 0.2019, 0}, {0.57, 0.0907, 0}, {0.8267, 0.0302, 0}, {0.92, 0.0202, 0}, {1, 0, 0}}};
// 160 Hz falling to 50 Hz (tau 5 ms); decay tau 85 ms over 340 ms.
inline constexpr std::array<Point, 15> organic_pitch{{{0, 160, 0}, {0.0022, 144.678, 0}, {0.0044, 131.49, 0}, {0.0074, 116.718, 0}, {0.0118, 99.4262, 0}, {0.0176, 83.1314, 0}, {0.0265, 68.1829, 0}, {0.0382, 58.1701, 0}, {0.0559, 52.4608, 0}, {0.0794, 50.4968, 0}, {0.1176, 50.0369, 0}, {0.1765, 50.0007, 0}, {0.2647, 50, 0}, {0.3824, 50, 0}, {1, 50, 0}}};
inline constexpr std::array<Point, 10> organic_amp{{{0, 1, 0}, {0.0088, 1, 0}, {0.0713, 0.7788, 0}, {0.1588, 0.5488, 0}, {0.2588, 0.3679, 0}, {0.4088, 0.2019, 0}, {0.6088, 0.0907, 0}, {0.8838, 0.0302, 0}, {0.92, 0.0261, 0}, {1, 0, 0}}};
// 170 Hz falling to 47 Hz (tau 5 ms); decay tau 120 ms over 380 ms.
inline constexpr std::array<Point, 15> lofi_pitch{{{0, 170, 0}, {0.002, 152.867, 0}, {0.0039, 138.121, 0}, {0.0066, 121.603, 0}, {0.0105, 102.267, 0}, {0.0158, 84.0469, 0}, {0.0237, 67.3318, 0}, {0.0342, 56.1357, 0}, {0.05, 49.7516, 0}, {0.0711, 47.5555, 0}, {0.1053, 47.0413, 0}, {0.1579, 47.0008, 0}, {0.2368, 47, 0}, {0.3421, 47, 0}, {1, 47, 0}}};
inline constexpr std::array<Point, 9> lofi_amp{{{0, 1, 0}, {0.0079, 1, 0}, {0.0868, 0.7788, 0}, {0.1974, 0.5488, 0}, {0.3237, 0.3679, 0}, {0.5132, 0.2019, 0}, {0.7658, 0.0907, 0}, {0.92, 0.0557, 0}, {1, 0, 0}}};
// Hardstyle Tok: 900 Hz falling to 55 Hz (tau 4 ms); decay tau 400 ms over 450 ms.
inline constexpr std::array<Point, 14> tok_pitch{{{0, 900, 0}, {0.0033, 635.759, 0}, {0.0067, 454.15, 0}, {0.0111, 297.097, 0}, {0.0178, 169.358, 0}, {0.0267, 97.0701, 0}, {0.0378, 67.0533, 0}, {0.0533, 57.0945, 0}, {0.0756, 55.1719, 0}, {0.1067, 55.0052, 0}, {0.1511, 55, 0}, {0.2111, 55, 0}, {0.2889, 55, 0}, {1, 55, 0}}};
inline constexpr std::array<Point, 7> tok_amp{{{0, 1, 0}, {0.0044, 1, 0}, {0.2267, 0.7788, 0}, {0.5378, 0.5488, 0}, {0.8933, 0.3679, 0}, {0.92, 0.357, 0}, {1, 0, 0}}};
// Hard Techno Screech: 700 Hz falling to 52 Hz (tau 6 ms); decay tau 300 ms over 600 ms.
inline constexpr std::array<Point, 14> screech_pitch{{{0, 700, 0}, {0.0025, 556.663, 0}, {0.005, 445.032, 0}, {0.0083, 333.62, 0}, {0.0133, 222.811, 0}, {0.02, 139.697, 0}, {0.0283, 90.1131, 0}, {0.04, 63.8685, 0}, {0.0567, 54.2417, 0}, {0.08, 52.2174, 0}, {0.1133, 52.0078, 0}, {0.1583, 52.0001, 0}, {0.2167, 52, 0}, {1, 52, 0}}};
inline constexpr std::array<Point, 8> screech_amp{{{0, 1, 0}, {0.0033, 1, 0}, {0.1283, 0.7788, 0}, {0.3033, 0.5488, 0}, {0.5033, 0.3679, 0}, {0.8033, 0.2019, 0}, {0.92, 0.1599, 0}, {1, 0, 0}}};
// 808 Long: 120 Hz falling to 44 Hz (tau 12 ms); decay tau 600 ms over 1400 ms.
inline constexpr std::array<Point, 14> long808_pitch{{{0, 120, 0}, {0.0011, 111.07, 0}, {0.0021, 103.189, 0}, {0.0036, 94.1023, 0}, {0.0057, 83.0197, 0}, {0.0086, 71.9588, 0}, {0.0121, 62.4316, 0}, {0.0171, 54.2855, 0}, {0.0243, 48.4701, 0}, {0.0343, 45.392, 0}, {0.0486, 44.2629, 0}, {0.0679, 44.0277, 0}, {0.0929, 44.0015, 0}, {1, 44, 0}}};
inline constexpr std::array<Point, 8> long808_amp{{{0, 1, 0}, {0.0036, 1, 0}, {0.1107, 0.7788, 0}, {0.2607, 0.5488, 0}, {0.4321, 0.3679, 0}, {0.6893, 0.2019, 0}, {0.92, 0.1179, 0}, {1, 0, 0}}};
// Trap Boom: 150 Hz falling to 41 Hz (tau 10 ms); decay tau 500 ms over 1100 ms.
inline constexpr std::array<Point, 14> trap_pitch{{{0, 150, 0}, {0.0014, 134.817, 0}, {0.0027, 121.749, 0}, {0.0045, 107.112, 0}, {0.0073, 89.9769, 0}, {0.0109, 73.8302, 0}, {0.0155, 60.9125, 0}, {0.0218, 50.8883, 0}, {0.0309, 44.6377, 0}, {0.0436, 41.897, 0}, {0.0618, 41.1214, 0}, {0.0864, 41.0082, 0}, {0.1182, 41.0002, 0}, {1, 41, 0}}};
inline constexpr std::array<Point, 8> trap_amp{{{0, 1, 0}, {0.0036, 1, 0}, {0.1173, 0.7788, 0}, {0.2764, 0.5488, 0}, {0.4582, 0.3679, 0}, {0.7309, 0.2019, 0}, {0.92, 0.1332, 0}, {1, 0, 0}}};
// 300 Hz falling to 58 Hz (tau 4 ms); decay tau 70 ms over 300 ms.
inline constexpr std::array<Point, 15> dnb_pitch{{{0, 300, 0}, {0.0025, 258.625, 0}, {0.005, 224.324, 0}, {0.0083, 187.533, 0}, {0.0133, 147.027, 0}, {0.02, 111.998, 0}, {0.03, 83.5066, 0}, {0.0433, 67.3834, 0}, {0.0633, 60.0937, 0}, {0.09, 58.2834, 0}, {0.1333, 58.011, 0}, {0.2, 58.0001, 0}, {0.3, 58, 0}, {0.4333, 58, 0}, {1, 58, 0}}};
inline constexpr std::array<Point, 10> dnb_amp{{{0, 1, 0}, {0.01, 1, 0}, {0.0683, 0.7788, 0}, {0.15, 0.5488, 0}, {0.2433, 0.3679, 0}, {0.3833, 0.2019, 0}, {0.57, 0.0907, 0}, {0.8267, 0.0302, 0}, {0.92, 0.0202, 0}, {1, 0, 0}}};
// 240 Hz falling to 65 Hz (tau 5 ms); decay tau 60 ms over 260 ms.
inline constexpr std::array<Point, 15> breaks_pitch{{{0, 240, 0}, {0.0029, 215.624, 0}, {0.0058, 194.643, 0}, {0.0096, 171.143, 0}, {0.0154, 143.633, 0}, {0.0231, 117.709, 0}, {0.0346, 93.9273, 0}, {0.05, 77.9979, 0}, {0.0731, 68.9149, 0}, {0.1038, 65.7904, 0}, {0.1538, 65.0587, 0}, {0.2308, 65.0011, 0}, {0.3462, 65, 0}, {0.5, 65, 0}, {1, 65, 0}}};
inline constexpr std::array<Point, 10> breaks_amp{{{0, 1, 0}, {0.0115, 1, 0}, {0.0692, 0.7788, 0}, {0.15, 0.5488, 0}, {0.2423, 0.3679, 0}, {0.3808, 0.2019, 0}, {0.5654, 0.0907, 0}, {0.8192, 0.0302, 0}, {0.92, 0.0195, 0}, {1, 0, 0}}};
// Init: 150 Hz falling to 50 Hz (tau 10 ms); decay tau 140 ms over 400 ms.
inline constexpr std::array<Point, 14> init_pitch{{{0, 150, 0}, {0.0037, 136.071, 0}, {0.0075, 124.082, 0}, {0.0125, 110.653, 0}, {0.02, 94.9329, 0}, {0.03, 80.1194, 0}, {0.0425, 68.2684, 0}, {0.06, 59.0718, 0}, {0.085, 53.3373, 0}, {0.12, 50.823, 0}, {0.17, 50.1114, 0}, {0.2375, 50.0075, 0}, {0.325, 50.0002, 0}, {1, 50, 0}}};
inline constexpr std::array<Point, 9> init_amp{{{0, 1, 0}, {0.0075, 1, 0}, {0.095, 0.7788, 0}, {0.2175, 0.5488, 0}, {0.3575, 0.3679, 0}, {0.5675, 0.2019, 0}, {0.8475, 0.0907, 0}, {0.92, 0.0737, 0}, {1, 0, 0}}};

inline constexpr std::array warehouse_v{
    V{"anvil.output", -1.5},
    V{"anvil.length", 480},
    V{"anvil.glue", 0.15},
    V{"anvil.punch", 0.15},
    V{"anvil.drive.amount", 0.08},
    V{"anvil.body.drive", 0.6},
    V{"anvil.rumble.amount", 0.28},
    V{"anvil.click.type", 3},
    V{"anvil.click.level", 0.3},
    V{"anvil.click.tone", 0.5},
    V{"anvil.body.wave", 0},
    V{"anvil.body.harmonics", 0.02},
};
inline constexpr std::array berlin_v{
    V{"anvil.length", 360},      V{"anvil.rumble.amount", 1}, V{"anvil.rumble.decay", 0.45},
    V{"anvil.click.type", 1},    V{"anvil.click.level", 0.3},    V{"anvil.click.length", 4},
    V{"anvil.drive.type", 1},    V{"anvil.drive.amount", 0.12},   V{"anvil.body.drive", 0.65},
    V{"anvil.punch", 0.15},
    V{"anvil.output", -1.5},
    V{"anvil.glue", 0.15},
    V{"anvil.body.wave", 0},
};
inline constexpr std::array hammer_v{
    V{"anvil.length", 420},       V{"anvil.rumble.amount", 0.08}, V{"anvil.rumble.drive", 0.9},
    V{"anvil.rumble.decay", 0.6}, V{"anvil.click.type", 3},      V{"anvil.click.level", 0.3},
    V{"anvil.drive.type", 2},     V{"anvil.drive.amount", 0.2},  V{"anvil.body.drive", 0.8},
    V{"anvil.punch", 0.15},        V{"anvil.glue", 0.2},          V{"anvil.output", -2},
    V{"anvil.body.wave", 0},
};
inline constexpr std::array hypnotic_v{
    V{"anvil.length", 560},        V{"anvil.rumble.amount", 0.64}, V{"anvil.rumble.decay", 1.2},
    V{"anvil.rumble.space", 0.6},  V{"anvil.rumble.width", 0.7},   V{"anvil.rumble.tone", 0.22},
    V{"anvil.click.level", 0.3},  V{"anvil.drive.amount", 0.05},  V{"anvil.tone", 0.6},
    V{"anvil.output", -2.5},
    V{"anvil.glue", 0.1},
    V{"anvil.body.drive", 0.5},
    V{"anvil.body.wave", 0},
    V{"anvil.punch", 0.15},
};
inline constexpr std::array peak_v{
    V{"anvil.length", 420},      V{"anvil.rumble.amount", 0.22}, V{"anvil.rumble.drive", 0.7},
    V{"anvil.click.level", 0.3}, V{"anvil.click.tone", 0.6},    V{"anvil.drive.amount", 0.1},
    V{"anvil.body.drive", 0.65},  V{"anvil.punch", 0.15},         V{"anvil.glue", 0.2},
    V{"anvil.output", -1.5},
    V{"anvil.click.type", 3},
    V{"anvil.body.wave", 0},
};
inline constexpr std::array dub_v{
    V{"anvil.length", 620},       V{"anvil.rumble.amount", 0.3},  V{"anvil.rumble.decay", 1.8},
    V{"anvil.rumble.space", 0.8}, V{"anvil.rumble.width", 0.85}, V{"anvil.rumble.tone", 0.18},
    V{"anvil.rumble.drive", 0.3}, V{"anvil.click.type", 2},       V{"anvil.click.level", 0.3},
    V{"anvil.drive.amount", 0.03}, V{"anvil.tone", 0.5},           V{"anvil.punch", 0.15},
    V{"anvil.output", -3},
    V{"anvil.glue", 0.1},
    V{"anvil.body.drive", 0.45},
    V{"anvil.body.wave", 0},
};
inline constexpr std::array rolling_v{
    V{"anvil.length", 460},        V{"anvil.rumble.amount", 0.32}, V{"anvil.rumble.decay", 0.9},
    V{"anvil.rumble.duck", 0.95},  V{"anvil.rumble.drive", 0.65}, V{"anvil.sub.on", 1},
    V{"anvil.sub.level", 0.2},
    V{"anvil.output", -1.5},
    V{"anvil.glue", 0.15},
    V{"anvil.body.drive", 0.6},
    V{"anvil.drive.amount", 0.08},
    V{"anvil.body.wave", 0},
    V{"anvil.click.level", 0.3},
    V{"anvil.punch", 0.15},
};
inline constexpr std::array groove_v{
    V{"anvil.length", 300},      V{"anvil.rumble.amount", 1}, V{"anvil.rumble.decay", 0.4},
    V{"anvil.click.level", 0.3}, V{"anvil.click.length", 3},     V{"anvil.drive.type", 1},
    V{"anvil.drive.amount", 0.12}, V{"anvil.punch", 0.15},
    V{"anvil.output", -1.5},
    V{"anvil.glue", 0.15},
    V{"anvil.body.drive", 0.65},
    V{"anvil.body.wave", 0},
};
inline constexpr std::array bunker_v{
    V{"anvil.length", 420},       V{"anvil.body.wave", 0},       V{"anvil.body.harmonics", 0.05},
    V{"anvil.rumble.amount", 0.14}, V{"anvil.rumble.drive", 1},   V{"anvil.drive.type", 2},
    V{"anvil.drive.amount", 0.18}, V{"anvil.click.type", 3},     V{"anvil.click.level", 0.3},
    V{"anvil.output", -2},
    V{"anvil.glue", 0.2},
    V{"anvil.body.drive", 0.75},
    V{"anvil.punch", 0.15},
};
inline constexpr std::array sub_v{
    V{"anvil.length", 520},       V{"anvil.rumble.amount", 0.1}, V{"anvil.sub.on", 1},
    V{"anvil.sub.level", 0.4},    V{"anvil.sub.octave", -1},     V{"anvil.click.level", 0.3},
    V{"anvil.tone", 0.55},
    V{"anvil.output", -1.5},
    V{"anvil.glue", 0.1},
    V{"anvil.body.drive", 0.5},
    V{"anvil.drive.amount", 0.05},
    V{"anvil.body.wave", 0},
    V{"anvil.punch", 0.15},
};
inline constexpr std::array thump_v{
    V{"anvil.length", 320},        V{"anvil.rumble.on", 0},     V{"anvil.sub.on", 0},
    V{"anvil.sub.level", 0.25},    V{"anvil.click.level", 0.22}, V{"anvil.click.tone", 0.55},
    V{"anvil.click.length", 4},    V{"anvil.drive.amount", 0}, V{"anvil.punch", 0.1},
    V{"anvil.glue", 0.05},          V{"anvil.body.drive", 0.4},
    V{"anvil.output", -2},
    V{"anvil.click.type", 3},
    V{"anvil.body.harmonics", 0.02},
    V{"anvil.tone", 0.93},
    V{"anvil.body.wave", 0},
};
inline constexpr std::array round_v{
    V{"anvil.length", 450},       V{"anvil.rumble.on", 0},      V{"anvil.sub.on", 1},
    V{"anvil.sub.level", 0.2},   V{"anvil.click.type", 2},     V{"anvil.click.level", 0.22},
    V{"anvil.click.tone", 0.35},  V{"anvil.drive.amount", 0}, V{"anvil.tone", 0.85},
    V{"anvil.punch", 0.1},        V{"anvil.body.harmonics", 0}, V{"anvil.body.drive", 0.3},
    V{"anvil.output", -2},
    V{"anvil.glue", 0.05},
    V{"anvil.click.length", 8},
    V{"anvil.body.wave", 0},
};
inline constexpr std::array nine_v{
    V{"anvil.length", 420},      V{"anvil.rumble.on", 0},    V{"anvil.click.type", 1},
    V{"anvil.click.level", 0.22}, V{"anvil.click.tone", 0.25}, V{"anvil.click.length", 7.8},
    V{"anvil.drive.type", 1},    V{"anvil.drive.amount", 0}, V{"anvil.punch", 0.1},
    V{"anvil.body.wave", 0},     V{"anvil.body.harmonics", 0.02},
    V{"anvil.output", -2},
    V{"anvil.glue", 0.05},
    V{"anvil.body.drive", 0.4},
    V{"anvil.tone", 0.9},
};
inline constexpr std::array tick_v{
    V{"anvil.length", 260},      V{"anvil.rumble.on", 0},      V{"anvil.click.level", 0.35},
    V{"anvil.click.tone", 0.8},  V{"anvil.click.length", 2.5}, V{"anvil.drive.amount", 0},
    V{"anvil.punch", 0.1},       V{"anvil.body.drive", 0.45},   V{"anvil.glue", 0.05},
    V{"anvil.output", -1.5},
    V{"anvil.click.type", 0},
    V{"anvil.tone", 1},
    V{"anvil.body.wave", 0},
};
inline constexpr std::array disco_v{
    V{"anvil.length", 400},      V{"anvil.rumble.on", 0},      V{"anvil.click.type", 1},
    V{"anvil.click.level", 0.22}, V{"anvil.body.wave", 0},      V{"anvil.body.harmonics", 0.02},
    V{"anvil.drive.type", 1},    V{"anvil.drive.amount", 0.05}, V{"anvil.tone", 0.85},
    V{"anvil.glue", 0.1},
    V{"anvil.output", -1},
    V{"anvil.punch", 0.1},
    V{"anvil.click.tone", 0.5},
    V{"anvil.click.length", 6},
    V{"anvil.body.drive", 0.4},
};
inline constexpr std::array garage_v{
    V{"anvil.length", 300},      V{"anvil.rumble.on", 0},     V{"anvil.click.type", 3},
    V{"anvil.click.level", 0.22}, V{"anvil.click.tone", 0.6}, V{"anvil.punch", 0.1},
    V{"anvil.drive.amount", 0.05}, V{"anvil.glue", 0.1},
    V{"anvil.output", -0.5},
    V{"anvil.click.length", 4},
    V{"anvil.body.drive", 0.5},
    V{"anvil.body.wave", 0},
};
inline constexpr std::array organic_v{
    V{"anvil.length", 340},       V{"anvil.rumble.on", 0},      V{"anvil.click.type", 2},
    V{"anvil.click.level", 0.22}, V{"anvil.click.tone", 0.4},  V{"anvil.body.wave", 0},
    V{"anvil.body.harmonics", 0}, V{"anvil.drive.amount", 0}, V{"anvil.tone", 0.8},
    V{"anvil.key", 0},
    V{"anvil.output", -1.5},
    V{"anvil.glue", 0.05},
    V{"anvil.punch", 0.1},
    V{"anvil.click.length", 8},
    V{"anvil.body.drive", 0.3},
};
inline constexpr std::array lofi_v{
    V{"anvil.length", 380},       V{"anvil.rumble.on", 0},      V{"anvil.sub.on", 1},
    V{"anvil.sub.level", 0.3},    V{"anvil.click.type", 1},     V{"anvil.click.tone", 0.25},
    V{"anvil.drive.type", 1},     V{"anvil.drive.amount", 0.15}, V{"anvil.tone", 0.55},
    V{"anvil.glue", 0.15},
    V{"anvil.output", -0.5},
    V{"anvil.punch", 0.1},
    V{"anvil.click.level", 0.22},
    V{"anvil.click.length", 8},
    V{"anvil.body.drive", 0.4},
    V{"anvil.body.wave", 0},
};
inline constexpr std::array tok_v{
    V{"anvil.length", 450},       V{"anvil.rumble.on", 0},       V{"anvil.click.type", 3},
    V{"anvil.click.level", 0.7},  V{"anvil.click.tone", 0.85},   V{"anvil.body.drive", 1},
    V{"anvil.drive.type", 2},     V{"anvil.drive.amount", 0.9},  V{"anvil.punch", 0.6},
    V{"anvil.glue", 0.6},         V{"anvil.output", -3.5},         V{"anvil.velocity.drive", 0.6},
};
inline constexpr std::array screech_v{
    V{"anvil.length", 600},        V{"anvil.body.wave", 2},       V{"anvil.body.harmonics", 0.6},
    V{"anvil.body.drive", 1},      V{"anvil.rumble.amount", 0.2}, V{"anvil.rumble.drive", 1},
    V{"anvil.drive.type", 2},      V{"anvil.drive.amount", 0.8},  V{"anvil.tone", 0.85},
    V{"anvil.glue", 0.6},          V{"anvil.output", -4},
};
inline constexpr std::array long808_v{
    V{"anvil.length", 1400},      V{"anvil.rumble.on", 0},      V{"anvil.click.level", 0.15},
    V{"anvil.body.drive", 0.2},    V{"anvil.drive.amount", 0.25}, V{"anvil.punch", 0.25},
    V{"anvil.key", 1},             V{"anvil.body.harmonics", 0.05},
    V{"anvil.output", -4},
};
inline constexpr std::array trap_v{
    V{"anvil.length", 1100},     V{"anvil.rumble.on", 0},       V{"anvil.click.type", 1},
    V{"anvil.click.level", 0.25}, V{"anvil.body.drive", 0.5},    V{"anvil.drive.type", 1},
    V{"anvil.drive.amount", 0.45}, V{"anvil.key", 1},            V{"anvil.output", -4.5},
};
inline constexpr std::array dnb_v{
    V{"anvil.length", 300},       V{"anvil.rumble.on", 0},      V{"anvil.click.level", 0.55},
    V{"anvil.click.tone", 0.8},   V{"anvil.punch", 0.75},       V{"anvil.drive.amount", 0.4},
    V{"anvil.glue", 0.45},        V{"anvil.body.drive", 0.5},
    V{"anvil.output", -5.5},
};
inline constexpr std::array breaks_v{
    V{"anvil.length", 260},       V{"anvil.rumble.on", 0},      V{"anvil.click.type", 2},
    V{"anvil.click.level", 0.5},  V{"anvil.drive.type", 1},     V{"anvil.drive.amount", 0.6},
    V{"anvil.tone", 0.45},        V{"anvil.glue", 0.55},
    V{"anvil.output", -5},
};
inline constexpr std::array init_v{
    V{"anvil.length", 400},       V{"anvil.rumble.on", 0},       V{"anvil.click.level", 0},
    V{"anvil.body.harmonics", 0}, V{"anvil.body.drive", 0},      V{"anvil.drive.amount", 0},
    V{"anvil.tone", 1},           V{"anvil.punch", 0},           V{"anvil.glue", 0},
    V{"anvil.velocity.drive", 0}, V{"anvil.output", 0},
};

inline constexpr std::array<SynthPreset, 25> presets{{
    {"Warehouse Rumble", "Techno", warehouse_v},
    {"Berlin Dry", "Techno", berlin_v},
    {"Industrial Hammer", "Techno", hammer_v},
    {"Hypnotic", "Techno", hypnotic_v},
    {"Peak Time", "Techno", peak_v},
    {"Dub Techno", "Techno", dub_v},
    {"Rolling Rumble", "Techno", rolling_v},
    {"Hard Groove", "Techno", groove_v},
    {"Bunker", "Techno", bunker_v},
    {"Warehouse Sub", "Techno", sub_v},
    {"Tech House Thump", "House", thump_v},
    {"Deep House Round", "House", round_v},
    {"909 Tribute", "House", nine_v},
    {"Minimal Tick", "House", tick_v},
    {"Disco Kick", "House", disco_v},
    {"Garage Punch", "House", garage_v},
    {"Organic Thud", "House", organic_v},
    {"Lo-Fi House", "House", lofi_v},
    {"Hardstyle Tok", "Hard", tok_v},
    {"Hard Techno Screech", "Hard", screech_v},
    {"808 Long", "808", long808_v},
    {"Trap Boom", "808", trap_v},
    {"DnB Punch", "Breaks", dnb_v},
    {"Breaks Knock", "Breaks", breaks_v},
    {"Init", "Init", init_v},
}};

// The drawn body of each preset, in preset order.
inline constexpr std::array<Body, presets.size()> bodies{{
    {warehouse_pitch, warehouse_amp, drive_attack_long},
    {berlin_pitch, berlin_amp, drive_attack_long},
    {hammer_pitch, hammer_amp, drive_attack_long},
    {hypnotic_pitch, hypnotic_amp, drive_attack_long},
    {peak_pitch, peak_amp, drive_attack_long},
    {dub_pitch, dub_amp, drive_attack_long},
    {rolling_pitch, rolling_amp, drive_attack_long},
    {groove_pitch, groove_amp, drive_attack_long},
    {bunker_pitch, bunker_amp, drive_attack_long},
    {warehouse_sub_pitch, warehouse_sub_amp, drive_attack_long},
    {thump_pitch, thump_amp, drive_attack},
    {round_pitch, round_amp, drive_attack},
    {nine_pitch, nine_amp, drive_attack},
    {tick_pitch, tick_amp, drive_attack},
    {disco_pitch, disco_amp, drive_attack},
    {garage_pitch, garage_amp, drive_attack},
    {organic_pitch, organic_amp, drive_attack},
    {lofi_pitch, lofi_amp, drive_attack},
    {tok_pitch, tok_amp, drive_flat},
    {screech_pitch, screech_amp, drive_flat},
    {long808_pitch, long808_amp, drive_none},
    {trap_pitch, trap_amp, drive_soft},
    {dnb_pitch, dnb_amp, drive_front},
    {breaks_pitch, breaks_amp, drive_soft},
    {init_pitch, init_amp, drive_none},
}};

} // namespace oscillate::native_dsp::synth::anvil_presets
// clang-format on
