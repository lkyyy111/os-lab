OUTPUT_ARCH( "riscv" )
ENTRY( _entry )

SECTIONS
{
  /* 内核起始物理地址 */
  . = 0x80000000;
  PROVIDE(KERNEL_START = .);

  /* 代码段 */
  .text : {
    *(.text .text.*)
    *(.trampsec)
    . = ALIGN(0x1000);
    PROVIDE(etext = .);     /* 内核代码结束地址 */
    PROVIDE(KERNEL_TEXT_END = .);
  }

  /* 只读段 */
  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*)
    . = ALIGN(16);
    *(.rodata .rodata.*)
    PROVIDE(KERNEL_RODATA_END = .);
  }

  /* 数据段 */
  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*)
    . = ALIGN(16);
    *(.data .data.*)
    PROVIDE(KERNEL_DATA_END = .);
  }

  /* bss 段 */
  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.*)
    . = ALIGN(16);
    *(.bss .bss.*)
    PROVIDE(KERNEL_BSS_END = .);
  }

  /* 内核结束 */
  PROVIDE(end = .);

  /* 便于物理内存管理的关键地址 */
  PROVIDE(KERNEL_DATA = KERNEL_DATA_END);   /* 数据段末尾作为数据区结束地址 */
  PROVIDE(ALLOC_BEGIN = ALIGN(0x1000));     /* 内核结束后按页对齐的第一个地址可作为分配区起点 */
  PROVIDE(ALLOC_END = 0x88000000);          /* 假设物理内存上限（可根据实际内存大小修改） */
}
