/* IOS36 v3864 ETH module
 * blk:  0x13aa0084..0x13aa1290, the AX88772 chip logic up to axInit (axOpen stays)
 * blk2: 0x13aa13c8..0x13aa1500, axForceReadCompletion + axDown (axClose stays), the driver
 *       does not use it, build_patch36.py fills it with UDF */
MEMORY {
  blk  (rx) : ORIGIN = 0x13aa0084, LENGTH = 0x120c
  blk2 (rx) : ORIGIN = 0x13aa13c8, LENGTH = 0x138
}
SECTIONS {
  .blk2 : { } > blk2
  .blk : { *(.text.e_*) *(.text) *(.text.*) *(.rodata*) } > blk
  /DISCARD/ : { *(.ARM.*) *(.comment) *(.note*) }
}
