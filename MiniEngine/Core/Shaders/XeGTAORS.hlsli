#define XE_GTAO_RootSig \
    "RootFlags(0), " \
    "CBV(b0), " \
    "RootConstants(b1, num32BitConstants = 2), " \
    "DescriptorTable(UAV(u0, numDescriptors = 5)), " \
    "DescriptorTable(SRV(t0, numDescriptors = 5)), " \
    "StaticSampler(s0, " \
        "addressU = TEXTURE_ADDRESS_CLAMP, " \
        "addressV = TEXTURE_ADDRESS_CLAMP, " \
        "addressW = TEXTURE_ADDRESS_CLAMP, " \
        "filter = FILTER_MIN_MAG_MIP_POINT)"
