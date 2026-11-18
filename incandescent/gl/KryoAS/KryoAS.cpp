
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
