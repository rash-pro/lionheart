#ifndef FF8_HOST_FF8_RAM_LAYOUT_H
#define FF8_HOST_FF8_RAM_LAYOUT_H
/* USA SLUS-00892 physical RAM. */
#define FF8_SAVEMAP 0x77378u
#define FF8_MODE 0x5f158u
#define FF8_FIELD_ID 0x5f14eu
#define FF8_CHARACTER 0x490u
#define FF8_CHARACTER_SIZE 0x98u
#define FF8_GF 0x50u
#define FF8_GF_SIZE 0x44u
#define FF8_GIL 0xb0cu
#define FF8_PARTY 0xd38u
#define FF8_BATTLES_WON 0xcdcu
#define FF8_PLAY_TIME 0xcd0u
#define FF8_DISC 0xe2cu
/* World map: three words the CPU adds to every terrain point before
 * projecting it (x, y, z), so each frame's terrain inputs move by their change. */
#define FF8_WORLD_OFFSET 0xdb0e8u
#endif
