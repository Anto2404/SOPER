set terminal png size 1000,700
set output "rendimiento.png"

set title "Rendimiento del miner (1-32 hilos)"
set xlabel "Numero de hilos"
set ylabel "Tiempo (segundos)"
set grid
set key off
set xtics 1
set style data linespoints

plot "tiempos.dat" using 1:2 linewidth 2 pointtype 7