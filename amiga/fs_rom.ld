/* smashrom.ld - vlink linker script for smashrom */

OUTPUT_FORMAT("amiga")
OUTPUT_ARCH(m68k)
/* SEARCH_DIR("/opt/amiga13/m68k-amigaos/lib"); */
SECTIONS
{
  PROVIDE(___machtype = 0x0);
  . = 0x0;
  .text :
  {
    __stext = .;
    *(.text)
    *(.text.main)
    _copy_to_ram_start = .;
    *(.text_to_ram)
    _copy_to_ram_end = .;
    *(.text*)
    *(_*)
    *(.rodata*)
    *(.data.rel.ro*)
    *sm_msg_comm.c.obj(.text)
    *(.gnu.linkonce.t.*)
    *(.gnu.linkonce.r.*)
    *(.gcc_except_table*)
    *(SORT_BY_NAME(.list___EH_FRAME*))
    *(SORT_BY_NAME(.list_*))
    *(.end_of_lists)
    ___datadata_relocs = .;
  }
    __etext = .;
  ___text_size = SIZEOF(.text);
  . = ALIGN(0x0);
  .data :
  {
    __sdata = .;
    *(.data)
    *(SORT_BY_NAME(.data.*))
    CONSTRUCTORS
    *(SORT_BY_NAME(.dlist___EH_FRAME_OBJECT*))
    *(SORT_BY_NAME(.dlist_*))
    *(.end_of_dlists)
    *(.data.__EH_FRAME_OBJECT__*)
    *(.gnu.linkonce.d.*)
/*  ___a4_init = 0x7ffe; */
    __edata = .;
    *(.rom_end)
  }
  .bss :
  {
    __bss_start = .;
/*  ___a4_init2 = 0x7ffe; */
    *(.bss)
    *(.bss.*)
    *(COMMON)
    __end = .;
  }
  ___data_size = SIZEOF(.data);
  ___bss_size = SIZEOF(.bss);
  .datachip :
  {
    *(.datachip)
  }
  .bsschip :
  {
    *(.bsschip)
  }
  .dwarf :
  {
    __debug_frame_start = .;
    *(.debug_frame)
    __debug_info_start = .;
    *(.debug_info)
    __debug_abbrev_start = .;
    *(.debug_abbrev)
      __debug_loclists_start = .;
    *(.debug_loclists)
    __debug_aranges_start = .;
    *(.debug_aranges)
    __debug_rgnlists_start = .;
    *(.debug_rnglists)
    __debug_line_start = .;
    *(.debug_line)
    __debug_str_start = .;
    *(.debug_str)
    __debug_line_str_start = .;
    *(.debug_line_str)
  }
}
