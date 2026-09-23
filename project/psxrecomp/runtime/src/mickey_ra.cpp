#include "mickey_ra.h"
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#ifndef _WIN32
#include "mickey_http.h"
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <wincred.h>
#endif
namespace fs=std::filesystem;
extern "C" uint8_t* memory_get_ram_ptr(void);
extern "C" uint8_t* memory_get_scratchpad_ptr(void);
namespace {
constexpr uint32_t kExpectedGameId=3843u;
fs::path g_disc_path;
rc_client_t* g_client=nullptr; fs::path g_base; bool g_loaded=false; bool g_login_pending=false; std::string g_status="RetroAchievements: not initialized"; std::mutex g_mu;
std::mutex g_unlock_mu; std::vector<MickeyRAUnlockEvent> g_unlock_events;
void set_login_pending(bool v){std::lock_guard<std::mutex> l(g_mu);g_login_pending=v;}
int login_pending(){std::lock_guard<std::mutex> l(g_mu);return g_login_pending?1:0;}
void status(const std::string& s){std::lock_guard<std::mutex> l(g_mu);g_status=s;std::fprintf(stdout,"MICKEY_RA: %s\n",s.c_str());std::fflush(stdout);}
fs::path auth_file(){return g_base/"retroachievements.ini";}
uint32_t RC_CCONV read_mem(uint32_t a,uint8_t* out,uint32_t n,rc_client_t*){
 if(!out||!n) return 0; if(a<0x200000u && n<=0x200000u-a){auto* p=memory_get_ram_ptr(); if(!p)return 0; std::memcpy(out,p+a,n); return n;}
 if(a>=0x200000u && a<0x200400u && n<=0x200400u-a){auto* p=memory_get_scratchpad_ptr(); if(!p)return 0; std::memcpy(out,p+(a-0x200000u),n); return n;} return 0;
}
#ifdef _WIN32
struct WH { HMODULE d=nullptr; decltype(&WinHttpOpen) Open=nullptr; decltype(&WinHttpCrackUrl) Crack=nullptr; decltype(&WinHttpConnect) Connect=nullptr; decltype(&WinHttpOpenRequest) OpenReq=nullptr; decltype(&WinHttpSendRequest) Send=nullptr; decltype(&WinHttpReceiveResponse) Recv=nullptr; decltype(&WinHttpQueryHeaders) Headers=nullptr; decltype(&WinHttpQueryDataAvailable) Avail=nullptr; decltype(&WinHttpReadData) Read=nullptr; decltype(&WinHttpCloseHandle) Close=nullptr; };
WH& wh(){static WH a; static bool once=false; if(!once){once=true;a.d=LoadLibraryW(L"winhttp.dll");if(a.d){
#define L(n,s) a.n=reinterpret_cast<decltype(a.n)>(GetProcAddress(a.d,s));
L(Open,"WinHttpOpen") L(Crack,"WinHttpCrackUrl") L(Connect,"WinHttpConnect") L(OpenReq,"WinHttpOpenRequest") L(Send,"WinHttpSendRequest") L(Recv,"WinHttpReceiveResponse") L(Headers,"WinHttpQueryHeaders") L(Avail,"WinHttpQueryDataAvailable") L(Read,"WinHttpReadData") L(Close,"WinHttpCloseHandle")
#undef L
}}return a;}
std::wstring widen(const char* s){if(!s)return{};int n=MultiByteToWideChar(CP_UTF8,0,s,-1,nullptr,0);if(n<=0)return{};std::wstring w((size_t)n,L'\0');MultiByteToWideChar(CP_UTF8,0,s,-1,w.data(),n);if(!w.empty()&&w.back()==0)w.pop_back();return w;}
#endif
void RC_CCONV server_call(const rc_api_request_t* req,rc_client_server_callback_t cb,void* data,rc_client_t*){
 if(!cb)return;
 rc_api_server_response_t r{}; std::string body;
#ifdef _WIN32
 auto& a=wh(); if(!req||!req->url||!cb||!a.Open||!a.Crack||!a.Connect||!a.OpenReq||!a.Send||!a.Recv||!a.Headers||!a.Avail||!a.Read||!a.Close){r.http_status_code=RC_API_SERVER_RESPONSE_CLIENT_ERROR;cb(&r,data);return;}
 std::wstring url=widen(req->url); URL_COMPONENTS u{};u.dwStructSize=sizeof(u); wchar_t host[512]{},path[4096]{},extra[4096]{};u.lpszHostName=host;u.dwHostNameLength=511;u.lpszUrlPath=path;u.dwUrlPathLength=4095;u.lpszExtraInfo=extra;u.dwExtraInfoLength=4095;
 if(!a.Crack(url.c_str(),(DWORD)url.size(),0,&u)){r.http_status_code=RC_API_SERVER_RESPONSE_CLIENT_ERROR;cb(&r,data);return;}
 std::wstring full(path,u.dwUrlPathLength); if(u.dwExtraInfoLength) full.append(extra,u.dwExtraInfoLength);
 HINTERNET ses=a.Open(L"MickeyWildAdventureRecomp/1.0",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
 HINTERNET con=ses?a.Connect(ses,std::wstring(host,u.dwHostNameLength).c_str(),u.nPort,0):nullptr; bool post=req->post_data&&req->post_data[0]; DWORD flags=(u.nScheme==INTERNET_SCHEME_HTTPS)?WINHTTP_FLAG_SECURE:0;
 HINTERNET h=con?a.OpenReq(con,post?L"POST":L"GET",full.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,flags):nullptr; bool ok=false;
 if(h){std::wstring hdr;if(post&&req->content_type)hdr=L"Content-Type: "+widen(req->content_type);const char* pd=post?req->post_data:nullptr;DWORD pn=pd?(DWORD)std::strlen(pd):0;ok=!!a.Send(h,hdr.empty()?WINHTTP_NO_ADDITIONAL_HEADERS:hdr.c_str(),hdr.empty()?0:(DWORD)-1L,pn?(LPVOID)pd:WINHTTP_NO_REQUEST_DATA,pn,pn,0);if(ok)ok=!!a.Recv(h,nullptr);if(ok){DWORD sc=0,sz=sizeof(sc);if(a.Headers(h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&sc,&sz,WINHTTP_NO_HEADER_INDEX))r.http_status_code=(int)sc;for(;;){DWORD av=0;if(!a.Avail(h,&av)||!av)break;size_t old=body.size();body.resize(old+av);DWORD got=0;if(!a.Read(h,body.data()+old,av,&got)){body.resize(old);break;}body.resize(old+got);if(!got)break;}}}
 if(h)a.Close(h);if(con)a.Close(con);if(ses)a.Close(ses);if(!ok&&r.http_status_code==0)r.http_status_code=RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
#else
 if(req&&req->url){
  auto response=mickey_http_request(req->url,req->post_data,req->content_type);
  r.http_status_code=response.status?response.status:RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
  body=std::move(response.body);
 }else r.http_status_code=RC_API_SERVER_RESPONSE_CLIENT_ERROR;
#endif
 r.body=body.c_str();r.body_length=body.size();cb(&r,data);
}
bool load_auth(std::string& user,std::string& token){std::ifstream f(auth_file(),std::ios::binary);std::string l;while(std::getline(f,l)){if(!l.empty()&&l.back()=='\r')l.pop_back();auto p=l.find('=');if(p==std::string::npos)continue;if(l.substr(0,p)=="username")user=l.substr(p+1);else if(l.substr(0,p)=="token")token=l.substr(p+1);}return !user.empty()&&!token.empty();}
void save_auth(){const rc_client_user_t* u=g_client?rc_client_get_user_info(g_client):nullptr;if(!u||!u->username||!u->token)return;std::ofstream f(auth_file(),std::ios::binary|std::ios::trunc);if(f)f<<"username="<<u->username<<"\n"<<"token="<<u->token<<"\n";}
fs::path disc(){if(!g_disc_path.empty())return g_disc_path;std::vector<fs::path> roots={g_base/"disc",g_base,fs::current_path()/"disc",fs::current_path()};const char* exts[]={".cue",".chd",".iso",".bin"};for(auto ext:exts)for(auto& root:roots){std::error_code ec;if(!fs::is_directory(root,ec))continue;for(auto& e:fs::directory_iterator(root,ec)){if(ec)break;if(!e.is_regular_file())continue;std::string x=e.path().extension().string();std::transform(x.begin(),x.end(),x.begin(),[](unsigned char c){return(char)std::tolower(c);});if(x==ext)return e.path();}}return{};}
void RC_CCONV load_done(int result,const char* err,rc_client_t* c,void*){if(result!=RC_OK){g_loaded=false;status(std::string("game load failed: ")+(err?err:"unknown"));return;}const auto* gi=rc_client_get_game_info(c);if(!gi||gi->id!=kExpectedGameId){g_loaded=false;char b[160];std::snprintf(b,sizeof(b),"unsupported game id %u (expected 3843)",gi?gi->id:0);status(b);return;}g_loaded=true;status(std::string("ready: ")+(gi->title?gi->title:"Mickey's Wild Adventure"));}
void begin_game(){if(!g_client||!rc_client_get_user_info(g_client))return;auto p=disc();if(p.empty()){status("signed in; PS1 disc image not found for RA hash");return;}char hash[33]{};if(!rc_hash_generate_from_file(hash,RC_CONSOLE_PLAYSTATION,p.string().c_str())){status("could not hash PlayStation disc");return;}status(std::string("loading Game ID 3843; hash=")+hash);rc_client_begin_load_game(g_client,hash,load_done,nullptr);}
void RC_CCONV login_done(int result,const char* err,rc_client_t* c,void*){set_login_pending(false);if(result!=RC_OK){status(std::string("sign-in failed: ")+(err?err:"unknown"));return;}save_auth();const auto*u=rc_client_get_user_info(c);status(std::string("signed in: ")+(u&&u->username?u->username:""));begin_game();}
void RC_CCONV event_cb(const rc_client_event_t* e,rc_client_t*){
 if(e&&e->type==RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED&&e->achievement){
  MickeyRAUnlockEvent ev{};
  ev.id=e->achievement->id; ev.points=e->achievement->points;
  std::snprintf(ev.title,sizeof(ev.title),"%s",e->achievement->title?e->achievement->title:"");
  std::snprintf(ev.description,sizeof(ev.description),"%s",e->achievement->description?e->achievement->description:"");
  std::snprintf(ev.badge_url,sizeof(ev.badge_url),"%s",e->achievement->badge_url?e->achievement->badge_url:"");
  { std::lock_guard<std::mutex> l(g_unlock_mu); g_unlock_events.push_back(ev); }
  status(std::string("unlocked: ")+(e->achievement->title?e->achievement->title:"achievement"));
 }
}
}
extern "C" void mickey_ra_init(const char* base){if(g_client)return;g_base=(base&&*base)?fs::path(base):fs::current_path();g_client=rc_client_create(read_mem,server_call);if(!g_client){status("rc_client_create failed");return;}rc_client_set_hardcore_enabled(g_client,0);rc_client_set_event_handler(g_client,event_cb);std::string u,t;if(load_auth(u,t)){status("restoring session");set_login_pending(true);rc_client_begin_login_with_token(g_client,u.c_str(),t.c_str(),login_done,nullptr);}else status("not signed in");}
extern "C" void mickey_ra_set_disc_path(const char* path){g_disc_path=path?fs::path(path):fs::path();}
extern "C" void mickey_ra_shutdown(void){if(g_client){rc_client_destroy(g_client);g_client=nullptr;}g_loaded=false;}
extern "C" void mickey_ra_do_frame(void){if(g_client&&g_loaded)rc_client_do_frame(g_client);}
extern "C" void mickey_ra_idle(void){if(g_client)rc_client_idle(g_client);}
extern "C" void mickey_ra_reset_runtime(void){if(g_client&&g_loaded)rc_client_reset(g_client);}
extern "C" void mickey_ra_on_savestate_result(int is_load,int slot,int ok){
 if(!ok||!g_client||!g_loaded||slot<0)return;
 const fs::path dir=g_base/"ra_states"; std::error_code ec; fs::create_directories(dir,ec);
 const fs::path file=dir/(std::string("slot_")+std::to_string(slot)+".bin");
 if(!is_load){
  const size_t n=rc_client_progress_size(g_client); if(!n)return; std::vector<uint8_t> data(n);
  if(rc_client_serialize_progress_sized(g_client,data.data(),data.size())!=RC_OK){status("could not serialize RA savestate progress");return;}
  std::ofstream out(file,std::ios::binary|std::ios::trunc); if(!out){status("could not write RA savestate sidecar");return;}
  const uint8_t hdr[12]={'M','R','A','1',0x03,0x0F,0,0,(uint8_t)(n&255),(uint8_t)((n>>8)&255),(uint8_t)((n>>16)&255),(uint8_t)((n>>24)&255)};
  out.write((const char*)hdr,sizeof(hdr)); out.write((const char*)data.data(),(std::streamsize)data.size());
 } else {
  std::ifstream in(file,std::ios::binary); if(!in){rc_client_reset(g_client);return;} uint8_t hdr[12]{};in.read((char*)hdr,sizeof(hdr));
  const uint32_t gid=(uint32_t)hdr[4]|((uint32_t)hdr[5]<<8)|((uint32_t)hdr[6]<<16)|((uint32_t)hdr[7]<<24);
  const uint32_t n=(uint32_t)hdr[8]|((uint32_t)hdr[9]<<8)|((uint32_t)hdr[10]<<16)|((uint32_t)hdr[11]<<24);
  if(!in||std::memcmp(hdr,"MRA1",4)!=0||gid!=kExpectedGameId||!n||n>16u*1024u*1024u){rc_client_reset(g_client);return;}
  std::vector<uint8_t> data(n); in.read((char*)data.data(),(std::streamsize)n);
  if(!in||rc_client_deserialize_progress_sized(g_client,data.data(),data.size())!=RC_OK)rc_client_reset(g_client);
 }
}
extern "C" int mickey_ra_logged_in(void){return g_client&&rc_client_get_user_info(g_client)?1:0;}
extern "C" int mickey_ra_game_loaded(void){return g_loaded?1:0;}
extern "C" const char* mickey_ra_username(void){const auto*u=g_client?rc_client_get_user_info(g_client):nullptr;return u&&u->username?u->username:"";}
extern "C" const char* mickey_ra_status(void){static thread_local std::string s;{std::lock_guard<std::mutex> l(g_mu);s=g_status;}return s.c_str();}
extern "C" int mickey_ra_login_pending(void){return login_pending();}
extern "C" int mickey_ra_login_with_password(const char* username,const char* password){if(!g_client||!username||!password)return 0;std::string u=username,p=password;auto trim=[](std::string& s){size_t a=0;while(a<s.size()&&std::isspace((unsigned char)s[a]))a++;size_t b=s.size();while(b>a&&std::isspace((unsigned char)s[b-1]))b--;s=s.substr(a,b-a);};trim(u);trim(p);if(u.empty()||p.empty()){status("sign-in failed: missing username or password");return 0;}set_login_pending(true);status("signing in...");rc_client_begin_login_with_password(g_client,u.c_str(),p.c_str(),login_done,nullptr);return 1;}
extern "C" int mickey_ra_achievement_count(void){if(!g_client||!g_loaded)return 0;auto*l=rc_client_create_achievement_list(g_client,RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);if(!l)return 0;int n=0;for(uint32_t i=0;i<l->num_buckets;i++)n+=(int)l->buckets[i].num_achievements;rc_client_destroy_achievement_list(l);return n;}
extern "C" int mickey_ra_get_achievement(int idx,MickeyRAAchievement*out){if(!out||idx<0||!g_client||!g_loaded)return 0;auto*l=rc_client_create_achievement_list(g_client,RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);if(!l)return 0;int cur=0,ok=0;for(uint32_t b=0;b<l->num_buckets&&!ok;b++)for(uint32_t i=0;i<l->buckets[b].num_achievements;i++,cur++){if(cur!=idx)continue;auto*a=l->buckets[b].achievements[i];if(a){std::memset(out,0,sizeof(*out));out->id=a->id;out->points=a->points;out->unlocked=a->unlocked!=RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE;std::snprintf(out->title,sizeof(out->title),"%s",a->title?a->title:"");std::snprintf(out->description,sizeof(out->description),"%s",a->description?a->description:"");std::snprintf(out->badge_url,sizeof(out->badge_url),"%s",a->badge_url?a->badge_url:"");std::snprintf(out->badge_locked_url,sizeof(out->badge_locked_url),"%s",a->badge_locked_url?a->badge_locked_url:"");ok=1;}break;}rc_client_destroy_achievement_list(l);return ok;}

extern "C" int mickey_ra_pop_unlock_event(MickeyRAUnlockEvent* out){
 if(!out)return 0;
 std::lock_guard<std::mutex> l(g_unlock_mu);
 if(g_unlock_events.empty())return 0;
 *out=g_unlock_events.front();
 g_unlock_events.erase(g_unlock_events.begin());
 return 1;
}
extern "C" void mickey_ra_logout(void){if(g_client){if(g_loaded)rc_client_unload_game(g_client);rc_client_logout(g_client);}set_login_pending(false);g_loaded=false;std::error_code ec;fs::remove(auth_file(),ec);status("signed out");}
extern "C" int mickey_ra_login_prompt(int lang){
#ifdef _WIN32
 if(!g_client)return 0;HMODULE d=LoadLibraryW(L"credui.dll");if(!d){status("CredUI unavailable");return 0;}using Fn=DWORD(WINAPI*)(PCREDUI_INFOW,LPCWSTR,PVOID,DWORD,LPWSTR,ULONG,LPWSTR,ULONG,PBOOL,DWORD);auto fn=reinterpret_cast<Fn>(GetProcAddress(d,"CredUIPromptForCredentialsW"));if(!fn){FreeLibrary(d);return 0;}
 static const wchar_t* cap[5]={L"Logowanie RetroAchievements",L"RetroAchievements sign in",L"Accesso RetroAchievements",L"RetroAchievements ログイン",L"RetroAchievements-Anmeldung"}; static const wchar_t* msg[5]={L"Podaj nazwę użytkownika i hasło. Hasło nie zostanie zapisane.",L"Enter username and password. The password will not be saved.",L"Inserisci nome utente e password. La password non verrà salvata.",L"ユーザー名とパスワードを入力してください。パスワードは保存されません。",L"Benutzername und Passwort eingeben. Das Passwort wird nicht gespeichert."};lang=std::clamp(lang,0,4);CREDUI_INFOW info{};info.cbSize=sizeof(info);info.pszCaptionText=cap[lang];info.pszMessageText=msg[lang];wchar_t user[256]{},pass[256]{};BOOL save=FALSE;DWORD rc=fn(&info,L"RetroAchievements",nullptr,0,user,256,pass,256,&save,CREDUI_FLAGS_GENERIC_CREDENTIALS|CREDUI_FLAGS_ALWAYS_SHOW_UI|CREDUI_FLAGS_DO_NOT_PERSIST|CREDUI_FLAGS_EXCLUDE_CERTIFICATES);if(rc==NO_ERROR){char u[768]{},p[768]{};WideCharToMultiByte(CP_UTF8,0,user,-1,u,sizeof(u),nullptr,nullptr);WideCharToMultiByte(CP_UTF8,0,pass,-1,p,sizeof(p),nullptr,nullptr);const int started=mickey_ra_login_with_password(u,p);SecureZeroMemory(p,sizeof(p));SecureZeroMemory(pass,sizeof(pass));FreeLibrary(d);return started;}SecureZeroMemory(pass,sizeof(pass));FreeLibrary(d);
#endif
 return 0;
}
