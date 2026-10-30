// m pocket for Cardputer Zero. UI runs on one thread; networking uses a private helper process.
#include "lvgl.h"
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
using json=nlohmann::json;
static lv_obj_t *feedViewport,*feedText,*progress,*head,*body,*foot,*qr,*topbar,*card,*meta,*brand,*brandMark,*keybar,*choices;
static lv_obj_t *menuKeys[5],*menuText[5],*menuValues[5];
static bool darkTheme=false,largeText=false,busy=false,home=false,endOfFeed=false,running=true;
// This is only an authentication indicator. Access tokens stay in the helper process.
static std::string token,server="mastodon.social",input,notice;
struct Post {std::string title,body,id;int scroll=0;};
static std::vector<Post> posts;
static int indexPost=0;
enum Mode {Settings,Help,Browse,ServerChoice,Server,Token,TokenHelp,AuthFallback,QR,Compose,Confirm};
static Mode mode=Browse;
static double nowSeconds(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static double nextPoll=0;
static int commandFD=-1,responseFD=-1;
static pid_t helperPID=-1;
static std::string outgoing,incoming,pendingOp;
static unsigned serial=0,pendingID=0;
static bool pendingAppend=false;
static void draw();
static void send(json command,bool foreground=true){
    if(foreground){pendingID=++serial;command["id"]=pendingID;pendingOp=command.at("op").get<std::string>();busy=true;}
    else command["id"]=0;
    outgoing+=command.dump()+"\n";
}
static void savePreferences(){send({{"op","save_prefs"},{"dark",darkTheme},{"large",largeText}},false);}
static bool showQR(const std::string &url){return lv_qrcode_update(qr,url.data(),url.size())==LV_RESULT_OK;}
static void applyTheme() {
    const uint32_t paper=darkTheme?0x21192d:0xf3effa;
    const uint32_t ink=darkTheme?0xeee7f8:0x302740;
    const uint32_t muted=darkTheme?0xbaaccb:0x726585;
    const uint32_t green=darkTheme?0xc9aaf1:0x7054aa;
    lv_obj_set_style_bg_color(lv_screen_active(),lv_color_hex(paper),0);
    lv_obj_set_style_text_color(feedText,lv_color_hex(ink),0);
    lv_obj_set_style_bg_color(progress,lv_color_hex(green),0);
    lv_obj_set_style_text_color(brandMark,lv_color_hex(green),0);
    for(auto *o:{body,brand}) lv_obj_set_style_text_color(o,lv_color_hex(ink),0);
    lv_obj_set_style_text_color(foot,lv_color_hex(0xf3effa),0);
    lv_obj_set_style_text_color(head,lv_color_hex(muted),0); lv_obj_set_style_text_color(meta,lv_color_hex(green),0);
    lv_obj_set_style_bg_color(topbar,lv_color_hex(darkTheme?0x51405f:0xd4c6e6),0);
    lv_obj_set_style_bg_color(keybar,lv_color_hex(darkTheme?0x382949:0x49345f),0);
    for(int i=0;i<5;i++) {
        lv_obj_set_style_bg_color(menuKeys[i],lv_color_hex(darkTheme?0x413151:0xe6dbf3),0);
        lv_obj_set_style_text_color(menuKeys[i],lv_color_hex(green),0);
        lv_obj_set_style_text_color(menuText[i],lv_color_hex(ink),0);
        lv_obj_set_style_text_color(menuValues[i],lv_color_hex(muted),0);
    }
}
static void draw() {
    applyTheme();
    const bool isQR=mode==QR;
    auto visible=[](lv_obj_t *o,bool show){if(show) lv_obj_remove_flag(o,LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);};
    visible(feedViewport,false); visible(progress,false);
    visible(qr,isQR); visible(topbar,!isQR); visible(brand,!isQR); visible(brandMark,!isQR); visible(keybar,!isQR);
    visible(card,false); visible(choices,false); visible(meta,false);
    lv_obj_set_pos(body,12,43); lv_obj_set_size(body,296,95); lv_obj_set_style_text_font(body,largeText?&lv_font_montserrat_16:&lv_font_montserrat_14,0);
    lv_label_set_text(brand,"pocket");
    if(isQR) {
        lv_label_set_text(head,""); lv_label_set_text(foot,"");
        lv_obj_set_pos(body,172,10); lv_obj_set_size(body,140,150); lv_obj_set_style_text_font(body,&lv_font_montserrat_12,0);
        lv_label_set_text(body,"SIGN IN\n\nScan with phone.\nApprove access.\n\nSign-in finishes\nautomatically.\n\nA  Access token\nEsc  Cancel"); return;
    }
    if(mode==Settings || mode==ServerChoice) {
        const bool settings=mode==Settings;
        lv_label_set_text(head,settings?"SETTINGS":"SERVER");visible(choices,true);lv_label_set_text(body,"");
        const char *labels[]={"Appearance","Text size","Server","Keyboard","Sign out"};
        const char *servers[]={"mastodon.social","mastodon.online","Another server..."};
        for(int i=0;i<5;i++) {
            bool show=settings || i<3; visible(menuKeys[i],show); visible(menuText[i],show); visible(menuValues[i],show);
            if(!show) continue;
            lv_label_set_text(menuText[i],settings?labels[i]:servers[i]);
            std::string value=settings?(i==0?(darkTheme?"Dark":"Light"):i==1?(largeText?"Large":"Normal"):i==2?server:i==3?"Shortcuts":token.empty()?"Not signed in":"This session"):"";
            lv_label_set_text(menuValues[i],value.c_str());
        }
        lv_label_set_text(foot,settings?"1-5  Change                         Esc  Back":"1-3  Select                         Esc  Back");return;
    }
    if(mode==Help) {
        lv_label_set_text(head,"KEYBOARD"); lv_obj_set_pos(body,12,37); lv_obj_set_height(body,106);lv_obj_set_style_text_font(body,&lv_font_montserrat_12,0);
        lv_label_set_text(body,"Z / C   Previous / next post\nF / X   Scroll up / down\nR         Refresh feed\nT         Home / local feed\nN         Write a post\nS / L   Server / sign in\nO         Settings"); lv_label_set_text(foot,"A  Manual access token                Esc  Back"); return;
    }

    if(mode==Server) { lv_label_set_text(head,"SERVER"); lv_label_set_text(body,("Your server address\n\n"+input+"_").c_str()); lv_label_set_text(foot,"Enter  Save                         Esc  Back"); return; }
    if(mode==AuthFallback) {
        lv_label_set_text(head,"SIGN-IN OPTIONS");lv_obj_set_style_text_font(body,&lv_font_montserrat_12,0);
        lv_obj_set_pos(body,12,39);lv_obj_set_size(body,296,103);
        lv_label_set_text(body,(notice+"\n\n1  Retry QR sign-in\n2  Use an access token").c_str());
        lv_label_set_text(foot,"Manual sign-in goes directly to your server");return;
    }
    if(mode==TokenHelp) {
        lv_label_set_text(head,"MANUAL SIGN-IN");lv_obj_set_pos(body,12,38);lv_obj_set_size(body,296,106);lv_obj_set_style_text_font(body,&lv_font_montserrat_12,0);
        lv_label_set_text(body,("On "+server+":\nPreferences > Development > New app\nEnable read:statuses (feed),\nwrite:statuses (posting).\nSave, then copy Your access token.\nKeep the token private.").c_str());
        lv_label_set_text(foot,"Enter  Type token                     Esc  Back");return;
    }
    if(mode==Token) {
        lv_label_set_text(head,"ACCESS TOKEN");lv_obj_set_style_text_font(body,&lv_font_montserrat_12,0);
        std::string masked(std::min<size_t>(input.size(),34),'*');
        lv_label_set_text(body,(server+"\n\n"+masked+"_\n\n"+(notice.empty()?"Your access token, not your password.":notice)).c_str());
        lv_label_set_text(foot,busy?"Checking token...":"Enter Sign in   Esc Cancel");return;
    }
    if(mode==Compose || mode==Confirm) { lv_label_set_text(head,mode==Confirm?"READY TO POST?":"WRITE / PUBLIC"); lv_label_set_text(body,input.empty()?"What's happening?":input.c_str()); lv_label_set_text(foot,mode==Confirm?"Y  Publish                          N  Cancel":(!notice.empty()?notice.c_str():"Enter  Review                       Esc  Cancel")); return; }
    std::string h=home?"HOME":"LOCAL"; if(!posts.empty()) h+="  "+std::to_string(indexPost+1)+"/"+std::to_string(posts.size());
    lv_label_set_text(head,h.c_str());
    if(token.empty() && posts.empty() && (notice.empty() || notice.find("rejected access")!=std::string::npos)) {
        lv_obj_set_style_text_font(body,&lv_font_montserrat_20,0); lv_obj_set_pos(body,12,49); lv_obj_set_height(body,54);
        lv_label_set_text(body,"m pocket");
        visible(meta,true); lv_obj_set_pos(meta,12,116); lv_label_set_text(meta,server.c_str());
        lv_label_set_text(foot,"L Sign in   A Token   S Server   O Settings"); return;
    }
    if(!notice.empty() && posts.empty()) lv_label_set_text(body,notice.c_str());
    else if(!posts.empty()) {
        visible(meta,true); lv_obj_set_pos(meta,12,39); lv_label_set_text(meta,posts[indexPost].title.c_str());
        lv_obj_set_pos(body,12,61); lv_obj_set_size(body,296,78);
        lv_label_set_text(body,""); visible(feedViewport,true);
        auto &post=posts[indexPost];
        const lv_font_t *font=largeText?&lv_font_montserrat_16:&lv_font_montserrat_14;
        lv_obj_set_style_text_font(feedText,font,0);
        lv_label_set_text(feedText,post.body.c_str());
        lv_obj_update_layout(feedViewport);
        const int height=lv_obj_get_height(feedText), viewport=lv_obj_get_height(feedViewport);
        const int maximum=std::max(0,height-viewport);
        post.scroll=std::clamp(post.scroll,0,maximum);
        lv_obj_scroll_to_y(feedViewport,post.scroll,LV_ANIM_OFF);
        if(maximum>0) {
            visible(progress,true);
            int thumb=std::max(8,viewport*viewport/height);
            lv_obj_set_height(progress,thumb);
            lv_obj_set_y(progress,61+(viewport-thumb)*post.scroll/maximum);
        }
    } else lv_label_set_text(body,home?"No posts yet.\n\nFollow a few accounts,\nthen refresh with R.":"No local posts.\n\nR to try again.");
    std::string hint=busy?"Loading...  F/X Scroll  O Settings":!notice.empty()?notice:endOfFeed && indexPost+1==(int)posts.size()?"End of feed   R Refresh   O Settings":"Z/C Posts   F/X Scroll   R Refresh   O Settings";
    lv_label_set_text(foot,hint.c_str());
}
static void ui_init() {
    lv_obj_t *screen=lv_screen_active(); lv_obj_set_style_bg_color(screen,lv_color_hex(0xf2efdf),0);
    auto panel=[&](int x,int y,int w,int h,uint32_t color){auto *o=lv_obj_create(screen);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);return o;};
    topbar=panel(12,30,296,1,0x262a28); card=panel(0,0,1,1,0xf2efdf); keybar=panel(0,146,320,24,0x262a28);
    auto label=[&](int x,int y,int w,const lv_font_t *font,uint32_t color){auto *o=lv_label_create(screen);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);return o;};
    brandMark=label(12,7,18,&lv_font_montserrat_16,0x7054aa);lv_label_set_text(brandMark,"m");
    brand=label(33,7,80,&lv_font_montserrat_16,0x262a28);
    head=label(116,11,192,&lv_font_montserrat_10,0x545b52); lv_obj_set_style_text_align(head,LV_TEXT_ALIGN_RIGHT,0);
    meta=label(12,39,296,&lv_font_montserrat_12,0x4d6551); lv_label_set_long_mode(meta,LV_LABEL_LONG_DOT);
    body=label(12,43,296,&lv_font_montserrat_14,0x262a28);lv_obj_set_height(body,95);lv_label_set_long_mode(body,LV_LABEL_LONG_WRAP);
    feedViewport=lv_obj_create(screen); lv_obj_remove_style_all(feedViewport);
    lv_obj_set_pos(feedViewport,12,61);lv_obj_set_size(feedViewport,290,78);
    lv_obj_set_scrollbar_mode(feedViewport,LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(feedViewport,LV_DIR_VER);
    lv_obj_remove_flag(feedViewport,LV_OBJ_FLAG_SCROLL_ELASTIC);lv_obj_remove_flag(feedViewport,LV_OBJ_FLAG_SCROLL_MOMENTUM);
    feedText=lv_label_create(feedViewport); lv_obj_set_width(feedText,290);lv_obj_set_height(feedText,LV_SIZE_CONTENT);
    lv_label_set_long_mode(feedText,LV_LABEL_LONG_WRAP);
    progress=panel(307,61,2,8,0x7054aa);
    foot=label(10,153,300,&lv_font_montserrat_10,0xf2efdf);
    lv_obj_set_height(foot,12);lv_label_set_long_mode(foot,LV_LABEL_LONG_DOT);
    choices=lv_obj_create(screen);lv_obj_remove_style_all(choices);lv_obj_set_pos(choices,12,38);lv_obj_set_size(choices,296,100);lv_obj_clear_flag(choices,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(choices,296,105);
    for(int i=0;i<5;i++) {
        auto *n=lv_label_create(choices);menuKeys[i]=n;lv_obj_set_pos(n,0,i*21);lv_obj_set_size(n,18,18);lv_label_set_text_fmt(n,"%d",i+1);lv_obj_set_style_text_font(n,&lv_font_montserrat_10,0);lv_obj_set_style_text_align(n,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_pad_top(n,3,0);lv_obj_set_style_bg_opa(n,LV_OPA_COVER,0);
        auto *t=lv_label_create(choices);menuText[i]=t;lv_obj_set_pos(t,27,i*21+2);lv_obj_set_width(t,155);lv_obj_set_style_text_font(t,&lv_font_montserrat_12,0);
        auto *v=lv_label_create(choices);menuValues[i]=v;lv_obj_set_pos(v,174,i*21+3);lv_obj_set_width(v,122);lv_label_set_long_mode(v,LV_LABEL_LONG_DOT);lv_obj_set_style_text_align(v,LV_TEXT_ALIGN_RIGHT,0);lv_obj_set_style_text_font(v,&lv_font_montserrat_10,0);
    }
    qr=lv_qrcode_create(screen);lv_qrcode_set_size(qr,160);lv_qrcode_set_quiet_zone(qr,true);
    lv_qrcode_set_dark_color(qr,lv_color_black());lv_qrcode_set_light_color(qr,lv_color_white());
    lv_obj_set_pos(qr,4,4);lv_obj_add_flag(qr,LV_OBJ_FLAG_HIDDEN);
    draw();
}
static void requestFeed(bool append=false){
    if(busy)return;
    if(home && token.empty()){notice="L Sign in   A Access token";draw();return;}
    pendingAppend=append;
    json command={{"op","feed"},{"home",home}};
    if(append && !posts.empty())command["max_id"]=posts.back().id;
    send(command);notice=posts.empty()?"Loading timeline...":"";draw();
}
static void beginLogin(){if(busy)return;mode=Browse;notice="Preparing QR sign-in...";send({{"op","login"}});draw();}
static void cancelAuth(){nextPoll=0;send({{"op","cancel"}});}
static void beginToken(){if(mode==QR)cancelAuth();mode=TokenHelp;input.clear();notice.clear();draw();}
static void setServer(const std::string &host){send({{"op","server"},{"server",host}});notice="Saving server...";mode=Browse;draw();}
static void receive(const json &value){
    if(!value.is_object())return;
    if(value.value("id",0u)==0){
        if(!value.value("ok",false)){notice="Could not save settings.";mode=Browse;draw();}return;
    }
    if(value.value("id",0u)!=pendingID)return;
    auto op=pendingOp;busy=false;pendingOp.clear();
    if(!value.value("ok",false)){
        notice=value.value("error",std::string("Request failed."));
        if(op=="login"||op=="poll"){mode=AuthFallback;nextPoll=0;}
        else if(op=="token")mode=Token;
        else if(op=="publish")mode=Compose;
        draw();return;
    }
    if(op=="prefs"){
        auto p=value.at("prefs");server=p.value("server",std::string("mastodon.social"));
        darkTheme=p.value("dark",false);largeText=p.value("large",false);
    }else if(op=="server"){
        server=value.at("server");token.clear();input.clear();posts.clear();indexPost=0;endOfFeed=false;home=false;notice.clear();
    }else if(op=="login"||op=="poll"){
        if(value.value("fallback",false)){mode=AuthFallback;notice=value.value("message",std::string("Use an access token."));nextPoll=0;}
        else if(value.value("authenticated",false)){token="signed-in";home=true;posts.clear();indexPost=0;endOfFeed=false;mode=Browse;nextPoll=0;notice.clear();requestFeed();return;}
        else if(value.contains("qr")){
            if(showQR(value.at("qr"))){mode=QR;nextPoll=nowSeconds()+3;notice.clear();}
            else{cancelAuth();mode=AuthFallback;notice="Could not draw QR. Use a token.";}
        }else if(value.value("pending",false))nextPoll=nowSeconds()+std::clamp(value.value("interval",3),3,30);
    }else if(op=="token"){
        token="signed-in";input.clear();home=true;mode=Browse;posts.clear();indexPost=0;endOfFeed=false;notice.clear();requestFeed();return;
    }else if(op=="logout"){
        token.clear();input.clear();posts.clear();indexPost=0;home=false;endOfFeed=false;notice.clear();mode=Browse;
    }else if(op=="feed"){
        std::string selected=posts.empty()?"":posts[indexPost].id;
        int oldScroll=posts.empty()?0:posts[indexPost].scroll;
        auto data=value.at("posts");auto count=posts.size();
        if(!pendingAppend){posts.clear();indexPost=0;endOfFeed=false;}
        for(auto &p:data){
            std::string id=p.at("id");
            if(std::none_of(posts.begin(),posts.end(),[&](const Post &post){return post.id==id;}))posts.push_back({p.at("title"),p.at("body"),id,0});
        }
        if(pendingAppend){endOfFeed=posts.size()==count;if(posts.size()>count)indexPost=count;}
        else for(size_t i=0;i<posts.size();i++)if(posts[i].id==selected){indexPost=i;posts[i].scroll=oldScroll;break;}
        // Keep memory bounded when paging through long timelines.
        if(posts.size()>200){int trim=posts.size()-200;posts.erase(posts.begin(),posts.begin()+trim);indexPost=std::max(0,indexPost-trim);}
        notice.clear();
    }else if(op=="publish"){
        input.clear();mode=Browse;notice="Published.";
    }
    draw();
}
static void pump(){
    if(commandFD<0)return;
    if(!outgoing.empty()){
        ssize_t n=write(commandFD,outgoing.data(),outgoing.size());
        if(n>0)outgoing.erase(0,n);
        else if(n<0 && errno!=EAGAIN && errno!=EINTR){notice="Network helper stopped. Reopen the app.";busy=false;draw();close(commandFD);commandFD=-1;return;}
    }
    char buffer[8192];
    for(int i=0;i<64;i++){
        ssize_t n=read(responseFD,buffer,sizeof(buffer));
        if(n>0)incoming.append(buffer,n);
        else if(n==0){notice="Network helper stopped. Reopen the app.";busy=false;mode=Browse;draw();close(commandFD);commandFD=-1;return;}
        else break;
    }
    if(incoming.size()>4*1024*1024){notice="Invalid helper response.";running=false;return;}
    size_t end;
    while((end=incoming.find('\n'))!=std::string::npos){
        std::string line=incoming.substr(0,end);incoming.erase(0,end+1);
        try{receive(json::parse(line));}catch(const json::exception &){busy=false;notice="Invalid helper response.";draw();}
    }
    if(mode==QR && !busy && nextPoll>0 && nowSeconds()>=nextPoll){nextPoll=0;send({{"op","poll"}});}
}
enum {K_ESCAPE=27,K_ENTER=13,K_BACKSPACE=8,K_LEFT=256,K_RIGHT,K_UP,K_DOWN};
static void key(int k,const std::string &text="",bool repeat=false){
    bool editing=mode==Server||mode==Token||mode==Compose;
    if(repeat && !editing && k!='f' && k!='x' && k!=K_UP && k!=K_DOWN)return;
    if(editing){
        if(busy)return;
        if(k==K_ESCAPE){input.clear();notice.clear();mode=Browse;}
        else if(k==K_BACKSPACE && !input.empty()){size_t start=input.size()-1;while(start>0 && (input[start]&0xc0)==0x80)--start;input.erase(start);}
        else if(k==K_ENTER){
            if(mode==Server && !input.empty())setServer(input);
            else if(mode==Token && !input.empty()){send({{"op","token"},{"token",input}});input.clear();notice="Checking token...";}
            else if(mode==Compose && !input.empty())mode=Confirm;
        }else if(!text.empty() && input.size()+text.size()<=(mode==Token?4096:mode==Server?253:500))input+=text;
        draw();return;
    }
    if(mode==QR){if(k=='a')beginToken();else if(k==K_ESCAPE){cancelAuth();mode=Browse;notice="Sign-in cancelled.";draw();}return;}
    if(mode==AuthFallback){if(k=='1'||k=='l')beginLogin();else if(k=='2'||k=='a')beginToken();else if(k==K_ESCAPE){mode=Browse;notice.clear();draw();}return;}
    if(mode==TokenHelp){if(k==K_ENTER && !busy){mode=Token;input.clear();notice.clear();draw();}else if(k==K_ESCAPE){mode=Browse;draw();}return;}
    if(mode==Settings){
        if(k==K_ESCAPE||k=='o')mode=Browse;
        else if(k=='1'){darkTheme=!darkTheme;savePreferences();}
        else if(k=='2'){largeText=!largeText;for(auto &p:posts)p.scroll=0;savePreferences();}
        else if(k=='3' && !busy)mode=ServerChoice;
        else if(k=='4')mode=Help;
        else if(k=='5' && !busy){send({{"op","logout"}});mode=Browse;notice="Signing out...";}
        else if(k=='a' && !busy)beginToken();
        draw();return;
    }
    if(mode==Help){if(k=='a' && !busy)beginToken();else if(k==K_ESCAPE||k=='h'){mode=Browse;draw();}return;}
    if(mode==ServerChoice){
        if(k==K_ESCAPE)mode=Browse;
        else if(!busy && (k=='1'||k=='2'))setServer(k=='1'?"mastodon.social":"mastodon.online");
        else if(!busy && k=='3'){input.clear();mode=Server;}
        draw();return;
    }
    if(mode==Confirm){
        if(k=='y' && !busy){send({{"op","publish"},{"text",input}});mode=Browse;notice="Publishing...";}
        else if(k=='n'||k==K_ESCAPE){mode=Browse;input.clear();notice="Post cancelled.";}
        draw();return;
    }
    if(k=='o')mode=Settings;
    else if(k=='h')mode=Help;
    else if(k==K_ESCAPE)running=false;
    else if(k=='s' && !busy)mode=ServerChoice;
    else if(k=='a' && !busy)beginToken();
    else if(k=='l')beginLogin();
    else if(k=='n' && !busy){if(token.empty())notice="L Sign in   A Access token";else{input.clear();notice.clear();mode=Compose;}}
    else if(k=='r')requestFeed();
    else if(k=='t' && !busy){if(token.empty() && !home)notice="L Sign in to read your home feed";else{home=!home;posts.clear();indexPost=0;endOfFeed=false;requestFeed();}}
    else if(k=='z'||k==K_LEFT){if(indexPost>0)--indexPost;notice.clear();}
    else if(k=='c'||k==K_RIGHT){if(indexPost+1<(int)posts.size())++indexPost;else if(!posts.empty() && !endOfFeed)requestFeed(true);notice.clear();}
    else if(k=='f'||k==K_UP){if(!posts.empty())posts[indexPost].scroll-=lv_font_get_line_height(largeText?&lv_font_montserrat_16:&lv_font_montserrat_14);}
    else if(k=='x'||k==K_DOWN){if(!posts.empty())posts[indexPost].scroll+=lv_font_get_line_height(largeText?&lv_font_montserrat_16:&lv_font_montserrat_14);}
    draw();
}
static bool startHelper(const char *path){
    int commands[2],responses[2];
    if(pipe(commands)!=0)return false;
    if(pipe(responses)!=0){close(commands[0]);close(commands[1]);return false;}
    helperPID=fork();
    if(helperPID==0){
        dup2(commands[0],STDIN_FILENO);dup2(responses[1],STDOUT_FILENO);
        close(commands[0]);close(commands[1]);close(responses[0]);close(responses[1]);
        execl("/usr/bin/python3","python3","-I","-u",path,(char*)nullptr);_exit(127);
    }
    close(commands[0]);close(responses[1]);
    if(helperPID<0){close(commands[1]);close(responses[0]);return false;}
    commandFD=commands[1];responseFD=responses[0];
    fcntl(commandFD,F_SETFL,O_NONBLOCK);fcntl(responseFD,F_SETFL,O_NONBLOCK);
    signal(SIGPIPE,SIG_IGN);return true;
}
static void stopHelper(){
    if(commandFD>=0)close(commandFD);
    if(responseFD>=0)close(responseFD);
    if(helperPID>0){kill(helperPID,SIGTERM);waitpid(helperPID,nullptr,0);}
    commandFD=responseFD=-1;helperPID=-1;
}
#ifndef POCKET_TESTING
#include "linux_device.hpp"
int main(int argc,char **argv){
    if(argc==2 && std::string(argv[1])=="--version"){puts("m pocket 0.2.0-beta1 (Cardputer Zero arm64)");return 0;}
    lv_init();
    auto *display=lv_linux_fbdev_create();
    std::string fb=framebufferPath();
    if(!display || lv_linux_fbdev_set_file(display,fb.c_str())!=LV_RESULT_OK){fprintf(stderr,"Cannot open Cardputer framebuffer. Check video-group access.\n");return 1;}
    if(lv_display_get_horizontal_resolution(display)!=320 || lv_display_get_vertical_resolution(display)!=170){fprintf(stderr,"m pocket requires a 320x170 framebuffer.\n");return 1;}
    int keyboard=openKeyboard();
    if(keyboard<0){fprintf(stderr,"Cannot open Cardputer keyboard. Check input-group access.\n");return 1;}
    if(!startHelper("/usr/lib/m-pocket/service.py")){fprintf(stderr,"Cannot start network helper.\n");close(keyboard);return 1;}
    signal(SIGTERM,stopSignal);signal(SIGINT,stopSignal);
    ui_init();send({{"op","prefs"}});
    while(running && !stopRequested){readKeyboard(keyboard);pump();lv_timer_handler();usleep(5000);}
    stopHelper();close(keyboard);lv_deinit();return 0;
}
#endif
