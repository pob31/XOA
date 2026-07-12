#pragma once

//==============================================================================
// XOA - reverse-Bessel-polynomial roots for near-field compensation (WP8,
// FR-6). GENERATED FILE - regenerate with tools/reference/gen_bessel_roots.py;
// do not edit by hand.
//
// Per Ambisonic order l = 1..10, the compensation filter is
//   H_l(s) = prod_i (s - q_i c/r_src) / (s - q_i c/r_ref)
// with q_i the roots of the monic reverse Bessel polynomial theta_l. Roots
// are grouped into ceil(l/2) sections: a conjugate pair (im>0 representative,
// -> 2nd-order section) or a lone real root (im==0, -> 1st-order section).
// The full derivation and the sign/direction convention live in the
// generator header comment.
//
// generated: 2026-07-12T12:53:29.399914+00:00
// python: 3.14.6  mpmath: 1.3.0
//==============================================================================

namespace xoa::nfc::tables
{

constexpr int kMaxOrder = 10;
constexpr int kTotalSections = 30;

/** One reverse-Bessel root per section. im == 0 marks a real root
    (1st-order section); im > 0 marks a conjugate pair (2nd-order). */
struct Root { double re; double im; };

/** Number of sections (ceil(l/2)) for each order, index 0..kMaxOrder. */
constexpr int kSectionCount[kMaxOrder + 1] = { 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5 };

/** Offset of order l's first section into kRoots, index 0..kMaxOrder. */
constexpr int kSectionOffset[kMaxOrder + 1] = { 0, 0, 1, 2, 4, 6, 9, 12, 16, 20, 25 };

constexpr Root kRoots[kTotalSections] = {
    { -1.0, 0.0 },   // order 1
    { -1.5, 0.8660254037844386 },   // order 2
    { -1.8389073226869572, 1.7543809597837217 }, { -2.3221853546260856, 0.0 },   // order 3
    { -2.8962106028203722, 0.8672341289345038 }, { -2.1037893971796278, 2.6574180418567526 },   // order 4
    { -3.3519563991535333, 1.7426614161831977 }, { -2.324674303181645, 3.571022920337976 }, { -3.6467385953296434, 0.0 },   // order 5
    { -4.248359395863364, 0.8675096732313656 }, { -3.735708356325815, 2.6262723114471256 }, { -2.5159322478108215, 4.492672953653942 },   // order 6
    { -4.758290528154629, 1.7392860611305365 }, { -4.070139163638138, 3.5171740477097533 }, { -2.6856768789432657, 5.420694130716749 }, { -4.971786858527936, 0.0 },   // order 7
    { -5.587886043263085, 0.8676144453527864 }, { -5.204840790636882, 2.6161751526425276 }, { -4.368289217202403, 4.414442500471539 }, { -2.8389839488976305, 6.353911298604877 },   // order 8
    { -6.129367904274273, 1.7378483834808625 }, { -5.604421819507781, 3.4981569178860936 }, { -4.6384398871803905, 5.317271675435651 }, { -2.9792607981800714, 7.291463688342182 }, { -6.297019181714968, 0.0 },   // order 9
    { -6.922044905427246, 0.8676651954512214 }, { -6.61529096547687, 2.61156792080009 }, { -5.967528328587786, 4.384947188941932 }, { -4.886219566858999, 6.224985482471567 }, { -3.108916233649098, 8.232699459073588 },   // order 10
};

} // namespace xoa::nfc::tables
