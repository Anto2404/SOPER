#!/bin/bash

TARGET=12345
ROUNDS=800

echo "#Hilos Tiempo" > tiempos.dat

for H in $(seq 1 100)
do
    echo "Probando $H hilos..."

    TIME=$( (time ./miner $TARGET $ROUNDS $H) 2>&1 | grep real | awk '{print $2}' ) # extraemos solo el valor del tiempo real.

    MIN=$(echo $TIME | cut -d'm' -f1)
    SEC=$(echo $TIME | cut -d'm' -f2 | sed 's/s//' | sed 's/,/./')

    TOTAL=$(echo "$MIN*60 + $SEC" | bc -l) # convertimos el tiempo min/seg a segundos totales.

    echo "$H $TOTAL" >> tiempos.dat
done