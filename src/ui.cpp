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
static lv_obj_t *timelineBar,*timelineTitle,*timelineTab,*postPanel,*avatar,*avatarLetter,*accountLabel,*postActions,*separator;
static bool darkTheme=false,largeText=false,busy=false,home=false,endOfFeed=false,running=true;
// This is only an authentication indicator. Access tokens stay in the helper process.
static std::string token,server="mastodon.social",input,notice;
struct Post {std::string title,body,id;int scroll=0;int replies=0,boosts=0,favourites=0;std::string target,acct,visibility="public",warning;bool revealed=false,boosted=false;};
static std::vector<Post> posts,savedPosts;
static bool searchView=false;static int savedIndex=0;
static std::string replyTarget,replyVisibility;
static int indexPost=0;
enum Mode {Settings,Help,Browse,ServerChoice,Server,Token,TokenHelp,AuthFallback,QR,Compose,Confirm,Search};
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
    bool timeline=mode==Browse && !posts.empty();
    for(auto *o:{timelineBar,timelineTitle,timelineTab,postPanel,avatar,avatarLetter,accountLabel,postActions,separator})visible(o,timeline);
    if(timeline){visible(topbar,false);visible(brand,false);visible(brandMark,false);visible(head,false);}else visible(head,!isQR);
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
            std::string value=settings?(i==0?(darkTheme?"Dark":"Light"):i==1?(largeText?"Large":"Normal"):i==2?server:i==3?"Shortcuts":token.empty()?"Not signed in":"Saved login"):"";
            lv_label_set_text(menuValues[i],value.c_str());
        }
        lv_label_set_text(foot,settings?"1-5  Change                         Esc  Back":"1-3  Select                         Esc  Back");return;
    }
    if(mode==Help) {
        lv_label_set_text(head,"KEYBOARD"); lv_obj_set_pos(body,12,37); lv_obj_set_height(body,106);lv_obj_set_style_text_font(body,&lv_font_montserrat_12,0);
        lv_label_set_text(body,"Z/C Posts  F/X Scroll  W Reveal CW\nB Boost  P Reply  Q Search\nR Refresh  T Home / local\nN Write  S Server  L Sign in\nO Settings  Esc Back"); lv_label_set_text(foot,"A  Manual access token                Esc  Back"); return;
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
        lv_label_set_text(body,("On "+server+":\nPreferences > Development > New app\nEnable read:statuses, read:search,\nwrite:statuses (posting / boosts).\nSave, then copy Your access token.\nKeep the token private.").c_str());
        lv_label_set_text(foot,"Enter  Type token                     Esc  Back");return;
    }
    if(mode==Token) {
        lv_label_set_text(head,"ACCESS TOKEN");lv_obj_set_style_text_font(body,&lv_font_montserrat_12,0);
        std::string masked(std::min<size_t>(input.size(),34),'*');
        lv_label_set_text(body,(server+"\n\n"+masked+"_\n\n"+(notice.empty()?"Your access token, not your password.":notice)).c_str());
        lv_label_set_text(foot,busy?"Checking token...":"Enter Sign in   Esc Cancel");return;
    }
    if(mode==Search) {lv_label_set_text(head,"SEARCH POSTS");lv_label_set_text(body,("Search your server\n\n"+input+"_").c_str());lv_label_set_text(foot,notice.empty()?"Enter  Search                       Esc  Cancel":notice.c_str());return;}
    if(mode==Compose || mode==Confirm) { lv_label_set_text(head,mode==Confirm?"READY TO POST?":replyTarget.empty()?"WRITE / PUBLIC":"WRITE / REPLY"); lv_label_set_text(body,input.empty()?"What's happening?":input.c_str()); lv_label_set_text(foot,mode==Confirm?"Y  Publish                          N  Cancel":(!notice.empty()?notice.c_str():"Enter  Review                       Esc  Cancel")); return; }
    std::string h=home?"HOME":"LOCAL"; if(!posts.empty()) h+="  "+std::to_string(indexPost+1)+"/"+std::to_string(posts.size());
    lv_label_set_text(head,h.c_str());
    if(!searchView && token.empty() && posts.empty() && (notice.empty() || notice.find("rejected access")!=std::string::npos)) {
        lv_obj_set_style_text_font(body,&lv_font_montserrat_20,0); lv_obj_set_pos(body,12,49); lv_obj_set_height(body,54);
        lv_label_set_text(body,"m pocket");
        visible(meta,true); lv_obj_set_pos(meta,12,116); lv_label_set_text(meta,server.c_str());
        lv_label_set_text(foot,"L Sign in   A Token   S Server   O Settings"); return;
    }
    if(!notice.empty() && posts.empty()) lv_label_set_text(body,notice.c_str());
    else if(!posts.empty()) {
        auto &selected=posts[indexPost];
        const uint32_t ink=darkTheme?0xe9e9f2:0x252437;
        const uint32_t subtle=darkTheme?0xaaaabd:0x686779;
        const uint32_t bg=darkTheme?0x191a29:0xffffff;
        lv_obj_set_style_bg_color(lv_screen_active(),lv_color_hex(darkTheme?0x11121e:0xf1f2f6),0);
        lv_obj_set_style_bg_color(timelineBar,lv_color_hex(darkTheme?0x303044:0x563acc),0);
        lv_obj_set_style_bg_color(postPanel,lv_color_hex(bg),0);
        lv_obj_set_style_bg_color(avatar,lv_color_hex(darkTheme?0x514b70:0xe6e2f8),0);
        lv_obj_set_style_bg_color(separator,lv_color_hex(darkTheme?0x353646:0xe7e7ef),0);
        lv_obj_set_style_text_color(timelineTitle,lv_color_white(),0);lv_obj_set_style_text_color(timelineTab,lv_color_hex(0xded7ff),0);
        lv_obj_set_style_text_color(avatarLetter,lv_color_hex(darkTheme?0xe0d7ff:0x563acc),0);
        lv_obj_set_style_text_color(accountLabel,lv_color_hex(ink),0);
        lv_obj_set_style_text_color(feedText,lv_color_hex(ink),0);
        lv_obj_set_style_text_color(postActions,lv_color_hex(subtle),0);
        lv_label_set_text(timelineTitle,searchView?"m  Search":home?"m  Home":"m  Local");
        lv_label_set_text(timelineTab,(std::to_string(indexPost+1)+" / "+std::to_string(posts.size())+"   T switch").c_str());
        std::string initial="@";if(!selected.title.empty() && selected.title[0]>='A' && selected.title[0]<='Z')initial=selected.title.substr(0,1);else if(!selected.title.empty() && selected.title[0]>='a' && selected.title[0]<='z')initial=std::string(1,selected.title[0]-'a'+'A');
        lv_label_set_text(avatarLetter,initial.c_str());
        lv_label_set_text(accountLabel,selected.title.c_str());
        lv_label_set_text(postActions,(std::string("Likes ")+std::to_string(selected.favourites)+"   Replies "+std::to_string(selected.replies)+"   "+(selected.boosted?"Boosted ":"Boosts ")+std::to_string(selected.boosts)).c_str());
        lv_label_set_text(body,""); visible(feedViewport,true);
        auto &post=posts[indexPost];
        const lv_font_t *font=largeText?&lv_font_montserrat_16:&lv_font_montserrat_14;
        lv_obj_set_style_text_font(feedText,font,0);
        std::string shown=post.warning.empty()?post.body:"CW: "+post.warning+(post.revealed?"\n"+post.body:"\n\nW  Show post");
        lv_label_set_text(feedText,shown.c_str());
        lv_obj_update_layout(feedViewport);
        const int height=lv_obj_get_height(feedText), viewport=lv_obj_get_height(feedViewport);
        const int maximum=std::max(0,height-viewport);
        post.scroll=std::clamp(post.scroll,0,maximum);
        lv_obj_scroll_to_y(feedViewport,post.scroll,LV_ANIM_OFF);
        if(maximum>0) {
            visible(progress,true);
            int thumb=std::max(8,viewport*viewport/height);
            lv_obj_set_height(progress,thumb);
            lv_obj_set_y(progress,55+(viewport-thumb)*post.scroll/maximum);
        }
    } else lv_label_set_text(body,searchView?"No matching posts.\n\nEsc to return to your feed.":home?"No posts yet.\n\nFollow a few accounts,\nthen refresh with R.":"No local posts.\n\nR to try again.");
    std::string hint=busy?"Loading...  F/X Scroll  O Settings":!notice.empty()?notice:endOfFeed && indexPost+1==(int)posts.size()?"End of feed   R Refresh   O Settings":"Z/C Posts  F/X Scroll  P Reply  B Boost  Q Search";
    lv_label_set_text(foot,hint.c_str());
}
static void ui_init() {
    lv_obj_t *screen=lv_screen_active(); lv_obj_set_style_bg_color(screen,lv_color_hex(0xf2efdf),0);
    auto panel=[&](int x,int y,int w,int h,uint32_t color){auto *o=lv_obj_create(screen);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);return o;};
    topbar=panel(12,30,296,1,0x262a28); card=panel(0,0,1,1,0xf2efdf);
    timelineBar=panel(0,0,320,24,0x563acc);postPanel=panel(0,24,320,121,0xffffff);
    avatar=panel(7,31,20,20,0xe6e2f8);lv_obj_set_style_radius(avatar,5,0);
    separator=panel(8,123,304,1,0xe7e7ef);keybar=panel(0,146,320,24,0x262a28);
    auto label=[&](int x,int y,int w,const lv_font_t *font,uint32_t color){auto *o=lv_label_create(screen);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);return o;};
    timelineTitle=label(9,5,110,&lv_font_montserrat_12,0xffffff);
    timelineTab=label(150,7,160,&lv_font_montserrat_10,0xded7ff);lv_obj_set_style_text_align(timelineTab,LV_TEXT_ALIGN_RIGHT,0);
    avatarLetter=label(7,34,20,&lv_font_montserrat_10,0x563acc);lv_obj_set_style_text_align(avatarLetter,LV_TEXT_ALIGN_CENTER,0);
    accountLabel=label(33,33,275,&lv_font_montserrat_12,0x252437);lv_label_set_long_mode(accountLabel,LV_LABEL_LONG_DOT);
    postActions=label(10,129,300,&lv_font_montserrat_10,0x686779);lv_label_set_long_mode(postActions,LV_LABEL_LONG_DOT);
    brandMark=label(12,7,18,&lv_font_montserrat_16,0x7054aa);lv_label_set_text(brandMark,"m");
    brand=label(33,7,80,&lv_font_montserrat_16,0x262a28);
    head=label(116,11,192,&lv_font_montserrat_10,0x545b52); lv_obj_set_style_text_align(head,LV_TEXT_ALIGN_RIGHT,0);
    meta=label(12,39,296,&lv_font_montserrat_12,0x4d6551); lv_label_set_long_mode(meta,LV_LABEL_LONG_DOT);
    body=label(12,43,296,&lv_font_montserrat_14,0x262a28);lv_obj_set_height(body,95);lv_label_set_long_mode(body,LV_LABEL_LONG_WRAP);
    feedViewport=lv_obj_create(screen); lv_obj_remove_style_all(feedViewport);
    lv_obj_set_pos(feedViewport,33,55);lv_obj_set_size(feedViewport,274,64);
    lv_obj_set_scrollbar_mode(feedViewport,LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(feedViewport,LV_DIR_VER);
    lv_obj_remove_flag(feedViewport,LV_OBJ_FLAG_SCROLL_ELASTIC);lv_obj_remove_flag(feedViewport,LV_OBJ_FLAG_SCROLL_MOMENTUM);
    feedText=lv_label_create(feedViewport); lv_obj_set_width(feedText,274);lv_obj_set_height(feedText,LV_SIZE_CONTENT);
    lv_label_set_long_mode(feedText,LV_LABEL_LONG_WRAP);
    progress=panel(310,55,2,8,0x7054aa);
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
        else if(op=="search")mode=Search;
        draw();return;
    }
    if(op=="prefs"){
        auto p=value.at("prefs");server=p.value("server",std::string("mastodon.social"));
        darkTheme=p.value("dark",false);largeText=p.value("large",false);
        if(value.value("authenticated",false)){token="signed-in";home=true;requestFeed();return;}
    }else if(op=="server"){
        server=value.at("server");token.clear();input.clear();posts.clear();savedPosts.clear();searchView=false;indexPost=0;endOfFeed=false;home=false;notice.clear();
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
        token.clear();input.clear();posts.clear();savedPosts.clear();searchView=false;indexPost=0;home=false;endOfFeed=false;notice.clear();mode=Browse;
    }else if(op=="feed"||op=="search"){
        if(op=="search"){if(!searchView){savedPosts=posts;savedIndex=indexPost;}searchView=true;mode=Browse;input.clear();pendingAppend=false;}
        std::string selected=posts.empty()?"":posts[indexPost].id;
        int oldScroll=posts.empty()?0:posts[indexPost].scroll;
        auto data=value.at("posts");auto count=posts.size();auto oldPosts=posts;
        if(!pendingAppend){posts.clear();indexPost=0;endOfFeed=false;}
        for(auto &p:data){
            std::string id=p.at("id");
            if(std::none_of(posts.begin(),posts.end(),[&](const Post &post){return post.id==id;})){
                Post item{};item.title=p.at("title");item.body=p.at("body");item.id=id;item.replies=p.value("replies",0);item.boosts=p.value("boosts",0);item.favourites=p.value("favourites",0);item.target=p.value("target",id);item.acct=p.value("acct",std::string());item.visibility=p.value("visibility",std::string("public"));item.warning=p.value("warning",std::string());item.boosted=p.value("boosted",false);
                auto old=std::find_if(oldPosts.begin(),oldPosts.end(),[&](const Post &a){return a.id==id;});if(old!=oldPosts.end() && old->warning==item.warning)item.revealed=old->revealed;posts.push_back(std::move(item));}
        }
        if(pendingAppend){endOfFeed=posts.size()==count;if(posts.size()>count)indexPost=count;}
        else for(size_t i=0;i<posts.size();i++)if(posts[i].id==selected){indexPost=i;posts[i].scroll=oldScroll;break;}
        // Keep memory bounded when paging through long timelines.
        if(posts.size()>200){int trim=posts.size()-200;posts.erase(posts.begin(),posts.begin()+trim);indexPost=std::max(0,indexPost-trim);}
        notice.clear();
    }else if(op=="boost"){
        auto target=value.value("target",std::string());bool boosted=value.value("boosted",false);int count=value.value("boosts",0);
        for(auto &post:posts)if(post.target==target){post.boosted=boosted;post.boosts=std::max(0,count);}
        notice=boosted?"Boosted.":"Boost removed.";
    }else if(op=="publish"){
        input.clear();replyTarget.clear();replyVisibility.clear();mode=Browse;notice="Published.";
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
    bool editing=mode==Server||mode==Token||mode==Compose||mode==Search;
    if(repeat && !editing && k!='f' && k!='x' && k!=K_UP && k!=K_DOWN)return;
    if(editing){
        if(busy)return;
        if(k==K_ESCAPE){input.clear();replyTarget.clear();replyVisibility.clear();notice.clear();mode=Browse;}
        else if(k==K_BACKSPACE && !input.empty()){size_t start=input.size()-1;while(start>0 && (input[start]&0xc0)==0x80)--start;input.erase(start);}
        else if(k==K_ENTER){
            if(mode==Server && !input.empty())setServer(input);
            else if(mode==Token && !input.empty()){send({{"op","token"},{"token",input}});input.clear();notice="Checking token...";}
            else if(mode==Search && !input.empty()){send({{"op","search"},{"query",input}});notice="Searching...";}
            else if(mode==Compose && !input.empty())mode=Confirm;
        }else if(!text.empty() && input.size()+text.size()<=(mode==Token?4096:mode==Server?253:mode==Search?200:500))input+=text;
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
        if(k=='y' && !busy){json command={{"op","publish"},{"text",input}};if(!replyTarget.empty()){command["reply_to"]=replyTarget;command["visibility"]=replyVisibility;}send(command);mode=Browse;notice="Publishing...";}
        else if(k=='n'||k==K_ESCAPE){mode=Browse;input.clear();replyTarget.clear();replyVisibility.clear();notice="Post cancelled.";}
        draw();return;
    }
    if(k=='o')mode=Settings;
    else if(k=='h')mode=Help;
    else if(k==K_ESCAPE){if(searchView){posts=std::move(savedPosts);indexPost=std::min(savedIndex,std::max(0,(int)posts.size()-1));searchView=false;notice.clear();}else running=false;}
    else if(k=='s' && !busy)mode=ServerChoice;
    else if(k=='a' && !busy)beginToken();
    else if(k=='l')beginLogin();
    else if(k=='n' && !busy){if(token.empty())notice="L Sign in   A Access token";else{input.clear();replyTarget.clear();replyVisibility.clear();notice.clear();mode=Compose;}}
    else if((k=='q'||k=='/') && !busy){if(token.empty())notice="Sign in to search.";else{input.clear();notice.clear();mode=Search;}}
    else if(k=='w' && !posts.empty() && !posts[indexPost].warning.empty()){posts[indexPost].revealed=!posts[indexPost].revealed;posts[indexPost].scroll=0;notice.clear();}
    else if(k=='p' && !busy && !posts.empty()){auto &post=posts[indexPost];if(token.empty())notice="Sign in to reply.";else if(post.target.empty()||post.acct.empty()||(post.visibility!="public"&&post.visibility!="unlisted"&&post.visibility!="private"&&post.visibility!="direct"))notice="Cannot reply to this post.";else{replyTarget=post.target;replyVisibility=post.visibility;input="@"+post.acct+" ";notice.clear();mode=Compose;}}
    else if(k=='b' && !busy && !posts.empty()){auto &post=posts[indexPost];if(token.empty())notice="Sign in to boost.";else if(post.visibility=="private"||post.visibility=="direct")notice="This post cannot be boosted.";else{send({{"op","boost"},{"target",post.target},{"undo",post.boosted}});notice="Updating boost...";}}
    else if(k=='r' && !searchView)requestFeed();
    else if(k=='t' && !busy && !searchView){if(token.empty() && !home)notice="L Sign in to read your home feed";else{home=!home;posts.clear();indexPost=0;endOfFeed=false;requestFeed();}}
    else if(k=='z'||k==K_LEFT){if(indexPost>0)--indexPost;notice.clear();}
    else if(k=='c'||k==K_RIGHT){if(indexPost+1<(int)posts.size())++indexPost;else if(!searchView && !posts.empty() && !endOfFeed)requestFeed(true);notice.clear();}
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
    if(argc==2 && std::string(argv[1])=="--version"){puts("m pocket 0.2.0-beta3 (Cardputer Zero arm64)");return 0;}
    lv_init();
    auto *display=lv_linux_fbdev_create();
    std::string fb=framebufferPath();
    if(!display || lv_linux_fbdev_set_file(display,fb.c_str())!=LV_RESULT_OK){fprintf(stderr,"Cannot open Cardputer framebuffer. Check video-group access.\n");return 1;}
    if(lv_display_get_horizontal_resolution(display)!=320 || lv_display_get_vertical_resolution(display)!=170){fprintf(stderr,"m pocket requires a 320x170 framebuffer.\n");return 1;}
    int keyboard=openKeyboard();
    if(keyboard<0){fprintf(stderr,"Cannot open Cardputer keyboard. Check input-group access.\n");return 1;}
    loadCpzKeymap();
    if(!startHelper("/usr/lib/m-pocket/service.py")){fprintf(stderr,"Cannot start network helper.\n");close(keyboard);return 1;}
    signal(SIGTERM,stopSignal);signal(SIGINT,stopSignal);
    ui_init();send({{"op","prefs"}});
    while(running && !stopRequested){readKeyboard(keyboard);pump();lv_timer_handler();usleep(5000);}
    stopHelper();close(keyboard);lv_deinit();return 0;
}
#endif
