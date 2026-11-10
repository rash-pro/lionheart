#include "gui_internal.h"
#include "ra.h"
#include "gui_assets.h"
#include "osd_font.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#ifdef FF8_VULKAN
#include "gui_vulkan.h"
#endif
#ifdef FF8_NFD
#include <nfd.h>
#include <nfd_sdl2.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
struct Observation {
    char id[128], text[2048];
    ImGuiID widget;
};
struct Ui {
    ImGuiContext *context = nullptr;
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    ImFont *fonts[5] = {};
    unsigned observation_count = 0;
    char focus[128] = {}, requested_focus[128] = {}, modal_parent[128] = {};
    gui_rect_t control_row = {};
    ImGuiID last_edit = 0, frame_edit = 0;
    host_gui_input_t edit_input = HOST_GUI_KEYBOARD;
    unsigned modal_depth = 0;
    bool control_row_pending = false, reopened_parent = false;
    float scale = 1.0f, font_scale = 0.0f, pending_scale = 0.0f;
    uint64_t resize_at = 0, clock = 0;
    int emblem_rect = -1;
    bool platform = false, soft = false, in_frame = false, modal = false, modal_frame = false;
    bool nfd = false, wait_for_pad_release = false;
    float opacity = 1;
    unsigned badge_revision = 0;
    uint64_t badges_at = 0;
} ui;
struct BadgeRect { unsigned id; int colour,grey; };
std::vector<BadgeRect> badges;
Observation observations[2048];

const ImU32 colours[] = {
    IM_COL32(242,242,242,255), IM_COL32(165,165,175,255), IM_COL32(107,107,120,255),
    IM_COL32(159,180,216,255), IM_COL32(124,207,154,255), IM_COL32(230,184,92,255),
    IM_COL32(224,108,108,255), IM_COL32(230,200,110,255)
};

ImU32 rgba(unsigned c) {
    return IM_COL32(c >> 24, (c >> 16) & 255, (c >> 8) & 255, unsigned((c & 255)*ui.opacity));
}
float pixel(float v) { return std::round(v * ui.scale); }
float scroll_y() { return ImGui::GetCurrentWindow()->Scroll.y; }
ImVec2 point(float x, float y) { return ImVec2(pixel(x), pixel(y) - scroll_y()); }
ImRect bounds(gui_rect_t r) { return ImRect(point(r.x,r.y), point(r.x+r.w,r.y+r.h)); }
ImDrawList *draw() { return ImGui::GetWindowDrawList(); }
void copy(char *out, size_t cap, const char *text) {
    if (!cap) return;
    size_t n=text ? std::min(cap-1,std::strlen(text)) : 0;
    if (n) std::memcpy(out,text,n);
    out[n]=0;
}
void observe(const char *id, const char *text, ImGuiID widget = 0) {
    if (!id || !*id) return;
    Observation *entry = nullptr;
    for (unsigned n = 0; n < ui.observation_count; ++n)
        if (!std::strcmp(observations[n].id,id)) { entry=&observations[n]; break; }
    if (!entry && ui.observation_count < sizeof(observations)/sizeof(observations[0]))
        entry=&observations[ui.observation_count++];
    if (!entry) return;
    copy(entry->id,sizeof(entry->id),id); copy(entry->text,sizeof(entry->text),text); entry->widget=widget;
}
void focus_after_item(const char *id, ImGuiID widget, bool enabled) {
    if (!enabled && !std::strcmp(ui.requested_focus,id)) ui.requested_focus[0]=0;
    if (enabled && !std::strcmp(ui.requested_focus,id)) {
        if (GImGui->ActiveId && GImGui->ActiveId!=widget) ImGui::ClearActiveID();
        ImGui::SetFocusID(widget,ImGui::GetCurrentWindow());
        ImGui::FocusWindow(ImGui::GetCurrentWindow());
        GImGui->NavCursorVisible=true;
        GImGui->NavInitRequest=false;
        GImGui->NavInitResult.ID=0;
        GImGui->NavAnyRequest=GImGui->NavMoveScoringItems;
        ui.requested_focus[0]=0;
    }
    if (enabled && GImGui->NavId==widget) copy(ui.focus,sizeof(ui.focus),id);
}
struct Item { ImRect rect; bool pressed, focused, hovered, held, enabled; ImGuiID id; };
Item item(const char *id, gui_rect_t r, unsigned flags) {
    Item out={bounds(r),false,false,false,false,!(flags & GUI_DISABLED),0};
    ImGui::SetCursorScreenPos(out.rect.Min);
    if (!out.enabled) ImGui::BeginDisabled();
    out.pressed=ImGui::InvisibleButton(id,out.rect.GetSize(),ImGuiButtonFlags_EnableNav);
    out.id=ImGui::GetItemID();
    focus_after_item(id,out.id,out.enabled);
    out.focused=out.enabled && GImGui->NavId==out.id;
    out.hovered=out.enabled && ImGui::IsItemHovered();
    out.held=out.enabled && ImGui::IsItemActive();
    if (!out.enabled) ImGui::EndDisabled();
    return out;
}
ImU32 opacity(ImU32 c, bool enabled) {
    return enabled ? c : ((c & 0x00ffffffu) | (ImU32(((c >> 24) & 255)*0.42f) << 24));
}
void ring(const Item &it, bool row) {
    if (!it.focused) return;
    float inset=pixel(1), thickness=std::max(1.0f,pixel(2));
    ImVec2 lo(it.rect.Min.x+inset,it.rect.Min.y+inset),hi(it.rect.Max.x-inset,it.rect.Max.y-inset);
    float radius=std::min({pixel(7),(hi.x-lo.x)*0.5f,(hi.y-lo.y)*0.5f});
    // Whole-pixel bands keep the two-pixel focus core bright at every scale;
    // corner arcs keep antialiasing away from those straight bands.
    const ImVec2 centres[]={ImVec2(lo.x+radius,lo.y+radius),ImVec2(hi.x-radius,lo.y+radius),
                           ImVec2(hi.x-radius,hi.y-radius),ImVec2(lo.x+radius,hi.y-radius)};
    const float starts[]={IM_PI,1.5f*IM_PI,0,0.5f*IM_PI};
    for (unsigned corner=0;corner<4;++corner) {
        draw()->PathArcTo(centres[corner],std::max(0.5f,radius-thickness*0.5f),starts[corner],starts[corner]+0.5f*IM_PI);
        draw()->PathStroke(colours[GUI_TEXT],ImDrawFlags_None,thickness);
    }
    draw()->AddRectFilled(ImVec2(lo.x+radius,lo.y),ImVec2(hi.x-radius,lo.y+thickness),colours[GUI_TEXT]);
    draw()->AddRectFilled(ImVec2(lo.x+radius,hi.y-thickness),ImVec2(hi.x-radius,hi.y),colours[GUI_TEXT]);
    draw()->AddRectFilled(ImVec2(lo.x,lo.y+radius),ImVec2(lo.x+thickness,hi.y-radius),colours[GUI_TEXT]);
    draw()->AddRectFilled(ImVec2(hi.x-thickness,lo.y+radius),ImVec2(hi.x,hi.y-radius),colours[GUI_TEXT]);
    if (row) draw()->AddRectFilled(ImVec2(it.rect.Min.x+inset,it.rect.Min.y+pixel(6)),
        ImVec2(it.rect.Min.x+pixel(4),it.rect.Max.y-pixel(6)),colours[GUI_ACCENT],pixel(1));
}
void paint_item(const Item &it, unsigned flags, bool row) {
    ImU32 fill=0;
    if (flags & GUI_PRIMARY) fill=colours[GUI_ACCENT];
    else if (!row && !(flags & GUI_GHOST)) fill=IM_COL32(34,34,43,255);
    if (flags & GUI_SELECTED) fill=IM_COL32(255,255,255,15);
    if (it.held) fill=IM_COL32(159,180,216,46);
    else if (it.focused) fill=IM_COL32(255,255,255,18);
    else if (it.hovered) fill=IM_COL32(255,255,255,9);
    if (fill) draw()->AddRectFilled(it.rect.Min,it.rect.Max,opacity(fill,it.enabled),pixel(7));
    if (flags & GUI_GHOST) draw()->AddRect(it.rect.Min,it.rect.Max,opacity(IM_COL32(255,255,255,20),it.enabled),pixel(7));
    if (flags & GUI_SELECTED) draw()->AddRectFilled(it.rect.Min,ImVec2(it.rect.Min.x+pixel(3),it.rect.Max.y),opacity(colours[GUI_ACCENT],it.enabled));
    ring(it,row);
}
Item control_item(const char *id,gui_rect_t r,unsigned flags) {
    bool row=ui.control_row_pending;
    Item it=item(id,row ? ui.control_row : r,flags);
    ui.control_row_pending=false;
    if (row) paint_item(it,0,true);
    return it;
}
void text_at(const char *text, ImRect r, gui_font_t font, ImU32 colour, bool centre=false, bool wrap=false) {
    if (!text || !*text || r.GetWidth()<=0 || r.GetHeight()<=0) return;
    ImFont *f=ui.fonts[font];
    ImVec2 p=r.Min;
    if (centre) {
        ImVec2 size=f->CalcTextSizeA(f->FontSize,FLT_MAX,0,text);
        p.x=std::round((r.Min.x+r.Max.x-size.x)*0.5f);
        p.y=std::round((r.Min.y+r.Max.y-size.y)*0.5f);
    }
    ImVec4 clip(r.Min.x,r.Min.y,r.Max.x,r.Max.y);
    colour=(colour&0xffffffu)|(ImU32(((colour>>24)&255)*ui.opacity)<<24);
    draw()->AddText(f,f->FontSize,p,colour,text,nullptr,wrap ? r.GetWidth() : 0.0f,&clip);
}
int arrow_delta(const Item &it) {
    if (!it.focused) return 0;
    bool left=ImGui::IsKeyPressed(ImGuiKey_LeftArrow,true) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft,true) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft,true);
    bool right=ImGui::IsKeyPressed(ImGuiKey_RightArrow,true) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight,true) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight,true);
    if (left || right) ImGui::NavMoveRequestCancel();
    return right ? 1 : left ? -1 : 0;
}
ImFont *font(const unsigned char *data,size_t bytes,float size) {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas=false; cfg.OversampleH=2; cfg.OversampleV=1; cfg.PixelSnapH=true;
    static const ImWchar ranges[]={0x20,0xff,0x2000,0x206f,0x2190,0x21ff,0x25a0,0x25ff,0};
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(data),int(bytes),size,&cfg,ranges);
}
struct MonoRects { ImFont *font; int rects[224], base, scale; };
MonoRects mono_rects(float target) {
    MonoRects result={};
    int best_distance=10000;
    for (int base : {16,24,32,64}) for (int factor=1;factor<=4;++factor) {
        int distance=std::abs(int(std::round(target))-base*factor);
        if (distance<best_distance) { best_distance=distance; result.base=base; result.scale=factor; }
    }
    int height=result.base*result.scale, width=result.base/2*result.scale;
    result.font=font(gui_font_regular,sizeof(gui_font_regular),float(height));
    for (int i=0;i<224;++i) result.rects[i]=ImGui::GetIO().Fonts->AddCustomRectFontGlyph(result.font,ImWchar(i+32),width,height,float(width));
    return result;
}
uint32_t mono_row(int base,int glyph,int row) {
    switch (base) { case 16:return spleen_16[glyph][row]; case 24:return spleen_24[glyph][row];
    case 32:return spleen_32[glyph][row]; default:return spleen_64[glyph][row]; }
}
void fill_mono(const MonoRects &mono,unsigned char *pixels,int atlas_width) {
    for (int i=0;i<224;++i) {
        const ImFontAtlasCustomRect *r=ImGui::GetIO().Fonts->GetCustomRectByIndex(mono.rects[i]);
        for (int y=0;y<r->Height;++y) for (int x=0;x<r->Width;++x) {
            size_t at=(size_t(r->Y+y)*atlas_width+r->X+x)*4;
            uint8_t alpha=(mono_row(mono.base,i,y/mono.scale) & (0x80000000u >> (x/mono.scale))) ? 255 : 0;
            pixels[at]=pixels[at+1]=pixels[at+2]=255; pixels[at+3]=alpha;
        }
    }
}
void invalidate_fonts() {
    if (ui.soft) ImGui_ImplSDLRenderer2_DestroyFontsTexture();
#ifdef FF8_VULKAN
    if (!ui.soft) host_gui_vulkan_invalidate_fonts();
#endif
}
void rebuild_fonts(float scale) {
    invalidate_fonts();
    ImGuiIO &io=ImGui::GetIO(); io.Fonts->Clear();
    io.Fonts->Flags|=ImFontAtlasFlags_NoBakedLines;
    ui.fonts[GUI_REGULAR]=font(gui_font_regular,sizeof(gui_font_regular),std::round(18*scale));
    ui.fonts[GUI_TITLE]=font(gui_font_semibold,sizeof(gui_font_semibold),std::round(26*scale));
    ui.fonts[GUI_SUB]=font(gui_font_regular,sizeof(gui_font_regular),std::round(14*scale));
    MonoRects mono=mono_rects(14*scale),hint=mono_rects(22*scale);
    ui.fonts[GUI_MONO]=mono.font; ui.fonts[GUI_HINT]=hint.font;
    ui.emblem_rect=io.Fonts->AddCustomRectRegular(gui_emblem_width,gui_emblem_height);
    badges.clear();
    const host_ra_status_t *status=host_ra_status();
    for (unsigned i=0;i<status->total;++i) {
        host_ra_achievement_t a;
        if (host_ra_achievement(i,&a) && a.badge)
            badges.push_back({a.id,io.Fonts->AddCustomRectRegular(64,64),io.Fonts->AddCustomRectRegular(64,64)});
    }
    io.FontDefault=ui.fonts[GUI_REGULAR];
    io.Fonts->Build();
    unsigned char *pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    fill_mono(mono,pixels,width); fill_mono(hint,pixels,width);
    const ImFontAtlasCustomRect *r=io.Fonts->GetCustomRectByIndex(ui.emblem_rect);
    for (int y=0;y<gui_emblem_height;++y)
        std::memcpy(pixels+(size_t(r->Y+y)*width+r->X)*4,gui_emblem_rgba+size_t(y)*gui_emblem_width*4,size_t(gui_emblem_width)*4);
    for (const auto &badge:badges) for (unsigned i=0;i<status->total;++i) {
        host_ra_achievement_t a;
        if (!host_ra_achievement(i,&a) || a.id!=badge.id || !a.badge) continue;
        for (int variant=0;variant<2;++variant) {
            const auto *rect=io.Fonts->GetCustomRectByIndex(variant ? badge.grey : badge.colour);
            for (unsigned y=0;y<64;++y) for (unsigned x=0;x<64;++x) {
                unsigned char *d=pixels+(size_t(rect->Y+y)*width+rect->X+x)*4;
                const unsigned char *s=a.badge+(y*64+x)*4;
                std::memcpy(d,s,4);
                if (variant) d[0]=d[1]=d[2]=(unsigned char)((unsigned(s[0])*77+unsigned(s[1])*150+unsigned(s[2])*29)>>8);
            }
        }
        break;
    }
    ui.badge_revision=status->badge_revision; ui.badges_at=SDL_GetTicks64();
    ui.font_scale=scale;
}
ImGuiKey key(SDL_Scancode code) {
    switch (code) {
    case SDL_SCANCODE_TAB:return ImGuiKey_Tab; case SDL_SCANCODE_LEFT:return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT:return ImGuiKey_RightArrow; case SDL_SCANCODE_UP:return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN:return ImGuiKey_DownArrow; case SDL_SCANCODE_PAGEUP:return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN:return ImGuiKey_PageDown; case SDL_SCANCODE_HOME:return ImGuiKey_Home;
    case SDL_SCANCODE_END:return ImGuiKey_End; case SDL_SCANCODE_INSERT:return ImGuiKey_Insert;
    case SDL_SCANCODE_DELETE:return ImGuiKey_Delete; case SDL_SCANCODE_BACKSPACE:return ImGuiKey_Backspace;
    case SDL_SCANCODE_SPACE:return ImGuiKey_Space; case SDL_SCANCODE_RETURN:return ImGuiKey_Enter;
    case SDL_SCANCODE_ESCAPE:return ImGuiKey_Escape;
    default: if (code>=SDL_SCANCODE_A && code<=SDL_SCANCODE_Z) return ImGuiKey(int(ImGuiKey_A)+int(code)-int(SDL_SCANCODE_A));
        if (code>=SDL_SCANCODE_1 && code<=SDL_SCANCODE_9) return ImGuiKey(int(ImGuiKey_1)+int(code)-int(SDL_SCANCODE_1));
        if (code==SDL_SCANCODE_0) return ImGuiKey_0;
        return ImGuiKey_None;
    }
}
bool pad_held() {
    for (int index=0;index<SDL_NumJoysticks();++index) {
        if (!SDL_IsGameController(index)) continue;
        SDL_GameController *pad=SDL_GameControllerOpen(index);
        if (!pad) continue;
        bool held=false;
        for (int button=0;button<SDL_CONTROLLER_BUTTON_MAX && !held;++button)
            held=SDL_GameControllerGetButton(pad,SDL_GameControllerButton(button))!=0;
        for (int axis=0;axis<SDL_CONTROLLER_AXIS_MAX && !held;++axis) {
            int value=SDL_GameControllerGetAxis(pad,SDL_GameControllerAxis(axis));
            held=axis>=SDL_CONTROLLER_AXIS_TRIGGERLEFT ? value>16000 : value>16000 || value< -16000;
        }
        SDL_GameControllerClose(pad);
        if (held) return true;
    }
    return false;
}
void headless_event(const SDL_Event *event) {
    ImGuiIO &io=ImGui::GetIO();
    switch(event->type) {
    case SDL_KEYDOWN: case SDL_KEYUP: {
        bool down=event->type==SDL_KEYDOWN;
        ImGuiKey k=key(event->key.keysym.scancode); if (k!=ImGuiKey_None) io.AddKeyEvent(k,down);
        io.AddKeyEvent(ImGuiMod_Ctrl,(event->key.keysym.mod & KMOD_CTRL)!=0);
        io.AddKeyEvent(ImGuiMod_Shift,(event->key.keysym.mod & KMOD_SHIFT)!=0);
        io.AddKeyEvent(ImGuiMod_Alt,(event->key.keysym.mod & KMOD_ALT)!=0);
        break; }
    case SDL_TEXTINPUT: io.AddInputCharactersUTF8(event->text.text); break;
    case SDL_MOUSEMOTION: io.AddMousePosEvent(float(event->motion.x),float(event->motion.y));break;
    case SDL_MOUSEWHEEL: io.AddMouseWheelEvent(float(event->wheel.x),float(event->wheel.y));break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
        int button=event->button.button==SDL_BUTTON_LEFT ? 0 : event->button.button==SDL_BUTTON_RIGHT ? 1 : 2;
        io.AddMouseButtonEvent(button,event->type==SDL_MOUSEBUTTONDOWN); break; }
    case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
        ImGuiKey k=ImGuiKey_None;
        switch(event->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_A:k=ImGuiKey_GamepadFaceDown;break;
        case SDL_CONTROLLER_BUTTON_B:k=ImGuiKey_GamepadFaceRight;break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:k=ImGuiKey_GamepadDpadUp;break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:k=ImGuiKey_GamepadDpadDown;break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:k=ImGuiKey_GamepadDpadLeft;break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:k=ImGuiKey_GamepadDpadRight;break;
        default:break; }
        if (k!=ImGuiKey_None) { io.BackendFlags|=ImGuiBackendFlags_HasGamepad; io.AddKeyEvent(k,event->type==SDL_CONTROLLERBUTTONDOWN); }
        break; }
    default:break;
    }
}
}

extern "C" int gui_ui_attach(SDL_Window *window,SDL_Renderer *renderer) {
    if (ui.context) {
        if (ui.window==window && ui.renderer==renderer) return 1;
        gui_ui_detach();
    }
    IMGUI_CHECKVERSION(); ui.context=ImGui::CreateContext();
    if (!ui.context) return 0;
    ImGuiIO &io=ImGui::GetIO();
    io.IniFilename=nullptr; io.LogFilename=nullptr;
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;
    io.KeyRepeatDelay=0.4f; io.KeyRepeatRate=0.09f;
    ui.window=window; ui.renderer=renderer;
    host_gui_test_window(window);
    ui.platform=window && (renderer ? ImGui_ImplSDL2_InitForSDLRenderer(window,renderer) : ImGui_ImplSDL2_InitForVulkan(window));
    if (renderer) {
        ui.soft=ImGui_ImplSDLRenderer2_Init(renderer);
        if (!ui.soft) { gui_ui_detach(); return 0; }
    }
    if (window) {
        SDL_Surface *icon=SDL_CreateRGBSurfaceWithFormatFrom(const_cast<unsigned char *>(gui_emblem_rgba),gui_emblem_width,gui_emblem_height,32,gui_emblem_width*4,SDL_PIXELFORMAT_RGBA32);
        if (icon) { SDL_SetWindowIcon(window,icon); SDL_FreeSurface(icon); }
    }
#ifdef FF8_NFD
    const char *driver=SDL_GetCurrentVideoDriver();
    bool offscreen=!driver || !std::strcmp(driver,"dummy") || !std::strcmp(driver,"offscreen");
    ui.nfd=!offscreen && NFD_Init()==NFD_OKAY;
#endif
    return 1;
}
extern "C" void gui_ui_detach(void) {
    if (!ui.context) return;
    ImGui::SetCurrentContext(ui.context);
    if (ui.in_frame) { ImGui::EndFrame(); ui.in_frame=false; }
#ifdef FF8_NFD
    if (ui.nfd) NFD_Quit();
#endif
    if (ui.soft) ImGui_ImplSDLRenderer2_Shutdown();
#ifdef FF8_VULKAN
    else host_gui_vulkan_shutdown();
#endif
    if (ui.platform) ImGui_ImplSDL2_Shutdown();
    host_gui_test_window(nullptr);
    ImGui::DestroyContext(ui.context); ui=Ui{};
}
extern "C" void gui_ui_reset_input(void) {
    if (!ui.context) return;
    ImGui::SetCurrentContext(ui.context);
    ImGuiIO &io=ImGui::GetIO();
    io.ClearEventsQueue();
    io.ClearInputKeys();
    ImGui::ClearActiveID();
    ui.last_edit=ui.frame_edit=0;
    if (ui.platform) {
        ui.wait_for_pad_release=pad_held();
        ImGui_ImplSDL2_SetGamepadMode(ui.wait_for_pad_release ? ImGui_ImplSDL2_GamepadMode_Manual : ImGui_ImplSDL2_GamepadMode_AutoFirst,nullptr,0);
    }
}
extern "C" void gui_ui_event(const SDL_Event *event) {
    if (!ui.context) return;
    ImGui::SetCurrentContext(ui.context);
    if (ui.platform && (event->type==SDL_CONTROLLERBUTTONDOWN || event->type==SDL_CONTROLLERBUTTONUP) && event->cbutton.which==-1) {
        SDL_Event navigation={};
        navigation.type=event->type==SDL_CONTROLLERBUTTONDOWN ? SDL_KEYDOWN : SDL_KEYUP;
        switch (event->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_A:navigation.key.keysym.scancode=SDL_SCANCODE_RETURN;break;
        case SDL_CONTROLLER_BUTTON_B:navigation.key.keysym.scancode=SDL_SCANCODE_ESCAPE;break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:navigation.key.keysym.scancode=SDL_SCANCODE_UP;break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:navigation.key.keysym.scancode=SDL_SCANCODE_DOWN;break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:navigation.key.keysym.scancode=SDL_SCANCODE_LEFT;break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:navigation.key.keysym.scancode=SDL_SCANCODE_RIGHT;break;
        default:break;
        }
        headless_event(&navigation);
    } else if (ui.platform) {
        SDL_Event copy=*event;
        if ((copy.type==SDL_KEYDOWN || copy.type==SDL_KEYUP) && !copy.key.windowID) copy.key.windowID=SDL_GetWindowID(ui.window);
        if (copy.type==SDL_TEXTINPUT && !copy.text.windowID) copy.text.windowID=SDL_GetWindowID(ui.window);
        ImGui_ImplSDL2_ProcessEvent(&copy);
    } else headless_event(event);
}
extern "C" void gui_ui_begin(unsigned width,unsigned height,int overlay) {
    if (!ui.context) return;
    ImGui::SetCurrentContext(ui.context);
    uint64_t now=SDL_GetTicks64();
    ui.scale=std::max(0.75f,std::min(4.0f,float(height)/720.0f));
    if (ui.pending_scale!=ui.scale) { ui.pending_scale=ui.scale;ui.resize_at=now; }
    if (!ui.font_scale || (ui.font_scale!=ui.scale && now-ui.resize_at>=250) ||
        (ui.badge_revision!=host_ra_status()->badge_revision && now-ui.badges_at>=250)) rebuild_fonts(ui.scale);
    if (ui.platform) {
        if (ui.wait_for_pad_release && !pad_held()) {
            ui.wait_for_pad_release=false;
            ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoFirst);
        }
        ImGui_ImplSDL2_NewFrame();
    }
    ImGuiIO &io=ImGui::GetIO();
    io.DisplaySize=ImVec2(float(width),float(height)); io.DisplayFramebufferScale=ImVec2(1,1);
    io.DeltaTime=ui.clock ? std::max(0.0001f,std::min(0.1f,float(now-ui.clock)/1000.0f)) : 1.0f/60.0f; ui.clock=now;
    if (ui.soft) ImGui_ImplSDLRenderer2_NewFrame();
#ifdef FF8_VULKAN
    else host_gui_vulkan_prepare(width,height);
#endif
    ImGuiStyle &style=ImGui::GetStyle();
    style.WindowPadding=ImVec2(0,0); style.WindowBorderSize=0; style.ChildBorderSize=0;
    style.FramePadding=ImVec2(pixel(14),pixel(10)); style.FrameBorderSize=0; style.FrameRounding=pixel(6);
    style.ItemSpacing=ImVec2(pixel(2),pixel(2)); style.ScrollbarSize=pixel(8); style.GrabMinSize=pixel(12);
    style.Colors[ImGuiCol_Text]=ImGui::ColorConvertU32ToFloat4(colours[GUI_TEXT]);
    style.Colors[ImGuiCol_FrameBg]=ImVec4(0,0,0,0); style.Colors[ImGuiCol_FrameBgHovered]=ImVec4(0,0,0,0);
    style.Colors[ImGuiCol_FrameBgActive]=ImVec4(0,0,0,0); style.Colors[ImGuiCol_Border]=ImVec4(0,0,0,0);
    style.Colors[ImGuiCol_NavCursor]=ImVec4(0,0,0,0); style.Colors[ImGuiCol_WindowBg]=ImVec4(0,0,0,0);
    style.Colors[ImGuiCol_ChildBg]=ImVec4(0,0,0,0); style.Colors[ImGuiCol_PopupBg]=ImVec4(0,0,0,0);
    style.Colors[ImGuiCol_ModalWindowDimBg]=ImVec4(5/255.0f,5/255.0f,8/255.0f,0.62f);
    style.Colors[ImGuiCol_ScrollbarBg]=ImVec4(0,0,0,0);
    style.Colors[ImGuiCol_ScrollbarGrab]=ImVec4(0.18f,0.18f,0.22f,1);
    ImGui::NewFrame(); ui.in_frame=true; ui.opacity=1; ui.observation_count=0;ui.focus[0]=0;ui.modal_frame=false;ui.modal_depth=0;ui.modal_parent[0]=0;
    ui.control_row_pending=false;ui.frame_edit=0;
    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(float(width),float(height)));
    ImGui::Begin("##lionheart",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoScrollWithMouse);
    if (overlay) draw()->AddRectFilled(ImVec2(0,0),ImVec2(float(width),float(height)),IM_COL32(6,6,10,189));
}
extern "C" void gui_ui_end(void) {
    if (!ui.in_frame) return;
    if (GImGui->OpenPopupStack.Size>int(ui.modal_depth)) ImGui::ClosePopupToLevel(int(ui.modal_depth),true);
    ui.modal=ui.modal_frame;
    if (ui.frame_edit) ui.last_edit=ui.frame_edit;
    else if (!ui.modal_frame) ui.last_edit=0;
    ImGui::End(); ImGui::Render(); ui.in_frame=false;
    /* Nav may settle at EndFrame; resolve the semantic ID after scoring. */
    for (unsigned n=0;n<ui.observation_count;++n)
        if (observations[n].widget && observations[n].widget==GImGui->NavId) {
            copy(ui.focus,sizeof(ui.focus),observations[n].id);break;
        }
}
extern "C" void gui_ui_soft_render(SDL_Renderer *renderer) {
    if (ui.context && ui.soft && ImGui::GetDrawData()) ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(),renderer);
}
extern "C" void gui_ui_text(const char *id,const char *text,gui_rect_t r,gui_font_t f,gui_colour_t c) {
    if (!ui.in_frame) return;
    observe(id,text); text_at(text,bounds(r),f,colours[c],false,true);
}
extern "C" void gui_ui_text_centred(const char *id,const char *text,gui_rect_t r,gui_font_t f,gui_colour_t c) {
    if (!ui.in_frame) return;
    observe(id,text);text_at(text,bounds(r),f,colours[c],true);
}
extern "C" void gui_ui_keycap(const char *id,const char *key_text,const char *label,gui_rect_t r) {
    if (!ui.in_frame) return;
    ImRect b=bounds(r);ImFont *f=ui.fonts[GUI_HINT];
    bool cross=!std::strcmp(key_text,"\xe2\x9c\x95") || !std::strcmp(key_text,"\xc3\x97");
    bool circle=!std::strcmp(key_text,"\xe2\x97\x8b");
    bool square=!std::strcmp(key_text,"\xe2\x96\xa1");
    bool triangle=!std::strcmp(key_text,"\xe2\x96\xb3");
    bool glyph=cross || circle || square || triangle;
    float width=glyph ? pixel(24) : std::max(pixel(24),f->CalcTextSizeA(f->FontSize,FLT_MAX,0,key_text).x+pixel(14));
    float height=pixel(26),y=std::round((b.Min.y+b.Max.y-height)*0.5f);
    ImRect cap(ImVec2(b.Min.x,y),ImVec2(b.Min.x+width,y+height));
    draw()->AddRectFilled(cap.Min,cap.Max,IM_COL32(42,42,51,255),pixel(glyph ? 13 : 5));
    if (glyph) {
        ImVec2 c((cap.Min.x+cap.Max.x)*0.5f,(cap.Min.y+cap.Max.y)*0.5f);float d=pixel(5),thickness=std::max(1.0f,pixel(1.5f));
        if (cross) {
            draw()->AddLine(ImVec2(c.x-d,c.y-d),ImVec2(c.x+d,c.y+d),colours[GUI_TEXT],thickness);
            draw()->AddLine(ImVec2(c.x-d,c.y+d),ImVec2(c.x+d,c.y-d),colours[GUI_TEXT],thickness);
        } else if (circle) draw()->AddCircle(c,d,colours[GUI_TEXT],16,thickness);
        else if (square) draw()->AddRect(ImVec2(c.x-d,c.y-d),ImVec2(c.x+d,c.y+d),colours[GUI_TEXT],0,0,thickness);
        else draw()->AddTriangle(ImVec2(c.x,c.y-d),ImVec2(c.x+d,c.y+d),ImVec2(c.x-d,c.y+d),colours[GUI_TEXT],thickness);
    } else text_at(key_text,cap,GUI_HINT,colours[GUI_TEXT],true);
    b.Min.x=cap.Max.x+pixel(7);
    b.Min.y=std::round((b.Min.y+b.Max.y-ui.fonts[GUI_SUB]->FontSize)*0.5f);
    text_at(label,b,GUI_SUB,colours[GUI_DIM],false);
    observe(id,label);char key_id[160];std::snprintf(key_id,sizeof(key_id),"%s.key",id);observe(key_id,key_text);
}
extern "C" float gui_ui_text_width(const char *text,gui_font_t font) {
    if (!ui.in_frame || !text) return 0;
    ImFont *f=ui.fonts[font];
    return f->CalcTextSizeA(f->FontSize,FLT_MAX,0,text).x/ui.scale;
}
extern "C" void gui_ui_panel(gui_rect_t r,unsigned colour,float radius) {
    if (ui.in_frame) { ImRect b=bounds(r);draw()->AddRectFilled(b.Min,b.Max,rgba(colour),pixel(radius)); }
}
extern "C" void gui_ui_line(float x1,float y1,float x2,float y2,unsigned colour,float width) {
    if (ui.in_frame) draw()->AddLine(point(x1,y1),point(x2,y2),rgba(colour),std::max(1.0f,pixel(width)));
}
extern "C" int gui_ui_button(const char *id,const char *label,gui_rect_t r,unsigned flags) {
    if (!ui.in_frame) return 0;
    Item it=item(id,r,flags); paint_item(it,flags,false);
    ImU32 colour=flags & GUI_DANGER ? colours[GUI_ERROR] : flags & GUI_PRIMARY ? IM_COL32(11,11,16,255) : colours[GUI_TEXT];
    if (it.focused && (flags & GUI_PRIMARY)) colour=colours[GUI_TEXT];
    text_at(label,it.rect,GUI_REGULAR,opacity(colour,it.enabled),true);
    observe(id,label,it.id); return it.pressed;
}
extern "C" int gui_ui_row(const char *id,const char *label,const char *sub,gui_rect_t r,unsigned flags) {
    if (!ui.in_frame) return 0;
    Item it=item(id,r,flags);paint_item(it,flags,true);
    ImRect label_rect=it.rect; label_rect.Min.x+=pixel(18);label_rect.Max.x-=pixel(18);
    if (sub && *sub) {
        label_rect.Min.y+=pixel(8);label_rect.Max.y=label_rect.Min.y+pixel(25);
        ImRect sub_rect=it.rect;sub_rect.Min.x+=pixel(18);sub_rect.Max.x-=pixel(18);sub_rect.Min.y+=pixel(34);sub_rect.Max.y-=pixel(6);
        text_at(sub,sub_rect,GUI_SUB,opacity(flags & (GUI_DANGER|GUI_ALERT) ? colours[GUI_ERROR] : colours[GUI_DIM],it.enabled));
    } else { label_rect.Min.y+=std::round((it.rect.GetHeight()-ui.fonts[GUI_REGULAR]->FontSize)*0.5f); }
    text_at(label,label_rect,GUI_REGULAR,opacity(flags & GUI_DANGER ? colours[GUI_ERROR] : colours[GUI_TEXT],it.enabled));
    observe(id,label,it.id);
    if (sub && *sub) {
        char sub_id[160];std::snprintf(sub_id,sizeof(sub_id),"%s.sub",id);observe(sub_id,sub);
    }
    return it.pressed;
}
extern "C" int gui_ui_segments(const char *id,gui_rect_t r,const char *const *labels,int count,int current,int enabled) {
    if (!ui.in_frame || count<=0) return current;
    Item it=control_item(id,r,enabled ? 0 : GUI_DISABLED);
    ImRect b=bounds(r);
    draw()->AddRectFilled(b.Min,b.Max,opacity(IM_COL32(27,27,34,255),it.enabled),pixel(6));
    int choice=current,delta=arrow_delta(it);
    if (delta) choice=(current+delta+count)%count;
    if (it.pressed) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && b.Contains(ImGui::GetIO().MousePos)) choice=std::max(0,std::min(count-1,int((ImGui::GetIO().MousePos.x-b.Min.x)/b.GetWidth()*count)));
        else choice=(current+1)%count;
    }
    float cell=(b.GetWidth()-pixel(6))/count;
    for (int n=0;n<count;++n) {
        ImRect segment(ImVec2(b.Min.x+pixel(3)+n*cell,b.Min.y+pixel(3)),ImVec2(b.Min.x+pixel(3)+(n+1)*cell-pixel(2),b.Max.y-pixel(3)));
        if (n==current) draw()->AddRectFilled(segment.Min,segment.Max,opacity(colours[GUI_ACCENT],it.enabled),pixel(4));
        text_at(labels[n],segment,GUI_SUB,opacity(n==current ? IM_COL32(11,11,16,255) : colours[GUI_DIM],it.enabled),true);
    }
    ring(it,true);observe(id,labels[std::max(0,std::min(count-1,current))],it.id);return choice;
}
extern "C" int gui_ui_toggle(const char *id,gui_rect_t r,int current,int enabled) {
    if (!ui.in_frame) return current;
    Item it=control_item(id,r,enabled ? 0 : GUI_DISABLED);
    ImRect b=bounds(r);
    float w=pixel(46),h=pixel(26),x=b.Max.x-w,y=std::round((b.Min.y+b.Max.y-h)*0.5f);
    draw()->AddRectFilled(ImVec2(x,y),ImVec2(x+w,y+h),opacity(current ? colours[GUI_ACCENT] : IM_COL32(42,42,51,255),it.enabled),h*0.5f);
    draw()->AddCircleFilled(ImVec2(x+pixel(current ? 33 : 13),y+h*0.5f),pixel(10),opacity(current ? IM_COL32(11,11,16,255) : colours[GUI_DIM],it.enabled));
    ring(it,true);observe(id,host_gui_string(current ? "val.on" : "val.off"),it.id);
    int delta=arrow_delta(it);return it.pressed ? !current : delta ? delta>0 : current;
}
extern "C" int gui_ui_stepper(const char *id,gui_rect_t r,int current,int minimum,int maximum,int enabled) {
    if (!ui.in_frame) return current;
    Item it=control_item(id,r,enabled ? 0 : GUI_DISABLED);
    ImRect b=bounds(r);
    draw()->AddRectFilled(b.Min,b.Max,opacity(IM_COL32(27,27,34,255),it.enabled),pixel(6));
    char number[32];std::snprintf(number,sizeof(number),"%d",current);
    text_at(number,b,GUI_MONO,opacity(colours[GUI_TEXT],it.enabled),true);
    float centre=(b.Min.y+b.Max.y)*0.5f;
    float left=b.Min.x+pixel(13),right=b.Max.x-pixel(13),dy=pixel(4);
    draw()->AddTriangleFilled(ImVec2(left-pixel(3),centre),ImVec2(left+pixel(3),centre-dy),ImVec2(left+pixel(3),centre+dy),opacity(colours[GUI_DIM],it.enabled));
    draw()->AddTriangleFilled(ImVec2(right+pixel(3),centre),ImVec2(right-pixel(3),centre-dy),ImVec2(right-pixel(3),centre+dy),opacity(colours[GUI_DIM],it.enabled));
    int delta=arrow_delta(it);
    if (it.pressed) delta=ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().MousePos.x<(b.Min.x+b.Max.x)*0.5f ? -1 : 1;
    ring(it,true);observe(id,number,it.id);return std::max(minimum,std::min(maximum,current+delta));
}
extern "C" int gui_ui_edit(const char *id,gui_rect_t r,char *text,size_t capacity,int password) {
    if (!ui.in_frame || !text || !capacity) return 0;
    ImRect b=bounds(r);
    draw()->AddRectFilled(b.Min,b.Max,IM_COL32(19,19,26,255),pixel(6));
    draw()->AddRect(b.Min,b.Max,IM_COL32(255,255,255,20),pixel(6));
    ImGui::SetCursorScreenPos(b.Min);ImGui::SetNextItemWidth(b.GetWidth());
    ImGui::PushFont(ui.fonts[GUI_REGULAR]);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(pixel(14),std::max(0.0f,std::round((b.GetHeight()-ui.fonts[GUI_REGULAR]->FontSize)*0.5f))));
    ImGuiInputTextFlags flags=ImGuiInputTextFlags_EnterReturnsTrue;
    if (password) flags|=ImGuiInputTextFlags_Password;
    if (host_gui_last_input()==HOST_GUI_PAD) flags|=ImGuiInputTextFlags_ReadOnly;
    ImGui::PushID(id);
    bool submitted=ImGui::InputText("##edit",text,capacity,flags);
    ImGuiID widget=ImGui::GetItemID();
    ImGui::PopID();focus_after_item(id,widget,true);
    bool focused=GImGui->NavId==widget || ImGui::IsItemActive();
    if (focused) {
        ui.frame_edit=widget;
        host_gui_input_t input=host_gui_last_input();
        if (ui.last_edit!=widget || ImGui::IsItemActivated() ||
            (input==HOST_GUI_PAD && ui.edit_input!=HOST_GUI_PAD)) host_gui_edit_focus(text,capacity,password);
        ui.edit_input=input;
        Item it={b,false,true,false,false,true,widget}; ring(it,false);
    }
    ImGui::PopStyleVar();ImGui::PopFont();
    if (password) {
        char masked[512];size_t n=std::min(std::strlen(text),sizeof(masked)-1);std::memset(masked,'*',n);masked[n]=0;observe(id,masked,widget);
    } else observe(id,text,widget);
    return submitted;
}
extern "C" void gui_ui_modal_begin(const char *id,gui_rect_t r) {
    if (!ui.in_frame) return;
    const ImGuiWindowFlags flags=ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground;
    ui.reopened_parent=false;
    if (ui.modal_frame && *ui.modal_parent) {
        /* Browser path editing puts the pad keyboard above the existing modal. */
        ImGui::BeginPopupModal(ui.modal_parent,nullptr,flags);
        ui.reopened_parent=true;
    } else copy(ui.modal_parent,sizeof(ui.modal_parent),id);
    ui.modal=true;ui.modal_frame=true;
    if (!ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
    ImRect b=bounds(r);ImGui::SetNextWindowPos(b.Min);ImGui::SetNextWindowSize(b.GetSize());
    bool keyboard=std::strcmp(id,"keyboard")==0;
    if (keyboard) ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg,ImVec4(0,0,0,0));
    ImGui::BeginPopupModal(id,nullptr,flags);
    if (keyboard) ImGui::PopStyleColor();
    ui.modal_depth=unsigned(GImGui->BeginPopupStack.Size);
    draw()->AddRectFilled(b.Min,b.Max,IM_COL32(22,22,28,255),pixel(10));
}
extern "C" void gui_ui_modal_end(void) {
    if (!ui.in_frame) return;
    ImGui::EndPopup();
    if (ui.reopened_parent) { ImGui::EndPopup();ui.reopened_parent=false; }
}
extern "C" void gui_ui_focus(const char *id) { copy(ui.requested_focus,sizeof(ui.requested_focus),id); }
extern "C" const char *gui_ui_focused(void) { return ui.focus; }
extern "C" int gui_ui_wants_text(void) {
    if (!ui.context) return 0;
    ImGui::SetCurrentContext(ui.context);return ImGui::GetIO().WantTextInput;
}
extern "C" void gui_ui_control_row(gui_rect_t r) { ui.control_row=r;ui.control_row_pending=true; }
extern "C" int gui_ui_visible(gui_rect_t r) {
    if (!ui.in_frame) return 0;
    ImRect b=bounds(r);return ImGui::IsRectVisible(b.Min,b.Max);
}
extern "C" const char *gui_ui_text_for(const char *id) {
    for (unsigned n=0;n<ui.observation_count;++n) if (!std::strcmp(observations[n].id,id)) return observations[n].text;
    return nullptr;
}
extern "C" int gui_ui_native_pick(gui_pick_t kind,char *path,size_t capacity) {
#ifdef FF8_NFD
    if (!ui.nfd || !ui.window || host_gui_last_input()==HOST_GUI_PAD) return -1;
    Uint32 flags=SDL_GetWindowFlags(ui.window);
    if ((flags & SDL_WINDOW_FULLSCREEN_DESKTOP)==SDL_WINDOW_FULLSCREEN) return -1;
    nfdu8char_t *selected=nullptr;nfdresult_t result;
    if (kind==GUI_PICK_MOVIES || kind==GUI_PICK_SCREENSHOTS) {
        nfdpickfolderu8args_t args={};NFD_GetNativeWindowFromSDLWindow(ui.window,&args.parentWindow);
        result=NFD_PickFolderU8_With(&selected,&args);
    } else {
        const bool tar=!std::strcmp(gui_archive_format(),"tar.xz");
        nfdu8filteritem_t filter={host_gui_string(kind==GUI_PICK_DISC ? "nav.discs" : kind!=GUI_PICK_ARCHIVE ? "nav.card" :
                                                  tar ? "browser.archives.tar.xz" : "browser.archives.zip"),
            kind==GUI_PICK_DISC ? "cue,bin" : kind==GUI_PICK_ARCHIVE ? gui_archive_format() : "mcr,mcd,srm"};
        nfdopendialogu8args_t args={};args.filterList=&filter;args.filterCount=1;
        NFD_GetNativeWindowFromSDLWindow(ui.window,&args.parentWindow);
        result=NFD_OpenDialogU8_With(&selected,&args);
    }
    if (result==NFD_ERROR) return -1;
    if (result!=NFD_OKAY) return 0;
    bool fits=std::strlen(selected)<capacity;
    if (fits) copy(path,capacity,selected);
    NFD_FreePathU8(selected);return fits ? 1 : -1;
#else
    (void)kind;(void)path;(void)capacity;return -1;
#endif
}
extern "C" void gui_ui_emblem(gui_rect_t r) {
    if (!ui.in_frame || ui.emblem_rect<0) return;
    ImFontAtlas *atlas=ImGui::GetIO().Fonts;ImVec2 uv0,uv1;
    atlas->CalcCustomRectUV(atlas->GetCustomRectByIndex(ui.emblem_rect),&uv0,&uv1);
    ImRect b=bounds(r);draw()->AddImage(atlas->TexID,b.Min,b.Max,uv0,uv1);
}
extern "C" void gui_ui_opacity(float opacity) { ui.opacity=std::max(0.0f,std::min(1.0f,opacity)); }
extern "C" void gui_ui_badge(unsigned id,gui_rect_t r,int locked,float opacity) {
    if (!ui.in_frame) return;
    ImRect b=bounds(r);
    auto found=std::find_if(badges.begin(),badges.end(),[=](const BadgeRect &v) { return v.id==id; });
    if (found!=badges.end()) {
        ImFontAtlas *atlas=ImGui::GetIO().Fonts; ImVec2 uv0,uv1;
        atlas->CalcCustomRectUV(atlas->GetCustomRectByIndex(locked ? found->grey : found->colour),&uv0,&uv1);
        draw()->AddImage(atlas->TexID,b.Min,b.Max,uv0,uv1,IM_COL32(255,255,255,unsigned(opacity*(locked ? 128 : 255))));
    } else {
        ImU32 colour=IM_COL32(107,107,120,unsigned(opacity*128));
        for (float at=0;at<r.w;at+=8) {
            float end=std::min(at+4,r.w);
            draw()->AddLine(point(r.x+at,r.y),point(r.x+end,r.y),colour,std::max(1.0f,pixel(1)));
            draw()->AddLine(point(r.x+at,r.y+r.h),point(r.x+end,r.y+r.h),colour,std::max(1.0f,pixel(1)));
        }
        for (float at=0;at<r.h;at+=8) {
            float end=std::min(at+4,r.h);
            draw()->AddLine(point(r.x,r.y+at),point(r.x,r.y+end),colour,std::max(1.0f,pixel(1)));
            draw()->AddLine(point(r.x+r.w,r.y+at),point(r.x+r.w,r.y+end),colour,std::max(1.0f,pixel(1)));
        }
    }
}
extern "C" void gui_ui_scroll_begin(const char *id,gui_rect_t r) {
    if (!ui.in_frame) return;
    ImRect b=bounds(r);ImGui::SetCursorScreenPos(b.Min);
    ImGui::BeginChild(id,b.GetSize(),ImGuiChildFlags_None,ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoSavedSettings);
}
extern "C" void gui_ui_scroll_end(void) { if (ui.in_frame) ImGui::EndChild(); }
extern "C" void gui_ui_region_begin(const char *id,gui_rect_t r) {
    if (!ui.in_frame) return;
    ImRect b=bounds(r); ImGui::SetCursorScreenPos(b.Min);
    ImGui::BeginChild(id,b.GetSize(),ImGuiChildFlags_None,
        ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
}
extern "C" void gui_ui_region_end(void) { if (ui.in_frame) ImGui::EndChild(); }
