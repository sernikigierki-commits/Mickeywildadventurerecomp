/* MICKEY_INPUT_REBIND_DYNAMIC_LEGENDS_V1 */
enum class MickeyInputKind { KeyboardMouse, Gamepad };
enum class MickeyInputAction { None=-1, Up=0, Down, Left, Right, Confirm, Back };
struct MickeyControlDef { const char* ini_name; int kb_button; };
static const std::array<MickeyControlDef,16> kMickeyControls = {{
    {"up",PSX_KB_UP},{"down",PSX_KB_DOWN},{"left",PSX_KB_LEFT},{"right",PSX_KB_RIGHT},
    {"cross",PSX_KB_CROSS},{"circle",PSX_KB_CIRCLE},{"square",PSX_KB_SQUARE},{"triangle",PSX_KB_TRIANGLE},
    {"l1",PSX_KB_L1},{"r1",PSX_KB_R1},{"l2",PSX_KB_L2},{"r2",PSX_KB_R2},
    {"l3",PSX_KB_L3},{"r3",PSX_KB_R3},{"start",PSX_KB_START},{"select",PSX_KB_SELECT}
}};
static const std::array<const char*,16> kMickeyPadDefaults = {{
    "dpup,lefty-","dpdown,lefty+","dpleft,leftx-","dpright,leftx+",
    "a","b","x","y","leftshoulder","rightshoulder","lefttrigger","righttrigger",
    "leftstick","rightstick","start","back"
}};
MickeyInputKind g_mickey_input_kind=MickeyInputKind::KeyboardMouse;
MickeyInputKind g_mickey_rebind_kind=MickeyInputKind::KeyboardMouse;
bool g_mickey_input_seen=false;
bool g_mickey_controls_rebinding=false;
int g_mickey_controls_rebind_index=-1;
std::array<std::string,16> g_mickey_pad_bindings{};
std::array<int,8> g_mickey_axis_latch{};

static std::string mickey_trim(std::string s){auto ws=[](unsigned char c){return std::isspace(c)!=0;};while(!s.empty()&&ws((unsigned char)s.front()))s.erase(s.begin());while(!s.empty()&&ws((unsigned char)s.back()))s.pop_back();return s;}
static std::string mickey_lower(std::string s){for(char& c:s)c=(char)std::tolower((unsigned char)c);return s;}

const char* mickey_controls_text(const char* key){
    const int lang=std::clamp(g_cfg.language,0,4);
    if(!std::strcmp(key,"controls")){static const char* t[5]={"Sterowanie","Controls","Controlli","コントロール","Steuerung"};return t[lang];}
    if(!std::strcmp(key,"device")){static const char* t[5]={"Urządzenie","Device","Dispositivo","デバイス","Gerät"};return t[lang];}
    if(!std::strcmp(key,"keyboard")){static const char* t[5]={"Klawiatura","Keyboard","Tastiera","キーボード","Tastatur"};return t[lang];}
    if(!std::strcmp(key,"gamepad")){static const char* t[5]={"Kontroler","Controller","Controller","コントローラー","Controller"};return t[lang];}
    if(!std::strcmp(key,"reset")){static const char* t[5]={"Przywróć domyślne","Reset Defaults","Ripristina","デフォルトに戻す","Standard zurücksetzen"};return t[lang];}
    if(!std::strcmp(key,"press_key")){static const char* t[5]={"Naciśnij nowy klawisz","Press a new key","Premi un tasto","新しいキーを押してください","Neue Taste drücken"};return t[lang];}
    if(!std::strcmp(key,"press_button")){static const char* t[5]={"Naciśnij przycisk pada","Press a controller button","Premi un pulsante","コントローラーボタンを押してください","Controller-Taste drücken"};return t[lang];}
    if(!std::strcmp(key,"cancel_esc")){static const char* t[5]={"ESC: anuluj","ESC: cancel","ESC: annulla","ESC: キャンセル","ESC: abbrechen"};return t[lang];}
    if(!std::strcmp(key,"move")){static const char* t[5]={"Ruch","Move","Muovi","移動","Bewegen"};return t[lang];}
    if(!std::strcmp(key,"select")){static const char* t[5]={"Wybierz","Select","Seleziona","決定","Auswählen"};return t[lang];}
    if(!std::strcmp(key,"back")){static const char* t[5]={"Powrót","Back","Indietro","戻る","Zurück"};return t[lang];}
    if(!std::strcmp(key,"active_auto")){static const char* t[5]={
        "Widok zmienia się automatycznie po użyciu klawiatury lub kontrolera.",
        "The view switches automatically to the last input device used.",
        "La vista cambia automaticamente in base all'ultimo dispositivo usato.",
        "最後に使用した入力デバイスへ自動的に切り替わります。",
        "Die Ansicht wechselt automatisch zum zuletzt verwendeten Eingabegerät."};return t[lang];}
    return "";
}

const char* mickey_control_label(int idx){
    const int lang=std::clamp(g_cfg.language,0,4);
    static const char* pl[16]={"Góra","Dół","Lewo","Prawo","Krzyżyk","Kółko","Kwadrat","Trójkąt","L1","R1","L2","R2","L3","R3","Start","Select"};
    static const char* en[16]={"Up","Down","Left","Right","Cross","Circle","Square","Triangle","L1","R1","L2","R2","L3","R3","Start","Select"};
    static const char* it[16]={"Su","Giù","Sinistra","Destra","Croce","Cerchio","Quadrato","Triangolo","L1","R1","L2","R2","L3","R3","Start","Select"};
    static const char* ja[16]={"上","下","左","右","クロス","サークル","スクエア","トライアングル","L1","R1","L2","R2","L3","R3","スタート","セレクト"};
    static const char* de[16]={"Oben","Unten","Links","Rechts","Kreuz","Kreis","Quadrat","Dreieck","L1","R1","L2","R2","L3","R3","Start","Select"};
    if(idx<0||idx>=16)return "";switch(lang){case 0:return pl[idx];case 1:return en[idx];case 2:return it[idx];case 3:return ja[idx];default:return de[idx];}
}

void mickey_input_mark_keyboard(){if(g_mickey_controls_rebinding&&g_mickey_rebind_kind==MickeyInputKind::Gamepad)return;g_mickey_input_kind=MickeyInputKind::KeyboardMouse;g_mickey_input_seen=true;}
void mickey_input_mark_gamepad(){if(g_mickey_controls_rebinding&&g_mickey_rebind_kind==MickeyInputKind::KeyboardMouse)return;g_mickey_input_kind=MickeyInputKind::Gamepad;g_mickey_input_seen=true;}

static int mickey_control_index_for_name(const std::string& name){const std::string k=mickey_lower(mickey_trim(name));for(int i=0;i<16;i++)if(k==kMickeyControls[i].ini_name)return i;return -1;}
void mickey_controls_set_pad_defaults(){for(int i=0;i<16;i++)g_mickey_pad_bindings[i]=kMickeyPadDefaults[i];}

void mickey_controls_reload_pad_file(){
    mickey_controls_set_pad_defaults();std::ifstream in(exe_dir()/"input.ini");if(!in)return;std::string section,line;
    while(std::getline(in,line)){std::string t=mickey_trim(line);if(t.empty()||t[0]==';'||t[0]=='#')continue;if(t.front()=='['&&t.back()==']'){section=mickey_lower(mickey_trim(t.substr(1,t.size()-2)));continue;}size_t eq=t.find('=');if(eq==std::string::npos)continue;int idx=mickey_control_index_for_name(t.substr(0,eq));if(idx>=0&&(section=="mapping"||section=="controller"||section.empty()))g_mickey_pad_bindings[idx]=mickey_trim(t.substr(eq+1));}
}
static bool mickey_ini_is_mapping_key(const std::string& line){size_t eq=line.find('=');return eq!=std::string::npos&&mickey_control_index_for_name(line.substr(0,eq))>=0;}

void mickey_controls_save_pad_file(){
    fs::path path=exe_dir()/"input.ini";std::vector<std::string> old;{std::ifstream in(path);std::string line;while(std::getline(in,line))old.push_back(line);}std::vector<std::string> keep;std::string section;bool skip_mapping=false,had_controller=false;
    for(const std::string& raw:old){std::string t=mickey_trim(raw);if(!t.empty()&&t.front()=='['&&t.back()==']'){section=mickey_lower(mickey_trim(t.substr(1,t.size()-2)));skip_mapping=(section=="mapping");if(section=="controller")had_controller=true;if(skip_mapping)continue;keep.push_back(raw);continue;}if(skip_mapping)continue;if(section=="controller"&&mickey_ini_is_mapping_key(t))continue;keep.push_back(raw);}
    std::ofstream out(path,std::ios::binary|std::ios::trunc);if(!out)return;if(!had_controller)out<<"[controller]\nenabled = true\ndevice = 0\ndeadzone = 12000\n\n";for(const std::string& line:keep)out<<line<<"\n";if(!keep.empty()&&!keep.back().empty())out<<"\n";out<<"[mapping]\n";for(int i=0;i<16;i++)out<<kMickeyControls[i].ini_name<<" = "<<g_mickey_pad_bindings[i]<<"\n";out.flush();mickey_input_reload_controller_config();
}

/* MICKEY_INPUT_CONTROLLER_DETECT_FIX_V1_1
 *
 * V1 tylko liczyl urzadzenia. W SDL zdarzenia przyciskow/osi kontrolera
 * sa pewne dopiero po OTWARCIU gamepada. Runtime PSX robi to samo przez
 * SDL_GameControllerOpen(), wiec frontend tez musi otworzyc kontroler
 * zanim zacznie czekac na jego eventy.
 */
static std::vector<decltype(SDL_GameControllerOpen(0))> g_mickey_frontend_pads;
static int g_mickey_frontend_pad_scan_count = -1;

static bool mickey_controls_refresh_gamepads(bool force=false) {
    const int n = SDL_NumJoysticks();

    if (!force && n == g_mickey_frontend_pad_scan_count)
        return !g_mickey_frontend_pads.empty();

    g_mickey_frontend_pad_scan_count = n;

    for (int i=0; i<n; ++i) {
        if (!SDL_IsGameController(i))
            continue;

        auto *h = SDL_GameControllerOpen(i);
        if (!h)
            continue;

        bool already=false;
        for (auto *old_h : g_mickey_frontend_pads) {
            if (old_h == h) {
                already=true;
                break;
            }
        }
        if (!already)
            g_mickey_frontend_pads.push_back(h);
    }

    return !g_mickey_frontend_pads.empty();
}

/* MICKEY_INPUT_GAMEPLAY_HANDOFF_FIX_V1_3_INC
 *
 * Frontend temporarily owns GameController handles while its menu is visible.
 * Before handing execution back to the PSX guest, drop those frontend refs.
 * On same-window return, acquire them again for the menu.
 */
static void mickey_controls_release_gamepads_for_gameplay() {
    int released = 0;
    for (auto *h : g_mickey_frontend_pads) {
        if (!h) continue;
        SDL_GameControllerClose(h);
        ++released;
    }
    g_mickey_frontend_pads.clear();
    g_mickey_frontend_pad_scan_count = -1;
    g_mickey_axis_latch.fill(0);
    SDL_PumpEvents();
    std::fprintf(stdout,
        "MICKEY_INPUT_GAMEPLAY_HANDOFF_FIX_V1_3: released %d frontend pad handle(s) for gameplay\n",
        released);
    std::fflush(stdout);
}

static void mickey_controls_reacquire_gamepads_for_frontend() {
    const bool have = mickey_controls_refresh_gamepads(true);
    g_mickey_axis_latch.fill(0);
    if (have)
        g_mickey_input_kind = MickeyInputKind::Gamepad;
    std::fprintf(stdout,
        "MICKEY_INPUT_GAMEPLAY_HANDOFF_FIX_V1_3: frontend pad reacquire = %s\n",
        have ? "OK" : "none");
    std::fflush(stdout);
}

void mickey_controls_init(){
    psx_keybinds_init(nullptr);
    mickey_controls_reload_pad_file();

    const bool have_pad = mickey_controls_refresh_gamepads(true);
    if(!g_mickey_input_seen)
        g_mickey_input_kind = have_pad
            ? MickeyInputKind::Gamepad
            : MickeyInputKind::KeyboardMouse;
}
static std::string mickey_pad_first_source(int idx){if(idx<0||idx>=16)return"";std::string s=g_mickey_pad_bindings[idx];size_t c=s.find(',');if(c!=std::string::npos)s=s.substr(0,c);return mickey_lower(mickey_trim(s));}
static bool mickey_pad_binding_contains(int idx,const std::string& source){if(idx<0||idx>=16||source.empty())return false;std::istringstream ss(g_mickey_pad_bindings[idx]);std::string item,w=mickey_lower(source);while(std::getline(ss,item,','))if(mickey_lower(mickey_trim(item))==w)return true;return false;}

const char* mickey_pad_source_label(const std::string& raw){static thread_local std::string r;std::string s=mickey_lower(raw);if(s=="a")r="A";else if(s=="b")r="B";else if(s=="x")r="X";else if(s=="y")r="Y";else if(s=="back")r="BACK";else if(s=="start")r="START";else if(s=="leftshoulder")r="LB";else if(s=="rightshoulder")r="RB";else if(s=="lefttrigger")r="LT";else if(s=="righttrigger")r="RT";else if(s=="leftstick")r="L3";else if(s=="rightstick")r="R3";else if(s=="dpup")r="D-PAD UP";else if(s=="dpdown")r="D-PAD DOWN";else if(s=="dpleft")r="D-PAD LEFT";else if(s=="dpright")r="D-PAD RIGHT";else if(s=="leftx-")r="LS LEFT";else if(s=="leftx+")r="LS RIGHT";else if(s=="lefty-")r="LS UP";else if(s=="lefty+")r="LS DOWN";else if(s=="rightx-")r="RS LEFT";else if(s=="rightx+")r="RS RIGHT";else if(s=="righty-")r="RS UP";else if(s=="righty+")r="RS DOWN";else r=raw.empty()?"---":raw;return r.c_str();}
const char* mickey_keyboard_binding_label(int logical){static thread_local std::string r;if(logical<0||logical>=16){r="---";return r.c_str();}SDL_Scancode sc=psx_keybinds_get_button(1,kMickeyControls[logical].kb_button);if(sc==SDL_SCANCODE_UNKNOWN){r="---";return r.c_str();}const char* n=SDL_GetScancodeName(sc);r=(n&&*n)?n:"---";return r.c_str();}
const char* mickey_control_binding_label(int logical){return g_mickey_input_kind==MickeyInputKind::Gamepad?mickey_pad_source_label(mickey_pad_first_source(logical)):mickey_keyboard_binding_label(logical);}
const char* mickey_input_device_name(){
    (void)mickey_controls_refresh_gamepads(false);
    return g_mickey_input_kind==MickeyInputKind::Gamepad
        ? mickey_controls_text("gamepad")
        : mickey_controls_text("keyboard");
}
const char* mickey_input_legend(bool with_lr=false){static thread_local std::string o;std::string move=mickey_control_binding_label(0);move+="/";move+=mickey_control_binding_label(1);if(with_lr){move+="/";move+=mickey_control_binding_label(2);move+="/";move+=mickey_control_binding_label(3);}o=move+" : "+mickey_controls_text("move")+"    "+mickey_control_binding_label(4)+" : "+mickey_controls_text("select")+"    "+mickey_control_binding_label(5)+" : "+mickey_controls_text("back");return o.c_str();}

void mickey_controls_begin_rebind(int idx){if(idx<0||idx>=16)return;g_mickey_controls_rebinding=true;g_mickey_controls_rebind_index=idx;g_mickey_rebind_kind=g_mickey_input_kind;menu_audio_enter();}
void mickey_controls_cancel_rebind(){g_mickey_controls_rebinding=false;g_mickey_controls_rebind_index=-1;menu_audio_pickup();}
bool mickey_controls_capture_keyboard(SDL_Scancode sc){if(!g_mickey_controls_rebinding||g_mickey_rebind_kind!=MickeyInputKind::KeyboardMouse)return false;if(sc==SDL_SCANCODE_ESCAPE){mickey_controls_cancel_rebind();return true;}if(sc==SDL_SCANCODE_UNKNOWN)return true;psx_keybinds_set_button(1,kMickeyControls[g_mickey_controls_rebind_index].kb_button,sc);psx_keybinds_save();g_mickey_controls_rebinding=false;g_mickey_controls_rebind_index=-1;menu_audio_enter();return true;}
void mickey_controls_set_gamepad_source(int idx,const std::string& source){if(idx<0||idx>=16||source.empty())return;std::string value=source;if(idx==0&&source=="dpup")value="dpup,lefty-";else if(idx==1&&source=="dpdown")value="dpdown,lefty+";else if(idx==2&&source=="dpleft")value="dpleft,leftx-";else if(idx==3&&source=="dpright")value="dpright,leftx+";g_mickey_pad_bindings[idx]=value;mickey_controls_save_pad_file();}
bool mickey_controls_capture_gamepad(const std::string& source){if(!g_mickey_controls_rebinding||g_mickey_rebind_kind!=MickeyInputKind::Gamepad)return false;if(source.empty())return true;mickey_controls_set_gamepad_source(g_mickey_controls_rebind_index,source);g_mickey_controls_rebinding=false;g_mickey_controls_rebind_index=-1;menu_audio_enter();return true;}
void mickey_controls_reset_active(){if(g_mickey_input_kind==MickeyInputKind::Gamepad){mickey_controls_set_pad_defaults();mickey_controls_save_pad_file();}else{psx_keybinds_reset_player(1);psx_keybinds_save();}menu_audio_enter();}

std::string mickey_gamepad_button_source(int b){
#if defined(PSX_SDL3)
    switch(b){case SDL_GAMEPAD_BUTTON_SOUTH:return"a";case SDL_GAMEPAD_BUTTON_EAST:return"b";case SDL_GAMEPAD_BUTTON_WEST:return"x";case SDL_GAMEPAD_BUTTON_NORTH:return"y";case SDL_GAMEPAD_BUTTON_BACK:return"back";case SDL_GAMEPAD_BUTTON_START:return"start";case SDL_GAMEPAD_BUTTON_LEFT_STICK:return"leftstick";case SDL_GAMEPAD_BUTTON_RIGHT_STICK:return"rightstick";case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:return"leftshoulder";case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:return"rightshoulder";case SDL_GAMEPAD_BUTTON_DPAD_UP:return"dpup";case SDL_GAMEPAD_BUTTON_DPAD_DOWN:return"dpdown";case SDL_GAMEPAD_BUTTON_DPAD_LEFT:return"dpleft";case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:return"dpright";default:return"";}
#else
    switch(b){case SDL_CONTROLLER_BUTTON_A:return"a";case SDL_CONTROLLER_BUTTON_B:return"b";case SDL_CONTROLLER_BUTTON_X:return"x";case SDL_CONTROLLER_BUTTON_Y:return"y";case SDL_CONTROLLER_BUTTON_BACK:return"back";case SDL_CONTROLLER_BUTTON_START:return"start";case SDL_CONTROLLER_BUTTON_LEFTSTICK:return"leftstick";case SDL_CONTROLLER_BUTTON_RIGHTSTICK:return"rightstick";case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:return"leftshoulder";case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:return"rightshoulder";case SDL_CONTROLLER_BUTTON_DPAD_UP:return"dpup";case SDL_CONTROLLER_BUTTON_DPAD_DOWN:return"dpdown";case SDL_CONTROLLER_BUTTON_DPAD_LEFT:return"dpleft";case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:return"dpright";default:return"";}
#endif
}
std::string mickey_gamepad_axis_source(int axis,int value,bool navigation){const int hi=18000,lo=7000;if(axis<0||axis>=(int)g_mickey_axis_latch.size())return"";if(std::abs(value)<lo){g_mickey_axis_latch[(size_t)axis]=0;return"";}int sign=value<0?-1:+1;if(navigation&&g_mickey_axis_latch[(size_t)axis]==sign)return"";if(navigation)g_mickey_axis_latch[(size_t)axis]=sign;
#if defined(PSX_SDL3)
    switch(axis){case SDL_GAMEPAD_AXIS_LEFTX:if(std::abs(value)>=hi)return value<0?"leftx-":"leftx+";break;case SDL_GAMEPAD_AXIS_LEFTY:if(std::abs(value)>=hi)return value<0?"lefty-":"lefty+";break;case SDL_GAMEPAD_AXIS_RIGHTX:if(std::abs(value)>=hi)return value<0?"rightx-":"rightx+";break;case SDL_GAMEPAD_AXIS_RIGHTY:if(std::abs(value)>=hi)return value<0?"righty-":"righty+";break;case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:if(value>=hi)return"lefttrigger";break;case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:if(value>=hi)return"righttrigger";break;default:break;}
#else
    switch(axis){case SDL_CONTROLLER_AXIS_LEFTX:if(std::abs(value)>=hi)return value<0?"leftx-":"leftx+";break;case SDL_CONTROLLER_AXIS_LEFTY:if(std::abs(value)>=hi)return value<0?"lefty-":"lefty+";break;case SDL_CONTROLLER_AXIS_RIGHTX:if(std::abs(value)>=hi)return value<0?"rightx-":"rightx+";break;case SDL_CONTROLLER_AXIS_RIGHTY:if(std::abs(value)>=hi)return value<0?"righty-":"righty+";break;case SDL_CONTROLLER_AXIS_TRIGGERLEFT:if(value>=hi)return"lefttrigger";break;case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:if(value>=hi)return"righttrigger";break;default:break;}
#endif
    return"";
}
MickeyInputAction mickey_action_from_pad_source(const std::string& source){if(mickey_pad_binding_contains(0,source))return MickeyInputAction::Up;if(mickey_pad_binding_contains(1,source))return MickeyInputAction::Down;if(mickey_pad_binding_contains(2,source))return MickeyInputAction::Left;if(mickey_pad_binding_contains(3,source))return MickeyInputAction::Right;if(mickey_pad_binding_contains(4,source))return MickeyInputAction::Confirm;if(mickey_pad_binding_contains(5,source))return MickeyInputAction::Back;return MickeyInputAction::None;}
MickeyInputAction mickey_action_from_keyboard(SDL_Scancode sc,SDL_Keycode key){if(key==SDLK_UP||sc==psx_keybinds_get_button(1,PSX_KB_UP))return MickeyInputAction::Up;if(key==SDLK_DOWN||sc==psx_keybinds_get_button(1,PSX_KB_DOWN))return MickeyInputAction::Down;if(key==SDLK_LEFT||sc==psx_keybinds_get_button(1,PSX_KB_LEFT))return MickeyInputAction::Left;if(key==SDLK_RIGHT||sc==psx_keybinds_get_button(1,PSX_KB_RIGHT))return MickeyInputAction::Right;if(key==SDLK_RETURN||key==SDLK_KP_ENTER||sc==psx_keybinds_get_button(1,PSX_KB_CROSS))return MickeyInputAction::Confirm;if(key==SDLK_ESCAPE||sc==psx_keybinds_get_button(1,PSX_KB_CIRCLE))return MickeyInputAction::Back;return MickeyInputAction::None;}

void mickey_frontend_apply_action(Page page,int& selected,MickeyInputAction action,bool& acted,bool& back_pressed){
    if(action==MickeyInputAction::None)return;if(action==MickeyInputAction::Back){back_pressed=true;menu_audio_pickup();return;}if(action==MickeyInputAction::Confirm){acted=true;menu_audio_enter();return;}
    if(page==Page::Achievements){int n=mickey_ra_achievement_count(),login=n,back=n+1,cols=ach_grid_cols(),page_size=ach_grid_page_size(),old=selected;if(selected<n){if(action==MickeyInputAction::Left&&selected>0)selected--;else if(action==MickeyInputAction::Right&&selected+1<n)selected++;else if(action==MickeyInputAction::Up&&selected-cols>=0)selected-=cols;else if(action==MickeyInputAction::Down){if(selected+cols<n)selected+=cols;else selected=login;}}else{if(action==MickeyInputAction::Left||action==MickeyInputAction::Right)selected=(selected==login)?back:login;else if(action==MickeyInputAction::Up&&n>0)selected=std::min(n-1,g_ra_first_visible+page_size-1);}if(selected!=old)menu_audio_pickup();return;}
    if(page==Page::Controls){int old=selected;if(selected<16){int col=selected/8,row=selected%8;if(action==MickeyInputAction::Up)selected=col*8+((row+7)%8);else if(action==MickeyInputAction::Down)selected=col*8+((row+1)%8);else if(action==MickeyInputAction::Left||action==MickeyInputAction::Right)selected=(1-col)*8+row;}else{if(action==MickeyInputAction::Left||action==MickeyInputAction::Right)selected=(selected==16)?17:16;else if(action==MickeyInputAction::Up)selected=(selected==16)?7:15;}if(selected!=old)menu_audio_pickup();return;}
    if(page==Page::DisplaySettings){if((action==MickeyInputAction::Left||action==MickeyInputAction::Right)&&selected<5){change_option(selected,action==MickeyInputAction::Left?-1:+1);menu_audio_pickup();return;}int old=selected;if(action==MickeyInputAction::Up)selected=(selected+6)%7;else if(action==MickeyInputAction::Down)selected=(selected+1)%7;if(selected!=old)menu_audio_pickup();return;}
    if(page==Page::ProfileNew||page==Page::ProfileContinue){int old=selected;if(action==MickeyInputAction::Left||action==MickeyInputAction::Up)selected=(selected+2)%3;else if(action==MickeyInputAction::Right||action==MickeyInputAction::Down)selected=(selected+1)%3;if(selected!=old)menu_audio_pickup();return;}
    if(page==Page::Main){int old=selected;if(action==MickeyInputAction::Up||action==MickeyInputAction::Left)selected=(selected+4)%5;else if(action==MickeyInputAction::Down||action==MickeyInputAction::Right)selected=(selected+1)%5;if(selected!=old)menu_audio_pickup();}
}

void render_controls_page(int selected){
    if (mickey_controls_refresh_gamepads(false) &&
        !g_mickey_input_seen)
        g_mickey_input_kind=MickeyInputKind::Gamepad;
    render_common();draw_front_text(mickey_controls_text("controls"),74,272,38,1.0f,0.58f,0.09f,1.0f);draw_frame_panel(58,330,1804,610,0.82f,true);char dev[256]{};std::snprintf(dev,sizeof(dev),"%s: %s",mickey_controls_text("device"),mickey_input_device_name());draw_front_text(dev,88,350,22,1.0f,0.78f,0.30f,1.0f);draw_front_text(mickey_controls_text("active_auto"),88,382,15,0.66f,0.66f,0.64f,1.0f);
    const float cx[2]={92.0f,980.0f},y0=435.0f,step=56.0f;for(int i=0;i<16;i++){int col=i/8,row=i%8;float x=cx[col],y=y0+row*step;bool sel=selected==i;if(sel)draw_solid(x-12,y-6,790,45,0.15f,0.08f,0.02f,0.88f);draw_front_text(mickey_control_label(i),x,y,20,sel?1.0f:0.90f,sel?0.66f:0.90f,sel?0.10f:0.88f,1.0f);draw_front_text(mickey_control_binding_label(i),x+365,y,19,sel?1.0f:0.74f,sel?0.82f:0.74f,sel?0.34f:0.72f,1.0f);}
    draw_action_button(92,888,500,46,mickey_controls_text("reset"),selected==16,false);draw_action_button(1350,888,350,46,tr("back"),selected==17,true);
    if(g_mickey_controls_rebinding){draw_solid(0,0,VW,VH,0,0,0,0.58f);draw_frame_panel(525,390,870,265,0.98f,true);const char* prompt=g_mickey_rebind_kind==MickeyInputKind::Gamepad?mickey_controls_text("press_button"):mickey_controls_text("press_key");draw_front_text(prompt,585,432,30,1.0f,0.62f,0.10f,1.0f);draw_front_text(mickey_control_label(g_mickey_controls_rebind_index),585,492,24,0.94f,0.94f,0.92f,1.0f);draw_front_text(mickey_controls_text("cancel_esc"),585,580,17,0.68f,0.68f,0.66f,1.0f);}else draw_front_text(mickey_input_legend(true),88,965,16,0.64f,0.64f,0.62f,1.0f);
}
int mickey_controls_row_from_mouse(float mx,float my){const float cx[2]={92.0f,980.0f},y0=435.0f,step=56.0f;for(int i=0;i<16;i++){int col=i/8,row=i%8;float x=cx[col],y=y0+row*step;if(mx>=x-12&&mx<=x+778&&my>=y-8&&my<=y+38)return i;}if(mx>=92&&mx<=592&&my>=888&&my<=934)return 16;if(mx>=1350&&mx<=1700&&my>=888&&my<=934)return 17;return-1;}
