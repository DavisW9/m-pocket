#define POCKET_TESTING 1
#include "../src/ui.cpp"
#include <cassert>
#include <fstream>
#include <filesystem>
#include "../src/keyboard_text.hpp"
static uint32_t clockMs=0;
static void response(const json &fields){auto v=fields;v["id"]=pendingID;v["ok"]=true;receive(v);}
int main(int argc,char **argv){
    assert(typedCharacter(232,false,false,false)=='.');
    assert(typedCharacter(212,false,false,false)=='_');
    assert(typedCharacter(31,false,false,true)=='.');
    assert(typedCharacter(30,true,false,false)=='A');
    assert(typedCharacter(16,false,false,false)=='q');
    assert(typedCharacter(11,true,false,false)==')');
    lv_init();lv_tick_set_cb([](){return clockMs;});auto *d=lv_display_create(320,170);
    static unsigned char pixels[320*170*2];
    lv_display_set_buffers(d,pixels,nullptr,sizeof(pixels),LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(d,[](lv_display_t *display,const lv_area_t *,uint8_t *){lv_display_flush_ready(display);});
    ui_init();
    auto capture=[&](const char *name){if(argc>2){lv_refr_now(d);std::ofstream out(std::string(argv[2])+"/"+name+".rgb565",std::ios::binary);out.write((char*)pixels,sizeof(pixels));}};
    capture("welcome");
    assert(mode==Browse);key('o');assert(mode==Settings);key('1');assert(darkTheme);key('2');assert(largeText);
    capture("settings-dark");key('4');assert(mode==Help);key(K_ESCAPE);key('s');key('3');assert(mode==Server);key('a',"a");key(K_ESCAPE);assert(server=="mastodon.social");
    token="signed-in";home=true;std::string longText;for(int i=0;i<150;i++)longText+="Line café, enough words to wrap.\n";
    posts={{"First",longText,"500",0},{"Second","Short","499",0}};indexPost=0;mode=Browse;draw();
    for(int i=0;i<1000;i++)key('x',"",true);
    int maximum=std::max(0,(int)lv_obj_get_height(feedText)-(int)lv_obj_get_height(feedViewport));
    assert(posts[0].scroll==maximum && maximum>1000);
    key('c');key('z');assert(posts[0].scroll==maximum);
    key('o');key('2');assert(posts[0].scroll==0);key(K_ESCAPE);
    key('x');int scroll=posts[0].scroll;key('r');assert(busy);
    response({{"posts",json::array({{{"id","500"},{"title","First"},{"body",longText}},{{"id","499"},{"title","Second"},{"body","Short"}}})}});
    assert(posts[0].scroll==scroll && !busy);
    key('c');key('c');assert(pendingAppend && busy);
    response({{"posts",json::array({{{"id","499"},{"title","Second"},{"body","Short"}},{{"id","498"},{"title","Third"},{"body","More"}}})}});
    assert(posts.size()==3 && indexPost==2);
    key('c');response({{"posts",json::array()}});assert(endOfFeed);
    key('n');key('a',"draft");key(K_ENTER);assert(mode==Confirm);key(K_ESCAPE);assert(input.empty() && mode==Browse);
    key('o');key('5');response({{"signed_out",true}});assert(token.empty() && posts.empty());
    key('l');response({{"qr","https://pocket-mastodon-callback.davforge.workers.dev/s/"+std::string(64,'a')}});assert(mode==QR && nextPoll>0);capture("qr");
    key('a');assert(mode==TokenHelp);response(json::object());key(K_ENTER);assert(mode==Token);
    key('a',"sensitive-token");draw();assert(std::string(lv_label_get_text(body)).find("sensitive-token")==std::string::npos);
    key(K_ENTER);assert(input.empty());response({{"authenticated",true}});assert(home && !token.empty() && pendingOp=="feed");
    response({{"posts",json::array()}});assert(!busy);
    if(argc>1){
        outgoing.clear();incoming.clear();busy=false;
        char tmp[]="/tmp/m-pocket-ipc-XXXXXX";char *directory=mkdtemp(tmp);assert(directory);
        const char *old=getenv("XDG_CONFIG_HOME");std::string oldValue=old?old:"";
        setenv("XDG_CONFIG_HOME",directory,1);
        assert(startHelper(argv[1]));
        auto wait=[&](){double deadline=nowSeconds()+5;while(busy && nowSeconds()<deadline){pump();usleep(10000);}assert(!busy && commandFD>=0);};
        send({{"op","prefs"}});wait();assert(!darkTheme && !largeText);
        darkTheme=true;largeText=true;savePreferences();send({{"op","prefs"}});wait();assert(darkTheme && largeText);
        setServer("example.social");wait();assert(server=="example.social");
        stopHelper();assert(startHelper(argv[1]));send({{"op","prefs"}});wait();assert(server=="example.social" && darkTheme && largeText);
        stopHelper();std::filesystem::remove_all(directory);
        if(old)setenv("XDG_CONFIG_HOME",oldValue.c_str(),1);else unsetenv("XDG_CONFIG_HOME");
        puts("Real helper IPC and settings persistence across restart passed.");
    }
    lv_deinit();puts("UI tests passed: settings, scroll bounds, paging, refresh, QR fallback, masking, compose cancel, sign-out.");
}
