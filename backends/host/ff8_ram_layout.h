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
/* Names as the game shows them, in its text encoding and ending in a zero byte:
 * the two the player can change live in the savemap, the others follow one
 * another in the kernel's text. */
#define FF8_NAME_SQUALL 0x18u
#define FF8_NAME_RINOA 0x24u
#define FF8_NAME_SIZE 12u
#define FF8_KERNEL_NAMES 0x7f164u
#define FF8_KERNEL_NAME_COUNT 9u
/* World map: three words the CPU adds to every terrain point before
 * projecting it (x, y, z), so each frame's terrain inputs move by their change. */
#define FF8_WORLD_OFFSET 0xdb0e8u
/* In the scratchpad: the origin of the block the camera is in (x, then z), a
 * multiple of 2048. Crossing a block moves it by 2048 and the offset back by
 * as much, so the camera's place is their sum. */
#define FF8_WORLD_ORIGIN 0x320u

/* Gameplay booster RAM addresses (USA SLUS-00892 physical RAM).
 * Only touched when the player explicitly opts into gameplay boosters. */
#define FF8_ENCOUNTER_CONTROL 0x78df8u
#define FF8_ENCOUNTER_NONE 0x000cu
#define FF8_BATTLE_SLOT_STRIDE 0x1d0u
#define FF8_BATTLE_STATUS_AURA 0x78741u
#define FF8_BATTLE_AURA_BIT 0x04u
#define FF8_BATTLE_MAX_HP 0x78890u
#define FF8_BATTLE_CUR_HP 0x78892u
#define FF8_BATTLE_ATB 0x788a4u
#define FF8_BATTLE_ATB_MAX 0x2ee0u
#define FF8_BATTLE_ENTITY_AURA 0xed222u
#define FF8_BATTLE_ENTITY_STRIDE 0xd0u
#endif
