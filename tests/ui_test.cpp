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
    assert(typedCharacter(20,false,false,true)=='/');
    assert(typedCharacter(30,true,false,false)=='A');
    assert(typedCharacter(16,false,false,false)=='q');
    assert(typedCharacter(11,true,false,false)==')');
    lv_init();lv_tick_set_cb([](){return clockMs;});auto *d=lv_display_create(320,170);
    static unsigned char pixels[320*170*2];
    lv_display_set_buffers(d,pixels,nullptr,sizeof(pixels),LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(d,[](lv_display_t *display,const lv_area_t *,uint8_t *){lv_display_flush_ready(display);});
    ui_init();
    posts={{"Mira @mira@example.org","A sample post.","123",0,2,3,4}};token="test";mode=Browse;
    for(bool theme:{false,true}){darkTheme=theme;draw();lv_obj_update_layout(lv_screen_active());
        assert(!lv_obj_has_flag(timelineBar,LV_OBJ_FLAG_HIDDEN));assert(lv_obj_get_width(feedViewport)==274 && lv_obj_get_height(feedViewport)==64);
        assert(std::string(lv_label_get_text(postActions)).find("Replies 2")!=std::string::npos);}
    mode=Settings;draw();assert(lv_obj_has_flag(timelineBar,LV_OBJ_FLAG_HIDDEN));
    posts.clear();token.clear();mode=Browse;darkTheme=false;draw();
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
    // Content warnings stay hidden until revealed, and retain their state on refresh.
    posts={{"Mira @mira@example.social","Secret words","88",0}};indexPost=0;mode=Browse;
    posts[0].target="88";posts[0].acct="mira@example.social";posts[0].visibility="private";posts[0].warning="Spoiler";draw();
    assert(std::string(lv_label_get_text(feedText)).find("Secret words")==std::string::npos);
    key('w');assert(std::string(lv_label_get_text(feedText)).find("Secret words")!=std::string::npos);
    key('p');assert(mode==Compose && replyTarget=="88" && replyVisibility=="private" && input=="@mira@example.social ");
    key('h',"hi");key(K_ENTER);assert(mode==Confirm);outgoing.clear();key('y');
    {auto sent=json::parse(outgoing.substr(0,outgoing.find('\n')));assert(sent["reply_to"]=="88" && sent["visibility"]=="private");}
    response({{"published",true}});assert(replyTarget.empty() && mode==Browse);
    key('b');assert(notice=="This post cannot be boosted.");
    posts[0].visibility="public";outgoing.clear();key('b');assert(busy);
    {auto sent=json::parse(outgoing.substr(0,outgoing.find('\n')));assert(sent["target"]=="88" && !sent["undo"].get<bool>());}
    response({{"target","88"},{"boosted",true},{"boosts",5}});assert(posts[0].boosted && posts[0].boosts==5);
    outgoing.clear();key('q');assert(mode==Search);key('c',"cats");key(K_ENTER);assert(pendingOp=="search");
    response({{"posts",json::array({{{"id","77"},{"target","77"},{"title","Result"},{"body","Found"}}})}});
    assert(searchView && posts.size()==1 && posts[0].id=="77");key(K_ESCAPE);assert(!searchView && posts[0].id=="88");
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
