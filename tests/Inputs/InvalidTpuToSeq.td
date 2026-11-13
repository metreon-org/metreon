include "Conversion/GraphIRToSeq.td"

def InvalidTpuToSeq : GraphIRToSeqRule<Metreon_TpuTensorCore,
    "external TPU operation", ["seq.compreg"], ["invalid target domain"]>;
