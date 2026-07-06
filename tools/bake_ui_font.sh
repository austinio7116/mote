#!/usr/bin/env bash
# Re-bake the Mote OS UI fonts (medium + large) from a TTF into the committed
# headers os/mote_ui_font_{med,lg}.h. Default source is assets/ui-font/Audiowide.ttf.
#
#   tools/bake_ui_font.sh [font.ttf] [med_px] [lg_px]
#
# The coverage tables are const -> flash/rodata, zero RAM. Rebuild firmware/Studio
# after running this.
set -euo pipefail
cd "$(dirname "$0")/.."
TTF="${1:-assets/ui-font/Audiowide.ttf}"
MED="${2:-11}"
LG="${3:-15}"
RD="${4:-13}"                     # reading size for gallery descriptions (~1.66x)
STB="$(dirname "$(find . -name stb_truetype.h -not -path '*/.winbuild/*' | head -1)")"
cc -O2 -I"$STB" -o /tmp/ttf2font tools/ttf2font.c -lm
/tmp/ttf2font mote_ui_med "$TTF" os/mote_ui_font_med.h  "$MED"
/tmp/ttf2font mote_ui_lg  "$TTF" os/mote_ui_font_lg.h   "$LG"
/tmp/ttf2font mote_ui_rd  "$TTF" os/mote_ui_font_read.h "$RD"
echo "baked $TTF -> os/mote_ui_font_{med,lg,read}.h  (med=${MED}px, lg=${LG}px, read=${RD}px)"
