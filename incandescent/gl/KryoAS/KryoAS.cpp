// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

#include "KryoAS.h"
#include <string>
namespace {
#include "KryoAS_Reconstruct.inc"
#include "KryoAS_Anomaly.inc"
}
namespace KryoAS {
const char* ReconstructionFragmentSource(){ return kKryoASReconstruct; }
const char* AnomalyFragmentSource(){ return kKryoASAnomaly; }
}
