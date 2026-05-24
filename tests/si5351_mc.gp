# gnuplot script for Si5351 Monte-Carlo error analysis (issue #133).
# Usage:  gnuplot tests/si5351_mc.gp   (run from a dir containing the CSVs)
# Produces: si5351_mc_scatter.png, si5351_mc_cdf.png

set datafile separator ","
set datafile commentschars "#"

# ---------- Plot 1: error vs frequency (log-log) ----------
set terminal pngcairo size 1100,720 enhanced font "Sans,11"
set output "si5351_mc_scatter.png"
set title "Si5351 synthesis error vs frequency (27 MHz reference)\nnew continued-fraction vs current integer vs hardware optimum (brute force)"
set xlabel "Requested frequency (Hz)"
set ylabel "|error| (ppm)"
set logscale x
set logscale y
set xrange [1e4:2e8]
set format x "%.0s%c"
set grid xtics ytics mxtics mytics
set key top left
# clamp a floor so zero-error points (log scale) still render
clampf(y) = (y < 1e-7 ? 1e-7 : y)
plot \
  "si5351_mc_scatter.csv" using 1:(clampf($3)) with points pt 7 ps 0.4 lc rgb "#cc3311" title "current (integer)", \
  "" using 1:(clampf($4)) with points pt 7 ps 0.4 lc rgb "#999999" title "hardware optimum (brute force)", \
  "" using 1:(clampf($2)) with points pt 7 ps 0.4 lc rgb "#0077bb" title "new (continued fraction)"

# ---------- Plot 2: error distribution (CDF) ----------
set output "si5351_mc_cdf.png"
set title "Si5351 synthesis error distribution (60,000 random frequencies, 10 kHz-135 MHz)"
set xlabel "|error| (ppm)   [values below 1e-6 clamped; left edge = exact]"
set ylabel "cumulative fraction of frequencies"
set logscale x
unset logscale y
set yrange [0:1]
set xrange [1e-6:1e-1]
set format x "%g"
set grid xtics ytics
set key bottom right
cl(x) = (x < 1e-6 ? 1e-6 : x)
plot \
  "si5351_mc_cdf.csv" using (cl($3)):1 with lines lw 2 lc rgb "#cc3311" title "current (integer)", \
  "" using (cl($2)):1 with lines lw 2 lc rgb "#0077bb" title "new (continued fraction)"
