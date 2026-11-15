#pragma once
#include <vector>

// Plan de escaneo para una seleccion de canales (MHz): los centros de paso (Hz) para kp::Radio::set_scan_plan().
// Cada paso de kepyur mira +-16.5 MHz a 40 MS/s y necesita el lomo entero de la portadora (hasta ~13 MHz de
// ancho), asi que un paso cubre las portadoras a +-10 MHz de su centro, como los pasos de 20 MHz de upstream.
// Vacio = todos los canales: se usa el plan completo de kepyur (que tambien cubre frecuencias no estandar).
std::vector<double> scan_plan_for(std::vector<double> channelsMHz);

// Pasos del plan completo de kepyur (radio.cpp, scan_plan(): 5652..5940 y 5370..5630 cada 20 MHz).
int full_scan_steps();
