#ifndef METREON_RESOURCE_OPS_TD
#define METREON_RESOURCE_OPS_TD

include "MetreonOpBase.td"

def Metreon_ResourceOp : Metreon_Op<"resource"> {
  let summary = "Metreon resource";
}

def Metreon_StateOp : Metreon_Op<"state"> {
  let summary = "Metreon resource state";
}

def Metreon_TransitionOp : Metreon_Op<"transition"> {
  let summary = "Metreon resource transition";
}

#endif // METREON_RESOURCE_OPS_TD
