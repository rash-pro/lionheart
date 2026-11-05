#ifndef HOST_FRAME_SCENE_H
#define HOST_FRAME_SCENE_H

enum { FRAME_SCENE_NONE, FRAME_SCENE_BATTLE, FRAME_SCENE_FIXED_FIELD };

static inline int frame_scene(unsigned mode,unsigned field) {
    if (mode==3) return FRAME_SCENE_BATTLE;
    /* Only bgroad_4 has a verified fixed-background walking/occlusion route.
     * A field ID alone is insufficient: the journal also checks every sprite's
     * position, texture, color and mask state before accepting a pair. */
    if (mode==1 && field==227) return FRAME_SCENE_FIXED_FIELD;
    return FRAME_SCENE_NONE;
}
#endif
