#if defined(_WIN32)
#define HC_RODATA ".section .rdata,\"dr\"\n"
#define HC_HIDDEN(sym) ""
#elif defined(__APPLE__)
#define HC_RODATA ".const_data\n"
#define HC_HIDDEN(sym) ".private_extern " #sym "\n"
#else
#define HC_RODATA ".section .rodata\n"
#define HC_HIDDEN(sym) ".hidden " #sym "\n"
#endif

#define HC_EMBED(sym, file, nul)                                   \
    __asm__(HC_RODATA                                              \
            ".balign 16\n"                                         \
            ".globl " #sym "\n"                                    \
            HC_HIDDEN(sym)                                         \
            #sym ":\n"                                             \
            ".incbin \"" HARDCOPY_PPOCR_DIR "/" file "\"\n"        \
            nul                                                    \
            ".globl " #sym "_end\n"                                \
            HC_HIDDEN(sym##_end)                                   \
            #sym "_end:\n"                                         \
            ".byte 0\n"                                            \
            ".text\n");

HC_EMBED(hc_ppocr_det_param, "PP_OCRv5_mobile_det.ncnn.param", ".byte 0\n")
HC_EMBED(hc_ppocr_det_bin,   "PP_OCRv5_mobile_det.ncnn.bin",   "")
HC_EMBED(hc_ppocr_rec_param, "PP_OCRv5_mobile_rec.ncnn.param", ".byte 0\n")
HC_EMBED(hc_ppocr_rec_bin,   "PP_OCRv5_mobile_rec.ncnn.bin",   "")
