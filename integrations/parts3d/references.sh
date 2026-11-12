#!/bin/bash
# Download KiCad 10's official STEP models used by `parts3d_check compare`, into $1, and print
# their names. Comparison only: none of these files is bundled.
set -u
mkdir -p "$1" && cd "$1" || exit 1
BASE=https://gitlab.com/kicad/libraries/kicad-packages3D/-/raw/10.0.0
get() { [ -s "$2.step" ] || curl -sfL --retry 3 -o "$2.step" "$BASE/$1.3dshapes/$2.step" || { echo "missing $1/$2" >&2; return; }; echo "$2"; }
for n in R_0402_1005Metric R_0603_1608Metric R_0805_2012Metric R_1206_3216Metric; do get Resistor_SMD $n; done
for n in C_0402_1005Metric C_0603_1608Metric C_0805_2012Metric C_1206_3216Metric; do get Capacitor_SMD $n; done
for n in PinHeader_1x01_P2.54mm_Vertical PinHeader_1x04_P2.54mm_Vertical PinHeader_1x10_P2.54mm_Vertical \
         PinHeader_2x05_P2.54mm_Vertical PinHeader_2x20_P2.54mm_Vertical PinHeader_1x04_P2.54mm_Horizontal \
         PinHeader_2x05_P2.54mm_Horizontal; do get Connector_PinHeader_2.54mm $n; done
for n in SOIC-8_3.9x4.9mm_P1.27mm SOIC-8-1EP_3.9x4.9mm_P1.27mm_EP2.29x3mm SOIC-14_3.9x8.7mm_P1.27mm \
         SOIC-16W_7.5x10.3mm_P1.27mm TSSOP-20_4.4x6.5mm_P0.65mm SSOP-20_5.3x7.2mm_P0.65mm; do get Package_SO $n; done
for n in LQFP-48_7x7mm_P0.5mm LQFP-64_10x10mm_P0.5mm TQFP-44_10x10mm_P0.8mm; do get Package_QFP $n; done
for n in SOT-23 SOT-23-5; do get Package_TO_SOT_SMD $n; done
