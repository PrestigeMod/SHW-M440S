cmd_arch/arm/mvp/pvtcpkm/pvtcp_off_linux_shim.o := /home/yyoung.kim/Toolchain/prebuilt/linux-x86/toolchain/arm-eabi-4.4.3/bin/arm-eabi-gcc -Wp,-MD,arch/arm/mvp/pvtcpkm/.pvtcp_off_linux_shim.o.d  -nostdinc -isystem /home/yyoung.kim/Toolchain/prebuilt/linux-x86/toolchain/arm-eabi-4.4.3/bin/../lib/gcc/arm-eabi/4.4.3/include -I/home/yyoung.kim/KOR_ANDR_OSS_OPEN_M0SKT-FINAL/Kernel/arch/arm/include -Iarch/arm/include/generated -Iinclude  -include include/generated/autoconf.h -D__KERNEL__ -mlittle-endian -Iarch/arm/mach-exynos/include -Iarch/arm/plat-s5p/include -Iarch/arm/plat-samsung/include -D__ASSEMBLY__ -mabi=aapcs-linux -mno-thumb-interwork  -D__LINUX_ARM_ARCH__=7 -march=armv7-a  -include asm/unified.h -msoft-float -gdwarf-2       -DMODULE  -c -o arch/arm/mvp/pvtcpkm/pvtcp_off_linux_shim.o arch/arm/mvp/pvtcpkm/pvtcp_off_linux_shim.S

source_arch/arm/mvp/pvtcpkm/pvtcp_off_linux_shim.o := arch/arm/mvp/pvtcpkm/pvtcp_off_linux_shim.S

deps_arch/arm/mvp/pvtcpkm/pvtcp_off_linux_shim.o := \
  /home/yyoung.kim/KOR_ANDR_OSS_OPEN_M0SKT-FINAL/Kernel/arch/arm/include/asm/unified.h \
    $(wildcard include/config/arm/asm/unified.h) \
    $(wildcard include/config/thumb2/kernel.h) \

arch/arm/mvp/pvtcpkm/pvtcp_off_linux_shim.o: $(deps_arch/arm/mvp/pvtcpkm/pvtcp_off_linux_shim.o)

$(deps_arch/arm/mvp/pvtcpkm/pvtcp_off_linux_shim.o):
