# P488 arm64 command line size candidate (REVIEW ONLY / NO BOOT / NO FLASH)

Base: cbc904fbd2a6178b440108eff4bfca72dcc4d347 (hardware-verified P486).
Reference: micr0softstore/samsung_android_kernel_a52xq@6bf351bd, arch/arm64/include/uapi/asm/setup.h uses COMMAND_LINE_SIZE=4096.
Mangrove uses 2048. Sanitized TG capture reports 2484 bytes; reconstructed Mangrove 368-byte built-in prefix + 511-byte ABL header + 2066-byte bootloader addition + separator(s) is about 2946 bytes. The kernel buffer includes the terminating NUL. The 511-byte ABL header limit is a separate restriction and is not changed.

Candidate: change only the arm64 constant to match TG. No config, DTB, DTBO, boot image, audio, charging or storage changes.
Initial source review: drivers/of/fdt.c bounds bootargs copy and concatenation with COMMAND_LINE_SIZE; init/main.c uses that macro for the initial buffers and allocates saved/static command lines dynamically; arm64 setup.c passes the boot_command_line pointer; bootconfig parsing copies into a macro-sized buffer. Trace buffers and EFI conversion use the macro. A52 display command-line hash iterates to NUL. Code search is not a complete source audit; fixed literal sizes, bootconfig, downstream consumers, compile and actual merged bootargs still require review.

Before any boot candidate: run full-tree search (including literal-size assumptions, packaging and compatibility code), review compiled result, and make CI assert the actual prefix + 511 + measured bootloader bytes + separators + NUL fit. Do not alter the header policy. No flash pending Claude review.
Hardware evidence required: before/after command-line length and token presence (redact device identifiers), plus ro.boot.bootloader, ro.boot.em.model, ro.boot.kg and ro.boot.warranty_bit compared against TG. Do not synthesize missing values. The reported missing arguments are a reconstruction, not a direct Mangrove capture.
