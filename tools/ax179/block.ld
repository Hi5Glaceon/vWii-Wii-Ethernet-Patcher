/* IOS58 v6432 ETH module
 * blk:  0x13aa0da4..0x13aa1810, the old AX88772 driver (2668 bytes)
 * blk2: 0x13aa18cc..0x13aa1960, the stock stop routine incl. its literal pool (148 bytes),
 *       its only caller 0x13aa03ec is retargeted to ax179_stop */
MEMORY {
  blk  (rx) : ORIGIN = 0x13aa0da4, LENGTH = 0xa6c
  blk2 (rx) : ORIGIN = 0x13aa18cc, LENGTH = 0x94
}
SECTIONS {
  .blk2 : { *(.text.e_stop) *(.text.e_rxreset) *(.text.e_handles) } > blk2
  .blk : { *(.text.e_*) *(.text) *(.text.*) *(.rodata*) } > blk
  /DISCARD/ : { *(.ARM.*) *(.comment) *(.note*) *(.data*) *(.bss*) }
}
